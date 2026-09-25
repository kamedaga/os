/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_SANDBOX_CONFIG_H
#define PACHA_KOBOX_SANDBOX_CONFIG_H

#include "bootstrap.h"
#include "device_authority.h"

/* Private launch policy copied read-only before starting the native thread;
 * never accept expected package/device identity from the data channel. */
#define PH_SANDBOX_CONFIG_ADDRESS UINT64_C(0x47000000)
#define PH_SANDBOX_CHANNEL_FD 16
struct ph_sandbox_config {
    struct ph_package_identity identity;
    uint64_t artifact_count, resource_count;
    uint64_t client_id, gpu_channel_id;
    struct ph_device_authority device;
};

#endif
