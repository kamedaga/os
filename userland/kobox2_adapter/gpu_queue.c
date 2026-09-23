/* SPDX-License-Identifier: MIT */
#include "gpu_queue.h"

#include <errno.h>
#include <string.h>

static int notify_drm_event(void *context, uint32_t slot, uint64_t cookie) {
    struct ph_gpu_queue *queue = context;
    if (!queue || slot >= GPUD_GPU_NATIVE_SESSION_LIMIT || !cookie)
        return -EINVAL;
    if (!atomic_load_explicit(&queue->event_admitted, memory_order_acquire))
        return 0;
    atomic_store_explicit(&queue->event_cookies[slot], cookie, memory_order_release);
    const uint64_t bit = UINT64_C(1) << slot;
    const uint64_t previous = atomic_fetch_or_explicit(
        &queue->event_pending, bit, memory_order_acq_rel);
    if (previous)
        return 0;
    const uint64_t one = 1;
    const long written = pacha_syscall3(PACHA_FD_SYSCALL_WRITE,
        queue->event_fd, (uintptr_t)&one, sizeof(one));
    if (written == (long)sizeof(one))
        return 0;
    atomic_store_explicit(&queue->event_error, -EIO, memory_order_release);
    return -EIO;
}

static int drain_event_doorbell(struct ph_gpu_queue *queue) {
    uint64_t count;
    const long read = pacha_syscall3(PACHA_FD_SYSCALL_READ,
        queue->event_fd, (uintptr_t)&count, sizeof(count));
    if (read == (long)sizeof(count) || read == PACHA_SYSCALL_ERR_NOT_READY)
        return 0;
    return -EIO;
}

static int notify_fence_event(void *context) {
    struct ph_gpu_queue *queue = context;
    if (!atomic_load_explicit(&queue->event_admitted, memory_order_acquire))
        return 0;
    if (atomic_exchange_explicit(&queue->fence_pending, 1, memory_order_acq_rel))
        return 0;
    const uint64_t one = 1;
    if (pacha_syscall3(PACHA_FD_SYSCALL_WRITE, queue->event_fd,
            (uintptr_t)&one, sizeof(one)) == (long)sizeof(one))
        return 0;
    atomic_store_explicit(&queue->event_error, -EIO, memory_order_release);
    return -EIO;
}

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
    atomic_store_explicit(&queue->event_admitted, 0, memory_order_release);
    int result = ph_gpu_query_destroy_aux(&queue->service.query);
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

