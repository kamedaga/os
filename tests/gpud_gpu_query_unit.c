/* SPDX-License-Identifier: MIT */
#include "../kobox2/linux-sandbox/kobox/boot/drm_query.h"
#include "../userland/gpud/drm_reply.h"
#include "../userland/gpud/drm_translate.h"
#include "../userland/kobox2_adapter/gpu_query.h"
#include "../userland/kobox2_adapter/gpu_queue.h"
#include <kobox2/gpu_session.h>
#include <kobox2/virtqueue_x86_64.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned int calls;

static int version(struct kobox_linux_drm_file *file,
                   const size_t *capacity,
                   struct kobox_linux_drm_version *out) {
    assert(file);
    ++calls;
    if (capacity[0] > sizeof(out->name) || capacity[1] > sizeof(out->date) ||
        capacity[2] > sizeof(out->description))
        return -EINVAL;
    struct kobox_linux_drm_version result = {.major = 5,
                                             .minor = 6,
                                             .patchlevel = 7,
                                             .name_length = 17,
                                             .date_length = 0,
                                             .description_length = 4};
    memcpy(result.name, "query-unit-driver", capacity[0] < 17 ? capacity[0] : 17);
    memcpy(result.description, "test", capacity[2] < 4 ? capacity[2] : 4);
    *out = result;
    return 0;
}

static int get_cap(struct kobox_linux_drm_file *file, uint64_t capability, uint64_t *value) {
    assert(file);
    ++calls;
    if (capability == UINT64_MAX)
        return -EINVAL;
    if (capability == 6)
        return -ENODEV;
    *value = UINT64_C(0x1234567887654321);
    return 0;
}

struct ph_image ph_core;
static unsigned char native_vmo[PH_GPU_QUERY_VMO_SIZE];
static _Alignas(16) unsigned char queue_vmo[GPUD_GPU_CHANNEL_SIZE];
static void *active_vmo = native_vmo;
static size_t active_size = sizeof(native_vmo);
static struct ph_ipc_packet sent;
static unsigned int mapped, unmapped, sends;
static uint64_t mock_cookie;
static int mock_open_error, mock_close_error;
static int mock_send_error;
static unsigned int mock_opens, mock_closes;

static int file_open(struct kobox_linux_drm_service *owner, uint64_t *cookie) {
    assert(owner);
    ++mock_opens;
    if (mock_open_error)
        return mock_open_error;
    *cookie = ++mock_cookie;
    return 0;
}

static int file_lookup(struct kobox_linux_drm_service *owner,
                       uint64_t cookie,
                       struct kobox_linux_drm_file **file) {
    assert(owner && cookie && cookie <= mock_cookie);
    *file = (struct kobox_linux_drm_file *)(void *)&calls;
    return 0;
}

static int file_close(struct kobox_linux_drm_service *owner, uint64_t cookie) {
    assert(owner && cookie && cookie <= mock_cookie);
    ++mock_closes;
    return mock_close_error;
}

