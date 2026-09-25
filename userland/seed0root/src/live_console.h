#pragma once

#include "pacha/live_bootstrap.h"
#include "pacha/root_handoff.h"

#include <stddef.h>
#include <stdint.h>

struct live_console;

int live_console_prepare(const pacha_live_root_bootstrap_t *boot,
    const struct pacha_root_device_record *devices, const int *device_fds,
    uint64_t device_count, int termd_fd, int inputd_fd,
    struct live_console **out);
const char *live_console_ctty(const struct live_console *console);
void live_console_report_usb_boot(struct live_console *console,
    const int *ready_fds, size_t ready_count);
void live_console_report_net_boot(struct live_console *console,
    int state, int status, uint64_t stage, unsigned carrier, unsigned mtu,
    unsigned nic_step, int detail, unsigned loaded, unsigned pci_bound,
    uint64_t source, unsigned line,
    unsigned fault_vector, unsigned fault_error_code,
    unsigned fault_core_relative, uint64_t fault_ip, uint64_t fault_address);
void live_console_finish_boot(struct live_console *console);
int live_console_run(struct live_console *console,
    void (*on_tick)(void *context), void *tick_context);
void live_console_destroy(struct live_console *console);
