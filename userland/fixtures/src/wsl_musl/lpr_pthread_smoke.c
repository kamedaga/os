#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define LPR_FUTEX_WAIT_PRIVATE (0 | 128)
#define LPR_FUTEX_WAKE_PRIVATE (1 | 128)
#define LPR_FUTEX_REQUEUE_PRIVATE (3 | 128)
#define LPR_FUTEX_WAIT_BITSET_PRIVATE (9 | 128)
#define LPR_FUTEX_BITSET_MATCH_ANY 0xffffffffu

enum {
    THREAD_COUNT = 4,
    MUTEX_ITERATIONS = 2000,
};

static __thread int tls_value;
static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static int counter;

static pthread_mutex_t cond_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int cond_ready;
static int cond_go;
static int cond_observed;
static pthread_mutex_t broadcast_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t broadcast_cond = PTHREAD_COND_INITIALIZER;
static _Atomic int broadcast_ready;
static int broadcast_go;
static _Atomic int broadcast_observed;
static _Atomic int requeue_source_word;
static _Atomic int requeue_target_word;
static _Atomic int requeue_ready;
static _Atomic int requeue_observed;
static int detached_done;
static _Atomic int futex_bitset_word;
static _Atomic int futex_bitset_waiter_started;
static long futex_bitset_wait_result;
static int futex_bitset_wait_errno;
static struct timespec futex_bitset_deadline;
static _Atomic int stale_futex_word;
static _Atomic int stale_wait_phase;
static int stale_wait_pipe[2];
static long stale_timeout_result;
static int stale_timeout_errno;
static ssize_t stale_pipe_read_result;
static volatile sig_atomic_t alarm_seen;

static void emit(const char *message)
{
    (void)write(1, message, strlen(message));
}

static int clock_getres_smoke(void)
{
    struct timespec realtime;
    struct timespec monotonic;
    if (clock_getres(CLOCK_REALTIME, &realtime) != 0 ||
        clock_getres(CLOCK_MONOTONIC, &monotonic) != 0)
    {
        return 0;
    }
    if (realtime.tv_sec < 0 || realtime.tv_nsec < 0 ||
        realtime.tv_nsec >= 1000000000L ||
        (realtime.tv_sec == 0 && realtime.tv_nsec == 0))
    {
        return 0;
    }
    if (monotonic.tv_sec < 0 || monotonic.tv_nsec < 0 ||
        monotonic.tv_nsec >= 1000000000L ||
        (monotonic.tv_sec == 0 && monotonic.tv_nsec == 0))
    {
        return 0;
    }
    return 1;
}

static int fadvise64_smoke(void)
{
    const int fd = open("/cmd/lpr_pthread_static.elf", O_RDONLY);
    if (fd < 0) return 0;
    const int result = posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
    const int close_result = close(fd);
    return result == 0 && close_result == 0;
}

static void alarm_handler(int signo)
{
    if (signo == SIGALRM) alarm_seen = 1;
}

static int interval_signal_timer_smoke(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = alarm_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, 0) != 0) return 0;

    sigset_t blocked;
    sigset_t old_mask;
    sigset_t suspend_mask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &blocked, &old_mask) != 0) return 0;

    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));
    timer.it_value.tv_usec = 50000;
    alarm_seen = 0;
    if (setitimer(ITIMER_REAL, &timer, 0) != 0) return 0;

    struct itimerval current;
    memset(&current, 0, sizeof(current));
    if (getitimer(ITIMER_REAL, &current) != 0 ||
        (current.it_value.tv_sec == 0 && current.it_value.tv_usec == 0))
    {
        return 0;
    }

    sigemptyset(&suspend_mask);
    errno = 0;
    const int suspend_result = sigsuspend(&suspend_mask);
    const int suspend_errno = errno;
    memset(&timer, 0, sizeof(timer));
    const int disarm_result = setitimer(ITIMER_REAL, &timer, 0);
    const int restore_result = sigprocmask(SIG_SETMASK, &old_mask, 0);
    return suspend_result == -1 && suspend_errno == EINTR &&
        alarm_seen == 1 && disarm_result == 0 && restore_result == 0;
}

