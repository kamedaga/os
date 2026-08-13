#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t handled;
static volatile sig_atomic_t altstack_ok;
static volatile sig_atomic_t sse_stack_ok;
static volatile sig_atomic_t avx_handled;
static unsigned char alternate_stack[64 * 1024];
static int terminate_event_fd = -1;

typedef float aligned_vec4_t __attribute__((vector_size(16)));

static const unsigned char avx_handler_pattern[32] __attribute__((aligned(32))) = {
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
};

static void write_marker(const char *text)
{
    (void)write(STDOUT_FILENO, text, strlen(text));
}

static void sigint_handler(int signo)
{
    if (signo == SIGINT) {
        aligned_vec4_t spill __attribute__((aligned(16)));
        const aligned_vec4_t value = { 1.0f, 2.0f, 4.0f, 8.0f };
        __asm__ volatile("movaps %1, %0" : "=m"(spill) : "x"(value) : "memory");
        if (spill[0] == 1.0f && spill[1] == 2.0f &&
            spill[2] == 4.0f && spill[3] == 8.0f)
        {
            sse_stack_ok = 1;
            write_marker("ASYNC_SSE_STACK=OK\n");
        } else {
            write_marker("ASYNC_SSE_STACK=BAD\n");
        }
        handled = 1;
        write_marker("ASYNC_HANDLER_CALLED\n");
    }
}

__attribute__((target("avx2")))
static void avx_signal_handler(int signo)
{
    if (signo != SIGUSR1) return;
    write_marker("AVX_SIGNAL_HANDLER=OK\n");
    __asm__ volatile(
        "vmovdqu (%0), %%ymm0\n\t"
        :
        : "r"(avx_handler_pattern)
        : "ymm0", "memory");
    avx_handled = 1;
}

__attribute__((target("avx2"), noinline))
static int run_avx_signal_roundtrip(void)
{
    static const unsigned char expected[32] __attribute__((aligned(32))) = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
        0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f,
    };
    unsigned char observed[32] __attribute__((aligned(32)));
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = avx_signal_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, 0) != 0) return 50;

    write_marker("AVX_SIGNAL_READY\n");
    const long pid = (long)getpid();
    long rc;
    __asm__ volatile(
        "vmovdqu (%[expected]), %%ymm0\n\t"
        "mov $62, %%eax\n\t"
        "mov %[pid], %%rdi\n\t"
        "mov $10, %%esi\n\t"
        "syscall\n\t"
        "vmovdqu %%ymm0, (%[observed])\n\t"
        "vzeroupper\n\t"
        : "=&a"(rc)
        : [expected] "r"(expected),
          [observed] "r"(observed),
          [pid] "r"(pid)
        : "rcx", "rdi", "rsi", "r11", "ymm0", "memory");
    write_marker("AVX_SIGNAL_RETURNED\n");
    if (rc != 0 || !avx_handled) return 51;
    if (memcmp(expected, observed, sizeof(expected)) != 0) return 52;
    write_marker("AVX_SIGNAL_XSTATE=OK\n");
    return 0;
}

__attribute__((target("avx2"), noinline))
static int run_avx_syscall_roundtrip(void)
{
    static const unsigned char expected[32] __attribute__((aligned(32))) = {
        0x3c, 0x2d, 0x1e, 0x0f, 0x4b, 0x5a, 0x69, 0x78,
        0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0,
        0x13, 0x24, 0x35, 0x46, 0x57, 0x68, 0x79, 0x8a,
        0x9b, 0xac, 0xbd, 0xce, 0xdf, 0xe0, 0xf1, 0x02,
    };
    unsigned char observed[32] __attribute__((aligned(32)));
    long rc;
    write_marker("AVX_SYSCALL_READY\n");
    __asm__ volatile(
        "vmovdqu (%[expected]), %%ymm0\n\t"
        "mov $39, %%eax\n\t"
        "syscall\n\t"
        "vmovdqu %%ymm0, (%[observed])\n\t"
        "vzeroupper\n\t"
        : "=a"(rc)
        : [expected] "r"(expected),
          [observed] "r"(observed)
        : "rcx", "r11", "ymm0", "memory");
    write_marker("AVX_SYSCALL_RETURNED\n");
    if (rc <= 0 || memcmp(expected, observed, sizeof(expected)) != 0) return 53;
    write_marker("AVX_SYSCALL_XSTATE=OK\n");
    return 0;
}

