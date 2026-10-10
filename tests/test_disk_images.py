import os
import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
ROMS = ROOT / "roms"
ENGLISH_FILES = ROOT / "build/files/base"
JAPANESE = "game version: Japanese"
ENGLISH = "game version: English (46 OkuMen v1.0.3)"

needs_tools = pytest.mark.skipif(
    not (ROOT / ".venv/bin/python").exists() or shutil.which("cc") is None,
    reason="needs .venv with capstone and cc",
)


def run_game(
    binary: pathlib.Path, game: pathlib.Path, *, save_dir: pathlib.Path, frames: int
) -> subprocess.CompletedProcess:
    environment = dict(
        os.environ,
        RUSTY_FAST="1",
        RUSTY_TAIL_SECONDS="0",
        RUSTY_ENTER_AT="1200",
        RUSTY_MAX_FRAMES=str(frames),
        RUSTY_SAVE_DIR=str(save_dir),
    )
    environment.pop("RUSTY_GAME_DIR", None)
    return subprocess.run(
        [str(binary), str(game)], capture_output=True, text=True, env=environment, cwd=ROOT, check=False
    )


@needs_tools
@pytest.mark.parametrize(
    ("game", "version", "program"),
    [
        ("Rusty (19xx)(C-Lab)/Rusty (19xx)(C-Lab).hdi", JAPANESE, 'run JO.EXE "": JO.EXE'),
        ("Rusty (Japan) (T-En by 46 OkuMen v1.0.3) (Floppy)", ENGLISH, 'run JO.EXE "": en/JO.EXE'),
    ],
)
def test_game_starts_from_a_disk_image_with_the_matching_modules(
    rusty_headless: pathlib.Path, tmp_path: pathlib.Path, game: str, version: str, program: str
) -> None:
    if not (ROMS / game).exists():
        pytest.skip(f"{game} not present")
    result = run_game(rusty_headless, ROMS / game, save_dir=tmp_path, frames=1500)
    assert result.returncode == 0, result.stderr[-2000:]
    assert version in result.stderr
    assert program in result.stderr


@needs_tools
@pytest.mark.skipif(not (ENGLISH_FILES / "BOOT.COM").exists(), reason="English game files not present")
def test_unknown_code_file_stops_with_its_name(rusty_headless: pathlib.Path, tmp_path: pathlib.Path) -> None:
    game = tmp_path / "game"
    shutil.copytree(ENGLISH_FILES, game)
    story = bytearray((game / "STORY1.COM").read_bytes())
    story[100] ^= 0xFF
    (game / "STORY1.COM").write_bytes(story)
    result = run_game(rusty_headless, game, save_dir=tmp_path / "savedata", frames=10)
    assert result.returncode == 3
    assert "STORY1.COM: unknown version of this code file" in result.stderr
