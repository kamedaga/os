/* SPDX-License-Identifier: MIT */
#include "boot.h"

#include <errno.h>
#include <string.h>

kb2_status_t ph_owner_boot_begin(struct ph_owner_boot *boot,
                                const struct ph_owner_boot_config *config) {
    if (!boot || boot->phase != PH_OWNER_BOOT_EMPTY || !config ||
        !config->controller || !config->launch || !config->ipc || !config->management ||
        !config->watch || !config->lifecycle || !config->sandbox_config ||
        !config->image || !config->image_size || !config->items ||
        !config->resource_set_id || !config->sandbox_id ||
        config->extra_blob_count >= GPUD_LAUNCH_MAX_BLOBS ||
        (config->extra_blob_count && !config->extra_blobs))
        return KB2_STATUS_INVALID_ARGUMENT;
    if (config->management->a >= 16 || config->management->b >= 16 ||
        config->ipc->admitted || config->launch->process.fd ||
        config->launch->thread_fd || config->launch->scratch_fd ||
        config->launch->scratch_view)
        return KB2_STATUS_INVALID_STATE;
    const uint64_t generation = kb2_controller_generation(config->controller);
    if (!generation || config->sandbox_config->identity.generation != generation ||
        config->sandbox_config->artifact_count != config->artifact_count ||
        config->sandbox_config->resource_count != config->resource_count ||
        config->sandbox_config->size != sizeof(*config->sandbox_config))
        return KB2_STATUS_INVALID_CONFIGURATION;
    /* The config selects the child's logger; transferring the endpoint
     * alone silently loses its Linux diagnostics. Conversely a selected
     * logger must never name a descriptor absent from the launch grants. */
    if (config->sandbox_config->log_endpoint_fd !=
        (config->log_endpoint_fd >= 16 ? PH_SANDBOX_LOG_FD : 0))
        return KB2_STATUS_INVALID_CONFIGURATION;
    *boot = (struct ph_owner_boot){.config = *config, .phase = PH_OWNER_BOOT_CHANNEL};
    int error = pacha_ipc_channel_create(config->management,
        PH_IPC_CHANNEL_RIGHTS | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER, 0);
    if (!error)
        error = ph_ipc_init(config->ipc, config->management->a, generation);
    if (error) {
        boot->native_error = error;
        /* No process was started, so these endpoints cannot own live grants. */
        if (config->management->a >= 16) {
            int cleanup = pacha_fd_close(config->management->a);
            if (cleanup)
                boot->native_error = cleanup;
            else
                config->management->a = -1;
        }
        if (config->management->b >= 16) {
            int cleanup = pacha_fd_close(config->management->b);
            if (cleanup)
                boot->native_error = cleanup;
            else
                config->management->b = -1;
        }
        return KB2_STATUS_HOST_FAILURE;
    }
    struct gpud_launch_blob blobs[GPUD_LAUNCH_MAX_BLOBS] = {
        {.bytes = config->sandbox_config, .size = sizeof(*config->sandbox_config),
         .address = PH_SANDBOX_CONFIG_ADDRESS},
    };
    if (config->extra_blob_count)
        memcpy(blobs + 1, config->extra_blobs,
            config->extra_blob_count * sizeof(*blobs));
    struct pacha_process_fd_grant grants[2] = {
        {.source_fd = (uint64_t)config->management->b,
         .target_fd = PH_SANDBOX_CHANNEL_FD, .rights = PH_IPC_CHANNEL_RIGHTS,
         .flags = PACHA_FD_FLAG_CLOEXEC},
    };
    if (config->log_endpoint_fd >= 16)
        grants[1] = (struct pacha_process_fd_grant){
            .source_fd = (uint64_t)config->log_endpoint_fd, .target_fd = PH_SANDBOX_LOG_FD,
            .rights = PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_CLOSE,
            .flags = PACHA_FD_FLAG_CLOEXEC};
    struct gpud_launch_request request = {.generation = generation,
        .load_bias = UINT64_C(0x40000000), .stack_address = UINT64_C(0x45000000),
        .stack_size = 256 * 1024, .image = config->image, .image_size = config->image_size,
        .blobs = blobs, .blob_count = 1 + config->extra_blob_count,
        .grants = grants, .grant_count = config->log_endpoint_fd >= 16 ? 2 : 1};
    struct gpud_launch_resources resources = {.generation = generation,
        .resource_set_id = config->resource_set_id, .sandbox_id = config->sandbox_id,
        .request = &request};
    memcpy(resources.manifest_digest, config->sandbox_config->identity.manifest_digest, 32);
    boot->phase = PH_OWNER_BOOT_LAUNCH;
    kb2_status_t status = gpud_launch_begin(&boot->launch, config->controller,
        config->launch, config->ipc, &resources);
    boot->native_error = boot->launch.launch_error;
    /* Stack-local mappings were copied by native prepare; do not retain them. */
    boot->config.sandbox_config = NULL;
    boot->config.image = NULL;
    boot->config.extra_blobs = NULL;
    return status;
}

