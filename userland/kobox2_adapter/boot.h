/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_BOOT_H
#define PACHA_KOBOX_BOOT_H

#include <stddef.h>

struct kobox_linux_boot_layout;
struct kobox_linux_task_report;

/* One fixed core per native sandbox process. main runs in upstream PID 1
 * after regular boot, with the adapter's CPU/TLS/notification ports active.
 * Verified image bytes remain borrowed until process exit. No Gate required. */
_Noreturn void ph_boot_run(const void *core, size_t size,
    void (*prepare)(const struct kobox_linux_boot_layout *, void *),
    void (*main)(const struct kobox_linux_task_report *, void *), void *context);

#endif
