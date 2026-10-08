/* SPDX-License-Identifier: MIT */
#ifndef PACHA_INSTALLER_FAT32_PROBE_H
#define PACHA_INSTALLER_FAT32_PROBE_H

#include "gpt.h"

/* Read-only check before reusing an ESP and after installation. Checks BPB,
 * FSInfo and backup boot sector; it is not a substitute for fsck.fat. */
int installer_probe_fat32(installer_read_at read_at, void *context,
    uint64_t disk_bytes, uint32_t logical_block,
    uint64_t first_lba, uint64_t last_lba);

#endif
