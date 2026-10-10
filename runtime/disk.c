/* The original game files: a host folder, or the files of PC-98 disk images read into memory (port of
 * tools/pc98disk.py). Images: HDI (hard disk, first partition of the PC-98 partition table), D88 (floppy) and HDM
 * (raw floppy), all FAT12/16. From an image the folder with BOOT.COM is used, else the root folder (data disks); a
 * host folder without BOOT.COM but with images uses all of its images (a floppy set), and the first copy of a file
 * name is kept. */
#include "rt.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_GAME_FILES 1024
#define MAX_IMAGES 16
#define MAX_DIR_DEPTH 4
#define D88_TRACK_TABLE 0x20
#define D88_TRACK_COUNT 164
#define D88_SECTOR_HEADER_SIZE 0x10
#define D88_MAX_SECTORS 64
#define HDI_HEADER_FIELDS 0x1C
#define PARTITION_ENTRY_SIZE 32
#define PARTITION_TABLE_SIZE 512
#define DIR_ENTRY_SIZE 32
#define ATTR_LONG_NAME 0x0F
#define ATTR_VOLUME 0x08
#define ATTR_DIRECTORY 0x10
#define START_PROGRAM "BOOT.COM"

typedef struct {
    char name[13];
    uint8_t *data;
    uint32_t size;
} GameFile;

typedef struct {
    const uint8_t *image;
    long size;
    long offset;
    unsigned bytes_per_sector;
    unsigned sectors_per_cluster;
    long fat_start;
    long root_start;
    long root_sectors;
    long data_start;
    long cluster_count;
    int fat12;
} Volume;

static GameFile files[MAX_GAME_FILES];
static int file_count;
static int image_mode;
static char host_dir[512];
static char source[512];

static unsigned le16(const uint8_t *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static unsigned long le32(const uint8_t *p)
{
    return (unsigned long)le16(p) | (unsigned long)le16(p + 2) << 16;
}

static uint8_t *read_host_file(const char *path, long *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fseek(file, 0, SEEK_END);
    *size = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)*size + 1);
    if (!data || fread(data, 1, (size_t)*size, file) != (size_t)*size) {
        free(data);
        data = 0;
    }
    fclose(file);
    return data;
}

static int has_extension(const char *path, const char *extension)
{
    size_t length = strlen(path), wanted = strlen(extension);
    return length > wanted && strcasecmp(path + length - wanted, extension) == 0;
}

int disk_is_image(const char *path)
{
    return has_extension(path, ".hdi") || has_extension(path, ".d88") || has_extension(path, ".hdm");
}

/* D88: a table of track offsets; each track has sectors with a 16-byte header (record number at +2, data size at
 * +0x0E). The flat image has the sectors of each track in record order. */
static uint8_t *flatten_d88(const uint8_t *data, long size, long *flat_size)
{
    if (size < D88_TRACK_TABLE + D88_TRACK_COUNT * 4) return 0;
    uint8_t *flat = malloc((size_t)size);
    if (!flat) return 0;
    long length = 0;
    for (int track = 0; track < D88_TRACK_COUNT; track++) {
        unsigned long position = le32(data + D88_TRACK_TABLE + track * 4);
        if (!position) continue;
        if (position + D88_SECTOR_HEADER_SIZE > (unsigned long)size) goto bad;
        unsigned count = le16(data + position + 4);
        if (count > D88_MAX_SECTORS) goto bad;
        long starts[D88_MAX_SECTORS];
        unsigned records[D88_MAX_SECTORS], sizes[D88_MAX_SECTORS];
        for (unsigned i = 0; i < count; i++) {
            if (position + D88_SECTOR_HEADER_SIZE > (unsigned long)size) goto bad;
            records[i] = data[position + 2];
            sizes[i] = le16(data + position + 0x0E);
            starts[i] = (long)position + D88_SECTOR_HEADER_SIZE;
            position += D88_SECTOR_HEADER_SIZE + sizes[i];
            if (position > (unsigned long)size) goto bad;
        }
        for (unsigned record = 0; record < 256; record++) {
            for (unsigned i = 0; i < count; i++) {
                if (records[i] != record) continue;
                memcpy(flat + length, data + starts[i], sizes[i]);
                length += sizes[i];
            }
        }
    }
    *flat_size = length;
    return flat;
bad:
    free(flat);
    return 0;
}

/* HDI: a header (size at +8, data size at +0x0C, sector size, sectors per track, heads), then the disk; the PC-98
 * partition table is in sector 1. Returns the byte offset of the first partition in the disk, or -1. */
