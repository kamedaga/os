/* SPDX-License-Identifier: MIT */
#include "boot.h"
#include "host.h"
#include "tail_trace.h"
#ifndef PH_SANDBOX_AMDGPU
#define PH_SANDBOX_AMDGPU 0
#endif
#if PH_SANDBOX_AMDGPU
#include <pacha/log.h>
#endif

struct ph_image ph_core;
static struct ph_task boot_task;
/* The boot context: the first Linux task, on the boot thread's own stack. */
static struct ph_fiber boot_fiber;
static struct kobox_linux_task_report task_report;
static void (*service_main)(const struct kobox_linux_task_report *, void *);
static void *service_context;

#if PH_SANDBOX_AMDGPU
static pacha_fd_t gpu_log_fd;
static atomic_flag gpu_log_lock = ATOMIC_FLAG_INIT;
static char gpu_log_line[256];
static size_t gpu_log_used;
static int gpu_log_overflow;

struct gpu_log_field {
    const char *name;
    unsigned long value;
};

static void gpu_log_fields(const struct gpu_log_field *fields, size_t count) {
    if (gpu_log_fd < 16)
        return;
    char line[256];
    char *out = line;
    for (size_t field = 0; field < count; ++field) {
        const char *name = fields[field].name;
        while (*name) {
            if (out == line + sizeof(line))
                return;
            *out++ = *name++;
        }
        /* The log endpoint holds only 16 IPC fragments. Fixed-width values
         * made two adjacent cache-transition records overflow that queue,
         * dropping the return status precisely when it mattered. */
        int shift = 60;
        while (shift != 0 && ((fields[field].value >> shift) & 15u) == 0)
            shift -= 4;
        for (; shift >= 0; shift -= 4) {
            if (out == line + sizeof(line))
                return;
            const unsigned digit = (fields[field].value >> shift) & 15u;
            *out++ = "0123456789abcdef"[digit];
        }
    }
    (void)pacha_log_emit(gpu_log_fd, PACHA_LOG_SOURCE_KOBOX2_GPU,
        line, (size_t)(out - line));
}

static int has_text(const char *line, size_t length, const char *needle) {
    const size_t wanted = strlen(needle);
    if (wanted > length)
        return 0;
    for (size_t i = 0; i <= length - wanted; ++i)
        if (!memcmp(line + i, needle, wanted))
            return 1;
    return 0;
}

static void gpu_log_copy(const char *text, size_t length) {
    if (gpu_log_fd < 16)
        return;
    size_t position = 0;
    while (position < length) {
        char line[sizeof(gpu_log_line)];
        size_t ready = 0;
        /* Linux printk can recur from an IRQ on the same host thread. The
         * serial sink remains authoritative; drop a contended RAM copy
         * rather than spinning inside that callback. */
        if (atomic_flag_test_and_set_explicit(&gpu_log_lock,
                memory_order_acquire))
            return;
        while (position < length) {
            const char c = text[position++];
            if (c == '\n') {
                if (!gpu_log_overflow) {
                    ready = gpu_log_used;
                    memcpy(line, gpu_log_line, ready);
                }
                gpu_log_used = 0;
                gpu_log_overflow = 0;
                break;
            }
            if (gpu_log_used < sizeof(gpu_log_line))
                gpu_log_line[gpu_log_used++] = c;
            else
                gpu_log_overflow = 1;
        }
        atomic_flag_clear_explicit(&gpu_log_lock, memory_order_release);
        if (ready && (has_text(line, ready, "kobox-firmware:") ||
                      has_text(line, ready, "kobox-pci:") ||
                      has_text(line, ready, "kobox-drm:") ||
                      has_text(line, ready, "kobox-mmio:") ||
                      has_text(line, ready, "Loaded FW:") ||
                      has_text(line, ready, "amdgpu") ||
                      has_text(line, ready, "ib test on") ||
                      has_text(line, ready, "ring ") ||
                      has_text(line, ready, "IRQ") ||
                      has_text(line, ready, "irq")))
            (void)pacha_log_emit(gpu_log_fd, PACHA_LOG_SOURCE_KOBOX2_GPU,
                line, ready);
    }
}
#endif

