/* SPDX-License-Identifier: MIT */
#include "gpu_queue.h"

#include <errno.h>
#include <string.h>

static int channel_fault(struct ph_gpu_queue *queue) {
    if (queue->bound)
        kb2_vq_channel_fault(&queue->channel.channel);
    return -EPROTO;
}

static int overlaps(uint64_t address, uint64_t length,
                    uint64_t start, uint64_t end) {
    return length && address < end &&
        (address >= start || length > start - address);
}

static int stop(void *context) {
    struct ph_gpu_queue *queue = context;
    int result = ph_gpu_query_release_aux(&queue->service.query);
    if (!queue->mapping)
        return result;
    /* This is terminal retirement, never an in-place restart. All queue
     * owners have stopped before the lifecycle invokes this callback. */
    if (queue->bound)
        kb2_vq_channel_fault(&queue->channel.channel);
    if (pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP,
                       (uintptr_t)queue->mapping,
                       GPUD_GPU_CHANNEL_SIZE)) {
        if (!result)
            result = -EIO;
    } else {
        queue->mapping = NULL;
    }
    return result;
}

static int next_request(void *context) {
    struct ph_gpu_queue *queue = context;
    if (!queue->mapping || queue->release_mapping_id)
        return PH_LIFECYCLE_SERVICE_IDLE;
    struct ph_gpu_query *query = &queue->service.query;
    if (query->terminal_after_completion)
        return query->terminal_after_completion;
    uint64_t generation = query->generation;
    /* Events are delivered by DRM read/poll; no peer submissions belong on
     * the protocol event lane. */
    int event_ready;
    if (kb2_vq_arm(&queue->channel.lanes[GPUD_GPU_QUEUE_EVENT], generation,
                   &event_ready) || event_ready)
        return channel_fault(queue);
    kb2_vq_t *lane = NULL;
    kb2_vq_status_t status = KB2_VQ_EMPTY;
    /* One dispatch at a time, alternating lanes to prevent starvation. A
     * CLOSE can run only after the preceding accepted command has released
     * its file. Unconsumed avail entries are not yet admitted commands. */
    for (unsigned int attempt = 0; attempt < 3; ++attempt) {
        queue->active_lane = queue->next_lane;
        queue->next_lane = queue->next_lane == GPUD_GPU_QUEUE_CONTROL ?
            GPUD_GPU_QUEUE_DISPLAY : queue->next_lane == GPUD_GPU_QUEUE_DISPLAY ?
            GPUD_GPU_QUEUE_EXECUTION : GPUD_GPU_QUEUE_CONTROL;
        lane = &queue->channel.lanes[queue->active_lane];
        status = kb2_vq_take_available(lane, generation, &queue->request);
        if (status == KB2_VQ_EMPTY) {
            int ready;
            if (kb2_vq_arm(lane, generation, &ready))
                return channel_fault(queue);
            if (ready)
                status = kb2_vq_take_available(lane, generation, &queue->request);
        }
        if (status != KB2_VQ_EMPTY)
            break;
    }
    if (status == KB2_VQ_EMPTY)
        return PH_LIFECYCLE_SERVICE_IDLE;
    size_t reply_capacity = queue->active_lane == GPUD_GPU_QUEUE_CONTROL ?
        GPUD_GPU_CONTROL_REPLY_CAPACITY :
        queue->active_lane == GPUD_GPU_QUEUE_DISPLAY ?
            GPUD_GPU_DISPLAY_REPLY_CAPACITY : sizeof(query->reply);
    if (status || queue->request.readable < KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE ||
        queue->request.readable > sizeof(query->request) ||
        queue->request.writable < reply_capacity)
        return channel_fault(queue);
    /* GPU output spans use the pre-bound output page. A descriptor must not
     * turn that page into an input, response buffer, or indirect table. */
    uint64_t output = GPUD_GPU_OUTPUT_OFFSET;
    uint64_t output_end = output + GPUD_GPU_CHANNEL_PAGE;
    uint64_t aux = GPUD_GPU_AUX_OFFSET;
    uint64_t aux_end = aux + GPUD_GPU_AUX_CAPACITY;
    for (size_t i = 0; i < queue->request.count; ++i) {
        kb2_vq_segment_t *segment = &queue->request.segments[i];
        if (overlaps(segment->address, segment->length, output, output_end) ||
            overlaps(segment->address, segment->length, aux, aux_end))
            return channel_fault(queue);
    }
    if (overlaps(queue->request.indirect, queue->request.indirect_length,
                 output, output_end) ||
        overlaps(queue->request.indirect, queue->request.indirect_length,
                 aux, aux_end))
        return channel_fault(queue);
    if (kb2_vq_copy_request(
            lane, generation, &queue->request, 0, query->request, queue->request.readable))
        return channel_fault(queue);
    uint32_t queue_class = queue->active_lane == GPUD_GPU_QUEUE_CONTROL ?
        KB2_GPU_QUEUE_CONTROL : queue->active_lane == GPUD_GPU_QUEUE_DISPLAY ?
        KB2_GPU_QUEUE_DISPLAY : KB2_GPU_QUEUE_EXECUTION;
    int result = ph_gpu_session_prepare(&queue->service, queue_class, queue->request.readable);
    if (result)
        return channel_fault(queue);
    if (query->plan.aux_input && query->aux_size) {
        if (!query->aux || query->aux_size > GPUD_GPU_AUX_CAPACITY)
            return channel_fault(queue);
        memcpy(query->aux,
               (unsigned char *)queue->mapping + GPUD_GPU_AUX_OFFSET,
               query->aux_size);
    }
    return 0;
}

