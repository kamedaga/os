/* SPDX-License-Identifier: MIT */
/* Real native compound wake/wait; no Linux futex emulation. */
#include <stdint.h>
#include <stddef.h>
#include <pacha/abi.h>
#include <pacha/ipc.h>
#include <pacha/syscall.h>

#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define STRING_1(x) #x
#define STRING(x) STRING_1(x)
static unsigned permits[2], done[2], entered[2];
static unsigned single_wait;
static unsigned competitor_ready, competitor_stop;
static uint64_t competitor_progress;
static unsigned char stacks[3][32768] __attribute__((aligned(4096)));
static uint64_t markers[2][2] = {
    {0x1020304050607080ull, 0x8070605040302010ull},
    {0x1133557799bbddffull, 0xffddbb9977553311ull}
};
static const uint64_t fp_pattern[2] __attribute__((aligned(16))) = {
    0xabcdef0123456789ull, 0x9876543210fedcbaull
};
static const uint64_t worker_fp_pattern[2][2] __attribute__((aligned(16))) = {
    {0x1029384756abcdefull, 0xfedcba6547382910ull},
    {0x0192837465fedcbaull, 0xabcdef5647382910ull}
};
static const uint64_t competitor_fp_pattern[2] __attribute__((aligned(16))) = {
    0x2468ace013579bdfull, 0xfdb975310eca8642ull
};
static unsigned ww_word __attribute__((used));
static unsigned signal_seen;
static uint64_t signal_status, signal_count, signal_rip;
extern unsigned char __start_ww_handlers[], __stop_ww_handlers[];
extern unsigned char ww_pending_begin[], ww_pending_resume[];
extern unsigned char __start_native_handlers[], __stop_native_handlers[];
extern void notification_entry(void), ww_signal_entry(void);
extern long ww_pending(unsigned self, uint64_t *count);

static void line(const char *text) {
    size_t n = 0;
    while (text[n]) ++n;
    (void)pacha_syscall2(1, (uintptr_t)text, n);
}
static int failure(unsigned n) {
    line("NATIVE_WAKE_WAIT=FAIL line=");
    char digits[16]; unsigned used = 0;
    do { digits[used++] = '0' + n % 10; n /= 10; } while (n);
    for (unsigned i = 0; i < used / 2; ++i) {
        char c = digits[i]; digits[i] = digits[used - 1 - i]; digits[used - 1 - i] = c;
    }
    digits[used++] = '\n'; digits[used] = 0;
    line(digits);
    return 1;
}
#define CHECK(x) do { if (!(x)) return failure(__LINE__); } while (0)

static uint64_t now(void) {
    uint64_t ns;
    return pacha_clock_read(1, &ns) ? UINT64_MAX : ns;
}
static long sleep_tick(void) {
    const uint64_t delay[2] = {0, 1000000};
    return pacha_syscall2(PACHA_RUNTIME_SYSCALL_NANOSLEEP, (uintptr_t)delay, 0);
}
static int bases_ok(unsigned i) {
    uint64_t fs, gs;
    __asm__ volatile("mov %%fs:0,%0; mov %%gs:0,%1" : "=r"(fs), "=r"(gs));
    return fs == markers[i][0] && gs == markers[i][1];
}

