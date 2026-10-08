/* SPDX-License-Identifier: MIT */
#ifndef GPUD_CONTROL_POLL_H
#define GPUD_CONTROL_POLL_H

#include <stdint.h>

enum { GPUD_CONTROL_POLL_MS = 1000 };

struct gpud_control_poll {
    uint64_t deadline_ms;
};

/* The RAM command file has no change-notification capability. Check it at
 * the same cadence when busy as when idle: loading a file for every DRM
 * request needlessly creates VMOs and blocks the display on filed RPCs. */
static inline int gpud_control_poll_due(
    struct gpud_control_poll *poll, uint64_t now_ms) {
    if (now_ms < poll->deadline_ms)
        return 0;
    poll->deadline_ms = now_ms > UINT64_MAX - GPUD_CONTROL_POLL_MS ?
        UINT64_MAX : now_ms + GPUD_CONTROL_POLL_MS;
    return 1;
}

/* Do not start a new full interval on every wakeup. Otherwise continuously
 * arriving DRM requests could postpone a stop command indefinitely. */
static inline uint64_t gpud_control_poll_timeout(
    const struct gpud_control_poll *poll, uint64_t now_ms) {
    return now_ms < poll->deadline_ms ? poll->deadline_ms - now_ms : 0;
}

#endif
