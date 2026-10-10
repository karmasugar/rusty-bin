import os
import pathlib
import shutil
import subprocess
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
VENV_PYTHON = ROOT / ".venv/bin/python"
C_FLAGS = ("-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-D_DEFAULT_SOURCE")
GAME_DIR = ROOT / "build/files/ja/RUSTY"
BOOT_ORIGINAL = GAME_DIR / "BOOT.COM"
BASE_GAME_DIR = ROOT / "build/files/base"
needs_game = pytest.mark.skipif(not BOOT_ORIGINAL.exists(), reason="Japanese game files not present")

needs_tools = pytest.mark.skipif(
    not VENV_PYTHON.exists() or shutil.which("cc") is None or shutil.which("nasm") is None,
    reason="needs .venv with capstone, cc and nasm",
)


def recompile_and_build(
    image: pathlib.Path, work: pathlib.Path, *, build: pathlib.Path, entries: list[str], options: tuple[str, ...] = ()
) -> pathlib.Path:
    generated = work / "gen.c"
    entry_args = [argument for entry in entries for argument in ("--entry", entry)]
    subprocess.run(
        [
            str(VENV_PYTHON),
            str(ROOT / "tools/recomp/recomp.py"),
            str(image),
            *entry_args,
            *options,
            "--prefix",
            "t",
            "--out",
            str(generated),
        ],
        check=True,
        capture_output=True,
    )
    link = work / "link.c"
    subprocess.run([sys.executable, str(ROOT / "tools/recomp/link.py"), "t", "--out", str(link)], check=True)
    objects = []
    for source in (generated, link):
        object_file = source.with_suffix(".o")
        subprocess.run(
            ["cc", *C_FLAGS, "-I", str(ROOT / "runtime"), "-c", str(source), "-o", str(object_file)],
            check=True,
            capture_output=True,
        )
        objects.append(str(object_file))
    binary = work / "program"
    libraries = [str(build / "librusty_runtime_headless.a"), str(build / "librusty_sound.a")]
    subprocess.run(["c++", *objects, *libraries, "-lm", "-o", str(binary)], check=True, capture_output=True)
    return binary


@needs_tools
@pytest.mark.parametrize("options", [(), ("--interpret",)], ids=["recompiled", "interpreted"])
def test_cpu_self_test(tmp_path: pathlib.Path, dev_build: pathlib.Path, options: tuple[str, ...]) -> None:
    image = tmp_path / "cpu_test.com"
    subprocess.run(["nasm", "-f", "bin", "-o", str(image), str(ROOT / "tests/recomp/cpu_test.asm")], check=True)
    binary = recompile_and_build(image, tmp_path, build=dev_build, entries=["100"], options=options)
    result = subprocess.run([str(binary), str(image)], capture_output=True, text=True, check=True)
    assert result.stdout.strip() == "ABCDEFGHI"


CONDITIONS = ("jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg")
VALUES = {
    16: ((0, 0), (1, 2), (0x7FFF, 0xFFFF), (0x8000, 1), (0xFFFF, 0xFFFF), (0x8000, 0x8000)),
    8: ((0, 0), (1, 2), (0x7F, 0xFF), (0x80, 1), (0xFF, 0xFF)),
}
REGISTERS = {16: ("ax", "bx", "word"), 8: ("al", "bl", "byte")}


def condition_case(
    operation: str, condition: str, *, width: int, values: tuple[int, int], form: str, label: str
) -> list[str]:
    first, second, size = REGISTERS[width]
    a, b = values
    if form == "memory":
        return [
            f"    mov {size} [value], {a}",
            f"    mov {second}, {b}",
            "    mov dl, '1'",
            f"    jmp {label}_compare",
            f"{label}_taken:",
            f"    jmp {label}_print",
            f"{label}_compare:",
            f"    {operation} {size} [value], {second}",
            f"    {condition} {label}_taken",
            "    mov dl, '0'",
            f"{label}_print:",
            "    test sp, sp",
            "    call putc",
        ]
    if form == "unary":
        operate = [f"    mov {first}, {a}", f"    {operation} {first}"]
    elif form == "immediate":
        operate = [f"    mov {first}, {a}", f"    {operation} {first}, {b}"]
    else:
        operate = [f"    mov {first}, {a}", f"    mov {second}, {b}", f"    {operation} {first}, {second}"]
    return [
        *operate,
        "    mov dl, '1'",
        f"    {condition} {label}_print",
        "    mov dl, '0'",
        f"{label}_print:",
        "    test sp, sp",
        "    call putc",
    ]


