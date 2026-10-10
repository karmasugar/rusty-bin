import pathlib

import pytest

import exepack
import lz_unpack
import pc98disk

ROMS = pathlib.Path(__file__).resolve().parent.parent / "roms"
JA_HDI = ROMS / "Rusty (19xx)(C-Lab)" / "Rusty (19xx)(C-Lab).hdi"
BASE_HDI = ROMS / "Rusty (Japan) [T-En by 46 OkuMen v1.0.3].hdi"
SYSTEM_D88 = (
    ROMS
    / "Rusty (Japan) (T-En by 46 OkuMen v1.0.3) (Floppy)"
    / "Rusty (Japan) (System Disk) [T-En by 46 OkuMen v1.0.3].d88"
)


@pytest.mark.skipif(not SYSTEM_D88.exists() or not BASE_HDI.exists(), reason="game images not present")
def test_d88_matches_hdi() -> None:
    floppy = pc98disk.open_volume(SYSTEM_D88)
    hdi = pc98disk.open_volume(BASE_HDI)
    floppy_entries = dict(floppy.walk())
    hdi_entries = dict(hdi.walk())
    assert floppy.read_file(floppy_entries["JO.EXE"]) == hdi.read_file(hdi_entries["JO.EXE"])


@pytest.mark.skipif(not BASE_HDI.exists(), reason="game image not present")
def test_real_overlays_unpack() -> None:
    volume = pc98disk.open_volume(BASE_HDI)
    entries = dict(volume.walk())
    enemy1 = lz_unpack.lz_unpack(volume.read_file(entries["ENEMY1.COM"]))
    assert len(enemy1) == 17762
    visual = lz_unpack.lz_unpack(volume.read_file(entries["VISUAL.COM"]))
    assert b"We've finally been attacked" in visual
    title = lz_unpack.lz_unpack(volume.read_file(entries["TITLE.MAG"]))
    assert title.startswith(b"MAKI02")


@pytest.mark.skipif(not JA_HDI.exists(), reason="game image not present")
def test_real_jo_exe() -> None:
    volume = pc98disk.open_volume(JA_HDI)
    entries = dict(volume.walk())
    packed = volume.read_file(entries["RUSTY/JO.EXE"])
    assert not lz_unpack.is_lz(packed)
    assert exepack.is_exepack(packed)
    unpacked = exepack.unpack(packed)
    header = exepack.MzHeader.parse(unpacked)
    assert header.relocation_count == 259
    assert (header.cs, header.ip) == (0, 0)
    image = unpacked[header.header_paragraphs * 16 :]
    assert len(image) == 0x6A5A * 16
    assert image[:3] == b"\x60\x1e\x06"
    assert b"story1.com" in image