void *ph_image_lookup(void *image, const char *name) {
    assert(image == &ph_core);
    struct kobox_drm_query_api api = {.version = version, .get_cap = get_cap};
    void *symbol = NULL;
    if (!strcmp(name, "kobox_linux_drm_version"))
        memcpy(&symbol, &api.version, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_get_cap"))
        memcpy(&symbol, &api.get_cap, sizeof(symbol));
    struct ph_gpu_session_service files = {
        .open = file_open, .file = file_lookup, .close = file_close};
    if (!strcmp(name, "kobox_linux_drm_service_open"))
        memcpy(&symbol, &files.open, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_service_file"))
        memcpy(&symbol, &files.file, sizeof(symbol));
    if (!strcmp(name, "kobox_linux_drm_service_close"))
        memcpy(&symbol, &files.close, sizeof(symbol));
    return symbol;
}

long pacha_syscall2(uint64_t number, uint64_t a0, uint64_t a1) {
    if (number == PACHA_FD_SYSCALL_GET_INFO) {
        assert(a0 == 16);
        *(struct pacha_fd_info *)(uintptr_t)a1 =
            (struct pacha_fd_info){.kind = PACHA_FD_KIND_VMO,
                                   .size = active_size,
                                   .rights = PH_GPU_QUERY_RIGHTS,
                                   .flags = PACHA_FD_FLAG_CLOEXEC};
    } else {
        assert(number == PACHA_VM_SYSCALL_MUNMAP && a0 == (uintptr_t)active_vmo &&
               a1 == active_size);
        ++unmapped;
    }
    return 0;
}

long pacha_syscall6(
    uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    assert(number == PACHA_VM_SYSCALL_MMAP && a0 == 16 && !a1 && a2 == active_size);
    assert(a3 == (PACHA_PROT_READ | PACHA_PROT_WRITE) && a4 == PACHA_MMAP_SHARED && !a5);
    ++mapped;
    return (long)(uintptr_t)active_vmo;
}

int ph_ipc_send(struct ph_ipc *ipc, struct ph_ipc_packet *packet) {
    assert(ipc->generation == packet->generation);
    sent = *packet;
    ++sends;
    return mock_send_error;
}

static void native_transport_case(uint64_t capability, int expected_error) {
    static struct ph_gpu_query query;
    memset(&query, 0, sizeof(query));
    memset(native_vmo, 0xa5, sizeof(native_vmo));
    mapped = unmapped = sends = 0;
    struct ph_lifecycle_service service;
    struct gpud_drm_binding binding = {.generation = 9, .frontend_handle = 8, .session_id = 7};
    drmd_ioctl_request_t request = {
        .handle = 8, .request = UINT64_C(0xc010640c), .arg_size = 16, .data_size = 16};
    memcpy(request.data, &capability, sizeof(capability));
    struct gpud_drm_translation encoded;
    assert(!gpud_drm_ioctl_encode(&encoded, &binding, &request, 1));
    kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
                                                .opcode = KB2_GPU_OPCODE_COMMAND,
                                                .generation = 9,
                                                .correlation_id = 99,
                                                .payload_length = encoded.command_size};
    size_t size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + encoded.command_size;
    assert(!kb2_protocol_message_envelope_encode(native_vmo, size, &envelope));
    memcpy(native_vmo + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, encoded.command, encoded.command_size);
    struct ph_ipc_packet packet = {
        .operation = PH_GPU_QUERY_OPERATION,
        .generation = 9,
        .correlation = 99,
        .value = size,
        .fd_count = 1,
        .fds = {{.fd = 16, .rights = PH_GPU_QUERY_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC}}};
    assert(!ph_gpu_query_init(&query, 9, 7, &service));
    packet.fds[0].rights ^= PACHA_FD_RIGHT_MAP_WRITE;
    assert(service.prepare(service.context, &packet) == -EACCES && !mapped);
    packet.fds[0].rights ^= PACHA_FD_RIGHT_MAP_WRITE;
    assert(!service.prepare(service.context, &packet) && mapped == 1);
    memset(native_vmo, 0xcc, PH_GPU_QUERY_PAGE); /* Peer mutation cannot alter the private plan. */
    assert(!service.dispatch(service.context, &calls));
    struct ph_ipc ipc = {.generation = 9};
    assert(service.complete(service.context, &ipc) == (capability == 6 ? -ENODEV : 0));
    assert(sends == 1 && sent.generation == 9 && sent.correlation == 99 && !sent.fd_count);
    assert(gpud_drm_ioctl_reply(&request,
                                &encoded,
                                99,
                                native_vmo + PH_GPU_QUERY_REPLY_OFFSET,
                                sent.value,
                                native_vmo + PH_GPU_QUERY_OUTPUT_OFFSET,
                                PH_GPU_QUERY_PAGE) == expected_error);
    assert(!service.release(service.context) && unmapped == 1 && !query.mapping);
    assert(service.prepare(service.context, &packet) == -EPROTO && mapped == 1); /* No replay. */
}

static void codec_errors(const unsigned char *bytes, size_t size, uint64_t session) {
    unsigned char mutated[512];
    kb2_gpu_inline_completion_t saved = {.session_id = 77, .status = 88}, decoded = saved;
    for (size_t length = 0; length < size; ++length) {
        assert(kb2_gpu_inline_completion_decode(bytes, length, session, &decoded));
        assert(!memcmp(&saved, &decoded, sizeof(saved)));
    }
    assert(kb2_gpu_inline_completion_decode(bytes, size, session + 1, &decoded));
    const size_t offsets[] = {KB2_GPU_COMPLETION_HEADER_TOTAL_SIZE_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_ABI_IDENTITY_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_SCHEMA_DIGEST_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_SPAN_COUNT_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_ATTACHMENT_COUNT_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_ARGUMENT_TABLE_OFFSET_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_INLINE_OFFSET_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_RESERVED_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_DETAIL_CODE_OFFSET,
                              KB2_GPU_COMPLETION_HEADER_DETAIL_SET_ID_OFFSET};
    for (size_t index = 0; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
        memcpy(mutated, bytes, size);
        mutated[offsets[index]] ^= 1;
        assert(kb2_gpu_inline_completion_decode(mutated, size, session, &decoded));
        assert(!memcmp(&saved, &decoded, sizeof(saved)));
    }
}

static void native_queue_case(unsigned int defect) {
    struct ph_gpu_queue queue = {0};
    struct gpud_gpu_sessions sessions = {0};
    struct gpud_gpu_channel frontend = {0};
    struct ph_lifecycle_service service;
    memset(queue_vmo, 0, sizeof(queue_vmo));
    active_vmo = queue_vmo;
    active_size = sizeof(queue_vmo);
    mapped = unmapped = sends = 0;
    assert(
        !ph_gpu_channel_bind(&frontend, queue_vmo, 9, 27, KB2_VQ_DRIVER, &kb2_vq_x86_64_atomics));
    assert(!ph_gpu_sessions_init(&sessions, 9, 2));
    assert(!ph_gpu_queue_init(&queue, &sessions, 7, 27, &kb2_vq_x86_64_atomics, &service));
    assert(service.next(service.context) == PH_LIFECYCLE_SERVICE_IDLE);
    struct ph_ipc_packet packet = {
        .operation = PH_GPU_QUEUE_BIND,
        .generation = 9,
        .value = 27,
        .fd_count = 1,
        .fds = {{.fd = 16, .rights = PH_GPU_QUERY_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC}}};
    if (defect == 1)
        queue_vmo[0] ^= 1;
    if (defect == 2)
        packet.value = 28;
    if (defect == 3)
        packet.fds[0].rights ^= PACHA_FD_RIGHT_MAP_WRITE;
    int result = service.prepare(service.context, &packet);
    if (defect >= 1 && defect <= 3) {
        assert(result < 0);
        assert(!service.stop(service.context) && !queue.mapping && mapped == unmapped);
        assert(mapped == (defect == 1));
        return;
    }
    assert(result == PH_LIFECYCLE_SERVICE_IDLE && mapped == 1);
    /* Open through the control queue, not a preinstalled session fixture. */
    unsigned char *control = queue_vmo + GPUD_GPU_CONTROL_REQUEST_OFFSET;
    size_t control_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + KB2_GPU_SESSION_OPEN_REQUEST_SIZE;
    kb2_protocol_message_envelope_t open_envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
                                                     .opcode = KB2_GPU_OPCODE_SESSION_OPEN,
                                                     .generation = 9,
                                                     .correlation_id = 1,
                                                     .payload_length =
                                                         KB2_GPU_SESSION_OPEN_REQUEST_SIZE};
    kb2_gpu_session_open_t open = {.node_type = KB2_GPU_NODE_RENDER, .client_id = 7};
    assert(!kb2_protocol_message_envelope_encode(control, control_size, &open_envelope));
    assert(!kb2_gpu_session_open_encode(
        control + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, KB2_GPU_SESSION_OPEN_REQUEST_SIZE, &open));
    kb2_vq_segment_t control_segments[] = {
        {GPUD_GPU_CONTROL_REQUEST_OFFSET, control_size, 0, 0},
        {GPUD_GPU_CONTROL_REPLY_OFFSET, GPUD_GPU_CONTROL_REPLY_CAPACITY, 1, 1}};
    kb2_vq_chain_t control_chain = {.segments = control_segments, .capacity = 2, .count = 2};
    kb2_vq_t *control_lane = &frontend.lanes[GPUD_GPU_QUEUE_CONTROL];
    int control_ready, control_notify;
    assert(!kb2_vq_arm(control_lane, 9, &control_ready) && !control_ready);
    assert(!kb2_vq_publish(control_lane, 9, &control_chain, &control_notify) && control_notify);
    packet = (struct ph_ipc_packet){.operation = PH_GPU_QUEUE_NOTIFY, .generation = 9, .value = 3};
    assert(!service.prepare(service.context, &packet));
    assert(!service.dispatch(service.context, &calls));
    struct ph_ipc control_ipc = {.generation = 9};
    if (defect == 7) {
        /* Failed delivery must not drop the successfully opened file's
         * ownership. Channel retirement leaves it for GPL device close-all. */
        unsigned int closes = mock_closes;
        mock_send_error = -EIO;
        assert(service.complete(service.context, &control_ipc) == -EIO);
        mock_send_error = 0;
        assert(!service.release(service.context));
        assert(!service.stop(service.context) && unmapped == 1 && !queue.mapping);
        assert(sessions.occupied == 1 && mock_closes == closes);
        assert(sessions.entries[0].state == GPUD_GPU_SESSION_OPEN &&
               sessions.entries[0].file_cookie);
        return;
    }
    assert(!service.complete(service.context, &control_ipc));
    assert(!service.release(service.context));
    kb2_vq_chain_t *control_done = NULL;
    assert(!kb2_vq_take_used(control_lane, 9, &control_done) && control_done == &control_chain);
    unsigned char control_reply[GPUD_GPU_CONTROL_REPLY_CAPACITY];
    assert(!kb2_vq_copy_response(
        control_lane, 9, control_done, 0, control_reply, control_done->used_length));
    kb2_gpu_session_completion_t opened;
    assert(!kb2_gpu_session_completion_decode(control_reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                              control_done->used_length -
                                                  KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                              KB2_GPU_OPCODE_SESSION_OPEN,
                                              &opened));
    assert(!opened.status && opened.session_id && sends == 1 && sent.value == 4);
    assert(!kb2_vq_release(control_lane, 9, control_done));
    sends = 0;
    packet = (struct ph_ipc_packet){.operation = PH_GPU_QUEUE_NOTIFY, .generation = 9, .value = 7};
    assert(service.prepare(service.context, &packet) == PH_LIFECYCLE_SERVICE_IDLE);
    for (uint64_t correlation = 1; correlation <= 2; ++correlation) {
        struct gpud_drm_binding binding = {
            .generation = 9, .frontend_handle = 8, .session_id = opened.session_id};
        drmd_ioctl_request_t request = {
            .handle = 8, .request = UINT64_C(0xc010640c), .arg_size = 16, .data_size = 16};
        uint64_t capability = defect == 4 ? 6 : 5;
        memcpy(request.data, &capability, sizeof(capability));
        struct gpud_drm_translation encoded;
        assert(!gpud_drm_ioctl_encode(&encoded, &binding, &request, 1));
        kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
                                                    .opcode = KB2_GPU_OPCODE_COMMAND,
                                                    .generation = 9,
                                                    .correlation_id = correlation,
                                                    .payload_length = encoded.command_size};
        unsigned char *bytes = queue_vmo + GPUD_GPU_REQUEST_OFFSET;
        size_t size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + encoded.command_size;
        assert(!kb2_protocol_message_envelope_encode(bytes, size, &envelope));
        memcpy(bytes + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, encoded.command, encoded.command_size);
        kb2_vq_segment_t segments[] = {{GPUD_GPU_REQUEST_OFFSET, size, 0, 0},
                                       {GPUD_GPU_REPLY_OFFSET, GPUD_GPU_CHANNEL_PAGE, 1, 1}};
        kb2_vq_chain_t send = {.segments = segments, .capacity = 2, .count = 2};
        kb2_vq_t *lane = &frontend.lanes[GPUD_GPU_QUEUE_EXECUTION];
        int ready, notify;
        assert(kb2_vq_arm(lane, 9, &ready) == KB2_VQ_OK && !ready);
        assert(kb2_vq_publish(lane, 9, &send, &notify) == KB2_VQ_OK && notify);
        unsigned int before = calls;
        if (defect == 5)
            bytes[KB2_PROTOCOL_MESSAGE_ENVELOPE_GENERATION_OFFSET] ^= 1;
        if (defect == 6) {
            /* Output page cannot be used as a response descriptor. */
            uint64_t address = GPUD_GPU_OUTPUT_OFFSET;
            memcpy(queue_vmo + lane->queue.descriptor_address + 16, &address, sizeof(address));
        }
        result = service.next(service.context); /* Drain without another packet. */
        if (defect == 5 || defect == 6) {
            assert(result == -EPROTO && calls == before);
            assert(!service.stop(service.context) && unmapped == 1);
            return;
        }
        assert(!result);
        memset(bytes, 0xcc, size); /* Private plan survives hostile source changes. */
        assert(!service.dispatch(service.context, &calls) && calls == before + 1);
        struct ph_ipc ipc = {.generation = 9};
        assert(service.complete(service.context, &ipc) == (defect == 4 ? -ENODEV : 0));
        assert(!service.release(service.context) && !unmapped && mapped == 1);
        assert(sends == correlation && sent.operation == PH_GPU_QUEUE_NOTIFY && sent.value == 8);
        kb2_vq_chain_t *done = NULL;
        assert(kb2_vq_take_used(lane, 9, &done) == KB2_VQ_OK && done == &send);
        unsigned char reply[GPUD_GPU_CHANNEL_PAGE];
        assert(kb2_vq_copy_response(lane, 9, done, 0, reply, done->used_length) == KB2_VQ_OK);
        assert(gpud_drm_ioctl_reply(&request,
                                    &encoded,
                                    correlation,
                                    reply,
                                    done->used_length,
                                    queue_vmo + GPUD_GPU_OUTPUT_OFFSET,
                                    GPUD_GPU_CHANNEL_PAGE) == (defect == 4 ? -EIO : 0));
        assert(kb2_vq_release(lane, 9, done) == KB2_VQ_OK);
        if (defect == 4) {
            assert(service.next(service.context) == -ENODEV && calls == before + 1);
            break;
        }
        assert(service.prepare(service.context, &packet) == PH_LIFECYCLE_SERVICE_IDLE);
    }
    assert(!service.stop(service.context) && unmapped == 1 && !queue.mapping);
    assert(!service.stop(service.context) && unmapped == 1);
}

