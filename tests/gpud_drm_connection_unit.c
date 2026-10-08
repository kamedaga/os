#if defined(GPUD_DRM_STACK_BUDGET)
#define _GNU_SOURCE
#endif
#include <assert.h>
#if defined(GPUD_DRM_STACK_BUDGET)
#include <pthread.h>
#endif
#ifndef GPUD_DRM_SERVICE_SOURCE
#define GPUD_DRM_SERVICE_SOURCE "../userland/gpud/drm_service.c"
#endif
#include GPUD_DRM_SERVICE_SOURCE

static char diagnostic[256];
static unsigned int observed_phases;
int pacha_log_emit(int endpoint_fd, unsigned source, const char *message, size_t length)
{
    assert(endpoint_fd == 29 && source == PACHA_LOG_SOURCE_GPUD);
    assert(length < sizeof(diagnostic));
    memcpy(diagnostic, message, length);
    diagnostic[length] = 0;
    if (strstr(diagnostic, "phase=acquire status=0")) observed_phases |= 1;
    if (strstr(diagnostic, "phase=encode status=0")) observed_phases |= 2;
    if (strstr(diagnostic, "phase=wire status=0")) observed_phases |= 4;
    if (strstr(diagnostic, "phase=transport status=0")) observed_phases |= 8;
    if (strstr(diagnostic, "phase=transport-return status=0")) observed_phases |= 16;
    return 0;
}

static unsigned char page[GPUD_DRM_PAGE_BYTES];
static unsigned char auxiliary[GPUD_DRM_AUX_REUSE_BYTES];
static uint64_t auxiliary_size = sizeof(auxiliary), auxiliary_rights;
static int auxiliary_map_failure, gpu_calls, output_request, fence_request, fence_waits;
static int resources_request;
static struct pacha_ipc_msg queued;
static struct pacha_ipc_fd queued_fds[PACHA_IPC_MAX_TRANSFER_FDS];
static int maps, unmaps, closes, replies, ready_fd, hungup_fd, map_failure, unmap_failure, close_failure;
static struct pacha_ipc_msg last_reply;

static void bounded_startup_journal(void)
{
    static struct gpud_drm_service service;
    service.profile_kind = KB2_GPU_PROFILE_AMDGPU;
    service.log_endpoint_fd = 29;
    service.files.generation = 9;
    gpud_drm_ioctl_request_t request = {.handle = 17,
        .request = GPUD_DRM_IOCTL_MODE_GETRESOURCES,
        .arg_size = 64, .data_size = 256};
    pacha_service_envelope_t header = {.op = GPUD_DRM_OP_HANDLE_IOCTL,
        .payload_size = sizeof(request)};
    assert(log_request_begin(&service, &header, &request) == 1);
    assert(strstr(diagnostic, "DRM request begin seq=1 generation=9"));
    assert(strstr(diagnostic, "handle=17 ioctl=0xc04064a0 arg=64 wire=256 aux=0"));
    header = (pacha_service_envelope_t){.op = GPUD_DRM_OP_HELLO};
    uint64_t result = 0;
    struct gpud_drm_reply_transfer transfer = {0};
    assert(dispatch(&service, &header, NULL, &result, &transfer, 0) == -EINVAL);
    assert(strstr(diagnostic, "DRM request end seq=2 generation=9"));
    assert(strstr(diagnostic, "status=-22"));
    for (unsigned int i = 3; i <= 64; i++)
        assert(log_request_begin(&service, &header, NULL) == i);
    assert(strstr(diagnostic, "startup-log-limit=64"));
    assert(!log_request_begin(&service, &header, NULL));
    assert(service.startup_requests_logged == 64);
    assert(strstr(diagnostic, "startup-log-limit=64"));
    service.startup_requests_logged = 0;
    service.profile_kind = KB2_GPU_PROFILE_VIRGL;
    assert(!log_request_begin(&service, &header, NULL));
}

