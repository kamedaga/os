#pragma once

#include <stdint.h>

struct inputd_public_device;
struct kb_input_device_snapshot;

int inputd_namespace_publish(int filed_fd,
    const struct inputd_public_device *device,
    const struct kb_input_device_snapshot *snapshot);
int inputd_namespace_remove(int filed_fd, uint32_t event_index);