static kb2_gpu_session_completion_t session_request(struct ph_gpu_session_service *service,
                                                    uint64_t client,
                                                    uint64_t session,
                                                    uint32_t node) {
    uint32_t opcode = session ? KB2_GPU_OPCODE_SESSION_CLOSE : KB2_GPU_OPCODE_SESSION_OPEN;
    size_t payload =
        session ? KB2_GPU_SESSION_CLOSE_REQUEST_SIZE : KB2_GPU_SESSION_OPEN_REQUEST_SIZE;
    size_t size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + payload;
    kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
                                                .opcode = opcode,
                                                .generation = service->query.generation,
                                                .correlation_id = service->last_control + 1,
                                                .payload_length = payload};
    assert(!kb2_protocol_message_envelope_encode(service->query.request, size, &envelope));
    unsigned char *bytes = service->query.request + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE;
    if (session) {
        assert(!kb2_gpu_session_close_encode(bytes, payload, session));
    } else {
        kb2_gpu_session_open_t request = {.client_id = client, .node_type = node};
        assert(!kb2_gpu_session_open_encode(bytes, payload, &request));
    }
    assert(!ph_gpu_session_prepare(service, KB2_GPU_QUEUE_CONTROL, size));
    memset(service->query.request, 0xcc, size);
    assert(!ph_gpu_session_dispatch(service, &calls));
    kb2_gpu_session_completion_t completion;
    assert(!kb2_protocol_message_envelope_decode(
        service->query.reply, service->query.reply_size, &envelope));
    assert(envelope.opcode == opcode && envelope.correlation_id == service->last_control &&
           envelope.generation == service->query.generation);
    assert(!kb2_gpu_session_completion_decode(service->query.reply +
                                                  KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                              envelope.payload_length,
                                              opcode,
                                              &completion));
    assert(!ph_gpu_session_service_release(service));
    return completion;
}

