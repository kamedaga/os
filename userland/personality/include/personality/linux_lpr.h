#ifndef PERSONALITY_LINUX_LPR_H
#define PERSONALITY_LINUX_LPR_H

#include <stdint.h>
#include "personality_abi.h"
#include "runtime_page.h"
#include "zpoline.h"

#define LPR_LINUX_SYS_READ 0ull
#define LPR_LINUX_SYS_WRITE 1ull
#define LPR_LINUX_SYS_OPEN 2ull
#define LPR_LINUX_SYS_CLOSE 3ull
#define LPR_LINUX_SYS_STAT 4ull
#define LPR_LINUX_SYS_FSTAT 5ull
#define LPR_LINUX_SYS_LSTAT 6ull
#define LPR_LINUX_SYS_POLL 7ull
#define LPR_LINUX_SYS_LSEEK 8ull
#define LPR_LINUX_SYS_MMAP 9ull
#define LPR_LINUX_SYS_MPROTECT 10ull
#define LPR_LINUX_SYS_MUNMAP 11ull
#define LPR_LINUX_SYS_BRK 12ull
#define LPR_LINUX_SYS_RT_SIGACTION 13ull
#define LPR_LINUX_SYS_RT_SIGPROCMASK 14ull
#define LPR_LINUX_SYS_IOCTL 16ull
#define LPR_LINUX_SYS_PREAD64 17ull
#define LPR_LINUX_SYS_READV 19ull
#define LPR_LINUX_SYS_WRITEV 20ull
#define LPR_LINUX_SYS_ACCESS 21ull
#define LPR_LINUX_SYS_PIPE 22ull
#define LPR_LINUX_SYS_SELECT 23ull
#define LPR_LINUX_SYS_DUP 32ull
#define LPR_LINUX_SYS_DUP2 33ull
#define LPR_LINUX_SYS_NANOSLEEP 35ull
#define LPR_LINUX_SYS_GETPID 39ull
#define LPR_LINUX_SYS_KILL 62ull
#define LPR_LINUX_SYS_SOCKET 41ull
#define LPR_LINUX_SYS_CONNECT 42ull
#define LPR_LINUX_SYS_ACCEPT 43ull
#define LPR_LINUX_SYS_SENDTO 44ull
#define LPR_LINUX_SYS_RECVFROM 45ull
#define LPR_LINUX_SYS_SENDMSG 46ull
#define LPR_LINUX_SYS_RECVMSG 47ull
#define LPR_LINUX_SYS_SHUTDOWN 48ull
#define LPR_LINUX_SYS_BIND 49ull
#define LPR_LINUX_SYS_LISTEN 50ull
#define LPR_LINUX_SYS_GETSOCKNAME 51ull
#define LPR_LINUX_SYS_GETPEERNAME 52ull
#define LPR_LINUX_SYS_SOCKETPAIR 53ull
#define LPR_LINUX_SYS_SETSOCKOPT 54ull
#define LPR_LINUX_SYS_GETSOCKOPT 55ull
#define LPR_LINUX_SYS_CLONE 56ull
#define LPR_LINUX_SYS_UNAME 63ull
#define LPR_LINUX_SYS_FORK 57ull
#define LPR_LINUX_SYS_VFORK 58ull
#define LPR_LINUX_SYS_EXECVE 59ull
#define LPR_LINUX_SYS_EXIT 60ull
#define LPR_LINUX_SYS_WAIT4 61ull
#define LPR_LINUX_SYS_FCNTL 72ull
#define LPR_LINUX_SYS_FLOCK 73ull
#define LPR_LINUX_SYS_FSYNC 74ull
#define LPR_LINUX_SYS_FDATASYNC 75ull
#define LPR_LINUX_SYS_GETCWD 79ull
#define LPR_LINUX_SYS_CHDIR 80ull
#define LPR_LINUX_SYS_FCHDIR 81ull
#define LPR_LINUX_SYS_RENAME 82ull
#define LPR_LINUX_SYS_MKDIR 83ull
#define LPR_LINUX_SYS_RMDIR 84ull
#define LPR_LINUX_SYS_LINK 86ull
#define LPR_LINUX_SYS_UNLINK 87ull
#define LPR_LINUX_SYS_READLINK 89ull
#define LPR_LINUX_SYS_CHMOD 90ull
#define LPR_LINUX_SYS_FCHMOD 91ull
#define LPR_LINUX_SYS_CHOWN 92ull
#define LPR_LINUX_SYS_FCHOWN 93ull
#define LPR_LINUX_SYS_LCHOWN 94ull
#define LPR_LINUX_SYS_UMASK 95ull
#define LPR_LINUX_SYS_GETRLIMIT 97ull
#define LPR_LINUX_SYS_GETUID 102ull
#define LPR_LINUX_SYS_GETGID 104ull
#define LPR_LINUX_SYS_SETUID 105ull
#define LPR_LINUX_SYS_SETGID 106ull
#define LPR_LINUX_SYS_GETEUID 107ull
#define LPR_LINUX_SYS_GETEGID 108ull
#define LPR_LINUX_SYS_SETPGID 109ull
#define LPR_LINUX_SYS_GETPPID 110ull
#define LPR_LINUX_SYS_GETPGRP 111ull
#define LPR_LINUX_SYS_SETSID 112ull
#define LPR_LINUX_SYS_GETRESUID 118ull
#define LPR_LINUX_SYS_GETRESGID 120ull
#define LPR_LINUX_SYS_GETPGID 121ull
#define LPR_LINUX_SYS_GETSID 124ull
#define LPR_LINUX_SYS_SETPRIORITY 138ull
#define LPR_LINUX_SYS_SETRLIMIT 160ull
#define LPR_LINUX_SYS_ARCH_PRCTL 158ull
#define LPR_LINUX_SYS_GETTID 186ull
#define LPR_LINUX_SYS_GETDENTS64 217ull
#define LPR_LINUX_SYS_SET_TID_ADDRESS 218ull
#define LPR_LINUX_SYS_CLOCK_GETTIME 228ull
#define LPR_LINUX_SYS_CLOCK_NANOSLEEP 230ull
#define LPR_LINUX_SYS_EXIT_GROUP 231ull
#define LPR_LINUX_SYS_OPENAT 257ull
#define LPR_LINUX_SYS_MKDIRAT 258ull
#define LPR_LINUX_SYS_MKNODAT 259ull
#define LPR_LINUX_SYS_FCHOWNAT 260ull
#define LPR_LINUX_SYS_NEWFSTATAT 262ull
#define LPR_LINUX_SYS_UNLINKAT 263ull
#define LPR_LINUX_SYS_RENAMEAT 264ull
#define LPR_LINUX_SYS_LINKAT 265ull
#define LPR_LINUX_SYS_FCHMODAT 268ull
#define LPR_LINUX_SYS_FACCESSAT 269ull
#define LPR_LINUX_SYS_SYMLINKAT 266ull
#define LPR_LINUX_SYS_UNSHARE 272ull
#define LPR_LINUX_SYS_PSELECT6 270ull
#define LPR_LINUX_SYS_PPOLL 271ull
#define LPR_LINUX_SYS_UTIMENSAT 280ull
#define LPR_LINUX_SYS_EVENTFD 284ull
#define LPR_LINUX_SYS_EVENTFD2 290ull
#define LPR_LINUX_SYS_DUP3 292ull
#define LPR_LINUX_SYS_PIPE2 293ull
#define LPR_LINUX_SYS_CLOSE_RANGE 436ull
#define LPR_LINUX_SYS_RECVMMSG 299ull
#define LPR_LINUX_SYS_PRLIMIT64 302ull
#define LPR_LINUX_SYS_SENDMMSG 307ull
#define LPR_LINUX_SYS_GETRANDOM 318ull

