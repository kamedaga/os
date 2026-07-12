#pragma once

#include <stddef.h>
#include <stdint.h>

#include <inputd/boot_config.h>
#include <inputd/ipc_protocol.h>

struct inputd_input_island {
    void *device_backend;
    void *modules[INPUTD_MAX_MODULES];
    uint32_t loaded_module_count;
    uint32_t device_count;
    int ready;
};

int inputd_input_island_init(
    struct inputd_input_island *island,
    const struct inputd_boot_config *cfg);
void inputd_input_island_pump(struct inputd_input_island *island);
int inputd_input_open(uint32_t event_index, uint32_t flags, uint64_t *out_handle);
int inputd_input_close(uint64_t handle);
int inputd_input_dup(uint64_t handle, uint64_t *out_handle);
int inputd_input_read(inputd_read_request_t *request);
int inputd_input_ioctl(inputd_ioctl_request_t *request);
int inputd_input_poll(inputd_poll_request_t *request);