static long hdi_partition(const uint8_t *data, long size, const uint8_t **disk, long *disk_size)
{
    if (size < HDI_HEADER_FIELDS) return -1;
    unsigned long header_size = le32(data + 8), data_size = le32(data + 0x0C), sector_size = le32(data + 0x10);
    unsigned long sectors = le32(data + 0x14), heads = le32(data + 0x18);
    if (header_size > (unsigned long)size) return -1;
    if (data_size > (unsigned long)size - header_size) data_size = (unsigned long)size - header_size;
    *disk = data + header_size;
    *disk_size = (long)data_size;
    if (sector_size + PARTITION_TABLE_SIZE > data_size) return -1;
    const uint8_t *table = *disk + sector_size;
    for (int i = 0; i < PARTITION_TABLE_SIZE / PARTITION_ENTRY_SIZE; i++) {
        const uint8_t *entry = table + i * PARTITION_ENTRY_SIZE;
        if (!entry[0]) continue;
        unsigned long lba = ((unsigned long)le16(entry + 10) * heads + entry[9]) * sectors + entry[8];
        return (long)(lba * sector_size);
    }
    return -1;
}

static int volume_init(Volume *v, const uint8_t *image, long size, long offset)
{
    if (offset < 0 || offset + 512 > size) return 0;
    const uint8_t *boot = image + offset;
    v->image = image;
    v->size = size;
    v->offset = offset;
    v->bytes_per_sector = le16(boot + 0x0B);
    v->sectors_per_cluster = boot[0x0D];
    unsigned reserved = le16(boot + 0x0E), fat_count = boot[0x10], root_entries = le16(boot + 0x11);
    unsigned long total = le16(boot + 0x13), sectors_per_fat = le16(boot + 0x16);
    unsigned bps = v->bytes_per_sector;
    if ((bps != 256 && bps != 512 && bps != 1024 && bps != 2048) || !fat_count || !v->sectors_per_cluster) return 0;
    if (!total) total = le32(boot + 0x20);
    v->fat_start = reserved;
    v->root_start = v->fat_start + (long)(fat_count * sectors_per_fat);
    v->root_sectors = (long)((root_entries * DIR_ENTRY_SIZE + bps - 1) / bps);
    v->data_start = v->root_start + v->root_sectors;
    v->cluster_count = ((long)total - v->data_start) / v->sectors_per_cluster;
    v->fat12 = v->cluster_count < 4085;
    return v->cluster_count > 0;
}

static const uint8_t *sector_data(const Volume *v, long sector, long count)
{
    long start = v->offset + sector * (long)v->bytes_per_sector;
    if (sector < 0 || start + count * (long)v->bytes_per_sector > v->size) return 0;
    return v->image + start;
}

static long next_cluster(const Volume *v, long cluster)
{
    const uint8_t *fat = sector_data(v, v->fat_start, 0);
    long position = v->fat12 ? cluster + cluster / 2 : cluster * 2;
    if (!fat || v->offset + v->fat_start * (long)v->bytes_per_sector + position + 2 > v->size) return -1;
    unsigned value = le16(fat + position);
    if (!v->fat12) return value;
    return cluster & 1 ? value >> 4 : value & 0x0FFF;
}

static int end_of_chain(const Volume *v, long cluster)
{
    return cluster < 2 || cluster >= (v->fat12 ? 0xFF8 : 0xFFF8);
}

/* All clusters of a chain (malloc), or 0 for a broken chain. */
static uint8_t *read_chain(const Volume *v, long cluster, long *length)
{
    long cluster_size = (long)v->bytes_per_sector * v->sectors_per_cluster;
    uint8_t *data = 0;
    long count = 0;
    while (!end_of_chain(v, cluster)) {
        const uint8_t *source_data = sector_data(v, v->data_start + (cluster - 2) * v->sectors_per_cluster,
                                                 v->sectors_per_cluster);
        uint8_t *bigger = count <= v->cluster_count ? realloc(data, (size_t)((count + 1) * cluster_size)) : 0;
        if (!source_data || cluster - 2 >= v->cluster_count || !bigger) {
            free(bigger ? bigger : data);
            return 0;
        }
        data = bigger;
        memcpy(data + count * cluster_size, source_data, (size_t)cluster_size);
        count++;
        cluster = next_cluster(v, cluster);
    }
    *length = count * cluster_size;
    return data;
}

static void entry_name(const uint8_t *raw, char *out)
{
    int length = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) out[length++] = (char)(i == 0 && raw[i] == 0x05 ? 0xE5 : raw[i]);
    if (raw[8] != ' ') out[length++] = '.';
    for (int i = 8; i < 11 && raw[i] != ' '; i++) out[length++] = (char)raw[i];
    out[length] = 0;
}