static void *join_worker(void *argument)
{
    const intptr_t index = (intptr_t)argument;
    tls_value = 100 + (int)index;
    return (void *)(intptr_t)(tls_value + 1);
}

static int create_join_smoke(void)
{
    pthread_t threads[THREAD_COUNT];
    for (intptr_t i = 0; i < THREAD_COUNT; ++i) {
        if (pthread_create(&threads[i], 0, join_worker, (void *)i) != 0) {
            return 0;
        }
    }
    for (intptr_t i = 0; i < THREAD_COUNT; ++i) {
        void *result = 0;
        if (pthread_join(threads[i], &result) != 0 ||
            (intptr_t)result != 101 + i)
        {
            return 0;
        }
    }
    return 1;
}

static void *counter_worker(void *argument)
{
    (void)argument;
    for (int i = 0; i < MUTEX_ITERATIONS; ++i) {
        if (pthread_mutex_lock(&counter_mutex) != 0) {
            return (void *)(intptr_t)1;
        }
        ++counter;
        if (pthread_mutex_unlock(&counter_mutex) != 0) {
            return (void *)(intptr_t)2;
        }
    }
    return 0;
}

static int mutex_smoke(void)
{
    pthread_t threads[THREAD_COUNT];
    counter = 0;
    for (int i = 0; i < THREAD_COUNT; ++i) {
        if (pthread_create(&threads[i], 0, counter_worker, 0) != 0) {
            return 0;
        }
    }
    for (int i = 0; i < THREAD_COUNT; ++i) {
        void *result = 0;
        if (pthread_join(threads[i], &result) != 0 || result != 0) {
            return 0;
        }
    }
    return counter == THREAD_COUNT * MUTEX_ITERATIONS;
}

static void *cond_worker(void *argument)
{
    (void)argument;
    if (pthread_mutex_lock(&cond_mutex) != 0) {
        return (void *)(intptr_t)1;
    }
    cond_ready = 1;
    if (pthread_cond_signal(&cond) != 0) {
        return (void *)(intptr_t)2;
    }
    while (!cond_go) {
        if (pthread_cond_wait(&cond, &cond_mutex) != 0) {
            return (void *)(intptr_t)3;
        }
    }
    cond_observed = 1;
    if (pthread_mutex_unlock(&cond_mutex) != 0) {
        return (void *)(intptr_t)4;
    }
    return 0;
}

static int cond_smoke(void)
{
    pthread_t thread;
    cond_ready = 0;
    cond_go = 0;
    cond_observed = 0;
    if (pthread_mutex_lock(&cond_mutex) != 0) {
        return 0;
    }
    if (pthread_create(&thread, 0, cond_worker, 0) != 0) {
        return 0;
    }
    while (!cond_ready) {
        if (pthread_cond_wait(&cond, &cond_mutex) != 0) {
            return 0;
        }
    }
    cond_go = 1;
    if (pthread_cond_signal(&cond) != 0 ||
        pthread_mutex_unlock(&cond_mutex) != 0)
    {
        return 0;
    }
    void *result = 0;
    return pthread_join(thread, &result) == 0 &&
        result == 0 && cond_observed == 1;
}

static void *cond_broadcast_worker(void *argument)
{
    (void)argument;
    if (pthread_mutex_lock(&broadcast_mutex) != 0) {
        return (void *)(intptr_t)1;
    }
    atomic_fetch_add_explicit(&broadcast_ready, 1, memory_order_release);
    while (!broadcast_go) {
        if (pthread_cond_wait(&broadcast_cond, &broadcast_mutex) != 0) {
            return (void *)(intptr_t)2;
        }
    }
    atomic_fetch_add_explicit(&broadcast_observed, 1, memory_order_relaxed);
    if (pthread_mutex_unlock(&broadcast_mutex) != 0) {
        return (void *)(intptr_t)3;
    }
    return 0;
}

