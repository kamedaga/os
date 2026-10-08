/* SPDX-License-Identifier: MIT */
#ifndef PACHA_INSTALLER_EXECUTE_H
#define PACHA_INSTALLER_EXECUTE_H

#include "catalog.h"
#include "gpt_write.h"
#include "menu.h"

/* Filesystem creation and deployment are supplied by the packaged,
 * unmodified formatter/archiver tools. The executor never implements a
 * second FAT or ext4 formatter. */
struct installer_tool_ops {
    int (*preflight)(void *context, installer_progress progress,
        void *progress_context);
    /* A live installer may delegate the complete, already-authorized
     * filesystem/deployment transaction to one Linux tool runner.  This is
     * mutually exclusive with the four fine-grained callbacks below; it
     * keeps formatter process management out of the native disk policy. */
    int (*run_plan)(void *context, struct installer_block_target *target,
        const struct installer_plan *plan, installer_progress progress,
        void *progress_context);
    int (*format_fat32)(void *context, struct installer_block_target *target,
        struct installer_extent partition, uint32_t logical_block);
    int (*format_ext4)(void *context, struct installer_block_target *target,
        struct installer_extent partition, uint32_t logical_block);
    int (*deploy_os)(void *context, struct installer_block_target *target,
        const struct installer_plan *plan);
    int (*verify)(void *context, struct installer_block_target *target,
        const struct installer_plan *plan);
    void *context;
};

struct installer_execution {
    struct installer_catalog_entry *entries;
    size_t count;
    size_t media_index;
    struct installer_tool_ops tools;
};

int installer_execute_plan(void *context, size_t target_index,
    const struct installer_plan *plan, installer_progress progress,
    void *progress_context);

#endif
