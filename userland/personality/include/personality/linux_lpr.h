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
#define LPR_LINUX_SYS_DUP 32ull
#define LPR_LINUX_SYS_GETPID 39ull
#define LPR_LINUX_SYS_CLONE 56ull
#define LPR_LINUX_SYS_FORK 57ull
#define LPR_LINUX_SYS_VFORK 58ull
#define LPR_LINUX_SYS_EXECVE 59ull
#define LPR_LINUX_SYS_EXIT 60ull
#define LPR_LINUX_SYS_WAIT4 61ull
#define LPR_LINUX_SYS_FCNTL 72ull
#define LPR_LINUX_SYS_FSYNC 74ull
#define LPR_LINUX_SYS_FDATASYNC 75ull
#define LPR_LINUX_SYS_GETCWD 79ull
#define LPR_LINUX_SYS_RENAME 82ull
#define LPR_LINUX_SYS_MKDIR 83ull
#define LPR_LINUX_SYS_RMDIR 84ull
#define LPR_LINUX_SYS_UNLINK 87ull
#define LPR_LINUX_SYS_READLINK 89ull
#define LPR_LINUX_SYS_CHMOD 90ull
#define LPR_LINUX_SYS_FCHMOD 91ull
#define LPR_LINUX_SYS_UMASK 95ull
#define LPR_LINUX_SYS_GETUID 102ull
#define LPR_LINUX_SYS_GETGID 104ull
#define LPR_LINUX_SYS_SETUID 105ull
#define LPR_LINUX_SYS_SETGID 106ull
#define LPR_LINUX_SYS_GETEUID 107ull
#define LPR_LINUX_SYS_GETEGID 108ull
#define LPR_LINUX_SYS_SETPRIORITY 138ull
#define LPR_LINUX_SYS_ARCH_PRCTL 158ull
#define LPR_LINUX_SYS_GETTID 186ull
#define LPR_LINUX_SYS_GETDENTS64 217ull
#define LPR_LINUX_SYS_SET_TID_ADDRESS 218ull
#define LPR_LINUX_SYS_CLOCK_GETTIME 228ull
#define LPR_LINUX_SYS_EXIT_GROUP 231ull
#define LPR_LINUX_SYS_OPENAT 257ull
#define LPR_LINUX_SYS_MKDIRAT 258ull
#define LPR_LINUX_SYS_MKNODAT 259ull
#define LPR_LINUX_SYS_FCHOWNAT 260ull
#define LPR_LINUX_SYS_NEWFSTATAT 262ull
#define LPR_LINUX_SYS_UNLINKAT 263ull
#define LPR_LINUX_SYS_RENAMEAT 264ull
#define LPR_LINUX_SYS_FCHMODAT 268ull
#define LPR_LINUX_SYS_FACCESSAT 269ull
#define LPR_LINUX_SYS_SYMLINKAT 266ull
#define LPR_LINUX_SYS_UNSHARE 272ull
#define LPR_LINUX_SYS_UTIMENSAT 280ull
#define LPR_LINUX_SYS_DUP3 292ull
#define LPR_LINUX_SYS_PIPE2 293ull
#define LPR_LINUX_SYS_GETRANDOM 318ull

#define LPR_LINUX_EPERM 1
#define LPR_LINUX_ENOENT 2
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
#define LPR_LINUX_ENOTTY 25
#define LPR_LINUX_ESPIPE 29
#define LPR_LINUX_ENAMETOOLONG 36
#define LPR_LINUX_ERANGE 34
#define LPR_LINUX_ENOTEMPTY 39
#define LPR_LINUX_ELOOP 40
#define LPR_LINUX_EPIPE 32
#define LPR_LINUX_ENOSYS 38
#define LPR_LINUX_ENOTSUP 95

#define LPR_LINUX_ARCH_SET_GS 0x1001ull
#define LPR_LINUX_ARCH_SET_FS 0x1002ull
#define LPR_LINUX_ARCH_GET_FS 0x1003ull
#define LPR_LINUX_ARCH_GET_GS 0x1004ull

int64_t lpr_start(struct lpr_runtime_page *runtime);
int64_t lpr_patch_mapping(const struct lpr_patch_mapping_request *request,
                          struct lpr_patch_mapping_result *result);
int64_t lpr_init_zpoline_page(uint8_t *page);
int64_t lpr_build_zpoline_page(uint8_t *page, uint64_t handler_va);
uint64_t lpr_zpoline_common_offset(void);

#endif
