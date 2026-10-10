/* Microsoft EXEPACK in the program loader: a packed EXE is unpacked to the MZ file the stub would restore in
 * memory (same algorithm as tools/exepack.py), so the image matches the recompiled module. */
#include "rt.h"

#include <string.h>

#define SIGNATURE_A 0x4252
#define RELOCATION_SECTIONS 16
#define HEADER_WORDS 14

static const char corrupt_message[] = "Packed file is corrupt";

static uint16_t word_at(const uint8_t *data, size_t offset)
{
    return (uint16_t)(data[offset] | (data[offset + 1] << 8));
}

static void put_word(uint8_t *data, size_t offset, uint16_t value)
{
    data[offset] = (uint8_t)value;
    data[offset + 1] = (uint8_t)(value >> 8);
}

static const uint8_t *stub_of(const uint8_t *data, size_t size)
{
    size_t start = (size_t)word_at(data, 8) * 16 + (size_t)word_at(data, 0x16) * 16;
    if (start + 18 > size) return 0;
    if (word_at(data, start + 16) == SIGNATURE_A || word_at(data, start + 14) == SIGNATURE_A) return data + start;
    return 0;
}

int exepack_detect(const uint8_t *data, size_t size)
{
    return size >= 0x1C && data[0] == 'M' && data[1] == 'Z' && stub_of(data, size) != 0;
}

size_t exepack_unpack(const uint8_t *data, size_t size, uint8_t *out, size_t capacity)
{
    const uint8_t *stub = stub_of(data, size);
    if (!stub) return 0;
    size_t image_start = (size_t)word_at(data, 8) * 16;
    size_t stub_offset = (size_t)word_at(data, 0x16) * 16;
    int long_header = word_at(stub, 16) == SIGNATURE_A;
    uint16_t real_ip = word_at(stub, 0), real_cs = word_at(stub, 2), exepack_size = word_at(stub, 6);
    uint16_t real_sp = word_at(stub, 8), real_ss = word_at(stub, 10), dest_len = word_at(stub, 12);
    uint16_t skip_len = long_header ? word_at(stub, 14) : 1;
    size_t stub_size = size - image_start - stub_offset;
    if (exepack_size < stub_size) stub_size = exepack_size;

    const uint8_t *message = 0;
    for (size_t i = 0; i + sizeof(corrupt_message) - 1 <= stub_size; i++) {
        if (memcmp(stub + i, corrupt_message, sizeof(corrupt_message) - 1) == 0) {
            message = stub + i + sizeof(corrupt_message) - 1;
            break;
        }
    }
    if (!message) return 0;
    uint32_t relocations[4096];
    size_t relocation_count = 0;
    size_t position = 0;
    for (int section = 0; section < RELOCATION_SECTIONS; section++) {
        uint16_t count = word_at(message, position);
        position += 2;
        for (uint16_t i = 0; i < count && relocation_count < 4096; i++, position += 2) {
            relocations[relocation_count++] = (uint32_t)(section * 0x1000) << 16 | word_at(message, position);
        }
    }

    size_t unpacked_size = (size_t)dest_len * 16;
    size_t header_size = (HEADER_WORDS * 2 + relocation_count * 4 + 15) / 16 * 16;
    if (header_size + unpacked_size > capacity) return 0;
    uint8_t *image = out + header_size;
    size_t compressed_size = stub_offset - (size_t)(skip_len - 1) * 16;
    memset(image, 0, unpacked_size);
    memcpy(image, data + image_start, compressed_size < unpacked_size ? compressed_size : unpacked_size);
    size_t src = compressed_size;
    for (int padding = 0; padding < 16 && src > 0 && image[src - 1] == 0xFF; padding++) src--;
    size_t dst = unpacked_size;
    for (;;) {
        if (src < 3) return 0;
        uint8_t command = image[--src];
        src -= 2;
        uint16_t length = word_at(image, src);
        if ((command & 0xFE) == 0xB0) {
            uint8_t fill = image[--src];
            if (length > dst) return 0;
            dst -= length;
            memset(image + dst, fill, length);
        } else if ((command & 0xFE) == 0xB2) {
            for (uint16_t i = 0; i < length; i++) image[--dst] = image[--src];
        } else {
            return 0;
        }
        if (dst < src) return 0;
        if (command & 1) break;
    }

    size_t file_size = header_size + unpacked_size;
    memset(out, 0, header_size);
    out[0] = 'M';
    out[1] = 'Z';
    put_word(out, 2, (uint16_t)(file_size % 512));
    put_word(out, 4, (uint16_t)((file_size + 511) / 512));
    put_word(out, 6, (uint16_t)relocation_count);
    put_word(out, 8, (uint16_t)(header_size / 16));
    put_word(out, 0x0A, word_at(data, 0x0A));
    put_word(out, 0x0C, word_at(data, 0x0C));
    put_word(out, 0x0E, real_ss);
    put_word(out, 0x10, real_sp);
    put_word(out, 0x14, real_ip);
    put_word(out, 0x16, real_cs);
    put_word(out, 0x18, HEADER_WORDS * 2);
    for (size_t i = 0; i < relocation_count; i++) {
        put_word(out, HEADER_WORDS * 2 + i * 4, (uint16_t)relocations[i]);
        put_word(out, HEADER_WORDS * 2 + i * 4 + 2, (uint16_t)(relocations[i] >> 16));
    }
    return file_size;
}