void ph_boot_set_log_fd(pacha_fd_t fd) {
#if PH_SANDBOX_AMDGPU
    gpu_log_fd = fd >= 16 ? fd : 0;
#else
    (void)fd;
#endif
}

void ph_boot_log_fault(unsigned long vector, unsigned long error,
    unsigned long core_relative, unsigned long ip, unsigned long address) {
#if PH_SANDBOX_AMDGPU
    const struct gpu_log_field fields[] = {
        {"Linux fault vector=0x", vector}, {" error=0x", error},
        {" core_relative=0x", core_relative}, {" ip=0x", ip},
        {" address=0x", address},
    };
    gpu_log_fields(fields, sizeof(fields) / sizeof(fields[0]));
#else
    (void)vector; (void)error; (void)core_relative; (void)ip; (void)address;
#endif
}

void ph_boot_log_fatal(unsigned long source, unsigned long line,
    unsigned long result) {
#if PH_SANDBOX_AMDGPU
    const struct gpu_log_field fields[] = {
        {"Linux fatal source=0x", source}, {" line=0x", line},
        {" result=0x", result},
    };
    gpu_log_fields(fields, sizeof(fields) / sizeof(fields[0]));
#else
    (void)source; (void)line; (void)result;
#endif
}

void ph_boot_log_gpu_map_failure(unsigned long stage, long status,
    unsigned long handle, unsigned long bytes) {
#if PH_SANDBOX_AMDGPU
    const struct gpu_log_field fields[] = {
        {"kobox-drm: map failure stage=0x", stage},
        {" status=0x", (unsigned long)status},
        {" handle=0x", handle},
        {" bytes=0x", bytes},
    };
    gpu_log_fields(fields, sizeof(fields) / sizeof(fields[0]));
#else
    (void)stage; (void)status; (void)handle; (void)bytes;
#endif
}

void ph_boot_log_gpu_page_view_failure(long status, unsigned long parent_fd,
    unsigned long rights, unsigned long cache, unsigned long page_count,
    unsigned long first_page, unsigned long middle_page,
    unsigned long last_page) {
#if PH_SANDBOX_AMDGPU
    const struct gpu_log_field fields[] = {
        {"kobox-drm: page view refused status=0x", (unsigned long)status},
        {" parent_fd=0x", parent_fd}, {" rights=0x", rights},
        {" cache=0x", cache}, {" pages=0x", page_count},
        {" first=0x", first_page}, {" middle=0x", middle_page},
        {" last=0x", last_page},
    };
    gpu_log_fields(fields, sizeof(fields) / sizeof(fields[0]));
#else
    (void)status; (void)parent_fd; (void)rights; (void)cache;
    (void)page_count; (void)first_page; (void)middle_page; (void)last_page;
#endif
}

void ph_boot_log_gpu_bar_export_failure(unsigned long reason, long status,
    unsigned long bar, unsigned long protection, unsigned long cache,
    unsigned long page_count, unsigned long first_page,
    unsigned long last_page) {
#if PH_SANDBOX_AMDGPU
    const struct gpu_log_field fields[] = {
        {"kobox-drm: BAR export refused reason=0x", reason},
        {" status=0x", (unsigned long)status},
        {" bar=0x", bar}, {" prot=0x", protection},
        {" cache=0x", cache}, {" pages=0x", page_count},
        {" first=0x", first_page}, {" last=0x", last_page},
    };
    gpu_log_fields(fields, sizeof(fields) / sizeof(fields[0]));
#else
    (void)reason; (void)status; (void)bar; (void)protection;
    (void)cache; (void)page_count; (void)first_page; (void)last_page;
#endif
}

