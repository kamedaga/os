/* SPDX-License-Identifier: MIT */
#ifndef PACHA_INSTALLER_CATALOG_H
#define PACHA_INSTALLER_CATALOG_H

#include "block_transport.h"
#include "menu.h"

struct installer_catalog_entry {
    struct installer_disk display;
    struct installer_block_target target;
    struct installer_gpt gpt;
    char name[sizeof(((kb2_block_disk_t *)0)->name)];
    int gpt_status;
};

/* Each entry keeps its source controller identity. In particular, two
 * sandboxes can both report diskseq=1 without becoming the same disk. */
int installer_catalog_scan(struct installer_block_source *sources,
    size_t source_count, struct installer_catalog_entry *entries,
    size_t capacity, size_t *count, size_t *media_index);

#endif
