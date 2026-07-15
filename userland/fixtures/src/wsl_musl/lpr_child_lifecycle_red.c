#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned char alternate_stack[SIGSTKSZ * 2u];
static volatile sig_atomic_t handler_count;
static volatile sig_atomic_t sigchld_handler_count;
static volatile sig_atomic_t sigusr1_handler_count;
static volatile sig_atomic_t handler_altstack_query_ok;
static volatile sig_atomic_t handler_on_altstack;
static volatile sig_atomic_t handler_local_on_altstack;

static void emit(const char *marker)
{
    (void)write(STDOUT_FILENO, marker, strlen(marker));
}

static int fail(const char *marker)
{
    emit(marker);
    return 1;
}

static void child_handler(int signo, siginfo_t *info, void *context)
{
    stack_t current;
    unsigned char local;
    const uintptr_t local_address = (uintptr_t)&local;
    const uintptr_t stack_start = (uintptr_t)alternate_stack;
    const uintptr_t stack_end = stack_start + sizeof(alternate_stack);

    (void)info;
    (void)context;
    if (signo == SIGCHLD) sigchld_handler_count += 1;
    if (signo == SIGUSR1) sigusr1_handler_count += 1;
    if (sigaltstack(0, &current) == 0) {
        handler_altstack_query_ok = 1;
        if ((current.ss_flags & SS_ONSTACK) != 0) {
            handler_on_altstack = 1;
        }
    }
    if (local_address >= stack_start && local_address < stack_end) {
        handler_local_on_altstack = 1;
    }
    handler_count += 1;
}

static int block_test_signals(void)
{
    sigset_t blocked;

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    sigaddset(&blocked, SIGUSR1);
    return sigprocmask(SIG_BLOCK, &blocked, 0);
}

enum sigchld_pending_state {
    SIGCHLD_PENDING_QUERY_FAILED = -1,
    SIGCHLD_NOT_PENDING = 0,
    SIGCHLD_PENDING = 1,
};

static enum sigchld_pending_state signal_pending_state(int signo)
{
    sigset_t pending;

    if (sigpending(&pending) != 0) return SIGCHLD_PENDING_QUERY_FAILED;
    return sigismember(&pending, signo) == 1 ? SIGCHLD_PENDING : SIGCHLD_NOT_PENDING;
}

