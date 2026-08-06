#include "bootfs_reader.h"

#include "pacha/ipc.h"

#include <stdlib.h>
#include <string.h>

enum {
    BOOTFS_MAGIC = 0x53465442u,
    BOOTFS_VERSION = 1,
    BOOTFS_HEADER_BYTES = 104,
    BOOTFS_ENTRY_BYTES = 48,
    BOOTFS_KIND_REGULAR = 1,
};

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint64_t rd64(const unsigned char *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int range_fits(uint64_t total, uint64_t offset, uint64_t length)
{
    return offset <= total && length <= total - offset;
}

int storage_bootfs_read_file(
    int archive_fd,
    uint64_t archive_size,
    const char *path,
    unsigned char **out_data,
    uint64_t *out_size)
{
    if (archive_fd < 16 || archive_size < BOOTFS_HEADER_BYTES ||
        path == NULL || out_data == NULL || out_size == NULL)
        return -22;
    *out_data = NULL;
    *out_size = 0;
    const uint64_t map_size = (archive_size + 4095u) & ~4095ull;
    const unsigned char *image = pacha_mmap(
        archive_fd, map_size, PACHA_PROT_READ, PACHA_MMAP_SHARED, 0);
    if (image == NULL) return -5;

    const uint32_t magic = rd32(image);
    const uint16_t version = rd16(image + 4);
    const uint16_t header_bytes = rd16(image + 6);
    const uint64_t total_size = rd64(image + 8);
    const uint32_t entry_count = rd32(image + 16);
    const uint32_t entry_bytes = rd32(image + 20);
    const uint64_t entry_offset = rd64(image + 24);
    const uint64_t string_offset = rd64(image + 32);
    const uint64_t string_bytes = rd64(image + 40);
    if (magic != BOOTFS_MAGIC || version != BOOTFS_VERSION ||
        header_bytes < BOOTFS_HEADER_BYTES ||
        entry_bytes != (uint64_t)entry_count * BOOTFS_ENTRY_BYTES ||
        total_size > archive_size ||
        !range_fits(total_size, entry_offset, entry_bytes) ||
        !range_fits(total_size, string_offset, string_bytes)) {
        (void)pacha_munmap((void *)image, map_size);
        return -5;
    }

    int status = -2;
    for (uint32_t i = 0; i < entry_count; i++) {
        const unsigned char *entry = image + entry_offset +
            (uint64_t)i * BOOTFS_ENTRY_BYTES;
        const uint32_t name_offset = rd32(entry);
        const uint16_t name_length = rd16(entry + 4);
        const uint8_t kind = entry[6];
        const uint64_t data_offset = rd64(entry + 8);
        const uint64_t data_size = rd64(entry + 16);
        if (kind != BOOTFS_KIND_REGULAR ||
            !range_fits(string_bytes, name_offset, name_length) ||
            !range_fits(total_size, data_offset, data_size))
            continue;
        const size_t path_length = strlen(path);
        if (path_length != name_length ||
            memcmp(image + string_offset + name_offset, path, name_length) != 0)
            continue;
        unsigned char *copy = malloc((size_t)data_size);
        if (copy == NULL) {
            status = -12;
            break;
        }
        memcpy(copy, image + data_offset, (size_t)data_size);
        *out_data = copy;
        *out_size = data_size;
        status = 0;
        break;
    }
    (void)pacha_munmap((void *)image, map_size);
    return status;
}
