/* SPDX-License-Identifier: MIT */
#include "net_frame_service.h"
#include "host.h"

#include <pacha/syscall.h>
#include <errno.h>
#include <string.h>

static int prepare(void *context, struct ph_ipc_packet *packet) {
    struct ph_net_frame_service *net = context;
    if (!packet->correlation || packet->generation != net->generation || net->operation)
        return -EPROTO;
    if (packet->operation == PH_NET_FRAME_ATTACH) {
        struct pacha_fd_info info = {0};
        if (net->shared || packet->fd_count != 1 ||
            packet->value != PH_NET_FRAME_SHARED_BYTES ||
            pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO,
                packet->fds[0].fd, (uintptr_t)&info) ||
            info.kind != PACHA_FD_KIND_VMO ||
            info.size < PH_NET_FRAME_SHARED_BYTES ||
            (info.rights & (PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE)) !=
                (PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE))
            return -EPROTO;
        long mapped = pacha_syscall6(PACHA_VM_SYSCALL_MMAP,
            packet->fds[0].fd, 0, PH_NET_FRAME_SHARED_BYTES,
            PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
        if (mapped < 4096) return -ENOMEM;
        net->shared = (void *)(uintptr_t)mapped;
    } else if (packet->operation == PH_NET_FRAME_INFO ||
               packet->operation == PH_NET_WIFI_REQUEST ||
               packet->operation == PH_NET_WIFI_CONFIG ||
               packet->operation == PH_NET_WIFI_EVENTS ||
               packet->operation == PH_NET_FRAME_READ ||
               packet->operation == PH_NET_FRAME_WRITE) {
        if (!net->shared || packet->fd_count || packet->value ||
            net->shared->magic != PH_NET_FRAME_SHARED_MAGIC ||
            net->shared->generation != net->generation ||
            net->shared->operation != packet->operation ||
            (packet->operation == PH_NET_FRAME_WRITE &&
             net->shared->count > PH_NET_FRAME_BATCH))
            return -EPROTO;
        if (packet->operation == PH_NET_FRAME_WRITE) {
            net->frame_count = net->shared->count;
            if (net->frame_count > PH_NET_FRAME_BATCH) return -EPROTO;
            memcpy(net->frame_snapshot, net->shared->frames,
                net->frame_count * sizeof(net->frame_snapshot[0]));
        }
        if (packet->operation == PH_NET_WIFI_REQUEST || packet->operation == PH_NET_WIFI_CONFIG) {
            net->wifi_size = net->shared->wifi_length;
            if (!net->wifi_size || net->wifi_size > PH_NET_WIFI_BYTES) return -EPROTO;
            memcpy(net->wifi_snapshot, net->shared->wifi_bytes, net->wifi_size);
        }
    } else {
        return -EPROTO;
    }
    net->operation = packet->operation;
    net->correlation = packet->correlation;
    return 0;
}

