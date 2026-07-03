#ifndef LPR_FILED_H
#define LPR_FILED_H

#include <stdint.h>

#define LPR_FILED_ENDPOINT_FD 240

int64_t lpr_linux_openat(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t mode);
int64_t lpr_linux_read(uint64_t fd, uint64_t buf, uint64_t count);
int64_t lpr_linux_readv(uint64_t fd, uint64_t iov, uint64_t iov_count);
int64_t lpr_linux_pread64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset);
int64_t lpr_linux_pread_to_vmo(uint64_t fd, uint64_t vmo_fd, uint64_t vmo_offset, uint64_t count, uint64_t file_offset);
int64_t lpr_linux_file_vmo(uint64_t fd, uint64_t file_offset, uint64_t length, uint64_t *out_loaded);
int64_t lpr_linux_write(uint64_t fd, uint64_t buf, uint64_t count);
int64_t lpr_linux_writev(uint64_t fd, uint64_t iov, uint64_t iov_count);
int64_t lpr_linux_close(uint64_t fd);
int64_t lpr_linux_fstat(uint64_t fd, uint64_t statbuf);
int64_t lpr_linux_fsync(uint64_t fd);
int64_t lpr_linux_lseek(uint64_t fd, uint64_t offset, uint64_t whence);
int64_t lpr_linux_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg);
int64_t lpr_linux_ioctl(uint64_t fd, uint64_t request, uint64_t arg);
int64_t lpr_linux_newfstatat(uint64_t dirfd, uint64_t path, uint64_t statbuf, uint64_t flags);
int64_t lpr_linux_faccessat(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t flags);
int64_t lpr_linux_fchmod(uint64_t fd, uint64_t mode);
int64_t lpr_linux_fchmodat(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t flags);
int64_t lpr_linux_utimensat(uint64_t dirfd, uint64_t path, uint64_t times, uint64_t flags);
int64_t lpr_linux_getdents64(uint64_t fd, uint64_t buf, uint64_t count);
int64_t lpr_linux_access(uint64_t path, uint64_t mode);
int64_t lpr_linux_readlink(uint64_t path, uint64_t buf, uint64_t bufsiz);
int64_t lpr_linux_mkdirat(uint64_t dirfd, uint64_t path, uint64_t mode);
int64_t lpr_linux_mknodat(uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t dev);
int64_t lpr_linux_unlinkat(uint64_t dirfd, uint64_t path, uint64_t flags);
int64_t lpr_linux_renameat(uint64_t old_dirfd, uint64_t old_path, uint64_t new_dirfd, uint64_t new_path);
int64_t lpr_linux_fchownat(uint64_t dirfd, uint64_t path, uint64_t owner, uint64_t group, uint64_t flags);
int64_t lpr_linux_symlinkat(uint64_t target, uint64_t new_dirfd, uint64_t linkpath);
int64_t lpr_linux_linkat(uint64_t old_dirfd, uint64_t old_path, uint64_t new_dirfd, uint64_t new_path, uint64_t flags);
int64_t lpr_linux_pipe2(uint64_t fds, uint64_t flags);
int64_t lpr_linux_dup(uint64_t fd, uint64_t min_fd, uint64_t cloexec);
int64_t lpr_linux_clone(uint64_t flags, uint64_t child_stack, uint64_t parent_tid, uint64_t child_tid, uint64_t tls);
int64_t lpr_linux_fork(void);
int64_t lpr_linux_vfork(void);
int64_t lpr_linux_wait4(uint64_t pid, uint64_t status, uint64_t options, uint64_t rusage);
int64_t lpr_linux_execve(uint64_t path, uint64_t argv, uint64_t envp);
int lpr_linux_filed_fd_active(uint64_t fd);
uint64_t lpr_linux_filed_fd_handle(uint64_t fd);
void lpr_linux_readv_cache_trace_dump(void);

#endif
