/* SPDX-License-Identifier: MIT */
#include "gpu_rpc.h"
#include <kobox2/gpu_sync_file.h>
#include "../kobox2_adapter/gpu_queue_message.h"
#include "../kobox2_adapter/gpu_event_message.h"

#include <errno.h>
#include <pacha/syscall.h>
#include <string.h>

#if defined(GPUD_DRM_RPC_PROFILE) && GPUD_DRM_RPC_PROFILE
extern int gpud_drm_profile_enabled(void);
static uint64_t rpc_stamp(void) {
    if (!gpud_drm_profile_enabled())
        return 0;
    uint32_t low, high;
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high) :: "memory");
    return (uint64_t)high << 32 | low;
}
#else
static inline uint64_t rpc_stamp(void) { return 0; }
#endif

/* Preserve the first fault before cleanup and subsequent calls can replace
 * the useful admission site with an already-terminal error return. */
static int rpc_failed(struct gpud_gpu_rpc *rpc, int error, unsigned int line) {
    if (!rpc->error) {
        rpc->error = error;
        rpc->failure_line = line;
    }
    return error;
}
#define RPC_FAIL(rpc, error) rpc_failed((rpc), (error), __LINE__)

static int now_ms(uint64_t *now) {
    uint64_t time[2];
    long result = pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME, 1, (uintptr_t)time);
    if (result)
        return result == PACHA_SYSCALL_ERR_NOT_READY ? -EAGAIN : -EIO;
    *now = time[0] * 1000 + time[1] / 1000000;
    return 0;
}

static int is_event_notification(
    const struct gpud_gpu_rpc *rpc, const struct ph_ipc_packet *packet) {
    return packet->operation == PH_GPU_QUEUE_NOTIFY &&
        !packet->correlation && !packet->fd_count &&
        packet->value == rpc->channel->lanes[GPUD_GPU_QUEUE_EVENT]
            .queue.used_notification_id;
}

static int receive_until(struct gpud_gpu_rpc *rpc, uint64_t deadline) {
    uint64_t now;
    int error = now_ms(&now);
    if (error)
        return error;
    if (now >= deadline)
        return -ETIMEDOUT;
    return ph_ipc_receive_wait(rpc->ipc, rpc->ipc->generation,
        &rpc->incoming, deadline - now);
}

static int wait_wake(struct gpud_gpu_rpc *rpc, uint64_t notification, uint64_t deadline) {
    for (;;) {
        struct ph_ipc_packet *packet = &rpc->incoming;
        int error = receive_until(rpc, deadline);
        if (!error) {
            if (is_event_notification(rpc, packet)) {
                rpc->event_notification = 1;
                int cleanup = ph_ipc_packet_release(packet);
                if (cleanup)
                    return cleanup;
                continue;
            }
            int valid = packet->operation == PH_GPU_QUEUE_NOTIFY && !packet->correlation &&
                !packet->fd_count && packet->value == notification;
            if (!valid &&
                (packet->operation == PH_GPU_MAPPING_ATTACHMENT ||
                 packet->operation == PH_GPU_DMA_BUF_ATTACHMENT) &&
                packet->correlation && packet->value && packet->fd_count == 1 &&
                !rpc->attachment.fd_count) {
                rpc->attachment = *packet;
                *packet = (struct ph_ipc_packet){0};
                continue;
            }
            int cleanup = ph_ipc_packet_release(packet);
            return cleanup ? cleanup : valid ? 0 : RPC_FAIL(rpc, -EPROTO);
        }
        if (error != -EAGAIN)
            return error;
    }
}

static int post_event_buffer(struct gpud_gpu_rpc *rpc) {
    kb2_vq_t *lane = &rpc->channel->lanes[GPUD_GPU_QUEUE_EVENT];
    int ready, notify;
    if (kb2_vq_arm(lane, rpc->ipc->generation, &ready) || ready ||
        kb2_vq_publish(lane, rpc->ipc->generation,
            &rpc->event_chain, &notify))
        return -EPROTO;
    if (notify) {
        struct ph_ipc_packet packet = {
            .operation = PH_GPU_QUEUE_NOTIFY,
            .generation = rpc->ipc->generation,
            .value = lane->queue.available_notification_id,
        };
        return ph_ipc_send(rpc->ipc, &packet);
    }
    return 0;
}