__attribute__((target("avx2"), noinline))
static int run_avx_local(void)
{
    static const unsigned char expected[32] __attribute__((aligned(32))) = {
        0x5f, 0x4e, 0x3d, 0x2c, 0x1b, 0x0a, 0x19, 0x28,
        0x37, 0x46, 0x55, 0x64, 0x73, 0x82, 0x91, 0xa0,
        0xaf, 0xbe, 0xcd, 0xdc, 0xeb, 0xfa, 0xe9, 0xd8,
        0xc7, 0xb6, 0xa5, 0x94, 0x83, 0x72, 0x61, 0x50,
    };
    unsigned char observed[32] __attribute__((aligned(32)));
    write_marker("AVX_LOCAL_READY\n");
    __asm__ volatile(
        "vmovdqu (%[expected]), %%ymm0\n\t"
        "vmovdqu %%ymm0, (%[observed])\n\t"
        "vzeroupper\n\t"
        :
        : [expected] "r"(expected),
          [observed] "r"(observed)
        : "ymm0", "memory");
    if (memcmp(expected, observed, sizeof(expected)) != 0) return 54;
    write_marker("AVX_LOCAL=OK\n");
    return 0;
}

static int run_avx_suite(void)
{
    int rc = run_avx_local();
    if (rc != 0) return rc;
    rc = run_avx_syscall_roundtrip();
    if (rc != 0) return rc;
    rc = run_avx_signal_roundtrip();
    if (rc != 0) return rc;
    write_marker("AVX_XSTATE_SUITE=OK\n");
    return 0;
}

static void altstack_handler(int signo, siginfo_t *info, void *context)
{
    (void)info;
    (void)context;
    stack_t current;
    unsigned char local;
    const uintptr_t local_addr = (uintptr_t)&local;
    const uintptr_t stack_start = (uintptr_t)alternate_stack;
    const uintptr_t stack_end = stack_start + sizeof(alternate_stack);
    if (signo == SIGINT &&
        sigaltstack(0, &current) == 0 &&
        (current.ss_flags & SS_ONSTACK) != 0 &&
        local_addr >= stack_start && local_addr < stack_end)
    {
        altstack_ok = 1;
        write_marker("ASYNC_ALTSTACK_ONSTACK=OK\n");
    } else {
        write_marker("ASYNC_ALTSTACK_ONSTACK=BAD\n");
    }
    handled = 1;
}

static void terminate_event_handler(int signo)
{
    static const char marker[] = "ASYNC_EPOLL_HANDLER_CALLED\n";
    const uint64_t one = 1;
    if (signo == SIGTERM &&
        write(terminate_event_fd, &one, sizeof(one)) == (ssize_t)sizeof(one))
    {
        (void)write(STDOUT_FILENO, marker, sizeof(marker) - 1);
    }
}

static int run_epoll_handler(void)
{
    terminate_event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (terminate_event_fd < 0 || epoll_fd < 0) return 6;

    struct epoll_event interest;
    memset(&interest, 0, sizeof(interest));
    interest.events = EPOLLIN;
    interest.data.fd = terminate_event_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, terminate_event_fd, &interest) != 0) return 7;

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = terminate_event_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, 0) != 0) return 8;

    write_marker("ASYNC_EPOLL_HANDLER_READY\n");
    struct epoll_event event;
    for (;;) {
        const int count = epoll_wait(epoll_fd, &event, 1, -1);
        if (count == 1 && event.data.fd == terminate_event_fd &&
            (event.events & EPOLLIN) != 0) break;
        if (count < 0 && errno == EINTR) continue;
        return 9;
    }

    uint64_t value = 0;
    if (read(terminate_event_fd, &value, sizeof(value)) != (ssize_t)sizeof(value) ||
        value != 1) return 10;
    write_marker("ASYNC_EPOLL_HANDLER_CONTINUED\n");
    (void)close(epoll_fd);
    (void)close(terminate_event_fd);
    terminate_event_fd = -1;
    return 0;
}

