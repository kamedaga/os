#pragma once

#include "kobox/device.h"
#include "kobox/module.h"
#include "netd/boot_config.h"

#include <stddef.h>
#include <stdint.h>

struct netd_runtime {
    const struct netd_boot_config *cfg;
    kb_device_backend_t *backend;
    kb_module_t *modules[NETD_MAX_MODULES];
};

uint64_t netd_metrics_read_tsc(void);
void netd_metrics_set_enabled(int enabled);
void netd_metrics_record(const char *stage, uint64_t start_cycles, uint64_t end_cycles);
void netd_metrics_record_ex(
    const char *stage,
    const char *name,
    uint64_t start_cycles,
    uint64_t end_cycles,
    uint64_t size);
void netd_metrics_print(void);

const char *netd_status_name(kb_status_t status);

int netd_device_init(struct netd_runtime *runtime);
int netd_module_stack_load(struct netd_runtime *runtime);
int netd_driver_bind(struct netd_runtime *runtime);
int netd_packet_io_start(struct netd_runtime *runtime);
void netd_packet_io_pump_once(void);
