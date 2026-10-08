/* SPDX-License-Identifier: MIT */
/* Oversubscribe native CPUs without cooperative yields during observation. */
#include <stdint.h>
#include <stddef.h>
#include <pacha/abi.h>
#include <pacha/ipc.h>
#include <pacha/syscall.h>

#define WORKERS 8
static unsigned char stacks[WORKERS][32768] __attribute__((aligned(4096)));
static unsigned ready, phase, stop;
static uint64_t progress[WORKERS], window_progress[2][WORKERS], observed[2][256];

static void line(const char *s)
{
    size_t n = 0;
    while (s[n]) ++n;
    (void)pacha_syscall2(1, (uintptr_t)s, n);
}

static void number(uint64_t value)
{
    char digits[24]; unsigned n = 0;
    do { digits[n++] = '0' + value % 10; value /= 10; } while (value);
    for (unsigned i = 0; i < n / 2; ++i) {
        char c = digits[i]; digits[i] = digits[n - i - 1]; digits[n - i - 1] = c;
    }
    pacha_syscall2(1, (uintptr_t)digits, n);
}

static uint64_t now(void)
{
    uint64_t ts[2];
    if (pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME, 1, (uintptr_t)ts))
        return UINT64_MAX;
    return ts[0] * 1000000000ull + ts[1];
}

__attribute__((used, noreturn)) static void slice_worker(unsigned i)
{
    __atomic_fetch_add(&ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        for (unsigned step = 0; step < 4096; ++step) __asm__ volatile("pause");
        const unsigned interval = __atomic_load_n(&phase, __ATOMIC_ACQUIRE);
        if (interval) {
            unsigned eax = 1, ebx, ecx, edx;
            __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
            /* These boot-only QEMU tests use APIC IDs 0..3. Observing two
             * distinct busy workers on one AP requires an actual switch. */
            __atomic_fetch_or(&observed[interval - 1][ebx >> 24],
                1ull << i, __ATOMIC_RELAXED);
            __atomic_fetch_add(&progress[i], 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&window_progress[interval - 1][i], 1, __ATOMIC_RELAXED);
        }
    }
    /* EXIT consumes clear-tid/flags as well as the code. CPUID deliberately
     * leaves feature bits in RDX; a one-argument wrapper would pass them as
     * flags and correctly be rejected instead of terminating this thread. */
    (void)pacha_syscall3(PACHA_THREAD_SYSCALL_EXIT, 0, 0, 0);
    __builtin_trap();
}

extern void slice_worker0(void), slice_worker1(void), slice_worker2(void), slice_worker3(void);
extern void slice_worker4(void), slice_worker5(void), slice_worker6(void), slice_worker7(void);
#define ENTRY(n) ".global slice_worker" #n "\nslice_worker" #n ": mov $" #n ", %edi; call slice_worker; ud2\n"
__asm__(".text\n" ENTRY(0) ENTRY(1) ENTRY(2) ENTRY(3) ENTRY(4) ENTRY(5) ENTRY(6) ENTRY(7));

static int wait_for(unsigned target)
{
    const uint64_t deadline = now() + 2000000000ull;
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != target)
        if (now() >= deadline) return 1;
    return 0;
}

int native_ap_slice_contract(void)
{
    unsigned failed_line = 0;
#define FAIL() do { failed_line = __LINE__; goto failed; } while (0)
    pacha_system_info_t info = {0};
    if (pacha_syscall1(PACHA_RUNTIME_SYSCALL_SYSTEM_INFO, (uintptr_t)&info) ||
        (info.online_cpu_count != 2 && info.online_cpu_count != 4)) FAIL();
    const unsigned count = (unsigned)info.online_cpu_count * 2;
    void (*const entries[WORKERS])(void) = { slice_worker0, slice_worker1,
        slice_worker2, slice_worker3, slice_worker4, slice_worker5, slice_worker6, slice_worker7 };
    uint64_t fd[WORKERS];
    for (unsigned i = 0; i < count; ++i) {
        fd[i] = pacha_syscall6(PACHA_THREAD_SYSCALL_CREATE, PACHA_PROCESS_SELF_FD,
            (uintptr_t)entries[i], (uintptr_t)(stacks[i] + sizeof(stacks[i])), 0, 0,
            PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE);
        if (fd[i] < 16 || pacha_syscall1(PACHA_THREAD_SYSCALL_START, fd[i])) FAIL();
    }
    if (wait_for(count)) FAIL();
    /* The second window excludes initial START IPIs and demands continued
     * preemption, not just progress achieved while constructing the tasks. */
    for (unsigned interval = 1; interval <= 2; ++interval) {
        __atomic_store_n(&phase, interval, __ATOMIC_RELEASE);
        const uint64_t deadline = now() + 150000000ull;
        while (now() < deadline) __asm__ volatile("pause");
    }
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < count; ++i) {
        uint64_t status[4], rc;
        const uint64_t deadline = now() + 2000000000ull;
        do {
            rc = pacha_syscall2(PACHA_THREAD_SYSCALL_WAIT, fd[i], (uintptr_t)status);
            if (now() >= deadline) FAIL();
        } while (rc == PACHA_SYSCALL_ERR_NOT_READY);
        if (rc || status[0] != 2 || status[1] ||
            !__atomic_load_n(&progress[i], __ATOMIC_RELAXED) ||
            pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, fd[i])) FAIL();
    }
    for (unsigned interval = 0; interval < 2; ++interval) {
        unsigned switching_aps = 0;
        for (unsigned i = 0; i < count; ++i)
            if (!__atomic_load_n(&window_progress[interval][i], __ATOMIC_RELAXED)) FAIL();
        for (unsigned cpu = 1; cpu < info.online_cpu_count; ++cpu) {
            const uint64_t mask = __atomic_load_n(&observed[interval][cpu], __ATOMIC_RELAXED);
            if (!mask) FAIL();
            switching_aps += (mask & (mask - 1)) != 0;
        }
        // START may move a round-robin-created thread to an idle CPU. Stealing
        // only helps idle CPUs, so 1/3/1/3 is legal and is not proof that an AP
        // missed preemption. Require every worker in both windows and actual
        // noncooperative switching on an AP, without assuming equal placement.
        if (!switching_aps) FAIL();
    }
    line("NATIVE_AP_SLICE=PASS\n");
    return 0;
failed:
    // Preserve the violated condition and CPU observations; an occasional
    // scheduler regression must not be hidden by retrying a generic FAIL.
    line("NATIVE_AP_SLICE_DIAGNOSTIC line="); number(failed_line);
    line(" ready="); number(__atomic_load_n(&ready, __ATOMIC_ACQUIRE));
    line(" masks=");
    for (unsigned interval = 0; interval < 2; ++interval)
        for (unsigned cpu = 0; cpu < 4; ++cpu) {
            number(__atomic_load_n(&observed[interval][cpu], __ATOMIC_RELAXED)); line(",");
        }
    line("\nNATIVE_AP_SLICE=FAIL\n");
    return 1;
}