static int prepare(void *context, const struct ph_ipc_packet *packet) {
    struct ph_gpu_queue *queue = context;
    if (packet->generation != queue->service.query.generation)
        return channel_fault(queue);
    if (packet->operation == PH_GPU_MAPPING_RELEASE) {
        if (!queue->mapping || queue->release_mapping_id || !packet->correlation ||
            packet->correlation <= queue->service.query.correlation ||
            !packet->value || packet->fd_count)
            return channel_fault(queue);
        queue->release_mapping_id = packet->value;
        queue->release_correlation = packet->correlation;
        queue->service.query.correlation = packet->correlation;
        return 0;
    }
    if (packet->correlation)
        return channel_fault(queue);
    if (packet->operation == PH_GPU_QUEUE_BIND) {
        if (queue->mapping || queue->channel.identity.generation || packet->fd_count != 1 ||
            packet->value != queue->channel_id)
            return channel_fault(queue);
        const struct pacha_ipc_fd *fd = &packet->fds[0];
        struct pacha_fd_info info = {0};
        if (fd->rights != PH_GPU_QUERY_RIGHTS || fd->flags != PACHA_FD_FLAG_CLOEXEC ||
            fd->transfer_flags ||
            pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, fd->fd, (uintptr_t)&info) ||
            info.kind != PACHA_FD_KIND_VMO || info.size != GPUD_GPU_CHANNEL_SIZE ||
            info.rights != PH_GPU_QUERY_RIGHTS || info.flags != PACHA_FD_FLAG_CLOEXEC)
            return -EACCES;
        long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP,
                                      fd->fd,
                                      0,
                                      GPUD_GPU_CHANNEL_SIZE,
                                      PACHA_PROT_READ | PACHA_PROT_WRITE,
                                      PACHA_MMAP_SHARED,
                                      0);
        if (address <= 0)
            return -ENOMEM;
        queue->mapping = (void *)(uintptr_t)address;
        int result = ph_gpu_channel_bind(&queue->channel,
                                         queue->mapping,
                                         queue->service.query.generation,
                                         queue->channel_id,
                                         KB2_VQ_DEVICE,
                                         &queue->atomics);
        if (result)
            return result;
        queue->bound = 1;
    } else if (packet->operation == PH_GPU_QUEUE_NOTIFY) {
        if (!queue->mapping || packet->fd_count ||
            (packet->value !=
                 queue->channel.queues[GPUD_GPU_QUEUE_EXECUTION].available_notification_id &&
             packet->value !=
                 queue->channel.queues[GPUD_GPU_QUEUE_DISPLAY].available_notification_id &&
             packet->value !=
                 queue->channel.queues[GPUD_GPU_QUEUE_CONTROL].available_notification_id))
            return channel_fault(queue);
    } else {
        return channel_fault(queue);
    }
    return next_request(queue);
}