#define LPR_LINUX_FD_MAX 0x7fffffffull
#define LPR_LINUX_FD_LIMIT (LPR_LINUX_FD_MAX + 1ull)

#define LPR_LINUX_EPERM 1
#define LPR_LINUX_ENOENT 2
#define LPR_LINUX_ESRCH 3
#define LPR_LINUX_EINTR 4
#define LPR_LINUX_EIO 5
#define LPR_LINUX_E2BIG 7
#define LPR_LINUX_EBADF 9
#define LPR_LINUX_ECHILD 10
#define LPR_LINUX_EAGAIN 11
#define LPR_LINUX_ENOMEM 12
#define LPR_LINUX_EACCES 13
#define LPR_LINUX_EFAULT 14
#define LPR_LINUX_EEXIST 17
#define LPR_LINUX_EXDEV 18
#define LPR_LINUX_ENOTDIR 20
#define LPR_LINUX_EISDIR 21
#define LPR_LINUX_EINVAL 22
#define LPR_LINUX_EMFILE 24
#define LPR_LINUX_ENOTTY 25
#define LPR_LINUX_ESPIPE 29
#define LPR_LINUX_EPIPE 32
#define LPR_LINUX_ENAMETOOLONG 36
#define LPR_LINUX_ERANGE 34
#define LPR_LINUX_ENOTEMPTY 39
#define LPR_LINUX_ELOOP 40
#define LPR_LINUX_ENOSYS 38
#define LPR_LINUX_EPROTONOSUPPORT 93
#define LPR_LINUX_ESOCKTNOSUPPORT 94
#define LPR_LINUX_ENOTSUP 95
#define LPR_LINUX_EOPNOTSUPP 95
#define LPR_LINUX_EAFNOSUPPORT 97
#define LPR_LINUX_ENETDOWN 100
#define LPR_LINUX_ENETUNREACH 101
#define LPR_LINUX_EISCONN 106
#define LPR_LINUX_ENOTCONN 107
#define LPR_LINUX_ETIMEDOUT 110
#define LPR_LINUX_ECONNREFUSED 111
#define LPR_LINUX_EALREADY 114
#define LPR_LINUX_EINPROGRESS 115

