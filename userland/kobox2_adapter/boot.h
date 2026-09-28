/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_BOOT_H
#define PACHA_KOBOX_BOOT_H

#include <stddef.h>

struct kobox_linux_boot_layout;
struct kobox_linux_task_report;

/* Optional SEND-only owner grant for selected Linux diagnostic lines. */
void ph_boot_set_log_fd(int fd);
/* Runtime faults cannot rely on the startup-only management receiver. */
void ph_boot_log_fault(unsigned long vector, unsigned long error,
    unsigned long core_relative, unsigned long ip, unsigned long address);
void ph_boot_log_fatal(unsigned long source, unsigned long line,
    unsigned long result);
/* Failure-path native mapping details for the existing RAM service log. */
void ph_boot_log_map_failure(unsigned long window, unsigned long offset,
    unsigned long backing, unsigned long bytes, unsigned long protection,
    unsigned long result);
void ph_boot_log_scatter_failure(unsigned long window, unsigned long offset,
    unsigned long first_page, unsigned long pages, unsigned long protection,
    unsigned long step, unsigned long result);
/* Preserve the failing native IRQ operation in the RAM log before rollback. */
void ph_boot_log_irq_failure(unsigned long mode, unsigned long index,
    unsigned long count, unsigned long cpu, unsigned long step,
    unsigned long raw, unsigned long result);
/* Native edges, Linux dispatches and handler ACKs are separate observations.
 * Emit the final snapshot only after the collector has stopped. */
void ph_boot_log_irq_summary(unsigned long received,
    unsigned long dispatched, unsigned long acknowledged);
void ph_boot_log_irq_ready(unsigned long received,
    unsigned long dispatched, unsigned long acknowledged);
/* Preserve the failing AMDGPU CPU-map stage without exposing Linux internals
 * to gpud or relying on a serial console during a physical probe. */
void ph_boot_log_gpu_map_failure(unsigned long stage, long status,
    unsigned long handle, unsigned long bytes);
/* A failed native page view otherwise loses the exact parent and page-list
 * shape before the Linux BO is released during error unwinding. */
void ph_boot_log_gpu_page_view_failure(long status, unsigned long parent_fd,
    unsigned long rights, unsigned long cache, unsigned long page_count,
    unsigned long first_page, unsigned long middle_page,
    unsigned long last_page);
/* Distinguish an out-of-BAR VRAM PFN from a native MMIO export rejection. */
void ph_boot_log_gpu_bar_export_failure(unsigned long reason, long status,
    unsigned long bar, unsigned long protection, unsigned long cache,
    unsigned long page_count, unsigned long first_page,
    unsigned long last_page);
/* Record both sides of a large native cache-policy syscall. A stalled GPU
 * allocation must be distinguishable from a stalled Linux BO constructor. */
void ph_boot_log_gpu_cache_transition(unsigned long stage,
    unsigned long offset, unsigned long bytes, unsigned long mode, long status);
/* A cache-policy rejection should identify an existing DMA pin in the same
 * userland RAM log. Scattered mappings remain a separate, uncertain count. */
void ph_boot_log_gpu_cache_reject(long status, unsigned long offset,
    unsigned long bytes, unsigned long direct_overlaps,
    unsigned long scattered_mappings, unsigned long sample_iova,
    long inventory_status);

/* One fixed core per native sandbox process. main runs in upstream PID 1
 * after regular boot, with the adapter's CPU/TLS/notification ports active.
 * Verified image bytes remain borrowed until process exit. No Gate required. */
_Noreturn void ph_boot_run(const void *core, size_t size,
    void (*prepare)(const struct kobox_linux_boot_layout *, void *),
    void (*main)(const struct kobox_linux_task_report *, void *), void *context);

#endif