__attribute__((used, noreturn)) static void ww_worker(unsigned i) {
    int bad = 0;
    STORE(entered[i], 1);
    if (LOAD(single_wait)) {
        long rc = pacha_syscall3(PACHA_RUNTIME_SYSCALL_FUTEX_WAIT,
            (uintptr_t)&permits[i], 0, 2000);
        STORE(done[i], rc ? 2 : 1);
        (void)pacha_syscall3(PACHA_THREAD_SYSCALL_EXIT, rc ? 1 : 0, 0, 0);
        __builtin_trap();
    }
    for (unsigned round = 0; round < 256 && !bad; ++round) {
        unsigned attempts = 0;
        while (!LOAD(permits[i])) {
            long rc = pacha_syscall3(PACHA_RUNTIME_SYSCALL_FUTEX_WAIT,
                (uintptr_t)&permits[i], 0, 20);
            if ((rc && rc != PACHA_SYSCALL_ERR_NOT_READY &&
                 rc != PACHA_FUTEX_WAIT_TIMED_OUT_STATUS) || ++attempts > 1000) {
                bad = 1; break;
            }
        }
        if (bad || !bases_ok(i)) { bad = 1; break; }
        STORE(permits[i], 0);
        STORE(permits[1 - i], 1);
        uint64_t count = UINT64_MAX, observed[2] __attribute__((aligned(16)));
        __asm__ volatile("movdqa %0,%%xmm15" :: "m"(worker_fp_pattern[i]) : "xmm15");
        /* The last publication still wakes once but intentionally mismatches
         * its own wait, so neither worker needs a post-exit inbound permit. */
        long rc = pacha_futex_wake_wait((uintptr_t)&permits[1 - i],
            (uintptr_t)&permits[i], round == 255 ? 1 : 0, 20, &count);
        __asm__ volatile("movdqa %%xmm15,%0" : "=m"(observed));
        if (count > 1 || (rc && rc != PACHA_SYSCALL_ERR_NOT_READY &&
            rc != PACHA_FUTEX_WAIT_TIMED_OUT_STATUS) || !bases_ok(i) ||
            observed[0] != worker_fp_pattern[i][0] ||
            observed[1] != worker_fp_pattern[i][1]) bad = 1;
    }
    if (bad) line("NATIVE_WAKE_WAIT=FAIL worker\n");
    STORE(done[i], bad ? 2 : 1);
    (void)pacha_syscall3(PACHA_THREAD_SYSCALL_EXIT, bad, 0, 0);
    __builtin_trap();
}
__attribute__((used, noreturn)) static void ww_competitor(void) {
    int bad = 0;
    __asm__ volatile("movdqa %0,%%xmm15" :: "m"(competitor_fp_pattern) : "xmm15");
    STORE(competitor_ready, 1);
    while (!LOAD(competitor_stop)) {
        uint64_t observed[2] __attribute__((aligned(16)));
        __asm__ volatile("movdqa %%xmm15,%0" : "=m"(observed));
        if (observed[0] != competitor_fp_pattern[0] ||
            observed[1] != competitor_fp_pattern[1]) { bad = 1; break; }
        __atomic_fetch_add(&competitor_progress, 1, __ATOMIC_RELAXED);
    }
    if (bad) line("NATIVE_WAKE_WAIT=FAIL competitor FP\n");
    (void)pacha_syscall3(PACHA_THREAD_SYSCALL_EXIT, bad, 0, 0);
    __builtin_trap();
}
extern void ww_worker0(void), ww_worker1(void), ww_competitor_entry(void);
__asm__(".text\n.global ww_worker0,ww_worker1,ww_competitor_entry\n"
    "ww_worker0: xor %edi,%edi; call ww_worker; ud2\n"
    "ww_worker1: mov $1,%edi; call ww_worker; ud2\n"
    "ww_competitor_entry: call ww_competitor; ud2\n");

static int join(uint64_t fd) {
    uint64_t status[4], end = now() + 3000000000ull;
    long rc;
    while ((rc = pacha_syscall2(PACHA_THREAD_SYSCALL_WAIT, fd,
        (uintptr_t)status)) == PACHA_SYSCALL_ERR_NOT_READY) {
        CHECK(now() < end && !sleep_tick());
    }
    CHECK(!rc && status[0] == 2 && !status[1]);
    CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, fd));
    return 0;
}
static long create(void (*entry)(void), unsigned index) {
    return pacha_syscall6(PACHA_THREAD_SYSCALL_CREATE, PACHA_PROCESS_SELF_FD,
        (uintptr_t)entry, (uintptr_t)(stacks[index] + sizeof(stacks[index])),
        0, 0, PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_SET_CONTEXT);
}
static int worker_contract(unsigned competitor) {
    uint64_t fd[2], third = 0;
    STORE(permits[0], 1); STORE(permits[1], 0);
    STORE(done[0], 0); STORE(done[1], 0);
    STORE(entered[0], 0); STORE(entered[1], 0);
    if (competitor) {
        STORE(competitor_ready, 0); STORE(competitor_stop, 0);
        third = create(ww_competitor_entry, 2);
        CHECK(third >= 16 && !pacha_syscall1(PACHA_THREAD_SYSCALL_START, third));
        uint64_t end = now() + 3000000000ull;
        while (!LOAD(competitor_ready)) CHECK(now() < end && !sleep_tick());
    }
    uint64_t progress = LOAD(competitor_progress);
    for (unsigned i = 0; i < 2; ++i) {
        fd[i] = create(i ? ww_worker1 : ww_worker0, i);
        CHECK(fd[i] >= 16);
        struct pacha_thread_context context;
        CHECK(!pacha_syscall4(PACHA_THREAD_SYSCALL_CONTEXT, fd[i],
            PACHA_THREAD_CONTEXT_GET, (uintptr_t)&context, sizeof(context)));
        context.fs_base = (uintptr_t)&markers[i][0];
        context.gs_base = (uintptr_t)&markers[i][1];
        CHECK(!pacha_syscall4(PACHA_THREAD_SYSCALL_CONTEXT, fd[i],
            PACHA_THREAD_CONTEXT_SET, (uintptr_t)&context, sizeof(context)));
    }
    for (unsigned i = 0; i < 2; ++i)
        CHECK(!pacha_syscall1(PACHA_THREAD_SYSCALL_START, fd[i]));
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(!join(fd[i]));
        CHECK(LOAD(entered[i]) && LOAD(done[i]) == 1);
    }
    if (competitor) {
        CHECK(LOAD(competitor_progress) > progress);
        STORE(competitor_stop, 1);
        CHECK(!join(third));
    }
    return 0;
}

