/* SPDX-License-Identifier: MIT */
#include "fs_queue.h"
#include "boot/module_launch.h"

#include <errno.h>
#include <string.h>

struct ph_fs_queue_request {
    struct ph_fs_queue_request *next;
    struct ph_fs_queue_lane *lane;
    struct ph_fs_queue *queue;
    struct ph_fs_client_queue *client;
    kb2_vq_chain_t chain;
    kb2_vq_segment_t segments[2];
    size_t extra_bytes;
    unsigned lane_index;
    int control;
    struct ph_ipc_notice notice;
    int published; /* Written before the Linux pool publishes collectability. */
#ifdef PH_FS_STORAGE_PROFILE
    uint64_t accepted;
    int profile_index;
#endif
};

/* A notification can schedule another fiber on this same logical CPU. Defer
 * that entry while holding shared ring metadata, otherwise it could wait for
 * the interrupted holder. Neither Linux/VFS calls nor IPC run under this lock. */
static uint64_t lane_lock(struct ph_fs_queue_lane *lane) {
    uint64_t mask = 0;
    if (lane->direct) { PH_OK(ph_notifications_save(&mask)); ph_lock(&lane->ring_lock); }
    return mask;
}
static void lane_unlock(struct ph_fs_queue_lane *lane, uint64_t mask) {
    if (lane->direct) { ph_unlock(&lane->ring_lock); PH_OK(ph_notifications_restore(mask)); }
}

static void lane_fault(struct ph_fs_queue_lane *lane) {
    uint64_t mask = lane_lock(lane);
    atomic_store(&lane->retiring, 1);
    if (lane->bound) kb2_vq_channel_fault(&lane->channel.channel);
    lane_unlock(lane, mask);
}

static void watch_lock(struct ph_fs_queue *queue) {
    while (atomic_flag_test_and_set_explicit(&queue->watch_lock, memory_order_acquire))
        __asm__ volatile("pause" ::: "memory");
}
static void watch_unlock(struct ph_fs_queue *queue) {
    atomic_flag_clear_explicit(&queue->watch_lock, memory_order_release);
}

static int wake(void *context) {
    struct ph_fs_queue *queue = context;
    if (!queue->wake_tx.admitted) return 0;
    struct ph_ipc_packet packet = {.operation = 1, .generation = queue->config[0].generation};
    int result = ph_ipc_send(&queue->wake_tx, &packet);
    return result == -EAGAIN ? 0 : result; /* A queued hint already interrupts WAIT. */
}

#ifdef PH_FS_STORAGE_PROFILE
static uint64_t profile_clock(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
static void profile_dump(struct ph_fs_queue *queue) {
    const unsigned opcodes[] = {KB2_FILESYSTEM_OP_STATX,
        KB2_FILESYSTEM_OP_PREAD, KB2_FILESYSTEM_OP_PWRITE};
    for (unsigned row = 0; row < 3; ++row) {
        /* count, admission, submit-to-publication residence, publication,
         * notify. Admission overlaps residence: the timestamp must precede
         * submit because a worker can publish before submit returns. */
        char line[128] = "[fs-queue-profile]";
        size_t at = sizeof("[fs-queue-profile]") - 1;
        for (unsigned column = 0; column < 6; ++column) {
            uint64_t value = column ? queue->profile_rows[row][column - 1] : opcodes[row];
            line[at++] = ' ';
            for (int shift = 60; shift >= 0; shift -= 4)
                line[at++] = "0123456789abcdef"[(value >> shift) & 15];
        }
        line[at++] = '\n';
        (void)pacha_syscall2(1, (uintptr_t)line, at);
    }
}
#endif

static int fault(struct ph_fs_queue *queue) {
    for (unsigned i = 0; i < 3; ++i)
        if (queue->lanes[i].bound) kb2_vq_channel_fault(&queue->lanes[i].channel.channel);
    return -EPROTO;
}

static void client_fault(struct ph_fs_queue *queue, struct ph_fs_client_queue *client) {
    lane_fault(&client->lane);
    watch_lock(queue);
    int changed = client->watch_fd != 0;
    client->watch_fd = 0;
    if (changed) ++queue->watch_epoch;
    watch_unlock(queue);
    /* Interrupt a snapshot containing this FD before closing it. The helper
     * handles stale wait results by epoch, never retains the client pointer. */
    if (changed) PH_OK(wake(queue));
    /* An accepted ticket pins this FD even after used is visible. A sender
     * can still be returning from SEND when a peer requests retirement. */
    if (!client->lane.inflight && client->ipc.generation)
        (void)ph_ipc_destroy(&client->ipc, client->config.generation);
}

static int lane_allocate(struct ph_fs_queue *queue, struct ph_fs_queue_lane *lane,
    const struct ph_fs_channel_config *config) {
    if (lane->slots) return -EBUSY;
    atomic_init(&lane->ring_lock, 0);
    size_t count = config->queue_size, requests = count / 2;
    size_t bytes = count * (sizeof(*lane->slots) + sizeof(*lane->owners) +
        sizeof(*lane->segments)) + requests * sizeof(*lane->requests);
    if (bytes > queue->inventory_budget - queue->inventory_bytes) return -ENOMEM;
    lane->slots = ph_alloc(count * sizeof(*lane->slots));
    lane->owners = ph_alloc(count * sizeof(*lane->owners));
    lane->segments = ph_alloc(count * sizeof(*lane->segments));
    lane->requests = ph_alloc(requests * sizeof(*lane->requests));
    memset(lane->requests, 0, requests * sizeof(*lane->requests));
    lane->request_count = requests;
    for (size_t i = 0; i < requests; ++i) {
        lane->requests[i].next = lane->free_requests;
        lane->free_requests = &lane->requests[i];
    }
    lane->probe = (kb2_vq_chain_t){.segments = lane->segments, .capacity = count};
    lane->inventory_bytes = bytes;
    queue->inventory_bytes += bytes;
    return 0;
}

static int lane_retire(struct ph_fs_queue *queue, struct ph_fs_queue_lane *lane,
    const struct ph_fs_channel_config *config) {
    if (lane->inflight) return -EBUSY;
    if (lane->bound) kb2_vq_channel_fault(&lane->channel.channel);
    if (lane->mapping) {
        if (pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP,
            (uintptr_t)lane->mapping, lane->mapping_bytes)) return -EIO;
        lane->mapping = NULL;
    }
    size_t count = config->queue_size;
    if (lane->slots) ph_free(lane->slots, count * sizeof(*lane->slots));
    if (lane->owners) ph_free(lane->owners, count * sizeof(*lane->owners));
    if (lane->segments) ph_free(lane->segments, count * sizeof(*lane->segments));
    if (lane->requests) ph_free(lane->requests, lane->request_count * sizeof(*lane->requests));
    queue->inventory_bytes -= lane->inventory_bytes;
    memset(lane, 0, sizeof(*lane));
    return 0;
}

