/* Native 2-CPU contract test; no LPR or pthread signal emulation. */
#include <stdint.h>
#include <stddef.h>
#include "pacha/ipc.h"

#define NOTIFY 12u
#define MASK (1ull << (NOTIFY - 1))
#define STRING_1(x) #x
#define STRING(x) STRING_1(x)
#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

static inline uint64_t native(uint64_t nr, uint64_t a, uint64_t b,
    uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    register uint64_t r10 __asm__("r10") = d;
    register uint64_t r8 __asm__("r8") = e;
    register uint64_t r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
        "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return nr;
}

static void log_line(const char *s)
{
    size_t n = 0;
    while (s[n]) ++n;
    (void)native(1, (uintptr_t)s, n, 0, 0, 0, 0);
}

__attribute__((noreturn)) static void fail_at(unsigned line)
{
    log_line("NATIVE_EXECUTION=FAIL\n");
    char digits[16];
    unsigned used = 0;
    do { digits[used++] = (char)('0' + line % 10); line /= 10; } while (line);
    for (unsigned i = 0; i < used / 2; ++i) {
        const char c = digits[i]; digits[i] = digits[used - i - 1]; digits[used - i - 1] = c;
    }
    digits[used++] = '\n'; digits[used] = 0;
    log_line(digits);
    (void)native(PACHA_PROCESS_SYSCALL_EXIT, 1, 0, 0, 0, 0, 0);
    __builtin_trap();
}
#define fail() fail_at(__LINE__)

static uint64_t now(void)
{
    uint64_t ts[2];
    if (native(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME, 1, (uintptr_t)ts, 0, 0, 0, 0)) fail();
    return ts[0] * 1000000000ull + ts[1];
}

static volatile unsigned ready[2], unmask[2], stop[2], count[2], bad;
static volatile unsigned inhibit_round[2], inhibit_ready[2], inhibit_release[2], inhibit_done[2];
static volatile uint64_t tids[2];
static unsigned char stacks[2][32768] __attribute__((aligned(4096)));
static unsigned char competitor_stacks[2][32768] __attribute__((aligned(4096)));
static volatile unsigned competitors_ready, competitors_stop;
static unsigned char fault_stack[16384] __attribute__((aligned(4096)));
static volatile uint64_t fault_address;
static volatile unsigned fault_count, invalid_return_checked;
static volatile uint64_t breakpoint_sp;
extern unsigned char native_breakpoint_resume[];
static const uint64_t fp_pattern[2] __attribute__((aligned(16))) = {
    0x123456789abcdef0ull, 0xfedcba9876543210ull
};

extern void notification_entry(void), worker0(void), worker1(void);
extern void competitor_entry(void);
extern unsigned char __start_native_handlers[], __stop_native_handlers[];
extern unsigned char __start_native_inhibited[], __stop_native_inhibited[];
extern void inhibited_wait(volatile unsigned *, volatile unsigned *, unsigned);
extern int native_clock_contract(void);

__attribute__((used, noinline, section("native_handlers")))
static void notification_body(struct pacha_native_signal_frame *frame)
{
    if (frame->magic != PACHA_PROCESS_SIGNAL_FRAME_MAGIC || frame->xstate_features != 7) fail();
    if (frame->signo == 0) {
        struct pacha_native_fault_frame *fault = (void *)frame;
        if (frame->size != sizeof(*fault)) fail();
        if (!LOAD(invalid_return_checked)) {
            const uint64_t cs = frame->cs;
            frame->cs = 0;
            if (native(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_RETURN,
                (uintptr_t)frame, 0, 0, 0, 0) != PACHA_SYSCALL_ERR_INVALID) fail();
            frame->cs = cs;
            STORE(invalid_return_checked, 1);
        }
        if (fault->vector == 14) {
            if (fault->address != LOAD(fault_address) || (fault->error_code & 6) != 6) fail();
            if (native(PACHA_VM_SYSCALL_MPROTECT, fault->address, 4096,
                PACHA_PROT_READ | PACHA_PROT_WRITE, 0, 0, 0)) fail();
        } else if (fault->vector == 6) {
            frame->rip += 2; /* This test's user policy skips its deliberate UD2. */
        } else if (fault->vector == 3) {
            if (fault->error_code || fault->address ||
                frame->rip != (uintptr_t)native_breakpoint_resume ||
                frame->rsp != LOAD(breakpoint_sp) || frame->r10 != 0x12345678) fail();
            /* A breakpoint is a trap: resume at the saved RIP, unchanged. */
        } else fail();
        __atomic_add_fetch(&fault_count, 1, __ATOMIC_RELEASE);
    } else {
        const uint64_t tid = native(PACHA_RUNTIME_SYSCALL_GETTID, 0, 0, 0, 0, 0, 0);
        const unsigned i = tid == LOAD(tids[0]) ? 0 : 1;
        if (frame->signo != NOTIFY || tid != LOAD(tids[i])) fail();
        __atomic_add_fetch(&count[i], 1, __ATOMIC_RELEASE);
    }
    frame->rflags |= (3ull << 12) | (1ull << 14) | (1ull << 17);
    __asm__ volatile("pxor %%xmm15, %%xmm15" ::: "xmm15");
}

