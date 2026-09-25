#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/inotify.h>

static int watched_inotify = -1;
static _Thread_local int reporting;

static void fd_report(const char *operation, int fd, long result, int error)
{
    int saved = errno;
    if (reporting) return;
    reporting = 1;
    int console = open("/dev/hvc0", O_WRONLY);
    dprintf(console >= 0 ? console : 2,
        "GLIB_FD pid=%ld op=%s fd=%d result=%ld errno=%d getfd=%d\n",
        (long)getpid(), operation, fd, result, error, fcntl(fd, F_GETFD));
    if (console >= 0) close(console);
    errno = saved;
    reporting = 0;
}

int inotify_init1(int flags)
{
    int (*next)(int) = dlsym(RTLD_NEXT, "inotify_init1");
    int result = next(flags);
    watched_inotify = result;
    fd_report("inotify_init1", result, result, errno);
    return result;
}

int inotify_init(void)
{
    int (*next)(void) = dlsym(RTLD_NEXT, "inotify_init");
    int result = next();
    watched_inotify = result;
    fd_report("inotify_init", result, result, errno);
    return result;
}

int close(int fd)
{
    int (*next)(int) = dlsym(RTLD_NEXT, "close");
    if (!reporting && fd == watched_inotify) {
        Dl_info info = {0};
        void *caller = __builtin_return_address(0);
        dladdr(caller, &info);
        fd_report(info.dli_fname ? info.dli_fname : "close", fd,
            (long)((char *)caller - (char *)info.dli_fbase), errno);
    }
    return next(fd);
}

int close_range(unsigned first, unsigned last, int flags)
{
    int (*next)(unsigned, unsigned, int) = dlsym(RTLD_NEXT, "close_range");
    fd_report("close_range", (int)first, last, flags);
    return next(first, last, flags);
}

ssize_t read(int fd, void *buffer, size_t size)
{
    ssize_t (*next)(int, void *, size_t) = dlsym(RTLD_NEXT, "read");
    ssize_t result = next(fd, buffer, size);
    if (result < 0 && errno == EBADF) fd_report("read", fd, result, errno);
    return result;
}

/* Diagnostic only: forward the same GLib message and level, including fatal
 * handling, while making structured errors visible on stderr. */
void g_log_structured_standard(const char *domain, unsigned level,
    const char *file, const char *line, const char *function, const char *format, ...)
{
    va_list args;
    char message[2048];
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (level & 7) {
        dprintf(2, "GLIB_ERROR pid=%ld domain=%s level=%u %s:%s %s: %s\n",
            (long)getpid(), domain ? domain : "-", level,
            file ? file : "-", line ? line : "-", function ? function : "-", message);
    }
    void (*next)(const char *, unsigned, const char *, const char *, const char *, const char *, ...) =
        dlsym(RTLD_NEXT, "g_log_structured_standard");
    next(domain, level, file, line, function, "%s", message);
}

struct log_field { const char *key; const void *value; long length; };
static int writer(unsigned level, const struct log_field *fields, size_t count, void *data)
{
    (void)data;
    if (level & 7) {
        int console = open("/dev/hvc0", O_WRONLY);
        int output = console >= 0 ? console : 2;
        for (size_t i = 0; i < count; ++i) {
            if (fields[i].value && fields[i].length < 0)
                dprintf(output, "GLIB_FATAL pid=%ld %s=%s\n", (long)getpid(), fields[i].key,
                    (const char *)fields[i].value);
        }
        if (console >= 0) close(console);
    }
    int (*next)(unsigned, const struct log_field *, size_t, void *) =
        dlsym(RTLD_DEFAULT, "g_log_writer_default");
    return next(level, fields, count, NULL);
}

__attribute__((constructor)) static void observe_structured_logging(void)
{
    void (*set_writer)(int (*)(unsigned, const struct log_field *, size_t, void *), void *, void *) =
        dlsym(RTLD_DEFAULT, "g_log_set_writer_func");
    if (set_writer) {
        set_writer(writer, NULL, NULL);
        dprintf(2, "GLIB_TRACE_ACTIVE pid=%ld\n", (long)getpid());
    }
}

void g_log_structured_array(unsigned level, const struct log_field *fields, size_t count)
{
    if (level & 7) {
        for (size_t i = 0; i < count; ++i) {
            if (!fields[i].value) continue;
            dprintf(2, "GLIB_FIELD pid=%ld level=%u %s=%.*s\n", (long)getpid(), level,
                fields[i].key, fields[i].length < 0 ? 2048 : (int)fields[i].length,
                (const char *)fields[i].value);
        }
    }
    void (*next)(unsigned, const struct log_field *, size_t) = dlsym(RTLD_NEXT, "g_log_structured_array");
    next(level, fields, count);
}
