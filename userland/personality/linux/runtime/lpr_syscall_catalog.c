#include "lpr_linux_syscall.h"

const char *lpr_linux_syscall_class_name(enum lpr_linux_syscall_class cls)
{
    switch (cls) {
    case LPR_LINUX_SYSCALL_CLASS_PROCESS: return "process";
    case LPR_LINUX_SYSCALL_CLASS_MEMORY: return "memory";
    case LPR_LINUX_SYSCALL_CLASS_THREAD_ARCH: return "thread_arch";
    case LPR_LINUX_SYSCALL_CLASS_FD_IO: return "fd_io";
    case LPR_LINUX_SYSCALL_CLASS_VFS_PATH: return "vfs_path";
    case LPR_LINUX_SYSCALL_CLASS_TIME_RANDOM: return "time_random";
    case LPR_LINUX_SYSCALL_CLASS_FD_CONTROL: return "fd_control";
    default: return "unknown";
    }
}

const char *lpr_linux_syscall_backend_name(enum lpr_linux_syscall_backend backend)
{
    switch (backend) {
    case LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE: return "local_state";
    case LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT: return "pacha_direct";
    case LPR_LINUX_SYSCALL_BACKEND_FILED: return "filed";
    case LPR_LINUX_SYSCALL_BACKEND_COORDINATOR: return "coordinator";
    default: return "unknown";
    }
}
