/* SPDX-License-Identifier: MIT */
/* Real controller + bootstrap sender + IPC port; only native syscalls mocked. */
#include "../userland/gpud/bootstrap.h"
#include <kobox2/closure.h>
#include <pacha/syscall.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long send_status;
static unsigned int calls;

long pacha_syscall1(uint64_t nr, uint64_t fd) {
    (void)nr; (void)fd;
    abort(); /* Transfer borrows all source capabilities; no close permitted. */
}

long pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t address) {
    assert(nr == PACHA_IPC_SYSCALL_SEND && fd == 16);
    const struct pacha_ipc_msg *message = (void *)(uintptr_t)address;
    ++calls;
    assert(message->word1 == 1 && message->word2 < 5);
    if (message->word2 == 4) {
        assert(message->word0 == PH_BOOTSTRAP_FINISH && !message->fd_count);
    } else {
        assert(message->word0 == PH_BOOTSTRAP_BLOB && message->fd_count == 1);
        assert(message->fds[0].fd == 100 + message->word2);
        assert(!message->fds[0].transfer_flags);
    }
    return send_status;
}

static void *allocate(void *context, size_t size) {
    (void)context; return malloc(size);
}

static void deallocate(void *context, void *pointer, size_t size) {
    (void)context; (void)size; free(pointer);
}

static void complete(kb2_controller_t *controller, kb2_action_type_t type,
    uint64_t resource_id, uint64_t sandbox_id) {
    const kb2_action_t *action = kb2_controller_pending_action(controller);
    assert(action && kb2_action_type(action) == type);
    assert(kb2_controller_complete_action(controller, kb2_action_generation(action),
        kb2_action_token(action), KB2_STATUS_OK, resource_id, sandbox_id) == KB2_STATUS_OK);
}

static kb2_controller_t *controller_create(void) {
    kb2_controller_t *controller;
    kb2_closure_builder_t *builder;
    kb2_closure_t *closure;
    uint8_t digest[KB2_DIGEST_SIZE];
    memset(digest, 0x41, sizeof(digest));
    assert(kb2_controller_create(allocate, deallocate, NULL, &controller) == KB2_STATUS_OK);
    assert(kb2_closure_builder_create(allocate, deallocate, NULL, digest, sizeof(digest),
        &builder) == KB2_STATUS_OK);
    assert(kb2_closure_builder_add_artifact(builder, 1, KB2_ARTIFACT_SHARED_PROVIDER,
        digest, sizeof(digest), "core", 4) == KB2_STATUS_OK);
    assert(kb2_closure_builder_set_native_lifecycle(builder, 1) == KB2_STATUS_OK);
    assert(kb2_closure_builder_add_artifact(builder, 2, KB2_ARTIFACT_RELOCATABLE_MODULE,
        digest, sizeof(digest), "driver", 6) == KB2_STATUS_OK);
    assert(kb2_closure_builder_set_native_lifecycle(builder, 2) == KB2_STATUS_OK);
    assert(kb2_closure_builder_add_dependency(builder, 2, 1) == KB2_STATUS_OK);
    assert(kb2_closure_builder_mark_root(builder, 2) == KB2_STATUS_OK);
    assert(kb2_closure_builder_seal(builder, &closure) == KB2_STATUS_OK);
    assert(kb2_controller_set_closure(controller, closure) == KB2_STATUS_OK);
    kb2_closure_destroy(closure);
    kb2_closure_builder_destroy(builder);
    for (unsigned int kind = KB2_DIGEST_PROFILE; kind <= KB2_DIGEST_CHANNEL_SET; ++kind)
        assert(kb2_controller_set_digest(controller, kind, digest, sizeof(digest)) == KB2_STATUS_OK);
    const uint64_t limits[] = {1u << 20, 2, 4, 64};
    for (unsigned int kind = 0; kind <= KB2_LIMIT_OUTSTANDING_REQUEST_COUNT; ++kind)
        assert(kb2_controller_set_limit(controller, kind, limits[kind]) == KB2_STATUS_OK);
    assert(kb2_controller_start(controller) == KB2_STATUS_OK);
    complete(controller, KB2_ACTION_ALLOCATE_RESOURCES, 51, 0);
    complete(controller, KB2_ACTION_LAUNCH_SANDBOX, 0, 71);
    return controller;
}

static void cleanup(kb2_controller_t *controller, int faulted) {
    assert(kb2_controller_stop(controller) == KB2_STATUS_OK);
    complete(controller, faulted ? KB2_ACTION_TERMINATE_SANDBOX : KB2_ACTION_QUIESCE_SANDBOX, 0, 0);
    complete(controller, KB2_ACTION_REVOKE_RESOURCES, 0, 0);
    complete(controller, KB2_ACTION_REAP_SANDBOX, 0, 0);
    complete(controller, KB2_ACTION_RELEASE_RESOURCES, 0, 0);
    assert(kb2_controller_state(controller) == KB2_STATE_IDLE);
    kb2_controller_destroy(controller);
}

