#ifndef LPR_FILED_H
#define LPR_FILED_H

#include <stdint.h>

#define LPR_FILED_ENDPOINT_FD 240
#define LPR_TERMD_TTY_ENDPOINT_FD 242

int64_t lpr_linux_openat(uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t mode);
int64_t lpr_linux_read(uint64_t fd, uint64_t buf, uint64_t count);
int64_t lpr_linux_readv(uint64_t fd, uint64_t iov, uint64_t iov_count);
int64_t lpr_linux_pread64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset);
int64_t lpr_linux_pread_to_vmo(uint64_t fd, uint64_t vmo_fd, uint64_t vmo_offset, uint64_t count, uint64_t file_offset);
int64_t lpr_linux_file_vmo(uint64_t fd, uint64_t file_offset, uint64_t length, uint64_t *out_loaded);
int64_t lpr_linux_write(uint64_t fd, uint64_t buf, uint64_t count);
int64_t lpr_linux_writev(uint64_t fd, uint64_t iov, uint64_t iov_count);
int64_t lpr_linux_close(uint64_t fd);
int64_t lpr_linux_close_range(uint64_t first, uint64_t last, uint64_t flags);
int64_t lpr_linux_fstat(uint64_t fd, uint64_t statbuf);
int64_t lpr_linux_fsync(uint64_t fd);
int64_t lpr_linux_sync(void);
int64_t lpr_linux_lseek(uint64_t fd, uint64_t offset, uint64_t whence);
int64_t lpr_linux_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg);
int64_t lpr_linux_flock(uint64_t fd, uint64_t operation);
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
int64_t lpr_linux_eventfd2(uint64_t initval, uint64_t flags);
int64_t lpr_linux_dup(uint64_t fd, uint64_t min_fd, uint64_t cloexec);
int64_t lpr_linux_dup2(uint64_t old_fd, uint64_t new_fd, uint64_t flags);
int64_t lpr_linux_clone(uint64_t flags, uint64_t child_stack, uint64_t parent_tid, uint64_t child_tid, uint64_t tls);
int64_t lpr_linux_fork(void);
int64_t lpr_linux_vfork(void);
void lpr_linux_apply_pending_fork_child(void);
void lpr_linux_prepare_process_exit(uint64_t exit_code);
int64_t lpr_linux_wait4(uint64_t pid, uint64_t status, uint64_t options, uint64_t rusage);
int64_t lpr_linux_execve(uint64_t path, uint64_t argv, uint64_t envp);
int64_t lpr_linux_getpid(void);
int64_t lpr_linux_getppid(void);
int64_t lpr_linux_getpgrp(void);
int64_t lpr_linux_getpgid(uint64_t pid);
int64_t lpr_linux_setpgid(uint64_t pid, uint64_t pgid);
int64_t lpr_linux_setsid(void);
int64_t lpr_linux_getsid(uint64_t pid);
int64_t lpr_linux_chdir(uint64_t path);
int64_t lpr_linux_fchdir(uint64_t fd);
int64_t lpr_linux_nanosleep(uint64_t req, uint64_t rem);
int64_t lpr_linux_clock_nanosleep(uint64_t clock_id, uint64_t flags, uint64_t req, uint64_t rem);
int64_t lpr_linux_kill(uint64_t pid, uint64_t sig);
int64_t lpr_linux_rt_sigaction(uint64_t sig, uint64_t act, uint64_t oldact, uint64_t sigsetsize);
int64_t lpr_linux_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t oldset, uint64_t sigsetsize);
int64_t lpr_linux_sigaltstack(uint64_t ss, uint64_t old_ss);
int64_t lpr_linux_dispatch_pending_signals(void);
int lpr_linux_filed_fd_active(uint64_t fd);
uint64_t lpr_linux_filed_fd_handle(uint64_t fd);
int lpr_linux_eventfd_active(uint64_t fd);
uint32_t lpr_linux_eventfd_poll_events(uint64_t fd, uint32_t events);
int lpr_linux_tty_fd_active(uint64_t fd);
uint32_t lpr_linux_tty_poll_events(uint64_t fd, uint32_t events);
int lpr_linux_pipe_fd_active(uint64_t fd);
uint32_t lpr_linux_pipe_poll_events(uint64_t fd, uint32_t events);
void lpr_linux_ensure_default_stdio(void);
void lpr_linux_readv_cache_trace_dump(void);

#endif
