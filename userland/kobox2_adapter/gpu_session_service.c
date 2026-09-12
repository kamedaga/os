/* SPDX-License-Identifier: MIT */
#include "gpu_session_service.h"

#include <errno.h>
#include <kobox2/gpu_session.h>

static uint32_t file_status(int result) {
    switch (result) {
    case 0:
        return KB2_GPU_STATUS_OK;
    case -ENOMEM:
        return KB2_GPU_STATUS_NO_MEMORY;
    case -EMFILE:
        return KB2_GPU_STATUS_LIMIT;
    case -ENOENT:
        return KB2_GPU_STATUS_NOT_FOUND;
    case -EPERM:
    case -EACCES:
        return KB2_GPU_STATUS_DENIED;
    default:
        return KB2_GPU_STATUS_DEVICE_LOST;
    }
}

int ph_gpu_session_service_init(struct ph_gpu_session_service *service,
                                struct gpud_gpu_sessions *sessions,
                                uint64_t client_id) {
    if (!service || service->client_id || !sessions || !sessions->generation || !client_id)
        return -EINVAL;
    int result =
        ph_gpu_query_init(&service->query, sessions->generation, 0, &service->query_service);
    if (result)
        return result;
    void *symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_service_open");
    if (!symbol)
        return -ENOENT;
    memcpy(&service->open, &symbol, sizeof(service->open));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_service_file");
    if (!symbol)
        return -ENOENT;
    memcpy(&service->file, &symbol, sizeof(service->file));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_service_close");
    if (!symbol)
        return -ENOENT;
    memcpy(&service->close, &symbol, sizeof(service->close));
    service->client_id = client_id;
    service->sessions = sessions;
    return 0;
}

static int prepare_command(struct ph_gpu_session_service *service,
                           const unsigned char *bytes,
                           size_t size,
                           size_t envelope_size) {
    const kb2_gpu_region_t region = {.region_id = PH_GPU_QUERY_OUTPUT_REGION,
                                     .rights = KB2_GPU_SPAN_RIGHT_WRITE,
                                     .length = sizeof(service->query.output)};
    kb2_gpu_command_t command;
    if (kb2_gpu_command_decode(bytes,
                               size,
                               service->query.generation,
                               KB2_GPU_PROFILE_VIRGL,
                               KB2_GPU_QUEUE_EXECUTION,
                               &region,
                               1,
                               &command))
        return -EPROTO;
    service->session_id = command.session_id;
    service->status = ph_gpu_session_acquire(service->sessions,
                                             service->query.generation,
                                             service->client_id,
                                             service->session_id,
                                             &service->file_cookie);
    if (service->status) {
        if (service->correlation <= service->query.correlation)
            return -EPROTO;
        service->query.correlation = service->correlation;
        return 0;
    }
    service->acquired = 1;
    service->query.session_id = service->session_id;
    return ph_gpu_query_prepare_snapshot(&service->query, envelope_size, service->correlation);
}

int ph_gpu_session_prepare(struct ph_gpu_session_service *service,
                           uint32_t queue_class,
                           size_t size) {
    kb2_protocol_message_envelope_t envelope;
    if (service && service->query.terminal_after_completion)
        return service->query.terminal_after_completion;
    if (!service || service->acquired || size > sizeof(service->query.request) ||
        kb2_protocol_message_envelope_decode(service->query.request, size, &envelope) ||
        envelope.protocol_id != KB2_GPU_PROTOCOL_ID ||
        envelope.generation != service->query.generation || envelope.flags ||
        !envelope.correlation_id ||
        envelope.payload_length != size - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE)
        return -EPROTO;
    service->opcode = envelope.opcode;
    service->correlation = envelope.correlation_id;
    service->status = KB2_GPU_STATUS_OK;
    service->session_id = service->file_cookie = 0;
    service->query.terminal_after_completion = 0;
    service->query.reply_size = 0;
    memset(service->query.output, 0, sizeof(service->query.output));
    const unsigned char *bytes = service->query.request + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE;
    if (service->opcode == KB2_GPU_OPCODE_COMMAND) {
        if (queue_class != KB2_GPU_QUEUE_EXECUTION)
            return -EPROTO;
        return prepare_command(service, bytes, envelope.payload_length, size);
    }
    if (queue_class != KB2_GPU_QUEUE_CONTROL || service->correlation <= service->last_control)
        return -EPROTO;
    service->last_control = service->correlation;
    if (service->opcode == KB2_GPU_OPCODE_SESSION_OPEN) {
        kb2_gpu_session_open_t request;
        if (kb2_gpu_session_open_decode(bytes, envelope.payload_length, &request))
            return -EPROTO;
        if (request.client_id != service->client_id) {
            service->status = KB2_GPU_STATUS_DENIED;
            return 0;
        }
        service->status = ph_gpu_session_open_begin(service->sessions,
                                                    service->query.generation,
                                                    service->client_id,
                                                    request.node_type,
                                                    &service->session_id);
    } else if (service->opcode == KB2_GPU_OPCODE_SESSION_CLOSE) {
        if (kb2_gpu_session_close_decode(bytes, envelope.payload_length, &service->session_id))
            return -EPROTO;
        service->status = ph_gpu_session_close_begin(
            service->sessions, service->query.generation, service->client_id, service->session_id);
    } else {
        return -EPROTO;
    }
    return 0;
}