int gpud_gpu_rpc_start_events(struct gpud_gpu_rpc *rpc) {
    if (!rpc || !rpc->ipc || !rpc->channel || !rpc->mapping ||
        rpc->error || rpc->event_started)
        return -EINVAL;
    rpc->event_segment = (kb2_vq_segment_t) {
        .address = GPUD_GPU_EVENT_OFFSET,
        .length = GPUD_GPU_CHANNEL_PAGE,
        .writable = 1,
    };
    rpc->event_chain = (kb2_vq_chain_t) {
        .segments = &rpc->event_segment,
        .capacity = 1,
        .count = 1,
    };
    int error = post_event_buffer(rpc);
    if (error)
        return RPC_FAIL(rpc, error);
    rpc->event_started = 1;
    return 0;
}

int gpud_gpu_rpc_next_event(struct gpud_gpu_rpc *rpc,
    unsigned char *message, size_t capacity, size_t *size_out) {
    if (!rpc || !rpc->ipc || !rpc->channel || !rpc->mapping ||
        !message || !size_out || capacity < GPUD_GPU_CHANNEL_PAGE ||
        !rpc->event_started)
        return -EINVAL;
    if (rpc->error)
        return rpc->error;
    if (!rpc->event_notification) {
        int error = ph_ipc_receive(
            rpc->ipc, rpc->ipc->generation, &rpc->incoming);
        if (error == -EAGAIN)
            return 0;
        if (error)
            return RPC_FAIL(rpc, error);
        const struct ph_ipc_packet *packet = &rpc->incoming;
        int valid = is_event_notification(rpc, packet);
        int cleanup = ph_ipc_packet_release(&rpc->incoming);
        if (cleanup)
            return RPC_FAIL(rpc, cleanup);
        if (!valid)
            return RPC_FAIL(rpc, -EPROTO);
        rpc->event_notification = 1;
    }
    kb2_vq_t *lane = &rpc->channel->lanes[GPUD_GPU_QUEUE_EVENT];
    kb2_vq_chain_t *done = NULL;
    if (kb2_vq_take_used(lane, rpc->ipc->generation, &done) ||
        done != &rpc->event_chain || !done->used_length ||
        done->used_length > GPUD_GPU_CHANNEL_PAGE ||
        kb2_vq_copy_response(lane, rpc->ipc->generation, done, 0,
            message, done->used_length))
        return RPC_FAIL(rpc, -EPROTO);
    const size_t used = done->used_length;
    if (kb2_vq_release(lane, rpc->ipc->generation, done))
        return RPC_FAIL(rpc, -EPROTO);
    *size_out = used;
    rpc->event_notification = 0;
    int error = post_event_buffer(rpc);
    if (error)
        return RPC_FAIL(rpc, error);
    return 1;
}

static int take_attachment(struct gpud_gpu_rpc *rpc, uint32_t operation,
    uint64_t correlation, uint64_t exchange_id, struct pacha_ipc_fd *fd) {
    if (!rpc || !fd || !correlation || !exchange_id)
        return -EINVAL;
    struct ph_ipc_packet *packet = &rpc->attachment;
    if (!packet->fd_count)
        return -ENOENT;
    if (packet->operation != operation ||
        packet->generation != rpc->ipc->generation ||
        packet->correlation != correlation || packet->value != exchange_id ||
        packet->fd_count != 1)
        return -EPROTO;
    *fd = packet->fds[0];
    packet->fds[0].fd = PH_IPC_NO_FD;
    *packet = (struct ph_ipc_packet){0};
    return 0;
}

int gpud_gpu_rpc_take_mapping(struct gpud_gpu_rpc *rpc,
    uint64_t correlation, uint64_t exchange_id, struct pacha_ipc_fd *fd) {
    return take_attachment(rpc, PH_GPU_MAPPING_ATTACHMENT,
        correlation, exchange_id, fd);
}