static GameFile *find_file(const char *name)
{
    for (int i = 0; i < file_count; i++) {
        if (strcasecmp(files[i].name, name) == 0) return &files[i];
    }
    return 0;
}

static void add_files(const Volume *v, const uint8_t *table, long length)
{
    for (long position = 0; position + DIR_ENTRY_SIZE <= length; position += DIR_ENTRY_SIZE) {
        const uint8_t *raw = table + position;
        if (!raw[0]) break;
        if (raw[0] == 0xE5 || raw[11] == ATTR_LONG_NAME || raw[11] & (ATTR_VOLUME | ATTR_DIRECTORY)) continue;
        char name[13];
        entry_name(raw, name);
        if (find_file(name) || file_count >= MAX_GAME_FILES) continue;
        unsigned long size = le32(raw + 28);
        long chain_length = 0;
        uint8_t *data = size ? read_chain(v, le16(raw + 26), &chain_length) : calloc(1, 1);
        if (!data || (unsigned long)chain_length < size) {
            rt_log("disk: %s: broken FAT chain, file skipped", name);
            free(data);
            continue;
        }
        GameFile *file = &files[file_count++];
        snprintf(file->name, sizeof(file->name), "%s", name);
        file->data = data;
        file->size = (uint32_t)size;
    }
}

static int has_entry(const uint8_t *table, long length, const char *wanted)
{
    for (long position = 0; position + DIR_ENTRY_SIZE <= length; position += DIR_ENTRY_SIZE) {
        const uint8_t *raw = table + position;
        if (!raw[0]) break;
        if (raw[0] == 0xE5 || raw[11] == ATTR_LONG_NAME || raw[11] & (ATTR_VOLUME | ATTR_DIRECTORY)) continue;
        char name[13];
        entry_name(raw, name);
        if (strcasecmp(name, wanted) == 0) return 1;
    }
    return 0;
}

/* Add the files of the first folder (root first, then subfolders) that holds the start program. */
static int add_program_folder(const Volume *v, const uint8_t *table, long length, int depth)
{
    if (has_entry(table, length, START_PROGRAM)) {
        add_files(v, table, length);
        return 1;
    }
    if (depth >= MAX_DIR_DEPTH) return 0;
    for (long position = 0; position + DIR_ENTRY_SIZE <= length; position += DIR_ENTRY_SIZE) {
        const uint8_t *raw = table + position;
        if (!raw[0]) break;
        if (raw[0] == 0xE5 || raw[0] == '.' || raw[11] == ATTR_LONG_NAME || !(raw[11] & ATTR_DIRECTORY)) continue;
        long sub_length = 0;
        uint8_t *sub = read_chain(v, le16(raw + 26), &sub_length);
        int found = sub && add_program_folder(v, sub, sub_length, depth + 1);
        free(sub);
        if (found) return 1;
    }
    return 0;
}