static int client_retire(struct ph_fs_queue *queue, struct ph_fs_client_queue *client) {
    if (client->lane.inflight) return -EBUSY;
    client_fault(queue, client);
    if (client->ipc.generation && (client->ipc.fd || client->ipc.rejected.fd_count)) {
        int error = ph_ipc_destroy(&client->ipc, client->config.generation);
        if (error) return error;
    }
    return lane_retire(queue, &client->lane, &client->config);
}

static int reply_management(struct ph_fs_queue *queue, struct ph_ipc *ipc,
    uint64_t operation, uint64_t id) {
    struct ph_ipc_packet packet = {.operation = operation,
        .generation = queue->config[0].generation, .correlation = id};
    return ph_ipc_send(ipc, &packet);
}

static int client_prepare(struct ph_fs_queue *queue, struct ph_ipc_packet *packet) {
    if (queue->reply_operation || !packet->correlation) return fault(queue);
    if (packet->operation == PH_FS_CLIENT_PREPARE) {
        if (packet->fd_count || !packet->value || packet->correlation <= queue->client_watermark ||
            packet->correlation <= queue->config[0].channel_id ||
            packet->correlation <= queue->config[1].channel_id) return fault(queue);
        if (sizeof(struct ph_fs_client_queue) > queue->inventory_budget - queue->inventory_bytes)
            return -ENOMEM;
        struct ph_fs_client_queue *client = ph_alloc(sizeof(*client));
        memset(client, 0, sizeof(*client));
        client->config = queue->config[0];
        client->config.channel_id = packet->correlation;
        client->client = packet->value;
        /* Monotonic append also makes the helper's numeric wait cursor safe
         * across unlink/free, without retaining mutable client pointers. */
        watch_lock(queue);
        struct ph_fs_client_queue **tail = &queue->clients;
        while (*tail) tail = &(*tail)->next;
        *tail = client;
        ++queue->watch_epoch;
        watch_unlock(queue);
        queue->inventory_bytes += sizeof(*client);
        queue->client_watermark = packet->correlation;
        PH_OK(wake(queue));
        return PH_LIFECYCLE_SERVICE_IDLE;
    }
    struct ph_fs_client_queue *client = queue->clients;
    while (client && client->config.channel_id != packet->correlation) client = client->next;
    if (!client) return fault(queue);
    struct ph_fs_queue_lane *lane = &client->lane;
    if (packet->operation == PH_FS_CLIENT_RELEASE) {
        if (packet->fd_count || packet->value || client->release_requested) return fault(queue);
        client->release_requested = 1;
        client_fault(queue, client);
        /* Accepted jobs keep their original authority and private output.
         * RELEASED is deferred until their leases are collected/released. */
        return PH_LIFECYCLE_SERVICE_IDLE;
    }
    if (packet->operation != PH_FS_CLIENT_BIND || lane->mapping || lane->bound ||
        lane->retiring || packet->fd_count != 2 || !packet->value) return fault(queue);
    client->config.arena_bytes = packet->value;
    size_t offset;
    int error = ph_fs_channel_layout(&client->config, &lane->mapping_bytes, &offset);
    if (error) return error;
    struct pacha_fd_info info = {0};
    const struct pacha_ipc_fd *vmo = &packet->fds[0], *doorbell = &packet->fds[1];
    if (vmo->rights != PH_FS_QUEUE_RIGHTS || vmo->flags != PACHA_FD_FLAG_CLOEXEC ||
        vmo->transfer_flags || doorbell->rights != PH_FS_CLIENT_DOORBELL_RIGHTS ||
        doorbell->flags != PACHA_FD_FLAG_CLOEXEC || doorbell->transfer_flags ||
        pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, vmo->fd, (uintptr_t)&info) ||
        info.kind != PACHA_FD_KIND_VMO || info.size != lane->mapping_bytes ||
        info.rights != PH_FS_QUEUE_RIGHTS || info.flags != PACHA_FD_FLAG_CLOEXEC) return -EACCES;
    if (pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, doorbell->fd, (uintptr_t)&info) ||
        info.kind != PACHA_FD_KIND_CHANNEL || info.rights != PH_FS_CLIENT_DOORBELL_RIGHTS ||
        info.flags != PACHA_FD_FLAG_CLOEXEC) return -EACCES;
    error = lane_allocate(queue, lane, &client->config);
    if (error) return error;
    lane->direct = 1;
    long mapped = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, vmo->fd, 0, lane->mapping_bytes,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (mapped < (long)PH_PAGE_SIZE) return -ENOMEM;
    lane->mapping = (void *)(uintptr_t)mapped;
    error = ph_fs_channel_bind(&lane->channel, lane->mapping, lane->mapping_bytes,
        &client->config, KB2_VQ_DEVICE, &queue->atomics, lane->slots, lane->owners);
    if (error) return error;
    error = ph_ipc_init(&client->ipc, (int)doorbell->fd, client->config.generation);
    if (error) return error;
    packet->fds[1].fd = PH_IPC_NO_FD;
    lane->bound = 1;
    watch_lock(queue);
    client->watch_fd = client->ipc.fd;
    ++queue->watch_epoch;
    watch_unlock(queue);
    PH_OK(wake(queue));
    queue->reply_operation = PH_FS_CLIENT_BOUND;
    queue->reply_id = packet->correlation;
    return 0;
}

