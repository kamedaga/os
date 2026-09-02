#include "compat.h"

#include <pacha/status.h>

enum {
    LPR_LINUX_PR_SET_NO_NEW_PRIVS = 38u,
    LPR_LINUX_PR_GET_NO_NEW_PRIVS = 39u,

    LPR_LINUX_CLONE_FS = 0x00000200ull,
    LPR_LINUX_CLONE_FILES = 0x00000400ull,
    LPR_LINUX_CLONE_NEWNS = 0x00020000ull,
    LPR_LINUX_CLONE_SYSVSEM = 0x00040000ull,
    LPR_LINUX_CLONE_NEWCGROUP = 0x02000000ull,
    LPR_LINUX_CLONE_NEWUTS = 0x04000000ull,
    LPR_LINUX_CLONE_NEWIPC = 0x08000000ull,
    LPR_LINUX_CLONE_NEWUSER = 0x10000000ull,
    LPR_LINUX_CLONE_NEWPID = 0x20000000ull,
    LPR_LINUX_CLONE_NEWNET = 0x40000000ull,
};

#define LPR_LINUX_CLONE_NAMESPACE_FLAGS \
    (LPR_LINUX_CLONE_NEWNS | LPR_LINUX_CLONE_NEWCGROUP | \
     LPR_LINUX_CLONE_NEWUTS | LPR_LINUX_CLONE_NEWIPC | \
     LPR_LINUX_CLONE_NEWUSER | LPR_LINUX_CLONE_NEWPID | \
     LPR_LINUX_CLONE_NEWNET)

int64_t lpr_linux_prctl_compat(uint64_t option,
                               uint64_t arg2,
                               uint64_t arg3,
                               uint64_t arg4,
                               uint64_t arg5)
{
    switch (option) {
    case LPR_LINUX_PR_SET_NO_NEW_PRIVS:
        if (arg2 != 1u || arg3 != 0 || arg4 != 0 || arg5 != 0) {
            return -LPR_LINUX_EINVAL;
        }
        /* Linux personalities cannot acquire Unix credentials or file
         * capabilities on CapabilityOS.  The no-new-privileges invariant is
         * therefore already permanent, including across fork and exec. */
        return 0;
    case LPR_LINUX_PR_GET_NO_NEW_PRIVS:
        if (arg2 != 0 || arg3 != 0 || arg4 != 0 || arg5 != 0) {
            return -LPR_LINUX_EINVAL;
        }
        return 1;
    default:
        return -LPR_LINUX_EINVAL;
    }
}

int64_t lpr_linux_clone_namespace_guard(uint64_t flags)
{
    /* Namespace isolation is not implemented.  Returning success or treating
     * these bits as an ordinary fork would falsely tell sandbox launchers that
     * isolation exists.  EPERM is the Linux-compatible capability failure and
     * lets callers such as bubblewrap select their documented fallback. */
    return (flags & LPR_LINUX_CLONE_NAMESPACE_FLAGS) != 0
        ? -LPR_LINUX_EPERM
        : 0;
}

int64_t lpr_linux_unshare(uint64_t flags)
{
    if (flags == 0) {
        return 0;
    }
    if ((flags & LPR_LINUX_CLONE_NAMESPACE_FLAGS) != 0) {
        return -LPR_LINUX_EPERM;
    }

    const uint64_t known_non_namespace_flags =
        LPR_LINUX_CLONE_FS |
        LPR_LINUX_CLONE_FILES |
        LPR_LINUX_CLONE_SYSVSEM;
    if ((flags & ~known_non_namespace_flags) != 0) {
        return -LPR_LINUX_EINVAL;
    }

    /* These flags require changing process-local sharing topology.  LPR has no
     * equivalent operation yet, so report that instead of silently succeeding. */
    return -LPR_LINUX_ENOSYS;
}
