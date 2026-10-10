import dataclasses
import datetime
import pathlib
import struct

D88_HEADER_SIZE = 0x2B0
D88_TRACK_COUNT = 164
D88_SECTOR_HEADER_SIZE = 0x10
PC98_PARTITION_ENTRY_SIZE = 32


class DiskFormatError(Exception):
    pass


@dataclasses.dataclass(frozen=True)
class DirEntry:
    name: str
    attributes: int
    first_cluster: int
    size: int
    modified: datetime.datetime | None

    @property
    def is_dir(self) -> bool:
        return bool(self.attributes & 0x10)

    @property
    def is_volume_label(self) -> bool:
        return bool(self.attributes & 0x08)


@dataclasses.dataclass(frozen=True)
class Partition:
    name: str
    offset: int


def read_hdi(data: bytes) -> tuple[bytes, list[Partition]]:
    header_size, data_size, sector_size, sectors, heads, _cylinders = struct.unpack_from("<6I", data, 8)
    body = data[header_size : header_size + data_size]
    table = body[sector_size : sector_size + 512]
    partitions = []
    for index in range(len(table) // PC98_PARTITION_ENTRY_SIZE):
        entry = table[index * PC98_PARTITION_ENTRY_SIZE : (index + 1) * PC98_PARTITION_ENTRY_SIZE]
        if entry[0] == 0:
            continue
        start_sector, start_head, start_cylinder = struct.unpack_from("<BBH", entry, 8)
        lba = (start_cylinder * heads + start_head) * sectors + start_sector
        name = entry[16:32].decode("cp932", errors="replace").rstrip(" \x00")
        partitions.append(Partition(name=name, offset=lba * sector_size))
    if not partitions:
        raise DiskFormatError("HDI image has no PC-98 partition table")
    return body, partitions


def read_d88(data: bytes) -> bytes:
    track_offsets = struct.unpack_from(f"<{D88_TRACK_COUNT}I", data, 0x20)
    flat = bytearray()
    for track_offset in track_offsets:
        if track_offset == 0:
            continue
        sectors = []
        position = track_offset
        sector_count = struct.unpack_from("<H", data, position + 4)[0]
        for _ in range(sector_count):
            _c, _h, record, _n, _count, _density, _deleted, _status = struct.unpack_from("<4BH3B", data, position)
            size = struct.unpack_from("<H", data, position + 0x0E)[0]
            payload = data[position + D88_SECTOR_HEADER_SIZE : position + D88_SECTOR_HEADER_SIZE + size]
            sectors.append((record, payload))
            position += D88_SECTOR_HEADER_SIZE + size
        for _record, payload in sorted(sectors):
            flat += payload
    return bytes(flat)


def open_volume(path: pathlib.Path) -> "FatVolume":
    data = path.read_bytes()
    suffix = path.suffix.lower()
    if suffix == ".hdi":
        body, partitions = read_hdi(data)
        return FatVolume(body, offset=partitions[0].offset)
    if suffix == ".d88":
        return FatVolume(read_d88(data), offset=0)
    return FatVolume(data, offset=0)


def decode_dos_datetime(date: int, time: int) -> datetime.datetime | None:
    try:
        return datetime.datetime(  # noqa: DTZ001 DOS stores local time
            1980 + (date >> 9),
            (date >> 5) & 0x0F,
            date & 0x1F,
            time >> 11,
            (time >> 5) & 0x3F,
            (time & 0x1F) * 2,
        )
    except ValueError:
        return None


def decode_dos_name(raw: bytes) -> str:
    if raw[0] == 0x05:
        raw = b"\xe5" + raw[1:]
    base = raw[:8].decode("cp932", errors="replace").rstrip()
    extension = raw[8:11].decode("cp932", errors="replace").rstrip()
    return f"{base}.{extension}" if extension else base


class FatVolume:
    def __init__(self, image: bytes, *, offset: int) -> None:
        boot = image[offset : offset + 512]
        (
            self.bytes_per_sector,
            self.sectors_per_cluster,
            reserved_sectors,
            fat_count,
            root_entries,
            total_sectors,
            _media,
            sectors_per_fat,
        ) = struct.unpack_from("<HBHBHHBH", boot, 0x0B)
        if self.bytes_per_sector not in (256, 512, 1024, 2048) or fat_count == 0 or self.sectors_per_cluster == 0:
            raise DiskFormatError(f"no valid FAT boot sector at offset {offset:#x}")
        if total_sectors == 0:
            total_sectors = struct.unpack_from("<I", boot, 0x20)[0]
        self.image = image
        self.offset = offset
        fat_start = reserved_sectors
        root_start = fat_start + fat_count * sectors_per_fat
        root_sectors = (root_entries * 32 + self.bytes_per_sector - 1) // self.bytes_per_sector
        self.data_start = root_start + root_sectors
        self.cluster_size = self.bytes_per_sector * self.sectors_per_cluster
        cluster_count = (total_sectors - self.data_start) // self.sectors_per_cluster
        self.fat12 = cluster_count < 4085
        self.fat = self.sector_bytes(fat_start, count=sectors_per_fat)
        self.root = self.sector_bytes(root_start, count=root_sectors)

    def sector_bytes(self, sector: int, *, count: int) -> bytes:
        start = self.offset + sector * self.bytes_per_sector
        return self.image[start : start + count * self.bytes_per_sector]

    def next_cluster(self, cluster: int) -> int:
        if self.fat12:
            value = struct.unpack_from("<H", self.fat, cluster + cluster // 2)[0]
            return value >> 4 if cluster & 1 else value & 0x0FFF
        return struct.unpack_from("<H", self.fat, cluster * 2)[0]

    def is_end_of_chain(self, cluster: int) -> bool:
        return cluster < 2 or cluster >= (0xFF8 if self.fat12 else 0xFFF8)

    def read_chain(self, first_cluster: int) -> bytes:
        chunks = []
        cluster = first_cluster
        seen = set()
        while not self.is_end_of_chain(cluster):
            if cluster in seen:
                raise DiskFormatError(f"FAT loop at cluster {cluster}")
            seen.add(cluster)
            sector = self.data_start + (cluster - 2) * self.sectors_per_cluster
            chunks.append(self.sector_bytes(sector, count=self.sectors_per_cluster))
            cluster = self.next_cluster(cluster)
        return b"".join(chunks)

    def read_file(self, entry: DirEntry) -> bytes:
        return self.read_chain(entry.first_cluster)[: entry.size]

    def list_dir(self, entry: DirEntry | None = None) -> list[DirEntry]:
        table = self.root if entry is None else self.read_chain(entry.first_cluster)
        entries = []
        for position in range(0, len(table), 32):
            raw = table[position : position + 32]
            if raw[0] == 0x00:
                break
            if raw[0] == 0xE5 or raw[11] == 0x0F:
                continue
            name = decode_dos_name(raw[:11])
            if name in (".", ".."):
                continue
            time, date, first_cluster, size = struct.unpack_from("<HHHI", raw, 22)
            entries.append(
                DirEntry(
                    name=name,
                    attributes=raw[11],
                    first_cluster=first_cluster,
                    size=size,
                    modified=decode_dos_datetime(date, time),
                )
            )
        return entries

    def walk(self, entry: DirEntry | None = None, *, prefix: str = "") -> list[tuple[str, DirEntry]]:
        result = []
        for child in self.list_dir(entry):
            if child.is_volume_label:
                continue
            path = f"{prefix}{child.name}"
            result.append((path, child))
            if child.is_dir:
                result.extend(self.walk(child, prefix=f"{path}/"))
        return result