static int prepare(void *context, struct ph_ipc_packet *packet) {
    struct ph_fs_queue *queue = context;
    if (queue->closing || packet->generation != queue->config[0].generation) return fault(queue);
    if (packet->operation == PH_FS_CLIENT_PREPARE || packet->operation == PH_FS_CLIENT_BIND ||
        packet->operation == PH_FS_CLIENT_RELEASE) return client_prepare(queue, packet);
    if (packet->operation == PH_FS_EXCHANGE_PREPARE || packet->operation == PH_FS_EXCHANGE_OWNER_PREPARE) {
        int owner = packet->operation == PH_FS_EXCHANGE_OWNER_PREPARE;
        if (queue->lanes[2].mapping || queue->config[2].channel_id ||
            packet->correlation <= queue->exchange_watermark ||
            packet->correlation <= queue->config[0].channel_id ||
            packet->correlation <= queue->config[1].channel_id ||
            (!owner && !packet->value) || packet->fd_count) return fault(queue);
        queue->config[2] = queue->config[0];
        queue->config[2].channel_id = packet->correlation;
        queue->exchange_watermark = packet->correlation;
        queue->exchange_client = packet->value;
        queue->exchange_owner = owner;
        queue->exchange_consumed = queue->exchange_release_requested = 0;
        return PH_LIFECYCLE_SERVICE_IDLE;
    }
    if (packet->operation == PH_FS_EXCHANGE_RELEASE) {
        struct ph_fs_queue_lane *lane = &queue->lanes[2];
        if (!lane->bound || queue->exchange_release_requested ||
            packet->correlation != queue->config[2].channel_id || packet->fd_count || packet->value)
            return fault(queue);
        queue->exchange_release_requested = lane->retiring = 1;
        kb2_vq_channel_fault(&lane->channel.channel);
        return PH_LIFECYCLE_SERVICE_IDLE;
    }
    unsigned i;
    if (packet->operation == PH_FS_QUEUE_BIND || packet->operation == PH_FS_QUEUE_NOTIFY) i = 0;
    else if (packet->operation == PH_FS_OWNER_BIND || packet->operation == PH_FS_OWNER_NOTIFY) i = 1;
    else if (packet->operation == PH_FS_EXCHANGE_BIND || packet->operation == PH_FS_EXCHANGE_NOTIFY) i = 2;
    else return fault(queue);
    if ((i != 2 && packet->correlation) || (i == 2 && (!queue->config[2].channel_id ||
        packet->correlation != queue->config[2].channel_id))) return fault(queue);
    struct ph_fs_queue_lane *lane = &queue->lanes[i];
    int bind = packet->operation == PH_FS_QUEUE_BIND || packet->operation == PH_FS_OWNER_BIND ||
        packet->operation == PH_FS_EXCHANGE_BIND;
    if (bind) {
        if (lane->mapping || lane->slots) return fault(queue);
        if (i == 2) queue->config[2].arena_bytes = packet->value;
        size_t offset;
        int error = ph_fs_channel_layout(&queue->config[i], &lane->mapping_bytes, &offset);
        if (error) return error;
        struct pacha_fd_info info = {0};
        if (packet->fd_count != 1 || (i != 2 && packet->value != queue->config[i].channel_id) ||
            packet->fds[0].rights != PH_FS_QUEUE_RIGHTS || packet->fds[0].flags != PACHA_FD_FLAG_CLOEXEC ||
            packet->fds[0].transfer_flags || pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO,
                packet->fds[0].fd, (uintptr_t)&info) || info.kind != PACHA_FD_KIND_VMO ||
            info.size != lane->mapping_bytes || info.rights != PH_FS_QUEUE_RIGHTS ||
            info.flags != PACHA_FD_FLAG_CLOEXEC) return -EACCES;
        error = lane_allocate(queue, lane, &queue->config[i]);
        if (error) return error;
        lane->direct = i == 2;
        long mapped = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, packet->fds[0].fd, 0,
            lane->mapping_bytes, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
        if (mapped < (long)PH_PAGE_SIZE) return -ENOMEM;
        lane->mapping = (void *)(uintptr_t)mapped;
        error = ph_fs_channel_bind(&lane->channel, lane->mapping, lane->mapping_bytes,
            &queue->config[i], KB2_VQ_DEVICE, &queue->atomics, lane->slots, lane->owners);
        if (error) return error;
        lane->bound = 1;
    } else if (!lane->bound || lane->retiring || packet->fd_count ||
        packet->value != lane->channel.queue.available_notification_id) return fault(queue);
    /* A hint never admits work. The opening task's next bounded quantum
     * snapshots chains independently of notification counts. */
    return PH_LIFECYCLE_SERVICE_IDLE;
}

