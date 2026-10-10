import argparse
import pathlib
import struct
import sys

PATTERN_SIZES = (16, 24, 32, 48)


def image_of(data: bytes, *, exe: bool) -> bytes:
    if not exe:
        return data
    header = struct.unpack_from("<H", data, 8)[0] * 16
    return data[header:]


def find_unique(image: bytes, pattern: bytes) -> int | None:
    first = image.find(pattern)
    if first < 0 or image.find(pattern, first + 1) >= 0:
        return None
    return first


def port_address(source: bytes, target: bytes, offset: int) -> int | None:
    """The same code in the target image: the source bytes at offset, found once (longer patterns if needed)."""
    if target[offset : offset + PATTERN_SIZES[0]] == source[offset : offset + PATTERN_SIZES[0]]:
        return offset
    for size in PATTERN_SIZES:
        pattern = source[offset : offset + size]
        if len(pattern) < size:
            return None
        found = find_unique(target, pattern)
        if found is not None:
            return found
    return None


def port_lines(lines: list[str], source: bytes, target: bytes, *, base: int) -> tuple[list[str], int, int]:
    """Names/entries lines with SEG:OFF addresses moved to the target image; lines not found are left out."""
    out = []
    moved = missing = 0
    for line in lines:
        if not line.strip() or line.startswith("#"):
            out.append(line)
            continue
        address, _, rest = line.partition("\t") if "\t" in line else (line.strip(), "", "")
        if ":" not in address:
            out.append(line)
            continue
        segment, offset = (int(part, 16) for part in address.split(":"))
        position = segment * 16 + offset - base
        kind = rest.split("\t")[0] if rest else "func"
        if kind != "func":
            if source[position : position + 2] == target[position : position + 2] or len(source) == len(target):
                out.append(line)
            continue
        found = port_address(source, target, position)
        if found is None:
            missing += 1
            continue
        new_offset = found + base - segment * 16
        if new_offset != offset:
            moved += 1
        new_address = f"{segment:04X}:{new_offset:04X}" if address.isupper() else f"{segment:04x}:{new_offset:04x}"
        out.append(new_address + (("\t" + rest) if rest else ""))
    return out, moved, missing


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Move the addresses of a names or entries file to another version of the program (the English "
        "patch): each function is found by its code bytes."
    )
    parser.add_argument("names", type=pathlib.Path, help="names .tsv or entries .txt of the source version")
    parser.add_argument("source", type=pathlib.Path, help="unpacked program of the source version")
    parser.add_argument("target", type=pathlib.Path, help="unpacked program of the target version")
    parser.add_argument(
        "--base", type=lambda text: int(text, 16), default=0x100, help="image base (COM 100, overlay 0)"
    )
    parser.add_argument("--exe", action="store_true", help="MZ executables (image after the header, base 0)")
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    source = image_of(args.source.read_bytes(), exe=args.exe)
    target = image_of(args.target.read_bytes(), exe=args.exe)
    lines, moved, missing = port_lines(
        args.names.read_text().splitlines(), source, target, base=0 if args.exe else args.base
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("".join(f"{line}\n" for line in lines))
    print(f"{args.names.name}: {moved} moved, {missing} not found -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
