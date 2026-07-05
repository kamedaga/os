#include "upper.h"

#include "libuinet_backend.h"

#include <stdio.h>

static uint64_t g_upper_delivered;
static int g_upper_trace;

int netd_upper_start(struct netd_runtime *runtime)
{
    if (runtime == NULL || runtime->cfg == NULL) {
        return 7;
    }
    g_upper_delivered = 0;
    g_upper_trace = (runtime->cfg->flags & NETD_BOOT_FLAG_TRACE) != 0;

    int status = netd_libuinet_start(runtime);
    if (status != 0) {
        return status;
    }
    if (g_upper_trace) {
        printf("[netd] upper libuinet state=%d\n", (int)netd_libuinet_state());
    }
    return 0;
}

int netd_upper_receive_frame(const struct netd_upper_frame *frame)
{
    if (frame == NULL || frame->bytes == NULL || frame->len == 0) {
        return -22;
    }
    int status = netd_libuinet_receive_frame(frame);
    if (status == 0) {
        g_upper_delivered++;
    }
    return status;
}

void netd_upper_poll(void)
{
    netd_libuinet_poll();
}

uint64_t netd_upper_delivered_frames(void)
{
    return g_upper_delivered;
}
