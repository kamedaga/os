#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int owner_wake_pipe[2] = {-1, -1};
static int worker_wait_pipe[2] = {-1, -1};
static atomic_int worker_ready;
static volatile sig_atomic_t handler_count;
static volatile sig_atomic_t handler_tid;
static pid_t owner_tid;
static pid_t worker_tid;

static void emit(const char *message)
{
    (void)write(STDOUT_FILENO, message, strlen(message));
}

static void term_handler(int signo)
{
    static const char owner_marker[] = "SIGNAL_OWNER_HANDLER=OWNER\n";
    static const char worker_marker[] = "SIGNAL_OWNER_HANDLER=WORKER\n";
    const pid_t tid = (pid_t)syscall(SYS_gettid);
    const char wake = 's';
    if (signo != SIGTERM) return;
    handler_tid = tid;
    handler_count += 1;
    if (tid == owner_tid) {
        (void)write(STDOUT_FILENO, owner_marker, sizeof(owner_marker) - 1u);
    } else {
        (void)write(STDOUT_FILENO, worker_marker, sizeof(worker_marker) - 1u);
    }
    (void)write(owner_wake_pipe[1], &wake, 1);
}

static void *blocking_worker(void *argument)
{
    (void)argument;
    worker_tid = (pid_t)syscall(SYS_gettid);
    atomic_store_explicit(&worker_ready, 1, memory_order_release);
    emit("SIGNAL_OWNER_WORKER_BLOCKING\n");
    struct pollfd item = {
        .fd = worker_wait_pipe[0],
        .events = POLLIN,
    };
    for (;;) {
        const int status = poll(&item, 1, -1);
        if (status < 0 && errno == EINTR) continue;
        if (status == 1) return 0;
        return (void *)(intptr_t)1;
    }
}

static int run_target(int ready_fd)
{
    owner_tid = (pid_t)syscall(SYS_gettid);
    if (pipe(owner_wake_pipe) != 0 || pipe(worker_wait_pipe) != 0) { emit("SIGNAL_OWNER_FAIL=pipe\n"); return 20; }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = term_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, 0) != 0) { emit("SIGNAL_OWNER_FAIL=sigaction\n"); return 21; }

    /* Probe: can the forked child map a thread-stack-sized region at all? */
    {
        void *probe = mmap(0, 131072, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        char buffer[80];
        const int length = snprintf(buffer, sizeof(buffer),
            "SIGNAL_OWNER_PROBE=mmap result=%s errno=%d\n",
            probe == MAP_FAILED ? "FAIL" : "OK",
            probe == MAP_FAILED ? errno : 0);
        if (length > 0) (void)write(STDOUT_FILENO, buffer, (size_t)length);
        if (probe != MAP_FAILED) (void)munmap(probe, 131072);
    }

    pthread_t worker;
    const int create_status = pthread_create(&worker, 0, blocking_worker, 0);
    if (create_status != 0) {
        char buffer[64];
        const int length = snprintf(buffer, sizeof(buffer),
            "SIGNAL_OWNER_FAIL=pthread_create rc=%d\n", create_status);
        if (length > 0) (void)write(STDOUT_FILENO, buffer, (size_t)length);
        return 22;
    }
    while (!atomic_load_explicit(&worker_ready, memory_order_acquire)) {
        const struct timespec delay = {.tv_nsec = 1000000};
        (void)nanosleep(&delay, 0);
    }

    const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event interest;
    memset(&interest, 0, sizeof(interest));
    interest.events = EPOLLIN;
    interest.data.fd = owner_wake_pipe[0];
    if (epoll_fd < 0 ||
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, owner_wake_pipe[0], &interest) != 0)
    {
        emit("SIGNAL_OWNER_FAIL=epoll_setup\n");
        return 23;
    }

    emit("SIGNAL_OWNER_EVENT_LOOP_BLOCKING\n");
    const char ready = 'r';
    if (write(ready_fd, &ready, 1) != 1) { emit("SIGNAL_OWNER_FAIL=ready_write\n"); return 24; }

    struct epoll_event event;
    for (;;) {
        const int count = epoll_wait(epoll_fd, &event, 1, -1);
        if (count == 1 && event.data.fd == owner_wake_pipe[0] &&
            (event.events & EPOLLIN) != 0)
        {
            break;
        }
        if (count < 0 && errno == EINTR) continue;
        emit("SIGNAL_OWNER_FAIL=epoll_wait\n");
        return 25;
    }

    char byte = 0;
    if (read(owner_wake_pipe[0], &byte, 1) != 1 || byte != 's') { emit("SIGNAL_OWNER_FAIL=wake_read\n"); return 26; }
    if (handler_count != 1) {
        emit("SIGNAL_OWNER_DELIVERY=BAD_COUNT\n");
        return 27;
    }
    if (handler_tid != owner_tid || handler_tid == worker_tid) {
        emit("SIGNAL_OWNER_DELIVERY=WRONG_THREAD\n");
        return 28;
    }
    emit("SIGNAL_OWNER_DELIVERY=OK\n");
    return 0;
}

int main(void)
{
    int target_ready[2] = {-1, -1};
    if (pipe(target_ready) != 0) return 1;
    emit("SIGNAL_OWNER_START\n");

    /* Occupy a lower scheduler slot before the target is forked.  Releasing
     * it before pthread_create makes the target worker reuse that slot, so
     * current first-blocked-slot delivery deterministically selects worker. */
    const pid_t holder = fork();
    if (holder < 0) return 2;
    if (holder == 0) {
        for (;;) pause();
    }

    const pid_t target = fork();
    if (target < 0) {
        (void)kill(holder, SIGKILL);
        return 3;
    }
    if (target == 0) {
        (void)close(target_ready[0]);
        _exit(run_target(target_ready[1]));
    }

    (void)close(target_ready[1]);
    if (kill(holder, SIGKILL) != 0) return 4;
    int holder_status = 0;
    if (waitpid(holder, &holder_status, 0) != holder) return 5;

    char ready = 0;
    if (read(target_ready[0], &ready, 1) != 1 || ready != 'r') return 6;
    const struct timespec settle = {.tv_nsec = 100000000};
    (void)nanosleep(&settle, 0);
    emit("SIGNAL_OWNER_PROCESS_SIGNAL_SENT\n");
    if (kill(target, SIGTERM) != 0) return 7;

    int target_status = 0;
    if (waitpid(target, &target_status, 0) != target) return 8;
    if (!WIFEXITED(target_status) || WEXITSTATUS(target_status) != 0) {
        emit("SIGNAL_OWNER_RESULT=BAD\n");
        return 9;
    }
    emit("SIGNAL_OWNER_DONE\n");
    return 0;
}
