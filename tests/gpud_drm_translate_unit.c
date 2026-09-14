/* SPDX-License-Identifier: MIT */
/* Actual GPU encoder/decoder, with LPR's existing native request layout.
 * This checks frontend translation, not DRM execution or IPC authorization. */
#include "../userland/gpud/drm_translate.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const struct gpud_drm_binding binding = {.generation = 7,
    .frontend_handle = 51, .session_id = 103};

static uint64_t read_u64(const unsigned char *bytes) {
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; ++i) value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

static kb2_gpu_command_t decode_queue(
    const struct gpud_drm_translation *translation, uint32_t queue_class) {
    kb2_gpu_command_t command;
    assert(translation->generation == binding.generation);
    assert(translation->frontend_handle == binding.frontend_handle);
    assert(translation->session_id == binding.session_id);
    assert(kb2_gpu_command_decode(translation->command, translation->command_size,
        binding.generation, KB2_GPU_PROFILE_VIRGL, queue_class,
        &translation->region, translation->region.region_id ? 1 : 0,
        &command) == KB2_PROTOCOL_OK);
    assert(command.session_id == binding.session_id && command.generation == binding.generation);
    assert(translation->queue_class == queue_class);
    assert(!command.flags && !command.deadline_ns);
    assert(!kb2_gpu_command_attachment_count(&command));
    return command;
}

static kb2_gpu_command_t decode(const struct gpud_drm_translation *translation) {
    return decode_queue(translation, KB2_GPU_QUEUE_EXECUTION);
}

static void expect_failure(gpud_drm_ioctl_request_t *request, int error) {
    struct gpud_drm_translation output;
    memset(&output, 0xa5, sizeof(output));
    struct gpud_drm_translation before = output;
    assert(gpud_drm_ioctl_encode(&output, &binding, request, 9) == error);
    assert(!memcmp(&output, &before, sizeof(output)));
}

static void scalar_commands(void) {
    gpud_drm_ioctl_request_t request = {.handle = 51, .request = 0xc010640c,
        .arg_size = 16, .data_size = 16};
    request.data[0] = 12;
    memset(request.data + 8, 0xab, 8); /* Caller output garbage is not an input. */
    struct gpud_drm_translation translation = {0};
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, 0));
    kb2_gpu_command_t command = decode(&translation);
    assert(command.command_id == KB2_GPU_DRM_CORE_COMMAND_GET_CAP);
    size_t size;
    const unsigned char *data = kb2_gpu_command_inline_data(&command, &size);
    assert(size == 8 && read_u64(data) == 12 && !command.counts[1]);
    request.request = 0x4010640d;
    memset(request.data + 8, 0, 8);
    request.data[8] = 1;
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, 0));
    command = decode(&translation);
    data = kb2_gpu_command_inline_data(&command, &size);
    assert(command.command_id == KB2_GPU_DRM_CORE_COMMAND_SET_CLIENT_CAP);
    assert(size == 16 && read_u64(data) == 12 && read_u64(data + 8) == 1);
    request.request = 0x40086409;
    request.arg_size = request.data_size = 8;
    request.data[0] = 5;
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, 0));
    command = decode(&translation);
    data = kb2_gpu_command_inline_data(&command, &size);
    assert(command.command_id == KB2_GPU_DRM_CORE_COMMAND_GEM_CLOSE && size == 8 && read_u64(data) == 5);
    request.data[4] = 1;
    expect_failure(&request, -EINVAL);
}