static int complete(void *context, struct ph_ipc *ipc) {
    struct ph_fs_queue *queue = context;
    if (!queue->reply_operation) return -EPROTO;
    return reply_management(queue, ipc, queue->reply_operation, queue->reply_id);
}
static int release(void *context) {
    struct ph_fs_queue *queue = context;
    queue->reply_operation = queue->reply_id = 0;
    return 0;
}

static const struct ph_fs_channel_config *request_config(struct ph_fs_queue *queue,
    struct ph_fs_queue_request *request) {
    return request->client ? &request->client->config : &queue->config[request->lane_index];
}

static void request_release(struct ph_fs_queue *queue, struct ph_fs_queue_request *request) {
    struct ph_fs_queue_lane *lane = request->lane;
    struct ph_fs_client_queue *client = request->client;
    if (request->extra_bytes) {
        ph_free(request->chain.segments, request->extra_bytes);
        queue->inventory_bytes -= request->extra_bytes;
    }
    if (queue->control_active == request) {
        queue->control_active = NULL;
        queue->barrier_client = queue->barrier_target = 0;
    }
    --lane->inflight;
    --queue->inflight;
    memset(request, 0, sizeof(*request));
    request->next = lane->free_requests;
    lane->free_requests = request;
    if (client && lane->retiring && !lane->inflight) client_fault(queue, client);
}

static int publish(struct ph_fs_queue *queue, struct ph_fs_queue_request *request,
    struct ph_ipc *ipc, const void *response, size_t used) {
    struct ph_fs_queue_lane *lane = request->lane;
    const struct ph_fs_channel_config *config = request_config(queue, request);
    int notify;
    uint64_t mask = lane_lock(lane);
    if (lane->retiring) { lane_unlock(lane, mask); return 0; }
#ifdef PH_FS_STORAGE_PROFILE
    uint64_t before = profile_clock();
#endif
    int error = used < KB2_FILESYSTEM_RESPONSE_SIZE || used > request->chain.writable ||
        kb2_vq_write_response(&lane->channel.lane, config->generation, &request->chain, response, used) ||
        kb2_vq_complete(&lane->channel.lane, config->generation, &request->chain, &notify);
    lane_unlock(lane, mask);
    if (error) {
        if (request->client) { lane_fault(lane); return 0; }
        return fault(queue);
    }
#ifdef PH_FS_STORAGE_PROFILE
    uint64_t after = profile_clock();
    if (request->profile_index >= 0) {
        atomic_ullong *row = queue->profile_rows[request->profile_index];
        atomic_fetch_add(&row[0], 1);
        atomic_fetch_add(&row[2], before - request->accepted);
        atomic_fetch_add(&row[3], after - before);
    }
#endif
    if (!notify) return 0;
    struct ph_ipc_packet packet = {.operation = request->client ? PH_FS_QUEUE_NOTIFY :
        request->lane_index == 2 ? PH_FS_EXCHANGE_NOTIFY :
        request->lane_index ? PH_FS_OWNER_NOTIFY : PH_FS_QUEUE_NOTIFY,
        .generation = config->generation, .value = lane->channel.queue.used_notification_id,
        .correlation = request->client || request->lane_index == 2 ? config->channel_id : 0};
    error = request->client ? ph_ipc_notify(&request->notice) : ph_ipc_send(ipc, &packet);
#ifdef PH_FS_STORAGE_PROFILE
    if (request->profile_index >= 0)
        atomic_fetch_add(&queue->profile_rows[request->profile_index][4], profile_clock() - after);
#endif
    if (error && request->client) { lane_fault(lane); return 0; }
    return error;
}

static void worker_publish(void *cookie, const void *response, size_t used, int error) {
    struct ph_fs_queue_request *request = cookie;
    /* Only authenticated direct lanes have a request-owned notification FD.
     * Legacy/native-capability management replies retain their owner path. */
    if (!request->client) return;
    if (error) lane_fault(request->lane);
    else (void)publish(request->queue, request, NULL, response, used);
    request->published = 1;
}

static int collect_one(struct ph_fs_queue *queue, struct ph_ipc *ipc) {
    struct kobox_linux_fs_work *work;
    struct ph_fs_queue_request *request;
    void *cookie;
    const void *response;
    size_t used;
    int execution;
    int result = queue->collect(queue->workers, &work, &cookie, &response, &used, &execution);
    if (result == -EAGAIN) return 0;
    if (result) return result;
    request = cookie;
    if (!request || !request->lane || !request->lane->inflight) return -EPROTO;
    if (request->published) {
        if (request->lane->retiring) client_fault(queue, request->client);
        result = 0;
    } else if (execution) {
        if (request->client) { client_fault(queue, request->client); result = 0; }
        else result = fault(queue);
    } else result = publish(queue, request, ipc, response, used);
    int freed = queue->release_work(queue->workers, work);
    request_release(queue, request);
    return result ? result : freed ? freed : PH_LIFECYCLE_SERVICE_PROGRESS;
}