static int dispatch(void *context, void *linux_service) {
    struct ph_net_frame_service *net = context;
    struct ph_net_frame_shared *shared = net->shared;
    size_t count = 0;
    int result = 0;
    if (!shared || !net->operation || !linux_service) return -EPROTO;
    if (net->operation == PH_NET_FRAME_INFO) {
        result = net->info(linux_service, &shared->info);
        shared->count = 0;
    } else if (net->operation == PH_NET_FRAME_READ) {
        result = net->read(linux_service, shared->frames, PH_NET_FRAME_BATCH, &count);
        shared->count = (uint32_t)count;
    } else if (net->operation == PH_NET_FRAME_WRITE) {
        for (uint32_t i = 0; i < net->frame_count; ++i) {
            const struct kobox_linux_net_frame *frame = &net->frame_snapshot[i];
            result = net->write(linux_service, frame->bytes, frame->length);
            if (result) break;
            ++count;
        }
        shared->count = (uint32_t)count;
    } else if (net->operation == PH_NET_WIFI_REQUEST) {
        result = net->wifi_request ? net->wifi_request(linux_service,
            net->wifi_snapshot, net->wifi_size, shared->wifi_bytes,
            sizeof(shared->wifi_bytes), &count) : -EOPNOTSUPP;
        if (count > sizeof(shared->wifi_bytes)) { count = 0; result = -EPROTO; }
        shared->wifi_length = (uint32_t)count;
    } else if (net->operation == PH_NET_WIFI_CONFIG) {
        result = net->wifi_configure ? net->wifi_configure(linux_service,
            net->wifi_snapshot, net->wifi_size) : -EOPNOTSUPP;
        for (size_t i = 0; i < PH_NET_WIFI_BYTES; ++i)
            ((volatile unsigned char *)shared->wifi_bytes)[i] = 0;
        shared->wifi_length = 0;
    } else if (net->operation == PH_NET_WIFI_EVENTS) {
        uint32_t events = 0;
        result = net->wifi_events ? net->wifi_events(linux_service, &events) : -EOPNOTSUPP;
        shared->wifi_events = events;
    } else if (net->operation == PH_NET_FRAME_ATTACH) {
        shared->count = 0;
    } else {
        return -EPROTO;
    }
    shared->status = result;
    atomic_thread_fence(memory_order_release);
    return 0;
}

static int complete(void *context, struct ph_ipc *ipc) {
    struct ph_net_frame_service *net = context;
    struct ph_ipc_packet reply = {.operation = net->operation,
        .generation = net->generation, .correlation = net->correlation,
        .value = (uint64_t)(int64_t)net->shared->status};
    return ph_ipc_send(ipc, &reply);
}

static int release(void *context) {
    struct ph_net_frame_service *net = context;
    net->operation = 0;
    net->correlation = 0;
    for (size_t i = 0; i < sizeof(net->wifi_snapshot); ++i)
        ((volatile unsigned char *)net->wifi_snapshot)[i] = 0;
    net->wifi_size = net->frame_count = 0;
    return 0;
}

static int stop(void *context) {
    struct ph_net_frame_service *net = context;
    if (!net->shared) return 0;
    if (pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP,
            (uintptr_t)net->shared, PH_NET_FRAME_SHARED_BYTES)) return -EIO;
    net->shared = NULL;
    return 0;
}

int ph_net_frame_service_init(struct ph_net_frame_service *net, void *core,
    uint64_t generation, struct ph_lifecycle_service *service) {
    if (!net || !core || !generation || !service) return -EINVAL;
    void *info = ph_image_lookup(core, "kobox_linux_net_port_info");
    void *read = ph_image_lookup(core, "kobox_linux_net_port_read");
    void *write = ph_image_lookup(core, "kobox_linux_net_port_write");
    if (!info || !read || !write) return -ENOENT;
    memset(net, 0, sizeof(*net));
    memcpy(&net->info, &info, sizeof(info));
    memcpy(&net->read, &read, sizeof(read));
    memcpy(&net->write, &write, sizeof(write));
#if PH_SANDBOX_WIFI
    void *wifi_request = ph_image_lookup(core, "kobox_linux_net_wifi_request");
    void *wifi_configure = ph_image_lookup(core, "kobox_linux_net_wifi_configure");
    void *wifi_events = ph_image_lookup(core, "kobox_linux_net_wifi_events");
    if (!wifi_request || !wifi_configure || !wifi_events) return -ENOENT;
    memcpy(&net->wifi_request, &wifi_request, sizeof(wifi_request));
    memcpy(&net->wifi_configure, &wifi_configure, sizeof(wifi_configure));
    memcpy(&net->wifi_events, &wifi_events, sizeof(wifi_events));
#endif
    net->generation = generation;
    *service = (struct ph_lifecycle_service){.context = net,
        .prepare = prepare, .dispatch = dispatch, .complete = complete,
        .release = release, .stop = stop};
    return 0;
}
