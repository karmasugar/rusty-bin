import argparse
import dataclasses
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SIGNATURE_INSTRUCTIONS = 40
JO_SERVICE_TABLE = 0x6360
JO_SERVICE_COUNT = 78
JO_ENEMY_SERVICE_TABLE = 0x4540
JO_ENEMY_SERVICE_COUNT = 56
SERVICE_HELPER_PATTERN = re.compile(rb"\x50\x2e\xa1..\x2e\xa3..\x2e\xa1..\x2e\xa3..\x58\x2e\xff\x1e", re.DOTALL)
STATE_WRITE = re.compile(r"MOV word ptr CS:\[0x([02])\],0x([0-9a-f]+)$")
STORY_ROOM_TABLE = 8

ENEMY1_NAMES = {
    0x0016: (
        "call_enemy_service",
        "Far call to JO.EXE enemy_service_dispatch (pointer from set_service_ptr); AX = service.",
    ),
    0x0032: (
        "set_service_ptr",
        "Save the service far pointer DI:ES and the object table BX; every entry calls it first.",
    ),
    0x0051: ("object_apply_gravity", "If the object falls: fall speed [SI+15] up to 12, add it to [SI+24]."),
    0x0070: ("object_stop_on_ground", "On the ground ([SI+7]): fall speed = 0."),
    0x0081: ("object_bounce_on_ground", "On the ground: small speed stops, else bounce with -speed/2. (guess)"),
    0x00AB: ("enemy_entry_init", "Entry 2: set_service_ptr, spawn the start objects of the room."),
    0x0C89: ("enemy_entry_alloc", "Entry 3: set_service_ptr, find a free object slot. (guess)"),
    0x0175: (
        "enemy_entry_frame",
        "Entry 4 (every frame from JO enemy_engine_update): spawn objects on room change, update.",
    ),
    0x00B2: (
        "spawn_room_objects",
        "Spawn the objects of the current room from the per-room table (skip killed ones). (guess)",
    ),
    0x029D: ("spawn_room_start_objects", "Spawn the start objects of the current room (flags 82DD). (guess)"),
    0x0392: ("find_free_object_slot", "Search the object table for a free slot (24 slots, [DI+2] = 0). (guess)"),
    0x05EC: ("aim_at_player_a", "Uses the player position and the distance to the player. (guess)"),
    0x062A: ("aim_at_player_b", "Uses the distance and direction to the player. (guess)"),
    0x096B: (
        "item_pickup",
        "Item effects: sound, add_key, set_special_weapon, add_mind_power, add_score, add_time. (guess)",
    ),
    0x1687: (
        "platform_carry_player",
        "Map probe at the next object position, then esvc_carry_player_x (moving platform). (guess)",
    ),
}
ENEMY_FILE_ONLY_NAMES = {
    "ENEMY1.COM": {0x1808: ("boss_update", "Large stage-specific routine; fades the music. (guess)")}
}


@dataclasses.dataclass
class Function:
    entry: int
    name: str
    instructions: list[tuple[int, str, str]]


def load_listing(program: str, *, export: pathlib.Path) -> dict[int, Function]:
    functions: dict[int, Function] = {}
    current = None
    for line in (export / f"{program}.lst").read_text(encoding="utf-8").splitlines():
        if line.startswith("; ==== FUNCTION"):
            parts = line.split()
            current = Function(entry=int(parts[3], 16), name=parts[4], instructions=[])
            functions[current.entry] = current
            continue
        if line.strip() and current is not None:
            offset, _segmented, raw, rest = line.split(None, 3)
            current.instructions.append((int(offset, 16), raw, rest.split("   ;")[0]))
    return functions


def signature(function: Function) -> str:
    parts = []
    for _offset, raw, _text in function.instructions[:SIGNATURE_INSTRUCTIONS]:
        code = bytes.fromhex(raw)
        relative_near = code[:1] in (b"\xe8", b"\xe9")
        relative_short = len(code) == 2 and (0x70 <= code[0] <= 0x7F or code[0] in (0xEB, 0xE2, 0xE3))
        if relative_near or relative_short:
            code = code[:1]
        parts.append(code.hex())
    return " ".join(parts)


def load_jo_names() -> dict[int, str]:
    names = {}
    for line in (ROOT / "tools/recomp/names/JO.EXE.tsv").read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        columns = line.split("\t")
        segment, offset = columns[0].split(":")
        if segment == "0000" and columns[1] == "func":
            names[int(offset, 16)] = columns[2]
    return names


def service_names(image: bytes, *, table: int, count: int, jo_names: dict[int, str]) -> dict[int, str]:
    result = {}
    for index in range(count):
        target = struct.unpack_from("<H", image, table + index * 2)[0]
        result[index] = jo_names.get(target, f"service_{index:02x}")
    return result


def jo_image() -> bytes:
    data = (ROOT / "build/unpacked/ja/JO.EXE").read_bytes()
    return data[struct.unpack_from("<H", data, 8)[0] * 16 :]


def find_service_helper(image: bytes) -> int | None:
    match = SERVICE_HELPER_PATTERN.search(image)
    return match.start() if match else None


def find_pointer_setter(functions: dict[int, Function]) -> int | None:
    for entry, function in sorted(functions.items()):
        texts = [text for _offset, _raw, text in function.instructions[:2]]
        if len(texts) == 2 and texts[0].endswith("],SI") and texts[1].endswith("],DS") and "CS:[" in texts[0]:
            return entry
    return None


