#define _GNU_SOURCE

#include "sched_loop.h"

#include <stdint.h>

enum {
    PACHA_SCHEDULERD_SCHEDCTL_FD = 16,
    PACHA_SCHEDULERD_EVENT_FD = 17,
};

static pacha_sched_loop_t g_sched_loop;

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    pacha_sched_loop_t *loop = &g_sched_loop;
    pacha_sched_loop_init(loop, PACHA_SCHEDULERD_SCHEDCTL_FD, PACHA_SCHEDULERD_EVENT_FD);

    for (;;) {
        const int status = pacha_sched_loop_run_once(loop);
        if (status == 0) continue;
        if (status == -5) {
            __asm__ volatile("pause");
            continue;
        }
        __asm__ volatile("pause");
    }
}
