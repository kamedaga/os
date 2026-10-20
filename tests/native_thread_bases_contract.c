/* SPDX-License-Identifier: MIT */
/* Distinct bases and PKRU must survive initial dispatch and blocked resumes. */
#include <stdint.h>
#include <stddef.h>
#include <pacha/abi.h>
#include <pacha/ipc.h>
#include <pacha/syscall.h>

static uint64_t markers[2][2] = {
    {0x1122334455667788ull, 0x8877665544332211ull},
    {0xaabbccddeeff0011ull, 0x1100ffeeddccbbaaull}
};
static unsigned char stacks[2][32768] __attribute__((aligned(4096)));
static unsigned pke;
static unsigned done[2];
static unsigned turn, competitor_ready, competitor_stop;
static uint64_t competitor_progress;
static unsigned char competitor_stack[32768] __attribute__((aligned(4096)));

static uint64_t sleep_tick(void) {
    const uint64_t delay[2] = {0, 1000000};
    return pacha_syscall2(PACHA_RUNTIME_SYSCALL_NANOSLEEP, (uintptr_t)delay, 0);
}

static void line(const char *s) {
    size_t n = 0;
    while (s[n]) ++n;
    (void)pacha_syscall2(1, (uintptr_t)s, n);
}

static unsigned has_pke(void) {
    unsigned eax = 7, ebx, ecx = 0, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    return (ecx & ((1u << 3) | (1u << 4))) == ((1u << 3) | (1u << 4));
}

static unsigned read_pkru(void) {
    unsigned eax, edx;
    __asm__ volatile("rdpkru" : "=a"(eax), "=d"(edx) : "c"(0));
    return eax;
}

static int bases_ok(unsigned i) {
    uint64_t fs, gs;
    __asm__ volatile("mov %%fs:0, %0; mov %%gs:0, %1"
        : "=r"(fs), "=r"(gs) :: "memory");
    return fs == markers[i][0] && gs == markers[i][1] &&
        (!pke || read_pkru() == (i ? 0x80000000u : 0x40000000u));
}

__attribute__((used, noreturn)) static void bases_worker(unsigned i) {
    for (unsigned round = 0; round < 64; ++round) {
        if (!bases_ok(i)) goto failed;
        /* A nonzero native sleep blocks and redispatches the actual thread;
         * checking a syscall-only loop would not exercise AP idle entry. */
        if (sleep_tick()) goto failed;
        if (!bases_ok(i)) goto failed;
    }
    for (unsigned round = 0; round < 512; ++round) {
        unsigned waits = 0;
        while (__atomic_load_n(&turn, __ATOMIC_ACQUIRE) != i) {
            const uint64_t rc = pacha_syscall4(PACHA_RUNTIME_SYSCALL_FUTEX_WAIT,
                (uintptr_t)&turn, 1 - i, 20, 0);
            if ((rc != 0 && rc != PACHA_SYSCALL_ERR_NOT_READY &&
                 rc != PACHA_FUTEX_WAIT_TIMED_OUT_STATUS) || ++waits > 1000) goto failed;
        }
        if (!bases_ok(i)) goto failed;
        __atomic_store_n(&turn, 1 - i, __ATOMIC_RELEASE);
        /* A successful handoff returns the receiver's frame now and must
         * preserve this sender's wake count for its later syscall return. */
        const uint64_t woke = pacha_syscall2(PACHA_RUNTIME_SYSCALL_FUTEX_WAKE,
            (uintptr_t)&turn, 1);
        if (woke > 1 || !bases_ok(i)) goto failed;
    }
    __atomic_store_n(&done[i], 1, __ATOMIC_RELEASE);
    (void)pacha_syscall3(PACHA_THREAD_SYSCALL_EXIT, 0, 0, 0);
    __builtin_trap();
failed:
    line("NATIVE_THREAD_BASES=FAIL\n");
    (void)pacha_syscall3(PACHA_THREAD_SYSCALL_EXIT, 1, 0, 0);
    __builtin_trap();
}

__attribute__((used, noreturn)) static void bases_competitor(void) {
    __atomic_store_n(&competitor_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&competitor_stop, __ATOMIC_ACQUIRE))
        __atomic_fetch_add(&competitor_progress, 1, __ATOMIC_RELAXED);
    (void)pacha_syscall3(PACHA_THREAD_SYSCALL_EXIT, 0, 0, 0);
    __builtin_trap();
}

