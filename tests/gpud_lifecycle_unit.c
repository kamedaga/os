/* SPDX-License-Identifier: MIT */
/* Real controller/IPC/process/lifecycle binding; only native syscalls modeled. */
#include "../userland/gpud/lifecycle.h"
#include "../userland/kobox2_adapter/lifecycle_message.h"
#include "gpud_controller_fixture.h"
#include <pacha/syscall.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static long receive_error, send_error, wait_error, close_error;
static unsigned int calls, sends, closed;
static struct gpud_process_exit exit_status;
static uint64_t ready_operation, ready_generation, ready_value, ready_fd;

long pacha_syscall4(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)nr; (void)a; (void)b; (void)c; (void)d;
    abort(); /* Lifecycle step tests only use nonblocking receive. */
}

long pacha_syscall1(uint64_t nr, uint64_t fd) {
    assert(nr == PACHA_FD_SYSCALL_CLOSE && (fd == 17 || fd == 40));
    ++calls;
    if (close_error) return close_error;
    ++closed;
    return 0;
}

long pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t address) {
    ++calls;
    if (nr == PACHA_PROCESS_SYSCALL_WAIT) {
        assert(fd == 17);
        *(struct gpud_process_exit *)(uintptr_t)address = exit_status;
        return wait_error;
    }
    if (nr == PACHA_PROCESS_SYSCALL_KILL) {
        assert(fd == 17 && address == 9);
        wait_error = 0;
        exit_status = (struct gpud_process_exit){.state = GPUD_PROCESS_KILLED, .code = 9};
        return 0;
    }
    assert(fd == 18);
    struct pacha_ipc_msg *message = (void *)(uintptr_t)address;
    if (nr == PACHA_IPC_SYSCALL_SEND) {
        assert(message->word0 == PH_LIFECYCLE_QUIESCE && message->word1 == 1 &&
            message->word2 && !message->word3 && !message->fd_count);
        if (!send_error) ++sends;
        return send_error;
    }
    assert(nr == PACHA_IPC_SYSCALL_RECV);
    if (receive_error) return receive_error;
    message->word0 = ready_operation;
    message->word1 = ready_generation;
    message->word2 = 0;
    message->word3 = ready_value;
    message->fd_count = ready_fd ? 1 : 0;
    if (ready_fd) message->fds[0] = (struct pacha_ipc_fd){.fd = ready_fd};
    return 0;
}

static void *allocate(void *context, size_t size) { (void)context; return malloc(size); }
static void deallocate(void *context, void *pointer, size_t size) {
    (void)context; (void)size; free(pointer);
}

static kb2_controller_t *setup(struct gpud_process_watch *watch,
    struct gpud_native_process *process, struct ph_ipc *ipc, struct gpud_lifecycle *lifecycle) {
    receive_error = send_error = close_error = 0;
    wait_error = PACHA_SYSCALL_ERR_NOT_READY;
    exit_status = (struct gpud_process_exit){0};
    ready_operation = PH_LIFECYCLE_READY; ready_generation = 1; ready_value = ready_fd = 0;
    calls = sends = closed = 0;
    kb2_controller_t *controller = gpud_test_controller_create(allocate, deallocate, NULL);
    assert(kb2_controller_start(controller) == KB2_STATUS_OK);
    gpud_test_complete(controller, KB2_ACTION_ALLOCATE_RESOURCES, 50, 0);
    gpud_test_complete(controller, KB2_ACTION_LAUNCH_SANDBOX, 0, 70);
    *process = (struct gpud_native_process){.generation = 1, .fd = 17};
    *ipc = (struct ph_ipc){.generation = 1, .fd = 18, .admitted = 1};
    assert(gpud_process_watch_init(watch, controller, process, ipc, 50, 70) == KB2_STATUS_OK);
    assert(gpud_lifecycle_init(lifecycle, watch) == KB2_STATUS_INVALID_STATE);
    gpud_test_complete(controller, KB2_ACTION_TRANSFER_RESOURCES, 0, 0);
    assert(gpud_lifecycle_init(lifecycle, watch) == KB2_STATUS_OK);
    return controller;
}

static void cleanup(kb2_controller_t *controller, struct gpud_process_watch *watch) {
    if (!kb2_controller_pending_action(controller))
        assert(kb2_controller_stop(controller) == KB2_STATUS_OK);
    while (kb2_controller_pending_action(controller)) {
        const kb2_action_t *action = kb2_controller_pending_action(controller);
        kb2_action_type_t type = kb2_action_type(action);
        if (type == KB2_ACTION_TERMINATE_SANDBOX || type == KB2_ACTION_REAP_SANDBOX)
            assert(gpud_process_action(watch, 1, kb2_action_token(action), 9) == KB2_STATUS_OK);
        else {
            assert(type == KB2_ACTION_REVOKE_RESOURCES || type == KB2_ACTION_RELEASE_RESOURCES);
            gpud_test_complete(controller, type, 0, 0);
        }
    }
    assert(!watch->process->fd && kb2_controller_state(controller) == KB2_STATE_IDLE);
    kb2_controller_destroy(controller);
}

