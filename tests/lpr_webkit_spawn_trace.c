#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

/* Diagnostic-only preload: observe launch/navigation without changing their
 * parameters, results, or errno. No upstream library source modification. */
struct unwind_context;
static unsigned long (*get_ip)(struct unwind_context *);
static int frame(struct unwind_context *context, void *arg)
{
    unsigned *count = arg;
    if ((*count)++ >= 30) return 5;
    void *ip = (void *)get_ip(context);
    Dl_info info = {0};
    dladdr(ip, &info);
    char line[768];
    int n = snprintf(line, sizeof(line),
        "SPAWN_FRAME pid=%ld n=%u module=%s offset=%#lx symbol=%s\n",
        (long)getpid(), *count, info.dli_fname ? info.dli_fname : "?",
        (unsigned long)ip - (unsigned long)info.dli_fbase,
        info.dli_sname ? info.dli_sname : "?");
    if (n > 0) write(2, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line)-1);
    return 0;
}

void *g_subprocess_launcher_spawnv(void *launcher, const char *const *argv, void **error)
{
    void *(*next)(void *, const char *const *, void **) =
        dlsym(RTLD_NEXT, "g_subprocess_launcher_spawnv");
    int saved = errno;
    char line[768];
    int n = snprintf(line, sizeof(line), "SPAWN_BEGIN pid=%ld path=%s\n",
        (long)getpid(), argv && argv[0] ? argv[0] : "?");
    if (n > 0) write(2, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line)-1);
    int (*backtrace_fn)(int (*)(struct unwind_context *, void *), void *) =
        dlsym(RTLD_DEFAULT, "_Unwind_Backtrace");
    get_ip = dlsym(RTLD_DEFAULT, "_Unwind_GetIP");
    unsigned count = 0;
    if (backtrace_fn && get_ip) backtrace_fn(frame, &count);
    errno = saved;
    return next(launcher, argv, error);
}

void webkit_web_view_load_uri(void *view, const char *uri)
{
    void (*next)(void *, const char *) = dlsym(RTLD_NEXT, "webkit_web_view_load_uri");
    int saved = errno;
    char line[768];
    int n = snprintf(line, sizeof(line), "LOAD_URI pid=%ld view=%p uri=%s\n",
        (long)getpid(), view, uri ? uri : "?");
    if (n > 0) write(2, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line)-1);
    errno = saved;
    next(view, uri);
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags)
{
    ssize_t (*next)(int, struct msghdr *, int) = dlsym(RTLD_NEXT, "recvmsg");
    ssize_t result = next(fd, msg, flags);
    int saved = errno;
    if (result > 0 && msg) {
        uint64_t hash = UINT64_C(14695981039346656037);
        unsigned char prefix[16];
        size_t seen = 0;
        for (int i = 0; i < msg->msg_iovlen && seen < (size_t)result; ++i) {
            const unsigned char *bytes = msg->msg_iov[i].iov_base;
            for (size_t j = 0; j < msg->msg_iov[i].iov_len && seen < (size_t)result; ++j, ++seen) {
                if (seen < sizeof(prefix)) prefix[seen] = bytes[j];
                hash = (hash ^ bytes[j]) * UINT64_C(1099511628211);
            }
        }
        char first[33];
        size_t size = seen < sizeof(prefix) ? seen : sizeof(prefix);
        for (size_t i = 0; i < size; ++i) {
            first[2*i] = "0123456789abcdef"[prefix[i] >> 4];
            first[2*i+1] = "0123456789abcdef"[prefix[i] & 15];
        }
        first[2*size] = 0;
        char line[256];
        int n = snprintf(line, sizeof(line),
            "RECV_HASH pid=%ld fd=%d bytes=%ld flags=%x control=%zu hash=%016lx first=%s\n",
            (long)getpid(), fd, (long)result, flags, (size_t)msg->msg_controllen,
            (unsigned long)hash, first);
        if (n > 0) write(2, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line)-1);
    }
    errno = saved;
    return result;
}
