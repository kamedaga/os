#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Diagnostic preload only. Preserve exit status and libc cleanup; do not
 * bypass atexit handlers or change WebKit's shutdown watchdog. */
static void report(const char *stage, int code, void *caller)
{
    struct timespec now = {0};
    Dl_info info = {0};
    char line[768];
    clock_gettime(CLOCK_MONOTONIC, &now);
    dladdr(caller, &info);
    int n = snprintf(line, sizeof(line),
        "EXIT_TRACE pid=%ld tid=%ld time=%ld.%09ld stage=%s code=%d "
        "caller=%p module=%s offset=%#lx\n", (long)getpid(), syscall(SYS_gettid),
        now.tv_sec, now.tv_nsec, stage, code, caller,
        info.dli_fname ? info.dli_fname : "?",
        (unsigned long)caller - (unsigned long)info.dli_fbase);
    int fd = open("/dev/hvc0", O_WRONLY | O_CLOEXEC);
    if (n > 0) write(fd >= 0 ? fd : STDERR_FILENO, line,
        (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    if (fd >= 0) close(fd);
}

__attribute__((constructor)) static void started(void)
{
    report("loaded", 0, __builtin_return_address(0));
}

__attribute__((destructor)) static void finalized(void)
{
    report("destructor", 0, __builtin_return_address(0));
}

__attribute__((noreturn)) void exit(int code)
{
    void (*next)(int) = dlsym(RTLD_NEXT, "exit");
    report("exit-enter", code, __builtin_return_address(0));
    if (next) next(code);
    _Exit(code);
}