static int retire_one(struct ph_fs_queue *queue, struct ph_ipc *ipc) {
    struct ph_fs_client_queue **cursor = &queue->clients;
    while (*cursor) {
        struct ph_fs_client_queue *client = *cursor;
        if (!client->release_requested || client->lane.inflight) { cursor = &client->next; continue; }
        int error = client_retire(queue, client);
        if (error) return error;
        error = reply_management(queue, ipc, PH_FS_CLIENT_RELEASED, client->config.channel_id);
        if (error) return error; /* Keep the inventory if ACK publication failed. */
        if (queue->client_cursor == client) queue->client_cursor = NULL;
        watch_lock(queue);
        *cursor = client->next;
        ++queue->watch_epoch;
        watch_unlock(queue);
        PH_OK(wake(queue));
        queue->inventory_bytes -= sizeof(*client);
        ph_free(client, sizeof(*client));
        return PH_LIFECYCLE_SERVICE_PROGRESS;
    }
    if (queue->exchange_release_requested && !queue->lanes[2].inflight) {
        uint64_t id = queue->config[2].channel_id;
        int error = lane_retire(queue, &queue->lanes[2], &queue->config[2]);
        if (error) return error;
        error = reply_management(queue, ipc, PH_FS_EXCHANGE_RELEASED, id);
        if (error) return error;
        memset(&queue->config[2], 0, sizeof(queue->config[2]));
        queue->exchange_release_requested = 0;
        return PH_LIFECYCLE_SERVICE_PROGRESS;
    }
    return 0;
}