int gpud_gpu_rpc_take_dma_buf(struct gpud_gpu_rpc *rpc,
    uint64_t correlation, uint64_t exchange_id, struct pacha_ipc_fd *fd) {
    return take_attachment(rpc, PH_GPU_DMA_BUF_ATTACHMENT,
        correlation, exchange_id, fd);
}

int gpud_gpu_rpc_release_mapping(struct gpud_gpu_rpc *rpc,
    uint64_t correlation, uint64_t mapping_id) {
    if (!rpc || !rpc->ipc || !correlation || !mapping_id)
        return -EINVAL;
    if (rpc->error)
        return rpc->error;
    if (rpc->attachment.fd_count || rpc->incoming.fd_count)
        goto protocol_error;
    struct ph_ipc_packet request = {
        .operation = PH_GPU_MAPPING_RELEASE,
        .generation = rpc->ipc->generation,
        .correlation = correlation,
        .value = mapping_id,
    };
    int error = ph_ipc_send(rpc->ipc, &request);
    if (error)
        goto failed;
    uint64_t deadline;
    error = now_ms(&deadline);
    if (error)
        goto failed;
    deadline += 5000;
    for (;;) {
        error = receive_until(rpc, deadline);
        if (!error) {
            const struct ph_ipc_packet *reply = &rpc->incoming;
            if (is_event_notification(rpc, reply)) {
                rpc->event_notification = 1;
                int cleanup = ph_ipc_packet_release(&rpc->incoming);
                if (cleanup) {
                    error = cleanup;
                    goto failed;
                }
                continue;
            }
            int valid = reply->operation == PH_GPU_MAPPING_RELEASED &&
                reply->generation == rpc->ipc->generation &&
                reply->correlation == correlation && reply->value == mapping_id &&
                !reply->fd_count;
            int cleanup = ph_ipc_packet_release(&rpc->incoming);
            if (cleanup) {
                error = cleanup;
                goto failed;
            }
            if (!valid)
                goto protocol_error;
            return 0;
        }
        if (error != -EAGAIN)
            goto failed;
    }
protocol_error:
    error = -EPROTO;
failed:
    return RPC_FAIL(rpc, error);
}

int gpud_gpu_rpc_call(struct gpud_gpu_rpc *rpc, unsigned int queue,
    const unsigned char *request, size_t request_size,
    unsigned char *reply, size_t reply_capacity, size_t *reply_size) {
    if (!rpc || !rpc->ipc || !rpc->channel || !rpc->mapping || !request ||
        !reply || !reply_size || (queue != GPUD_GPU_QUEUE_CONTROL &&
        queue != GPUD_GPU_QUEUE_DISPLAY &&
        queue != GPUD_GPU_QUEUE_EXECUTION))
        return -EINVAL;
    if (rpc->error)
        return rpc->error;
    const int control = queue == GPUD_GPU_QUEUE_CONTROL;
    const int display = queue == GPUD_GPU_QUEUE_DISPLAY;
    size_t request_offset = control ? GPUD_GPU_CONTROL_REQUEST_OFFSET :
        display ? GPUD_GPU_DISPLAY_REQUEST_OFFSET : GPUD_GPU_REQUEST_OFFSET;
    size_t response_offset = control ? GPUD_GPU_CONTROL_REPLY_OFFSET :
        display ? GPUD_GPU_DISPLAY_REPLY_OFFSET : GPUD_GPU_REPLY_OFFSET;
    size_t capacity = control ? GPUD_GPU_CONTROL_REPLY_CAPACITY :
        display ? GPUD_GPU_DISPLAY_REPLY_CAPACITY : GPUD_GPU_CHANNEL_PAGE;
    if (!request_size || request_size >
        (control || display ? 2048u : GPUD_GPU_OUTPUT_OFFSET - GPUD_GPU_REQUEST_OFFSET) ||
        reply_capacity < capacity)
        return -EMSGSIZE;
    const uint64_t profile_start = rpc_stamp();
    uint64_t deadline;
    int error = now_ms(&deadline);
    if (error)
        return error;
    /* GET_CAPS may wait up to 5*HZ in virtio-gpu. Execution must leave room
     * for the Linux completion and the adapter's private-buffer copy-out. */
    deadline += control ? 5000 : 15000;
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
    const uint64_t profile_published = rpc_stamp();
    if (notify) {
        struct ph_ipc_packet packet = {.operation = PH_GPU_QUEUE_NOTIFY,
            .generation = generation, .value = lane->queue.available_notification_id};
        error = ph_ipc_send(rpc->ipc, &packet);
        if (error)
            goto failed;
    }
    const uint64_t profile_notified = rpc_stamp();
    /* Arming before publication requires a used wake even when the ring is
     * already complete. Consume it before returning IPC to lifecycle handling. */
    error = wait_wake(rpc, lane->queue.used_notification_id, deadline);
    if (error)
        goto failed;
    const uint64_t profile_used = rpc_stamp();
    kb2_vq_chain_t *done = NULL;
    if (kb2_vq_take_used(lane, generation, &done) || done != &rpc->chain ||
        done->used_length < KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE || done->used_length > capacity ||
        kb2_vq_copy_response(lane, generation, done, 0, reply, done->used_length))
        goto protocol_error;
    *reply_size = done->used_length;
    if (kb2_vq_release(lane, generation, done))
        goto protocol_error;
#if defined(GPUD_DRM_RPC_PROFILE) && GPUD_DRM_RPC_PROFILE
    if (gpud_drm_profile_enabled()) {
        rpc->profile_calls++;
        rpc->profile_prepare += profile_published - profile_start;
        rpc->profile_notify += profile_notified - profile_published;
        rpc->profile_wait += profile_used - profile_notified;
        rpc->profile_copy += rpc_stamp() - profile_used;
    }
#else
    (void)profile_start; (void)profile_published; (void)profile_notified; (void)profile_used;
#endif
    return 0;
protocol_error:
    error = -EPROTO;
failed:
    return RPC_FAIL(rpc, error);
}

