/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_HOST_H
#define PACHA_KOBOX_HOST_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <pacha/ipc.h>
#include <pacha/syscall.h>
#include "boot/core.h"
#include "boot/fixed_image.h"
#include "boot/image_layout.h"
#include "machine/domain.h"

/* Internal PachaOS host (ph_) API. One sandbox process owns one Linux core. */
#define PH_PAGE_SIZE 4096ul
#define PH_CPU_COUNT KOBOX_LINUX_MEMORY_LOGICAL_CPUS
#define PH_NOTIFICATION_SIGNAL 12u
#define PH_NOTIFICATION_MASK (UINT64_C(1) << (PH_NOTIFICATION_SIGNAL - 1))
#define PH_THREAD_STACK_SIZE (256ul * 1024)
/* At 256 MiB, real multi-tab 2560x1264 rendering reached the DRM allocation
 * reserve and rejected even one-page resources. Add 128 MiB of hosted-core
 * headroom without changing the VM's 4 GiB guest RAM configuration. */
#define PH_RAM_SIZE (384ul << 20)
#define PH_IMAGE_PHYSICAL_BASE ((size_t)KOBOX_CORE_PHYSICAL_BASE)

struct ph_window {
    void *base;
    size_t size;
    /* One byte per page plus an end sentinel: protection bits and VMA starts.
     * Mapping/protection changes and this metadata share the same lock. */
    unsigned char *pages;
    atomic_uint lock;
};

struct ph_image {
    struct kobox_boot_core core;
    struct kobox_fixed_image fixed;
    void *base;              /* Executable image view into the RAM VMO. */
    void *direct;            /* Direct-map view of the entire RAM VMO. */
    size_t size;
    int ram_fd;
    struct ph_window window;
};

struct ph_task {
    struct ph_task *self; /* Native FS:0, independent of hosted Linux TLS. */
    int fd;
    /* Borrowed boot thread; not joinable by the adapter. */
    bool bound;
    /* Protect host operations from notification-driven Linux entry.
     * Native delivery stays enabled; Linux's logical IRQ depth is separate. */
    atomic_ullong notification_mask;
    atomic_uint notification_deferred;
    uint64_t saved_notification_mask; /* Mask before the most recent cpu_enter. */
    int logical_cpu;                  /* Owned CPU, or -1 outside Linux. */
    /* Counting wake permits preserve wake-before-park. */
    atomic_uint permit;
    void *(*entry)(void *);
    void *argument;
    void *stack;
    void *fault_stack;
    /* The adapter keeps its native ABI state in FS. Hosted Linux machine
     * state is explicit, so this object is portable to hosts using libc TLS. */
    struct kobox_runtime_thread_state runtime;
};

static inline struct ph_task *ph_current_task(void) {
    struct ph_task *task;

    __asm__ volatile("mov %%fs:0, %0" : "=r"(task));
    return task;
}

/* Freestanding support. Allocation and unexpected syscall failures are fatal. */
void ph_log(const char *text);
void ph_number(const char *name, uint64_t value);
_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result);
void ph_set_failure_reporter(void (*reporter)(const char *, unsigned, uint64_t));
void ph_set_exception_reporter(void (*reporter)(const struct pacha_native_fault_frame *));
void ph_report_exception(const struct pacha_native_fault_frame *frame);

#define PH_CHECK(condition) do { \
    if (!(condition)) { \
        ph_fail(__FILE__, __LINE__, 0); \
    } \
} while (0)

/* For zero-success operations only, not syscalls returning counts or FDs. */
#define PH_OK(expression) do { \
    long ph_result_ = (expression); \
    if (ph_result_ != 0) { \
        ph_fail(__FILE__, __LINE__, ph_result_); \
    } \
} while (0)

void *ph_alloc(size_t size);
void ph_free(void *base, size_t size);
void ph_wait(atomic_uint *word, unsigned expected);
void ph_wake(atomic_uint *word);
void ph_lock(atomic_uint *lock);
void ph_unlock(atomic_uint *lock);
long ph_wait_readable(struct pacha_pollfd *descriptors, size_t count);

/* Native thread/notification ownership. Call only on a registered ph_task. */
int ph_notifications_save(uint64_t *mask);
int ph_notifications_restore(uint64_t mask);
void ph_thread_setup(struct ph_task *task);
struct ph_task *ph_thread_create(void *(*entry)(void *), void *argument);
int ph_thread_join(void *task);
_Noreturn void ph_thread_exit(void);

/* Logical CPUs and Linux exception dispatch; these callbacks may migrate. */
void ph_dispatch_notifications(void);
void ph_machine_init(struct ph_image *image);
int ph_exceptions_install(kobox_linux_exception_fn dispatch);
void ph_exception(struct pacha_native_fault_frame *frame);

/* Bounded core ELF loading and the host-operation tables passed into Linux. */
int ph_image_open(struct ph_image *image, const void *file, size_t size);
void *ph_image_lookup(void *image, const char *name);
int ph_image_protect(void *image, size_t offset, size_t length, unsigned protection);
void ph_layout_init(struct ph_image *image, struct kobox_linux_boot_layout *layout);
extern const struct kobox_linux_task_host_operations ph_task_ops;
extern const struct kobox_linux_memory_host_operations ph_memory_ops;
extern struct ph_image ph_core;

/* Boot-only verification entry; not the external client/VM transport. */
void ph_verify_chapter2_core(void);

#endif