static int accept_lane(struct ph_fs_queue *queue, struct ph_fs_queue_lane *lane,
    const struct ph_fs_channel_config *config, struct ph_fs_client_queue *client,
    unsigned index, struct ph_ipc *ipc) {
    int control = !client && (index == 1 || (index == 2 && queue->exchange_owner));
    if (!lane->bound || lane->retiring || !lane->free_requests ||
        (index == 2 && !client && queue->exchange_consumed) || (control && queue->control_active) ||
        (client && (client->client == queue->barrier_client || client->client == queue->barrier_target)))
        return 0;
    uint64_t mask = lane_lock(lane);
    kb2_vq_status_t status = kb2_vq_take_available(&lane->channel.lane, config->generation, &lane->probe);
    if (status == KB2_VQ_EMPTY) {
        int ready;
        status = kb2_vq_arm(&lane->channel.lane, config->generation, &ready);
        if (!status && ready) status = kb2_vq_take_available(&lane->channel.lane, config->generation, &lane->probe);
        else if (!status) { lane_unlock(lane, mask); return 0; }
    }
    lane_unlock(lane, mask);
    if (status == KB2_VQ_EMPTY) return 0;
    if (status || lane->probe.readable < KB2_FILESYSTEM_REQUEST_SIZE ||
        lane->probe.writable < KB2_FILESYSTEM_RESPONSE_SIZE || lane->probe.readable > config->arena_bytes ||
        lane->probe.writable > config->arena_bytes || (lane->direct &&
        (lane->probe.count != 2 || lane->probe.indirect_length || lane->probe.segments[0].writable ||
        !lane->probe.segments[1].writable))) {
        if (client) { client_fault(queue, client); return PH_LIFECYCLE_SERVICE_PROGRESS; }
        return fault(queue);
    }
    size_t input_size = (size_t)lane->probe.readable;
    size_t capacity = (size_t)lane->probe.writable;
    unsigned char header[KB2_FILESYSTEM_REQUEST_SIZE];
    void *input = NULL;
    size_t copied = 0;
    /* A trusted filed lane still routes by a private full snapshot. Direct
     * clients route exclusively by binding and Linux makes their sole copy. */
    if (!client) {
        copied = input_size;
        if (copied > queue->inventory_budget - queue->inventory_bytes) return -ENOMEM;
        input = ph_alloc(copied);
        queue->inventory_bytes += copied;
        if (kb2_vq_copy_request(&lane->channel.lane, config->generation, &lane->probe, 0, input, input_size)) {
            ph_free(input, copied); queue->inventory_bytes -= copied; return fault(queue);
        }
        memcpy(header, input, sizeof(header));
    } else {
        input = (unsigned char *)lane->mapping + lane->probe.segments[0].address;
        memcpy(header, input, sizeof(header));
    }
    kb2_fs_request_t decoded = {0};
    int error = kb2_fs_request_decode(header, sizeof(header), input_size, &decoded);
    uint64_t authenticated = client ? client->client : index == 2 && !control ? queue->exchange_client : decoded.client;
    if (!error && !control && ((queue->barrier_client && authenticated == queue->barrier_client) ||
        (queue->barrier_target && authenticated == queue->barrier_target))) {
        mask = lane_lock(lane);
        status = kb2_vq_return_available(&lane->channel.lane, config->generation, &lane->probe);
        lane_unlock(lane, mask);
        if (copied) { ph_free(input, copied); queue->inventory_bytes -= copied; }
        return status ? fault(queue) : 0;
    }
    struct ph_fs_queue_request *request = lane->free_requests;
    lane->free_requests = request->next;
    memset(request, 0, sizeof(*request));
    request->lane = lane; request->queue = queue;
    request->client = client; request->lane_index = index; request->control = control;
    if (client) request->notice = (struct ph_ipc_notice){.fd = client->ipc.fd,
        .operation = PH_FS_QUEUE_NOTIFY, .generation = config->generation,
        .correlation = config->channel_id, .value = lane->channel.queue.used_notification_id};
    request->chain = (kb2_vq_chain_t){.segments = request->segments, .capacity = 2};
    if (lane->probe.count > 2) {
        request->extra_bytes = lane->probe.count * sizeof(*request->chain.segments);
        if (request->extra_bytes > queue->inventory_budget - queue->inventory_bytes) {
            if (copied) { ph_free(input, copied); queue->inventory_bytes -= copied; }
            request->next = lane->free_requests; lane->free_requests = request;
            return -ENOMEM;
        }
        request->chain.segments = ph_alloc(request->extra_bytes);
        request->chain.capacity = lane->probe.count;
        queue->inventory_bytes += request->extra_bytes;
    }
    mask = lane_lock(lane);
    status = kb2_vq_move_accepted(&lane->channel.lane, config->generation, &lane->probe, &request->chain);
    lane_unlock(lane, mask);
    ++lane->inflight; ++queue->inflight;
    /* A previous worker may fault the lane after take but before move. No
     * new request was executed; retain the faulted probe until lane teardown. */
    if (status) {
        if (copied) { ph_free(input, copied); queue->inventory_bytes -= copied; }
        if (client) client_fault(queue, client);
        request_release(queue, request);
        return client ? PH_LIFECYCLE_SERVICE_PROGRESS : fault(queue);
    }
#ifdef PH_FS_STORAGE_PROFILE
    uint64_t before = profile_clock();
    request->profile_index = -1;
    if (!control && !error) {
        if (decoded.opcode == KB2_FILESYSTEM_OP_STATX) request->profile_index = 0;
        else if (decoded.opcode == KB2_FILESYSTEM_OP_PREAD) request->profile_index = 1;
        else if (decoded.opcode == KB2_FILESYSTEM_OP_PWRITE) request->profile_index = 2;
    }
    request->accepted = before;
#endif
    if (error) error = -EPROTO;
    else if (!control && decoded.opcode == KB2_FILESYSTEM_OP_EXEC_OPEN) error = -EPERM;
    else error = control ? queue->submit_control(queue->workers, input, input_size, capacity, request) :
        queue->submit(queue->workers, authenticated, input, input_size, capacity, request);
    if (copied) { ph_free(input, copied); queue->inventory_bytes -= copied; }
    if (error == -EAGAIN) {
        /* No Linux request was admitted: give back only this last unwritten
         * chain, then sleep until accepted work releases its charged budget.
         * No staging or mutable-byte backlog survives credential updates. */
        mask = lane_lock(lane);
        status = kb2_vq_return_available(&lane->channel.lane, config->generation, &request->chain);
        lane_unlock(lane, mask);
        if (status && client) client_fault(queue, client);
        request_release(queue, request);
        return status ? client ? PH_LIFECYCLE_SERVICE_PROGRESS : fault(queue) : 0;
    }
    if (error) {
#ifdef PH_FS_STORAGE_PROFILE
        request->profile_index = -1;
#endif
        if (error == -EPROTO || error == -EINVAL) {
            if (client) { client_fault(queue, client); error = 0; }
            else error = fault(queue);
        } else {
            unsigned char response[KB2_FILESYSTEM_RESPONSE_SIZE];
            kb2_fs_response_t denied = {.opcode = decoded.opcode, .generation = config->generation,
                .sequence = decoded.sequence, .result = error};
            PH_CHECK(!kb2_fs_response_encode(response, sizeof(response), &denied));
            error = publish(queue, request, ipc, response, sizeof(response));
        }
        request_release(queue, request);
        return error ? error : PH_LIFECYCLE_SERVICE_PROGRESS;
    }
    if (control) {
        queue->control_active = request;
        if (decoded.opcode == KB2_FILESYSTEM_OP_CLIENT_REGISTER ||
            decoded.opcode == KB2_FILESYSTEM_OP_CLIENT_CREDENTIALS ||
            decoded.opcode == KB2_FILESYSTEM_OP_CLIENT_UNREGISTER || decoded.opcode == KB2_FILESYSTEM_OP_TRANSFER_DUP) {
            queue->barrier_client = decoded.client;
            if (decoded.opcode == KB2_FILESYSTEM_OP_TRANSFER_DUP) queue->barrier_target = decoded.second_handle;
        }
    }
    if (!client && index == 2) queue->exchange_consumed = 1;
#ifdef PH_FS_STORAGE_PROFILE
    if (request->profile_index >= 0)
        atomic_fetch_add(&queue->profile_rows[request->profile_index][1], profile_clock() - before);
#endif
    return PH_LIFECYCLE_SERVICE_PROGRESS;
}

static int accept_client(struct ph_fs_queue *queue, struct ph_ipc *ipc) {
    struct ph_fs_client_queue *first = queue->client_cursor ? queue->client_cursor : queue->clients;
    if (!first) return 0;
    struct ph_fs_client_queue *client = first;
    do {
        queue->client_cursor = client->next ? client->next : queue->clients;
        if (client->lane.bound && !client->lane.retiring) {
            struct ph_ipc_packet packet = {0};
            int error = ph_ipc_receive(&client->ipc, client->config.generation, &packet);
            if (!error && (packet.operation != PH_FS_QUEUE_NOTIFY || packet.correlation != client->config.channel_id ||
                packet.fd_count || packet.value != client->lane.channel.queue.available_notification_id)) error = -EPROTO;
            int closed = ph_ipc_packet_release(&packet);
            if (closed) { client->ipc.rejected = packet; error = closed; }
            if (error && error != -EAGAIN) client_fault(queue, client);
            if (!client->lane.retiring) {
                error = accept_lane(queue, &client->lane, &client->config, client, 0, ipc);
                if (error) return error;
            }
        }
        client = queue->client_cursor;
    } while (client != first);
    return 0;
}

