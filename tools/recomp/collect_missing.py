import argparse
import pathlib
import re
import sys

FAMILY = re.compile(r"^(ENEMY|STORY)(\d+)\.COM$")
PRINT_SIZE = 16
# Module name prefixes of other versions (en/JO.EXE) and their unpacked files next to build/unpacked/ja.
VERSION_FILES = {"en": "base"}


def same_code_in_family(name: str, address: str, *, files: pathlib.Path) -> list[str]:
    folder, _, base_name = name.rpartition("/")
    if folder:
        files = files.parent / VERSION_FILES.get(folder, folder)
        return [f"{folder}/{sibling}" for sibling in same_code_in_family(base_name, address, files=files)]
    match = FAMILY.match(name)
    if not match or not files.is_dir():
        return [name]
    segment, offset = (int(part, 16) for part in address.split(":"))
    source = files / name
    if segment != 0 or not source.exists():
        return [name]
    code = source.read_bytes()[offset : offset + PRINT_SIZE]
    names = []
    for sibling in sorted(files.glob(f"{match.group(1)}*.COM")):
        if FAMILY.match(sibling.name) and sibling.read_bytes()[offset : offset + PRINT_SIZE] == code:
            names.append(sibling.name)
    return names or [name]


def collect(missing: pathlib.Path, entries: pathlib.Path, *, files: pathlib.Path) -> dict[str, int]:
    added = {}
    if not missing.exists():
        return added
    found: dict[str, set[str]] = {}
    for line in missing.read_text().splitlines():
        parts = line.split()
        if len(parts) == 2 and ":" in parts[1]:
            address = parts[1].lower()
            for name in same_code_in_family(parts[0], address, files=files):
                found.setdefault(name, set()).add(address)
    entries.mkdir(parents=True, exist_ok=True)
    for name, addresses in found.items():
        path = entries / f"{name}.txt"
        path.parent.mkdir(parents=True, exist_ok=True)
        known = set(path.read_text().split()) if path.exists() else set()
        new = addresses - known
        if new:
            path.write_text("".join(f"{address}\n" for address in sorted(known | addresses)))
            added[name] = len(new)
    missing.unlink()
    return added


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Move the runtime missing-address log into tools/recomp/entries/<FILE>.txt (kept in the project). "
        "An address in ENEMYn/STORYn is also added to the other files of the family with the same code there."
    )
    root = pathlib.Path(__file__).resolve().parent.parent.parent
    parser.add_argument("--missing", type=pathlib.Path, default=root / "build/recomp/missing.txt")
    parser.add_argument("--entries", type=pathlib.Path, default=root / "tools/recomp/entries")
    parser.add_argument(
        "--files", type=pathlib.Path, default=root / "build/unpacked/ja", help="unpacked game files (ENEMYn/STORYn)"
    )
    args = parser.parse_args()
    for name, count in collect(args.missing, args.entries, files=args.files).items():
        print(f"{name}: {count} new entry points")
    return 0


if __name__ == "__main__":
    sys.exit(main())
