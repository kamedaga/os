#include <pacha/abi.h>
#pragma once

#include <stdint.h>

struct inputd_public_device;
struct kb_input_device_snapshot;

int inputd_namespace_publish(pacha_fd_t filed_fd,
    const struct inputd_public_device *device,
    const struct kb_input_device_snapshot *snapshot);
int inputd_namespace_remove(pacha_fd_t filed_fd, uint32_t event_index);