kb2_status_t ph_owner_boot_step(struct ph_owner_boot *boot) {
    if (!boot || !boot->config.controller)
        return KB2_STATUS_INVALID_ARGUMENT;
    const struct ph_owner_boot_config *config = &boot->config;
    if (boot->phase == PH_OWNER_BOOT_LAUNCH) {
        kb2_status_t status = gpud_launch_step(&boot->launch);
        boot->native_error = boot->launch.cleanup_error ?
            boot->launch.cleanup_error : boot->launch.launch_error;
        if (status != KB2_STATUS_OK || !boot->launch.completed)
            return status;
        if (boot->launch.launch_error)
            return KB2_STATUS_HOST_FAILURE;
        int error = pacha_fd_close(config->management->b);
        if (error) {
            boot->native_error = error;
            return KB2_STATUS_HOST_FAILURE;
        }
        config->management->b = -1;
        boot->phase = PH_OWNER_BOOT_WATCH;
        status = gpud_process_watch_init(config->watch, config->controller,
            &config->launch->process, config->ipc, config->resource_set_id, config->sandbox_id);
        if (status != KB2_STATUS_OK)
            return status;
        struct gpud_bootstrap_resources resources = {
            .generation = boot->launch.generation, .resource_set_id = config->resource_set_id,
            .sandbox_id = config->sandbox_id, .items = config->items,
            .artifact_count = config->artifact_count, .resource_handle_count = config->resource_count};
        /* Launch begin checked the controller digest before any native start. */
        const kb2_action_t *action = kb2_controller_pending_action(config->controller);
        if (!action)
            return KB2_STATUS_INVALID_STATE;
        status = kb2_action_copy_digest(action, KB2_DIGEST_MANIFEST,
            resources.manifest_digest, sizeof(resources.manifest_digest));
        if (status != KB2_STATUS_OK)
            return status;
        boot->phase = PH_OWNER_BOOT_TRANSFER;
        status = gpud_bootstrap_begin(&boot->transfer, config->controller, config->ipc, &resources);
        if (status != KB2_STATUS_OK)
            return status;
        return KB2_STATUS_ACTION_PENDING;
    }
    if (boot->phase == PH_OWNER_BOOT_TRANSFER) {
        kb2_status_t status = gpud_bootstrap_step(&boot->transfer);
        boot->native_error = boot->transfer.native_error;
        if (status != KB2_STATUS_OK || !boot->transfer.completed)
            return status;
        boot->phase = PH_OWNER_BOOT_LIFECYCLE;
        status = gpud_lifecycle_init(config->lifecycle, config->watch);
        if (status != KB2_STATUS_OK)
            return status;
        boot->phase = PH_OWNER_BOOT_COMPLETE;
        return KB2_STATUS_OK;
    }
    return KB2_STATUS_INVALID_STATE;
}
