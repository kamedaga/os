#pragma once

#include "netd/boot_config.h"
#include "dhcp.h"

#include <stddef.h>
#include <stdint.h>

struct netd_runtime {
    const struct netd_boot_config *cfg;
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

int netd_packet_io_start(struct netd_runtime *runtime);
int netd_packet_io_smoke_after_ipv4(void);
void netd_packet_io_pump_once(void);
int netd_packet_io_dhcp_start(struct netd_runtime *runtime);
void netd_packet_io_dhcp_disable(void);
const struct netd_dhcp_client *netd_packet_io_dhcp_status(void);
