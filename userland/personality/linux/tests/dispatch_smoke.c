#include <stdint.h>
#include "../runtime/lpr_linux_syscall.h"
#include <pachaos/abi.h>

struct mock_call {
    uint64_t nr;
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;
    uint64_t a4;
    uint64_t a5;
};

static struct mock_call g_last;

int64_t lpr_pacha_syscall0(uint64_t nr) {
    g_last.nr = nr;
    g_last.a0 = 0;
    g_last.a1 = 0;
    g_last.a2 = 0;
    if (nr == PACHAOS_SYSCALL_GETPID) return 1234;
    if (nr == PACHAOS_SYSCALL_GETTID) return 5678;
    return -1;
}

int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t a0) {
    g_last.nr = nr;
    g_last.a0 = a0;
    g_last.a1 = 0;
    g_last.a2 = 0;
    return 0;
}

int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1) {
    g_last.nr = nr;
    g_last.a0 = a0;
    g_last.a1 = a1;
    g_last.a2 = 0;
    if (nr == PACHAOS_SYSCALL_CLOCK_GETRES) {
        struct pachaos_timespec *out = (struct pachaos_timespec *)(uintptr_t)a1;
        if (a0 == PACHAOS_CLOCK_REALTIME) {
            out->tv_sec = 1;
            out->tv_nsec = 0;
            return 0;
        }
        if (a0 == PACHAOS_CLOCK_MONOTONIC) {
            out->tv_sec = 0;
            out->tv_nsec = 1000000;
            return 0;
        }
        return PACHAOS_SYSCALL_ERR_INVALID;
    }
    if (nr == PACHAOS_SYSCALL_PROCESS_SIGNAL_CTL &&
        a0 == PACHAOS_PROCESS_SIGNAL_CTL_GET_TIMER)
    {
        uint64_t *state = (uint64_t *)(uintptr_t)a1;
        state[0] = 14;
        state[1] = 1234;
        state[2] = 2000;
        return 0;
    }
    if (nr == PACHAOS_SYSCALL_FD_GET_INFO) return PACHAOS_SYSCALL_ERR_INVALID;
    return 0;
}

int64_t lpr_pacha_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    g_last.nr = nr;
    g_last.a0 = a0;
    g_last.a1 = a1;
    g_last.a2 = a2;
    if (nr == PACHAOS_SYSCALL_GETRANDOM) return (int64_t)a1;
    return (int64_t)a2;
}

int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    g_last.nr = nr;
    g_last.a0 = a0;
    g_last.a1 = a1;
    g_last.a2 = a2;
    g_last.a3 = a3;
    return 0;
}

int64_t lpr_pacha_syscall5(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    g_last.nr = nr;
    g_last.a0 = a0;
    g_last.a1 = a1;
    g_last.a2 = a2;
    g_last.a3 = a3;
    g_last.a4 = a4;
    if (nr == PACHAOS_SYSCALL_PROCESS_SIGNAL_CTL &&
        a0 == PACHAOS_PROCESS_SIGNAL_CTL_SET_TIMER && a4 != 0)
    {
        uint64_t *old = (uint64_t *)(uintptr_t)a4;
        old[0] = 14;
        old[1] = 7;
        old[2] = 3;
    }
    return 0;
}

int64_t lpr_pacha_syscall6(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    g_last.nr = nr;
    g_last.a0 = a0;
    g_last.a1 = a1;
    g_last.a2 = a2;
    g_last.a3 = a3;
    g_last.a4 = a4;
    g_last.a5 = a5;
    return 0x10000000;
}

static int expect(int condition) {
    return condition ? 0 : 1;
}