static int frame_reply(struct ph_gpu_session_service *service, size_t size) {
    kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
                                                .opcode = service->opcode,
                                                .generation = service->query.generation,
                                                .correlation_id = service->correlation,
                                                .payload_length = size};
    service->query.reply_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + size;
    return kb2_protocol_message_envelope_encode(
               service->query.reply, service->query.reply_size, &envelope)
               ? -EPROTO
               : 0;
}

static int command_error(struct ph_gpu_session_service *service) {
    kb2_gpu_inline_completion_t completion = {.session_id = service->session_id,
                                              .status = service->status};
    size_t size;
    if (kb2_gpu_inline_completion_encode(service->query.reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                         sizeof(service->query.reply) -
                                             KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                         &size,
                                         &completion))
        return -EPROTO;
    return frame_reply(service, size);
}

int ph_gpu_session_service_release(struct ph_gpu_session_service *service) {
    if (!service->acquired)
        return 0;
    uint32_t status = ph_gpu_session_release(
        service->sessions, service->query.generation, service->client_id, service->session_id);
    if (status)
        return -EPROTO;
    service->acquired = 0;
    return 0;
}

int ph_gpu_session_dispatch(struct ph_gpu_session_service *service, void *linux_service) {
    if (!linux_service)
        return -ENODEV;
    struct kobox_linux_drm_service *owner = linux_service;
    if (service->opcode == KB2_GPU_OPCODE_COMMAND) {
        ++service->command_count;
        if (service->status)
            return command_error(service);
        struct kobox_linux_drm_file *file = NULL;
        int result = service->file(owner, service->file_cookie, &file);
        if (result || !file) {
            /* A private cookie becoming invalid is an ownership failure, not
             * a peer's missing session. Do not continue this generation. */
            service->status = KB2_GPU_STATUS_DEVICE_LOST;
            service->query.terminal_after_completion = -ENODEV;
            result = command_error(service);
        } else {
            result = service->query_service.dispatch(&service->query, file);
        }
        int released = ph_gpu_session_service_release(service);
        return result ? result : released;
    }
    if (!service->status && service->opcode == KB2_GPU_OPCODE_SESSION_OPEN) {
        uint64_t cookie = 0;
        service->status = file_status(service->open(owner, &cookie));
        if (ph_gpu_session_open_finish(service->sessions,
                                       service->query.generation,
                                       service->client_id,
                                       service->session_id,
                                       cookie,
                                       service->status))
            return -EPROTO;
    } else if (!service->status && service->opcode == KB2_GPU_OPCODE_SESSION_CLOSE) {
        service->status = ph_gpu_session_close_ready(service->sessions,
                                                     service->query.generation,
                                                     service->client_id,
                                                     service->session_id,
                                                     &service->file_cookie);
        if (!service->status) {
            int result = service->close(owner, service->file_cookie);
            /* Any close failure leaves the ledger terminal, including a
             * flush error that already consumed the actual file. */
            service->status = result ? KB2_GPU_STATUS_DEVICE_LOST : KB2_GPU_STATUS_OK;
            if (ph_gpu_session_close_finish(service->sessions,
                                            service->query.generation,
                                            service->client_id,
                                            service->session_id,
                                            service->status))
                return -EPROTO;
        }
    }
    if (service->status == KB2_GPU_STATUS_DEVICE_LOST)
        service->query.terminal_after_completion = -ENODEV;
    kb2_gpu_session_completion_t completion = {
        .opcode = service->opcode, .status = service->status, .session_id = service->session_id};
    if (service->opcode == KB2_GPU_OPCODE_SESSION_OPEN) {
        if (service->status)
            completion.session_id = 0;
        else
            completion.topology_epoch = 1; /* Render-only; no KMS topology is advertised. */
    }
    size_t size;
    if (kb2_gpu_session_completion_encode(service->query.reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                          sizeof(service->query.reply) -
                                              KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                          &size,
                                          &completion))
        return -EPROTO;
    return frame_reply(service, size);
}
