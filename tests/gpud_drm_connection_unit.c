#include <assert.h>
#include "../userland/gpud/drm_service.c"

static unsigned char page[GPUD_DRM_PAGE_BYTES];
static unsigned char auxiliary[GPUD_DRM_AUX_REUSE_BYTES];
static uint64_t auxiliary_size = sizeof(auxiliary), auxiliary_rights;
static int auxiliary_map_failure, gpu_calls, output_request, fence_request, fence_waits;
static struct pacha_ipc_msg queued;
static struct pacha_ipc_fd queued_fds[PACHA_IPC_MAX_TRANSFER_FDS];
static int maps, unmaps, closes, replies, ready_fd, hungup_fd, map_failure, unmap_failure, close_failure;
static struct pacha_ipc_msg last_reply;

long pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b)
{
    if (nr == PACHA_FD_SYSCALL_GET_INFO) {
        struct pacha_fd_info *info = (void *)(uintptr_t)b;
        *info = (struct pacha_fd_info){.kind = a == 20 || a == 23 ? PACHA_FD_KIND_VMO :
            a == 21 || a == 24 || a == 25 ? PACHA_FD_KIND_CHANNEL : PACHA_FD_KIND_REPLY,
            .size = a == 20 ? GPUD_DRM_PAGE_BYTES : a == 23 ? auxiliary_size : 0,
            .rights = a == 23 ? auxiliary_rights : UINT64_MAX};
        return 0;
    }
    if (nr == PACHA_VM_SYSCALL_MUNMAP) {
        assert((a == (uintptr_t)page && b == sizeof(page)) ||
            (a == (uintptr_t)auxiliary && b == auxiliary_size));
        ++unmaps; return unmap_failure;
    }
    assert(nr == PACHA_IPC_SYSCALL_RECV);
    if ((int)a != ready_fd) return PACHA_SYSCALL_ERR_EMPTY;
    struct pacha_ipc_msg *message = (void *)(uintptr_t)b;
    assert(message->fd_capacity >= queued.fd_count);
    struct pacha_ipc_fd *fds = message->fds;
    *message = queued;
    message->fds = fds;
    memcpy(fds, queued_fds, queued.fd_count * sizeof(*fds));
    ready_fd = 0;
    return 0;
}
long pacha_syscall6(uint64_t nr, uint64_t fd, uint64_t address, uint64_t bytes,
    uint64_t prot, uint64_t flags, uint64_t offset)
{
    assert(nr == PACHA_VM_SYSCALL_MMAP && !address);
    assert((fd == 20 && bytes == sizeof(page)) || (fd == 23 && bytes == auxiliary_size));
    assert(prot == (PACHA_PROT_READ | PACHA_PROT_WRITE) && flags == PACHA_MMAP_SHARED && !offset);
    ++maps;
    return (map_failure || (fd == 23 && auxiliary_map_failure)) ? PACHA_SYSCALL_ERR_ALLOC :
        (long)(uintptr_t)(fd == 20 ? page : auxiliary);
}
int pacha_fd_close(int fd) { assert(fd >= 16); ++closes; return close_failure; }
int pacha_ipc_reply(int fd, const struct pacha_ipc_msg *reply)
{ assert(fd == 22); ++replies; last_reply = *reply; return 0; }
int ph_ipc_packet_release(struct ph_ipc_packet *packet)
{
    for (size_t i = 0; i < packet->fd_count; ++i)
        if (packet->fds[i].fd != PH_IPC_NO_FD) assert(!pacha_fd_close((int)packet->fds[i].fd));
    *packet = (struct ph_ipc_packet){0};
    return 0;
}