static void version_command(void) {
    gpud_drm_ioctl_request_t request = {.handle = 51, .request = 0xc0406400,
        .arg_size = 64, .data_size = sizeof(gpud_drm_version_wire_t)};
    gpud_drm_version_wire_t version = {.name_capacity = UINT64_MAX, .date_capacity = 0, .desc_capacity = 3};
    memcpy(request.data, &version, sizeof(version));
    struct gpud_drm_translation translation = {0};
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, 9));
    kb2_gpu_command_t command = decode(&translation);
    assert(command.command_id == KB2_GPU_DRM_CORE_COMMAND_VERSION);
    assert(command.counts[0] == 3 && command.counts[1] == 2);
    const unsigned int capacity[] = {GPUD_DRM_VERSION_NAME_BYTES, 0, 3};
    const unsigned int offset[] = {0, GPUD_DRM_VERSION_NAME_BYTES, GPUD_DRM_VERSION_NAME_BYTES + GPUD_DRM_VERSION_DATE_BYTES};
    size_t present = 0;
    for (size_t i = 0; i < 3; ++i) {
        kb2_gpu_span_t span;
        kb2_gpu_argument_t argument;
        assert(translation.version_capacity[i] == capacity[i]);
        if (!capacity[i]) continue;
        assert(kb2_gpu_command_span(&command, present, &span) == KB2_PROTOCOL_OK);
        assert(kb2_gpu_command_argument(&command, ++present, &argument) == KB2_PROTOCOL_OK);
        assert(span.span_id == i + 1 && span.region_id == 9 && span.offset == offset[i]);
        assert(span.length == capacity[i] && span.element_count == capacity[i]);
        assert(span.rights == KB2_GPU_SPAN_RIGHT_WRITE && span.flags == KB2_GPU_SPAN_FLAG_OUTPUT);
        assert(argument.value == span.span_id && argument.count == capacity[i]);
    }
    memset(request.data, 0xff, sizeof(request.data)); /* Output borrows no frontend storage. */
    (void)decode(&translation);
    assert(gpud_drm_ioctl_encode(&translation, &binding, &request, 0) == -EINVAL);
    memset(request.data, 0, sizeof(request.data));
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, 0));
    command = decode(&translation);
    assert(command.counts[0] == 1 && !command.counts[1] && !translation.region.region_id);
}

static void malformed_requests(void) {
    gpud_drm_ioctl_request_t request = {.handle = 51, .request = 0xc010640c,
        .arg_size = 16, .data_size = 16};
    request.handle = 52; expect_failure(&request, -EACCES); request.handle = 51;
    request.request |= UINT64_C(1) << 32; expect_failure(&request, -EINVAL);
    request.request = 0x8010640c; expect_failure(&request, -EOPNOTSUPP); /* Wrong direction. */
    request.request = 0xc008640c; expect_failure(&request, -EOPNOTSUPP); /* Wrong ABI size. */
    request.request = 0xc010640c;
    request.arg_size = 15; expect_failure(&request, -EINVAL); request.arg_size = 16;
    request.data_size = 15; expect_failure(&request, -EINVAL); request.data_size = 16;
    request.reserved0 = 1; expect_failure(&request, -EINVAL); request.reserved0 = 0;
    request.aux_size = 4096; expect_failure(&request, -EOPNOTSUPP); request.aux_size = 0;
    request.fd_flags = GPUD_DRM_IOCTL_FD_INPUT_WAIT; expect_failure(&request, -EOPNOTSUPP);
}

