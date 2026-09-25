#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <stdio.h>
#include <unistd.h>

/* Diagnostic only: leave return values and errno unchanged. No payloads. */
static void report(const char *op, int fd, unsigned long command, int result,
    int error, int before, void *caller)
{
    Dl_info info = {0};
    dladdr(caller, &info);
    char line[768];
    int n = snprintf(line, sizeof(line),
        "BROWSER_FD pid=%ld op=%s fd=%d cmd=%#lx result=%d errno=%d before=%d caller=%s+%#lx\n",
        (long)getpid(), op, fd, command, result, error, before,
        info.dli_fname ? info.dli_fname : "?",
        (unsigned long)caller - (unsigned long)info.dli_fbase);
    if (n > 0) write(2, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line)-1);
}

int close(int fd)
{
    int (*next)(int) = dlsym(RTLD_NEXT, "close");
    int original = errno;
    int before = fcntl(fd, F_GETFD);
    errno = original;
    int result = next(fd);
    int error = errno;
    if (result < 0) report("close", fd, 0, result, error, before, __builtin_return_address(0));
    errno = error;
    return result;
}

int drmIoctl(int fd, unsigned long request, void *arg)
{
    int (*next)(int, unsigned long, void *) = dlsym(RTLD_NEXT, "drmIoctl");
    int result = next(fd, request, arg);
    int error = errno;
    if (result < 0) report("drmIoctl", fd, request, result, error, -2, __builtin_return_address(0));
    errno = error;
    return result;
}

static int module(struct dl_phdr_info *info, size_t size, void *arg)
{
    (void)size; (void)arg;
    char line[768];
    int n = snprintf(line, sizeof(line), "BROWSER_MODULE pid=%ld base=%#lx name=%s\n",
        (long)getpid(), (unsigned long)info->dlpi_addr, info->dlpi_name);
    if (n > 0) write(2, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line)-1);
    return 0;
}

__attribute__((constructor)) static void modules(void)
{
    int error = errno;
    dl_iterate_phdr(module, NULL);
    errno = error;
}
