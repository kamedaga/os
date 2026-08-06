#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
static int detached_done;

static void emit(const char *message)
{
    (void)write(1, message, strlen(message));
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

int main(void)
{
    emit("LPR_PTHREAD_START\n");
    if (!create_join_smoke()) {
        emit("LPR_PTHREAD_CREATE_JOIN=BAD\n");
        return 1;
    }
    emit("LPR_PTHREAD_CREATE_JOIN=OK\n");
    if (!mutex_smoke()) {
        emit("LPR_PTHREAD_MUTEX=BAD\n");
        return 2;
    }
    emit("LPR_PTHREAD_MUTEX=OK\n");
    if (!cond_smoke()) {
        emit("LPR_PTHREAD_COND=BAD\n");
        return 3;
    }
    emit("LPR_PTHREAD_COND=OK\n");
    if (!detached_exit_smoke()) {
        emit("LPR_PTHREAD_DETACHED_EXIT=BAD\n");
        return 4;
    }
    emit("LPR_PTHREAD_DETACHED_EXIT=OK\n");
    /* A detached musl thread exits while holding __thread_list_lock and asks
     * the kernel to clear it after the stack is unmapped.  Verify that the
     * next creator does not inherit a stale owner TID. */
    if (!create_join_smoke()) {
        emit("LPR_PTHREAD_POST_DETACHED_CREATE_JOIN=BAD\n");
        return 5;
    }
    emit("LPR_PTHREAD_POST_DETACHED_CREATE_JOIN=OK\n");
    return 0;
}