static void session_service_cases(void) {
    struct gpud_gpu_sessions sessions = {0};
    struct ph_gpu_session_service first = {0}, other = {0};
    assert(!ph_gpu_sessions_init(&sessions, 9, 2));
    assert(!ph_gpu_session_service_init(&first, &sessions, 7));
    assert(!ph_gpu_session_service_init(&other, &sessions, 8));
    mock_opens = mock_closes = 0;
    kb2_gpu_session_completion_t reply = session_request(&first, 8, 0, KB2_GPU_NODE_RENDER);
    assert(reply.status == KB2_GPU_STATUS_DENIED && !mock_opens && !sessions.occupied);
    reply = session_request(&first, 7, 0, KB2_GPU_NODE_PRIMARY);
    assert(reply.status == KB2_GPU_STATUS_UNSUPPORTED && !mock_opens);
    mock_open_error = -ENOMEM;
    reply = session_request(&first, 7, 0, KB2_GPU_NODE_RENDER);
    assert(reply.status == KB2_GPU_STATUS_NO_MEMORY && !reply.session_id && !sessions.occupied);
    mock_open_error = 0;
    reply = session_request(&first, 7, 0, KB2_GPU_NODE_RENDER);
    assert(!reply.status && reply.session_id > 1 && mock_opens == 2);
    uint64_t a = reply.session_id;
    reply = session_request(&other, 8, a, 0);
    assert(reply.status == KB2_GPU_STATUS_DENIED && !mock_closes);
    reply = session_request(&other, 8, 0, KB2_GPU_NODE_RENDER);
    assert(!reply.status && reply.session_id != a && sessions.occupied == 2);
    uint64_t b = reply.session_id, cookie;
    reply = session_request(&first, 7, 0, KB2_GPU_NODE_RENDER);
    assert(reply.status == KB2_GPU_STATUS_LIMIT && mock_opens == 3);
    /* Model an already admitted request on another lane. Never close its file
     * or report CLOSE success while it is still in dispatch. */
    assert(!ph_gpu_session_acquire(&sessions, 9, 7, a, &cookie));
    reply = session_request(&first, 7, a, 0);
    assert(reply.status == KB2_GPU_STATUS_BUSY && !mock_closes);
    assert(ph_gpu_session_acquire(&sessions, 9, 7, a, &cookie) == KB2_GPU_STATUS_SESSION_LOST);
    assert(!ph_gpu_session_release(&sessions, 9, 7, a));
    reply = session_request(&first, 7, a, 0);
    assert(!reply.status && mock_closes == 1 && sessions.occupied == 1);
    reply = session_request(&first, 7, a, 0);
    assert(reply.status == KB2_GPU_STATUS_NOT_FOUND && mock_closes == 1);
    mock_close_error = -EIO;
    reply = session_request(&other, 8, b, 0);
    assert(reply.status == KB2_GPU_STATUS_DEVICE_LOST && mock_closes == 2 &&
           sessions.occupied == 1);
    assert(other.query.terminal_after_completion == -ENODEV);
    assert(ph_gpu_session_prepare(&other, KB2_GPU_QUEUE_CONTROL, 0) == -ENODEV);
    assert(ph_gpu_session_close_begin(&sessions, 9, 8, b) == KB2_GPU_STATUS_SESSION_LOST);
    mock_close_error = 0;
}