extern void bases_worker0(void), bases_worker1(void), bases_competitor_entry(void);
__asm__(".text\n"
    ".global bases_worker0, bases_worker1\n"
    "bases_worker0: xor %edi, %edi; call bases_worker; ud2\n"
    "bases_worker1: mov $1, %edi; call bases_worker; ud2\n"
    ".global bases_competitor_entry\n"
    "bases_competitor_entry: call bases_competitor; ud2\n");

int native_thread_bases_contract(void) {
    uint64_t fd[2];
    pke = has_pke();
    const uint64_t competitor = pacha_syscall6(PACHA_THREAD_SYSCALL_CREATE,
        PACHA_PROCESS_SELF_FD, (uintptr_t)bases_competitor_entry,
        (uintptr_t)(competitor_stack + sizeof(competitor_stack)), 0, 0,
        PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE);
    if (competitor < 16 || pacha_syscall1(PACHA_THREAD_SYSCALL_START, competitor)) goto failed;
    while (!__atomic_load_n(&competitor_ready, __ATOMIC_ACQUIRE))
        if (sleep_tick()) goto failed;
    const uint64_t progress_before = __atomic_load_n(&competitor_progress, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < 2; ++i) {
        fd[i] = pacha_syscall6(PACHA_THREAD_SYSCALL_CREATE, PACHA_PROCESS_SELF_FD,
            (uintptr_t)(i ? bases_worker1 : bases_worker0),
            (uintptr_t)(stacks[i] + sizeof(stacks[i])), 0, 0,
            PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_SET_CONTEXT);
        if (fd[i] < 16) goto failed;
        struct pacha_thread_context image;
        if (pacha_syscall4(PACHA_THREAD_SYSCALL_CONTEXT, fd[i], PACHA_THREAD_CONTEXT_GET,
            (uintptr_t)&image, sizeof(image))) goto failed;
        image.fs_base = (uintptr_t)&markers[i][0];
        image.gs_base = (uintptr_t)&markers[i][1];
        /* Only unused key 15 differs; mappings used by this test use key 0.
         * Nonzero PKRU is tested only when CPUID reports OS-enabled PKE. */
        image.pkru = pke ? (i ? 0x80000000u : 0x40000000u) : 0;
        if (pacha_syscall4(PACHA_THREAD_SYSCALL_CONTEXT, fd[i], PACHA_THREAD_CONTEXT_SET,
            (uintptr_t)&image, sizeof(image))) goto failed;
    }
    for (unsigned i = 0; i < 2; ++i)
        if (pacha_syscall1(PACHA_THREAD_SYSCALL_START, fd[i])) goto failed;
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t status[4], rc;
        unsigned tries = 0;
        while ((rc = pacha_syscall2(PACHA_THREAD_SYSCALL_WAIT, fd[i],
                   (uintptr_t)status)) == PACHA_SYSCALL_ERR_NOT_READY) {
            if (++tries > 2000 || sleep_tick()) goto failed;
        }
        if (rc || status[0] != 2 || status[1] ||
            !__atomic_load_n(&done[i], __ATOMIC_ACQUIRE) ||
            pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, fd[i])) goto failed;
    }
    if (__atomic_load_n(&competitor_progress, __ATOMIC_RELAXED) <= progress_before) goto failed;
    __atomic_store_n(&competitor_stop, 1, __ATOMIC_RELEASE);
    uint64_t status[4], rc;
    unsigned tries = 0;
    while ((rc = pacha_syscall2(PACHA_THREAD_SYSCALL_WAIT, competitor,
               (uintptr_t)status)) == PACHA_SYSCALL_ERR_NOT_READY)
        if (++tries > 2000 || sleep_tick()) goto failed;
    if (rc || status[0] != 2 || status[1] ||
        pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, competitor)) goto failed;
    line("NATIVE_FUTEX_HANDOFF=PASS\n");
    line(pke ? "NATIVE_THREAD_PKRU=PASS\n" : "NATIVE_THREAD_PKRU=UNSUPPORTED\n");
    line("NATIVE_THREAD_BASES=PASS\n");
    return 0;
failed:
    line("NATIVE_THREAD_BASES=FAIL\n");
    return 1;
}