static int cond_broadcast_smoke(void)
{
    pthread_t threads[THREAD_COUNT];
    atomic_store_explicit(&broadcast_ready, 0, memory_order_relaxed);
    atomic_store_explicit(&broadcast_observed, 0, memory_order_relaxed);
    broadcast_go = 0;
    for (int i = 0; i < THREAD_COUNT; ++i) {
        if (pthread_create(&threads[i], 0, cond_broadcast_worker, 0) != 0) {
            return 0;
        }
    }
    while (atomic_load_explicit(&broadcast_ready, memory_order_acquire) !=
        THREAD_COUNT)
    {
        sched_yield();
    }
    if (pthread_mutex_lock(&broadcast_mutex) != 0) {
        return 0;
    }
    broadcast_go = 1;
    if (pthread_cond_broadcast(&broadcast_cond) != 0 ||
        pthread_mutex_unlock(&broadcast_mutex) != 0)
    {
        return 0;
    }
    for (int i = 0; i < THREAD_COUNT; ++i) {
        void *result = 0;
        if (pthread_join(threads[i], &result) != 0 || result != 0) {
            return 0;
        }
    }
    return atomic_load_explicit(
        &broadcast_observed, memory_order_acquire) == THREAD_COUNT;
}

static void *futex_requeue_waiter(void *argument)
{
    (void)argument;
    atomic_fetch_add_explicit(&requeue_ready, 1, memory_order_release);
    errno = 0;
    const long result = syscall(
        SYS_futex,
        &requeue_source_word,
        LPR_FUTEX_WAIT_PRIVATE,
        0,
        0,
        0,
        0);
    if (result != 0 || errno != 0) return (void *)(intptr_t)1;
    atomic_fetch_add_explicit(&requeue_observed, 1, memory_order_release);
    return 0;
}

static int futex_requeue_smoke(void)
{
    enum { REQUEUE_WAITERS = 2 };
    const struct timespec settle_delay = { .tv_nsec = 250000000 };
    pthread_t threads[REQUEUE_WAITERS];
    atomic_store_explicit(&requeue_source_word, 0, memory_order_relaxed);
    atomic_store_explicit(&requeue_target_word, 0, memory_order_relaxed);
    atomic_store_explicit(&requeue_ready, 0, memory_order_relaxed);
    atomic_store_explicit(&requeue_observed, 0, memory_order_relaxed);
    for (int i = 0; i < REQUEUE_WAITERS; ++i) {
        if (pthread_create(&threads[i], 0, futex_requeue_waiter, 0) != 0) {
            return 0;
        }
    }
    while (atomic_load_explicit(&requeue_ready, memory_order_acquire) !=
        REQUEUE_WAITERS)
    {
        sched_yield();
    }
    (void)nanosleep(&settle_delay, 0);
    errno = 0;
    const long requeue_result = syscall(
        SYS_futex,
        &requeue_source_word,
        LPR_FUTEX_REQUEUE_PRIVATE,
        0,
        REQUEUE_WAITERS,
        &requeue_target_word,
        0);
    const int requeue_errno = errno;
    errno = 0;
    const long wake_result = syscall(
        SYS_futex,
        &requeue_target_word,
        LPR_FUTEX_WAKE_PRIVATE,
        REQUEUE_WAITERS,
        0,
        0,
        0);
    const int wake_errno = errno;
    for (int i = 0; i < REQUEUE_WAITERS; ++i) {
        void *result = 0;
        if (pthread_join(threads[i], &result) != 0 || result != 0) {
            return 0;
        }
    }
    return requeue_result == REQUEUE_WAITERS && requeue_errno == 0 &&
        wake_result == REQUEUE_WAITERS && wake_errno == 0 &&
        atomic_load_explicit(
            &requeue_observed, memory_order_acquire) == REQUEUE_WAITERS;
}

