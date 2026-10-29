/* SPDX-License-Identifier: MIT */
#include "../userland/kobox2_owner/boot.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Native launch, transfer and lifecycle internals have their own tests. These
 * doubles isolate orchestration, so failure/backpressure cannot hide a second
 * launch, premature readiness, or ownership discarded by the shared owner. */
static int fail_channel, fail_adopt, fail_close, fail_launch, fail_watch, fail_transfer;
static unsigned closed, launches, transfers, lifecycle_inits, extra, log_grant;
static struct ph_bootstrap_item items[3];
static struct ph_sandbox_config sandbox;
static unsigned char image;
static uint64_t controller_storage, action_storage;

uint64_t kb2_controller_generation(const kb2_controller_t *controller) {
    assert(controller == (const kb2_controller_t *)&controller_storage);
    return 7;
}

const kb2_action_t *kb2_controller_pending_action(const kb2_controller_t *controller) {
    assert(controller == (const kb2_controller_t *)&controller_storage);
    return (const kb2_action_t *)&action_storage;
}

kb2_status_t kb2_action_copy_digest(const kb2_action_t *action, kb2_digest_kind_t kind,
    uint8_t *output, size_t capacity) {
    assert(action == (const kb2_action_t *)&action_storage && kind == KB2_DIGEST_MANIFEST);
    assert(capacity == 32);
    memset(output, 0, capacity);
    return KB2_STATUS_OK;
}

int pacha_ipc_channel_create(struct pacha_ipc_channel_pair *pair, uint64_t rights, uint32_t flags) {
    assert(rights & PACHA_FD_RIGHT_TRANSFER);
    assert(!flags);
    if (fail_channel)
        return -ENOMEM;
    *pair = (struct pacha_ipc_channel_pair){.a = 20, .b = 21};
    return 0;
}

int pacha_fd_close(pacha_fd_t fd) {
    assert(fd == 20 || fd == 21);
    if (fail_close)
        return -EIO;
    closed++;
    return 0;
}

int ph_ipc_init(struct ph_ipc *ipc, pacha_fd_t fd, uint64_t generation) {
    if (fail_adopt)
        return -EIO;
    *ipc = (struct ph_ipc){.fd = fd, .generation = generation, .admitted = 1};
    return 0;
}

kb2_status_t gpud_launch_begin(struct gpud_launch_transaction *transaction,
    kb2_controller_t *controller, struct gpud_native_launch *launch,
    struct ph_ipc *ipc, const struct gpud_launch_resources *resources) {
    launches++;
    const struct gpud_launch_request *request = resources->request;
    assert(request->generation == 7 && request->image == &image);
    assert(request->blob_count == 1 + extra);
    assert(request->grant_count == 1 + log_grant);
    assert(request->grants[0].source_fd == 21);
    assert(request->grants[0].target_fd == PH_SANDBOX_CHANNEL_FD);
    assert(request->blobs[0].bytes == &sandbox);
    if (log_grant)
        assert(request->grants[1].source_fd == 90 &&
            request->grants[1].rights == (PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_CLOSE));
    *transaction = (struct gpud_launch_transaction){.controller = controller,
        .launch = launch, .ipc = ipc, .generation = 7};
    launch->process.fd = 22;
    launch->process.generation = 7;
    return KB2_STATUS_OK;
}

kb2_status_t gpud_launch_step(struct gpud_launch_transaction *transaction) {
    if (fail_launch) {
        transaction->launch_error = -EIO;
        return KB2_STATUS_HOST_FAILURE;
    }
    transaction->completed = 1;
    return KB2_STATUS_OK;
}

kb2_status_t gpud_process_watch_init(struct gpud_process_watch *watch,
    kb2_controller_t *controller, struct gpud_native_process *process,
    struct ph_ipc *ipc, uint64_t resource, uint64_t sandbox_id) {
    (void)watch; (void)controller; (void)process; (void)ipc;
    assert(resource == 1 && sandbox_id == 1);
    return fail_watch ? KB2_STATUS_HOST_FAILURE : KB2_STATUS_OK;
}

kb2_status_t gpud_bootstrap_begin(struct gpud_bootstrap_transfer *transfer,
    kb2_controller_t *controller, struct ph_ipc *ipc,
    const struct gpud_bootstrap_resources *resources) {
    (void)controller; (void)ipc;
    assert(resources->items == items && resources->artifact_count == 1);
    assert(resources->resource_handle_count == 0 && resources->generation == 7);
    memset(transfer, 0, sizeof(*transfer));
    return KB2_STATUS_OK;
}

kb2_status_t gpud_bootstrap_step(struct gpud_bootstrap_transfer *transfer) {
    transfers++;
    if (fail_transfer) {
        transfer->native_error = -EIO;
        return KB2_STATUS_HOST_FAILURE;
    }
    if (transfers == 1) {
        transfer->native_error = -EAGAIN;
        return KB2_STATUS_ACTION_PENDING;
    }
    transfer->completed = 1;
    return KB2_STATUS_OK;
}

