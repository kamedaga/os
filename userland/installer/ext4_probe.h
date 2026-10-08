/* SPDX-License-Identifier: MIT */
#ifndef PACHA_INSTALLER_EXT4_PROBE_H
#define PACHA_INSTALLER_EXT4_PROBE_H

#include "gpt.h"

/* Read-only preflight for a manual root that will be reused without
 * formatting. This detects ext4, but does not replace e2fsck. */
int installer_probe_ext4(installer_read_at read_at, void *context,
    uint64_t disk_bytes, uint32_t logical_block,
    uint64_t first_lba, uint64_t last_lba);

#endif
