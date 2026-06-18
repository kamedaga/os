#include "bootfs_reader.h"

#include "bootstrap_abi.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    SEED0_BOOTFS_MAGIC = 0x53465442u,
    SEED0_BOOTFS_VERSION = 1,
    SEED0_BOOTFS_HEADER_BYTES = 104,
    SEED0_BOOTFS_ENTRY_BYTES = 48,
    SEED0_BOOTFS_KIND_REGULAR = 1,
};

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int range_fits(uint64_t total, uint64_t offset, uint64_t len)
{
    return offset <= total && len <= total - offset;
}

int seed0_bootfs_open_file(const char *path, const unsigned char **out_data, uint32_t *out_size)
{
    if (path == 0 || out_data == 0 || out_size == 0) {
        return -1;
    }
    *out_data = 0;
    *out_size = 0;

    const struct seed0_init_descriptor_page *desc = seed0_bootstrap_descriptor();
    if (desc == 0 ||
        (desc->bootfs_archive.flags & SEED0_INIT_DEVICE_FLAG_PRESENT) == 0 ||
        desc->bootfs_archive.image_va == 0 ||
        desc->bootfs_archive.size_bytes < SEED0_BOOTFS_HEADER_BYTES) {
        fprintf(stderr, "[seed0boot] bootfs: descriptor unavailable\n");
        return -2;
    }

    const unsigned char *image = (const unsigned char *)(uintptr_t)desc->bootfs_archive.image_va;
    const uint64_t image_size = desc->bootfs_archive.size_bytes;
    const uint32_t magic = rd32(image + 0);
    const uint16_t version = rd16(image + 4);
    const uint16_t header_bytes = rd16(image + 6);
    const uint64_t total_size = rd64(image + 8);
    const uint32_t entry_count = rd32(image + 16);
    const uint32_t entry_table_bytes = rd32(image + 20);
    const uint64_t entry_table_offset = rd64(image + 24);
    const uint64_t string_table_offset = rd64(image + 32);
    const uint64_t string_table_bytes = rd64(image + 40);
    if (magic != SEED0_BOOTFS_MAGIC ||
        version != SEED0_BOOTFS_VERSION ||
        header_bytes < SEED0_BOOTFS_HEADER_BYTES ||
        total_size > image_size ||
        entry_table_bytes != (uint64_t)entry_count * SEED0_BOOTFS_ENTRY_BYTES ||
        !range_fits(total_size, entry_table_offset, entry_table_bytes) ||
        !range_fits(total_size, string_table_offset, string_table_bytes)) {
        fprintf(stderr, "[seed0boot] bootfs: invalid image magic=0x%x version=%u entries=%u\n",
            magic,
            version,
            entry_count);
        return -3;
    }

    const size_t wanted_len = strlen(path);
    for (uint32_t i = 0; i < entry_count; i++) {
        const unsigned char *entry = image + entry_table_offset + (uint64_t)i * SEED0_BOOTFS_ENTRY_BYTES;
        const uint32_t path_offset = rd32(entry + 0);
        const uint16_t path_len = rd16(entry + 4);
        const uint8_t kind = entry[6];
        const uint64_t data_offset = rd64(entry + 8);
        const uint64_t data_size = rd64(entry + 16);
        if (kind != SEED0_BOOTFS_KIND_REGULAR ||
            !range_fits(string_table_bytes, path_offset, path_len) ||
            !range_fits(total_size, data_offset, data_size)) {
            continue;
        }
        const char *entry_path = (const char *)(const void *)(image + string_table_offset + path_offset);
        if (path_len == wanted_len && memcmp(entry_path, path, wanted_len) == 0) {
            if (data_size > UINT32_MAX) {
                return -4;
            }
            *out_data = image + data_offset;
            *out_size = (uint32_t)data_size;
            return 0;
        }
    }
    return -5;
}