static void ready_rejections(void) {
    for (unsigned int mode = 0; mode < 5; ++mode) {
        struct gpud_process_watch watch = {0}; struct gpud_native_process process = {0};
        struct ph_ipc ipc = {0}; struct gpud_lifecycle lifecycle = {0};
        kb2_controller_t *controller = setup(&watch, &process, &ipc, &lifecycle);
        if (mode == 0) ready_operation = PH_LIFECYCLE_QUIESCE;
        if (mode == 1) ready_generation = 2;
        if (mode == 2) ready_value = 1;
        if (mode == 3) { ready_fd = 40; close_error = PACHA_SYSCALL_ERR_NOT_READY; }
        if (mode == 4) { wait_error = 0; exit_status.state = GPUD_PROCESS_EXITED; }
        assert(gpud_lifecycle_ready(&lifecycle) == KB2_STATUS_HOST_FAILURE && !ipc.admitted);
        assert(!lifecycle.ready);
        if (mode == 3) {
            assert(lifecycle.incoming.fd_count == 1 && !closed);
            close_error = 0;
            assert(gpud_lifecycle_ready(&lifecycle) == KB2_STATUS_HOST_FAILURE);
            assert(!lifecycle.incoming.fd_count && closed == 1);
        }
        assert(kb2_controller_state(controller) == KB2_STATE_FAULTED);
        assert(!gpud_lifecycle_release(&lifecycle));
        cleanup(controller, &watch);
    }
}

static void quiesce_round(unsigned int mode) {
    struct gpud_process_watch watch = {0}; struct gpud_native_process process = {0};
    struct ph_ipc ipc = {0}; struct gpud_lifecycle lifecycle = {0};
    kb2_controller_t *controller = setup(&watch, &process, &ipc, &lifecycle);
    receive_error = PACHA_SYSCALL_ERR_NOT_READY;
    assert(gpud_lifecycle_ready(&lifecycle) == KB2_STATUS_ACTION_PENDING);
    receive_error = 0;
    assert(gpud_lifecycle_ready(&lifecycle) == KB2_STATUS_OK && lifecycle.ready);
    assert(kb2_controller_state(controller) == KB2_STATE_RUNNING);
    assert(gpud_lifecycle_ready(&lifecycle) == KB2_STATUS_INVALID_STATE);
    assert(kb2_controller_stop(controller) == KB2_STATUS_OK);
    uint64_t token = kb2_action_token(kb2_controller_pending_action(controller));
    unsigned int before = calls;
    assert(gpud_lifecycle_quiesce(&lifecycle, token + 1) == KB2_STATUS_STALE_ACTION && calls == before);
    ipc.generation = 2;
    assert(gpud_lifecycle_quiesce(&lifecycle, token) == KB2_STATUS_STALE_GENERATION && calls == before);
    ipc.generation = 1;
    send_error = PACHA_SYSCALL_ERR_NOT_READY;
    assert(gpud_lifecycle_quiesce(&lifecycle, token) == KB2_STATUS_ACTION_PENDING && !sends);
    send_error = 0;
    assert(gpud_lifecycle_quiesce(&lifecycle, token) == KB2_STATUS_ACTION_PENDING && sends == 1);
    assert(gpud_lifecycle_quiesce(&lifecycle, token) == KB2_STATUS_ACTION_PENDING && sends == 1);
    assert(!process.terminal && process.fd == 17 && !closed);
    if (mode == 2) wait_error = PACHA_SYSCALL_ERR_INVALID;
    else { wait_error = 0; exit_status.state = GPUD_PROCESS_EXITED; exit_status.code = mode ? 1 : 0; }
    assert(gpud_lifecycle_quiesce(&lifecycle, token) == (mode ? KB2_STATUS_HOST_FAILURE : KB2_STATUS_OK));
    assert(lifecycle.completed && !ipc.admitted && !closed && sends == 1);
    if (mode == 2) wait_error = PACHA_SYSCALL_ERR_NOT_READY;
    assert(!gpud_lifecycle_release(&lifecycle));
    cleanup(controller, &watch);
}

int main(void) {
    ready_rejections();
    for (unsigned int mode = 0; mode < 3; ++mode) quiesce_round(mode);
    puts("gpud module lifecycle binding: PASS (not DRM service readiness)");
    return 0;
}