static int completed_wake_contract(void) {
    /* Force a real peer to park before testing wait-side rejection. This
     * proves count1, not merely an allowed count0 on a publication race. */
    STORE(single_wait, 1);
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        STORE(permits[0], 0); STORE(entered[0], 0); STORE(done[0], 0);
        long fd = create(ww_worker0, 0);
        CHECK(fd >= 16 && !pacha_syscall1(PACHA_THREAD_SYSCALL_START, fd));
        uint64_t end = now() + 3000000000ull;
        while (!LOAD(entered[0])) CHECK(now() < end && !sleep_tick());
        const uint64_t delay[2] = {0, 3000000};
        CHECK(!pacha_syscall2(PACHA_RUNTIME_SYSCALL_NANOSLEEP, (uintptr_t)delay, 0));
        unsigned own_word = 1;
        uint64_t count;
        uint64_t wait_va = scenario == 0 ? (uintptr_t)&own_word :
            scenario == 1 ? 0 : (uintptr_t)&permits[0];
        long rc = pacha_futex_wake_wait((uintptr_t)&permits[0], wait_va,
            0, scenario == 2 ? 2 : 0, &count);
        CHECK(count == 1);
        CHECK(rc == (scenario == 0 ? PACHA_SYSCALL_ERR_NOT_READY :
            scenario == 1 ? PACHA_SYSCALL_ERR_INVALID : PACHA_FUTEX_WAIT_TIMED_OUT_STATUS));
        CHECK(!join(fd) && LOAD(done[0]) == 1);
    }
    STORE(single_wait, 0);
    return 0;
}

__attribute__((used, noinline, section("ww_handlers")))
static void ww_signal_body(struct pacha_native_signal_frame *frame) {
    signal_status = frame->rax;
    signal_count = frame->rdx;
    signal_rip = frame->rip;
    STORE(signal_seen, frame->signo);
    __asm__ volatile("pxor %%xmm15,%%xmm15" ::: "xmm15");
}
__asm__(".pushsection ww_handlers,\"ax\",@progbits\n.global ww_signal_entry\n"
    "ww_signal_entry: mov %rdi,%r12; call ww_signal_body; mov %r12,%rsi\n"
    "mov $3,%edi; mov $" STRING(PACHA_PROCESS_SYSCALL_SIGNAL_CTL) ",%eax; syscall; ud2\n"
    ".popsection\n.text\n.global ww_pending\n"
    "ww_pending: push %rbx; mov %rsi,%rbx\n"
    ".global ww_pending_begin\nww_pending_begin:\n"
    "mov $12,%esi; mov $" STRING(PACHA_THREAD_SYSCALL_SIGNAL) ",%eax; syscall\n"
    "test %rax,%rax; jnz 1f\n"
    "lea ww_word(%rip),%rdi; mov %rdi,%rsi; xor %r10d,%r10d; xor %r8d,%r8d\n"
    "mov $" STRING(PACHA_RUNTIME_SYSCALL_FUTEX_WAKE_WAIT) ",%eax; syscall\n"
    ".global ww_pending_resume\nww_pending_resume: mov %rdx,(%rbx)\n"
    "1: pop %rbx; ret\n");