__asm__(
    ".text\n"
#ifndef PACHA_NATIVE_TEST_INIT
    ".global _start\n"
    "_start: and $-16, %rsp; call main; mov %eax, %edi; mov $5, %eax; syscall; ud2\n"
#endif
    ".pushsection native_handlers,\"ax\",@progbits\n"
    ".global notification_entry\n"
    "notification_entry:\n"
    "mov %rdi, %r12\n"
    "call notification_body\n"
    "mov %r12, %rsi\n"
    "mov $3, %edi\n"
    "mov $" STRING(PACHA_PROCESS_SYSCALL_SIGNAL_CTL) ", %eax\n"
    "syscall\n"
    "ud2\n"
    ".popsection\n"
    ".pushsection native_inhibited,\"ax\",@progbits\n"
    ".global inhibited_wait\n"
    "inhibited_wait: mov %edx, (%rdi)\n"
    "1: pause; cmp %edx, (%rsi); jne 1b; ret\n"
    ".popsection\n"
    ".text\n"
    ".global worker0, worker1\n"
    "worker0: xor %edi, %edi; call worker_body; ud2\n"
    "worker1: mov $1, %edi; call worker_body; ud2\n"
    ".global competitor_entry\n"
    "competitor_entry: call competitor_body; ud2\n");

__attribute__((used, noreturn)) static void competitor_body(void)
{
    __atomic_add_fetch(&competitors_ready, 1, __ATOMIC_RELEASE);
    while (!LOAD(competitors_stop)) __asm__ volatile("pause");
    (void)native(PACHA_THREAD_SYSCALL_EXIT, 0, 0, 0, 0, 0, 0);
    __builtin_trap();
}

__attribute__((used, noreturn)) static void worker_body(unsigned i)
{
    if (native(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_REGISTER,
        (uintptr_t)notification_entry, (uintptr_t)__start_native_handlers,
        (uintptr_t)__stop_native_handlers, (uintptr_t)__start_native_inhibited,
        (uintptr_t)__stop_native_inhibited)) fail();
    if (native(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_SET_MASK,
        MASK, 0, 0, 0, 0)) fail();
    STORE(tids[i], native(PACHA_RUNTIME_SYSCALL_GETTID, 0, 0, 0, 0, 0, 0));
    __asm__ volatile("movdqa %0, %%xmm15" :: "m"(fp_pattern) : "xmm15");
    STORE(ready[i], 1);
    while (!LOAD(unmask[i])) __asm__ volatile("pause");
    if (native(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_SET_MASK,
        0, 0, 0, 0, 0)) fail();
    /* No syscalls in the busy loop: delivery must interrupt actual execution. */
    while (!LOAD(stop[i])) {
        const unsigned round = LOAD(inhibit_round[i]);
        if (round != LOAD(inhibit_done[i])) {
            inhibited_wait(&inhibit_ready[i], &inhibit_release[i], round);
            STORE(inhibit_done[i], round);
        }
        uint64_t observed[2] __attribute__((aligned(16)));
        __asm__ volatile("movdqa %%xmm15, %0" : "=m"(observed));
        if (observed[0] != fp_pattern[0] || observed[1] != fp_pattern[1]) STORE(bad, 1);
        __asm__ volatile("pause");
    }
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    if (flags & ((3ull << 12) | (1ull << 14) | (1ull << 17))) STORE(bad, 1);
    (void)native(PACHA_THREAD_SYSCALL_EXIT, 0, 0, 0, 0, 0, 0);
    __builtin_trap();
}

static void await(volatile unsigned *word, unsigned value)
{
    const uint64_t end = now() + 2000000000ull;
    while (LOAD(*word) != value) {
        if (LOAD(bad) || now() >= end) fail();
        __asm__ volatile("pause");
    }
}

