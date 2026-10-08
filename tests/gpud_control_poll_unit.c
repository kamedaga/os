/* SPDX-License-Identifier: MIT */
#include "../userland/gpud/control_poll.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    struct gpud_control_poll poll = {0};
    assert(gpud_control_poll_due(&poll, 12000));
    assert(gpud_control_poll_timeout(&poll, 12000) == 1000);
    /* A stream of requests must neither reload the file nor move its
     * deadline. The idle wait also uses the remaining interval. */
    for (uint64_t now = 12001; now < 13000; ++now) {
        assert(!gpud_control_poll_due(&poll, now));
        assert(gpud_control_poll_timeout(&poll, now) == 13000 - now);
    }
    assert(gpud_control_poll_timeout(&poll, 13000) == 0);
    assert(gpud_control_poll_due(&poll, 13000));
    /* A long ioctl must not trigger a burst of overdue file reads. */
    assert(gpud_control_poll_due(&poll, 17000));
    assert(!gpud_control_poll_due(&poll, 17000));
    assert(gpud_control_poll_timeout(&poll, 17000) == 1000);
    /* A late wakeup after completing file I/O is immediately eligible. */
    assert(gpud_control_poll_timeout(&poll, 18001) == 0);
    assert(gpud_control_poll_due(&poll, 18001));
    assert(gpud_control_poll_due(&poll, UINT64_MAX - 100));
    assert(poll.deadline_ms == UINT64_MAX);
    assert(gpud_control_poll_timeout(&poll, UINT64_MAX - 10) == 10);
    puts("gpud control polling: PASS (busy cadence, idle deadline, late wakeup)");
    return 0;
}