static void virtgpu_input_commands(void) {
    gpud_drm_ioctl_request_t request = {
        .handle = 51, .request = GPUD_DRM_IOCTL_VIRTGPU_EXECBUFFER,
        .arg_size = sizeof(gpud_drm_virtgpu_execbuffer_t),
        .data_size = sizeof(gpud_drm_virtgpu_execbuffer_t), .aux_size = 12};
    gpud_drm_virtgpu_execbuffer_t exec = {
        .size = 4, .bo_handles = 8, .num_bo_handles = 1, .fence_fd = -1};
    memcpy(request.data, &exec, sizeof(exec));
    struct gpud_drm_translation translation = {0};
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, 9));
    kb2_gpu_command_t command = decode(&translation);
    assert(command.command_set_id == KB2_GPU_DRM_VIRTGPU_SET_ID &&
        command.command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_EXECBUFFER &&
        command.counts[0] == 3 && command.counts[1] == 2 &&
        translation.region.rights == KB2_GPU_SPAN_RIGHT_READ &&
        translation.region.length == request.aux_size);
    kb2_gpu_span_t span;
    assert(!kb2_gpu_command_span(&command, 0, &span) && !span.offset && span.length == 4);
    assert(!kb2_gpu_command_span(&command, 1, &span) && span.offset == 8 && span.length == 4);
    exec.fence_fd = 0;
    memcpy(request.data, &exec, sizeof(exec));
    expect_failure(&request, -EINVAL);

    gpud_drm_virtgpu_context_wire_t context = {
        .parameter_mask = GPUD_DRM_VIRTGPU_CONTEXT_HAS_CAPSET_ID, .capset_id = 1};
    request = (gpud_drm_ioctl_request_t) {
        .handle = 51, .request = GPUD_DRM_IOCTL_VIRTGPU_CONTEXT_INIT,
        .arg_size = sizeof(gpud_drm_virtgpu_context_init_t),
        .data_size = sizeof(context)};
    memcpy(request.data, &context, sizeof(context));
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, 9));
    command = decode(&translation);
    assert(command.command_set_id == KB2_GPU_DRM_VIRTGPU_SET_ID &&
        command.command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_CONTEXT_INIT &&
        command.counts[0] == 1 && !command.counts[1] &&
        !translation.region.region_id);
    context.parameter_mask = GPUD_DRM_VIRTGPU_CONTEXT_HAS_POLL_RINGS_MASK;
    context.poll_ring_mask = 1;
    memcpy(request.data, &context, sizeof(context));
    expect_failure(&request, -EINVAL);
}

static void dumb_buffer_commands(void) {
    gpud_drm_ioctl_request_t request = {
        .handle = 51,
        .request = GPUD_DRM_IOCTL_MODE_CREATE_DUMB,
        .arg_size = sizeof(gpud_drm_mode_create_dumb_t),
        .data_size = sizeof(gpud_drm_mode_create_dumb_t),
    };
    gpud_drm_mode_create_dumb_t create = {
        .height = 61,
        .width = 79,
        .bpp = 32,
        .handle = UINT32_MAX,
        .pitch = UINT32_MAX,
        .size = UINT64_MAX,
    };
    memcpy(request.data, &create, sizeof(create));
    struct gpud_drm_translation translation = {0};
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, 0));
    kb2_gpu_command_t command = decode_queue(
        &translation, KB2_GPU_QUEUE_DISPLAY);
    size_t size;
    const unsigned char *data = kb2_gpu_command_inline_data(&command, &size);
    assert(command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
        command.command_id == KB2_GPU_DRM_MODE_COMMAND_CREATE_DUMB &&
        size == KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_REQUEST_SIZE);
    assert(data[0] == 61 && data[4] == 79 && data[8] == 32 && !data[12]);

    create.height = 0;
    memcpy(request.data, &create, sizeof(create));
    expect_failure(&request, -EINVAL);
    create.height = 61;
    create.flags = 1;
    memcpy(request.data, &create, sizeof(create));
    expect_failure(&request, -EINVAL);

    gpud_drm_mode_map_dumb_t map = {
        .handle = 27,
        .offset = UINT64_MAX,
    };
    request.request = GPUD_DRM_IOCTL_MODE_MAP_DUMB;
    request.arg_size = request.data_size = sizeof(map);
    memcpy(request.data, &map, sizeof(map));
    assert(!gpud_drm_ioctl_encode(&translation, &binding, &request, 0));
    command = decode_queue(&translation, KB2_GPU_QUEUE_DISPLAY);
    data = kb2_gpu_command_inline_data(&command, &size);
    assert(command.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
        command.command_id == KB2_GPU_DRM_MODE_COMMAND_MAP_DUMB &&
        size == KB2_GPU_DRM_MODE_RECORD_MAP_REQUEST_SIZE);
    assert(data[0] == 27 && data[4] ==
        (KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE));
    map.pad = 1;
    memcpy(request.data, &map, sizeof(map));
    expect_failure(&request, -EINVAL);
}

int main(void) {
    scalar_commands();
    version_command();
    malformed_requests();
    virtgpu_input_commands();
    dumb_buffer_commands();
    puts("gpud LPR DRM request translation: PASS canonical GPU commands (not service execution)");
    return 0;
}
