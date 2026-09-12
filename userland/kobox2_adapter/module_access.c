/* SPDX-License-Identifier: MIT */
/* Isolated native access to inherited mappings; never enter hosted Linux in
 * the child. This is the OS equivalent of the Linux adapter's fork probe. */
#include "vm_internal.h"
#include "boot/module_gate.h"

enum {
    PROBE_ALLOWED = 0,
    PROBE_DENIED = 76,
    PROBE_FAILED = 77,
};

extern void ph_module_fault_entry(void);
extern void ph_module_probe(void *address, unsigned operation);
extern char ph_module_probe_read[], ph_module_probe_write[];

#define PROBE_EXEC_COOKIE UINT64_C(0x6b62326d6f64756c)

static _Noreturn void exit_probe(unsigned status) {
    (void)pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT, status);
    __builtin_trap();
}

_Noreturn void ph_module_access_fault(struct pacha_native_fault_frame *frame) {
    bool valid = frame->signal.magic == PACHA_PROCESS_SIGNAL_FRAME_MAGIC &&
        frame->signal.size == sizeof(*frame) && !frame->signal.signo && frame->vector == 14;
    const struct pacha_native_signal_frame *saved = &frame->signal;
    bool access_site = (saved->rip == (uintptr_t)ph_module_probe_read ||
        saved->rip == (uintptr_t)ph_module_probe_write) &&
        frame->address >= saved->rdi && frame->address - saved->rdi < sizeof(uint64_t);
    bool execute_site = (frame->error_code & 16) && saved->r11 == PROBE_EXEC_COOKIE &&
        saved->rip == saved->rdi && frame->address == saved->rdi;

    /* A fault during child setup or in the probe implementation is not
     * evidence that the requested target access was denied. */
    exit_probe(valid && !(frame->error_code & ~UINT64_C(0x17)) &&
        (access_site || execute_site) ? PROBE_DENIED : PROBE_FAILED);
}

static _Noreturn void run_probe(void *address, enum kobox_linux_module_access operation) {
    struct ph_task *task = ph_current_task();

    if (pacha_syscall4(PACHA_PROCESS_SYSCALL_SIGNAL_CTL,
        PACHA_PROCESS_SIGNAL_CTL_REGISTER_FAULT, (uintptr_t)ph_module_fault_entry,
        (uintptr_t)task->fault_stack, PH_THREAD_STACK_SIZE)) {
        exit_probe(PROBE_FAILED);
    }
    ph_module_probe(address, operation);
    exit_probe(PROBE_ALLOWED);
}

int ph_module_access(void *address, enum kobox_linux_module_access operation) {
    if (!address || operation < KOBOX_MODULE_READ || operation > KOBOX_MODULE_EXECUTE) {
        return KOBOX_MODULE_ACCESS_FAILED;
    }
    uint64_t mask;

    PH_OK(ph_notifications_save(&mask));
    uint64_t rights = PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE;
    long process = pacha_syscall3(PACHA_PROCESS_SYSCALL_CLONE, rights,
        PACHA_PROCESS_CLONE_CURRENT_THREAD, 0);

    if (!process) {
        run_probe(address, operation);
    }
    int result = KOBOX_MODULE_ACCESS_FAILED;

    if (process >= 16) {
        struct pacha_pollfd completion = {
            .fd = (int)process, .events = PACHA_FD_EVENT_READABLE,
        };
        uint64_t status[4];

        PH_CHECK(ph_wait_readable(&completion, 1) == 1);
        PH_OK(pacha_syscall2(PACHA_PROCESS_SYSCALL_WAIT, process, (uintptr_t)status));
        if (status[0] == 2) {
            if (status[1] == PROBE_ALLOWED) {
                result = KOBOX_MODULE_ACCESS_ALLOWED;
            } else if (status[1] == PROBE_DENIED) {
                result = KOBOX_MODULE_ACCESS_DENIED;
            }
        }
        PH_OK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, process));
    } else {
        ph_number("module native probe create", process);
    }
    PH_OK(ph_notifications_restore(mask));
    return result;
}
