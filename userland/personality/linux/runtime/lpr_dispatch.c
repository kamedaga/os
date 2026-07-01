#include "lpr_linux_syscall.h"
#include "lpr_filed.h"
#include "lpr_memory.h"
#include "lpr_vfs_local.h"
#include "support/syscall.h"
#include <pachaos/abi.h>

#define LPR_LINUX_PROT_READ 0x1ull
#define LPR_LINUX_PROT_WRITE 0x2ull
#define LPR_LINUX_PROT_EXEC 0x4ull
#define LPR_LINUX_MAP_SHARED 0x01ull
#define LPR_LINUX_MAP_PRIVATE 0x02ull
#define LPR_LINUX_MAP_FIXED 0x10ull
#define LPR_LINUX_MAP_ANONYMOUS 0x20ull
#define LPR_LINUX_MAP_NORESERVE 0x4000ull
#define LPR_LINUX_MAP_FIXED_NOREPLACE 0x100000ull
#define LPR_LINUX_AT_FDCWD ((uint64_t)(int64_t)-100)
#define LPR_LINUX_AT_REMOVEDIR 0x200ull

static uint64_t lpr_page_align_up(uint64_t value)
{
    const uint64_t mask = 4095ull;
    if (value > UINT64_MAX - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

static int64_t lpr_linux_pacha_status_to_errno(int64_t status)
{
    if (status == 0) {
        return status;
    }
    int negative = 0;
    if (status < 0) {
        negative = 1;
        status = -status;
    }
    if (status > PACHAOS_SYSCALL_ERR_EMPTY) {
        return negative ? -status : status;
    }
    switch (status) {
    case PACHAOS_SYSCALL_ERR_INVALID:
        return -LPR_LINUX_EINVAL;
    case PACHAOS_SYSCALL_ERR_ALLOC:
    case PACHAOS_SYSCALL_ERR_MAP:
        return -LPR_LINUX_ENOMEM;
    case PACHAOS_SYSCALL_ERR_NOT_READY:
    case PACHAOS_SYSCALL_ERR_EMPTY:
        return -LPR_LINUX_EAGAIN;
    default:
        return -LPR_LINUX_EINVAL;
    }
}

static uint64_t lpr_linux_prot_to_pacha(uint64_t prot)
{
    uint64_t out = 0;
    if ((prot & LPR_LINUX_PROT_READ) != 0) {
        out |= PACHAOS_PROT_READ;
    }
    if ((prot & LPR_LINUX_PROT_WRITE) != 0) {
        out |= PACHAOS_PROT_WRITE;
    }
    if ((prot & LPR_LINUX_PROT_EXEC) != 0) {
        out |= PACHAOS_PROT_EXEC;
    }
    return out;
}

static int lpr_linux_mmap_flags_to_pacha(uint64_t flags, uint64_t *out)
{
    uint64_t pacha = 0;
    const int shared = (flags & LPR_LINUX_MAP_SHARED) != 0;
    const int private = (flags & LPR_LINUX_MAP_PRIVATE) != 0;
    if (out == 0 || shared == private) {
        return -LPR_LINUX_EINVAL;
    }
    if (shared) {
        pacha |= PACHAOS_MMAP_SHARED;
    }
    if (private) {
        pacha |= PACHAOS_MMAP_PRIVATE;
    }
    if ((flags & LPR_LINUX_MAP_FIXED) != 0) {
        pacha |= PACHAOS_MMAP_FIXED;
    }
    if ((flags & LPR_LINUX_MAP_FIXED_NOREPLACE) != 0) {
        pacha |= PACHAOS_MMAP_FIXED_NOREPLACE;
    }
    if ((flags & LPR_LINUX_MAP_ANONYMOUS) != 0) {
        pacha |= PACHAOS_MMAP_ANONYMOUS;
    }
    if ((flags & LPR_LINUX_MAP_NORESERVE) != 0) {
        pacha |= PACHAOS_MMAP_NORESERVE;
    }
    *out = pacha;
    return 0;
}

static int64_t lpr_dispatch_arch_prctl(uint64_t code, uint64_t value)
{
    switch (code) {
    case LPR_LINUX_ARCH_SET_FS:
        return lpr_linux_pacha_status_to_errno(lpr_pacha_syscall1(PACHAOS_SYSCALL_THREAD_SET_FS_BASE, value));
    case LPR_LINUX_ARCH_SET_GS:
        return lpr_linux_pacha_status_to_errno(lpr_pacha_syscall1(PACHAOS_SYSCALL_THREAD_SET_GS_BASE, value));
    case LPR_LINUX_ARCH_GET_FS:
    case LPR_LINUX_ARCH_GET_GS:
        return -LPR_LINUX_ENOSYS;
    default:
        return -LPR_LINUX_EINVAL;
    }
}

static int64_t lpr_dispatch_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset)
{
    uint64_t pacha_flags;
    const int flag_status = lpr_linux_mmap_flags_to_pacha(flags, &pacha_flags);
    if (flag_status != 0 || len == 0) {
        return -LPR_LINUX_EINVAL;
    }
    if ((flags & LPR_LINUX_MAP_ANONYMOUS) == 0 && lpr_linux_filed_fd_active(fd)) {
        if ((flags & LPR_LINUX_MAP_SHARED) != 0) {
            return -LPR_LINUX_ENOTSUP;
        }
        if ((offset & 4095ull) != 0) {
            return -LPR_LINUX_EINVAL;
        }
        const uint64_t map_len = lpr_page_align_up(len);
        if (map_len == 0) {
            return -LPR_LINUX_ENOMEM;
        }
        const uint64_t load_prot =
            PACHAOS_PROT_READ |
            PACHAOS_PROT_WRITE |
            (lpr_linux_prot_to_pacha(prot) & PACHAOS_PROT_EXEC);
        const int64_t mapped = lpr_pacha_syscall6(
            PACHAOS_SYSCALL_MMAP,
            0,
            addr,
            map_len,
            load_prot,
            (pacha_flags | PACHAOS_MMAP_ANONYMOUS) & ~PACHAOS_MMAP_SHARED,
            0);
        if (mapped < 4096) {
            return lpr_linux_pacha_status_to_errno(mapped);
        }
        uint64_t done = 0;
        while (done < len) {
            const uint64_t chunk = len - done > 7680ull ? 7680ull : len - done;
            const int64_t bytes = lpr_linux_pread64(
                fd,
                (uint64_t)mapped + done,
                chunk,
                offset + done);
            if (bytes < 0) {
                (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)mapped, map_len);
                return bytes;
            }
            if (bytes == 0) {
                break;
            }
            done += (uint64_t)bytes;
            if ((uint64_t)bytes < chunk) {
                break;
            }
        }
        const int64_t protect_status = lpr_pacha_syscall3(
            PACHAOS_SYSCALL_MPROTECT,
            (uint64_t)mapped,
            map_len,
            lpr_linux_prot_to_pacha(prot));
        if (protect_status != 0) {
            (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)mapped, map_len);
            return lpr_linux_pacha_status_to_errno(protect_status);
        }
        return mapped;
    }
    const uint64_t pacha_fd = (flags & LPR_LINUX_MAP_ANONYMOUS) != 0 ? 0 : fd;
    const int64_t ret = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        pacha_fd,
        addr,
        len,
        lpr_linux_prot_to_pacha(prot),
        pacha_flags,
        offset);
    return lpr_linux_pacha_status_to_errno(ret);
}