static int start(void *context, void *linux_service) {
    struct ph_fs_queue *queue = context;
    struct kobox_linux_io_service *io = linux_service;
    if (!io || !io->filesystem) return -EINVAL;
    return queue->create(io->filesystem, &queue->worker_config, &queue->workers);
}

static int advance(void *context, void *linux_service, struct ph_ipc *ipc, bool quiescing) {
    struct ph_fs_queue *queue = context;
    (void)linux_service;
    if (!queue->workers) return -EPROTO;
    if (quiescing && !queue->closing) {
        queue->closing = 1;
        int error = queue->close(queue->workers);
        if (error) return error;
    }
    int progressed = collect_one(queue, ipc);
    if (progressed < 0) return progressed;
    int retired = queue->closing ? 0 : retire_one(queue, ipc);
    if (retired < 0) return retired;
    if (queue->closing) return queue->inflight ? (progressed || retired ? 2 : 0) : 1;
    if (queue->prefer_client) {
        int result = accept_client(queue, ipc);
        if (result) { queue->prefer_client = 0; return result; }
    }
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        unsigned i = queue->next;
        queue->next = (queue->next + 1) % 3;
        int result = accept_lane(queue, &queue->lanes[i], &queue->config[i], NULL, i, ipc);
        if (result) { queue->prefer_client = 1; return result; }
    }
    int result = accept_client(queue, ipc);
    return result ? result : progressed || retired ? PH_LIFECYCLE_SERVICE_PROGRESS : 0;
}

static int finish(void *context, void *linux_service) {
    struct ph_fs_queue *queue = context;
    (void)linux_service;
    if (!queue->workers) return 0;
    PH_CHECK(!queue->inflight);
    int error = queue->close(queue->workers);
    if (!error) error = queue->destroy(queue->workers);
    if (!error) queue->workers = NULL;
    return error;
}

static int stop(void *context) {
    struct ph_fs_queue *queue = context;
    if (queue->inflight || queue->workers) return -EBUSY;
    int error = 0;
    for (struct ph_fs_client_queue *client = queue->clients; client; client = client->next) {
        int status = client_retire(queue, client);
        if (status) error = status;
    }
    for (unsigned i = 0; i < 3; ++i) {
        int status = lane_retire(queue, &queue->lanes[i], &queue->config[i]);
        if (status) error = status;
    }
    if (queue->wake_tx.generation) {
        int status = ph_ipc_destroy(&queue->wake_tx, queue->config[0].generation);
        if (status) error = status;
    }
    if (queue->wake_rx.generation) {
        int status = ph_ipc_destroy(&queue->wake_rx, queue->config[0].generation);
        if (status) error = status;
    }
    return error;
}

static int wait_events(struct pacha_pollfd *events, size_t count, uint64_t timeout) {
    long status = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY, (uintptr_t)events, count, timeout, 0);
    size_t ready = 0;
    for (size_t i = 0; i < count; ++i) ready += events[i].revents != 0;
    if (ready) return status > 0 && (size_t)status == ready ? 1 : -EIO;
    return status == 0 || status == PACHA_SYSCALL_ERR_NOT_READY ? 0 : -EIO;
}

static void drain_wake(struct ph_fs_queue *queue) {
    struct ph_ipc_packet packet = {0};
    int result;
    while (!(result = ph_ipc_receive(&queue->wake_rx, queue->config[0].generation, &packet))) {
        PH_CHECK(packet.operation == 1 && !packet.fd_count && !packet.correlation && !packet.value);
        PH_OK(ph_ipc_packet_release(&packet));
    }
    PH_CHECK(result == -EAGAIN);
}

