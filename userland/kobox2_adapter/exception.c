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
        ph_number("fault logical cpu", (uint64_t)(int64_t)ph_current_task()->logical_cpu);
        ph_number("fault stack", saved->rsp);
        ph_number("fault rbp", saved->rbp);
        /* The native exception stack hides Linux's interrupted call chain.
         * Report only code pointers from the mapped stack page, never raw
         * stack data, when the Linux exception bridge cannot resume it. */
        if (saved->rsp >= 4096 && !(saved->rsp & 7)) {
            const uintptr_t core_start = (uintptr_t)ph_core.base;
            const size_t words = (4096 - (saved->rsp & 4095)) / sizeof(uintptr_t);
            const uintptr_t *stack = (const uintptr_t *)(uintptr_t)saved->rsp;
            unsigned shown = 0;
            for (size_t i = 0; i < words && shown < 8; ++i) {
                for (unsigned segment = 0; segment < ph_core.fixed.segment_count; ++segment) {
                    const struct kobox_fixed_image_segment *code =
                        &ph_core.fixed.segments[segment];
                    const uintptr_t begin = core_start + code->image_offset;
                    if ((code->protection & KOBOX_FIXED_IMAGE_EXECUTE) &&
                        stack[i] >= begin && stack[i] - begin < code->mapping_size) {
                        ph_number("core stack candidate", stack[i] - core_start);
                        ++shown;
                        break;
                    }
                }
            }
        }
        /* GOP-only boots cannot see ph_number's serial output. Publish the
         * exact fault location before the fatal process exit when a service
         * has installed an owner-side diagnostic reporter. */
        ph_report_exception(native);
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
