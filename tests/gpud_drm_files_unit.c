/* SPDX-License-Identifier: MIT */
#include "../userland/gpud/drm_control.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void descriptions(void) {
    struct gpud_drm_files files = {0};
    struct gpud_drm_binding binding;
    uint64_t handle, later;
    assert(!gpud_drm_files_init(&files, 9, 2));
    assert(!gpud_drm_file_open_begin(&files, 9, &handle));
    assert(gpud_drm_file_dup(&files, 9, handle) == -EBUSY);
    assert(!gpud_drm_file_open_finish(&files, 9, handle, 101, 0));
    assert(gpud_drm_file_acquire(&files, 8, handle, &binding) == -ESTALE);

    assert(!gpud_drm_file_dup(&files, 9, handle));
    assert(!gpud_drm_file_close(&files, 9, handle));
    assert(!gpud_drm_file_acquire(&files, 9, handle, &binding));
    assert(binding.session_id == 101 && binding.frontend_handle == handle);
    assert(!gpud_drm_file_close(&files, 9, handle));
    assert(!gpud_drm_file_next_close(&files, 9, &binding));
    assert(!gpud_drm_file_release(&files, 9, handle));
    assert(gpud_drm_file_next_close(&files, 9, &binding) == 1 &&
           binding.session_id == 101);
    assert(!gpud_drm_file_next_close(&files, 9, &binding));
    assert(!gpud_drm_file_close_finish(&files, 9, handle, 0));

    assert(!gpud_drm_file_open_begin(&files, 9, &later) && later > handle);
    assert(!gpud_drm_file_open_finish(&files, 9, later, 0, -ENOMEM));
    assert(!gpud_drm_file_open_begin(&files, 9, &later));
    assert(!gpud_drm_file_open_finish(&files, 9, later, 102, 0));
    assert(!gpud_drm_file_close(&files, 9, later));
    assert(gpud_drm_file_next_close(&files, 9, &binding) == 1);
    assert(gpud_drm_file_close_finish(&files, 9, later, -EIO) == -EIO);
    assert(files.files[0].state == GPUD_DRM_FILE_FAILED);
    assert(gpud_drm_file_open_begin(&files, 9, &later) == -EIO);
    assert(gpud_drm_files_fault(&files, 9, -ENOMEM) == -EIO);
}

static void capacity(void) {
    struct gpud_drm_files files = {0};
    uint64_t handles[2], extra;
    assert(!gpud_drm_files_init(&files, 1, 2));
    assert(!gpud_drm_file_open_begin(&files, 1, &handles[0]));
    assert(!gpud_drm_file_open_begin(&files, 1, &handles[1]));
    assert(gpud_drm_file_open_begin(&files, 1, &extra) == -EMFILE);
    assert(!gpud_drm_file_open_finish(&files, 1, handles[0], 0, -ENOMEM));
    assert(!gpud_drm_file_open_begin(&files, 1, &extra) && extra > handles[1]);
    assert(!gpud_drm_file_open_finish(&files, 1, handles[1], 100, 0));
    assert(gpud_drm_file_open_finish(&files, 1, extra, 100, 0) == -EPROTO);
    assert(files.terminal_error == -EPROTO);
}

static size_t make_reply(unsigned char *bytes,
                         const struct gpud_drm_control *pending,
                         uint32_t status,
                         uint64_t session) {
    kb2_gpu_session_completion_t completion = {
        .opcode = pending->opcode,
        .status = status,
        .session_id = session,
        .topology_epoch = !status && pending->opcode == KB2_GPU_OPCODE_SESSION_OPEN ? 1 : 0};
    size_t payload;
    assert(!kb2_gpu_session_completion_encode(bytes + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                              256 - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                              &payload,
                                              &completion));
    kb2_protocol_message_envelope_t envelope = {
        .protocol_id = KB2_GPU_PROTOCOL_ID,
        .opcode = pending->opcode,
        .generation = pending->generation,
        .correlation_id = pending->correlation,
        .payload_length = payload};
    assert(!kb2_protocol_message_envelope_encode(bytes, 256, &envelope));
    return KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + payload;
}

static void control(int defect) {
    struct gpud_drm_files files = {0};
    struct gpud_drm_control pending = {0};
    uint64_t handle = 999;
    unsigned char bytes[256], reply[256];
    size_t size = 777;
    assert(!gpud_drm_files_init(&files, 9, 2));
    gpud_drm_open_request_t request = {.device_minor = 128, .flags = 2 | 04000 | 02000000};
    assert(gpud_drm_open_prepare(&files, 9, 100, 1, &request,
               &pending, bytes, 1, &size) == -EMSGSIZE);
    assert(!pending.handle && !files.handle_sequence && size == 777);
    request.device_minor = 0;
    assert(!gpud_drm_open_prepare(&files, 9, 100, 1, &request,
        &pending, bytes, sizeof(bytes), &size));
    kb2_gpu_session_open_t open;
    assert(!kb2_gpu_session_open_decode(bytes + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                        size - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                        &open));
    assert(open.client_id == 100 && open.node_type == KB2_GPU_NODE_PRIMARY);
    size = make_reply(reply, &pending,
        defect == 1 ? KB2_GPU_STATUS_NO_MEMORY : KB2_GPU_STATUS_OK,
        defect == 1 ? 0 : 101);
    if (defect == 2)
        reply[KB2_PROTOCOL_MESSAGE_ENVELOPE_CORRELATION_ID_OFFSET] ^= 1;
    int result = gpud_drm_control_complete(&files, &pending, reply, size, &handle);
    if (defect == 1) {
        assert(result == -ENOMEM && handle == 999 && !pending.handle && !files.files[0].handle);
        return;
    }
    if (defect == 2) {
        assert(result == -EPROTO && handle == 999 && pending.handle && files.terminal_error);
        return;
    }
    assert(!result && handle && handle != 101 && !pending.handle);
    assert(!gpud_drm_file_close(&files, 9, handle));
    assert(gpud_drm_close_prepare(&files, 9, 2,
        &pending, bytes, sizeof(bytes), &size) == 1);
    uint64_t closed;
    assert(!kb2_gpu_session_close_decode(bytes + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                         size - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                                         &closed) && closed == 101);
    size = make_reply(reply, &pending, KB2_GPU_STATUS_OK, defect == 3 ? 102 : 101);
    result = gpud_drm_control_complete(&files, &pending, reply, size, &closed);
    assert(result == (defect == 3 ? -EPROTO : 0));
    if (defect == 3)
        assert(pending.handle && files.files[0].state == GPUD_DRM_FILE_CLOSING);
    else
        assert(!pending.handle && !files.files[0].handle);
}

int main(void) {
    descriptions();
    capacity();
    for (int defect = 0; defect <= 3; ++defect)
        control(defect);
    puts("GPUD_DRM_FILES_UNIT=PASS OFD dup inflight last-close capacity close-fault control-codec");
    return 0;
}