def condition_program() -> tuple[str, int]:
    """A COM program that prints 1 or 0 for every operation, conditional jump and value pair."""
    cases = []
    for condition in CONDITIONS:
        for width, pairs in VALUES.items():
            for values in pairs:
                forms = [(operation, "registers") for operation in ("cmp", "test", "and", "or", "xor", "add", "sub")]
                forms += [(operation, "unary") for operation in ("inc", "dec")]
                if width == 16:
                    forms += [
                        (operation, "immediate") for operation in ("cmp", "test", "and", "or", "xor", "add", "sub")
                    ]
                    forms += [(operation, "memory") for operation in ("cmp", "test")]
                for operation, form in forms:
                    label = f"case{len(cases)}"
                    cases.append(
                        condition_case(operation, condition, width=width, values=values, form=form, label=label)
                    )
    lines = ["cpu 186", "org 100h", *(line for case in cases for line in case)]
    lines += ["    mov ax, 0x4c00", "    int 0x21", "putc:", "    mov ah, 2", "    int 0x21", "    ret", "value: dw 0"]
    return "\n".join(lines) + "\n", len(cases)


@needs_tools
def test_conditional_jumps_folded_into_c_conditions_branch_like_the_interpreter(
    tmp_path: pathlib.Path, dev_build: pathlib.Path
) -> None:
    text, count = condition_program()
    source = tmp_path / "conditions.asm"
    source.write_text(text)
    image = tmp_path / "conditions.com"
    subprocess.run(["nasm", "-f", "bin", "-o", str(image), str(source)], check=True)
    outputs = {}
    for name, options in (("recompiled", ()), ("interpreted", ("--interpret",))):
        work = tmp_path / name
        work.mkdir()
        binary = recompile_and_build(image, work, build=dev_build, entries=["100"], options=options)
        result = subprocess.run([str(binary), str(image)], capture_output=True, text=True, check=True)
        outputs[name] = result.stdout
    jumps = [line for line in (tmp_path / "recompiled/gen.c").read_text().splitlines() if line.startswith("    if (")]
    assert sum("FLAG_" not in line for line in jumps) > count // 2
    assert len(outputs["recompiled"]) == count
    assert outputs["recompiled"] == outputs["interpreted"]


@needs_tools
@needs_game
def test_whole_game_chain_from_boot_com_to_the_first_cut_scene(
    tmp_path: pathlib.Path, rusty_headless: pathlib.Path
) -> None:
    environment = dict(
        os.environ,
        RUSTY_FAST="1",
        RUSTY_MAX_FRAMES="3000",
        RUSTY_ENTER_AT="1200",
        RUSTY_KEYS="2000:29",
        RUSTY_GAME_DIR=str(GAME_DIR),
        RUSTY_SAVE_DIR=str(tmp_path / "savedata"),
    )
    result = subprocess.run(
        [str(rusty_headless), str(BOOT_ORIGINAL)],
        capture_output=True,
        text=True,
        env=environment,
        cwd=ROOT,
        check=False,
    )
    assert result.returncode == 0, result.stderr[-2000:]
    programs = [line.split()[2] for line in result.stderr.splitlines() if line.startswith("[rt] run ")]
    assert programs == [
        "BOOT.COM",
        "ONGCHK.COM",
        "PDR.COM",
        "PPSDRV.COM",
        "PMDB2.COM",
        "CLABLOGO.EXE",
        "OP.COM",
        "JO.EXE",
    ]
    assert "JO.EXE: EXEPACK unpacked" in result.stderr
    for name in ("title.mag", "visual.com", "vs1.ani"):
        assert f'open "A:{name}"' in result.stderr