static void prime_profile_admission(void)
{
    static struct gpud_drm_service service;
    const unsigned profiles[] = {KB2_GPU_PROFILE_VIRGL, KB2_GPU_PROFILE_AMDGPU};
    uint64_t result = 0;
    struct gpud_drm_reply_transfer transfer = {0};
    for (size_t i = 0; i < 2; ++i) {
        service.profile_kind = profiles[i];
        service.files.generation = 1;
        service.received.fd_count = 2;
        gpud_drm_prime_export_request_t export = {0};
        pacha_service_envelope_t header = {.op = GPUD_DRM_OP_PRIME_EXPORT,
            .payload_size = sizeof(export)};
        /* A missing GEM handle is an invalid request for either profile,
         * not evidence that the GPU cannot share images. */
        assert(dispatch_request(&service, &header, &export, &result,
            &transfer, 0) == -EINVAL);
        gpud_drm_prime_import_request_t import = {.token = 1};
        header.op = GPUD_DRM_OP_PRIME_IMPORT;
        header.payload_size = sizeof(import) - 1;
        assert(dispatch_request(&service, &header, &import, &result,
            &transfer, 0) == -EINVAL);
        header.payload_size = sizeof(import);
        service.received.fd_count = 1;
        assert(dispatch_request(&service, &header, &import, &result,
            &transfer, 0) == -EINVAL);
        service.received.fd_count = 2;
        gpud_drm_prime_token_request_t token = {.token = 1};
        header = (pacha_service_envelope_t){.op = GPUD_DRM_OP_PRIME_RELEASE,
            .payload_size = sizeof(token)};
        assert(dispatch_request(&service, &header, &token, &result,
            &transfer, 0) == -ENOENT);
        service.received.fd_count = 3;
        header.op = GPUD_DRM_OP_PRIME_ACQUIRE;
        assert(dispatch_request(&service, &header, &token, &result,
            &transfer, 0) == -EINVAL);
    }
}

static void primary_master_lifetime(void)
{
    static struct gpud_drm_service service;
    service.files.generation = 7;
    service.log_endpoint_fd = 29;
    service.watches[0] = (struct gpud_drm_watch){.handle = 41, .device_minor = 0};
    service.watches[1] = (struct gpud_drm_watch){.handle = 42, .device_minor = 0};
    service.watches[2] = (struct gpud_drm_watch){.handle = 43, .device_minor = 128};
    gpud_drm_ioctl_request_t set = {.handle = 41,
        .request = GPUD_DRM_IOCTL_SET_MASTER};
    gpud_drm_ioctl_request_t drop = {.handle = 41,
        .request = GPUD_DRM_IOCTL_DROP_MASTER};
    primary_opened(&service, 41);
    assert(service.master_handle == 41);
    assert(strstr(diagnostic, "master acquired by open generation=7 handle=41"));
    primary_opened(&service, 42);
    assert(service.master_handle == 41);
    set.handle = 42;
    assert(primary_master_check(&service, &set) == -EBUSY);
    set.handle = 43;
    assert(primary_master_check(&service, &set) == -EINVAL);
    drop.handle = 42;
    assert(primary_master_check(&service, &drop) == -EINVAL);
    drop.handle = 41;
    assert(!primary_master_check(&service, &drop));
    primary_master_completed(&service, &drop);
    assert(!service.master_handle);
    set.handle = 42;
    assert(!primary_master_check(&service, &set));
    primary_master_completed(&service, &set);
    assert(service.master_handle == 42);
    primary_closed(&service, 41);
    assert(service.master_handle == 42);
    primary_closed(&service, 42);
    assert(!service.master_handle);
    assert(strstr(diagnostic, "master released by close generation=7 handle=42"));
    primary_opened(&service, 41);
    assert(service.master_handle == 41);
}

