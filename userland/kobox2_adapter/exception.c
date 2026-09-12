/* SPDX-License-Identifier: MIT */
/* Translate native fault frames; Linux retains exception/fixup policy. */
#include "host.h"

static kobox_linux_exception_fn linux_exception_dispatch;

int ph_exceptions_install(kobox_linux_exception_fn dispatch) {
    PH_CHECK(dispatch != NULL && linux_exception_dispatch == NULL);
    linux_exception_dispatch = dispatch;
    return 0;
}

void ph_exception(struct pacha_native_fault_frame *native) {
    struct pacha_native_signal_frame *saved = &native->signal;
    uint64_t previous_mask;

    PH_OK(ph_notifications_save(&previous_mask));
    struct kobox_linux_exception_frame exception = {
        .size = sizeof(exception),
        .ip = saved->rip,
        .sp = saved->rsp,
        .flags = saved->rflags,
        .fault_address = native->address,
        .error_code = native->error_code,
        .vector = (uint32_t)native->vector,
        .cpu = (uint32_t)ph_current_task()->logical_cpu,
        .registers = {
            saved->rax, saved->rbx, saved->rcx, saved->rdx,
            saved->rsi, saved->rdi, saved->rbp,
            saved->r8, saved->r9, saved->r10, saved->r11,
            saved->r12, saved->r13, saved->r14, saved->r15,
        },
    };

    if (linux_exception_dispatch == NULL || ph_current_task()->logical_cpu < 0 ||
        linux_exception_dispatch(&exception) != KOBOX_EXCEPTION_RESUME) {
        ph_number("fault vector", native->vector);
        ph_number("fault ip", saved->rip);
        ph_number("core offset", saved->rip - (uintptr_t)ph_core.base);
        ph_number("fault address", native->address);
        ph_fail(__FILE__, __LINE__, native->error_code);
    }

    /* Linux exception-table fixups may change both the PC and result registers. */
    uint64_t *registers[] = {
        &saved->rax, &saved->rbx, &saved->rcx, &saved->rdx,
        &saved->rsi, &saved->rdi, &saved->rbp,
        &saved->r8, &saved->r9, &saved->r10, &saved->r11,
        &saved->r12, &saved->r13, &saved->r14, &saved->r15,
    };
    for (unsigned i = 0; i < KOBOX_EXCEPTION_REGISTERS; ++i) {
        *registers[i] = exception.registers[i];
    }
    saved->rip = exception.ip;
    saved->rsp = exception.sp;
    saved->rflags = exception.flags;
    PH_OK(ph_notifications_restore(previous_mask));
}