void ph_boot_log_gpu_cache_transition(unsigned long stage,
    unsigned long offset, unsigned long bytes, unsigned long mode, long status) {
#if PH_SANDBOX_AMDGPU
    const struct gpu_log_field fields[] = {
        {"kobox-drm: cache transition stage=0x", stage},
        {" offset=0x", offset}, {" bytes=0x", bytes},
        {" mode=0x", mode}, {" status=0x", (unsigned long)status},
    };
    gpu_log_fields(fields, sizeof(fields) / sizeof(fields[0]));
#else
    (void)stage; (void)offset; (void)bytes; (void)mode; (void)status;
#endif
}

void ph_boot_log_gpu_cache_reject(long status, unsigned long offset,
    unsigned long bytes, unsigned long direct_overlaps,
    unsigned long scattered_mappings, unsigned long sample_iova,
    long inventory_status) {
#if PH_SANDBOX_AMDGPU
    const struct gpu_log_field fields[] = {
        {"kobox-drm: cache rejected status=0x", (unsigned long)status},
        {" offset=0x", offset}, {" bytes=0x", bytes},
        {" direct_dma=0x", direct_overlaps},
        {" scattered_dma=0x", scattered_mappings},
        {" sample_iova=0x", sample_iova},
        {" inventory_status=0x", (unsigned long)inventory_status},
    };
    gpu_log_fields(fields, sizeof(fields) / sizeof(fields[0]));
#else
    (void)status; (void)offset; (void)bytes; (void)direct_overlaps;
    (void)scattered_mappings; (void)sample_iova; (void)inventory_status;
#endif
}

void ph_boot_log_map_failure(unsigned long window, unsigned long offset,
    unsigned long backing, unsigned long bytes, unsigned long protection,
    unsigned long result) {
#if PH_SANDBOX_AMDGPU
    const struct gpu_log_field fields[] = {
        {"native mmap refused window=0x", window},
        {" offset=0x", offset},
        {" backing=0x", backing},
        {" bytes=0x", bytes},
        {" prot=0x", protection},
        {" result=0x", result},
    };
    gpu_log_fields(fields, sizeof(fields) / sizeof(fields[0]));
#else
    (void)window;
    (void)offset;
    (void)backing;
    (void)bytes;
    (void)protection;
    (void)result;
#endif
}

void ph_boot_log_scatter_failure(unsigned long window, unsigned long offset,
    unsigned long first_page, unsigned long pages, unsigned long protection,
    unsigned long step, unsigned long result) {
#if PH_SANDBOX_AMDGPU
    const struct gpu_log_field fields[] = {
        {"native scatter map refused step=0x", step},
        {" window=0x", window},
        {" offset=0x", offset},
        {" first_page=0x", first_page},
        {" pages=0x", pages},
        {" prot=0x", protection},
        {" result=0x", result},
    };
    gpu_log_fields(fields, sizeof(fields) / sizeof(fields[0]));
#else
    (void)window;
    (void)offset;
    (void)first_page;
    (void)pages;
    (void)protection;
    (void)step;
    (void)result;
#endif
}

void ph_boot_log_irq_failure(unsigned long mode, unsigned long index,
    unsigned long count, unsigned long cpu, unsigned long step,
    unsigned long raw, unsigned long result) {
#if PH_SANDBOX_AMDGPU
    const struct gpu_log_field fields[] = {
        {"native IRQ allocation refused mode=0x", mode},
        {" index=0x", index},
        {" count=0x", count},
        {" cpu=0x", cpu},
        {" step=0x", step},
        {" raw=0x", raw},
        {" result=0x", result},
    };
    gpu_log_fields(fields, sizeof(fields) / sizeof(fields[0]));
#else
    (void)mode;
    (void)index;
    (void)count;
    (void)cpu;
    (void)step;
    (void)raw;
    (void)result;
#endif
}

