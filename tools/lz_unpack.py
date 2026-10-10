import argparse
import pathlib
import shutil
import struct
import sys

LZ_MAGIC = b"LZ\x1a"
LZ_HEADER_SIZE = 7
RING_SIZE = 4096
MATCH_MAX = 18
MATCH_MIN = 3


class LzFormatError(Exception):
    pass


def is_lz(data: bytes) -> bool:
    return data.startswith(LZ_MAGIC) and len(data) >= LZ_HEADER_SIZE


def lzss_decode(data: bytes, *, size: int) -> bytes:
    ring = bytearray(RING_SIZE)
    ring_pos = RING_SIZE - MATCH_MAX
    out = bytearray()
    position = 0
    flags = 0
    while len(out) < size:
        flags >>= 1
        if not flags & 0x100:
            if position >= len(data):
                break
            flags = data[position] | 0xFF00
            position += 1
        if flags & 1:
            if position >= len(data):
                break
            value = data[position]
            position += 1
            out.append(value)
            ring[ring_pos] = value
            ring_pos = (ring_pos + 1) % RING_SIZE
            continue
        if position + 1 >= len(data):
            break
        low, high = data[position], data[position + 1]
        position += 2
        match_pos = low | ((high & 0xF0) << 4)
        for step in range((high & 0x0F) + MATCH_MIN):
            value = ring[(match_pos + step) % RING_SIZE]
            out.append(value)
            ring[ring_pos] = value
            ring_pos = (ring_pos + 1) % RING_SIZE
    if len(out) < size:
        raise LzFormatError(f"stream ended at {len(out)} of {size} bytes")
    return bytes(out[:size])


def lz_unpack(data: bytes) -> bytes:
    if not is_lz(data):
        raise LzFormatError("missing LZ header")
    size = struct.unpack_from("<I", data, 3)[0]
    return lzss_decode(data[LZ_HEADER_SIZE:], size=size)


def unpack_path(source: pathlib.Path, destination: pathlib.Path) -> bool:
    data = source.read_bytes()
    if not is_lz(data):
        return False
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(lz_unpack(data))
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="Unpack Rusty LZ-compressed files (LZ 1A header + LZSS).")
    parser.add_argument("source", type=pathlib.Path, help="packed file or directory of extracted game files")
    parser.add_argument("destination", type=pathlib.Path, help="output file or directory")
    parser.add_argument("--copy-plain", action="store_true", help="also copy files that are not packed")
    args = parser.parse_args()

    if args.source.is_file():
        if not unpack_path(args.source, args.destination):
            print(f"{args.source}: not an LZ file", file=sys.stderr)
            return 1
        print(f"{args.source} -> {args.destination}")
        return 0

    count = 0
    copied = 0
    for source in sorted(path for path in args.source.rglob("*") if path.is_file()):
        destination = args.destination / source.relative_to(args.source)
        if unpack_path(source, destination):
            print(f"{source.relative_to(args.source)}: {source.stat().st_size} -> {destination.stat().st_size}")
            count += 1
        elif args.copy_plain:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
            copied += 1
    print(f"unpacked {count} files, copied {copied} plain files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
