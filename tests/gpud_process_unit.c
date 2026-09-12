/* SPDX-License-Identifier: MIT */
/* Real controller/IPC/process owner. Syscalls alone use fault-injection state. */
#include "../userland/gpud/process.h"
#include <kobox2/closure.h>
#include <pacha/syscall.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct pacha_fd_info info;
static struct gpud_process_exit observed;
static long info_error, wait_error, kill_error, close_error;
static unsigned int calls, kills, closes, waits, exit_on_kill;

long pacha_syscall1(uint64_t nr, uint64_t fd) {
    assert(nr == PACHA_FD_SYSCALL_CLOSE && fd == 17);
    ++calls; ++closes;
    return close_error;
}

long pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t argument) {
    assert(fd == 17);
    ++calls;
    switch (nr) {
    case PACHA_FD_SYSCALL_GET_INFO:
        *(struct pacha_fd_info *)(uintptr_t)argument = info;
        return info_error;
    case PACHA_PROCESS_SYSCALL_WAIT:
        ++waits;
        /* Even failed copyout can leave bytes in the caller's temporary. */
        *(struct gpud_process_exit *)(uintptr_t)argument = observed;
        return wait_error;
    case PACHA_PROCESS_SYSCALL_KILL:
        assert(argument == 9);
        ++kills;
        if (exit_on_kill) wait_error = 0;
        return kill_error;
    default: abort();
    }
}

static void reset_native(void) {
    info = (struct pacha_fd_info){.kind = PACHA_FD_KIND_PROCESS,
        .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_KILL |
            PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE};
    observed = (struct gpud_process_exit){.state = GPUD_PROCESS_KILLED, .code = 9,
        .principal = 100};
    info_error = kill_error = close_error = 0;
    wait_error = PACHA_SYSCALL_ERR_NOT_READY;
    calls = kills = closes = waits = exit_on_kill = 0;
}

static void test_native_owner(void) {
    reset_native();
    struct gpud_native_process process = {0};
    assert(gpud_native_process_adopt(&process, 17, 0) == -EINVAL && !calls);
    info_error = PACHA_SYSCALL_ERR_INVALID;
    assert(gpud_native_process_adopt(&process, 17, 1) == -EINVAL && !process.fd);
    info_error = 0; info.kind = PACHA_FD_KIND_CHANNEL;
    assert(gpud_native_process_adopt(&process, 17, 1) == -EACCES && !process.fd);
    info.kind = PACHA_FD_KIND_PROCESS;
    const uint64_t required = info.rights;
    for (unsigned int bit = 0; bit < 64; ++bit) {
        if (!(required & (UINT64_C(1) << bit))) continue;
        info.rights = required & ~(UINT64_C(1) << bit);
        assert(gpud_native_process_adopt(&process, 17, 1) == -EACCES && !process.fd);
    }
    info.rights = required;
    assert(!gpud_native_process_adopt(&process, 17, 1));
    unsigned int before = calls;
    assert(gpud_native_process_adopt(&process, 17, 2) == -EBUSY);
    assert(gpud_native_process_poll(&process, 2) == -ESTALE);
    assert(gpud_native_process_terminate(&process, 2, 9) == -ESTALE);
    assert(gpud_native_process_release(&process, 2) == -ESTALE && calls == before);
    assert(gpud_native_process_release(&process, 1) == -EBUSY && !closes);
    assert(gpud_native_process_poll(&process, 1) == -EAGAIN && !process.terminal);
    assert(!process.exit.state);
    wait_error = PACHA_SYSCALL_ERR_INVALID;
    assert(gpud_native_process_terminate(&process, 1, 9) == -EINVAL && !kills);
    wait_error = 0; observed.state = 4;
    assert(gpud_native_process_poll(&process, 1) == -EPROTO && !process.terminal);
    observed.state = GPUD_PROCESS_KILLED; observed.reserved = 1;
    assert(gpud_native_process_poll(&process, 1) == -EPROTO && !process.terminal);
    observed.reserved = 0; wait_error = PACHA_SYSCALL_ERR_NOT_READY;
    assert(gpud_native_process_terminate(&process, 1, 9) == -EAGAIN);
    assert(kills == 1 && process.kill_accepted && !process.terminal);
    assert(gpud_native_process_terminate(&process, 1, 9) == -EAGAIN && kills == 1);
    wait_error = 0;
    assert(!gpud_native_process_terminate(&process, 1, 9) && process.terminal);
    assert(process.exit.code == 9 && process.exit.principal == 100);
    before = calls;
    assert(!gpud_native_process_poll(&process, 1) && calls == before);
    close_error = PACHA_SYSCALL_ERR_NOT_READY;
    assert(gpud_native_process_release(&process, 1) == -EAGAIN && process.fd == 17);
    close_error = 0;
    assert(!gpud_native_process_release(&process, 1) && !process.fd && closes == 2);
    assert(!gpud_native_process_release(&process, 1) && closes == 2);
    assert(gpud_native_process_adopt(&process, 17, 1) == -ESTALE);
    assert(!gpud_native_process_adopt(&process, 17, 2));
    before = calls;
    assert(gpud_native_process_release(&process, 1) == -ESTALE && calls == before);
    assert(!process.terminal && !process.kill_accepted);
    assert(!gpud_native_process_poll(&process, 2));
    assert(!gpud_native_process_release(&process, 2));
}

