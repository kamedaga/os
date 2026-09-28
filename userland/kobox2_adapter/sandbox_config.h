/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_SANDBOX_CONFIG_H
#define PACHA_KOBOX_SANDBOX_CONFIG_H

#include "bootstrap.h"
#include "device_authority.h"

/* Private launch policy copied read-only before starting the native thread;
 * never accept expected package/device identity from the data channel. */
#define PH_SANDBOX_CONFIG_ADDRESS UINT64_C(0x47000000)
#define PH_SANDBOX_FIRMWARE_ADDRESS UINT64_C(0x48000000)
#define PH_SANDBOX_CONFIG_VERSION UINT64_C(3)
#define PH_SANDBOX_CHANNEL_FD 16
#define PH_SANDBOX_LOG_FD 17
struct ph_sandbox_config {
    uint64_t version;
    struct ph_package_identity identity;
    uint64_t artifact_count, resource_count;
    uint64_t client_id, gpu_channel_id;
    struct ph_device_authority device;
    /* Optional read-only launch mapping outside the closure transport. Its
     * whole-image digest is authenticated by the owner; the sandbox verifies
     * it and each file before Linux can request firmware. */
    uint64_t firmware_address, firmware_size;
    uint8_t firmware_sha256[32];
    uint64_t log_endpoint_fd;
};

#endif