int gpud_gpu_rpc_prime_cpu_sync(struct gpud_gpu_rpc *rpc,
    uint64_t correlation, uint64_t token, uint64_t flags)
{
    if (!rpc || !rpc->ipc || !correlation || !token || !(flags & 3) || (flags & ~UINT64_C(7)))
        return -EINVAL;
    unsigned char request[KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + KB2_GPU_DMA_BUF_CPU_SYNC_REQUEST_SIZE];
    unsigned char reply[GPUD_GPU_CONTROL_REPLY_CAPACITY];
    const kb2_protocol_message_envelope_t envelope = {
        .protocol_id = KB2_GPU_PROTOCOL_ID, .opcode = KB2_GPU_OPCODE_DMA_BUF_CPU_SYNC,
        .generation = rpc->ipc->generation, .correlation_id = correlation,
        .payload_length = KB2_GPU_DMA_BUF_CPU_SYNC_REQUEST_SIZE};
    if (kb2_protocol_message_envelope_encode(request, sizeof(request), &envelope)) return -EPROTO;
    unsigned char *body = request + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE;
    ph_gpu_event_store_u64(body + KB2_GPU_DMA_BUF_CPU_SYNC_REQUEST_TOKEN_OFFSET, token);
    ph_gpu_event_store_u64(body + KB2_GPU_DMA_BUF_CPU_SYNC_REQUEST_FLAGS_OFFSET, flags);
    size_t size = 0;
    int error = gpud_gpu_rpc_call(rpc, GPUD_GPU_QUEUE_CONTROL, request, sizeof(request),
        reply, sizeof(reply), &size);
    if (error) return error;
    kb2_protocol_message_envelope_t decoded;
    if (size != KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + KB2_GPU_DMA_BUF_CPU_SYNC_RESPONSE_SIZE ||
        kb2_protocol_message_envelope_decode(reply, size, &decoded) ||
        decoded.protocol_id != envelope.protocol_id || decoded.opcode != envelope.opcode ||
        decoded.generation != envelope.generation || decoded.correlation_id != correlation ||
        decoded.flags || decoded.payload_length != KB2_GPU_DMA_BUF_CPU_SYNC_RESPONSE_SIZE ||
        rpc->attachment.fd_count) return RPC_FAIL(rpc, -EPROTO);
    body = reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE;
    const int64_t result = (int64_t)ph_gpu_event_load_u64(body + KB2_GPU_DMA_BUF_CPU_SYNC_RESPONSE_RESULT_OFFSET);
    if (ph_gpu_event_load_u64(body + KB2_GPU_DMA_BUF_CPU_SYNC_RESPONSE_TOKEN_OFFSET) != token ||
        ph_gpu_event_load_u64(body + KB2_GPU_DMA_BUF_CPU_SYNC_RESPONSE_FLAGS_OFFSET) != flags ||
        result > 0 || result < -4095) return RPC_FAIL(rpc, -EPROTO);
    return (int)result;
}