int gpud_gpu_rpc_call(struct gpud_gpu_rpc *r, unsigned int q,
    const unsigned char *b, size_t n, unsigned char *o, size_t cap, size_t *s)
{
    assert(q == GPUD_GPU_QUEUE_EXECUTION); ++gpu_calls;
    kb2_protocol_message_envelope_t envelope;
    assert(!kb2_protocol_message_envelope_decode(b, n, &envelope));
    unsigned char result[8] = {0};
    kb2_gpu_inline_completion_t completion = {.session_id = 7,
        .status = KB2_GPU_STATUS_OK, .length = sizeof(result), .data = result,
        .record_schema_id = output_request ? KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_RESULT :
            KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_RESULT};
    if (output_request) {
        for (size_t i = 0; i < 12; ++i) assert(!r->mapping[GPUD_GPU_AUX_OFFSET + i]);
        memset(r->mapping + GPUD_GPU_AUX_OFFSET, 0x5a, 4);
        result[0] = result[4] = 12;
    } else {
        assert(!memcmp(r->mapping + GPUD_GPU_AUX_OFFSET, auxiliary, 12));
    }
    size_t payload_size;
    assert(!kb2_gpu_inline_completion_encode(o + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
        cap - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, &payload_size, &completion));
    envelope.payload_length = payload_size;
    *s = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + payload_size;
    assert(!kb2_protocol_message_envelope_encode(o, *s, &envelope));
    return 0;
}
int gpud_gpu_rpc_take_mapping(struct gpud_gpu_rpc *r, uint64_t c, uint64_t id, struct pacha_ipc_fd *f)
{ (void)r;(void)c;(void)id;(void)f; assert(0); return -EIO; }
int gpud_gpu_rpc_take_dma_buf(struct gpud_gpu_rpc *r, uint64_t c, uint64_t id, struct pacha_ipc_fd *f)
{ (void)r;(void)c;(void)id;(void)f; assert(0); return -EIO; }
int gpud_gpu_rpc_release_mapping(struct gpud_gpu_rpc *r, uint64_t c, uint64_t id)
{ (void)r;(void)c;(void)id; assert(0); return -EIO; }
int gpud_gpu_rpc_next_event(struct gpud_gpu_rpc *r, unsigned char *m, size_t cap, size_t *s)
{ (void)r;(void)m;(void)cap; *s = 0; return 0; }
int pacha_ipc_channel_create(struct pacha_ipc_channel_pair *p, uint64_t r, uint32_t f)
{ (void)p;(void)r;(void)f; assert(0); return -EIO; }
int pacha_fd_get_info(int f, struct pacha_fd_info *i)
{ return (int)pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, f, (uintptr_t)i); }
int pacha_vmo_revoke(int f) { (void)f; assert(0); return -EIO; }
int pacha_ipc_send(int f, const struct pacha_ipc_msg *m)
{ (void)f;(void)m; assert(0); return -EIO; }
long pacha_syscall4(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    assert(nr == PACHA_FD_SYSCALL_WAIT_MANY && b == 2 && c == UINT64_MAX && !d);
    struct pacha_pollfd *fds = (void *)(uintptr_t)a;
    assert(fds[0].fd == 24); ++fence_waits;
    fds[0].revents = PACHA_FD_EVENT_READABLE;
    return 1;
}
long pacha_fd_wait_many_batched(struct pacha_pollfd *fds, uint64_t count, uint64_t ticks)
{
    assert(!ticks);
    long ready = 0;
    for (uint64_t i = 0; i < count; ++i) {
        fds[i].revents = (fds[i].fd == ready_fd ? PACHA_FD_EVENT_READABLE : 0) |
            (fds[i].fd == hungup_fd ? PACHA_FD_EVENT_HANGUP : 0);
        ready += !!fds[i].revents;
    }
    return ready;
}
long pacha_fd_poll(struct pacha_pollfd *fds, uint64_t count)
{ (void)fds;(void)count; assert(0); return -EIO; }