def service_call_comments(
    functions: dict[int, Function], *, helper: int, names: dict[int, str], prefix: str
) -> list[str]:
    lines = []
    call = f"CALL 0x2000:{helper:04x}"
    for function in functions.values():
        value = None
        for offset, _raw, text in function.instructions:
            match = re.match(r"MOV AX,0x([0-9a-f]+)$", text)
            if match:
                value = int(match.group(1), 16)
            if text == call and value is not None:
                lines.append(f"0000:{offset:04X}\tcomment\t-\t{prefix} {value:02X}h {names.get(value, '?')}")
    return lines


def enemy_names(export: pathlib.Path, *, esvc: dict[int, str], out: pathlib.Path) -> None:
    files = [f"ENEMY{number}.COM" for number in range(1, 11)]
    listings = {name: load_listing(name, export=export) for name in files}
    reference = listings["ENEMY1.COM"]
    by_signature = {signature(reference[entry]): value for entry, value in ENEMY1_NAMES.items() if entry in reference}
    for name in files:
        image = (ROOT / "build/unpacked/ja" / name).read_bytes()
        lines = [
            f"# Names for {name} (ENEMYn engine). Generated by tools/ghidra/gen_overlay_names.py.",
            "# Shared functions are matched by code signature from ENEMY1.COM.",
        ]
        for entry, function in sorted(listings[name].items()):
            found = ENEMY_FILE_ONLY_NAMES.get(name, {}).get(entry) or by_signature.get(signature(function))
            if found:
                lines.append(f"0000:{entry:04X}\tfunc\t{found[0]}\t{found[1]}")
        helper = find_service_helper(image)
        if helper is not None:
            lines += service_call_comments(listings[name], helper=helper, names=esvc, prefix="esvc")
        (out / f"{name}.tsv").write_text("\n".join(lines) + "\n")
        print(f"{name}: {len(lines) - 2} lines")


def story_room_handlers(image: bytes) -> list[int]:
    handlers = set()
    first = struct.unpack_from("<H", image, STORY_ROOM_TABLE)[0]
    position = STORY_ROOM_TABLE
    rooms = []
    while position < first:
        rooms.append(struct.unpack_from("<H", image, position)[0])
        position += 2
    for room in rooms:
        cursor = room
        while cursor + 10 <= len(image) and struct.unpack_from("<H", image, cursor)[0] != 0xFFFF:
            handlers.add(struct.unpack_from("<H", image, cursor + 8)[0])
            cursor += 10
    return sorted(handler for handler in handlers if handler < len(image))


def story_names(export: pathlib.Path, *, svc: dict[int, str], out: pathlib.Path) -> None:
    for number in range(1, 11):
        name = f"STORY{number}.COM"
        image = (ROOT / "build/unpacked/ja" / name).read_bytes()
        listing = load_listing(name, export=export)
        entries = struct.unpack_from("<4H", image, 0)
        lines = [
            f"# Names for {name} (level script). Generated by tools/ghidra/gen_overlay_names.py.",
            "# Entry 0 and entry 1 are state slots: the script writes new handler addresses into CS:0000 / CS:0002.",
        ]
        named = {}
        named[entries[0]] = (
            "story_entry0_start",
            "Entry 0 at load time (called every frame by JO story_update; replaced by state writes).",
        )
        named[entries[1]] = (
            "story_entry1_start",
            "Entry 1 at load time (called by JO render_scroll_phase; replaced by state writes).",
        )
        named[entries[2]] = (
            "story_entry2_scene",
            "Entry 2: called first by JO stage_transition (story scene after the boss). (guess)",
        )
        named[entries[3]] = (
            "story_entry3_next",
            "Entry 3: called by JO stage_transition after stage_clear (prepare the next part). (guess)",
        )
        for handler in story_room_handlers(image):
            named.setdefault(
                handler,
                (f"room_trigger_{handler:04x}", "Room trigger handler (rectangle in the room table at entry 4)."),
            )
        for function in listing.values():
            for _offset, _raw, text in function.instructions:
                match = STATE_WRITE.match(text)
                if match:
                    target = int(match.group(2), 16)
                    kind = "state" if match.group(1) == "0" else "render_state"
                    named.setdefault(
                        target,
                        (f"story_{kind}_{target:04x}", f"Written into entry {int(match.group(1)) // 2} by the script."),
                    )
        setter = find_pointer_setter(listing)
        if setter is not None:
            named[setter] = (
                "story_set_service_ptr",
                "Save the service far pointer SI:DS and CX; the entries call it first.",
            )
        helper = find_service_helper(image)
        if helper is not None:
            named[helper] = (
                "story_call_service",
                "Far call to JO.EXE service_dispatch (pointer saved by story_set_service_ptr); AX = service.",
            )
        for entry, (label, comment) in sorted(named.items()):
            lines.append(f"0000:{entry:04X}\tfunc\t{label}\t{comment}")
        if helper is not None:
            lines += service_call_comments(listing, helper=helper, names=svc, prefix="svc")
        (out / f"{name}.tsv").write_text("\n".join(lines) + "\n")
        print(f"{name}: {len(named)} functions, {len(lines) - 2 - len(named)} service call comments")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate Ghidra names files for the ENEMYn and STORYn overlays.")
    parser.add_argument("--export", type=pathlib.Path, default=ROOT / "build/export/ja", help="RustyExport.java output")
    parser.add_argument(
        "--out",
        type=pathlib.Path,
        default=ROOT / "build/generated_names",
        help="output directory (tools/recomp/names has names added later by hand; do not overwrite it)",
    )
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    image = jo_image()
    jo_names = load_jo_names()
    svc = service_names(image, table=JO_SERVICE_TABLE, count=JO_SERVICE_COUNT, jo_names=jo_names)
    esvc = service_names(image, table=JO_ENEMY_SERVICE_TABLE, count=JO_ENEMY_SERVICE_COUNT, jo_names=jo_names)
    enemy_names(args.export, esvc=esvc, out=args.out)
    story_names(args.export, svc=svc, out=args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
