/* SPDX-License-Identifier: MIT */
#ifndef PACHA_NETD_KOBOX2_NIC_H
#define PACHA_NETD_KOBOX2_NIC_H

#include <stdint.h>
#include <stddef.h>

struct netd_runtime;

struct netd_nic_diagnostic {
    unsigned step, loaded, pci_bound;
    int detail;
    uint64_t source;
    unsigned line;
    unsigned fault, fault_vector, fault_error_code, fault_core_relative;
    uint64_t fault_ip, fault_address;
};

int netd_kobox2_nic_start(struct netd_runtime *runtime);
uint64_t netd_kobox2_wifi_generation(void);
int netd_kobox2_wifi_command(const void *command, size_t length,
    void *reply, size_t capacity, size_t *received);
int netd_kobox2_wifi_configuration(const void *configuration, size_t length);
int netd_kobox2_wifi_take_events(uint32_t *events);
const struct netd_nic_diagnostic *netd_kobox2_nic_diagnostic(void);

#endif