#define LPR_LINUX_ARCH_SET_GS 0x1001ull
#define LPR_LINUX_ARCH_SET_FS 0x1002ull
#define LPR_LINUX_ARCH_GET_FS 0x1003ull
#define LPR_LINUX_ARCH_GET_GS 0x1004ull

#define LPR_BOOTSTRAP_FD 243
#define LPR_SUPERVISOR_ENDPOINT_FD 244
#define LPR_BOOTSTRAP_MAGIC 0x315450424c50524cull
#define LPR_BOOTSTRAP_VERSION 5ull
#define LPR_BOOTSTRAP_FLAG_DEFAULT_STDIO 1ull
#define LPR_BOOTSTRAP_FLAG_SUPERVISOR 2ull
#define LPR_BOOTSTRAP_FD_FILED 1u
#define LPR_BOOTSTRAP_FD_TTY 2u
#define LPR_BOOTSTRAP_FD_EVENT 3u
#define LPR_BOOTSTRAP_CTTY_BYTES 64
#define LPR_BOOTSTRAP_CWD_BYTES 480

typedef struct lpr_bootstrap_fd {
    uint64_t fd;
    uint64_t kind;
    uint64_t flags;
    uint64_t handle;
    uint64_t offset_or_counter;
} lpr_bootstrap_fd_t;

struct lpr_bootstrap {
    uint64_t magic;
    uint64_t version;
    uint64_t byte_size;
    uint64_t local_fd_table_offset;
    uint64_t local_fd_table_bytes;
    uint64_t local_fd_count;
    uint64_t linux_pid;
    uint64_t linux_ppid;
    uint64_t linux_sid;
    uint64_t linux_pgrp;
    uint64_t linux_next_pid;
    uint64_t cwd_handle;
    uint64_t supervisor_token;
    uint64_t supervisor_endpoint_fd;
    uint64_t fd_table_token;
    uint64_t flags;
    char ctty[LPR_BOOTSTRAP_CTTY_BYTES];
    char cwd[LPR_BOOTSTRAP_CWD_BYTES];
};

int64_t lpr_start(struct lpr_runtime_page *runtime);
int64_t lpr_patch_mapping(const struct lpr_patch_mapping_request *request,
                          struct lpr_patch_mapping_result *result);
int64_t lpr_init_zpoline_page(uint8_t *page);
int64_t lpr_build_zpoline_page(uint8_t *page, uint64_t handler_va);
uint64_t lpr_zpoline_common_offset(void);

#endif