static int run_signalfd_handler(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, 0) != 0) return 11;

    const int signal_fd = signalfd(
        -1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (signal_fd < 0 || epoll_fd < 0) return 12;

    struct epoll_event interest;
    memset(&interest, 0, sizeof(interest));
    interest.events = EPOLLIN;
    interest.data.fd = signal_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, signal_fd, &interest) != 0) {
        return 13;
    }

    write_marker("ASYNC_SIGNALFD_READY\n");
    struct epoll_event event;
    for (;;) {
        const int count = epoll_wait(epoll_fd, &event, 1, -1);
        if (count == 1 && event.data.fd == signal_fd &&
            (event.events & EPOLLIN) != 0)
        {
            break;
        }
        if (count < 0 && errno == EINTR) continue;
        return 14;
    }

    struct signalfd_siginfo info;
    if (read(signal_fd, &info, sizeof(info)) != (ssize_t)sizeof(info) ||
        info.ssi_signo != SIGTERM)
    {
        return 15;
    }
    write_marker("ASYNC_SIGNALFD_EPOLL=OK\n");
    (void)close(epoll_fd);
    (void)close(signal_fd);
    return 0;
}

static int run_loop(void)
{
    write_marker("ASYNC_LOOP_READY\n");
    for (;;) {
        __asm__ volatile("pause" ::: "memory");
    }
}

static int run_handler(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = sigint_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, 0) != 0) {
        return 2;
    }
    write_marker("ASYNC_HANDLER_READY\n");
    while (!handled) {
        __asm__ volatile("pause" ::: "memory");
    }
    if (!sse_stack_ok) {
        return 11;
    }
    write_marker("ASYNC_HANDLER_CONTINUED\n");
    return 0;
}

static int run_altstack(void)
{
    stack_t stack;
    memset(&stack, 0, sizeof(stack));
    stack.ss_sp = alternate_stack;
    stack.ss_size = sizeof(alternate_stack);
    if (sigaltstack(&stack, 0) != 0) {
        return 3;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = altstack_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, 0) != 0) {
        return 4;
    }
    write_marker("ASYNC_ALTSTACK_READY\n");
    while (!handled) {
        __asm__ volatile("pause" ::: "memory");
    }
    if (!altstack_ok) {
        return 5;
    }
    write_marker("ASYNC_ALTSTACK_CONTINUED\n");
    return 0;
}