kb2_status_t gpud_lifecycle_init(struct gpud_lifecycle *lifecycle,
    struct gpud_process_watch *watch) {
    (void)watch;
    lifecycle_inits++;
    assert(!lifecycle->ready);
    return KB2_STATUS_OK;
}

static void run(unsigned failure) {
    fail_channel = failure == 1; fail_adopt = failure == 2 || failure == 7;
    fail_launch = failure == 3; fail_close = failure == 4 || failure == 7;
    fail_watch = failure == 5; fail_transfer = failure == 6;
    closed = launches = transfers = lifecycle_inits = 0;
    struct gpud_native_launch launch = {0};
    struct ph_ipc ipc = {0};
    struct pacha_ipc_channel_pair pair = {.a = -1, .b = -1};
    struct gpud_process_watch watch = {0};
    struct gpud_lifecycle lifecycle = {0};
    const struct gpud_launch_blob blob = {.bytes = &image, .size = 1, .address = 0x48000000};
    sandbox = (struct ph_sandbox_config){.size = sizeof(struct ph_sandbox_config),
        .identity = {.generation = 7}, .artifact_count = 1,
        .log_endpoint_fd = log_grant ? PH_SANDBOX_LOG_FD : 0};
    struct ph_owner_boot_config config = {.controller = (kb2_controller_t *)&controller_storage,
        .launch = &launch, .ipc = &ipc, .management = &pair, .watch = &watch,
        .lifecycle = &lifecycle, .resource_set_id = 1, .sandbox_id = 1,
        .sandbox_config = &sandbox, .image = &image, .image_size = 1,
        .items = items, .artifact_count = 1, .extra_blobs = &blob,
        .extra_blob_count = extra, .log_endpoint_fd = log_grant ? 90 : -1};
    struct ph_owner_boot boot = {0};
    const uint64_t invalid_sizes[] = {0, sizeof(sandbox) - 1, sizeof(sandbox) + 1};
    for (size_t i = 0; i < sizeof(invalid_sizes) / sizeof(invalid_sizes[0]); ++i) {
        sandbox.size = invalid_sizes[i];
        assert(ph_owner_boot_begin(&boot, &config) == KB2_STATUS_INVALID_CONFIGURATION);
        assert(boot.phase == PH_OWNER_BOOT_EMPTY && !launches && !transfers && !closed);
        assert(pair.a == -1 && pair.b == -1);
    }
    sandbox.size = sizeof(sandbox);
    const uint64_t selected_logger = sandbox.log_endpoint_fd;
    sandbox.log_endpoint_fd = log_grant ? 0 : PH_SANDBOX_LOG_FD;
    assert(ph_owner_boot_begin(&boot, &config) == KB2_STATUS_INVALID_CONFIGURATION);
    assert(boot.phase == PH_OWNER_BOOT_EMPTY && !launches && !transfers && !closed);
    assert(pair.a == -1 && pair.b == -1);
    sandbox.log_endpoint_fd = selected_logger;
    kb2_status_t status = ph_owner_boot_begin(&boot, &config);
    if (failure == 1 || failure == 2 || failure == 7) {
        assert(status == KB2_STATUS_HOST_FAILURE && !launches);
        assert(closed == (failure == 2 ? 2u : 0u));
        assert(pair.a == (failure == 7 ? 20 : -1));
        assert(pair.b == (failure == 7 ? 21 : -1));
        return;
    }
    assert(status == KB2_STATUS_OK && launches == 1);
    assert(ph_owner_boot_begin(&boot, &config) == KB2_STATUS_INVALID_ARGUMENT);
    for (unsigned steps = 0; steps < 8; ++steps) {
        status = ph_owner_boot_step(&boot);
        if (status != KB2_STATUS_ACTION_PENDING)
            break;
        assert(!lifecycle.ready && !lifecycle_inits);
    }
    if (failure) {
        assert(status == KB2_STATUS_HOST_FAILURE && !lifecycle_inits);
        assert(launch.process.fd == 22);
        assert(boot.phase != PH_OWNER_BOOT_COMPLETE);
    } else {
        assert(status == KB2_STATUS_OK && boot.phase == PH_OWNER_BOOT_COMPLETE);
        assert(launches == 1 && transfers == 2 && lifecycle_inits == 1);
        assert(!lifecycle.ready && pair.b == -1 && closed == 1);
        assert(ph_owner_boot_step(&boot) == KB2_STATUS_INVALID_STATE);
    }
}

int main(void) {
    for (unsigned failure = 0; failure <= 7; ++failure)
        run(failure);
    extra = log_grant = 1;
    run(0);
    puts("shared sandbox owner boot: OK (9 scenarios; invalid extents and missing/unselected log grants rejected)");
    return 0;
}
