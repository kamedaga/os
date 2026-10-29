/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_HOST_H
#define PACHA_KOBOX_HOST_H

#include <pacha/abi.h>
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
#include "stack_layout.h"

/* Internal PachaOS host (ph_) API. One sandbox process owns one Linux core. */
#define PH_PAGE_SIZE 4096ul
#define PH_CPU_COUNT KOBOX_LINUX_MEMORY_LOGICAL_CPUS
#define PH_NOTIFICATION_SIGNAL 12u
#define PH_NOTIFICATION_MASK (UINT64_C(1) << (PH_NOTIFICATION_SIGNAL - 1))
/* At 256 MiB, real multi-tab 2560x1264 rendering reached the DRM allocation
 * reserve and rejected even one-page resources. Add 128 MiB of hosted-core
 * headroom without changing the VM's 4 GiB guest RAM configuration. */
#define PH_RAM_SIZE (384ul << 20)
#define PH_IMAGE_PHYSICAL_BASE ((size_t)KOBOX_CORE_PHYSICAL_BASE)

/* Each dedicated native waiter owns this cache. Fiber waits may migrate
 * between native threads and must continue using the transient helper. */
struct ph_wait_retained {
    size_t count;
    int valid;
    struct { uint64_t fd, events, minimum; } items[256];
};
long ph_wait_many_retained(struct ph_wait_retained *cache,
    struct pacha_pollfd *fds, size_t count, uint64_t timeout);

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
    size_t ram_size;         /* Immutable actual allocation after image_open. */
    pacha_fd_t ram_fd;
    unsigned char *ram_cache_modes; /* One byte per physical RAM page. */
    struct ph_dma *device_dma; /* Borrowed attached-domain list, quiesced at detach. */
    bool cache_drain_completed;
    struct ph_window window;
};

/* One hosted Linux task. Linux tasks are user-level contexts on the native
 * thread of their logical CPU: a native thread handoff per Linux context switch
 * cost about 2 us, an in-process switch only saves callee-saved state. Linux
 * migration resumes a fiber on another CPU thread, so everything here follows
 * the task, and nothing here may name the native thread it last ran on. */
enum ph_fiber_state { PH_FIBER_NEW, PH_FIBER_RUNNING, PH_FIBER_SWITCHED, PH_FIBER_DEAD };
struct ph_fiber {
    uint64_t rsp;                     /* Saved switch frame while not running. */
    void *mapping;                    /* Guard page plus stack; NULL if borrowed. */
    void *stack;                      /* Usable stack base (above the guard). */
    size_t stack_size;
    void *(*entry)(void *);
    void *argument;
    /* Native thread started on this fiber's own stack (secondary idle or an
     * aborted new task); it must exit before the stack can be released. */
    struct ph_task *origin;
    enum ph_fiber_state state;
    bool bound;                       /* Borrowed boot context; never released. */
    uint64_t saved_notification_mask; /* Mask before the most recent cpu_enter. */
    int logical_cpu;                  /* Owned CPU, or -1 outside Linux. */
    /* Hosted Linux machine state is explicit, so this object is portable to
     * hosts using libc TLS. It stays per task: Linux keeps current_task and
     * an active user call across schedule(), and port.c refreshes the CPU
     * fields whenever a switch returns, including on another logical CPU. */
    struct kobox_runtime_thread_state runtime;
};

struct ph_task {
    struct ph_task *self; /* Native FS:0, independent of hosted Linux TLS. */
    /* The fiber running on this native thread, NULL on helper threads. Read
     * only through ph_current_fiber(): see the single-instruction rule below. */
    struct ph_fiber *fiber;
    pacha_fd_t fd;
    /* Borrowed boot thread; not joinable by the adapter. */
    bool bound;
    /* Protect host operations from notification-driven Linux entry.
     * Native delivery stays enabled; Linux's logical IRQ depth is separate. */
    atomic_ullong notification_mask;
    atomic_uint notification_deferred;
#if defined(PH_SCHED_DIAGNOSTIC) && PH_SCHED_DIAGNOSTIC
    /* Private diagnostic state only: native upcalls can capture a masked
     * owner even when Linux's privileged register dump cannot run here. */
    atomic_uint diagnostic_frame_busy;
    atomic_ullong diagnostic_frame_sequence;
    atomic_ullong diagnostic_rip, diagnostic_rsp;
    atomic_ullong diagnostic_rdi, diagnostic_rsi, diagnostic_rax, diagnostic_rbx;
    atomic_ullong diagnostic_stack_valid;
    atomic_ullong diagnostic_stack_words[16];
    atomic_ullong diagnostic_frame_mask;
    atomic_ullong diagnostic_wait_word, diagnostic_wait_expected;
#endif
    void *(*entry)(void *);
    void *argument;
    void *stack;       /* Helper thread stack; NULL when started on a fiber. */
    void *fault_stack;
};

