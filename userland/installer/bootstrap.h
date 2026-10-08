/* SPDX-License-Identifier: MIT */
#ifndef PACHA_INSTALLER_BOOTSTRAP_H
#define PACHA_INSTALLER_BOOTSTRAP_H

#include <stdint.h>

enum {
    PACHA_INSTALLER_BOOT_MAGIC = 0x31544f4f424e4950ull,
    PACHA_INSTALLER_MAX_SOURCES = 8,

    PACHA_INSTALLER_TERMD_FD = 220,
    PACHA_INSTALLER_READY_FD = 221,
    PACHA_INSTALLER_CONTROL_FD = 222,
    PACHA_INSTALLER_TARGET_PORT_FD = 223,
    PACHA_INSTALLER_MEDIA_PORT_FD = 224,
    PACHA_INSTALLER_SOURCE_FD_BASE = 225,

    PACHA_INSTALLER_MSG_PLAN = 0x4e414c50534e4950ull,
    PACHA_INSTALLER_MSG_RUNNER_DONE = 0x454e4f44524e4950ull,
    PACHA_INSTALLER_MSG_COMPLETE = 0x4554454c504d4f43ull,
    PACHA_INSTALLER_RESULT_SHELL = 1,
    PACHA_INSTALLER_RESULT_INSTALLED = 2,
};

struct pacha_installer_source_boot {
    uint64_t fd;
    uint64_t resource_id;
    uint32_t kind;
    uint32_t reserved;
};

struct pacha_installer_bootstrap {
    uint64_t magic;
    uint32_t size;
    uint32_t source_count;
    uint32_t pts_index;
    uint32_t reserved;
    uint64_t termd_fd;
    uint64_t ready_fd;
    uint64_t control_fd;
    uint64_t target_port_fd;
    uint64_t media_port_fd;
    struct pacha_installer_source_boot sources[PACHA_INSTALLER_MAX_SOURCES];
};

/* Immutable request handed to the launch broker after the native installer
 * has revalidated and armed one target.  Offsets are bytes, lengths are
 * bounded by their respective selected disks, and no raw host path exists. */
struct pacha_installer_runner_plan {
    uint64_t magic;
    uint32_t size;
    uint32_t flags;
    uint64_t target_bytes;
    uint64_t esp_offset;
    uint64_t esp_bytes;
    uint64_t root_offset;
    uint64_t root_bytes;
    uint64_t media_bytes;
    uint64_t media_esp_offset;
    uint64_t media_data_offset;
};

enum {
    PACHA_INSTALLER_RUNNER_FORMAT_ESP = 1u << 0,
    PACHA_INSTALLER_RUNNER_FORMAT_ROOT = 1u << 1,
    PACHA_INSTALLER_RUNNER_IMPORT_LIVE = 1u << 2,
    PACHA_INSTALLER_RUNNER_PREFLIGHT = 1u << 3,
};

/* Read-only preparation is a different authority class from installation,
 * so it has an explicit flag rather than borrowing an import request. */
static inline int pacha_installer_runner_flags_valid(uint32_t flags) {
    const uint32_t formats = PACHA_INSTALLER_RUNNER_FORMAT_ESP |
        PACHA_INSTALLER_RUNNER_FORMAT_ROOT;
    return flags == PACHA_INSTALLER_RUNNER_IMPORT_LIVE ||
        flags == PACHA_INSTALLER_RUNNER_PREFLIGHT ||
        (flags && !(flags & ~formats));
}

static inline int pacha_installer_runner_read_only(uint32_t flags) {
    return flags == PACHA_INSTALLER_RUNNER_IMPORT_LIVE ||
        flags == PACHA_INSTALLER_RUNNER_PREFLIGHT;
}

_Static_assert(sizeof(struct pacha_installer_runner_plan) <= 4096,
    "installer runner plan fits one sealed transfer page");

#endif