static uint64_t create_worker(unsigned i)
{
    const uint64_t rights = PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_PROCESS_SIGNAL;
    const uint64_t fd = native(PACHA_THREAD_SYSCALL_CREATE, PACHA_PROCESS_SELF_FD,
        (uintptr_t)(i ? worker1 : worker0), (uintptr_t)(stacks[i] + sizeof(stacks[i])), 0, 0, rights);
    if (fd < 16) fail();
    return fd;
}

int main(void)
{
    _Static_assert(sizeof(struct pacha_native_signal_frame) == 1024, "signal frame");
    _Static_assert(sizeof(struct pacha_native_fault_frame) == 1048, "fault frame");
    /* Children inherit a blocked delivery configuration before START. */
    if (native(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_REGISTER,
        (uintptr_t)notification_entry, (uintptr_t)__start_native_handlers,
        (uintptr_t)__stop_native_handlers, 0, 0) ||
        native(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_SET_MASK,
        MASK, 0, 0, 0, 0)) fail();
    uint64_t fd[2] = { create_worker(0), create_worker(1) };
    const uint64_t self_rights = PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_PROCESS_SIGNAL;
    const uint64_t self = native(PACHA_FD_SYSCALL_DUP, PACHA_THREAD_SELF_FD,
        16, self_rights, 0, 0, 0);
    if (self < 16 || native(PACHA_THREAD_SYSCALL_SIGNAL, self, NOTIFY, 0, 0, 0, 0) ||
        native(PACHA_FD_SYSCALL_DUP, PACHA_THREAD_SELF_FD, 16,
        self_rights | PACHA_FD_RIGHT_KILL, 0, 0, 0) != PACHA_SYSCALL_ERR_INVALID ||
        native(PACHA_FD_SYSCALL_CLOSE, self, 0, 0, 0, 0, 0)) fail();
    /* Notification authority must not implicitly START a suspended thread. */
    if (native(PACHA_THREAD_SYSCALL_SIGNAL, fd[0], NOTIFY, 0, 0, 0, 0)) fail();
    for (unsigned k = 0; k < 1000; ++k) (void)now();
    if (LOAD(ready[0])) fail();
    for (unsigned i = 0; i < 2; ++i) {
        if (native(PACHA_THREAD_SYSCALL_START, fd[i], 0, 0, 0, 0, 0)) fail();
        await(&ready[i], 1);
    }
    const uint64_t restricted = native(PACHA_FD_SYSCALL_DUP, fd[0], 16,
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP, 0, 0, 0);
    if (restricted < 16 || native(PACHA_THREAD_SYSCALL_SIGNAL, restricted,
        NOTIFY, 0, 0, 0, 0) != PACHA_SYSCALL_ERR_INVALID) fail();
    if (native(PACHA_FD_SYSCALL_DUP, restricted, 16,
        PACHA_FD_RIGHT_PROCESS_SIGNAL, 0, 0, 0) != PACHA_SYSCALL_ERR_INVALID) fail();
    if (native(PACHA_FD_SYSCALL_CLOSE, restricted, 0, 0, 0, 0, 0)) fail();
    for (unsigned k = 0; k < 2; ++k)
        if (native(PACHA_THREAD_SYSCALL_SIGNAL, fd[0], NOTIFY, 0, 0, 0, 0)) fail();
    for (unsigned k = 0; k < 1000; ++k) (void)now();
    if (LOAD(count[0]) || LOAD(count[1])) fail();
    STORE(unmask[0], 1); STORE(unmask[1], 1);
    await(&count[0], 1);
    if (LOAD(count[1])) fail();
    for (unsigned round = 0; round < 32; ++round) {
        const unsigned i = round & 1;
        const unsigned old = LOAD(count[i]);
        if (native(PACHA_THREAD_SYSCALL_SIGNAL, fd[i], NOTIFY, 0, 0, 0, 0)) fail();
        await(&count[i], old + 1);
    }
    /* Consume the initial IPI inside an inhibited region, then leave it
     * without a syscall. Timer/preemption return must deliver the pending
     * notification; a second signal would hide a missing return boundary. */
    log_line("NATIVE_PENDING_NOTIFICATION_RETURN=START\n");
    uint64_t competitors[2];
    for (unsigned i = 0; i < 2; ++i) {
        competitors[i] = native(PACHA_THREAD_SYSCALL_CREATE, PACHA_PROCESS_SELF_FD,
            (uintptr_t)competitor_entry,
            (uintptr_t)(competitor_stacks[i] + sizeof(competitor_stacks[i])), 0, 0,
            PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE);
        if (competitors[i] < 16 || native(PACHA_THREAD_SYSCALL_START,
            competitors[i], 0, 0, 0, 0, 0)) fail();
    }
    /* Keep more runnable threads than native CPUs so expiration exercises
     * a context switch, not just the no-competitor continue-running path. */
    await(&competitors_ready, 2);
    for (unsigned round = 1; round <= 16; ++round) {
        for (unsigned i = 0; i < 2; ++i) {
            const unsigned old = LOAD(count[i]);
            STORE(inhibit_round[i], round);
            await(&inhibit_ready[i], round);
            if (native(PACHA_THREAD_SYSCALL_SIGNAL, fd[i], NOTIFY, 0, 0, 0, 0)) fail();
            const uint64_t until = now() + 20000000ull;
            while (now() < until) if (LOAD(count[i]) != old) fail();
            STORE(inhibit_release[i], round);
            await(&count[i], old + 1);
            await(&inhibit_done[i], round);
        }
    }
    log_line("NATIVE_PENDING_NOTIFICATION_RETURN=PASS\n");
    STORE(competitors_stop, 1);
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t status[4], rc;
        const uint64_t end = now() + 2000000000ull;
        while ((rc = native(PACHA_THREAD_SYSCALL_WAIT, competitors[i],
            (uintptr_t)status, 0, 0, 0, 0)) == PACHA_SYSCALL_ERR_NOT_READY)
            if (now() >= end) fail();
        if (rc || status[0] != 2 || status[1] ||
            native(PACHA_FD_SYSCALL_CLOSE, competitors[i], 0, 0, 0, 0, 0)) fail();
    }
    for (unsigned i = 0; i < 2; ++i) {
        STORE(stop[i], 1);
        uint64_t status[4];
        const uint64_t end = now() + 2000000000ull;
        uint64_t rc;
        while ((rc = native(PACHA_THREAD_SYSCALL_WAIT, fd[i], (uintptr_t)status, 0, 0, 0, 0)) == PACHA_SYSCALL_ERR_NOT_READY)
            if (now() >= end) fail();
        if (rc || status[0] != 2 || status[1] || LOAD(bad)) fail();
        if (native(PACHA_THREAD_SYSCALL_SIGNAL, fd[i], NOTIFY, 0, 0, 0, 0) != PACHA_SYSCALL_ERR_INVALID) fail();
        if (native(PACHA_FD_SYSCALL_CLOSE, fd[i], 0, 0, 0, 0, 0)) fail();
    }
    log_line("NATIVE_THREAD_NOTIFICATION=PASS\n");

    const uint64_t mapping = native(PACHA_VM_SYSCALL_MMAP, 0, 0, 4096, 0,
        PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS, 0);
    if (mapping < 4096) fail();
    STORE(fault_address, mapping);
    if (native(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_REGISTER_FAULT,
        (uintptr_t)notification_entry, (uintptr_t)fault_stack, sizeof(fault_stack), 0, 0)) fail();
    uint64_t observed[2] __attribute__((aligned(16)));
    __asm__ volatile("movdqa %0, %%xmm15" :: "m"(fp_pattern) : "xmm15");
    *(volatile uint64_t *)(uintptr_t)mapping = 0x1122334455667788ull;
    __asm__ volatile("ud2");
    uint64_t breakpoint_register;
    __asm__ volatile("mov %%rsp, %0; mov $0x12345678, %%r10; int3; "
        ".global native_breakpoint_resume; native_breakpoint_resume: mov %%r10, %1"
        : "=m"(breakpoint_sp), "=r"(breakpoint_register) :: "r10", "memory");
    __asm__ volatile("movdqa %%xmm15, %0" : "=m"(observed));
    if (LOAD(fault_count) != 3 || breakpoint_register != 0x12345678 || !LOAD(invalid_return_checked) ||
        observed[0] != fp_pattern[0] || observed[1] != fp_pattern[1] ||
        *(volatile uint64_t *)(uintptr_t)mapping != 0x1122334455667788ull) fail();
    if (native(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_REGISTER_FAULT,
        0, 0, 0, 0, 0) || native(PACHA_VM_SYSCALL_MUNMAP, mapping, 4096, 0, 0, 0, 0)) fail();
    log_line("NATIVE_FAULT_RETURN=PASS\n");
    log_line("NATIVE_EXECUTION=PASS\n");
    if (native_clock_contract()) fail();
    log_line("NATIVE_CLOCK_CONTRACT=PASS\n");
    return 0;
}
