/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "live_desktop_frame_profile.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static atomic_int stopping, failed;
static atomic_ullong completed;

static uint64_t monotonic_us(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return UINT64_MAX;
    return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static void *timer_worker(void *unused)
{
    (void)unused;
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (fd < 0) { atomic_store(&failed, 1); return NULL; }
    while (!atomic_load(&stopping)) {
        const struct itimerspec deadline = {.it_value = {.tv_nsec = 500000}};
        struct pollfd pending = {.fd = fd, .events = POLLIN};
        uint64_t expirations;
        if (timerfd_settime(fd, 0, &deadline, NULL) || poll(&pending, 1, 1000) != 1 ||
            !(pending.revents & POLLIN) || read(fd, &expirations, sizeof(expirations)) !=
            sizeof(expirations) || !expirations) {
            atomic_store(&failed, 1); break;
        }
        atomic_fetch_add(&completed, 1);
    }
    close(fd);
    return NULL;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 2 || (strcmp(argv[1], "idle") && strcmp(argv[1], "stress"))) return 2;
    const int stress = !strcmp(argv[1], "stress");
    pthread_t workers[4];
    unsigned started = 0;
    if (stress) {
        for (; started < 4; ++started) {
            if (pthread_create(&workers[started], NULL, timer_worker, NULL)) {
                atomic_store(&failed, 1); break;
            }
        }
    }
    struct timespec resolution;
    if (clock_getres(CLOCK_MONOTONIC, &resolution)) return 1;
    printf("CLOCK_SOURCE mode=%s resolution_ns=%llu workers=%u\n", argv[1],
        (unsigned long long)((uint64_t)resolution.tv_sec * 1000000000 + resolution.tv_nsec), started);
    for (unsigned sample = 0; sample < 3 && !atomic_load(&failed); ++sample) {
        const uint64_t begin = monotonic_us(), cycles = frame_cycles();
        if (begin == UINT64_MAX) { atomic_store(&failed, 1); break; }
        printf("CLOCK_BEGIN sample=%u mode=%s mono_us=%llu tsc=%llu\n", sample,
            argv[1], (unsigned long long)begin, (unsigned long long)cycles);
        struct timespec remaining = {.tv_sec = 2};
        while (nanosleep(&remaining, &remaining)) {
            if (errno != EINTR) { atomic_store(&failed, 1); break; }
        }
        const uint64_t finish = monotonic_us();
        if (finish == UINT64_MAX || finish < begin) { atomic_store(&failed, 1); break; }
        printf("CLOCK_END sample=%u mode=%s elapsed_us=%llu elapsed_cycles=%llu "
            "timer_completions=%llu\n", sample, argv[1],
            (unsigned long long)(finish - begin), (unsigned long long)(frame_cycles() - cycles),
            (unsigned long long)atomic_load(&completed));
    }
    atomic_store(&stopping, 1);
    for (unsigned i = 0; i < started; ++i) pthread_join(workers[i], NULL);
    printf("CLOCK_RESULT mode=%s status=%s timer_completions=%llu\n", argv[1],
        atomic_load(&failed) ? "FAIL" : "PASS", (unsigned long long)atomic_load(&completed));
    return atomic_load(&failed) ? 1 : 0;
}
