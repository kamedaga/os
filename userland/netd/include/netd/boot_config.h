#ifndef NETD_BOOT_CONFIG_H
#define NETD_BOOT_CONFIG_H

#include <stdint.h>

enum {
    NETD_BOOT_CONFIG_MAGIC = 0x4e455444424f4f54ull,
    NETD_BOOT_CONFIG_VERSION = 8,
    NETD_BOOT_STATUS_MAGIC = 0x3153544154454e4eull,
    NETD_BOOT_FAULT_MAGIC = 0x31544c5541464e4eull,
    NETD_BOOT_STAGE_NIC = 1,
    NETD_BOOT_STAGE_SOCKET = 2,
    NETD_BOOT_STAGE_TIMER = 3,
    NETD_BOOT_STAGE_READY = 4,
    NETD_BOOT_STAGE_IPV4 = 5,
    NETD_BOOT_STAGE_LINK = 6,
    NETD_BOOT_STAGE_FAULT = 7,
    NETD_NIC_STEP_PACKAGE = 1,
    NETD_NIC_STEP_CONTROLLER = 2,
    NETD_NIC_STEP_RESOURCE = 3,
    NETD_NIC_STEP_CHANNEL = 4,
    NETD_NIC_STEP_SANDBOX_LAUNCH = 5,
    NETD_NIC_STEP_PROCESS_WATCH = 6,
    NETD_NIC_STEP_TRANSFER = 7,
    NETD_NIC_STEP_LIFECYCLE = 8,
    NETD_NIC_STEP_FRAME_ATTACH = 9,
    NETD_NIC_STEP_FRAME_INFO = 10,
    NETD_NIC_STEP_LINK_ATTACH = 11,
    NETD_BOOT_FLAG_SMOKE = 1ull << 0,
    NETD_BOOT_FLAG_TRACE = 1ull << 1,
    NETD_BOOT_FLAG_METRIC = 1ull << 2,
};

static inline uint32_t netd_boot_fault_metadata(unsigned vector,
    unsigned error_code, unsigned core_relative) {
    return (error_code & 0xffffu) | ((vector & 0x7fffu) << 16) |
        ((core_relative & 1u) << 31);
}

/* IPv4 is optional. netd starts the NIC without it and may load a policy
 * from /run/pacha/network.conf later. The link MAC comes from the NIC. */
struct netd_ipv4_boot_config {
    uint8_t address[4];
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t dns[4];
    uint8_t reserved[8];
};

struct netd_boot_config {
    uint64_t magic;
    uint64_t version;
    uint64_t device_fd;
    uint64_t filed_endpoint_fd;
    uint64_t socket_endpoint_fd;
    uint64_t flags;
    struct netd_ipv4_boot_config ipv4;
    /* Optional seed0root status channel. Zero for non-live service launches.
     * This private bootstrap ABI is rebuilt with its consumer; v8 also
     * carries unhandled fault location without accepting older layouts. */
    uint64_t status_channel_fd;
    uint64_t reserved[5];
};

_Static_assert(sizeof(struct netd_boot_config) == 120,
    "netd private bootstrap ABI");

#endif
