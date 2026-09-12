/* SPDX-License-Identifier: MIT */
/* Test-launcher packaging. The adapter's image loader receives only a bounded ELF. */
#include "host.h"
#include "bootfs.h"
#include "../seed0boot/src/bootstrap_abi.h"

/* Bootfs wire offsets, decoded explicitly rather than cast to native structs. */
enum {
    BOOTFS_MAGIC = 0x53465442,
    BOOTFS_VERSION = 1,
    BOOTFS_HEADER_SIZE = 104,
    BOOTFS_ENTRY_SIZE = 48,
    BOOTFS_FILE_KIND = 1,
    HEADER_MAGIC = 0,
    HEADER_VERSION = 4,
    HEADER_LENGTH = 8,
    HEADER_ENTRY_COUNT = 16,
    HEADER_TABLE_SIZE = 20,
    HEADER_TABLE_OFFSET = 24,
    HEADER_STRINGS_OFFSET = 32,
    HEADER_STRINGS_SIZE = 40,
    ENTRY_NAME_OFFSET = 0,
    ENTRY_NAME_SIZE = 4,
    ENTRY_KIND = 6,
    ENTRY_DATA_OFFSET = 8,
    ENTRY_DATA_SIZE = 16,
};

struct boot_archive {
    const unsigned char *bytes;
    uint64_t length;
    uint64_t entries_offset;
    uint64_t strings_offset;
    uint64_t strings_size;
    uint32_t entry_count;
};

/* The native launcher is x86-64 little-endian; memcpy permits unaligned fields. */
static uint16_t read_u16(const unsigned char *bytes) {
    uint16_t value;

    memcpy(&value, bytes, sizeof(value));
    return value;
}

static uint32_t read_u32(const unsigned char *bytes) {
    uint32_t value;

    memcpy(&value, bytes, sizeof(value));
    return value;
}

static uint64_t read_u64(const unsigned char *bytes) {
    uint64_t value;

    memcpy(&value, bytes, sizeof(value));
    return value;
}

static struct boot_archive open_boot_archive(void) {
    const struct seed0_init_descriptor_page *descriptor = seed0_bootstrap_descriptor();

    PH_CHECK(descriptor != NULL && descriptor->bootfs_archive.image_va != 0);
    PH_CHECK(descriptor->bootfs_archive.size_bytes >= BOOTFS_HEADER_SIZE);
    const unsigned char *bytes = (void *)(uintptr_t)descriptor->bootfs_archive.image_va;

    PH_CHECK(read_u32(bytes + HEADER_MAGIC) == BOOTFS_MAGIC);
    PH_CHECK(read_u16(bytes + HEADER_VERSION) == BOOTFS_VERSION);
    struct boot_archive archive = {
        .bytes = bytes,
        .length = read_u64(bytes + HEADER_LENGTH),
        .entries_offset = read_u64(bytes + HEADER_TABLE_OFFSET),
        .strings_offset = read_u64(bytes + HEADER_STRINGS_OFFSET),
        .strings_size = read_u64(bytes + HEADER_STRINGS_SIZE),
        .entry_count = read_u32(bytes + HEADER_ENTRY_COUNT),
    };
    uint32_t table_size = read_u32(bytes + HEADER_TABLE_SIZE);

    PH_CHECK(archive.length <= descriptor->bootfs_archive.size_bytes);
    PH_CHECK(archive.entries_offset <= archive.length);
    PH_CHECK((uint64_t)archive.entry_count * BOOTFS_ENTRY_SIZE == table_size);
    PH_CHECK(table_size <= archive.length - archive.entries_offset);
    PH_CHECK(archive.strings_offset <= archive.length);
    PH_CHECK(archive.strings_size <= archive.length - archive.strings_offset);
    return archive;
}

const void *ph_bootfs_file(const char *path, size_t *size_out) {
    size_t path_size = strlen(path);
    struct boot_archive archive = open_boot_archive();

    for (uint32_t i = 0; i < archive.entry_count; ++i) {
        const unsigned char *entry = archive.bytes + archive.entries_offset +
            (size_t)i * BOOTFS_ENTRY_SIZE;
        uint32_t name_offset = read_u32(entry + ENTRY_NAME_OFFSET);
        uint16_t name_size = read_u16(entry + ENTRY_NAME_SIZE);
        uint64_t data_offset = read_u64(entry + ENTRY_DATA_OFFSET);
        uint64_t data_size = read_u64(entry + ENTRY_DATA_SIZE);

        PH_CHECK(name_offset <= archive.strings_size);
        PH_CHECK(name_size <= archive.strings_size - name_offset);
        PH_CHECK(data_offset <= archive.length);
        PH_CHECK(data_size <= archive.length - data_offset);

        const char *name = (const char *)archive.bytes + archive.strings_offset + name_offset;
        if (entry[ENTRY_KIND] == BOOTFS_FILE_KIND && name_size == path_size &&
            memcmp(name, path, name_size) == 0) {
            *size_out = data_size;
            return archive.bytes + data_offset;
        }
    }
    ph_fail(__FILE__, __LINE__, 0);
}

const void *ph_bootfs_core(size_t *size_out) {
    return ph_bootfs_file("/srv/kobox2/core.so", size_out);
}