void ph_boot_log_irq_summary(unsigned long received,
    unsigned long dispatched, unsigned long acknowledged) {
#if PH_SANDBOX_AMDGPU
    const struct gpu_log_field fields[] = {
        {"native IRQ summary received=0x", received},
        {" dispatched=0x", dispatched},
        {" acknowledged=0x", acknowledged},
    };
    gpu_log_fields(fields, sizeof(fields) / sizeof(fields[0]));
#else
    (void)received;
    (void)dispatched;
    (void)acknowledged;
#endif
}

void ph_boot_log_irq_ready(unsigned long received,
    unsigned long dispatched, unsigned long acknowledged) {
#if PH_SANDBOX_AMDGPU
    const struct gpu_log_field fields[] = {
        {"native IRQ ready received=0x", received},
        {" dispatched=0x", dispatched},
        {" acknowledged=0x", acknowledged},
    };
    gpu_log_fields(fields, sizeof(fields) / sizeof(fields[0]));
#else
    (void)received;
    (void)dispatched;
    (void)acknowledged;
#endif
}

static void console_write(void *context, const char *text, size_t length) {
    (void)context;
    (void)pacha_syscall2(1, (uintptr_t)text, length);
    ph_tail_console(text, length);
#if PH_SANDBOX_AMDGPU
    gpu_log_copy(text, length);
#endif
}

static void kernel_main(void *argument) {
    service_main(argument, service_context);
    (void)pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT, 0);
    ph_fail(__FILE__, __LINE__, 0);
}

_Noreturn void ph_boot_run(const void *core, size_t size,
    void (*prepare)(const struct kobox_linux_boot_layout *, void *),
    void (*main)(const struct kobox_linux_task_report *, void *), void *context) {
    PH_CHECK(core && size && main && !service_main);
    service_main = main;
    service_context = context;
    boot_task.self = &boot_task;
    boot_task.bound = true;
    extern char ph_boot_stack[];
    boot_task.stack = ph_boot_stack;
    /* Same overflow guard as every other fiber: its lowest page. */
    PH_OK(pacha_syscall3(PACHA_VM_SYSCALL_MPROTECT, (uintptr_t)ph_boot_stack,
        PH_PAGE_SIZE, 0));
    boot_fiber.stack = ph_boot_stack + PH_PAGE_SIZE;
    boot_fiber.stack_size = PH_THREAD_STACK_SIZE - PH_PAGE_SIZE;
    boot_fiber.state = PH_FIBER_RUNNING;
    boot_fiber.bound = true;
    boot_fiber.logical_cpu = -1;
    boot_task.fiber = &boot_fiber;
    PH_OK(pacha_syscall1(PACHA_THREAD_SYSCALL_SET_FS_BASE, (uintptr_t)&boot_task));
    boot_task.fault_stack = ph_alloc(PH_THREAD_STACK_SIZE);
    ph_thread_setup(&boot_task);
    boot_task.fd = (pacha_fd_t)pacha_syscall4(PACHA_FD_SYSCALL_DUP, PACHA_THREAD_SELF_FD, 16,
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_PROCESS_SIGNAL, 0);
    PH_CHECK(boot_task.fd >= 16);

    PH_OK(ph_image_open(&ph_core, core, size));
    struct kobox_linux_boot_layout layout;
    ph_layout_init(&ph_core, &layout);
    layout.console_write = console_write;
    layout.kernel_main = kernel_main;
    layout.kernel_argument = &task_report;
    task_report.size = sizeof(task_report);
    task_report.identity = KOBOX_LINUX_TASK_HOST_IDENTITY;
    ph_machine_init(&ph_core);
    if (prepare)
        prepare(&layout, context);
    PH_OK(ph_task_ops.cpu_enter(0, &boot_fiber));
    PH_OK(ph_task_ops.cpu_irq_disable(0));
    ph_log("PACHA_KOBOX_CORE=START\n");
    int status;
    PH_CHECK(kobox_boot_core_start(&ph_core.core, &layout, &task_report, &status) == KOBOX_BOOT_CORE_OK);
    ph_fail(__FILE__, __LINE__, status);
}
