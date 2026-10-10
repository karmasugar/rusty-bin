import argparse
import os
import pathlib
import re
import struct
import sys

import analysis
import lift

RUNTIME_HEADER = pathlib.Path(__file__).resolve().parent.parent.parent / "runtime/rt.h"


def parse_address(text: str) -> tuple[int, int]:
    if ":" in text:
        segment, offset = text.split(":")
        return (int(segment, 16), int(offset, 16))
    return (0, int(text, 16))


def names_entries(path: pathlib.Path) -> list[tuple[int, int]]:
    entries = []
    for line in path.read_text(encoding="utf-8").splitlines():
        columns = line.split("\t")
        if len(columns) > 2 and columns[1] == "func" and not line.startswith("#"):
            entries.append(parse_address(columns[0]))
    return entries


def runtime_identifiers() -> set[str]:
    return set(re.findall(r"\b[A-Za-z_][A-Za-z0-9_]*\b", RUNTIME_HEADER.read_text(encoding="utf-8")))


def names_naming(paths: list[pathlib.Path], *, prefix: str) -> lift.Naming:
    naming = lift.Naming(prefix)
    reserved = runtime_identifiers()
    lines = [line for path in paths for line in path.read_text(encoding="utf-8").splitlines()]
    for line in lines:
        columns = line.split("\t")
        if line.startswith("#") or len(columns) < 3:
            continue
        key = parse_address(columns[0])
        kind, name = columns[1], columns[2]
        text = columns[3].strip() if len(columns) > 3 else ""
        address = f"{key[0]:04X}:{key[1]:04X}"
        if kind == "func" and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            naming.functions[key] = f"{prefix}_{name}" if name in reserved else name
            naming.notes.setdefault(key, []).append(f"{address} {name}: {text}" if text else f"{address} {name}")
        elif kind == "data" and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            naming.data[key] = (name, text)
        elif kind == "label":
            naming.notes.setdefault(key, []).append(f"{name}: {text}" if text else name)
        elif kind == "comment" and text:
            naming.notes.setdefault(key, []).append(text)
    return naming


def load_mz(data: bytes) -> tuple[bytes, set[int], tuple[int, int]]:
    if data[:2] != b"MZ":
        raise analysis.AnalysisError("not an MZ executable")
    last_page, pages, relocation_count, header_paragraphs = struct.unpack_from("<4H", data, 2)
    ip, cs, relocation_offset = struct.unpack_from("<3H", data, 0x14)
    size = pages * 512 - (512 - last_page if last_page else 0)
    image = data[header_paragraphs * 16 : size]
    relocations = set()
    for index in range(relocation_count):
        offset, segment = struct.unpack_from("<HH", data, relocation_offset + index * 4)
        linear = segment * 16 + offset
        relocations.update((linear, linear + 1))
    return image, relocations, (cs, ip)


def main() -> int:
    parser = argparse.ArgumentParser(description="Recompile a real-mode x86 program image to C (prototype).")
    parser.add_argument("image", type=pathlib.Path, help="COM file, unpacked overlay, or MZ executable (with --exe)")
    parser.add_argument("--exe", action="store_true", help="MZ executable: image, relocations and start from header")
    parser.add_argument("--base", type=lambda text: int(text, 16), default=0x100, help="offset of the first byte (COM)")
    parser.add_argument("--entry", type=parse_address, action="append", default=[], help="entry [SEG:]OFF (hex)")
    parser.add_argument(
        "--names",
        type=pathlib.Path,
        action="append",
        default=[],
        help="names file (repeatable): its func entries become entry points and give the C names",
    )
    parser.add_argument(
        "--entries-file",
        type=pathlib.Path,
        action="append",
        default=[],
        help="optional entries, one [SEG:]OFF per line",
    )
    parser.add_argument(
        "--coverage", type=pathlib.Path, help="executed offsets (coverage.py output) for code constants"
    )
    parser.add_argument("--data-tables", action="store_true", help="with --coverage: executed addresses in word tables")
    parser.add_argument(
        "--interpret", action="store_true", help="test mode: every function only calls the fallback interpreter"
    )
    parser.add_argument(
        "--safe-point",
        type=lambda text: int(text, 16),
        action="append",
        default=[],
        help="offset (hex) where the runtime may save or load a snapshot (rt_safe_point), e.g. the main loop",
    )
    parser.add_argument("--prefix", default="f", help="C function name prefix")
    parser.add_argument("--module-name", help="module name in the runtime (default: the file name)")
    parser.add_argument("--out", type=pathlib.Path, required=True, help="output C file")
    args = parser.parse_args()

    data = args.image.read_bytes()
    try:
        relocations: set[int] = set()
        base = args.base
        entries = list(args.entry)
        image = data
        if args.exe:
            image, relocations, start = load_mz(data)
            base = 0
            entries.insert(0, start)
        for names_path in args.names:
            entries += [entry for entry in names_entries(names_path) if entry not in entries]
        if not entries:
            parser.error("give --entry or --exe")
        executed = None
        if args.coverage:
            executed = {int(line, 16) for line in args.coverage.read_text().split()}
        optional = []
        for path in args.entries_file:
            if path.exists():
                optional += [parse_address(line) for line in path.read_text().split() if line]
        program = analysis.analyze(
            image,
            base=base,
            entries=entries,
            relocations=relocations,
            executed=executed,
            data_tables=args.data_tables,
            optional_entries=optional,
        )
        naming = names_naming(args.names, prefix=args.prefix)
        code = lift.lift_program(
            program,
            naming=naming,
            name=args.module_name or args.image.name,
            interpret=args.interpret or bool(os.environ.get("RUSTY_INTERPRET_ALL")),
            safe_points=frozenset(args.safe_point),
            reserved=frozenset(runtime_identifiers()),
            exe=args.exe,
        )
    except (analysis.AnalysisError, lift.LiftError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(code)
    count = sum(len(function.instructions) for function in program.functions.values())
    segments = sorted({function.segment for function in program.functions.values()})
    print(
        f"{args.image.name}: {len(program.functions)} functions, {count} instructions, code segments "
        f"{' '.join(f'{segment:04x}' for segment in segments)} -> {args.out}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
