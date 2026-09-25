#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

struct unwind_context;
static unsigned long (*get_ip)(struct unwind_context *);

static void trace_write(const char *line, size_t size)
{
    int console = open("/dev/hvc0", O_WRONLY);
    (void)write(console >= 0 ? console : 2, line, size);
    if (console >= 0) close(console);
}

static int frame(struct unwind_context *context, void *arg)
{
    unsigned *count = arg;
    if ((*count)++ >= 24) return 5;
    void *ip = (void *)get_ip(context);
    Dl_info info = {0};
    (void)dladdr(ip, &info);
    char line[1024];
    int n = snprintf(line, sizeof(line),
        "ABORT_FRAME pid=%ld n=%u ip=%p module=%s offset=%#lx symbol=%s\n",
        (long)getpid(), *count, ip, info.dli_fname ? info.dli_fname : "?",
        (unsigned long)ip - (unsigned long)info.dli_fbase,
        info.dli_sname ? info.dli_sname : "?");
    if (n > 0) trace_write(line,
        (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    pthread_attr_t attr;
    void *base = NULL;
    size_t size = 0;
    int status = pthread_getattr_np(pthread_self(), &attr);
    if (!status) {
        status = pthread_attr_getstack(&attr, &base, &size);
        pthread_attr_destroy(&attr);
    }
    n = snprintf(line, sizeof(line),
        "ABORT_STACK pid=%ld tid=%ld local=%p base=%p size=%#lx status=%d\n",
        (long)getpid(), syscall(SYS_gettid), (void *)&attr, base,
        (unsigned long)size, status);
    if (n > 0) trace_write(line,
        (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    return 0;
}

/* Diagnostic-only LD_PRELOAD: report the caller, then preserve abort().
 * No WebKit/libc source changes or replacement failure semantics. */
__attribute__((noreturn)) void abort(void)
{
    void *caller = __builtin_return_address(0);
    Dl_info info = {0};
    (void)dladdr(caller, &info);
    char line[1024];
    int n = snprintf(line, sizeof(line),
        "ABORT_TRACE pid=%ld caller=%p module=%s offset=%#lx symbol=%s\n",
        (long)getpid(), caller, info.dli_fname ? info.dli_fname : "?",
        (unsigned long)caller - (unsigned long)info.dli_fbase,
        info.dli_sname ? info.dli_sname : "?");
    if (n > 0) trace_write(line,
        (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    int (*backtrace_fn)(int (*)(struct unwind_context *, void *), void *) =
        dlsym(RTLD_DEFAULT, "_Unwind_Backtrace");
    get_ip = dlsym(RTLD_DEFAULT, "_Unwind_GetIP");
    unsigned count = 0;
    if (backtrace_fn && get_ip) (void)backtrace_fn(frame, &count);
    void (*real_abort)(void) = dlsym(RTLD_NEXT, "abort");
    if (real_abort) real_abort();
    _Exit(134);
}
