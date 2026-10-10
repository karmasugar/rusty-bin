import argparse
import pathlib
import struct
import sys

CODE_START = 0x16


def handler_addresses(image: bytes) -> list[int]:
    table = struct.unpack_from("<H", image, 2)[0]
    first_list = struct.unpack_from("<H", image, table)[0]
    handlers = set()
    for group in range((first_list - table) // 2):
        cursor = struct.unpack_from("<H", image, table + group * 2)[0]
        while cursor + 1 < len(image):
            value = struct.unpack_from("<H", image, cursor)[0]
            if not CODE_START <= value < table:
                break
            handlers.add(value)
            cursor += 2
    return sorted(handlers)


def main() -> int:
    parser = argparse.ArgumentParser(description="List the AI handler addresses of an unpacked ENEMYn.COM overlay.")
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path, required=True, help="entries file (one hex offset per line)")
    args = parser.parse_args()
    handlers = handler_addresses(args.image.read_bytes())
    args.out.write_text("".join(f"{value:04x}\n" for value in handlers))
    print(f"{args.image.name}: {len(handlers)} handler addresses")
    return 0


if __name__ == "__main__":
    sys.exit(main())
