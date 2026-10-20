#include <pacha/abi.h>
#pragma once

#include <stdint.h>

int64_t lpr_input_open_path(const char *path, uint64_t flags);
int64_t lpr_input_close_handle(uint64_t handle);
int64_t lpr_input_dup_handle(uint64_t handle);
int64_t lpr_input_transfer_dup_handle(
    uint64_t handle, pacha_fd_t lease_fd, uint64_t *out_handle);
int64_t lpr_input_read_events(uint64_t fd, uint64_t buf, uint64_t count, unsigned *wait_not_ready __attribute__((unused)));
int64_t lpr_input_ioctl(uint64_t fd, uint64_t request, uint64_t arg);
int64_t lpr_input_poll_events(uint64_t fd, uint32_t events);
pacha_fd_t lpr_input_native_wait_fd(uint64_t fd);