static inline struct ph_task *ph_current_task(void) {
    struct ph_task *task;

    __asm__ volatile("mov %%fs:0, %0" : "=r"(task));
    return task;
}

/* A notification taken while unmasked can switch fibers and resume this code
 * on another native thread at any instruction boundary. A ph_task pointer held
 * in a register across such a point names the old thread and, through it, the
 * fiber now running there. Code that can run unmasked therefore touches its
 * thread's words only through these single %fs-relative instructions. Masked
 * code cannot migrate. Tests without a native FS define PH_TLS_TEST_HOOKS and
 * supply ph_test_current_task(); they never migrate. */
#ifdef PH_TLS_TEST_HOOKS
struct ph_task *ph_test_current_task(void);
#endif
#ifndef PH_TLS_TEST_HOOKS
#define PH_TLS_LOAD(type, field) __extension__ ({ type ph_tls_value_; \
    __asm__ volatile("mov %%fs:%c1, %0" : "=r"(ph_tls_value_) \
        : "i"(offsetof(struct ph_task, field)) : "memory"); ph_tls_value_; })
#define PH_TLS_STORE(field, value) __asm__ volatile("mov %0, %%fs:%c1" \
    :: "r"(value), "i"(offsetof(struct ph_task, field)) : "memory")
#define PH_TLS_EXCHANGE(field, value) ({ __typeof__(value) ph_tls_value_ = (value); \
    __asm__ volatile("xchg %0, %%fs:%c1" : "+r"(ph_tls_value_) \
        : "i"(offsetof(struct ph_task, field)) : "memory"); ph_tls_value_; })
#else
#define PH_TLS_LOAD(type, field) ((type)atomic_load(&ph_test_current_task()->field))
#define PH_TLS_STORE(field, value) atomic_store(&ph_test_current_task()->field, (value))
#define PH_TLS_EXCHANGE(field, value) atomic_exchange(&ph_test_current_task()->field, (value))
#endif

static inline struct ph_fiber *ph_current_fiber(void) {
#ifndef PH_TLS_TEST_HOOKS
    return PH_TLS_LOAD(struct ph_fiber *, fiber);
#else
    return ph_test_current_task()->fiber;
#endif
}

static inline void ph_set_current_fiber(struct ph_fiber *fiber) {
#ifndef PH_TLS_TEST_HOOKS
    PH_TLS_STORE(fiber, fiber);
#else
    ph_test_current_task()->fiber = fiber;
#endif
}

/* The logical CPU of the running fiber; -1 on helpers and outside Linux. */
static inline int ph_current_cpu(void) {
    struct ph_fiber *fiber = ph_current_fiber();
    return fiber ? fiber->logical_cpu : -1;
}

/* Freestanding support. Allocation and unexpected syscall failures are fatal. */
void ph_log(const char *text);
/* Both IRQ enable and notification unmask can reenter Linux; the shared
 * preserving entry protects their caller's bank before running arbitrary C. */
int ph_notifications_dispatch_preserved(void);
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

/* Fibers. ph_fiber_switch saves the callee-saved GPRs, MXCSR and x87 control
 * word into a frame on the current stack, stores its address and resumes the
 * frame at next_rsp. Everything else is caller-saved at this call boundary. */
struct ph_fiber *ph_fiber_create(void *(*entry)(void *), void *argument);
void ph_fiber_destroy(struct ph_fiber *fiber);
void ph_fiber_switch(uint64_t *save_rsp, uint64_t next_rsp);
/* Starts a native thread directly on a NEW fiber's initial frame. */
struct ph_task *ph_fiber_thread_start(struct ph_fiber *fiber);

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
