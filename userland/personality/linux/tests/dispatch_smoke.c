#include <stdint.h>
#include "../runtime/lpr_linux_syscall.h"
#include <pachaos/abi.h>

struct mock_call {
    uint64_t nr;
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
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

int64_t lpr_pacha_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    g_last.nr = nr;
    g_last.a0 = a0;
    g_last.a1 = a1;
    g_last.a2 = a2;
    return (int64_t)a2;
}

static int expect(int condition) {
    return condition ? 0 : 1;
}

int main(void) {
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_GETPID, 0, 0, 0, 0, 0, 0) == 1234)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_GETPID)) return 1;
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_GETTID, 0, 0, 0, 0, 0, 0) == 5678)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_GETTID)) return 1;

    const char text[] = "hello";
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_WRITE, 1, (uint64_t)(uintptr_t)text, 5, 0, 0, 0) == 5)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_FD_WRITE)) return 1;
    if (expect(g_last.a0 == 1 && g_last.a1 == (uint64_t)(uintptr_t)text && g_last.a2 == 5)) return 1;

    if (expect(lpr_dispatch_syscall(999999, 0, 0, 0, 0, 0, 0) == -LPR_LINUX_ENOSYS)) return 1;
    return 0;
}
