import argparse
import os
import pathlib
import sys

import pc98disk


def extract_image(image: pathlib.Path, destination: pathlib.Path, *, verbose: bool) -> int:
    volume = pc98disk.open_volume(image)
    count = 0
    for relative, entry in volume.walk():
        target = destination / relative
        if entry.is_dir:
            target.mkdir(parents=True, exist_ok=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(volume.read_file(entry))
        if entry.modified is not None:
            stamp = entry.modified.timestamp()
            os.utime(target, (stamp, stamp))
        count += 1
        if verbose:
            print(f"{relative:40} {entry.size:>8}")
    return count


def list_image(image: pathlib.Path) -> None:
    volume = pc98disk.open_volume(image)
    for relative, entry in volume.walk():
        stamp = entry.modified.isoformat(sep=" ") if entry.modified else "-"
        size = "<DIR>" if entry.is_dir else str(entry.size)
        print(f"{relative:40} {size:>8}  {stamp}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Extract files from PC-98 disk images (HDI, D88, raw FAT).")
    parser.add_argument("images", type=pathlib.Path, nargs="+", help="disk image files")
    parser.add_argument("-o", "--output", type=pathlib.Path, help="output directory (all images merge into it)")
    parser.add_argument("-l", "--list", action="store_true", help="list files only")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if not args.list and args.output is None:
        parser.error("give --output or --list")

    for image in args.images:
        try:
            if args.list:
                print(f"== {image}")
                list_image(image)
                continue
            count = extract_image(image, args.output, verbose=args.verbose)
        except pc98disk.DiskFormatError as error:
            print(f"{image}: {error}", file=sys.stderr)
            return 1
        print(f"{image.name}: {count} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