static void test_transfer(int fail) {
    calls = 0; send_status = 0;
    kb2_controller_t *controller = controller_create();
    struct ph_ipc ipc = {.generation = 1, .fd = 16, .admitted = 1};
    struct ph_bootstrap_item items[4];
    for (size_t i = 0; i < 4; ++i) items[i] = (struct ph_bootstrap_item){.size = 37 + i,
        .capability = {.fd = 100 + i, .rights = PH_BOOTSTRAP_BLOB_RIGHTS,
            .flags = PACHA_FD_FLAG_CLOEXEC}};
    struct gpud_bootstrap_resources resources = {.generation = 1, .resource_set_id = 51,
        .sandbox_id = 71, .items = items, .artifact_count = 2};
    memset(resources.manifest_digest, 0x41, sizeof(resources.manifest_digest));
    struct gpud_bootstrap_transfer transfer = {0};
    resources.generation = 2;
    assert(gpud_bootstrap_begin(&transfer, controller, &ipc, &resources) == KB2_STATUS_STALE_GENERATION);
    resources.generation = 1; resources.resource_set_id = 52;
    assert(gpud_bootstrap_begin(&transfer, controller, &ipc, &resources) == KB2_STATUS_RESOURCE_DENIED);
    resources.resource_set_id = 51; resources.manifest_digest[0] = 0;
    assert(gpud_bootstrap_begin(&transfer, controller, &ipc, &resources) == KB2_STATUS_RESOURCE_DENIED);
    resources.manifest_digest[0] = 0x41;
    assert(!transfer.controller && !calls);
    assert(gpud_bootstrap_begin(&transfer, controller, &ipc, &resources) == KB2_STATUS_OK);
    ipc.generation = 2;
    assert(gpud_bootstrap_step(&transfer) == KB2_STATUS_STALE_GENERATION && !calls && ipc.admitted);
    ipc.generation = 1;
    send_status = PACHA_SYSCALL_ERR_NOT_READY;
    assert(gpud_bootstrap_step(&transfer) == KB2_STATUS_ACTION_PENDING && transfer.sender.next == 0);
    assert(transfer.native_error == -EAGAIN);
    send_status = PACHA_SYSCALL_ERR_ALLOC;
    assert(gpud_bootstrap_step(&transfer) == KB2_STATUS_ACTION_PENDING && transfer.sender.next == 0);
    assert(transfer.native_error == -ENOMEM);
    send_status = 0;
    assert(gpud_bootstrap_step(&transfer) == KB2_STATUS_ACTION_PENDING && transfer.sender.next == 1);
    if (fail == 1) {
        send_status = PACHA_SYSCALL_ERR_INVALID;
        assert(gpud_bootstrap_step(&transfer) == KB2_STATUS_HOST_FAILURE);
        assert(transfer.completed && !ipc.admitted);
        assert(kb2_controller_state(controller) == KB2_STATE_FAULTED);
        assert(kb2_controller_fault_kind(controller) == KB2_FAULT_HOST_ACTION);
        cleanup(controller, 1);
    } else {
        if (fail == 2) {
            /* Another owner mutation invalidates a scheduled old callback. */
            complete(controller, KB2_ACTION_TRANSFER_RESOURCES, 0, 0);
            unsigned int before = calls;
            assert(gpud_bootstrap_step(&transfer) == KB2_STATUS_STALE_ACTION);
            assert(calls == before && ipc.admitted);
        } else {
            assert(gpud_bootstrap_step(&transfer) == KB2_STATUS_ACTION_PENDING);
            assert(gpud_bootstrap_step(&transfer) == KB2_STATUS_ACTION_PENDING);
            assert(gpud_bootstrap_step(&transfer) == KB2_STATUS_ACTION_PENDING);
            assert(kb2_controller_state(controller) == KB2_STATE_STARTING);
            assert(gpud_bootstrap_step(&transfer) == KB2_STATUS_OK);
            assert(transfer.completed && transfer.sender.finished && calls == 7);
            assert(gpud_bootstrap_step(&transfer) == KB2_STATUS_INVALID_STATE && calls == 7);
        }
        assert(kb2_controller_state(controller) == KB2_STATE_HANDSHAKING);
        assert(kb2_controller_report_ready(controller, 1) == KB2_STATUS_OK);
        cleanup(controller, 0);
    }
    for (size_t i = 0; i < 4; ++i) assert(items[i].capability.fd == 100 + i);
}

int main(void) {
    test_transfer(0);
    test_transfer(1);
    test_transfer(2);
    puts("gpud controller/bootstrap binding unit: PASS");
    return 0;
}