static void test_exit_race(void) {
    for (unsigned int mode = 0; mode < 3; ++mode) {
        reset_native();
        struct gpud_native_process process = {0};
        assert(!gpud_native_process_adopt(&process, 17, 1));
        kill_error = mode == 2 ? PACHA_SYSCALL_ERR_NOT_READY : PACHA_SYSCALL_ERR_INVALID;
        exit_on_kill = mode == 1;
        observed.state = GPUD_PROCESS_EXITED; observed.code = 42;
        int result = gpud_native_process_terminate(&process, 1, 9);
        if (exit_on_kill) {
            assert(!result && process.terminal && process.exit.code == 42);
        } else {
            assert(result == (mode == 2 ? -EAGAIN : -EINVAL) && !process.terminal);
            assert(!process.kill_accepted && process.fd == 17);
            wait_error = 0;
            assert(!gpud_native_process_poll(&process, 1));
        }
        assert(!gpud_native_process_release(&process, 1));
    }
}

static void *allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void deallocate(void *context, void *pointer, size_t size) {
    (void)context; (void)size;
    free(pointer);
}

/* Other resource actions are explicit fixture completions, NOT native proof. */
static void complete(kb2_controller_t *controller, kb2_action_type_t type,
    uint64_t resource_id, uint64_t sandbox_id) {
    const kb2_action_t *action = kb2_controller_pending_action(controller);
    assert(kb2_action_type(action) == type);
    assert(kb2_controller_complete_action(controller, kb2_action_generation(action),
        kb2_action_token(action), KB2_STATUS_OK, resource_id, sandbox_id) == KB2_STATUS_OK);
}

