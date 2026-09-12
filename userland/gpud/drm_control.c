/* SPDX-License-Identifier: MIT */
#include "drm_control.h"
#include "drm_reply.h"

#include <errno.h>
#include <string.h>

static int encode_envelope(
    unsigned char *bytes, size_t size, uint32_t opcode, uint64_t generation, uint64_t correlation) {
    kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
                                                .opcode = opcode,
                                                .generation = generation,
                                                .correlation_id = correlation,
                                                .payload_length =
                                                    size - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE};
    return kb2_protocol_message_envelope_encode(bytes, size, &envelope) ? -EPROTO : 0;
}

int gpud_drm_open_prepare(struct gpud_drm_files *files,
                          uint64_t generation,
                          uint64_t backend_client,
                          uint64_t correlation,
                          const drmd_open_request_t *request,
                          struct gpud_drm_control *pending,
                          unsigned char *bytes,
                          size_t capacity,
                          size_t *size_out) {
    if (!request || !pending || pending->handle || !bytes || !size_out || !backend_client ||
        !correlation)
        return -EINVAL;
    /* Linux userspace flags, not native fd flags. NONBLOCK/CLOEXEC are LPR
     * description/descriptor policy, never raw kernel-open flag forwarding. */
    const uint64_t access_mask = 3, read_write = 2;
    const uint64_t nonblock = 04000, largefile = 0100000, cloexec = 02000000;
    if (request->device_minor != 128)
        return -EOPNOTSUPP;
    if ((request->flags & access_mask) != read_write ||
        request->flags & ~(access_mask | nonblock | largefile | cloexec))
        return -EOPNOTSUPP;
    size_t size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + KB2_GPU_SESSION_OPEN_REQUEST_SIZE;
    if (capacity < size)
        return -EMSGSIZE;
    unsigned char encoded[KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + KB2_GPU_SESSION_OPEN_REQUEST_SIZE];
    kb2_gpu_session_open_t open = {.node_type = KB2_GPU_NODE_RENDER, .client_id = backend_client};
    if (encode_envelope(encoded, size, KB2_GPU_OPCODE_SESSION_OPEN, generation, correlation) ||
        kb2_gpu_session_open_encode(
            encoded + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, KB2_GPU_SESSION_OPEN_REQUEST_SIZE, &open))
        return -EPROTO;
    uint64_t handle;
    int error = gpud_drm_file_open_begin(files, generation, &handle);
    if (error)
        return error;
    *pending = (struct gpud_drm_control){.generation = generation,
                                         .correlation = correlation,
                                         .handle = handle,
                                         .opcode = KB2_GPU_OPCODE_SESSION_OPEN};
    memcpy(bytes, encoded, size);
    *size_out = size;
    return 0;
}

int gpud_drm_close_prepare(struct gpud_drm_files *files,
                           uint64_t generation,
                           uint64_t correlation,
                           struct gpud_drm_control *pending,
                           unsigned char *bytes,
                           size_t capacity,
                           size_t *size_out) {
    if (!pending || pending->handle || !bytes || !size_out || !correlation)
        return -EINVAL;
    size_t size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + KB2_GPU_SESSION_CLOSE_REQUEST_SIZE;
    if (capacity < size)
        return -EMSGSIZE;
    struct gpud_drm_binding binding;
    int result = gpud_drm_file_next_close(files, generation, &binding);
    if (result != 1)
        return result;
    *pending = (struct gpud_drm_control){.generation = generation,
                                         .correlation = correlation,
                                         .handle = binding.frontend_handle,
                                         .session = binding.session_id,
                                         .opcode = KB2_GPU_OPCODE_SESSION_CLOSE};
    if (encode_envelope(bytes, size, pending->opcode, generation, correlation) ||
        kb2_gpu_session_close_encode(bytes + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                     KB2_GPU_SESSION_CLOSE_REQUEST_SIZE,
                                     binding.session_id))
        return gpud_drm_files_fault(files, generation, -EPROTO);
    *size_out = size;
    return 1;
}

int gpud_drm_control_complete(struct gpud_drm_files *files,
                              struct gpud_drm_control *pending,
                              const unsigned char *reply,
                              size_t size,
                              uint64_t *handle_out) {
    if (!files || !pending || !pending->handle || !handle_out)
        return -EINVAL;
    kb2_protocol_message_envelope_t envelope;
    kb2_gpu_session_completion_t completion;
    if (kb2_protocol_message_envelope_decode(reply, size, &envelope) ||
        envelope.protocol_id != KB2_GPU_PROTOCOL_ID || envelope.opcode != pending->opcode ||
        envelope.generation != pending->generation ||
        envelope.correlation_id != pending->correlation || envelope.flags ||
        envelope.payload_length != size - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE ||
        kb2_gpu_session_completion_decode(reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                          envelope.payload_length,
                                          pending->opcode,
                                          &completion))
        return gpud_drm_files_fault(files, pending->generation, -EPROTO);
    int error = gpud_drm_status_errno(completion.status);
    if (completion.status == KB2_GPU_STATUS_LIMIT)
        error = -EMFILE;
    int result;
    if (pending->opcode == KB2_GPU_OPCODE_SESSION_OPEN) {
        result = gpud_drm_file_open_finish(
            files, pending->generation, pending->handle, completion.session_id, error);
    } else if (pending->opcode == KB2_GPU_OPCODE_SESSION_CLOSE) {
        /* The private pending operation must bind the expected backend ID,
         * not merely accept any well-formed nonzero CLOSE session. */
        if (!pending->session || completion.session_id != pending->session)
            return gpud_drm_files_fault(files, pending->generation, -EPROTO);
        result = gpud_drm_file_close_finish(files, pending->generation, pending->handle, error);
    } else {
        return gpud_drm_files_fault(files, pending->generation, -EPROTO);
    }
    if (result && result != error)
        return gpud_drm_files_fault(files, pending->generation, result);
    if (!error)
        *handle_out = pending->handle;
    memset(pending, 0, sizeof(*pending));
    if (completion.status == KB2_GPU_STATUS_DEVICE_LOST ||
        completion.status == KB2_GPU_STATUS_SESSION_LOST ||
        completion.status == KB2_GPU_STATUS_STALE_GENERATION)
        return gpud_drm_files_fault(files, files->generation, error);
    return error;
}
