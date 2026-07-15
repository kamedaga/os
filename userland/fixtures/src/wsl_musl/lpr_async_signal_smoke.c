#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t handled;
static volatile sig_atomic_t altstack_ok;
static volatile sig_atomic_t sse_stack_ok;
static unsigned char alternate_stack[SIGSTKSZ * 2];
static int terminate_event_fd = -1;

typedef float aligned_vec4_t __attribute__((vector_size(16)));

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
    const pid_t waited = waitpid(pid, &status, 0);
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
    if (strcmp(argv[1], "suite") == 0) {
        return run_suite(argv[0]);
    }
    return 1;
}
