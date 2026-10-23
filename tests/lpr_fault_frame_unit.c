#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"

lpr_state_t lpr_state;
static jmp_buf returned;
static void *returned_frame;
static unsigned char linux_stack[32768] __attribute__((aligned(16)));

int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t a) { (void)nr; (void)a; assert(0); return 0; }
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b) {
    if (nr == PACHAOS_SYSCALL_PROCESS_SIGNAL_CTL && a == PACHAOS_PROCESS_SIGNAL_CTL_RETURN) {
        returned_frame = (void *)(uintptr_t)b;
        longjmp(returned, 1);
    }
    return 0;
}
int64_t lpr_pacha_syscall4(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{ (void)n; (void)a; (void)b; (void)c; (void)d; return 0; }
int64_t lpr_pacha_syscall6(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{ (void)n; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; assert(0); return 0; }
int64_t lpr_pacha_status_to_errno(int64_t status) { return status; }
void *lpr_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void *lpr_memset(void *d, int c, size_t n) { return memset(d, c, n); }
lpr_signal_thread_state_t *lpr_signal_thread_state_current(void) { return &lpr_state.signal.threads[0].state; }
void lpr_signal_thread_state_prepare_current(void) {}
void lpr_state_lock(volatile uint32_t *word) { (void)word; }
void lpr_state_unlock(volatile uint32_t *word) { (void)word; }
uint64_t lpr_signalfd_union_mask(void) { return 0; }
int lpr_signalfd_enqueue(uint32_t signal) { (void)signal; assert(0); return 0; }
void lpr_linux_queue_signal(uint32_t signal) { (void)signal; assert(0); }
uint64_t lpr_linux_signal_bit(uint32_t signal) { return 1ULL << (signal - 1); }
uint64_t lpr_linux_unblockable_signal_mask(void) { return (1ULL << 8) | (1ULL << 18); }
int lpr_linux_default_signal_ignored(uint32_t signal) { (void)signal; return 0; }
int lpr_linux_default_signal_stops(uint32_t signal) { (void)signal; return 0; }
void lpr_linux_exit_for_signal(uint32_t signal) { (void)signal; assert(0); }
void lpr_async_signal_entry(void) {}
void lpr_fault_entry(void) {}
void lpr_signal_enter_handler(void) {}
void lpr_async_signal_restorer(void) {}
void lpr_syscall_entry(void) {}

#include "../userland/personality/linux/runtime/lpr_signal.c"

int main(void)
{
    static struct {
        lpr_pacha_signal_frame_t signal;
        uint64_t vector, error_code, address;
    } fault;
    static lpr_pacha_signal_frame_t original;
    const uint64_t errors[] = {4, 4 | PACHAOS_FAULT_ERROR_BACKING_UNAVAILABLE};
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        memset(&lpr_state, 0, sizeof(lpr_state));
        lpr_linux_signal_runtime_registered = 1;
        const unsigned signal = attempt ? 7 : 11;
        lpr_linux_sigactions[signal].handler = 0x12345678;
        memset(&fault, 0, sizeof(fault));
        fault.signal.magic = PACHAOS_PROCESS_SIGNAL_FRAME_MAGIC;
        fault.signal.size = PACHAOS_PROCESS_FAULT_FRAME_SIZE;
        fault.signal.xstate_features = PACHAOS_PROCESS_SIGNAL_XSTATE_FEATURE_MASK;
        uint64_t *regs = (void *)&fault.signal.context;
        for (unsigned i = 0; i < 20; ++i) regs[i] = 0x10000 + i;
        fault.signal.context.rsp = (uintptr_t)linux_stack + sizeof(linux_stack);
        for (unsigned i = 0; i < sizeof(fault.signal.x_state); ++i)
            fault.signal.x_state[i] = (unsigned char)(i * 31);
        fault.vector = 14; fault.error_code = errors[attempt]; fault.address = 0xdead000;
        original = fault.signal;
        if (!setjmp(returned)) lpr_linux_fault_prepare(&fault);
        assert(returned_frame == &fault.signal);
        assert(fault.signal.context.rip == (uintptr_t)lpr_signal_enter_handler);
        lpr_linux_signal_frame_t *body = (void *)(uintptr_t)fault.signal.context.rax;
        assert((unsigned char *)body >= linux_stack && (unsigned char *)(body + 1) <= linux_stack + sizeof(linux_stack));
        assert(body->info.si_signo == (int)signal && body->info.payload[0] == fault.address);
        assert(body->info.si_code == (attempt ? 2 : 1));
        assert(!memcmp(&body->native.context, &original.context, sizeof(original.context)));
        assert(!memcmp(body->native.x_state, original.x_state, sizeof(original.x_state)));
        assert(body->native.size == PACHAOS_PROCESS_SIGNAL_FRAME_SIZE);
        if (!setjmp(returned)) lpr_linux_rt_sigreturn_body(body);
        assert(returned_frame == &body->native);
        assert(!memcmp(&body->native.context, &original.context, sizeof(original.context)));
        assert(!memcmp(body->native.x_state, original.x_state, sizeof(original.x_state)));
    }
    assert(lpr_fault_linux_signal(6, 0) == 4 && lpr_fault_linux_signal(0, 0) == 8);
    assert(lpr_fault_linux_signal(3, 0) == 5 && lpr_fault_linux_signal(17, 0) == 7);
    puts("LPR_FAULT_FRAME=PASS original-native-return Linux-stack GPR/XState SIGBUS/SIGSEGV rt_sigreturn vectors");
}