static kb2_controller_t *new_controller(void) {
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

static void test_controller_exit(unsigned int mode) {
    reset_native();
    kb2_controller_t *controller = new_controller();
    struct gpud_native_process process = {0};
    struct ph_ipc ipc = {.generation = 1, .fd = 16, .admitted = 1};
    struct gpud_process_watch watch = {0};
    assert(!gpud_native_process_adopt(&process, 17, 1));
    assert(gpud_process_watch_init(&watch, controller, &process, &ipc, 51, 72) ==
        KB2_STATUS_INVALID_STATE && !watch.controller);
    assert(gpud_process_watch_init(&watch, controller, &process, &ipc, 51, 71) == KB2_STATUS_OK);
    unsigned int before = calls;
    assert(gpud_process_observe(&watch, 2) == KB2_STATUS_STALE_GENERATION && calls == before);
    ipc.generation = 2;
    assert(gpud_process_observe(&watch, 1) == KB2_STATUS_STALE_GENERATION && calls == before);
    ipc.generation = 1;
    process.generation = 2;
    assert(gpud_process_observe(&watch, 1) == KB2_STATUS_STALE_GENERATION && calls == before);
    process.generation = 1;
    assert(gpud_process_observe(&watch, 1) == KB2_STATUS_ACTION_PENDING && ipc.admitted);
    if (mode) {
        complete(controller, KB2_ACTION_TRANSFER_RESOURCES, 0, 0);
        assert(kb2_controller_report_ready(controller, 1) == KB2_STATUS_OK);
    }
    if (mode == 2) {
        assert(kb2_controller_report_fault(controller, 1, KB2_FAULT_PROTOCOL, 99) == KB2_STATUS_OK);
    } else {
        wait_error = 0;
        assert(gpud_process_observe(&watch, 1) == (mode ? KB2_STATUS_OK : KB2_STATUS_HOST_FAILURE));
        assert(!ipc.admitted && kb2_controller_state(controller) == KB2_STATE_FAULTED);
        assert(kb2_controller_fault_kind(controller) ==
            (mode ? KB2_FAULT_PROCESS_EXIT : KB2_FAULT_HOST_ACTION));
    }
    assert(kb2_controller_restart(controller) == KB2_STATUS_OK);
    if (mode != 1) {
        const kb2_action_t *action = kb2_controller_pending_action(controller);
        assert(kb2_action_type(action) == KB2_ACTION_TERMINATE_SANDBOX);
        uint64_t token = kb2_action_token(action);
        before = calls;
        assert(gpud_process_action(&watch, 2, token, 9) == KB2_STATUS_STALE_GENERATION);
        assert(gpud_process_action(&watch, 1, token + 1, 9) == KB2_STATUS_STALE_ACTION);
        assert(calls == before);
        if (mode == 2) {
            kill_error = PACHA_SYSCALL_ERR_INVALID;
            assert(gpud_process_action(&watch, 1, token, 9) == KB2_STATUS_HOST_FAILURE);
            assert(!process.terminal && !process.kill_accepted && !closes);
            assert(kb2_action_token(kb2_controller_pending_action(controller)) == token);
            kill_error = 0;
            assert(gpud_process_action(&watch, 1, token, 9) == KB2_STATUS_ACTION_PENDING);
            assert(!ipc.admitted && kills == 2 && !process.terminal && !closes);
            assert(gpud_process_action(&watch, 1, token, 9) == KB2_STATUS_ACTION_PENDING && kills == 2);
            wait_error = 0;
        }
        assert(gpud_process_action(&watch, 1, token, 9) == KB2_STATUS_OK);
        assert(gpud_process_action(&watch, 1, token, 9) == KB2_STATUS_STALE_ACTION);
    }
    const kb2_action_t *action = kb2_controller_pending_action(controller);
    assert(kb2_action_type(action) == KB2_ACTION_REVOKE_RESOURCES && !closes);
    assert(gpud_process_action(&watch, 1, kb2_action_token(action), 9) == KB2_STATUS_INVALID_STATE);
    complete(controller, KB2_ACTION_REVOKE_RESOURCES, 0, 0);
    action = kb2_controller_pending_action(controller);
    assert(kb2_action_type(action) == KB2_ACTION_REAP_SANDBOX);
    uint64_t token = kb2_action_token(action);
    close_error = PACHA_SYSCALL_ERR_INVALID;
    assert(gpud_process_action(&watch, 1, token, 9) == KB2_STATUS_HOST_FAILURE);
    assert(process.fd == 17 && kb2_action_token(kb2_controller_pending_action(controller)) == token);
    close_error = 0;
    assert(gpud_process_action(&watch, 1, token, 9) == KB2_STATUS_OK && !process.fd);
    complete(controller, KB2_ACTION_RELEASE_RESOURCES, 0, 0);
    assert(kb2_controller_generation(controller) == 2);
    before = calls;
    assert(gpud_process_observe(&watch, 1) == KB2_STATUS_STALE_GENERATION);
    assert(gpud_process_action(&watch, 1, token, 9) == KB2_STATUS_STALE_GENERATION && calls == before);
    /* Fail the new allocation: no live native process/resource remains. */
    action = kb2_controller_pending_action(controller);
    assert(kb2_controller_complete_action(controller, 2, kb2_action_token(action),
        KB2_STATUS_HOST_FAILURE, 0, 0) == KB2_STATUS_HOST_FAILURE);
    assert(kb2_controller_stop(controller) == KB2_STATUS_OK);
    assert(kb2_controller_state(controller) == KB2_STATE_IDLE);
    kb2_controller_destroy(controller);
}

int main(void) {
    test_native_owner();
    test_exit_race();
    for (unsigned int mode = 0; mode < 3; ++mode) test_controller_exit(mode);
    puts("gpud process ownership/controller exit unit: PASS (not a service recovery Gate)");
    return 0;
}
