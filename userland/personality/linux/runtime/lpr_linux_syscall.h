#ifndef LPR_LINUX_SYSCALL_H
#define LPR_LINUX_SYSCALL_H

#include <personality/linux_lpr.h>

enum lpr_linux_syscall_class {
    LPR_LINUX_SYSCALL_CLASS_PROCESS = 1,
    LPR_LINUX_SYSCALL_CLASS_MEMORY = 2,
    LPR_LINUX_SYSCALL_CLASS_THREAD_ARCH = 3,
    LPR_LINUX_SYSCALL_CLASS_FD_IO = 4,
    LPR_LINUX_SYSCALL_CLASS_VFS_PATH = 5,
    LPR_LINUX_SYSCALL_CLASS_TIME_RANDOM = 6,
    LPR_LINUX_SYSCALL_CLASS_FD_CONTROL = 7,
};

enum lpr_linux_syscall_backend {
    LPR_LINUX_SYSCALL_BACKEND_LOCAL_STATE = 1,
    LPR_LINUX_SYSCALL_BACKEND_PACHA_DIRECT = 2,
    LPR_LINUX_SYSCALL_BACKEND_FILED = 3,
    LPR_LINUX_SYSCALL_BACKEND_COORDINATOR = 4,
};

struct lpr_linux_syscall_info {
    uint64_t nr;
    const char *name;
    enum lpr_linux_syscall_class cls;
    enum lpr_linux_syscall_backend backend;
};

struct lpr_linux_user_frame {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rax;
    uint64_t rip;
    uint64_t rsp;
    uint64_t rflags;
};

const struct lpr_linux_syscall_info *lpr_linux_syscall_lookup(uint64_t nr);
const char *lpr_linux_syscall_class_name(enum lpr_linux_syscall_class cls);
const char *lpr_linux_syscall_backend_name(enum lpr_linux_syscall_backend backend);

const struct lpr_linux_user_frame *lpr_current_linux_user_frame(void);

int64_t lpr_dispatch_syscall(uint64_t nr,
                             uint64_t a0,
                             uint64_t a1,
                             uint64_t a2,
                             uint64_t a3,
                             uint64_t a4,
                             uint64_t a5);
int64_t lpr_dispatch_syscall_frame(struct lpr_linux_user_frame *frame,
                                   uint64_t nr,
                                   uint64_t a0,
                                   uint64_t a1,
                                   uint64_t a2,
                                   uint64_t a3,
                                   uint64_t a4,
                                   uint64_t a5);

#endif