static void *detached_worker(void *argument)
{
    (void)argument;
    __atomic_store_n(&detached_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static int detached_exit_smoke(void)
{
    const struct timespec poll_delay = { .tv_nsec = 1000000 };
    const struct timespec settle_delay = { .tv_nsec = 100000000 };
    pthread_attr_t attr;
    pthread_t thread;
    detached_done = 0;
    if (pthread_attr_init(&attr) != 0 ||
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0 ||
        pthread_create(&thread, &attr, detached_worker, 0) != 0)
    {
        (void)pthread_attr_destroy(&attr);
        return 0;
    }
    (void)pthread_attr_destroy(&attr);
    for (int tick = 0; tick < 1000; ++tick) {
        if (__atomic_load_n(&detached_done, __ATOMIC_ACQUIRE) != 0) {
            /* The worker sets the flag immediately before musl's detached
             * __unmapself path.  Keep this process alive long enough for that
             * path to finish and expose a self-unmap teardown fault. */
            (void)nanosleep(&settle_delay, 0);
            return 1;
        }
        (void)nanosleep(&poll_delay, 0);
    }
    return 0;
}

static void *futex_bitset_waiter(void *argument)
{
    (void)argument;
    atomic_store_explicit(
        &futex_bitset_waiter_started, 1, memory_order_release);
    errno = 0;
    futex_bitset_wait_result = syscall(
        SYS_futex,
        &futex_bitset_word,
        LPR_FUTEX_WAIT_BITSET_PRIVATE,
        0,
        &futex_bitset_deadline,
        0,
        LPR_FUTEX_BITSET_MATCH_ANY);
    futex_bitset_wait_errno = errno;
    return 0;
}

static int futex_bitset_smoke(void)
{
    const struct timespec settle_delay = { .tv_nsec = 100000000 };
    pthread_t waiter;
    atomic_store_explicit(&futex_bitset_word, 0, memory_order_relaxed);
    atomic_store_explicit(
        &futex_bitset_waiter_started, 0, memory_order_relaxed);
    futex_bitset_wait_result = -1;
    futex_bitset_wait_errno = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &futex_bitset_deadline) != 0) return 0;
    futex_bitset_deadline.tv_sec += 5;
    if (pthread_create(&waiter, 0, futex_bitset_waiter, 0) != 0) return 0;
    while (atomic_load_explicit(
        &futex_bitset_waiter_started, memory_order_acquire) == 0)
    {
        sched_yield();
    }
    (void)nanosleep(&settle_delay, 0);
    errno = 0;
    const long wake_result = syscall(
        SYS_futex,
        &futex_bitset_word,
        LPR_FUTEX_WAKE_PRIVATE,
        1,
        0,
        0,
        0);
    const int wake_errno = errno;
    if (pthread_join(waiter, 0) != 0) return 0;
    return wake_result == 1 && wake_errno == 0 &&
        futex_bitset_wait_result == 0 && futex_bitset_wait_errno == 0;
}

static void *futex_timeout_then_pipe_waiter(void *argument)
{
    (void)argument;
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return 0;
    deadline.tv_sec += 1;
    errno = 0;
    stale_timeout_result = syscall(
        SYS_futex,
        &stale_futex_word,
        LPR_FUTEX_WAIT_BITSET_PRIVATE,
        0,
        &deadline,
        0,
        LPR_FUTEX_BITSET_MATCH_ANY);
    stale_timeout_errno = errno;
    atomic_store_explicit(&stale_wait_phase, 1, memory_order_release);
    char byte = 0;
    stale_pipe_read_result = read(stale_wait_pipe[0], &byte, 1);
    if (stale_pipe_read_result == 1 && byte == 'x') {
        atomic_store_explicit(&stale_wait_phase, 2, memory_order_release);
    }
    return 0;
}

