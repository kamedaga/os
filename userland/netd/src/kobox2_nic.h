/* SPDX-License-Identifier: MIT */
#ifndef PACHA_NETD_KOBOX2_NIC_H
#define PACHA_NETD_KOBOX2_NIC_H

#include <stdint.h>

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
const struct netd_nic_diagnostic *netd_kobox2_nic_diagnostic(void);

#endif