static int run_iteration(int epoll_fd, int expected_status, int iteration)
{
    const sig_atomic_t count_before = handler_count;
    const sig_atomic_t sigchld_count_before = sigchld_handler_count;
    const sig_atomic_t sigusr1_count_before = sigusr1_handler_count;
    sigset_t unblocked;
    struct epoll_event event;
    int status = 0;

    handler_altstack_query_ok = 0;
    handler_on_altstack = 0;
    handler_local_on_altstack = 0;

    if (block_test_signals() != 0) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=SIGPROCMASK\n");
    }
    const enum sigchld_pending_state sigchld_pending_before_fork =
        signal_pending_state(SIGCHLD);
    const enum sigchld_pending_state sigusr1_pending_before_fork =
        signal_pending_state(SIGUSR1);
    if (sigchld_pending_before_fork == SIGCHLD_PENDING_QUERY_FAILED ||
        sigusr1_pending_before_fork == SIGCHLD_PENDING_QUERY_FAILED) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=SIGPENDING_QUERY\n");
    }
    if (sigchld_pending_before_fork == SIGCHLD_PENDING ||
        sigusr1_pending_before_fork == SIGCHLD_PENDING) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=PENDING_BEFORE_FORK\n");
    }

    const pid_t child = fork();
    if (child < 0) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=FORK\n");
    }
    if (child == 0) {
        if (kill(getppid(), SIGUSR1) != 0) _exit(125);
        _exit(expected_status);
    }

    // Keep both signals blocked long enough for the explicit SIGUSR1 and the
    // child's exit notification to become pending before epoll_pwait applies
    // its temporary empty mask.
    usleep(100000);
    sigemptyset(&unblocked);
    for (int attempt = 0;
         attempt < 2 &&
            (sigchld_handler_count == sigchld_count_before ||
             sigusr1_handler_count == sigusr1_count_before);
         ++attempt)
    {
        errno = 0;
        if (epoll_pwait(epoll_fd, &event, 1, 5000, &unblocked) != -1) {
            return fail("LPR_CHILD_LIFECYCLE_RED_BAD=EPOLL_RETURN\n");
        }
        if (errno != EINTR) {
            return fail("LPR_CHILD_LIFECYCLE_RED_BAD=EPOLL_ERRNO\n");
        }
    }
    {
        sigset_t current_mask;

        if (sigprocmask(SIG_SETMASK, NULL, &current_mask) != 0 ||
            sigismember(&current_mask, SIGCHLD) != 1 ||
            sigismember(&current_mask, SIGUSR1) != 1) {
            return fail("LPR_CHILD_LIFECYCLE_RED_BAD=MASK_RESTORE\n");
        }
    }
    if (handler_count != count_before + 2) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=HANDLER_COUNT\n");
    }
    if (sigchld_handler_count != sigchld_count_before + 1 ||
        sigusr1_handler_count != sigusr1_count_before + 1) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=HANDLER_SIGNAL\n");
    }
    if (!handler_altstack_query_ok) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=ALTSTACK_QUERY\n");
    }
    if (!handler_on_altstack) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=ALTSTACK_FLAG\n");
    }
    if (!handler_local_on_altstack) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=ALTSTACK_LOCAL\n");
    }

    if (waitpid(child, &status, 0) != child) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=WAITPID\n");
    }
    if (!WIFEXITED(status)) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=WAITPID_NOT_EXITED\n");
    }
    if (WEXITSTATUS(status) != expected_status) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=WAITPID_STATUS\n");
    }

    errno = 0;
    if (waitpid(child, &status, WNOHANG) != -1 || errno != ECHILD) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=WAITPID_STALE\n");
    }
    const enum sigchld_pending_state sigchld_pending_after_reap =
        signal_pending_state(SIGCHLD);
    const enum sigchld_pending_state sigusr1_pending_after_reap =
        signal_pending_state(SIGUSR1);
    if (sigchld_pending_after_reap == SIGCHLD_PENDING_QUERY_FAILED ||
        sigusr1_pending_after_reap == SIGCHLD_PENDING_QUERY_FAILED) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=SIGPENDING_QUERY\n");
    }
    if (sigchld_pending_after_reap == SIGCHLD_PENDING ||
        sigusr1_pending_after_reap == SIGCHLD_PENDING) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=PENDING_AFTER_REAP\n");
    }

    if (iteration == 1) {
        emit("LPR_CHILD_LIFECYCLE_RED_ITERATION_1=OK\n");
    } else {
        emit("LPR_CHILD_LIFECYCLE_RED_ITERATION_2=OK\n");
    }
    return 0;
}

int main(void)
{
    stack_t stack;
    struct sigaction action;
    int epoll_fd;
    int status = 0;

    memset(&stack, 0, sizeof(stack));
    stack.ss_sp = alternate_stack;
    stack.ss_size = sizeof(alternate_stack);
    if (sigaltstack(&stack, 0) != 0) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=SIGALTSTACK\n");
    }

    memset(&action, 0, sizeof(action));
    action.sa_sigaction = child_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGCHLD, &action, 0) != 0) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=SIGACTION\n");
    }
    if (sigaction(SIGUSR1, &action, 0) != 0) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=SIGACTION\n");
    }

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=EPOLL_CREATE\n");
    }

    emit("LPR_CHILD_LIFECYCLE_RED_START\n");
    if (run_iteration(epoll_fd, 37, 1) != 0) return 1;
    if (run_iteration(epoll_fd, 73, 2) != 0) return 1;

    errno = 0;
    if (waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=WAITPID_ANY\n");
    }
    emit("LPR_CHILD_LIFECYCLE_RED_NO_CHILDREN=OK\n");

    if (close(epoll_fd) != 0) {
        return fail("LPR_CHILD_LIFECYCLE_RED_BAD=EPOLL_CLOSE\n");
    }
    emit("LPR_CHILD_LIFECYCLE_RED_DONE\n");
    return 0;
}