@needs_tools
@needs_game
def test_stage_one_building_event_finds_story1_by_its_code(
    tmp_path: pathlib.Path, rusty_headless: pathlib.Path
) -> None:
    attacks = ",".join(f"{frame}:29:4" for frame in range(13610, 17600, 20))
    environment = dict(
        os.environ,
        RUSTY_FAST="1",
        RUSTY_TAIL_SECONDS="0",
        RUSTY_MAX_FRAMES="17600",
        RUSTY_ENTER_AT="1200",
        RUSTY_KEYS=f"2000:29,13600:48:4000,{attacks}",
        RUSTY_GAME_DIR=str(GAME_DIR),
        RUSTY_SAVE_DIR=str(tmp_path / "savedata"),
    )
    result = subprocess.run(
        [str(rusty_headless), str(BOOT_ORIGINAL)],
        capture_output=True,
        text=True,
        env=environment,
        cwd=ROOT,
        check=False,
    )
    assert result.returncode == 0, result.stderr[-2000:]
    assert "STORY1.COM at 8ebe found by its code" in result.stderr


@needs_tools
@needs_game
def test_snapshot_load_continues_exactly_like_the_session_that_saved(
    tmp_path: pathlib.Path, rusty_headless: pathlib.Path
) -> None:
    attacks = ",".join(f"{frame}:29:4" for frame in range(13610, 17600, 20))
    states = tmp_path / "states"
    states.mkdir()
    base = dict(
        os.environ,
        RUSTY_FAST="1",
        RUSTY_TAIL_SECONDS="0",
        RUSTY_ENTER_AT="1200",
        RUSTY_KEYS=f"2000:29,13600:48:4000,{attacks}",
        RUSTY_MAX_FRAMES="17600",
        RUSTY_DUMP_EVERY="1600",
        RUSTY_GAME_DIR=str(GAME_DIR),
        RUSTY_STATE_DIR=str(states),
    )
    runs = {"saving": {"RUSTY_STATE_SAVE_AT": "14500:1"}, "loading": {"RUSTY_STATE_LOAD": "1"}}
    logs = {}
    for name, settings in runs.items():
        folder = tmp_path / name
        folder.mkdir()
        environment = dict(base, RUSTY_DUMP_DIR=str(folder), RUSTY_SAVE_DIR=str(tmp_path / f"savedata-{name}"))
        result = subprocess.run(
            [str(rusty_headless), str(BOOT_ORIGINAL)],
            capture_output=True,
            text=True,
            env=dict(environment, **settings),
            cwd=ROOT,
            check=True,
        )
        logs[name] = result.stderr
    assert "Saved to slot 1 (stage 1)" in logs["saving"]
    assert "Loaded slot 1 (stage 1)" in logs["loading"]
    for frame in ("016000", "017600"):
        saved = (tmp_path / "saving" / f"frame_{frame}.ppm").read_bytes()
        loaded = (tmp_path / "loading" / f"frame_{frame}.ppm").read_bytes()
        assert saved == loaded, frame


@needs_tools
@needs_game
def test_snapshot_load_releases_a_key_that_was_held_at_the_save(
    tmp_path: pathlib.Path, rusty_headless: pathlib.Path
) -> None:
    attacks = ",".join(f"{frame}:29:4" for frame in range(13610, 17600, 20))
    states = tmp_path / "states"
    states.mkdir()
    base = dict(
        os.environ,
        RUSTY_FAST="1",
        RUSTY_TAIL_SECONDS="0",
        RUSTY_ENTER_AT="1200",
        RUSTY_MAX_FRAMES="17600",
        RUSTY_DUMP_EVERY="1600",
        RUSTY_GAME_DIR=str(GAME_DIR),
        RUSTY_STATE_DIR=str(states),
    )
    held_at_save = f"2000:29,13600:48:4000,{attacks}"
    released_at_save = f"2000:29,13600:48:900,{attacks}"
    runs = {
        "saving": {"RUSTY_KEYS": held_at_save, "RUSTY_STATE_SAVE_AT": "14500:1"},
        "releasing": {"RUSTY_KEYS": released_at_save},
        "loading": {"RUSTY_KEYS": released_at_save, "RUSTY_STATE_LOAD": "1"},
    }
    for name, settings in runs.items():
        folder = tmp_path / name
        folder.mkdir()
        environment = dict(base, RUSTY_DUMP_DIR=str(folder), RUSTY_SAVE_DIR=str(tmp_path / f"savedata-{name}"))
        subprocess.run(
            [str(rusty_headless), str(BOOT_ORIGINAL)],
            capture_output=True,
            env=dict(environment, **settings),
            cwd=ROOT,
            check=True,
        )
    for frame in ("016000", "017600"):
        released = (tmp_path / "releasing" / f"frame_{frame}.ppm").read_bytes()
        loaded = (tmp_path / "loading" / f"frame_{frame}.ppm").read_bytes()
        held = (tmp_path / "saving" / f"frame_{frame}.ppm").read_bytes()
        assert loaded == released, frame
        assert loaded != held, frame