static uint64_t wait_snapshot(struct ph_fs_queue *queue, int control_fd, uint64_t after,
    struct pacha_pollfd *events, size_t *count, uint64_t *epoch, int *whole) {
    events[0] = (struct pacha_pollfd){.fd = control_fd,
        .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
    events[1] = (struct pacha_pollfd){.fd = queue->wake_rx.fd,
        .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
    *count = 2;
    uint64_t following = 0, last = after;
    watch_lock(queue);
    *epoch = queue->watch_epoch;
    for (struct ph_fs_client_queue *client = queue->clients; client; client = client->next) {
        if (!client->watch_fd || client->config.channel_id <= after) continue;
        if (*count == 256) { following = last; break; }
        events[(*count)++] = (struct pacha_pollfd){.fd = client->watch_fd,
            .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
        last = client->config.channel_id;
    }
    *whole = !after && !following;
    watch_unlock(queue);
    return following;
}

static int wait_clients(void *context, int control_fd, uint64_t timeout) {
    struct ph_fs_queue *queue = context;
    for (;;) {
        uint64_t after = 0;
        do {
            struct pacha_pollfd events[256];
            size_t count; uint64_t epoch; int whole;
            after = wait_snapshot(queue, control_fd, after, events, &count, &epoch, &whole);
            int ready = wait_events(events, count, 0);
            watch_lock(queue);
            int changed = epoch != queue->watch_epoch;
            watch_unlock(queue);
            if (events[1].revents) drain_wake(queue);
            if (changed || ready) return changed || ready > 0 ? 0 : ready;
        } while (after);
        if (!timeout) return 0;
        struct pacha_pollfd events[256];
        size_t count; uint64_t epoch; int whole;
        uint64_t following = wait_snapshot(queue, control_fd, queue->wait_after,
            events, &count, &epoch, &whole);
        int ready = wait_events(events, count, whole ? timeout : 1);
        queue->wait_after = following;
        watch_lock(queue);
        int changed = epoch != queue->watch_epoch;
        watch_unlock(queue);
        if (events[1].revents) drain_wake(queue);
        if (changed || ready || whole) return changed || ready >= 0 ? 0 : ready;
        if (timeout != PACHA_FD_WAIT_FOREVER) --timeout;
    }
}

int ph_fs_queue_init(struct ph_fs_queue *queue, void *core,
    const struct ph_fs_channel_config *data, const struct ph_fs_channel_config *owner,
    const kb2_vq_atomic_ops_t *atomics, struct ph_lifecycle_service *service) {
    if (!queue || !core || !data || !owner || !atomics || !service ||
        data->generation != owner->generation || data->channel_id == owner->channel_id ||
        !ph_core.ram_size || owner->arena_bytes > SIZE_MAX / 2) return -EINVAL;
    size_t mapping, offset;
    if (ph_fs_channel_layout(data, &mapping, &offset) || ph_fs_channel_layout(owner, &mapping, &offset))
        return -EINVAL;
    memset(queue, 0, sizeof(*queue));
    atomic_flag_clear(&queue->watch_lock);
    for (unsigned i = 0; i < 3; ++i) {
        atomic_init(&queue->lanes[i].retiring, 0);
        atomic_init(&queue->lanes[i].ring_lock, 0);
    }
#ifdef PH_FS_STORAGE_PROFILE
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 5; ++j) atomic_init(&queue->profile_rows[i][j], 0);
#endif
    queue->config[0] = *data; queue->config[1] = *owner; queue->atomics = *atomics;
    queue->inventory_budget = ph_core.ram_size / 16;
    unsigned workers = PH_CPU_COUNT * 2;
    if (workers < 3) workers = 3;
    queue->worker_config = (struct kobox_linux_fs_worker_config){.workers = workers,
        .client_running = workers / 2, .client_requests = data->queue_size / 2,
        .total_requests = data->queue_size, .client_bytes = ph_core.ram_size / 16,
        .total_bytes = ph_core.ram_size / 8, .control_bytes = owner->arena_bytes * 2,
        .publish = worker_publish};
#ifdef PH_FS_OWNER_PUBLISH
    queue->worker_config.publish = NULL;
#endif
    if (queue->worker_config.client_running < 2) queue->worker_config.client_running = 2;
#define LOAD(member, symbol_name) do { \
    void *symbol = ph_image_lookup(core, symbol_name); \
    if (!symbol) return -ENOENT; \
    memcpy(&queue->member, &symbol, sizeof(queue->member)); \
} while (0)
    LOAD(create, "kobox_linux_fs_workers_create");
    LOAD(submit, "kobox_linux_fs_workers_submit");
    LOAD(submit_control, "kobox_linux_fs_workers_submit_control");
    LOAD(collect, "kobox_linux_fs_workers_collect");
    LOAD(release_work, "kobox_linux_fs_workers_release");
    LOAD(close, "kobox_linux_fs_workers_close");
    LOAD(destroy, "kobox_linux_fs_workers_destroy");
#undef LOAD
    uint64_t pair[2] = {0};
    long status = pacha_syscall3(PACHA_IPC_SYSCALL_CHANNEL_CREATE, (uintptr_t)pair,
        PH_IPC_CHANNEL_RIGHTS | PACHA_FD_RIGHT_CLOSE, PACHA_FD_FLAG_CLOEXEC | PACHA_FD_FLAG_PRIVATE);
    if (status) return -ENOMEM;
    /* Creation owns both FDs even if initialization fails. Keep them in a
     * packet until init takes ownership; every failure closes the remainder. */
    struct ph_ipc_packet inventory = {.fd_count = 2, .fds = {{.fd = pair[0]}, {.fd = pair[1]}}};
    int error = ph_ipc_init(&queue->wake_tx, (int)pair[0], data->generation);
    if (!error) { inventory.fds[0].fd = PH_IPC_NO_FD;
        error = ph_ipc_init(&queue->wake_rx, (int)pair[1], data->generation); }
    if (!error) inventory.fds[1].fd = PH_IPC_NO_FD;
    PH_OK(ph_ipc_packet_release(&inventory));
    if (error) { if (queue->wake_tx.generation) PH_OK(ph_ipc_destroy(&queue->wake_tx, data->generation)); return error; }
    *service = (struct ph_lifecycle_service){.context = queue, .prepare = prepare,
        .complete = complete, .release = release, .start = start, .advance = advance,
        .finish = finish, .stop = stop, .wake = wake, .wake_fd = queue->wake_rx.fd, .wait = wait_clients};
    return 0;
}

int ph_fs_queue_finish(struct ph_fs_queue *queue) {
    if (!queue) return -EINVAL;
    if (queue->workers || queue->inflight || queue->wake_tx.fd || queue->wake_rx.fd ||
        queue->wake_tx.rejected.fd_count || queue->wake_rx.rejected.fd_count) return -EBUSY;
    while (queue->clients) {
        struct ph_fs_client_queue *client = queue->clients;
        if (client->lane.mapping || client->lane.slots || client->ipc.fd || client->ipc.rejected.fd_count) return -EBUSY;
        queue->clients = client->next;
        queue->inventory_bytes -= sizeof(*client);
        ph_free(client, sizeof(*client));
    }
    if (queue->inventory_bytes) return -EBUSY;
#ifdef PH_FS_STORAGE_PROFILE
    profile_dump(queue);
#endif
    return 0;
}
