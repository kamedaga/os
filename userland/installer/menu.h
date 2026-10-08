/* SPDX-License-Identifier: MIT */
#ifndef PACHA_INSTALLER_MENU_H
#define PACHA_INSTALLER_MENU_H

#include "plan.h"
#include <stdio.h>

struct installer_disk {
    const char *name;
    const char *model;
    /* A diskseq is local to one Linux sandbox. These values together identify
     * the controller generation and namespace selected by the user. */
    uint64_t resource_id;
    uint64_t generation;
    uint64_t bytes;
    uint64_t diskseq;
    uint32_t major;
    uint32_t minor;
    uint32_t logical_block;
    /* NULL is accepted only when discovery has independently verified that
     * the disk is blank. A damaged GPT or I/O error is not a blank disk. */
    const struct installer_gpt *gpt;
    int blank_verified;
    installer_read_at read_at;
    void *read_context;
};

/* Progress is monotonic in whole percent.  The executor reports completed
 * policy/tool stages; the UI alone decides how that state is rendered. */
typedef void (*installer_progress)(void *context, unsigned percent,
    const char *phase);
typedef int (*installer_execute)(void *context, size_t target_index,
    const struct installer_plan *plan, installer_progress progress,
    void *progress_context);
typedef int (*installer_read_key)(void *context, uint64_t timeout_ns);

enum { INSTALLER_UI_SHELL = 0, INSTALLER_UI_DONE = 1 };

int installer_ui_run(installer_read_key read_key, void *input_context,
    FILE *output,
    const struct installer_disk *disks, size_t count, size_t media_index,
    unsigned rows, unsigned columns,
    installer_execute execute, void *context);

void installer_ui_message(FILE *output, unsigned rows, unsigned columns,
    const char *title, const char *message, int error);
void installer_ui_leave_for_shell(FILE *output);

#endif