static int futex_timeout_stale_smoke(void)
{
    const struct timespec settle_delay = { .tv_nsec = 250000000 };
    pthread_t waiter;
    if (pipe(stale_wait_pipe) != 0) return 0;
    atomic_store_explicit(&stale_futex_word, 0, memory_order_relaxed);
    atomic_store_explicit(&stale_wait_phase, 0, memory_order_relaxed);
    stale_timeout_result = 0;
    stale_timeout_errno = 0;
    stale_pipe_read_result = -1;
    if (pthread_create(&waiter, 0, futex_timeout_then_pipe_waiter, 0) != 0) {
        (void)close(stale_wait_pipe[0]);
        (void)close(stale_wait_pipe[1]);
        return 0;
    }
    while (atomic_load_explicit(&stale_wait_phase, memory_order_acquire) == 0) {
        sched_yield();
    }
    (void)nanosleep(&settle_delay, 0);
    errno = 0;
    const long stale_wake_result = syscall(
        SYS_futex,
        &stale_futex_word,
        LPR_FUTEX_WAKE_PRIVATE,
        1,
        0,
        0,
        0);
    const int stale_wake_errno = errno;
    const int pipe_was_still_blocked =
        atomic_load_explicit(&stale_wait_phase, memory_order_acquire) == 1;
    const ssize_t write_result = write(stale_wait_pipe[1], "x", 1);
    const int joined = pthread_join(waiter, 0) == 0;
    (void)close(stale_wait_pipe[0]);
    (void)close(stale_wait_pipe[1]);
    return stale_timeout_result == -1 &&
        stale_timeout_errno == ETIMEDOUT &&
        stale_wake_result == 0 && stale_wake_errno == 0 &&
        pipe_was_still_blocked && write_result == 1 && joined &&
        stale_pipe_read_result == 1 &&
        atomic_load_explicit(&stale_wait_phase, memory_order_acquire) == 2;
}

int main(void)
{
    emit("LPR_PTHREAD_START\n");
    if (!clock_getres_smoke()) {
        emit("LPR_PTHREAD_CLOCK_GETRES=BAD\n");
        return 1;
    }
    emit("LPR_PTHREAD_CLOCK_GETRES=OK\n");
    if (!fadvise64_smoke()) {
        emit("LPR_PTHREAD_FADVISE64=BAD\n");
        return 2;
    }
    emit("LPR_PTHREAD_FADVISE64=OK\n");
    if (!interval_signal_timer_smoke()) {
        emit("LPR_PTHREAD_INTERVAL_SIGNAL_TIMER=BAD\n");
        return 3;
    }
    emit("LPR_PTHREAD_INTERVAL_SIGNAL_TIMER=OK\n");
    if (!create_join_smoke()) {
        emit("LPR_PTHREAD_CREATE_JOIN=BAD\n");
        return 4;
    }
    emit("LPR_PTHREAD_CREATE_JOIN=OK\n");
    if (!mutex_smoke()) {
        emit("LPR_PTHREAD_MUTEX=BAD\n");
        return 4;
    }
    emit("LPR_PTHREAD_MUTEX=OK\n");
    if (!cond_smoke()) {
        emit("LPR_PTHREAD_COND=BAD\n");
        return 5;
    }
    emit("LPR_PTHREAD_COND=OK\n");
    if (!cond_broadcast_smoke()) {
        emit("LPR_PTHREAD_COND_BROADCAST=BAD\n");
        return 6;
    }
    emit("LPR_PTHREAD_COND_BROADCAST=OK\n");
    if (!futex_requeue_smoke()) {
        emit("LPR_PTHREAD_FUTEX_REQUEUE=BAD\n");
        return 7;
    }
    emit("LPR_PTHREAD_FUTEX_REQUEUE=OK\n");
    if (!detached_exit_smoke()) {
        emit("LPR_PTHREAD_DETACHED_EXIT=BAD\n");
        return 8;
    }
    emit("LPR_PTHREAD_DETACHED_EXIT=OK\n");
    if (!futex_bitset_smoke()) {
        emit("LPR_PTHREAD_FUTEX_WAIT_BITSET=BAD\n");
        return 9;
    }
    emit("LPR_PTHREAD_FUTEX_WAIT_BITSET=OK\n");
    if (!futex_timeout_stale_smoke()) {
        emit("LPR_PTHREAD_FUTEX_TIMEOUT_STALE=BAD\n");
        return 10;
    }
    emit("LPR_PTHREAD_FUTEX_TIMEOUT_STALE=OK\n");
    /* A detached musl thread exits while holding __thread_list_lock and asks
     * the kernel to clear it after the stack is unmapped.  Verify that the
     * next creator does not inherit a stale owner TID. */
    if (!create_join_smoke()) {
        emit("LPR_PTHREAD_POST_DETACHED_CREATE_JOIN=BAD\n");
        return 11;
    }
    emit("LPR_PTHREAD_POST_DETACHED_CREATE_JOIN=OK\n");
    return 0;
}
