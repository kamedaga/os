/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_SANDBOX_CONFIG_H
#define PACHA_KOBOX_SANDBOX_CONFIG_H

#include "bootstrap.h"
#include "device_authority.h"

/* Private launch policy copied read-only before starting the native thread;
 * never accept expected package/device identity from the data channel. */
#define PH_SANDBOX_CONFIG_ADDRESS UINT64_C(0x47000000)
#define PH_SANDBOX_FIRMWARE_ADDRESS UINT64_C(0x48000000)
#define PH_SANDBOX_STORAGE_DEVICES_ADDRESS UINT64_C(0x49000000)
#define PH_SANDBOX_CHANNEL_FD 16
#define PH_SANDBOX_LOG_FD 17
struct ph_sandbox_config {
    /* Development builds must agree on the exact launch record extent.
     * This is a bounds check, not an ABI generation or compatibility promise. */
    uint64_t size;
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
    /* Storage authority is immutable launch policy. UUID selects only among
     * devices descending from the transferred PCI capabilities. Queue/RAM
     * budgets are resources, never pathname/file-size limits. */
    uint8_t root_uuid[16];
    uint64_t fs_channel_id, fs_owner_channel_id, fs_arena_bytes, ram_bytes;
    uint32_t fs_queue_size, storage_reserved;
    /* ph_device_authority[] lives in a separately authenticated read-only
     * launch blob; storage does not infer additional authority from PCI IDs. */
    uint64_t storage_devices_address, storage_device_count;
};

#endif