static void add_image(const char *path)
{
    long size = 0;
    uint8_t *data = read_host_file(path, &size);
    if (!data) rt_fatal("cannot read %s", path);
    const uint8_t *image = data;
    long image_size = size, offset = 0;
    uint8_t *flat = 0;
    if (has_extension(path, ".d88")) {
        flat = flatten_d88(data, size, &image_size);
        if (!flat) rt_fatal("%s: not a valid D88 floppy image", path);
        image = flat;
    } else if (has_extension(path, ".hdi")) {
        offset = hdi_partition(data, size, &image, &image_size);
        if (offset < 0) rt_fatal("%s: no PC-98 partition table in this HDI image", path);
    }
    Volume v;
    if (!volume_init(&v, image, image_size, offset)) rt_fatal("%s: no FAT file system", path);
    const uint8_t *root = sector_data(&v, v.root_start, v.root_sectors);
    if (!root) rt_fatal("%s: the root folder is outside the image", path);
    int before = file_count;
    if (!add_program_folder(&v, root, v.root_sectors * (long)v.bytes_per_sector, 0)) {
        add_files(&v, root, v.root_sectors * (long)v.bytes_per_sector);
    }
    rt_log("disk: %s: %d files", path, file_count - before);
    free(flat);
    free(data);
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The images in a host folder, sorted by name; returns their count. */
static int folder_images(const char *dir, char **paths)
{
    DIR *listing = opendir(dir);
    if (!listing) return 0;
    int count = 0;
    struct dirent *item;
    while ((item = readdir(listing)) && count < MAX_IMAGES) {
        if (!disk_is_image(item->d_name)) continue;
        size_t size = strlen(dir) + strlen(item->d_name) + 2;
        paths[count] = malloc(size);
        if (paths[count]) snprintf(paths[count++], size, "%s/%s", dir, item->d_name);
    }
    closedir(listing);
    qsort(paths, (size_t)count, sizeof(*paths), compare_names);
    return count;
}

/* name (any case) in a host folder: 1 and the path in out. */
int disk_host_file(const char *dir, const char *name, char *out, size_t size)
{
    DIR *listing = opendir(dir);
    int found = 0;
    if (!listing) return 0;
    struct dirent *item;
    while (!found && (item = readdir(listing))) {
        if (strcasecmp(item->d_name, name) != 0) continue;
        snprintf(out, size, "%s/%s", dir, item->d_name);
        found = 1;
    }
    closedir(listing);
    return found;
}

void disk_use_folder(const char *dir)
{
    image_mode = 0;
    snprintf(host_dir, sizeof(host_dir), "%s", dir);
    snprintf(source, sizeof(source), "%s", dir);
}

/* The game in a host folder (for a start without a game argument): the folder when it has the game files or only
 * floppy images, else its one hard disk image. 0 when the folder has no game; fatal when it has several .hdi. */
int disk_find_game(const char *dir, char *out, size_t size)
{
    char boot[512];
    if (disk_host_file(dir, START_PROGRAM, boot, sizeof(boot))) {
        snprintf(out, size, "%s", dir);
        return 1;
    }
    char *paths[MAX_IMAGES];
    int count = folder_images(dir, paths);
    int hard_disks = 0;
    char names[512] = "";
    for (int i = 0; i < count; i++) {
        if (!has_extension(paths[i], ".hdi")) continue;
        if (!hard_disks++) snprintf(out, size, "%s", paths[i]);
        const char *slash = strrchr(paths[i], '/');
        size_t used = strlen(names);
        snprintf(names + used, sizeof(names) - used, "\n%s", slash ? slash + 1 : paths[i]);
    }
    for (int i = 0; i < count; i++) free(paths[i]);
    if (hard_disks > 1) {
        rt_fatal("%s has several disk images:%s\n\nKeep one, or start the program with the image as argument "
                 "(drag the image onto the program).",
                 dir, names);
    }
    if (hard_disks == 0 && count > 0) snprintf(out, size, "%s", dir);
    return count > 0;
}

int disk_open_game(const char *path)
{
    struct stat info;
    if (stat(path, &info) != 0) rt_fatal("cannot open %s", path);
    char boot[512];
    if (S_ISDIR(info.st_mode) && disk_host_file(path, START_PROGRAM, boot, sizeof(boot))) {
        disk_use_folder(path);
        return 1;
    }
    if (S_ISDIR(info.st_mode)) {
        char *paths[MAX_IMAGES];
        int count = folder_images(path, paths);
        if (!count) rt_fatal("%s: no %s and no disk images (.hdi, .d88, .hdm) in this folder", path, START_PROGRAM);
        for (int i = 0; i < count; i++) {
            add_image(paths[i]);
            free(paths[i]);
        }
    } else if (disk_is_image(path)) {
        add_image(path);
    } else {
        return 0;
    }
    if (!find_file(START_PROGRAM)) rt_fatal("%s: %s not found in the disk images", path, START_PROGRAM);
    image_mode = 1;
    snprintf(source, sizeof(source), "%s", path);
    return 1;
}

const char *disk_source(void)
{
    return source;
}

/* The host folder of the game: the game folder, the floppy folder or the folder of the image. */
void disk_host_folder(char *out, size_t size)
{
    struct stat info;
    if (!image_mode || (stat(source, &info) == 0 && S_ISDIR(info.st_mode))) {
        snprintf(out, size, "%s", image_mode ? source : host_dir);
        return;
    }
    const char *slash = strrchr(source, '/');
    snprintf(out, size, "%.*s", slash ? (int)(slash - source) : 1, slash ? source : ".");
}

/* An original game file for reading (0 when it does not exist): a host file in folder mode, else the data of the
 * image in memory. */
int disk_open_file(const char *name, DiskFile *out)
{
    *out = (DiskFile){0, 0, 0};
    if (!image_mode) {
        char path[512];
        out->file = disk_host_file(host_dir, name, path, sizeof(path)) ? fopen(path, "rb") : 0;
        return out->file != 0;
    }
    GameFile *file = find_file(name);
    if (!file) return 0;
    out->data = file->data;
    out->size = (long)file->size;
    return 1;
}