static int dispatch(void *context, void *linux_service) {
    struct ph_gpu_queue *queue = context;
    if (queue->release_mapping_id) {
        if (!linux_service || !queue->service.unmap)
            return -ENODEV;
        return queue->service.unmap(linux_service, queue->release_mapping_id);
    }
    return ph_gpu_session_dispatch(&queue->service, linux_service);
}

static int complete(void *context, struct ph_ipc *ipc) {
    struct ph_gpu_queue *queue = context;
    if (queue->release_mapping_id) {
        struct ph_ipc_packet packet = {
            .operation = PH_GPU_MAPPING_RELEASED,
            .generation = queue->service.query.generation,
            .correlation = queue->release_correlation,
            .value = queue->release_mapping_id,
        };
        return ph_ipc_send(ipc, &packet);
    }
    struct ph_gpu_query *query = &queue->service.query;
    uint64_t generation = query->generation;
    kb2_vq_t *lane = &queue->channel.lanes[queue->active_lane];
    int result = ph_gpu_query_publish_attachment(query, ipc);
    if (result)
        return result;
    if (queue->active_lane != GPUD_GPU_QUEUE_CONTROL)
        memcpy((unsigned char *)queue->mapping + GPUD_GPU_OUTPUT_OFFSET,
               query->output,
               sizeof(query->output));
    if (query->aux_size && query->plan.aux_output) {
        if (!query->aux || query->aux_size > GPUD_GPU_AUX_CAPACITY)
            return channel_fault(queue);
        memcpy((unsigned char *)queue->mapping + GPUD_GPU_AUX_OFFSET,
               query->aux,
               query->aux_size);
    }
    int notify;
    if (kb2_vq_write_response(lane, generation, &queue->request, query->reply, query->reply_size) ||
        kb2_vq_complete(lane, generation, &queue->request, &notify))
        return channel_fault(queue);
    if (notify) {
        struct ph_ipc_packet packet = {.operation = PH_GPU_QUEUE_NOTIFY,
                                       .generation = generation,
                                       .value = lane->queue.used_notification_id};
        result = ph_ipc_send(ipc, &packet);
        if (result)
            return result;
    }
    return query->terminal_after_completion;
}

static int release_request(void *context) {
    /* Chains end at used publication. A failed chain remains owned until
     * terminal channel retirement; never drop it and admit more work. */
    struct ph_gpu_queue *queue = context;
    if (queue->release_mapping_id) {
        queue->release_mapping_id = 0;
        queue->release_correlation = 0;
        return 0;
    }
    int session_result = ph_gpu_session_service_release(&queue->service);
    int aux_result = ph_gpu_query_release_aux(&queue->service.query);
    return session_result ? session_result : aux_result;
}

int ph_gpu_queue_init(struct ph_gpu_queue *queue,
                      struct gpud_gpu_sessions *sessions,
                      uint64_t client_id,
                      uint64_t channel_id,
                      const kb2_vq_atomic_ops_t *atomics,
                      struct ph_lifecycle_service *service) {
    if (!queue || queue->channel_id || !channel_id || !service || !atomics ||
        !atomics->load_acquire || !atomics->store_release || !atomics->fence)
        return -EINVAL;
    int result = ph_gpu_session_service_init(&queue->service, sessions, client_id);
    if (result)
        return result;
    queue->channel_id = channel_id;
    queue->next_lane = GPUD_GPU_QUEUE_CONTROL;
    queue->atomics = *atomics;
    queue->request = (kb2_vq_chain_t){.segments = queue->segments, .capacity = GPUD_GPU_QUEUE_SIZE};
    *service = (struct ph_lifecycle_service){.context = queue,
                                             .prepare = prepare,
                                             .dispatch = dispatch,
                                             .complete = complete,
                                             .release = release_request,
                                             .next = next_request,
                                             .stop = stop};
    return 0;
}