static int next_event(struct ph_gpu_queue *queue) {
    int error = atomic_load_explicit(&queue->event_error, memory_order_acquire);
    if (error)
        return error;
    error = drain_event_doorbell(queue);
    if (error)
        return error;
    if (atomic_exchange_explicit(&queue->fence_pending, 0, memory_order_acq_rel)) {
        kb2_vq_t *lane = &queue->channel.lanes[GPUD_GPU_QUEUE_EVENT];
        kb2_vq_status_t status = kb2_vq_take_available(
            lane, queue->service.query.generation, &queue->request);
        if (status == KB2_VQ_EMPTY) {
            int ready;
            if (kb2_vq_arm(lane, queue->service.query.generation, &ready))
                return channel_fault(queue);
            if (ready)
                status = kb2_vq_take_available(
                    lane, queue->service.query.generation, &queue->request);
        }
        if (status == KB2_VQ_EMPTY) {
            atomic_store_explicit(&queue->fence_pending, 1, memory_order_release);
            return PH_LIFECYCLE_SERVICE_IDLE;
        }
        if (status || queue->request.readable ||
            queue->request.writable < PH_GPU_DRM_EVENT_MESSAGE_BYTES)
            return channel_fault(queue);
        queue->active_lane = GPUD_GPU_QUEUE_EVENT;
        queue->event_session = 0;
        queue->event_fences = 1;
        queue->event_active = 1;
        return 0;
    }
    for (;;) {
        uint64_t pending = atomic_load_explicit(
            &queue->event_pending, memory_order_acquire);
        if (!pending)
            return PH_LIFECYCLE_SERVICE_IDLE;
        const uint32_t slot = (uint32_t)__builtin_ctzll(pending);
        const uint64_t bit = UINT64_C(1) << slot;
        if (!(atomic_fetch_and_explicit(&queue->event_pending, ~bit,
                memory_order_acq_rel) & bit))
            continue;
        const uint64_t cookie = atomic_load_explicit(
            &queue->event_cookies[slot], memory_order_acquire);
        uint64_t session = 0;
        uint32_t session_status = ph_gpu_session_event_acquire(
            queue->service.sessions, queue->service.query.generation,
            queue->service.client_id, cookie, &session);
        if (session_status == KB2_GPU_STATUS_NOT_FOUND)
            continue;
        if (session_status)
            return channel_fault(queue);
        kb2_vq_t *lane = &queue->channel.lanes[GPUD_GPU_QUEUE_EVENT];
        kb2_vq_status_t status = kb2_vq_take_available(
            lane, queue->service.query.generation, &queue->request);
        if (status == KB2_VQ_EMPTY) {
            int ready;
            if (kb2_vq_arm(lane, queue->service.query.generation, &ready)) {
                (void)ph_gpu_session_release(queue->service.sessions,
                    queue->service.query.generation, queue->service.client_id,
                    session);
                return channel_fault(queue);
            }
            if (ready)
                status = kb2_vq_take_available(
                    lane, queue->service.query.generation, &queue->request);
        }
        if (status == KB2_VQ_EMPTY) {
            (void)ph_gpu_session_release(queue->service.sessions,
                queue->service.query.generation, queue->service.client_id,
                session);
            error = notify_drm_event(queue, slot, cookie);
            return error ? error : PH_LIFECYCLE_SERVICE_IDLE;
        }
        if (status || queue->request.readable ||
            queue->request.writable < PH_GPU_DRM_EVENT_MESSAGE_BYTES) {
            (void)ph_gpu_session_release(queue->service.sessions,
                queue->service.query.generation, queue->service.client_id,
                session);
            return channel_fault(queue);
        }
        queue->active_lane = GPUD_GPU_QUEUE_EVENT;
        queue->event_cookie = cookie;
        queue->event_session = session;
        queue->event_active = 1;
        return 0;
    }
}

static int next_request(void *context) {
    struct ph_gpu_queue *queue = context;
    if (!queue->mapping || queue->release_mapping_id)
        return PH_LIFECYCLE_SERVICE_IDLE;
    struct ph_gpu_query *query = &queue->service.query;
    if (query->terminal_after_completion)
        return query->terminal_after_completion;
    uint64_t generation = query->generation;
    int event = next_event(queue);
    if (event != PH_LIFECYCLE_SERVICE_IDLE)
        return event;
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
                 queue->channel.queues[GPUD_GPU_QUEUE_CONTROL].available_notification_id &&
             packet->value !=
                 queue->channel.queues[GPUD_GPU_QUEUE_EVENT].available_notification_id))
            return channel_fault(queue);
    } else {
        return channel_fault(queue);
    }
    return next_request(queue);
}

static int dispatch(void *context, void *linux_service) {
    struct ph_gpu_queue *queue = context;
    if (queue->event_active) {
        if (!linux_service)
            return -ENODEV;
        if (queue->event_fences) {
            queue->event_size = 0;
            while (sizeof(queue->event_data) - queue->event_size >= PH_GPU_FENCE_RECORD_BYTES) {
                struct kobox_drm_fence_result done;
                int result = queue->service.take_fence(linux_service, &done);
                if (result <= 0)
                    return result;
                if (result != 1 || !done.session || !done.correlation ||
                    !done.status || done.status > 1)
                    return -EPROTO;
                unsigned char *record = queue->event_data + queue->event_size;
                ph_gpu_event_store_u64(record, done.session);
                ph_gpu_event_store_u64(record + 8, done.correlation);
                ph_gpu_event_store_u32(record + 16, (uint32_t)done.status);
                ph_gpu_event_store_u32(record + 20, 0);
                queue->event_size += PH_GPU_FENCE_RECORD_BYTES;
            }
            /* A full batch may leave more completions. The next owner turn
             * drains them; callbacks never allocate or send event payloads. */
            atomic_store_explicit(&queue->fence_pending, 1, memory_order_release);
            return 0;
        }
        struct kobox_linux_drm_file *file = NULL;
        int result = queue->service.file(linux_service,
            queue->event_cookie, &file);
        if (!result)
            result = queue->service.query.api.read_events(file,
                queue->event_data, sizeof(queue->event_data),
                &queue->event_size);
        return result;
    }
    if (queue->release_mapping_id) {
        if (!linux_service || !queue->service.unmap)
            return -ENODEV;
        return queue->service.unmap(linux_service, queue->release_mapping_id);
    }
    return ph_gpu_session_dispatch(&queue->service, linux_service);
}