static int child_status_matches(int status, int signal)
{
    if (WIFSIGNALED(status) && WTERMSIG(status) == signal) {
        return 1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 128 + signal;
}

static void write_child_status_failure(
    const char *mode,
    pid_t pid,
    int status,
    int signal)
{
    (void)dprintf(STDOUT_FILENO,
        "ASYNC_CHILD_FAILURE mode=%s stage=status rc=23 pid=%ld sent=%d expected=%d "
        "raw=0x%08x exited=%d exit=%d signaled=%d signal=%d\n",
        mode,
        (long)pid,
        signal,
        signal,
        (unsigned int)status,
        WIFEXITED(status) ? 1 : 0,
        WIFEXITED(status) ? WEXITSTATUS(status) : -1,
        WIFSIGNALED(status) ? 1 : 0,
        WIFSIGNALED(status) ? WTERMSIG(status) : -1);
}

static int run_signaled_child(const char *mode, int signal, int expect_signal)
{
    const pid_t pid = fork();
    if (pid < 0) {
        const int saved_errno = errno;
        (void)dprintf(STDOUT_FILENO,
            "ASYNC_CHILD_FAILURE mode=%s stage=fork rc=20 errno=%d\n",
            mode, saved_errno);
        return 20;
    }
    if (pid == 0) {
        if (strcmp(mode, "handler") == 0) {
            _exit(run_handler());
        }
        if (strcmp(mode, "altstack") == 0) {
            _exit(run_altstack());
        }
        if (strcmp(mode, "epoll") == 0) {
            _exit(run_epoll_handler());
        }
        if (strcmp(mode, "signalfd") == 0) {
            _exit(run_signalfd_handler());
        }
        _exit(run_loop());
    }
    sleep(1);
    if (kill(pid, signal) != 0) {
        const int saved_errno = errno;
        (void)dprintf(STDOUT_FILENO,
            "ASYNC_CHILD_FAILURE mode=%s stage=kill rc=21 pid=%ld sent=%d errno=%d\n",
            mode, (long)pid, signal, saved_errno);
        (void)kill(pid, SIGKILL);
        return 21;
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != pid) {
        const int saved_errno = errno;
        (void)dprintf(STDOUT_FILENO,
            "ASYNC_CHILD_FAILURE mode=%s stage=waitpid rc=22 pid=%ld got=%ld errno=%d\n",
            mode, (long)pid, (long)waited, saved_errno);
        return 22;
    }
    if (expect_signal != 0) {
        if (!child_status_matches(status, expect_signal)) {
            write_child_status_failure(mode, pid, status, expect_signal);
            return 23;
        }
        return 0;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 24;
}

static int run_busybox_timeout(const char *self)
{
    const pid_t pid = fork();
    if (pid < 0) {
        return 30;
    }
    if (pid == 0) {
        execl("/cmd/busybox", "busybox", "timeout", "1", self, "loop", (char *)0);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        return 31;
    }
    return WIFEXITED(status) &&
        (WEXITSTATUS(status) == 124 || WEXITSTATUS(status) == 128 + SIGTERM) ? 0 : 32;
}

static int run_suite(const char *self)
{
    if (run_signaled_child("loop", SIGINT, SIGINT) != 0) {
        return 40;
    }
    write_marker("ASYNC_SIGINT_DEFAULT=OK\n");
    if (run_signaled_child("loop", SIGKILL, SIGKILL) != 0) {
        return 41;
    }
    write_marker("ASYNC_SIGKILL=OK\n");
    if (run_busybox_timeout(self) != 0) {
        return 42;
    }
    write_marker("ASYNC_BUSYBOX_TIMEOUT=OK\n");
    if (run_signaled_child("handler", SIGINT, 0) != 0) {
        return 43;
    }
    write_marker("ASYNC_CUSTOM_HANDLER=OK\n");
    if (run_signaled_child("altstack", SIGINT, 0) != 0) {
        return 44;
    }
    write_marker("ASYNC_SIGALTSTACK=OK\n");
    if (run_signaled_child("epoll", SIGTERM, 0) != 0) {
        return 45;
    }
    write_marker("ASYNC_EPOLL_HANDLER=OK\n");
    if (run_signaled_child("signalfd", SIGTERM, 0) != 0) {
        return 46;
    }
    write_marker("ASYNC_SIGNALFD=OK\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 1;
    }
    if (strcmp(argv[1], "loop") == 0) {
        return run_loop();
    }
    if (strcmp(argv[1], "handler") == 0) {
        return run_handler();
    }
    if (strcmp(argv[1], "altstack") == 0) {
        return run_altstack();
    }
    if (strcmp(argv[1], "epoll") == 0) {
        return run_epoll_handler();
    }
    if (strcmp(argv[1], "signalfd") == 0) {
        return run_signalfd_handler();
    }
    if (strcmp(argv[1], "suite") == 0) {
        return run_suite(argv[0]);
    }
    if (strcmp(argv[1], "avx") == 0) {
        return run_avx_signal_roundtrip();
    }
    if (strcmp(argv[1], "avx-syscall") == 0) {
        return run_avx_syscall_roundtrip();
    }
    if (strcmp(argv[1], "avx-local") == 0) {
        return run_avx_local();
    }
    if (strcmp(argv[1], "avx-suite") == 0) {
        return run_avx_suite();
    }
    return 1;
}