static int sync_file_control(struct gpud_gpu_rpc *rpc,uint64_t correlation,
    const kb2_gpu_sync_file_request_t *value,uint64_t *exported)
{
    if(!rpc || !rpc->ipc || !correlation || !exported ||
        !kb2_gpu_sync_file_request_valid(value)) return -EINVAL;
    unsigned char request[KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE+KB2_GPU_DMA_BUF_EXPORT_SYNC_FILE_REQUEST_SIZE];
    unsigned char reply[GPUD_GPU_CONTROL_REPLY_CAPACITY];
    const size_t bytes=kb2_gpu_sync_file_request_size(value->opcode);
    const kb2_protocol_message_envelope_t envelope={.protocol_id=KB2_GPU_PROTOCOL_ID,
        .opcode=value->opcode,.generation=rpc->ipc->generation,.correlation_id=correlation,
        .payload_length=bytes};
    if(kb2_protocol_message_envelope_encode(request,KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE+bytes,&envelope) ||
        kb2_gpu_sync_file_request_encode(request+KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,bytes,value)) return -EPROTO;
    size_t size=0;
    int error=gpud_gpu_rpc_call(rpc,GPUD_GPU_QUEUE_CONTROL,request,
        KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE+bytes,reply,sizeof(reply),&size);
    if(error) return error;
    kb2_protocol_message_envelope_t decoded;
    int result;uint64_t token;
    if(size!=KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE+kb2_gpu_sync_file_response_size(value->opcode) ||
        kb2_protocol_message_envelope_decode(reply,size,&decoded) ||
        decoded.protocol_id!=envelope.protocol_id || decoded.opcode!=envelope.opcode ||
        decoded.generation!=envelope.generation || decoded.correlation_id!=correlation ||
        decoded.flags || decoded.payload_length!=size-KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE ||
        rpc->attachment.fd_count || kb2_gpu_sync_file_response_decode(
            reply+KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,decoded.payload_length,value,&result,&token))
        return RPC_FAIL(rpc, -EPROTO);
    if(!result) *exported=token;
    return result;
}

int gpud_gpu_rpc_prime_export_sync_file(struct gpud_gpu_rpc *rpc,uint64_t correlation,
    uint64_t token,uint64_t flags,uint64_t session,uint64_t *exported)
{
    const kb2_gpu_sync_file_request_t request={.opcode=KB2_GPU_OPCODE_DMA_BUF_EXPORT_SYNC_FILE,
        .token=token,.flags=flags,.session_id=session};
    return sync_file_control(rpc,correlation,&request,exported);
}
int gpud_gpu_rpc_prime_import_sync_file(struct gpud_gpu_rpc *rpc,uint64_t correlation,
    uint64_t token,uint64_t flags,uint64_t sync_file)
{
    const kb2_gpu_sync_file_request_t request={.opcode=KB2_GPU_OPCODE_DMA_BUF_IMPORT_SYNC_FILE,
        .token=token,.flags=flags,.sync_file_token=sync_file};
    uint64_t unused;
    return sync_file_control(rpc,correlation,&request,&unused);
}
int gpud_gpu_rpc_release_sync_file(struct gpud_gpu_rpc *rpc,uint64_t correlation,uint64_t token)
{
    const kb2_gpu_sync_file_request_t request={.opcode=KB2_GPU_OPCODE_SYNC_FILE_RELEASE,.token=token};
    uint64_t unused;
    return sync_file_control(rpc,correlation,&request,&unused);
}
