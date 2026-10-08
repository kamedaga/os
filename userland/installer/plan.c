/* SPDX-License-Identifier: MIT */
#include "plan.h"

#include <errno.h>
#include <string.h>

static int valid_disk(uint64_t bytes, uint32_t block) {
    return block >= 512 && block <= 4096 && !(block & (block - 1)) &&
        bytes >= UINT64_C(2) * 1024 * 1024 * 1024 && bytes % block == 0;
}

int installer_guided_plan(uint64_t diskseq, uint64_t disk_bytes,
    uint32_t logical_block, const uint8_t target_guid[16],
    struct installer_plan *out) {
    if (!out || !target_guid || !diskseq || !valid_disk(disk_bytes, logical_block))
        return -EINVAL;
    uint64_t sectors = disk_bytes / logical_block;
    uint64_t alignment = UINT64_C(1024) * 1024 / logical_block;
    uint64_t table_sectors = (UINT64_C(128) * 128 + logical_block - 1) /
        logical_block;
    uint64_t first_usable = 2 + table_sectors;
    uint64_t last_usable = sectors - 2 - table_sectors;
    uint64_t esp_first = (first_usable + alignment - 1) / alignment * alignment;
    uint64_t esp_end = esp_first + (UINT64_C(512) * 1024 * 1024 /
        logical_block);
    uint64_t root_first = (esp_end + alignment - 1) / alignment * alignment;
    uint64_t root_end = (last_usable + 1) / alignment * alignment;
    if (root_end <= root_first ||
        root_end - root_first < UINT64_C(1024) * 1024 * 1024 /
            logical_block) return -ENOSPC;
    *out = (struct installer_plan){.kind = INSTALLER_LAYOUT_GUIDED,
        .target_diskseq = diskseq, .target_bytes = disk_bytes,
        .logical_block = logical_block,
        .esp_filesystem = INSTALLER_FS_FAT32,
        .root_filesystem = INSTALLER_FS_EXT4,
        .esp = {esp_first, esp_end - 1},
        .root = {root_first, root_end - 1},
        .erase = {0, sectors - 1},
        .format_esp = 1, .format_root = 1};
    memcpy(out->target_guid, target_guid, 16);
    return 0;
}

int installer_manual_plan(uint64_t diskseq, uint64_t disk_bytes,
    uint32_t logical_block, const struct installer_gpt *gpt,
    size_t esp_index, size_t root_index, int format_esp,
    int format_root, struct installer_plan *out) {
    if (!out || !gpt || !diskseq || !valid_disk(disk_bytes, logical_block) ||
        esp_index >= gpt->partition_count ||
        root_index >= gpt->partition_count || esp_index == root_index ||
        (format_esp != 0 && format_esp != 1) ||
        (format_root != 0 && format_root != 1) || gpt->install_media)
        return -EINVAL;
    const struct installer_partition *esp = &gpt->partitions[esp_index];
    const struct installer_partition *root = &gpt->partitions[root_index];
    if (memcmp(esp->type_guid, installer_esp_type_guid, 16) ||
        memcmp(root->type_guid, installer_linux_fs_type_guid, 16) ||
        esp->first_lba < gpt->first_usable_lba ||
        root->first_lba < gpt->first_usable_lba ||
        esp->last_lba > gpt->last_usable_lba ||
        root->last_lba > gpt->last_usable_lba ||
        esp->last_lba < esp->first_lba ||
        root->last_lba < root->first_lba ||
        (esp->last_lba - esp->first_lba + 1) * logical_block <
            UINT64_C(64) * 1024 * 1024 ||
        (root->last_lba - root->first_lba + 1) * logical_block <
            UINT64_C(1024) * 1024 * 1024)
        return -EINVAL;
    *out = (struct installer_plan){.kind = INSTALLER_LAYOUT_MANUAL,
        .target_diskseq = diskseq, .target_bytes = disk_bytes,
        .logical_block = logical_block,
        .esp_filesystem = INSTALLER_FS_FAT32,
        .root_filesystem = INSTALLER_FS_EXT4,
        .esp = {esp->first_lba, esp->last_lba},
        .root = {root->first_lba, root->last_lba},
        .format_esp = format_esp, .format_root = format_root};
    memcpy(out->target_guid, gpt->disk_guid, 16);
    memcpy(out->esp_guid, esp->unique_guid, 16);
    memcpy(out->root_guid, root->unique_guid, 16);
    return 0;
}

int installer_plan_revalidate(const struct installer_plan *plan,
    uint64_t diskseq, uint64_t disk_bytes, uint32_t logical_block,
    const struct installer_gpt *current) {
    if (!plan || !diskseq || plan->target_diskseq != diskseq ||
        plan->target_bytes != disk_bytes || plan->logical_block != logical_block ||
        plan->esp_filesystem != INSTALLER_FS_FAT32 ||
        plan->root_filesystem != INSTALLER_FS_EXT4 ||
        (current && current->install_media) ||
        (current && !installer_guid_equal(plan->target_guid, current->disk_guid)) ||
        (!current && memcmp(plan->target_guid, (uint8_t[16]){0}, 16)))
        return -ESTALE;
    if (plan->kind == INSTALLER_LAYOUT_MANUAL) {
        if (!current) return -ESTALE;
        int found_esp = 0, found_root = 0;
        for (size_t i = 0; i < current->partition_count; ++i) {
            const struct installer_partition *p = &current->partitions[i];
            if (p->first_lba == plan->esp.first_lba &&
                p->last_lba == plan->esp.last_lba &&
                !memcmp(p->type_guid, installer_esp_type_guid, 16) &&
                installer_guid_equal(p->unique_guid, plan->esp_guid)) found_esp++;
            if (p->first_lba == plan->root.first_lba &&
                p->last_lba == plan->root.last_lba &&
                !memcmp(p->type_guid, installer_linux_fs_type_guid, 16) &&
                installer_guid_equal(p->unique_guid, plan->root_guid)) found_root++;
        }
        if (found_esp != 1 || found_root != 1) return -ESTALE;
    }
    return 0;
}
