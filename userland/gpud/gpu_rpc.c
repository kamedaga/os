/* SPDX-License-Identifier: MIT */
#include "gpu_rpc.h"
#include "../kobox2_adapter/gpu_queue_message.h"

#include <errno.h>
#include <pacha/syscall.h>
#include <string.h>

static int now_ms(uint64_t *now) {
    uint64_t time[2];
    long result = pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME, 1, (uintptr_t)time);
    if (result)
        return result == PACHA_SYSCALL_ERR_NOT_READY ? -EAGAIN : -EIO;
    *now = time[0] * 1000 + time[1] / 1000000;
    return 0;
}

static int wait_wake(struct gpud_gpu_rpc *rpc, uint64_t notification, uint64_t deadline) {
    for (;;) {
        struct ph_ipc_packet *packet = &rpc->incoming;
        int error = ph_ipc_receive(rpc->ipc, rpc->ipc->generation, packet);
        if (!error) {
            int valid = packet->operation == PH_GPU_QUEUE_NOTIFY && !packet->correlation &&
                !packet->fd_count && packet->value == notification;
            int cleanup = ph_ipc_packet_release(packet);
            return cleanup ? cleanup : valid ? 0 : -EPROTO;
        }
        if (error != -EAGAIN)
            return error;
        uint64_t now;
        error = now_ms(&now);
        if (error)
            return error;
        if (now >= deadline)
            return -ETIMEDOUT;
        struct pacha_pollfd event = {
            .fd = rpc->ipc->fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
        long result = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
            (uintptr_t)&event, 1, deadline - now, 0);
        if (result == 1 && event.revents) {
            if (event.revents & PACHA_FD_EVENT_HANGUP)
                return -EPIPE;
        } else if (event.revents || (result && result != PACHA_SYSCALL_ERR_NOT_READY)) {
            return -EIO;
        }
    }
}

int gpud_gpu_rpc_call(struct gpud_gpu_rpc *rpc, unsigned int queue,
    const unsigned char *request, size_t request_size,
    unsigned char *reply, size_t reply_capacity, size_t *reply_size) {
    if (!rpc || !rpc->ipc || !rpc->channel || !rpc->mapping || !request ||
        !reply || !reply_size || (queue != GPUD_GPU_QUEUE_CONTROL &&
        queue != GPUD_GPU_QUEUE_EXECUTION))
        return -EINVAL;
    if (rpc->error)
        return rpc->error;
    int control = queue == GPUD_GPU_QUEUE_CONTROL;
    size_t request_offset = control ? GPUD_GPU_CONTROL_REQUEST_OFFSET : GPUD_GPU_REQUEST_OFFSET;
    size_t response_offset = control ? GPUD_GPU_CONTROL_REPLY_OFFSET : GPUD_GPU_REPLY_OFFSET;
    size_t capacity = control ? GPUD_GPU_CONTROL_REPLY_CAPACITY : GPUD_GPU_CHANNEL_PAGE;
    if (!request_size || request_size > (control ? 2048u : GPUD_GPU_CHANNEL_PAGE) ||
        reply_capacity < capacity)
        return -EMSGSIZE;
    uint64_t deadline;
    int error = now_ms(&deadline);
    if (error)
        return error;
    deadline += 5000;
    uint64_t generation = rpc->ipc->generation;
    kb2_vq_t *lane = &rpc->channel->lanes[queue];
    if (request != rpc->mapping + request_offset)
        memcpy(rpc->mapping + request_offset, request, request_size);
    rpc->segments[0] = (kb2_vq_segment_t){request_offset, request_size, 0, 0};
    rpc->segments[1] = (kb2_vq_segment_t){response_offset, capacity, 1, 1};
    rpc->chain = (kb2_vq_chain_t){.segments = rpc->segments, .capacity = 2, .count = 2};
    int ready, notify;
    if (kb2_vq_arm(lane, generation, &ready) || ready ||
        kb2_vq_publish(lane, generation, &rpc->chain, &notify))
        goto protocol_error;
    if (notify) {
        struct ph_ipc_packet packet = {.operation = PH_GPU_QUEUE_NOTIFY,
            .generation = generation, .value = lane->queue.available_notification_id};
        error = ph_ipc_send(rpc->ipc, &packet);
        if (error)
            goto failed;
    }
    /* Arming before publication requires a used wake even when the ring is
     * already complete. Consume it before returning IPC to lifecycle handling. */
    error = wait_wake(rpc, lane->queue.used_notification_id, deadline);
    if (error)
        goto failed;
    kb2_vq_chain_t *done = NULL;
    if (kb2_vq_take_used(lane, generation, &done) || done != &rpc->chain ||
        done->used_length < KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE || done->used_length > capacity ||
        kb2_vq_copy_response(lane, generation, done, 0, reply, done->used_length))
        goto protocol_error;
    *reply_size = done->used_length;
    if (kb2_vq_release(lane, generation, done))
        goto protocol_error;
    return 0;
protocol_error:
    error = -EPROTO;
failed:
    rpc->error = error;
    return error;
}
