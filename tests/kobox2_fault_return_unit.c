/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "../userland/kobox2_adapter/vm_client_control.h"
#include <pacha/syscall.h>
#include <pacha/ipc.h>
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

extern _Noreturn void ph_vm_client_fault_return(struct pacha_native_fault_frame *);
extern long ph_vm_client_set_segment_base(uint64_t, uint64_t);
static jmp_buf escaped;
static struct pacha_native_fault_frame frame, original;
static unsigned calls, yields, denied;
static long response, yield_response;
static int restored;
static unsigned segment_calls, segment_denied;

long pacha_syscall2(uint64_t nr, uint64_t operation, uint64_t pointer) {
    assert(nr == PACHA_PROCESS_SYSCALL_SIGNAL_CTL);
    assert(operation == PACHA_PROCESS_SIGNAL_CTL_RETURN && pointer == (uintptr_t)&frame);
    assert(!memcmp(&frame, &original, sizeof(frame)));
    ++calls;
    if (calls <= denied) return PACHA_SYSCALL_ERR_NOT_READY;
    if (restored) longjmp(escaped, 1); /* Successful return transfers control. */
    return response;
}
long pacha_syscall0(uint64_t nr) {
    assert(nr == PACHA_RUNTIME_SYSCALL_YIELD);
    ++yields;
    return yield_response;
}
long pacha_syscall1(uint64_t nr, uint64_t code) {
    if (nr == PACHA_THREAD_SYSCALL_SET_FS_BASE || nr == PACHA_THREAD_SYSCALL_SET_GS_BASE) {
        assert(code == 0x1234000);
        return ++segment_calls <= segment_denied ? PACHA_SYSCALL_ERR_NOT_READY : response;
    }
    assert(nr == PACHA_PROCESS_SYSCALL_EXIT && code >= 128);
    longjmp(escaped, 2);
}

static void run(unsigned refusals, int success, long error, long yield_error,
    int expected, unsigned expected_calls, unsigned expected_yields) {
    calls = yields = 0; denied = refusals; restored = success;
    response = error; yield_response = yield_error;
    memset(&frame, 0xa5, sizeof(frame)); original = frame;
    int outcome = setjmp(escaped);
    if (!outcome) ph_vm_client_fault_return(&frame);
    assert(outcome == expected && calls == expected_calls && yields == expected_yields);
    assert(!memcmp(&frame, &original, sizeof(frame)));
}

int main(void) {
    void *control = mmap((void *)PH_VM_CLIENT_CONTROL, 4096,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(control == (void *)PH_VM_CLIENT_CONTROL);
    run(0, 1, 0, 0, 1, 1, 0);
    run(3, 1, 0, 0, 1, 4, 3);
    run(3, 1, 0, PACHA_SYSCALL_ERR_NOT_READY, 1, 4, 3);
    run(0, 0, PACHA_SYSCALL_ERR_INVALID, 0, 2, 1, 0);
    assert(((struct ph_vm_client_control *)control)->result == PACHA_SYSCALL_ERR_INVALID);
    run(0, 0, PACHA_ERR_NOT_READY, 0, 2, 1, 0); /* Wrong signed errno is not admitted. */
    run(1024, 1, 0, 0, 2, 1024, 1024);
    assert(((struct ph_vm_client_control *)control)->result == PACHA_SYSCALL_ERR_NOT_READY);
    response = 0; segment_denied = 3; segment_calls = yields = 0; yield_response = 0;
    assert(!ph_vm_client_set_segment_base(PACHA_THREAD_SYSCALL_SET_FS_BASE, 0x1234000));
    assert(segment_calls == 4 && yields == 3);
    response = PACHA_SYSCALL_ERR_INVALID; segment_denied = 0; segment_calls = yields = 0;
    assert(ph_vm_client_set_segment_base(PACHA_THREAD_SYSCALL_SET_GS_BASE, 0x1234000) == PACHA_SYSCALL_ERR_INVALID);
    assert(segment_calls == 1 && !yields);
    segment_denied = 1024; segment_calls = yields = 0;
    assert(ph_vm_client_set_segment_base(PACHA_THREAD_SYSCALL_SET_FS_BASE, 0x1234000) == PACHA_SYSCALL_ERR_NOT_READY);
    assert(segment_calls == 1024 && yields == 1024);
    segment_calls = yields = 0;
    assert(ph_vm_client_set_segment_base(PACHA_FD_SYSCALL_CLOSE, 0x1234000) == PACHA_SYSCALL_ERR_INVALID);
    assert(!segment_calls && !yields);
    assert(!munmap(control, 4096));
    puts("Native fault return: rejected admission, preserved frame, invalid errors and retry bound PASS");
}
