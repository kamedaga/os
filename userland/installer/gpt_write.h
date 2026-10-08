/* SPDX-License-Identifier: MIT */
#ifndef PACHA_INSTALLER_GPT_WRITE_H
#define PACHA_INSTALLER_GPT_WRITE_H

#include "plan.h"

typedef int (*installer_write_at)(void *context, uint64_t offset,
    const void *buffer, size_t bytes);
typedef int (*installer_flush)(void *context);
typedef void (*installer_gpt_progress)(void *context, unsigned percent,
    const char *phase);

struct installer_new_guids {
    uint8_t disk[16];
    uint8_t esp[16];
    uint8_t root[16];
};

/* The caller must supply a freshly checked diskseq/size and a write grant
 * scoped to that exact whole disk. blank_verified means a separate complete
 * blank-media check has succeeded; malformed GPT is never blank by itself.
 * A fresh GPT read, plan comparison and media-marker check precede writes.
 * The backup and primary tables are flushed and then read back through the
 * same block path. This function never formats a filesystem. */
int installer_write_guided_gpt(const struct installer_plan *plan,
    uint64_t diskseq, uint64_t disk_bytes, uint32_t logical_block,
    int blank_verified, const struct installer_new_guids *new_guids,
    installer_read_at read_at, installer_write_at write_at,
    installer_flush flush, void *context,
    installer_gpt_progress progress, void *progress_context);

#endif
