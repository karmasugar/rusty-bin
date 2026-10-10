import argparse
import dataclasses
import pathlib
import re
import sys

from PIL import Image, ImageDraw

from lz_unpack import is_lz, lz_unpack

TILE_WIDTH = 32
TILE_HEIGHT = 16
TILE_BYTES_PER_LINE = TILE_WIDTH // 8
STAGES = range(1, 11)
FILE_NAME = re.compile(rb"([a-z0-9_]+)\.(dat|rgb|map)\0")
ATTRIBUTE_COLORS = {0x80: (255, 0, 0, 110), 0x40: (0, 140, 255, 90), 0x20: (255, 230, 0, 140)}
DEFAULT_OUT = pathlib.Path(__file__).resolve().parent.parent / "build/maps"


@dataclasses.dataclass(frozen=True)
class StageMap:
    stage: int
    map_name: str
    tiles_name: str
    palette_name: str


def read_game_file(folder: pathlib.Path, name: str) -> bytes | None:
    for path in folder.iterdir():
        if path.name.lower() == name.lower():
            data = path.read_bytes()
            return lz_unpack(data) if is_lz(data) else data
    return None


def is_tile_set(stem: str) -> bool:
    return stem.startswith(("bg", "lasbos")) and "spr" not in stem


def stage_maps(folder: pathlib.Path) -> list[StageMap]:
    """The maps of each STORYn.COM, with the tile set and palette that the stage script loads last before the map."""
    found = []
    for stage in STAGES:
        script = read_game_file(folder, f"STORY{stage}.COM")
        if script is None:
            continue
        tiles_name = palette_name = ""
        for match in FILE_NAME.finditer(script):
            stem, extension = match.group(1).decode(), match.group(2).decode()
            name = f"{stem}.{extension}"
            if extension == "dat" and is_tile_set(stem):
                tiles_name = name
            elif extension == "rgb":
                palette_name = name
            elif extension == "map" and tiles_name and palette_name:
                found.append(StageMap(stage, name, tiles_name, palette_name))
    return found


def palette_of(data: bytes) -> list[int]:
    return [value * 17 for value in data[:48]]


def tile_images(data: bytes, palette: list[int]) -> list[Image.Image]:
    """Tiles of 32x16 pixels: 4 bit planes one after the other, 4 bytes per line, 16 lines per tile."""
    plane_size = len(data) // 4
    lines = plane_size // TILE_BYTES_PER_LINE
    pixels = []
    for line in range(lines):
        for column in range(TILE_BYTES_PER_LINE):
            planes = [data[plane * plane_size + line * TILE_BYTES_PER_LINE + column] for plane in range(4)]
            for bit in range(7, -1, -1):
                pixels.append(sum(((planes[plane] >> bit) & 1) << plane for plane in range(4)))
    sheet = Image.new("P", (TILE_WIDTH, lines))
    sheet.putpalette(palette)
    sheet.putdata(pixels)
    return [sheet.crop((0, top, TILE_WIDTH, top + TILE_HEIGHT)) for top in range(0, lines, TILE_HEIGHT)]


def render_map(data: bytes, tiles: list[Image.Image], *, palette: list[int], attributes: bool) -> Image.Image:
    """MAP: width and height (16-bit), then a tile number per cell, then an attribute byte per cell."""
    width = data[0] | data[1] << 8
    height = data[2] | data[3] << 8
    cells = data[4 : 4 + width * height]
    flags = data[4 + width * height : 4 + 2 * width * height]
    image = Image.new("P", (width * TILE_WIDTH, height * TILE_HEIGHT))
    image.putpalette(palette)
    for index, tile in enumerate(cells):
        if tile < len(tiles):
            image.paste(tiles[tile], ((index % width) * TILE_WIDTH, (index // width) * TILE_HEIGHT))
    if not attributes:
        return image
    layer = Image.new("RGBA", image.size)
    draw = ImageDraw.Draw(layer)
    for index, value in enumerate(flags):
        left, top = (index % width) * TILE_WIDTH, (index // width) * TILE_HEIGHT
        for bit, color in ATTRIBUTE_COLORS.items():
            if value & bit:
                draw.rectangle((left, top, left + TILE_WIDTH - 1, top + TILE_HEIGHT - 1), fill=color)
    return Image.alpha_composite(image.convert("RGBA"), layer)


def main() -> int:
    parser = argparse.ArgumentParser(description="Render the stage maps of Rusty (PC-98) to PNG files.")
    parser.add_argument("game", type=pathlib.Path, help="folder with the game files (packed or unpacked)")
    parser.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUT, help=f"output folder (default {DEFAULT_OUT})")
    parser.add_argument(
        "--attributes", action="store_true", help="mark deadly (red), front layer (blue) and rope/hang (yellow) cells"
    )
    args = parser.parse_args()

    maps = stage_maps(args.game)
    if not maps:
        print(f"{args.game}: no STORYn.COM files found", file=sys.stderr)
        return 1
    args.out.mkdir(parents=True, exist_ok=True)
    for entry in maps:
        files = {
            name: read_game_file(args.game, name) for name in (entry.map_name, entry.tiles_name, entry.palette_name)
        }
        missing = [name for name, data in files.items() if data is None]
        if missing:
            print(f"stage {entry.stage}: {entry.map_name} skipped, missing {', '.join(missing)}")
            continue
        palette = palette_of(files[entry.palette_name])
        tiles = tile_images(files[entry.tiles_name], palette)
        image = render_map(files[entry.map_name], tiles, palette=palette, attributes=args.attributes)
        target = args.out / f"stage{entry.stage:02}_{entry.map_name.removesuffix('.map')}.png"
        image.save(target)
        print(
            f"stage {entry.stage}: {entry.map_name} + {entry.tiles_name} + {entry.palette_name} -> {target} {image.size}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
