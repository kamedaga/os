/* SPDX-License-Identifier: MIT */
#ifndef PACHA_INSTALLER_GPT_H
#define PACHA_INSTALLER_GPT_H

#include <stddef.h>
#include <stdint.h>

#define PACHA_INSTALLER_MAX_PARTITIONS 128u

extern const uint8_t installer_esp_type_guid[16];
extern const uint8_t installer_linux_fs_type_guid[16];

struct installer_partition {
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    char name[73];
};

struct installer_gpt {
    uint8_t disk_guid[16];
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    size_t partition_count;
    struct installer_partition partitions[PACHA_INSTALLER_MAX_PARTITIONS];
    int install_media;
};

typedef int (*installer_read_at)(void *context, uint64_t offset,
    void *buffer, size_t bytes);

/* A missing or invalid GPT is not proof that a disk is safe to erase. The
 * caller must identify exactly one install-media marker before offering any
 * target, and re-read the selected disk identity before enabling writes. */
int installer_read_gpt(installer_read_at read_at, void *context,
    uint64_t disk_bytes, uint32_t logical_block, struct installer_gpt *out);
/* A missing GPT is writable only when both conventional partition-table
 * locations are zero. A damaged GPT is never silently treated as blank. */
int installer_verify_blank(installer_read_at read_at, void *context,
    uint64_t disk_bytes, uint32_t logical_block);
int installer_find_media(const struct installer_gpt *tables,
    size_t count, size_t *media_index);
int installer_guid_equal(const uint8_t a[16], const uint8_t b[16]);
void installer_guid_text(const uint8_t guid[16], char out[37]);

#endif