@needs_tools
@needs_game
def test_idle_loop_skip_reaches_the_same_state_as_running_every_pass(
    tmp_path: pathlib.Path, rusty_headless: pathlib.Path
) -> None:
    attacks = ",".join(f"{frame}:29:4" for frame in range(13610, 15600, 20))
    recording = tmp_path / "run.rec"
    base = dict(os.environ, RUSTY_FAST="1", RUSTY_TAIL_SECONDS="0", RUSTY_GAME_DIR=str(GAME_DIR))
    recorded = dict(
        base,
        RUSTY_ENTER_AT="1200",
        RUSTY_KEYS=f"2000:29,13600:48:2000,{attacks}",
        RUSTY_MAX_FRAMES="15601",
        RUSTY_SAVE_AT="15600",
        RUSTY_SAVE=str(recording),
        RUSTY_SAVE_DIR=str(tmp_path / "savedata"),
    )
    subprocess.run([str(rusty_headless), str(BOOT_ORIGINAL)], capture_output=True, env=recorded, cwd=ROOT, check=True)
    folders = {}
    ends = {}
    for setting in ("1", "0"):
        folder = tmp_path / f"skip-{setting}"
        folder.mkdir()
        # The replay sets the DOS clock from the recording, so the end state is the same in both runs.
        replay = dict(
            base,
            RUSTY_LOAD=str(recording),
            RUSTY_MAX_FRAMES="15600",
            RUSTY_DUMP_EVERY="300",
            RUSTY_DUMP_DIR=str(folder),
            RUSTY_SAVE_DIR=str(tmp_path / f"savedata-{setting}"),
            RUSTY_LOOP_SKIP=setting,
        )
        result = subprocess.run(
            [str(rusty_headless), str(BOOT_ORIGINAL)], capture_output=True, text=True, env=replay, cwd=ROOT, check=True
        )
        folders[setting] = folder
        ends[setting] = [line for line in result.stderr.splitlines() if "frame limit" in line]
    assert len(ends["1"]) == 1
    assert "instructions" in ends["1"][0]
    assert ends["1"] == ends["0"]
    frames = sorted(path.name for path in folders["1"].iterdir())
    assert len(frames) == 52
    assert frames == sorted(path.name for path in folders["0"].iterdir())
    for name in frames:
        assert (folders["1"] / name).read_bytes() == (folders["0"] / name).read_bytes(), name


@needs_tools
@pytest.mark.skipif(not (BASE_GAME_DIR / "BOOT.COM").exists(), reason="English game files not present")
def test_english_version_draws_the_menu_with_half_width_letters_through_the_egc(
    tmp_path: pathlib.Path, rusty_headless: pathlib.Path
) -> None:
    environment = dict(
        os.environ,
        RUSTY_FAST="1",
        RUSTY_TAIL_SECONDS="0",
        RUSTY_ENTER_AT="1200",
        RUSTY_MAX_FRAMES="1500",
        RUSTY_GAME_DIR=str(BASE_GAME_DIR),
        RUSTY_SAVE_DIR=str(tmp_path / "savedata"),
        RUSTY_TRACE_FONT="1",
    )
    result = subprocess.run(
        [str(rusty_headless), str(BASE_GAME_DIR / "BOOT.COM")],
        capture_output=True,
        text=True,
        env=environment,
        cwd=ROOT,
        check=False,
    )
    assert result.returncode == 0, result.stderr[-2000:]
    assert 'run JO.EXE "": en/JO.EXE' in result.stderr
    assert 'open "A:grpegc.com"' in result.stderr
    assert "[font] read A3=09" in result.stderr
