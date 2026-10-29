/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "live_desktop_frame_profile.h"
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>

static unsigned active_phase = FRAME_PHASE_COUNT;
static _Thread_local unsigned nested;
#if defined(__GLIBC__)
typedef unsigned long frame_ioctl_request_t;
#else
typedef int frame_ioctl_request_t;
#endif
static int (*next_ioctl)(int, frame_ioctl_request_t, ...);
static int (*next_poll)(struct pollfd *, nfds_t, int);
static struct frame_trace_snapshot counters;
static unsigned lock;
static void acquire(void)
{
    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}
static void release(void) { __atomic_store_n(&lock, 0, __ATOMIC_RELEASE); }
__attribute__((constructor)) static void resolve(void)
{
    *(void **)(&next_ioctl) = dlsym(RTLD_NEXT, "ioctl");
    *(void **)(&next_poll) = dlsym(RTLD_NEXT, "poll");
}
void frame_trace_phase(unsigned phase) { __atomic_store_n(&active_phase, phase, __ATOMIC_RELEASE); }
void frame_trace_reset(void)
{
    acquire(); memset(&counters, 0, sizeof(counters)); release();
}
void frame_trace_snapshot(struct frame_trace_snapshot *out)
{
    acquire(); *out = counters; release();
}
static void record(unsigned phase, uint32_t operation, uint64_t start, int failed)
{
    const uint64_t elapsed = frame_cycles() - start;
    if (phase >= FRAME_PHASE_COUNT) return;
    acquire();
    for (unsigned i = 0; i < FRAME_TRACE_SLOTS; ++i) {
        struct frame_trace_entry *entry = &counters.entries[i];
        if (entry->calls && (entry->phase != phase || entry->operation != operation))
            continue;
        entry->phase = phase; entry->operation = operation;
        ++entry->calls; entry->cycles += elapsed; entry->errors += failed;
        if (elapsed > entry->maximum) entry->maximum = elapsed;
        release(); return;
    }
    ++counters.overflow; release();
}
int ioctl(int fd, frame_ioctl_request_t operation, ...)
{
    if (!next_ioctl) { errno = ENOSYS; return -1; }
    va_list arguments; va_start(arguments, operation);
    /* This interposer is scoped to pointer-argument DRM calls in this test;
     * it is not injected into arbitrary programs with no-argument ioctls. */
    void *argument = va_arg(arguments, void *); va_end(arguments);
    const unsigned reentered = nested++;
    const unsigned phase = __atomic_load_n(&active_phase, __ATOMIC_ACQUIRE);
    const uint64_t start = frame_cycles();
    int result = next_ioctl(fd, operation, argument), saved_errno = errno;
    if (!reentered) record(phase, (uint32_t)operation, start, result < 0);
    --nested; errno = saved_errno; return result;
}
int poll(struct pollfd *fds, nfds_t count, int timeout)
{
    if (!next_poll) { errno = ENOSYS; return -1; }
    const unsigned reentered = nested++;
    const unsigned phase = __atomic_load_n(&active_phase, __ATOMIC_ACQUIRE);
    const uint64_t start = frame_cycles();
    int result = next_poll(fds, count, timeout), saved_errno = errno;
    if (!reentered) {
        record(phase, timeout == 0 ? 0xffff0000u : 0xffff0001u, start, result < 0);
        /* Capture only the call shape during the timed loop. Resolving an FD
         * through fstat/socket queries here would add IPC to the wait itself. */
        const uint64_t elapsed = frame_cycles() - start;
        if (phase < FRAME_PHASE_COUNT) {
            /* A rejected poll may have an invalid userspace pointer. Keep
             * errno attribution without dereferencing that failed argument. */
            const int fd = result >= 0 && fds && count == 1 ? fds[0].fd : -1;
            unsigned events = 0, revents = 0;
            if (fds && result >= 0) {
                for (nfds_t i = 0; i < count; ++i) {
                    events |= (unsigned short)fds[i].events;
                    revents |= (unsigned short)fds[i].revents;
                }
            }
            acquire();
            unsigned i;
            for (i = 0; i < FRAME_POLL_SLOTS; ++i) {
                struct frame_trace_poll_entry *entry = &counters.polls[i];
                if (entry->calls && (entry->phase != phase || entry->fd != fd ||
                    entry->nfds != count || entry->timeout != timeout || entry->events != events))
                    continue;
                entry->phase = phase; entry->fd = fd; entry->nfds = count;
                entry->timeout = timeout; entry->events = events; entry->revents |= revents;
                ++entry->calls; entry->cycles += elapsed;
                if (elapsed > entry->maximum) entry->maximum = elapsed;
                entry->ready += result > 0; entry->timeouts += result == 0;
                entry->errors += result < 0;
                break;
            }
            if (i == FRAME_POLL_SLOTS) ++counters.poll_overflow;
            release();
        }
    }
    --nested; errno = saved_errno; return result;
}
