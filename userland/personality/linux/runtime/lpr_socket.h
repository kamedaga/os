#ifndef LPR_SOCKET_H
#define LPR_SOCKET_H

#include <stdint.h>

#define LPR_NETD_ENDPOINT_FD 241

int lpr_linux_socket_fd_active(uint64_t fd);
int64_t lpr_linux_socket(uint64_t domain, uint64_t type, uint64_t protocol);
int64_t lpr_linux_socket_close(uint64_t fd);
int64_t lpr_linux_socket_read(uint64_t fd, uint64_t buf, uint64_t count);
int64_t lpr_linux_socket_readv(uint64_t fd, uint64_t iov, uint64_t iov_count);
int64_t lpr_linux_socket_write(uint64_t fd, uint64_t buf, uint64_t count);
int64_t lpr_linux_socket_writev(uint64_t fd, uint64_t iov, uint64_t iov_count);
int64_t lpr_linux_connect(uint64_t fd, uint64_t addr, uint64_t addrlen);
int64_t lpr_linux_bind(uint64_t fd, uint64_t addr, uint64_t addrlen);
int64_t lpr_linux_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t dest_addr, uint64_t addrlen);
int64_t lpr_linux_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t src_addr, uint64_t addrlen);
int64_t lpr_linux_sendmsg(uint64_t fd, uint64_t msg, uint64_t flags);
int64_t lpr_linux_recvmsg(uint64_t fd, uint64_t msg, uint64_t flags);
int64_t lpr_linux_sendmmsg(uint64_t fd, uint64_t msgvec, uint64_t vlen, uint64_t flags);
int64_t lpr_linux_recvmmsg(uint64_t fd, uint64_t msgvec, uint64_t vlen, uint64_t flags, uint64_t timeout);
int64_t lpr_linux_getsockname(uint64_t fd, uint64_t addr, uint64_t addrlen);
int64_t lpr_linux_getpeername(uint64_t fd, uint64_t addr, uint64_t addrlen);
int64_t lpr_linux_shutdown(uint64_t fd, uint64_t how);
int64_t lpr_linux_setsockopt(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen);
int64_t lpr_linux_getsockopt(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen);
int64_t lpr_linux_socket_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg);
int64_t lpr_linux_socket_ioctl(uint64_t fd, uint64_t request, uint64_t arg);
int64_t lpr_linux_socket_fstat(uint64_t fd, uint64_t statbuf);
int64_t lpr_linux_poll(uint64_t fds, uint64_t nfds, uint64_t timeout_ms);
int64_t lpr_linux_ppoll(uint64_t fds, uint64_t nfds, uint64_t timeout_ts, uint64_t sigmask, uint64_t sigsetsize);
int64_t lpr_linux_select(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, uint64_t timeout);
int64_t lpr_linux_pselect6(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, uint64_t timeout, uint64_t sigmask);

#endif