static void binding(struct gpud_drm_service *service, int aux)
{
    queued = (struct pacha_ipc_msg){.word0 = GPUD_DRM_BIND_PAGE_REQUEST_MAGIC,
        .word1 = aux ? sizeof(auxiliary) : 0, .fd_count = 3u + aux};
    queued_fds[0] = (struct pacha_ipc_fd){.fd = 20};
    queued_fds[1] = (struct pacha_ipc_fd){.fd = 21};
    queued_fds[2] = (struct pacha_ipc_fd){.fd = 23, .rights = auxiliary_rights};
    queued_fds[2u + aux] = (struct pacha_ipc_fd){.fd = 22};
    ready_fd = service->endpoint_fd;
    assert(!gpud_drm_service_receive(service));
}
static void auxiliary_call(struct gpud_drm_service *service, uint64_t handle, int bound)
{
    pacha_service_envelope_t *header = (void *)page;
    *header = (pacha_service_envelope_t){.magic = PACHA_SERVICE_REQUEST_MAGIC,
        .abi_version = PACHA_SERVICE_ABI_VERSION, .service_id = GPUD_DRM_SERVICE_ID,
        .op = GPUD_DRM_OP_HANDLE_IOCTL, .request_id = 30 + gpu_calls,
        .flags = PACHA_SERVICE_FLAG_PAGE_PAYLOAD, .payload_size = sizeof(gpud_drm_ioctl_request_t)};
    gpud_drm_ioctl_request_t *request = (void *)(page + sizeof(*header));
    *request = (gpud_drm_ioctl_request_t){.handle = handle, .aux_size = 12};
    if (output_request) {
        gpud_drm_virtgpu_get_caps_t caps = {.cap_set_id = 1, .size = 12};
        request->request = GPUD_DRM_IOCTL_VIRTGPU_GET_CAPS;
        request->arg_size = request->data_size = sizeof(caps);
        memcpy(request->data, &caps, sizeof(caps));
    } else {
        gpud_drm_virtgpu_execbuffer_t exec = {.size = 4, .bo_handles = 8,
            .num_bo_handles = 1, .fence_fd = -1};
        if (fence_request) {
            exec.flags = GPUD_DRM_VIRTGPU_EXECBUF_FENCE_FD_IN | GPUD_DRM_VIRTGPU_EXECBUF_FENCE_FD_OUT;
            request->fd_flags = GPUD_DRM_IOCTL_FD_INPUT_WAIT | GPUD_DRM_IOCTL_FD_OUTPUT_NOTIFY;
        }
        request->request = GPUD_DRM_IOCTL_VIRTGPU_EXECBUFFER;
        request->arg_size = request->data_size = sizeof(exec);
        memcpy(request->data, &exec, sizeof(exec));
    }
    memset(auxiliary, 0xa5, 12);
    queued = (struct pacha_ipc_msg){.word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = bound ? GPUD_DRM_REQUEST_BOUND_AUX : 0,
        .word3 = header->request_id, .fd_count = bound ? 1 : 2};
    queued_fds[0] = (struct pacha_ipc_fd){.fd = 23, .rights = auxiliary_rights};
    size_t index = bound ? 0 : 1;
    if (fence_request) {
        queued_fds[index++] = (struct pacha_ipc_fd){.fd = 24};
        queued_fds[index++] = (struct pacha_ipc_fd){.fd = 25};
        queued.fd_count += 2;
    }
    queued_fds[index] = (struct pacha_ipc_fd){.fd = 22};
    ready_fd = 21;
    assert(!gpud_drm_service_reap_hangups(service));
    assert(!gpud_drm_service_receive(service));
    assert(!header->status && !service->request_aux);
    if (output_request) {
        for (size_t i = 0; i < 12; ++i) assert(auxiliary[i] == (i < 4 ? 0x5a : 0));
    }
}
int main(void)
{
    static struct gpud_drm_service service;
    service.endpoint_fd = 16; service.backend_client = 1; service.files.generation = 1;
    auxiliary_rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    struct pacha_ipc_fd auxiliary_fd = {.fd = 23, .rights = auxiliary_rights};
    assert(valid_aux_fd(&auxiliary_fd, sizeof(auxiliary)));
    assert(!valid_aux_fd(&auxiliary_fd, 4096));
    auxiliary_fd.flags = PACHA_FD_FLAG_PRIVATE;
    assert(!valid_aux_fd(&auxiliary_fd, sizeof(auxiliary)));
    auxiliary_fd.flags = 0; auxiliary_fd.rights &= ~PACHA_FD_RIGHT_MAP_WRITE;
    assert(!valid_aux_fd(&auxiliary_fd, sizeof(auxiliary)));
    binding(&service, 0);
    assert(service.connections && service.connections->page == page && service.connections->fd == 21);
    assert(maps == 1 && !unmaps && closes == 2 && last_reply.word0 == GPUD_DRM_BIND_PAGE_REPLY_MAGIC);
    for (unsigned i = 1; i <= 8; ++i) {
        pacha_service_envelope_t *header = (void *)page;
        *header = (pacha_service_envelope_t){.magic = PACHA_SERVICE_REQUEST_MAGIC,
            .abi_version = PACHA_SERVICE_ABI_VERSION, .service_id = GPUD_DRM_SERVICE_ID,
            .op = GPUD_DRM_OP_HELLO, .request_id = i};
        queued = (struct pacha_ipc_msg){.word0 = PACHA_SERVICE_REQUEST_MAGIC, .word3 = i, .fd_count = 1};
        queued_fds[0] = (struct pacha_ipc_fd){.fd = 22};
        ready_fd = 21;
        assert(!gpud_drm_service_reap_hangups(&service) && service.connections->ready);
        assert(!gpud_drm_service_receive(&service));
        assert(header->status == 0 && header->result == 1 && header->request_id == i);
        assert(last_reply.word3 == i && !last_reply.word1 && !service.page);
    }
    assert(maps == 1 && !unmaps); /* No per-request remapping. */
    /* A bound request cannot add a second control VMO and change attachment
     * meaning. The normal HELLO validator sees and rejects the extra FD. */
    pacha_service_envelope_t *header = (void *)page;
    *header = (pacha_service_envelope_t){.magic = PACHA_SERVICE_REQUEST_MAGIC,
        .abi_version = PACHA_SERVICE_ABI_VERSION, .service_id = GPUD_DRM_SERVICE_ID,
        .op = GPUD_DRM_OP_HELLO, .request_id = 9};
    queued = (struct pacha_ipc_msg){.word0 = PACHA_SERVICE_REQUEST_MAGIC, .word3 = 9, .fd_count = 2};
    queued_fds[0] = (struct pacha_ipc_fd){.fd = 20};
    queued_fds[1] = (struct pacha_ipc_fd){.fd = 22};
    ready_fd = 21;
    assert(!gpud_drm_service_reap_hangups(&service));
    assert(!gpud_drm_service_receive(&service) && header->status == -EINVAL);
    assert(maps == 1 && !unmaps);
    struct pacha_pollfd events[2];
    assert(gpud_drm_service_pollfds(&service, NULL, 0) == 1);
    assert(gpud_drm_service_pollfds(&service, events, 2) == 1);
    assert(events[0].fd == 21 && (events[0].events & PACHA_FD_EVENT_READABLE));
    unmap_failure = 1; hungup_fd = 21;
    assert(gpud_drm_service_reap_hangups(&service) == -EIO && service.connections->page == page);
    assert(service.error == -EIO && gpud_drm_service_receive(&service) == -EIO);
    unmap_failure = 0; close_failure = 1;
    assert(retire_connection(&service.connections) == -EIO && !service.connections->page);
    close_failure = 0;
    int before = unmaps;
    assert(!retire_connection(&service.connections) && !service.connections && unmaps == before);
    service.error = 0; hungup_fd = 0;
    map_failure = 1;
    binding(&service, 0);
    assert(!service.connections && (int64_t)last_reply.word1 == -ENOMEM);
    map_failure = 0; auxiliary_map_failure = 1;
    before = unmaps;
    binding(&service, 1);
    assert(!service.connections && (int64_t)last_reply.word1 == -ENOMEM && unmaps == before + 1);
    auxiliary_map_failure = 0;
    binding(&service, 1);
    assert(service.connections->aux == auxiliary);
    memset(&service.files, 0, sizeof(service.files));
    assert(!gpud_drm_files_init(&service.files, 1, 4));
    uint64_t handle;
    assert(!gpud_drm_file_open_begin(&service.files, 1, &handle));
    assert(!gpud_drm_file_open_finish(&service.files, 1, handle, 7, 0));
    service.gpu.mapping = calloc(1, GPUD_GPU_AUX_OFFSET + sizeof(auxiliary));
    assert(service.gpu.mapping);
    before = unmaps; int before_maps = maps;
    for (int i = 0; i < 8; ++i) {
        output_request = i % 2;
        auxiliary_call(&service, handle, 1);
    }
    assert(gpu_calls == 8 && unmaps == before && maps == before_maps);
    struct ph_ipc ipc = {.fd = 26, .generation = 1};
    service.gpu.ipc = &ipc;
    output_request = 0; fence_request = 1;
    auxiliary_call(&service, handle, 1);
    assert(fence_waits == 1 && service.fences && service.fences->fd == 25);
    assert(!retire_fence(&service, service.fences, 0));
    fence_request = 0;
    /* The unbound-aux path retains its strict exact-size VMO and unmap. */
    auxiliary_size = 4096;
    auxiliary_call(&service, handle, 0);
    assert(gpu_calls == 10 && maps == before_maps + 1 && unmaps == before + 1);
    auxiliary_size = sizeof(auxiliary);
    unmap_failure = 1;
    assert(retire_connection(&service.connections) == -EIO && service.connections->aux == auxiliary);
    unmap_failure = 0;
    assert(!retire_connection(&service.connections) && !service.connections);
    free(service.gpu.mapping);
    free(service.pollfds);
    puts("GPUD page connection: bind, actual HELLO reuse/correlation, cleanup failures PASS");
    puts("GPUD auxiliary connection: real execbuffer/caps codecs, copy/zero, fallback, lifetime PASS");
}