int main(void) {
    const struct lpr_linux_syscall_info *info = lpr_linux_syscall_lookup(LPR_LINUX_SYS_OPENAT);
    if (expect(info != 0)) return 1;
    if (expect(info->cls == LPR_LINUX_SYSCALL_CLASS_VFS_PATH)) return 1;
    if (expect(info->backend == LPR_LINUX_SYSCALL_BACKEND_FILED)) return 1;
    if (expect(lpr_linux_syscall_lookup(999999) == 0)) return 1;

    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_GETPID, 0, 0, 0, 0, 0, 0) == 1234)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_GETPID)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_GETTID, 0, 0, 0, 0, 0, 0) == 5678)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_GETTID)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_SET_TID_ADDRESS, 0x1000, 0, 0, 0, 0, 0) == 5678)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_GETTID)) return 1;

    uint64_t clock_result[2] = {0, 0};
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_CLOCK_GETTIME, 5,
            (uint64_t)(uintptr_t)clock_result, 0, 0, 0, 0) == 0)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_CLOCK_GETTIME && g_last.a0 == 0)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_CLOCK_GETTIME, 4,
            (uint64_t)(uintptr_t)clock_result, 0, 0, 0, 0) == 0)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_CLOCK_GETTIME && g_last.a0 == 1)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_CLOCK_GETTIME, 6,
            (uint64_t)(uintptr_t)clock_result, 0, 0, 0, 0) == 0)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_CLOCK_GETTIME && g_last.a0 == 1)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_CLOCK_GETTIME, 7,
            (uint64_t)(uintptr_t)clock_result, 0, 0, 0, 0) == 0)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_CLOCK_GETTIME && g_last.a0 == 1)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_CLOCK_GETTIME, 2,
            (uint64_t)(uintptr_t)clock_result, 0, 0, 0, 0) ==
            -LPR_LINUX_EINVAL)) return 1;

    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_CLOCK_GETRES, 0,
            (uint64_t)(uintptr_t)clock_result, 0, 0, 0, 0) == 0)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_CLOCK_GETRES &&
            g_last.a0 == PACHAOS_CLOCK_REALTIME && clock_result[0] == 1 &&
            clock_result[1] == 0)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_CLOCK_GETRES, 1,
            (uint64_t)(uintptr_t)clock_result, 0, 0, 0, 0) == 0)) return 1;
    if (expect(g_last.a0 == PACHAOS_CLOCK_MONOTONIC &&
            clock_result[0] == 0 && clock_result[1] == 1000000)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_CLOCK_GETRES, 4,
            0, 0, 0, 0, 0) == 0)) return 1;
    if (expect(g_last.a0 == PACHAOS_CLOCK_MONOTONIC)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_CLOCK_GETRES, 2,
            (uint64_t)(uintptr_t)clock_result, 0, 0, 0, 0) ==
            -LPR_LINUX_EINVAL)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_FADVISE64, 0, 0, 0,
            6, 0, 0) == -LPR_LINUX_EINVAL)) return 1;

    struct {
        int64_t interval_sec;
        int64_t interval_usec;
        int64_t value_sec;
        int64_t value_usec;
    } new_itimer = {0, 250000, 1, 500000}, old_itimer = {0};
    if (expect(lpr_dispatch_syscall(
            LPR_LINUX_SYS_SETITIMER,
            0,
            (uint64_t)(uintptr_t)&new_itimer,
            (uint64_t)(uintptr_t)&old_itimer,
            0, 0, 0) == 0)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_PROCESS_SIGNAL_CTL &&
            g_last.a0 == PACHAOS_PROCESS_SIGNAL_CTL_SET_TIMER &&
            g_last.a1 == 14 && g_last.a2 == 1500 && g_last.a3 == 250)) return 1;
    if (expect(old_itimer.interval_sec == 0 &&
            old_itimer.interval_usec == 3000 &&
            old_itimer.value_sec == 0 &&
            old_itimer.value_usec == 7000)) return 1;
    if (expect(lpr_dispatch_syscall(
            LPR_LINUX_SYS_SETITIMER,
            1,
            (uint64_t)(uintptr_t)&new_itimer,
            0, 0, 0, 0) == -LPR_LINUX_EINVAL)) return 1;
    old_itimer.interval_sec = 0;
    old_itimer.interval_usec = 0;
    old_itimer.value_sec = 0;
    old_itimer.value_usec = 0;
    if (expect(lpr_dispatch_syscall(
            LPR_LINUX_SYS_GETITIMER,
            0,
            (uint64_t)(uintptr_t)&old_itimer,
            0, 0, 0, 0) == 0)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_PROCESS_SIGNAL_CTL &&
            g_last.a0 == PACHAOS_PROCESS_SIGNAL_CTL_GET_TIMER)) return 1;
    if (expect(old_itimer.interval_sec == 2 &&
            old_itimer.interval_usec == 0 &&
            old_itimer.value_sec == 1 &&
            old_itimer.value_usec == 234000)) return 1;
    uint64_t suspend_mask = 0;
    if (expect(lpr_dispatch_syscall(
            LPR_LINUX_SYS_RT_SIGSUSPEND,
            (uint64_t)(uintptr_t)&suspend_mask,
            16, 0, 0, 0, 0) == -LPR_LINUX_EINVAL)) return 1;

    const char text[] = "hello";
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_WRITE, 1, (uint64_t)(uintptr_t)text, 5, 0, 0, 0) == 5)) return 1;

    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_READ, 3, (uint64_t)(uintptr_t)text, 4, 0, 0, 0) == 4)) return 1;

    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_ARCH_PRCTL, LPR_LINUX_ARCH_SET_FS, 0x12345000, 0, 0, 0, 0) == 0)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_THREAD_SET_FS_BASE && g_last.a0 == 0x12345000)) return 1;

    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_GETRANDOM, (uint64_t)(uintptr_t)text, 7, 0, 0, 0, 0) == 7)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_GETRANDOM)) return 1;

    info = lpr_linux_syscall_lookup(LPR_LINUX_SYS_MEMBARRIER);
    if (expect(info != 0)) return 1;
    if (expect(info->cls == LPR_LINUX_SYSCALL_CLASS_MEMORY)) return 1;
    if (expect(info->backend == LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_MEMBARRIER, 1ull << 3, 0, 0, 0, 0, 0) == -LPR_LINUX_EPERM)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_MEMBARRIER, 0, 0, 0, 0, 0, 0) == ((1ull << 3) | (1ull << 4)))) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_MEMBARRIER, 1ull << 4, 0, 0, 0, 0, 0) == 0)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_MEMBARRIER, 1ull << 3, 1, 0, 0, 0, 0) == -LPR_LINUX_EINVAL)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_MEMBARRIER, 1ull << 3, 0, 0, 0, 0, 0) == 0)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_PROCESS_MEMORY_BARRIER &&
               g_last.a0 == PACHAOS_PROCESS_MEMORY_BARRIER_FLAG_NONE)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_MEMBARRIER, 1ull << 7, 0, 0, 0, 0, 0) == -LPR_LINUX_EINVAL)) return 1;

    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_BRK, 0, 0, 0, 0, 0, 0) == 0x10000000)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_MMAP)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_BRK, 0x10002000, 0, 0, 0, 0, 0) == 0x10002000)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_BRK, 0x20000000, 0, 0, 0, 0, 0) == 0x10002000)) return 1;

    char cwd[8] = {0};
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_GETCWD, (uint64_t)(uintptr_t)cwd, sizeof(cwd), 0, 0, 0, 0) == 2)) return 1;
    if (expect(cwd[0] == '/' && cwd[1] == '\0')) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_GETCWD, (uint64_t)(uintptr_t)cwd, 1, 0, 0, 0, 0) == -LPR_LINUX_ERANGE)) return 1;

    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_MMAP, 0, 4096, 3, 0x22, UINT64_MAX, 0) == 0x10000000)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_MMAP)) return 1;

    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_OPENAT, (uint64_t)(int64_t)-100, (uint64_t)(uintptr_t)"/", 0, 0, 0, 0) == -LPR_LINUX_ENOSYS)) return 1;
    if (expect(lpr_dispatch_syscall(999999, 0, 0, 0, 0, 0, 0) == -LPR_LINUX_ENOSYS)) return 1;
    return 0;
}
