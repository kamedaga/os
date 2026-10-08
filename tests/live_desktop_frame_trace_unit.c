/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "live_desktop_frame_profile.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
int main(void)
{
    void (*phase)(unsigned) = NULL, (*reset)(void) = NULL;
    void (*snapshot)(struct frame_trace_snapshot *) = NULL;
    *(void **)(&phase) = dlsym(RTLD_DEFAULT, "frame_trace_phase");
    *(void **)(&reset) = dlsym(RTLD_DEFAULT, "frame_trace_reset");
    *(void **)(&snapshot) = dlsym(RTLD_DEFAULT, "frame_trace_snapshot");
    assert(phase && reset && snapshot);
    reset(); phase(FRAME_PHASE_DRAW);
    unsigned argument = 0;
    assert(ioctl(-1, 0xc0046400, &argument) == -1 && errno == EBADF);
    phase(FRAME_PHASE_PRESENT);
    assert(poll(NULL, 0, 0) == 0);
    assert(poll(NULL, 0, 1) == 0);
    phase(FRAME_PHASE_COUNT);
    assert(poll(NULL, 0, 0) == 0);
    struct frame_trace_snapshot data; snapshot(&data);
    assert(data.entries[0].operation == 0xc0046400 && data.entries[0].calls == 1 &&
        data.entries[0].phase == FRAME_PHASE_DRAW && data.entries[0].errors == 1);
    assert(data.entries[1].operation == 0xffff0000 && data.entries[1].calls == 1 &&
        data.entries[1].phase == FRAME_PHASE_PRESENT);
    assert(data.entries[2].operation == 0xffff0001 && data.entries[2].calls == 1);
    assert(!data.entries[3].calls && !data.overflow);
    assert(data.polls[0].calls == 1 && data.polls[0].timeout == 0 && data.polls[0].timeouts == 1);
    assert(data.polls[1].calls == 1 && data.polls[1].timeout == 1 && data.polls[1].timeouts == 1);
    reset(); phase(FRAME_PHASE_PRESENT);
    int pair[2]; assert(pipe(pair) == 0);
    assert(write(pair[1], "x", 1) == 1);
    struct pollfd pending = {.fd = pair[0], .events = POLLIN};
    assert(poll(&pending, 1, -1) == 1);
    snapshot(&data);
    assert(data.polls[0].fd == pair[0] && data.polls[0].nfds == 1 &&
        data.polls[0].timeout == -1 && data.polls[0].events == POLLIN &&
        data.polls[0].revents == POLLIN && data.polls[0].ready == 1 &&
        !data.polls[0].timeouts && !data.polls[0].errors && !data.poll_overflow);
    close(pair[0]); close(pair[1]);
    reset(); phase(FRAME_PHASE_PRESENT);
    assert(poll((struct pollfd *)(uintptr_t)1, 1, 0) == -1 && errno == EFAULT);
    snapshot(&data);
    assert(data.polls[0].fd == -1 && data.polls[0].errors == 1 &&
        data.entries[0].errors == 1);
    reset(); phase(FRAME_PHASE_PRESENT);
    assert(pipe(pair) == 0);
    assert(write(pair[1], "x", 1) == 1);
    pending = (struct pollfd){.fd = pair[0], .events = POLLIN};
    for (unsigned i = 0; i <= FRAME_POLL_SLOTS; ++i)
        assert(poll(&pending, 1, -(int)i - 1) == 1);
    snapshot(&data);
    assert(data.poll_overflow == 1);
    for (unsigned i = 0; i < FRAME_POLL_SLOTS; ++i)
        assert(data.polls[i].calls == 1 && data.polls[i].ready == 1);
    close(pair[0]); close(pair[1]);
    reset(); phase(FRAME_PHASE_SUBMIT);
    for (unsigned i = 0; i <= FRAME_TRACE_SLOTS; ++i)
        assert(ioctl(-1, 0xc0046400 + i, &argument) == -1 && errno == EBADF);
    snapshot(&data);
    assert(data.overflow == 1);
    for (unsigned i = 0; i < FRAME_TRACE_SLOTS; ++i)
        assert(data.entries[i].calls == 1 && data.entries[i].phase == FRAME_PHASE_SUBMIT);
    puts("FRAME_TRACE: attribution, errors, zero/blocking polls, reset and explicit overflow PASS");
}
