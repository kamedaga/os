/* SPDX-License-Identifier: MIT */
#ifndef PACHA_INSTALLER_PROGRESS_PROTOCOL_H
#define PACHA_INSTALLER_PROGRESS_PROTOCOL_H

#include <stdint.h>

#define INSTALLER_PROGRESS_MAGIC UINT64_C(0x3152474f52504e49)
/* Project-private Linux ioctl, carried only by the attenuated read-only
 * installer-media descriptor. */
#define INSTALLER_PROGRESS_IOCTL UINT64_C(0x402070f0)

enum installer_progress_phase {
    INSTALLER_PROGRESS_PREFLIGHT_FILES = 1,
    INSTALLER_PROGRESS_PREFLIGHT_ROOTFS,
    INSTALLER_PROGRESS_RECHECK_FILES,
    INSTALLER_PROGRESS_RECHECK_ROOTFS,
    INSTALLER_PROGRESS_EXTRACT_ROOTFS,
    INSTALLER_PROGRESS_FORMAT_EXT4,
    INSTALLER_PROGRESS_VERIFY_EXT4,
    INSTALLER_PROGRESS_FORMAT_ESP,
    INSTALLER_PROGRESS_DEPLOY_UEFI,
    INSTALLER_PROGRESS_VERIFY_UEFI,
    INSTALLER_PROGRESS_COMPLETE,
};

struct installer_progress_wire {
    uint64_t magic;
    uint64_t completed_bytes;
    uint64_t total_bytes;
    uint32_t phase;
    uint32_t reserved;
};

_Static_assert(sizeof(struct installer_progress_wire) == 32,
    "installer progress wire is fixed-size");

#endif
