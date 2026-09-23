#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

/* Regress the real allocator yield path without allocating pressure objects. */
static _Thread_local uintptr_t cookie;
static atomic_uint finished;
static volatile sig_atomic_t signals;

static void on_signal(int signo)
{
    (void)signo;
    signals++;
}

static void *worker(void *argument)
{
    uintptr_t id = (uintptr_t)argument;
    cookie = id;
    for (unsigned i = 0; i < 256; i++) {
        errno = EAGAIN;
        if (sched_yield() != 0 || errno != EAGAIN || cookie != id)
            return (void *)1;
        /* A syscall must preserve user SIMD state, unlike a C function call. */
        uint64_t expected[2] = { id, i }, actual[2];
        long result;
        __asm__ volatile (
            "movdqu %2, %%xmm15\n\t"
            "syscall\n\t"
            "movdqu %%xmm15, %1"
            : "=a"(result), "=m"(actual)
            : "m"(expected), "a"(24L)
            : "rcx", "r11", "xmm15", "memory", "cc");
        if (result != 0 || actual[0] != id || actual[1] != i || cookie != id)
            return (void *)2;
    }
    atomic_fetch_add(&finished, 1);
    return NULL;
}

int main(void)
{
    pthread_t threads[2];
    struct sigaction action = { .sa_handler = on_signal };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0) return 1;
    for (uintptr_t i = 0; i < 2; i++)
        if (pthread_create(&threads[i], NULL, worker, (void *)(i + 1)) != 0)
            return 2;
    if (raise(SIGUSR1) != 0) return 3;
    for (unsigned i = 0; i < 2; i++) {
        void *result;
        if (pthread_join(threads[i], &result) != 0 || result != NULL) return 4;
    }
    if (atomic_load(&finished) != 2 || signals != 1) return 5;
    puts("sched_yield guest PASS: return errno TLS SIMD signal thread-exit");
    return 0;
}
