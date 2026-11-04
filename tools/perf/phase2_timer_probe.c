#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum { MAX_SAMPLES = 1200 };

static uint64_t mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) abort();
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static int compare_u64(const void *left, const void *right)
{
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : a > b;
}

static void report(const char *kind, uint64_t requested_ns,
                   uint64_t *samples, size_t count, unsigned errors)
{
    qsort(samples, count, sizeof(*samples), compare_u64);
    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i) total += samples[i];
    const size_t p50 = (count - 1) * 50 / 100;
    const size_t p95 = (count - 1) * 95 / 100;
    const size_t p99 = (count - 1) * 99 / 100;
    printf("RESULT kind=%s requested_ns=%llu count=%zu errors=%u "
           "min_ns=%llu p50_ns=%llu p95_ns=%llu p99_ns=%llu max_ns=%llu avg_ns=%llu\n",
           kind, (unsigned long long)requested_ns, count, errors,
           (unsigned long long)samples[0],
           (unsigned long long)samples[p50],
           (unsigned long long)samples[p95],
           (unsigned long long)samples[p99],
           (unsigned long long)samples[count - 1],
           (unsigned long long)(total / count));
}

static void probe_nanosleep(uint64_t requested_ns, size_t count)
{
    uint64_t samples[MAX_SAMPLES];
    unsigned errors = 0;
    const struct timespec request = {
        .tv_sec = (time_t)(requested_ns / UINT64_C(1000000000)),
        .tv_nsec = (long)(requested_ns % UINT64_C(1000000000)),
    };
    for (size_t i = 0; i < count; ++i) {
        const uint64_t start = mono_ns();
        struct timespec remaining = request;
        while (nanosleep(&remaining, &remaining) != 0) {
            if (errno != EINTR) { ++errors; break; }
        }
        samples[i] = mono_ns() - start;
    }
    report("nanosleep", requested_ns, samples, count, errors);
}

static int futex_wait(volatile uint32_t *word, uint32_t expected,
                      const struct timespec *timeout)
{
    return (int)syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, expected,
                        timeout, NULL, 0);
}

static int futex_wake(volatile uint32_t *word, int count)
{
    return (int)syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, count,
                        NULL, NULL, 0);
}

static void probe_futex_timeout(uint64_t requested_ns, size_t count)
{
    uint64_t samples[MAX_SAMPLES];
    unsigned errors = 0;
    volatile uint32_t word = 0;
    const struct timespec request = {
        .tv_sec = (time_t)(requested_ns / UINT64_C(1000000000)),
        .tv_nsec = (long)(requested_ns % UINT64_C(1000000000)),
    };
    for (size_t i = 0; i < count; ++i) {
        errno = 0;
        const uint64_t start = mono_ns();
        const int result = futex_wait(&word, 0, &request);
        samples[i] = mono_ns() - start;
        if (result != -1 || errno != ETIMEDOUT) ++errors;
    }
    report("futex_timeout", requested_ns, samples, count, errors);
}

struct wake_case {
    volatile uint32_t word;
    atomic_uint ready;
    uint64_t returned_ns;
    int result;
    int error;
};

static void *wake_waiter(void *raw)
{
    struct wake_case *test = raw;
    atomic_store_explicit(&test->ready, 1, memory_order_release);
    errno = 0;
    test->result = futex_wait(&test->word, 0, NULL);
    test->error = errno;
    test->returned_ns = mono_ns();
    return NULL;
}

static void short_pause(void)
{
    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };
    nanosleep(&pause, NULL);
}

static void probe_futex_wake(size_t count)
{
    uint64_t samples[MAX_SAMPLES];
    unsigned errors = 0;
    for (size_t i = 0; i < count; ++i) {
        struct wake_case test = {0};
        pthread_t thread;
        if (pthread_create(&thread, NULL, wake_waiter, &test) != 0) abort();
        while (!atomic_load_explicit(&test.ready, memory_order_acquire)) sched_yield();
        short_pause();
        const uint64_t sent = mono_ns();
        const int woken = futex_wake(&test.word, 1);
        pthread_join(thread, NULL);
        samples[i] = test.returned_ns - sent;
        if (woken != 1 || test.result != 0) ++errors;
    }
    report("futex_wake", 0, samples, count, errors);
}

static void signal_handler(int signal_number) { (void)signal_number; }

struct interrupt_case {
    atomic_uint ready;
    uint64_t returned_ns;
    int result;
    int error;
    int use_futex;
    volatile uint32_t word;
};

static void *interrupt_waiter(void *raw)
{
    struct interrupt_case *test = raw;
    const struct timespec long_wait = { .tv_sec = 5, .tv_nsec = 0 };
    atomic_store_explicit(&test->ready, 1, memory_order_release);
    errno = 0;
    test->result = test->use_futex
        ? futex_wait(&test->word, 0, &long_wait)
        : nanosleep(&long_wait, NULL);
    test->error = errno;
    test->returned_ns = mono_ns();
    return NULL;
}

static void probe_signal_interrupt(const char *kind, int use_futex, size_t count)
{
    uint64_t samples[MAX_SAMPLES];
    unsigned errors = 0;
    unsigned returned_zero = 0;
    unsigned returned_eintr = 0;
    unsigned returned_other = 0;
    int first_other_result = 0;
    int first_other_error = 0;
    for (size_t i = 0; i < count; ++i) {
        struct interrupt_case test = { .use_futex = use_futex };
        pthread_t thread;
        if (pthread_create(&thread, NULL, interrupt_waiter, &test) != 0) abort();
        while (!atomic_load_explicit(&test.ready, memory_order_acquire)) sched_yield();
        short_pause();
        const uint64_t sent = mono_ns();
        const int sent_status = pthread_kill(thread, SIGUSR1);
        pthread_join(thread, NULL);
        samples[i] = test.returned_ns - sent;
        if (test.result == 0) ++returned_zero;
        else if (test.result == -1 && test.error == EINTR) ++returned_eintr;
        else {
            if (returned_other == 0) {
                first_other_result = test.result;
                first_other_error = test.error;
            }
            ++returned_other;
        }
        if (sent_status != 0 || test.result != -1 || test.error != EINTR) ++errors;
    }
    report(kind, 0, samples, count, errors);
    printf("DETAIL kind=%s returned_zero=%u returned_eintr=%u returned_other=%u "
           "first_other_result=%d first_other_errno=%d\n",
           kind, returned_zero, returned_eintr, returned_other,
           first_other_result, first_other_error);
}

int main(int argc, char **argv)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0) {
        perror("sigaction");
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "--interrupt-only") == 0) {
        probe_signal_interrupt("signal_nanosleep", 0, 200);
        probe_signal_interrupt("signal_futex", 1, 200);
        return 0;
    }
    probe_nanosleep(UINT64_C(1000000), 1000);
    probe_nanosleep(UINT64_C(10000000), 300);
    probe_nanosleep(UINT64_C(100000000), 100);
    probe_futex_timeout(UINT64_C(1000000), 1000);
    probe_futex_timeout(UINT64_C(10000000), 300);
    probe_futex_timeout(UINT64_C(100000000), 100);
    probe_futex_wake(300);
    probe_signal_interrupt("signal_nanosleep", 0, 200);
    probe_signal_interrupt("signal_futex", 1, 200);
    return 0;
}