int main(void) {
    struct gpud_drm_binding binding = {.generation = 9, .frontend_handle = 8, .session_id = 7};
    struct kobox_drm_query_api api = {.version = version, .get_cap = get_cap};
    const kb2_gpu_region_t region = {
        .region_id = 1, .rights = KB2_GPU_SPAN_RIGHT_WRITE, .length = 224};
    for (unsigned int test = 0; test < 6; ++test) {
        drmd_ioctl_request_t request = {.handle = binding.frontend_handle};
        if (test < 3) {
            drmd_version_wire_t wire = {.name_capacity = test == 0 ? 64 : test == 1 ? 1 : 0};
            request.request = UINT64_C(0xc0406400);
            request.arg_size = 64;
            request.data_size = sizeof(wire);
            memcpy(request.data, &wire, sizeof(wire));
        } else {
            uint64_t data[2] = {test == 4 ? UINT64_MAX : 5, 0};
            request.request = test == 5 ? UINT64_C(0x4010640d) : UINT64_C(0xc010640c);
            request.arg_size = request.data_size = sizeof(data);
            memcpy(request.data, data, sizeof(data));
        }
        struct gpud_drm_translation translation;
        assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, region.region_id));
        struct kobox_drm_query query, sentinel;
        memset(&query, 0xa5, sizeof(query));
        sentinel = query;
        assert(kobox_drm_query_prepare(&query,
                                       binding.generation + 1,
                                       binding.session_id,
                                       translation.command,
                                       translation.command_size,
                                       &region) == -EPROTO);
        assert(!memcmp(&query, &sentinel, sizeof(query)));
        assert(kobox_drm_query_prepare(&query,
                                       binding.generation,
                                       binding.session_id + 1,
                                       translation.command,
                                       translation.command_size,
                                       &region) == -EPROTO);
        assert(!kobox_drm_query_prepare(&query,
                                        binding.generation,
                                        binding.session_id,
                                        translation.command,
                                        translation.command_size,
                                        &region));
        struct gpud_drm_translation admitted = translation;
        memset(translation.command, 0xcc, sizeof(translation.command));
        unsigned char output[224] = {0}, completion[512];
        size_t size = 0;
        unsigned int before = calls;
        assert(kobox_drm_query_execute(
                   &query, &api, (void *)&calls, output, sizeof(output), completion, 1, &size) ==
               -EINVAL);
        assert(calls == before);
        assert(!kobox_drm_query_execute(&query,
                                        &api,
                                        (void *)&calls,
                                        output,
                                        sizeof(output),
                                        completion,
                                        sizeof(completion),
                                        &size));
        assert(calls == before + (test == 5 ? 0 : 1));
        kb2_gpu_inline_completion_t decoded;
        assert(!kb2_gpu_inline_completion_decode(completion, size, binding.session_id, &decoded));
        if (test < 3) {
            assert(decoded.status == KB2_GPU_STATUS_OK && decoded.length == 32 &&
                   decoded.record_schema_id == KB2_GPU_DRM_CORE_RECORD_VERSION_RESULT);
            assert(decoded.data[0] == 5 && decoded.data[16] == 17);
            assert(output[0] == (test == 2 ? 0 : 'q'));
            assert(output[1] == (test == 0 ? 'u' : 0));
        } else if (test == 3) {
            assert(decoded.status == KB2_GPU_STATUS_OK && decoded.length == 8);
            assert(decoded.data[0] == 0x21 && decoded.data[7] == 0x12);
        } else {
            assert(decoded.status ==
                   (test == 4 ? KB2_GPU_STATUS_INVALID : KB2_GPU_STATUS_UNSUPPORTED));
            assert(!decoded.length && !decoded.record_schema_id);
        }
        codec_errors(completion, size, binding.session_id);
        unsigned char reply[1024];
        kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
                                                    .opcode = KB2_GPU_OPCODE_COMMAND,
                                                    .generation = binding.generation + 1,
                                                    .correlation_id = 99,
                                                    .payload_length = size};
        size_t reply_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + size;
        assert(!kb2_protocol_message_envelope_encode(reply, reply_size, &envelope));
        memcpy(reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, completion, size);
        drmd_ioctl_request_t original = request;
        assert(gpud_drm_ioctl_reply(
                   &request, &admitted, 99, reply, reply_size, output, sizeof(output)) == -EPROTO);
        assert(!memcmp(&request, &original, sizeof(request)));
        envelope.generation = binding.generation;
        assert(!kb2_protocol_message_envelope_encode(reply, reply_size, &envelope));
        assert(gpud_drm_ioctl_reply(
                   &request, &admitted, 100, reply, reply_size, output, sizeof(output)) == -EPROTO);
        int result = gpud_drm_ioctl_reply(
            &request, &admitted, 99, reply, reply_size, output, sizeof(output));
        if (test >= 4) {
            assert(result == (test == 4 ? -EINVAL : -EOPNOTSUPP));
            assert(!memcmp(&request, &original, sizeof(request)));
        } else {
            assert(!result);
            if (test < 3) {
                drmd_version_wire_t version;
                memcpy(&version, request.data, sizeof(version));
                assert(version.major == 5 && version.name_length == 17);
                assert(version.name[0] == (test == 2 ? 0 : 'q'));
                drmd_ioctl_request_t unchanged = request;
                size_t record_offset = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE +
                                       KB2_GPU_COMPLETION_HEADER_SIZE +
                                       KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE;
                reply[record_offset + 12] = 1;
                assert(gpud_drm_ioctl_reply(
                           &request, &admitted, 99, reply, reply_size, output, sizeof(output)) ==
                       -EPROTO);
                assert(!memcmp(&request, &unchanged, sizeof(request)));
                reply[record_offset + 12] = 0;
                size_t id_offset = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE +
                                   KB2_GPU_COMPLETION_HEADER_SIZE +
                                   KB2_GPU_ARGUMENT_DESCRIPTOR_RECORD_SCHEMA_ID_OFFSET;
                reply[id_offset] ^= 1;
                assert(gpud_drm_ioctl_reply(
                           &request, &admitted, 99, reply, reply_size, output, sizeof(output)) ==
                       -EPROTO);
                assert(!memcmp(&request, &unchanged, sizeof(request)));
            } else {
                uint64_t value;
                memcpy(&value, request.data + 8, sizeof(value));
                assert(value == UINT64_C(0x1234567887654321));
            }
        }
    }
    native_transport_case(5, 0);
    native_transport_case(UINT64_MAX, -EINVAL);
    native_transport_case(6, -EIO);
    for (unsigned int defect = 0; defect <= 7; ++defect)
        native_queue_case(defect);
    session_service_cases();
    puts("gpud GPU query pipeline: PASS encode, private plan, typed dispatch, canonical "
         "completion, loss admission");
    return 0;
}
