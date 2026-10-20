#include <pacha/abi.h>
/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX2_OWNER_BOOT_H
#define PACHA_KOBOX2_OWNER_BOOT_H

#include "../gpud/bootstrap.h"
#include "../gpud/launch.h"
#include "../gpud/lifecycle.h"
#include "../kobox2_adapter/sandbox_config.h"

/* Every owner describes the same unnumbered native lifecycle contract. A
 * numbered role-local label would imply an ABI freeze that does not exist. */
#define PH_OWNER_CHANNEL_CONTRACT "kobox2-lifecycle-ipc"

enum ph_owner_boot_phase {
    PH_OWNER_BOOT_EMPTY,
    PH_OWNER_BOOT_CHANNEL,
    PH_OWNER_BOOT_LAUNCH,
    PH_OWNER_BOOT_WATCH,
    PH_OWNER_BOOT_TRANSFER,
    PH_OWNER_BOOT_LIFECYCLE,
    PH_OWNER_BOOT_COMPLETE,
};

struct ph_owner_boot_config {
    kb2_controller_t *controller;
    struct gpud_native_launch *launch;
    struct ph_ipc *ipc;
    struct pacha_ipc_channel_pair *management;
    struct gpud_process_watch *watch;
    struct gpud_lifecycle *lifecycle;
    uint64_t resource_set_id, sandbox_id;
    const struct ph_sandbox_config *sandbox_config;
    const void *image;
    size_t image_size;
    const struct ph_bootstrap_item *items;
    size_t artifact_count, resource_count;
    const struct gpud_launch_blob *extra_blobs;
    size_t extra_blob_count;
    pacha_fd_t log_endpoint_fd;
};

struct ph_owner_boot {
    struct ph_owner_boot_config config;
    struct gpud_launch_transaction launch;
    struct gpud_bootstrap_transfer transfer;
    enum ph_owner_boot_phase phase;
    int native_error;
};

/* Controller-only: never link this into a GPL sandbox. Bind the role's
 * resources and complete ALLOCATE before begin. One owner drives step;
 * controller, native inventory and immutable package items remain borrowed.
 * Config/image/extra blobs are read only during begin and may then expire.
 * Failure retains published native inventory for the controller's existing
 * revoke/reset/reap path; never overwrite that inventory to retry a start.
 * COMPLETE initializes the lifecycle but does not claim Linux READY. Role
 * code still handles probe diagnostics, policy and service attachment. */
kb2_status_t ph_owner_boot_begin(struct ph_owner_boot *boot,
                                const struct ph_owner_boot_config *config);
kb2_status_t ph_owner_boot_step(struct ph_owner_boot *boot);

#endif