int64_t lpr_dispatch_syscall(uint64_t nr,
                             uint64_t a0,
                             uint64_t a1,
                             uint64_t a2,
                             uint64_t a3,
                             uint64_t a4,
                             uint64_t a5) {
    switch (nr) {
    case LPR_LINUX_SYS_READ:
        return lpr_linux_read(a0, a1, a2);
    case LPR_LINUX_SYS_GETPID:
        return lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID);
    case LPR_LINUX_SYS_GETTID:
        return lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID);
    case LPR_LINUX_SYS_WRITE:
        return lpr_linux_write(a0, a1, a2);
    case LPR_LINUX_SYS_OPEN:
        return lpr_linux_openat(LPR_LINUX_AT_FDCWD, a0, a1, a2);
    case LPR_LINUX_SYS_CLOSE:
        return lpr_linux_close(a0);
    case LPR_LINUX_SYS_STAT:
        return lpr_linux_newfstatat(LPR_LINUX_AT_FDCWD, a0, a1, 0);
    case LPR_LINUX_SYS_LSEEK:
        return lpr_linux_lseek(a0, a1, a2);
    case LPR_LINUX_SYS_LSTAT:
        return lpr_linux_newfstatat(LPR_LINUX_AT_FDCWD, a0, a1, 0x100);
    case LPR_LINUX_SYS_MMAP:
        return lpr_dispatch_mmap(a0, a1, a2, a3, a4, a5);
    case LPR_LINUX_SYS_MPROTECT:
        return lpr_linux_pacha_status_to_errno(
            lpr_pacha_syscall3(PACHAOS_SYSCALL_MPROTECT, a0, a1, lpr_linux_prot_to_pacha(a2)));
    case LPR_LINUX_SYS_MUNMAP:
        return lpr_linux_pacha_status_to_errno(lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, a0, a1));
    case LPR_LINUX_SYS_ARCH_PRCTL:
        return lpr_dispatch_arch_prctl(a0, a1);
    case LPR_LINUX_SYS_CLOCK_GETTIME:
        return lpr_linux_pacha_status_to_errno(lpr_pacha_syscall2(PACHAOS_SYSCALL_CLOCK_GETTIME, a0, a1));
    case LPR_LINUX_SYS_GETRANDOM:
        return lpr_pacha_syscall3(PACHAOS_SYSCALL_GETRANDOM, a0, a1, a2);
    case LPR_LINUX_SYS_EXIT:
    case LPR_LINUX_SYS_EXIT_GROUP:
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, a0);
        for (;;) {
        }
    case LPR_LINUX_SYS_BRK:
        return lpr_linux_brk(a0);
    case LPR_LINUX_SYS_IOCTL:
        return lpr_linux_ioctl(a0, a1, a2);
    case LPR_LINUX_SYS_FSTAT:
        return lpr_linux_fstat(a0, a1);
    case LPR_LINUX_SYS_FSYNC:
    case LPR_LINUX_SYS_FDATASYNC:
        return lpr_linux_fsync(a0);
    case LPR_LINUX_SYS_PREAD64:
        return lpr_linux_pread64(a0, a1, a2, a3);
    case LPR_LINUX_SYS_READV:
        return lpr_linux_readv(a0, a1, a2);
    case LPR_LINUX_SYS_WRITEV:
        return lpr_linux_writev(a0, a1, a2);
    case LPR_LINUX_SYS_ACCESS:
        return lpr_linux_access(a0, a1);
    case LPR_LINUX_SYS_READLINK:
        return lpr_linux_readlink(a0, a1, a2);
    case LPR_LINUX_SYS_CHMOD:
        return lpr_linux_fchmodat(LPR_LINUX_AT_FDCWD, a0, a1, 0);
    case LPR_LINUX_SYS_FCHMOD:
        return lpr_linux_fchmod(a0, a1);
    case LPR_LINUX_SYS_RENAME:
        return lpr_linux_renameat(LPR_LINUX_AT_FDCWD, a0, LPR_LINUX_AT_FDCWD, a1);
    case LPR_LINUX_SYS_MKDIR:
        return lpr_linux_mkdirat(LPR_LINUX_AT_FDCWD, a0, a1);
    case LPR_LINUX_SYS_RMDIR:
        return lpr_linux_unlinkat(LPR_LINUX_AT_FDCWD, a0, LPR_LINUX_AT_REMOVEDIR);
    case LPR_LINUX_SYS_UNLINK:
        return lpr_linux_unlinkat(LPR_LINUX_AT_FDCWD, a0, 0);
    case LPR_LINUX_SYS_GETDENTS64:
        return lpr_linux_getdents64(a0, a1, a2);
    case LPR_LINUX_SYS_OPENAT:
        return lpr_linux_openat(a0, a1, a2, a3);
    case LPR_LINUX_SYS_MKDIRAT:
        return lpr_linux_mkdirat(a0, a1, a2);
    case LPR_LINUX_SYS_NEWFSTATAT:
        return lpr_linux_newfstatat(a0, a1, a2, a3);
    case LPR_LINUX_SYS_UNLINKAT:
        return lpr_linux_unlinkat(a0, a1, a2);
    case LPR_LINUX_SYS_RENAMEAT:
        return lpr_linux_renameat(a0, a1, a2, a3);
    case LPR_LINUX_SYS_FCHMODAT:
        return lpr_linux_fchmodat(a0, a1, a2, a3);
    case LPR_LINUX_SYS_UTIMENSAT:
        return lpr_linux_utimensat(a0, a1, a2, a3);
    case LPR_LINUX_SYS_GETCWD:
        return lpr_linux_getcwd(a0, a1);
    case LPR_LINUX_SYS_FCNTL:
        return lpr_linux_fcntl(a0, a1, a2);
    default:
        return -LPR_LINUX_ENOSYS;
    }
}
