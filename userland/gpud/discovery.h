#include <pacha/abi.h>
/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum gpud_discovery_kind { GPUD_DISC_DIR, GPUD_DISC_TEXT,
    GPUD_DISC_LINK, GPUD_DISC_NODE };
struct gpud_discovery_entry {
    enum gpud_discovery_kind kind;
    char path[160], value[160];
    uint64_t dev;
};
struct gpud_discovery {
    struct gpud_discovery_entry entries[32];
    size_t count, published, ancestor_count;
    uint32_t owned;
};
/* Snapshot only the granted PCI function. No driver/model selection. */
int gpud_discovery_prepare(struct gpud_discovery *, pacha_fd_t device_fd, int primary);
int gpud_discovery_publish(struct gpud_discovery *, pacha_fd_t filed_fd);
int gpud_discovery_remove(struct gpud_discovery *, pacha_fd_t filed_fd);
