/* SPDX-License-Identifier: MIT */
#ifndef PACHA_INSTALLER_PLAN_H
#define PACHA_INSTALLER_PLAN_H

#include "gpt.h"

enum installer_layout_kind {
    INSTALLER_LAYOUT_GUIDED = 1,
    INSTALLER_LAYOUT_MANUAL = 2,
};

enum installer_filesystem {
    INSTALLER_FS_FAT32 = 1,
    INSTALLER_FS_EXT4 = 2,
};

struct installer_extent {
    uint64_t first_lba;
    uint64_t last_lba;
};

struct installer_plan {
    enum installer_layout_kind kind;
    uint8_t target_guid[16];
    uint8_t esp_guid[16];
    uint8_t root_guid[16];
    uint64_t target_diskseq;
    uint64_t target_bytes;
    uint32_t logical_block;
    enum installer_filesystem esp_filesystem;
    enum installer_filesystem root_filesystem;
    struct installer_extent esp;
    struct installer_extent root;
    /* A guided plan replaces the complete GPT and both filesystems. A
     * manual plan only operates on the two explicitly selected partitions. */
    struct installer_extent erase;
    int format_esp;
    int format_root;
};

int installer_guided_plan(uint64_t diskseq, uint64_t disk_bytes,
    uint32_t logical_block, const uint8_t target_guid[16],
    struct installer_plan *out);
int installer_manual_plan(uint64_t diskseq, uint64_t disk_bytes,
    uint32_t logical_block, const struct installer_gpt *gpt,
    size_t esp_index, size_t root_index, int format_esp,
    int format_root, struct installer_plan *out);
int installer_plan_revalidate(const struct installer_plan *plan,
    uint64_t diskseq, uint64_t disk_bytes, uint32_t logical_block,
    const struct installer_gpt *current);

#endif
