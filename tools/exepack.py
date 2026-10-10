import argparse
import dataclasses
import pathlib
import struct
import sys

EXEPACK_SIGNATURE = b"RB"
CORRUPT_MESSAGE = b"Packed file is corrupt"
MZ_HEADER_WORDS = 14
RELOCATION_SECTIONS = 16


class ExepackError(Exception):
    pass


@dataclasses.dataclass(frozen=True)
class MzHeader:
    last_page_bytes: int
    pages: int
    relocation_count: int
    header_paragraphs: int
    min_alloc: int
    max_alloc: int
    ss: int
    sp: int
    checksum: int
    ip: int
    cs: int
    relocation_offset: int
    overlay: int

    @classmethod
    def parse(cls, data: bytes) -> "MzHeader":
        if data[:2] not in (b"MZ", b"ZM"):
            raise ExepackError("not an MZ executable")
        return cls(*struct.unpack_from("<13H", data, 2))

    @property
    def file_size(self) -> int:
        return self.pages * 512 - (512 - self.last_page_bytes if self.last_page_bytes else 0)


@dataclasses.dataclass(frozen=True)
class ExepackHeader:
    real_ip: int
    real_cs: int
    exepack_size: int
    real_sp: int
    real_ss: int
    dest_len: int
    skip_len: int


def parse_exepack_header(stub: bytes) -> ExepackHeader:
    if stub[16:18] == EXEPACK_SIGNATURE:
        real_ip, real_cs, _mem_start, exepack_size, real_sp, real_ss, dest_len, skip_len = struct.unpack_from(
            "<8H", stub, 0
        )
    elif stub[14:16] == EXEPACK_SIGNATURE:
        real_ip, real_cs, _mem_start, exepack_size, real_sp, real_ss, dest_len = struct.unpack_from("<7H", stub, 0)
        skip_len = 1
    else:
        raise ExepackError("no EXEPACK signature at CS:0000")
    return ExepackHeader(
        real_ip=real_ip,
        real_cs=real_cs,
        exepack_size=exepack_size,
        real_sp=real_sp,
        real_ss=real_ss,
        dest_len=dest_len,
        skip_len=skip_len,
    )


def is_exepack(data: bytes) -> bool:
    try:
        header = MzHeader.parse(data)
    except ExepackError:
        return False
    stub_start = header.header_paragraphs * 16 + header.cs * 16
    return data[stub_start + 14 : stub_start + 16] == EXEPACK_SIGNATURE or (
        data[stub_start + 16 : stub_start + 18] == EXEPACK_SIGNATURE
    )


def decompress(compressed: bytes, *, dest_len: int) -> bytes:
    buffer = bytearray(max(dest_len * 16, len(compressed)))
    buffer[: len(compressed)] = compressed
    src = len(compressed)
    padding = 0
    while padding < 16 and src > 0 and buffer[src - 1] == 0xFF:
        src -= 1
        padding += 1
    dst = dest_len * 16
    while True:
        if src < 3:
            raise ExepackError("compressed stream ends early")
        src -= 1
        command = buffer[src]
        src -= 2
        length = buffer[src] | (buffer[src + 1] << 8)
        kind = command & 0xFE
        if kind == 0xB0:
            src -= 1
            fill = buffer[src]
            dst -= length
            buffer[dst : dst + length] = bytes([fill]) * length
        elif kind == 0xB2:
            for _ in range(length):
                src -= 1
                dst -= 1
                buffer[dst] = buffer[src]
        else:
            raise ExepackError(f"unknown command {command:#04x} at {src:#x}")
        if dst < src:
            raise ExepackError("output overwrites unread input")
        if command & 1:
            break
    return bytes(buffer[: dest_len * 16])


def parse_relocations(table: bytes) -> list[tuple[int, int]]:
    relocations = []
    position = 0
    for section in range(RELOCATION_SECTIONS):
        count = struct.unpack_from("<H", table, position)[0]
        position += 2
        for _ in range(count):
            offset = struct.unpack_from("<H", table, position)[0]
            position += 2
            relocations.append((section * 0x1000, offset))
    return relocations


def build_mz(image: bytes, *, relocations: list[tuple[int, int]], source: MzHeader, packed: ExepackHeader) -> bytes:
    header_size = MZ_HEADER_WORDS * 2 + len(relocations) * 4
    header_size = (header_size + 15) // 16 * 16
    file_size = header_size + len(image)
    stub_paragraphs = (packed.exepack_size + 15) // 16
    image_paragraphs = (len(image) + 15) // 16
    packed_image_paragraphs = source.cs + stub_paragraphs
    min_alloc = max(0, source.min_alloc + packed_image_paragraphs - image_paragraphs)
    header = struct.pack(
        "<2s13H",
        b"MZ",
        file_size % 512,
        (file_size + 511) // 512,
        len(relocations),
        header_size // 16,
        min_alloc,
        source.max_alloc,
        packed.real_ss,
        packed.real_sp,
        0,
        packed.real_ip,
        packed.real_cs,
        MZ_HEADER_WORDS * 2,
        0,
    )
    table = b"".join(struct.pack("<HH", offset, segment) for segment, offset in relocations)
    return (header + table).ljust(header_size, b"\x00") + image


def unpack(data: bytes) -> bytes:
    source = MzHeader.parse(data)
    image_start = source.header_paragraphs * 16
    image = data[image_start : source.file_size]
    stub_offset = source.cs * 16
    stub = image[stub_offset:]
    packed = parse_exepack_header(stub)
    message = stub.find(CORRUPT_MESSAGE)
    if message < 0:
        raise ExepackError("EXEPACK stub message not found")
    relocations = parse_relocations(stub[message + len(CORRUPT_MESSAGE) : packed.exepack_size])
    compressed = image[: stub_offset - (packed.skip_len - 1) * 16]
    unpacked = decompress(compressed, dest_len=packed.dest_len)
    return build_mz(unpacked, relocations=relocations, source=source, packed=packed)


def main() -> int:
    parser = argparse.ArgumentParser(description="Unpack Microsoft EXEPACK executables.")
    parser.add_argument(
        "source", type=pathlib.Path, help="packed EXE, or directory (unpacks every packed .EXE in place)"
    )
    parser.add_argument("destination", type=pathlib.Path, nargs="?", help="output file (file mode only)")
    args = parser.parse_args()

    if args.source.is_dir():
        for path in sorted(args.source.rglob("*")):
            if path.suffix.upper() == ".EXE" and is_exepack(path.read_bytes()):
                result = unpack(path.read_bytes())
                path.write_bytes(result)
                print(f"{path.relative_to(args.source)}: unpacked to {len(result)} bytes")
        return 0

    data = args.source.read_bytes()
    if not is_exepack(data):
        print(f"{args.source}: not an EXEPACK file", file=sys.stderr)
        return 1
    if args.destination is None:
        parser.error("give a destination file")
    result = unpack(data)
    args.destination.write_bytes(result)
    print(f"{args.source} -> {args.destination} ({len(result)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
