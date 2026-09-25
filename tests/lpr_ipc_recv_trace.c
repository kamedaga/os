#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

/* Diagnostic interposer: preserve recvmsg semantics; never log payloads. */
#if defined(LPR_IPC_RECV_START_DIAG) && LPR_IPC_RECV_START_DIAG
__attribute__((constructor)) static void ipc_recv_trace_loaded(void)
{
    int saved = errno;
    char line[96];
    int n = snprintf(line, sizeof(line), "IPC_RECV_LOADED pid=%ld\n", (long)getpid());
    if (n > 0 && (size_t)n < sizeof(line)) (void)write(2, line, (size_t)n);
    errno = saved;
}
#endif

ssize_t recvmsg(int fd, struct msghdr *msg, int flags)
{
    int incoming_errno = errno;
    ssize_t (*next)(int, struct msghdr *, int) = dlsym(RTLD_NEXT, "recvmsg");
#if defined(LPR_IPC_RECV_START_DIAG) && LPR_IPC_RECV_START_DIAG
    static unsigned called;
    if (!__atomic_exchange_n(&called, 1, __ATOMIC_RELAXED) || !next) {
        char line[128];
        int n = snprintf(line, sizeof(line), "IPC_RECV_RESOLVE pid=%ld fd=%d next=%p\n",
            (long)getpid(), fd, (void *)next);
        if (n > 0 && (size_t)n < sizeof(line)) (void)write(2, line, (size_t)n);
    }
#endif
    if (!next) { errno = ENOSYS; return -1; }
    errno = incoming_errno;
    size_t capacity = msg ? msg->msg_controllen : 0;
    ssize_t result = next(fd, msg, flags);
    int saved_errno = errno;
    if ((result < 0 && saved_errno != EAGAIN && saved_errno != EINTR) ||
        (result >= 0 && msg && ((msg->msg_flags & MSG_CTRUNC)
#if !defined(LPR_IPC_RECV_ERRORS_ONLY) || !LPR_IPC_RECV_ERRORS_ONLY
            || msg->msg_controllen
#endif
        ))) {
        char line[512];
        int n = snprintf(line, sizeof(line),
            "IPC_RECV pid=%ld fd=%d result=%ld errno=%d flags=%#x output=%#x control=%zu/%zu iovs=%zu firstlen=%zu\n",
            (long)getpid(), fd, (long)result, saved_errno, flags,
            msg ? msg->msg_flags : 0, msg ? (size_t)msg->msg_controllen : 0, capacity,
            msg ? (size_t)msg->msg_iovlen : 0,
            msg && msg->msg_iovlen ? msg->msg_iov[0].iov_len : 0);
        if (n > 0) (void)write(2, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    }
    errno = saved_errno;
    return result;
}