static int complete(void *context, struct ph_ipc *ipc) {
    struct ph_gpu_queue *queue = context;
    if (queue->event_active) {
        if (queue->event_sequence == UINT64_MAX ||
            ph_gpu_event_encode(queue->event_message,
                sizeof(queue->event_message), &queue->event_message_size,
                queue->service.query.generation, queue->event_session,
                ++queue->event_sequence, queue->event_data,
                queue->event_size, queue->event_fences))
            return channel_fault(queue);
        kb2_vq_t *lane = &queue->channel.lanes[GPUD_GPU_QUEUE_EVENT];
        int notify;
        if (kb2_vq_write_response(lane, queue->service.query.generation,
                &queue->request, queue->event_message,
                queue->event_message_size) ||
            kb2_vq_complete(lane, queue->service.query.generation,
                &queue->request, &notify))
            return channel_fault(queue);
        if (notify) {
            struct ph_ipc_packet packet = {
                .operation = PH_GPU_QUEUE_NOTIFY,
                .generation = queue->service.query.generation,
                .value = lane->queue.used_notification_id,
            };
            return ph_ipc_send(ipc, &packet);
        }
        return 0;
    }
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
    if (queue->event_active) {
        int result = queue->event_fences ? 0 : ph_gpu_session_release(queue->service.sessions,
            queue->service.query.generation, queue->service.client_id,
            queue->event_session);
        queue->event_cookie = 0;
        queue->event_session = 0;
        queue->event_size = 0;
        queue->event_message_size = 0;
        queue->event_active = 0;
        queue->event_fences = 0;
        return result ? -EPROTO : 0;
    }
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
    const uint64_t event_rights = PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WRITE |
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE;
    long event_fd = pacha_syscall3(
        PACHA_FD_SYSCALL_EVENTFD_CREATE, 0, event_rights, 0);
    if (event_fd < 16 || event_fd >= PACHA_FD_TABLE_LIMIT)
        return -ENOMEM;
    queue->channel_id = channel_id;
    queue->event_fd = (int)event_fd;
    queue->next_lane = GPUD_GPU_QUEUE_CONTROL;
    queue->atomics = *atomics;
    atomic_init(&queue->event_admitted, 1);
    atomic_init(&queue->fence_pending, 0);
    atomic_init(&queue->event_pending, 0);
    atomic_init(&queue->event_error, 0);
    for (size_t i = 0; i < GPUD_GPU_NATIVE_SESSION_LIMIT; ++i)
        atomic_init(&queue->event_cookies[i], 0);
    if (!atomic_is_lock_free(&queue->event_admitted) ||
        !atomic_is_lock_free(&queue->fence_pending) ||
        !atomic_is_lock_free(&queue->event_pending) ||
        !atomic_is_lock_free(&queue->event_error)) {
        (void)pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, queue->event_fd);
        queue->event_fd = 0;
        return -EOPNOTSUPP;
    }
    queue->event_host = (struct kobox_linux_drm_event_host) {
        .size = sizeof(queue->event_host),
        .context = queue,
        .notify = notify_drm_event,
        .notify_fences = notify_fence_event,
    };
    queue->request = (kb2_vq_chain_t){.segments = queue->segments, .capacity = GPUD_GPU_QUEUE_SIZE};
    *service = (struct ph_lifecycle_service){.context = queue,
                                             .prepare = prepare,
                                             .dispatch = dispatch,
                                             .complete = complete,
                                             .release = release_request,
                                             .next = next_request,
                                             .stop = stop,
                                             .wake_fd = queue->event_fd};
    return 0;
}

int ph_gpu_queue_finish(struct ph_gpu_queue *queue) {
    if (!queue || queue->event_fd < 16 || queue->mapping ||
        (queue->bound && atomic_load_explicit(
            &queue->event_admitted, memory_order_acquire)))
        return -EINVAL;
    const long result = pacha_syscall1(
        PACHA_FD_SYSCALL_CLOSE, queue->event_fd);
    if (result)
        return -EIO;
    queue->event_fd = 0;
    return 0;
}