static int checked_word_contract(void) {
    const uint64_t page = 4096;
    long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, 0, 0, page,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS, 0);
    CHECK(address >= (long)page);
    unsigned *words = (void *)(uintptr_t)address;
    words[0] = words[page / sizeof(*words) - 1] = 1;
    uint64_t count;
    /* Mismatched values test the read without arming a wait. Include the
     * final aligned word, readonly input and failures after permission/lifetime
     * changes; a fast copy must not use an unchecked physical alias. */
    for (unsigned readonly = 0; readonly < 2; ++readonly) {
        CHECK(pacha_futex_wake_wait(address, address, 0, 0, &count) ==
            PACHA_SYSCALL_ERR_NOT_READY && count == 0);
        CHECK(pacha_futex_wake_wait(address, address + page - 4, 0, 0, &count) ==
            PACHA_SYSCALL_ERR_NOT_READY && count == 0);
        CHECK(!pacha_syscall3(PACHA_VM_SYSCALL_MPROTECT, address, page, PACHA_PROT_READ));
    }
    CHECK(pacha_futex_wake_wait(address, address + 1, 0, 0, &count) ==
        PACHA_SYSCALL_ERR_INVALID && count == 0);
    CHECK(!pacha_syscall3(PACHA_VM_SYSCALL_MPROTECT, address, page, 0));
    CHECK(pacha_futex_wake_wait(address, address, 0, 0, &count) ==
        PACHA_SYSCALL_ERR_INVALID && count == 0);
    CHECK(!pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, address, page));
    CHECK(pacha_futex_wake_wait(address, address + page - 4, 0, 0, &count) ==
        PACHA_SYSCALL_ERR_INVALID && count == 0);
    return 0;
}

int native_wake_wait_contract(void) {
    CHECK(!checked_word_contract());
    uint64_t count = 99;
    unsigned word = 1;
    CHECK(pacha_futex_wake_wait(0, (uintptr_t)&word, 0, 0, &count) ==
        PACHA_SYSCALL_ERR_INVALID && count == UINT64_MAX);
    CHECK(pacha_futex_wake_wait((uintptr_t)&word, (uintptr_t)&word, 0, 0, &count) ==
        PACHA_SYSCALL_ERR_NOT_READY && count == 0);
    CHECK(pacha_futex_wake_wait((uintptr_t)&word, 0, 0, 0, &count) ==
        PACHA_SYSCALL_ERR_INVALID && count == 0);
    word = 0;
    CHECK(pacha_futex_wake_wait((uintptr_t)&word, (uintptr_t)&word, 0, 2, &count) ==
        PACHA_FUTEX_WAIT_TIMED_OUT_STATUS && count == 0);
    /* A timeout retired the caller token; alias must not wake that old wait. */
    word = 1;
    CHECK(pacha_futex_wake_wait((uintptr_t)&word, (uintptr_t)&word, 0, 0, &count) ==
        PACHA_SYSCALL_ERR_NOT_READY && count == 0);
    CHECK(!completed_wake_contract());
    CHECK(!worker_contract(0));
    CHECK(!worker_contract(1));
    uint64_t self = pacha_syscall4(PACHA_FD_SYSCALL_DUP, PACHA_THREAD_SELF_FD, 16,
        PACHA_FD_RIGHT_PROCESS_SIGNAL | PACHA_FD_RIGHT_CLOSE, 0);
    CHECK(self >= 16);
    CHECK(!pacha_syscall6(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_REGISTER,
        (uintptr_t)ww_signal_entry, (uintptr_t)__start_ww_handlers,
        (uintptr_t)__stop_ww_handlers, (uintptr_t)ww_pending_begin,
        (uintptr_t)ww_pending_resume));
    CHECK(!pacha_syscall2(PACHA_PROCESS_SYSCALL_SIGNAL_CTL,
        PACHA_PROCESS_SIGNAL_CTL_SET_MASK, 0));
    uint64_t observed[2] __attribute__((aligned(16)));
    __asm__ volatile("movdqa %0,%%xmm15" :: "m"(fp_pattern) : "xmm15");
    CHECK(ww_pending(self, &count) == PACHA_SYSCALL_ERR_NOT_READY);
    __asm__ volatile("movdqa %%xmm15,%0" : "=m"(observed));
    CHECK(count == 0 && LOAD(signal_seen) == 12 && signal_status == PACHA_SYSCALL_ERR_NOT_READY);
    CHECK(signal_count == count && signal_rip == (uintptr_t)ww_pending_resume);
    CHECK(observed[0] == fp_pattern[0] && observed[1] == fp_pattern[1]);
    CHECK(!pacha_syscall6(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_REGISTER,
        (uintptr_t)notification_entry, (uintptr_t)__start_native_handlers,
        (uintptr_t)__stop_native_handlers, 0, 0));
    CHECK(!pacha_syscall2(PACHA_PROCESS_SYSCALL_SIGNAL_CTL,
        PACHA_PROCESS_SIGNAL_CTL_SET_MASK, 1ull << 11));
    CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, self));
    line("NATIVE_WAKE_WAIT=PASS\n");
    return 0;
}
