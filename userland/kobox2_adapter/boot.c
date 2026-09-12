/* SPDX-License-Identifier: MIT */
#include "boot.h"
#include "host.h"

struct ph_image ph_core;
static struct ph_task boot_task;
static struct kobox_linux_task_report task_report;
static void (*service_main)(const struct kobox_linux_task_report *, void *);
static void *service_context;

static void console_write(void *context, const char *text, size_t length) {
    (void)context;
    (void)pacha_syscall2(1, (uintptr_t)text, length);
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
    boot_task.logical_cpu = -1;
    boot_task.bound = true;
    PH_OK(pacha_syscall1(PACHA_THREAD_SYSCALL_SET_FS_BASE, (uintptr_t)&boot_task));
    boot_task.fault_stack = ph_alloc(PH_THREAD_STACK_SIZE);
    ph_thread_setup(&boot_task);
    boot_task.fd = (int)pacha_syscall4(PACHA_FD_SYSCALL_DUP, PACHA_THREAD_SELF_FD, 16,
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
    PH_OK(ph_task_ops.cpu_enter(0, &boot_task));
    PH_OK(ph_task_ops.cpu_irq_disable(0));
    ph_log("PACHA_KOBOX_CORE=START\n");
    int status;
    PH_CHECK(kobox_boot_core_start(&ph_core.core, &layout, &task_report, &status) == KOBOX_BOOT_CORE_OK);
    ph_fail(__FILE__, __LINE__, status);
}
