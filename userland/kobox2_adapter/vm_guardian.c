/* SPDX-License-Identifier: MIT */
/* This process is independent of the managed child's STOP/CONTINUE state. */
#include "vm_guardian.h"
#include <pacha/ipc.h>
#include <pacha/syscall.h>

static struct pacha_pollfd events[] = {
    {.fd = PH_GUARDIAN_LIFELINE_FD,
        .events = PACHA_FD_EVENT_HANGUP | PACHA_FD_EVENT_ERROR},
    {.fd = PH_GUARDIAN_CHILD_FD,
        .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
};
static const uint64_t one = 1;

static _Noreturn void finish(unsigned status) {
    if (status) {
        static const char message[] = "VM guardian failed\n";
        (void)pacha_syscall2(1, (uintptr_t)message, sizeof(message) - 1);
    }
    (void)pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT, status);
    __builtin_trap();
}

_Noreturn void ph_vm_guardian_main(void) {
    if (pacha_syscall3(PACHA_FD_SYSCALL_WRITE, PH_GUARDIAN_READY_FD,
            (uintptr_t)&one, sizeof(one)) != sizeof(one)) finish(1);
    for (;;) {
        events[0].revents = events[1].revents = 0;
        long ready = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
            (uintptr_t)events, 2, PACHA_FD_WAIT_FOREVER, 0);
        if (events[1].revents & PACHA_FD_EVENT_READABLE) finish(0);
        if (events[0].revents & (PACHA_FD_EVENT_HANGUP | PACHA_FD_EVENT_ERROR)) {
            for (;;) {
                long killed = pacha_syscall2(PACHA_PROCESS_SYSCALL_KILL,
                    PH_GUARDIAN_CHILD_FD, 0);
                if (!killed) break;
                uint64_t status[4];
                long result = pacha_syscall2(PACHA_PROCESS_SYSCALL_WAIT,
                    PH_GUARDIAN_CHILD_FD, (uintptr_t)status);
                if (!result && (status[0] == 2 || status[0] == 3)) finish(0);
                /* NOT_READY means the kill was not accepted, not that a
                 * terminal notification is guaranteed to arrive. */
                if (killed == PACHA_SYSCALL_ERR_NOT_READY &&
                    result == PACHA_SYSCALL_ERR_NOT_READY) {
                    (void)pacha_syscall0(PACHA_RUNTIME_SYSCALL_YIELD);
                    continue;
                }
                finish(2);
            }
            /* Exit only after the killed child's terminal state is visible. */
            events[0] = events[1];
            for (;;) {
                events[0].revents = 0;
                long result = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
                    (uintptr_t)events, 1, PACHA_FD_WAIT_FOREVER, 0);
                if (events[0].revents & PACHA_FD_EVENT_READABLE) finish(0);
                if (result != PACHA_SYSCALL_ERR_NOT_READY) finish(3);
            }
        }
        if (ready == PACHA_SYSCALL_ERR_NOT_READY &&
            !events[0].revents && !events[1].revents) continue;
        finish(4);
    }
}
