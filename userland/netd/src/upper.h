#pragma once

#include "netd_internal.h"

#include <stddef.h>
#include <stdint.h>

struct netd_upper_frame {
    void *dev;
    const unsigned char *bytes;
    size_t len;
};

int netd_upper_start(struct netd_runtime *runtime);
int netd_upper_receive_frame(const struct netd_upper_frame *frame);
void netd_upper_poll(void);
uint64_t netd_upper_delivered_frames(void);
