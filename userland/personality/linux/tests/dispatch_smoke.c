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

    const char text[] = "hello";
    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_WRITE, 1, (uint64_t)(uintptr_t)text, 5, 0, 0, 0) == 5)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_FD_WRITE)) return 1;
    if (expect(g_last.a0 == 1 && g_last.a1 == (uint64_t)(uintptr_t)text && g_last.a2 == 5)) return 1;

    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_READ, 3, (uint64_t)(uintptr_t)text, 4, 0, 0, 0) == 4)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_FD_READ)) return 1;

    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_CLOSE, 3, 0, 0, 0, 0, 0) == 0)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_FD_CLOSE && g_last.a0 == 3)) return 1;

    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_ARCH_PRCTL, LPR_LINUX_ARCH_SET_FS, 0x12345000, 0, 0, 0, 0) == 0)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_THREAD_SET_FS_BASE && g_last.a0 == 0x12345000)) return 1;

    if (expect(lpr_dispatch_syscall(LPR_LINUX_SYS_GETRANDOM, (uint64_t)(uintptr_t)text, 7, 0, 0, 0, 0) == 7)) return 1;
    if (expect(g_last.nr == PACHAOS_SYSCALL_GETRANDOM)) return 1;

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