static void kms_session_lifetime(void)
{
    static struct gpud_drm_service service;
    service.files.generation = 8;
    service.watches[0] = (struct gpud_drm_watch){.handle = 51, .device_minor = 0};
    primary_opened(&service, 51);
    gpud_drm_mode_atomic_wire_t wire = {
        .flags = GPUD_DRM_MODE_ATOMIC_ALLOW_MODESET |
            GPUD_DRM_MODE_ATOMIC_TEST_ONLY,
        .count_objs = 3, .total_props = 3,
        .objects = {101, 102, 103},
        .object_prop_counts = {1, 1, 1},
        .props = {201, 202, 203},
        .prop_values = {102, 301, 1}};
    gpud_drm_ioctl_request_t request = {.handle = 51,
        .request = GPUD_DRM_IOCTL_MODE_ATOMIC,
        .arg_size = 56, .data_size = sizeof(wire)};
    memcpy(request.data, &wire, sizeof(wire));
    assert(!display_session_check(&service, &request));
    wire.flags &= ~GPUD_DRM_MODE_ATOMIC_TEST_ONLY;
    memcpy(request.data, &wire, sizeof(wire));
    assert(!display_session_check(&service, &request));
    wire.flags |= GPUD_DRM_MODE_ATOMIC_TEST_ONLY;
    memcpy(request.data, &wire, sizeof(wire));
    display_session_completed(&service, &request);
    assert(!service.kms_session_mode);
    wire.flags &= ~GPUD_DRM_MODE_ATOMIC_TEST_ONLY;
    memcpy(request.data, &wire, sizeof(wire));
    assert(!display_session_check(&service, &request));
    request.handle = 52;
    assert(display_session_check(&service, &request) == -EACCES);
    request.handle = 51;
    gpud_drm_ioctl_request_t legacy = {.handle = 51,
        .request = GPUD_DRM_IOCTL_MODE_SETCRTC};
    assert(!display_session_check(&service, &legacy));
    display_session_completed(&service, &request);
    assert(display_session_check(&service, &legacy) == -EBUSY);
    wire.prop_values[2] = 2; /* A later frame has different plane state. */
    memcpy(request.data, &wire, sizeof(wire));
    assert(!display_session_check(&service, &request));
    gpud_drm_ioctl_request_t drop = {.handle = 51,
        .request = GPUD_DRM_IOCTL_DROP_MASTER};
    primary_master_completed(&service, &drop);
    primary_opened(&service, 51);
    assert(!display_session_check(&service, &legacy));
    display_session_completed(&service, &legacy);
    wire.flags |= GPUD_DRM_MODE_ATOMIC_TEST_ONLY;
    memcpy(request.data, &wire, sizeof(wire));
    assert(display_session_check(&service, &request) == -EBUSY);
    primary_closed(&service, 51);
    assert(!service.kms_session_mode);
}

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
    assert(q == (resources_request ? GPUD_GPU_QUEUE_DISPLAY : GPUD_GPU_QUEUE_EXECUTION));
    ++gpu_calls;
    kb2_protocol_message_envelope_t envelope;
    assert(!kb2_protocol_message_envelope_decode(b, n, &envelope));
    unsigned char result[KB2_GPU_DRM_MODE_RECORD_RESOURCES_RESULT_SIZE] = {0};
    kb2_gpu_inline_completion_t completion = {.session_id = 7,
        .status = KB2_GPU_STATUS_OK, .length = 8, .data = result,
        .record_schema_id = output_request ? KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_RESULT :
            KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_RESULT};
    if (resources_request) {
        kb2_gpu_command_t command;
        assert(!kb2_gpu_command_decode(b + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
            envelope.payload_length, envelope.generation, KB2_GPU_PROFILE_AMDGPU,
            KB2_GPU_QUEUE_DISPLAY, NULL, 0, &command));
        assert(command.command_id == KB2_GPU_DRM_MODE_COMMAND_GET_RESOURCES);
        assert(command.counts[0] == 1 && !command.counts[1]);
        completion.record_schema_id = KB2_GPU_DRM_MODE_RECORD_RESOURCES_RESULT;
        completion.length = sizeof(result);
        result[0] = 1;
        result[12] = result[16] = result[20] = 1;
        uint32_t maximum = 4096;
        memcpy(result + 28, &maximum, sizeof(maximum));
        memcpy(result + 36, &maximum, sizeof(maximum));
    } else if (output_request) {
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
        .header_size = sizeof(pacha_service_envelope_t), .service_id = GPUD_DRM_SERVICE_ID,
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
static void resources_call(struct gpud_drm_service *service)
{
    observed_phases = 0;
    pacha_service_envelope_t *header = (void *)page;
    *header = (pacha_service_envelope_t){.magic = PACHA_SERVICE_REQUEST_MAGIC,
        .header_size = sizeof(pacha_service_envelope_t), .service_id = GPUD_DRM_SERVICE_ID,
        .op = GPUD_DRM_OP_HANDLE_IOCTL, .request_id = 99,
        .flags = PACHA_SERVICE_FLAG_PAGE_PAYLOAD, .payload_size = sizeof(gpud_drm_ioctl_request_t)};
    gpud_drm_ioctl_request_t *request = (void *)(page + sizeof(*header));
    *request = (gpud_drm_ioctl_request_t){.handle = 1,
        .request = GPUD_DRM_IOCTL_MODE_GETRESOURCES,
        .arg_size = sizeof(gpud_drm_mode_card_res_t),
        .data_size = sizeof(gpud_drm_kms_resources_wire_t)};
    queued = (struct pacha_ipc_msg){.word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word3 = 99, .fd_count = 1};
    queued_fds[0] = (struct pacha_ipc_fd){.fd = 22};
    service->connections->ready = 1;
    ready_fd = 21;
    resources_request = 1;
    assert(!gpud_drm_service_receive(service));
    assert(!header->status && !service->files.files[0].in_flight);
    gpud_drm_kms_resources_wire_t wire;
    memcpy(&wire, request->data, sizeof(wire));
    assert(!wire.value.count_fbs && wire.value.count_crtcs == 1 &&
        wire.value.count_connectors == 1 && wire.value.count_encoders == 1);
    assert(wire.value.max_width == 4096 && wire.value.max_height == 4096);
    assert(strstr(diagnostic, "DRM request end seq=1"));
    assert(observed_phases == 31);
    assert(!service->active_request_sequence);
    resources_request = 0;
}

static struct gpud_drm_service *resources_service(void)
{
    static struct gpud_drm_service service;
    static struct gpud_drm_connection connection;
    service.endpoint_fd = 16;
    service.backend_client = 1;
    service.profile_kind = KB2_GPU_PROFILE_AMDGPU;
    service.log_endpoint_fd = 29;
    assert(!gpud_drm_files_init(&service.files, 1, 4));
    uint64_t handle;
    assert(!gpud_drm_file_open_begin(&service.files, 1, &handle) && handle == 1);
    assert(!gpud_drm_file_open_finish(&service.files, 1, handle, 7, 0));
    connection = (struct gpud_drm_connection){.fd = 21, .page = page};
    service.connections = &connection;
    service.gpu.mapping = calloc(1, GPUD_GPU_CHANNEL_SIZE);
    assert(service.gpu.mapping);
    return &service;
}

#if defined(GPUD_DRM_STACK_BUDGET)
static void *resources_stack(void *argument)
{
    /* The release binary retains about 30 KiB in main/run while serving.
     * Model that live caller frame on the actual 128 KiB service budget;
     * host sanitizer stacks are deliberately not used for this regression. */
    volatile unsigned char caller[30 * 1024];
    for (size_t i = 0; i < sizeof(caller); ++i) caller[i] = (unsigned char)i;
    resources_call(argument);
    for (size_t i = 0; i < sizeof(caller); ++i) assert(caller[i] == (unsigned char)i);
    return NULL;
}
#endif

int main(void)
{
#if defined(GPUD_DRM_STACK_BUDGET)
    struct gpud_drm_service *service = resources_service();
    pthread_attr_t attributes;
    pthread_t thread;
    assert(!pthread_attr_init(&attributes));
    assert(!pthread_attr_setstacksize(&attributes, PACHA_PROCESS_DEFAULT_STACK_SIZE));
    assert(!pthread_create(&thread, &attributes, resources_stack, service));
    assert(!pthread_join(thread, NULL));
    assert(!pthread_attr_destroy(&attributes));
    free(service->gpu.mapping);
    puts("GPUD count-only GETRESOURCES on 128 KiB service stack PASS");
    return 0;
#else
    struct gpud_drm_service *resources = resources_service();
    resources_call(resources);
    free(resources->gpu.mapping);
    resources->gpu.mapping = NULL;
    gpu_calls = 0;
    bounded_startup_journal();
    prime_profile_admission();
    kms_session_lifetime();
    primary_master_lifetime();
    static struct gpud_drm_service service;
    service.endpoint_fd = 16; service.backend_client = 1; service.files.generation = 1;
    /* The optional physical GPU endpoint is published before manual probe.
     * Both the cached-page handshake and uncached request must fail promptly. */
    queued = (struct pacha_ipc_msg){.word0 = GPUD_DRM_BIND_PAGE_REQUEST_MAGIC,
        .fd_count = 3};
    queued_fds[0] = (struct pacha_ipc_fd){.fd = 20};
    queued_fds[1] = (struct pacha_ipc_fd){.fd = 21};
    queued_fds[2] = (struct pacha_ipc_fd){.fd = 22};
    ready_fd = 16;
    assert(!gpud_drm_service_reject_unavailable(16));
    assert(last_reply.word0 == GPUD_DRM_BIND_PAGE_REPLY_MAGIC &&
        (int64_t)last_reply.word1 == -ENODEV);
    pacha_service_envelope_t *offline_header = (void *)page;
    *offline_header = (pacha_service_envelope_t){.magic = PACHA_SERVICE_REQUEST_MAGIC,
        .header_size = sizeof(pacha_service_envelope_t), .service_id = GPUD_DRM_SERVICE_ID,
        .op = GPUD_DRM_OP_OPEN_NODE, .request_id = 17};
    queued = (struct pacha_ipc_msg){.word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word3 = 17, .fd_count = 2};
    queued_fds[0] = (struct pacha_ipc_fd){.fd = 20};
    queued_fds[1] = (struct pacha_ipc_fd){.fd = 22};
    ready_fd = 16;
    assert(!gpud_drm_service_reject_unavailable(16));
    assert(offline_header->magic == PACHA_SERVICE_REPLY_MAGIC &&
        offline_header->status == -ENODEV && last_reply.word3 == 17);
    assert(maps == 1 && unmaps == 1);
    queued = (struct pacha_ipc_msg){.word0 = GPUD_DRM_INLINE_IOCTL_REQUEST_MAGIC,
        .word2 = UINT64_C(0xc010640c), .fd_count = 1};
    queued_fds[0] = (struct pacha_ipc_fd){.fd = 22};
    ready_fd = 16;
    assert(!gpud_drm_service_reject_unavailable(16));
    assert(last_reply.word0 == GPUD_DRM_INLINE_IOCTL_REPLY_MAGIC &&
        (int64_t)last_reply.word1 == -ENODEV && last_reply.word2 == queued.word2);
    maps = unmaps = closes = replies = 0;
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
            .header_size = sizeof(pacha_service_envelope_t), .service_id = GPUD_DRM_SERVICE_ID,
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
        .header_size = sizeof(pacha_service_envelope_t), .service_id = GPUD_DRM_SERVICE_ID,
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
    service.profile_kind = KB2_GPU_PROFILE_AMDGPU;
    service.log_endpoint_fd = 29;
    gpud_drm_ioctl_request_t unsupported = {.handle = handle,
        .request = UINT32_C(0xc004644f), .arg_size = 4, .data_size = 4};
    memcpy(unsupported.data, "test", 4);
    assert(ioctl_request(&service, &unsupported) == -EOPNOTSUPP);
    assert(strstr(diagnostic, "phase=translate request=0xc004644f") != NULL);
    assert(strstr(diagnostic, "arg=4 wire=4 aux=0") != NULL);
    assert(strstr(diagnostic, "test") == NULL);
    service.profile_kind = 0;
    service.log_endpoint_fd = 0;
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
#endif
}
