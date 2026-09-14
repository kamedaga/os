/* SPDX-License-Identifier: MIT */
#include "gpu_query.h"
#include "gpu_queue_message.h"
#include "host.h"
#include "../gpud/gpu_channel.h"

#include <errno.h>
#include <string.h>

int ph_gpu_query_release_aux(struct ph_gpu_query *query) {
    if (!query)
        return -EINVAL;
    if (!query->aux) {
        query->result = (struct kobox_drm_query_result){0};
        return 0;
    }
    if (pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP,
                       (uintptr_t)query->aux,
                       query->aux_mapping_size))
        return -EIO;
    query->aux = NULL;
    query->aux_size = 0;
    query->aux_mapping_size = 0;
    query->result = (struct kobox_drm_query_result){0};
    return 0;
}

static int release(void *context) {
    struct ph_gpu_query *query = context;
    int aux_result = ph_gpu_query_release_aux(query);
    int mapping_result = 0;
    if (query->mapping) {
        if (pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP,
                           (uintptr_t)query->mapping,
                           PH_GPU_QUERY_VMO_SIZE))
            mapping_result = -EIO;
        else
            query->mapping = NULL;
    }
    return aux_result ? aux_result : mapping_result;
}

static int prepare(void *context, const struct ph_ipc_packet *packet) {
    struct ph_gpu_query *query = context;
    if (query->mapping || packet->operation != PH_GPU_QUERY_OPERATION ||
        packet->generation != query->generation || !packet->correlation ||
        packet->correlation <= query->correlation || packet->fd_count != 1 ||
        packet->value < KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE || packet->value > PH_GPU_QUERY_PAGE)
        return -EPROTO;
    const struct pacha_ipc_fd *fd = &packet->fds[0];
    struct pacha_fd_info info = {0};
    if (fd->rights != PH_GPU_QUERY_RIGHTS || fd->flags != PACHA_FD_FLAG_CLOEXEC || fd->transfer_flags)
        return -EACCES;
    if (pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, fd->fd, (uintptr_t)&info)) return -EIO;
    if (info.kind != PACHA_FD_KIND_VMO || info.size != PH_GPU_QUERY_VMO_SIZE ||
        info.rights != PH_GPU_QUERY_RIGHTS || info.flags != PACHA_FD_FLAG_CLOEXEC) return -EACCES;
    long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, fd->fd, 0, PH_GPU_QUERY_VMO_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (address <= 0) return -ENOMEM;
    query->mapping = (void *)(uintptr_t)address;
    /* Snapshot first, then validate only private bytes. The sender may still
     * own a writable VMO alias; no peer memory is read during Linux dispatch. */
    memcpy(query->request, query->mapping, packet->value);
    int result = ph_gpu_query_prepare_snapshot(query, packet->value,
        packet->correlation, KB2_GPU_QUEUE_EXECUTION);
    if (!result && query->plan.aux_input) {
        ph_gpu_query_release_aux(query);
        return -EOPNOTSUPP;
    }
    return result;
}

int ph_gpu_query_prepare_snapshot(struct ph_gpu_query *query, size_t size,
    uint64_t expected_correlation, uint32_t queue_class) {
    if (!query || query->aux ||
        size < KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE || size > sizeof(query->request))
        return -EPROTO;
    memset(query->output, 0, sizeof(query->output));
    memset(query->reply, 0, sizeof(query->reply));
    query->result = (struct kobox_drm_query_result){0};
    query->terminal_after_completion = 0;
    kb2_protocol_message_envelope_t envelope;
    if (kb2_protocol_message_envelope_decode(query->request, size, &envelope) ||
        envelope.protocol_id != KB2_GPU_PROTOCOL_ID || envelope.opcode != KB2_GPU_OPCODE_COMMAND ||
        envelope.flags || envelope.generation != query->generation ||
        !envelope.correlation_id || envelope.correlation_id <= query->correlation ||
        (expected_correlation && envelope.correlation_id != expected_correlation) ||
        envelope.payload_length != size - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE) return -EPROTO;
    const kb2_gpu_region_t region = {.region_id = PH_GPU_QUERY_OUTPUT_REGION,
        .rights = KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE,
        .length = GPUD_GPU_AUX_CAPACITY};
    int result = kobox_drm_query_prepare(&query->plan, query->generation,
        query->session_id, queue_class,
        query->request + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
        envelope.payload_length, &region);
    if (result) return result;
    if (query->plan.aux_size) {
        size_t mapping_size = (query->plan.aux_size + PH_GPU_QUERY_PAGE - 1) &
            ~(size_t)(PH_GPU_QUERY_PAGE - 1);
        if (query->plan.aux_size > GPUD_GPU_AUX_CAPACITY ||
            mapping_size < query->plan.aux_size)
            return -EPROTO;
        long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, 0, 0, mapping_size,
            PACHA_PROT_READ | PACHA_PROT_WRITE,
            PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS, 0);
        if (address < PH_GPU_QUERY_PAGE)
            return -ENOMEM;
        query->aux = (void *)(uintptr_t)address;
        query->aux_size = query->plan.aux_size;
        query->aux_mapping_size = mapping_size;
        memset(query->aux, 0, mapping_size);
    }
    query->correlation = envelope.correlation_id;
    return 0;
}

int ph_gpu_query_dispatch(struct ph_gpu_query *query,
                          struct kobox_linux_drm_service *service,
                          uint64_t file_cookie,
                          struct kobox_linux_drm_file *file) {
    const int mode_map =
        query->plan.command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
        query->plan.command_id == KB2_GPU_DRM_MODE_COMMAND_MAP_DUMB;
    const int map = mode_map ||
        (query->plan.command_set_id == KB2_GPU_DRM_VIRTGPU_SET_ID &&
         query->plan.command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_MAP);
    const int prime_export =
        query->plan.command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
        query->plan.command_id ==
            KB2_GPU_DRM_CORE_COMMAND_PRIME_HANDLE_TO_ATTACHMENT;
    if (map || prime_export) {
        struct kobox_linux_virtgpu_resource_info resource = {
            .bo_handle = map ? query->plan.mapping_handle : query->plan.prime_handle,
        };
        size_t page_count = 1;
        if (!query->api.virtgpu_resource_info(file, &resource)) {
            if (!resource.size || resource.size % PH_PAGE_SIZE)
                return -EPROTO;
            page_count = resource.size / PH_PAGE_SIZE;
        }
        size_t capacity = GPUD_GPU_AUX_CAPACITY / sizeof(uint64_t);
        if (page_count > capacity)
            page_count = capacity;
        size_t bytes = page_count * sizeof(uint64_t);
        size_t mapping_size = (bytes + PH_GPU_QUERY_PAGE - 1) &
            ~(size_t)(PH_GPU_QUERY_PAGE - 1);
        long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, 0, 0, mapping_size,
            PACHA_PROT_READ | PACHA_PROT_WRITE,
            PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS, 0);
        if (address < PH_GPU_QUERY_PAGE)
            return -ENOMEM;
        query->aux = (void *)(uintptr_t)address;
        query->aux_size = bytes;
        query->aux_mapping_size = mapping_size;
        query->plan.mapping_page_capacity = page_count;
        query->plan.aux_size = bytes;
        memset(query->aux, 0, mapping_size);
    }
    size_t size = 0;
    int result = kobox_drm_query_execute_service(&query->plan, &query->api,
        service, file_cookie, file,
        query->output, sizeof(query->output), query->aux, query->aux_size,
        query->reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
        sizeof(query->reply) - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, &size,
        &query->result);
    if (result) return result;
    if (query->result.mapping_id &&
        query->result.attachment_class == KB2_GPU_ATTACHMENT_MEMORY) {
        kb2_gpu_virtgpu_map_completion_t completion;
        kb2_protocol_status_t decoded = mode_map ?
            kb2_gpu_mode_map_completion_decode(
                query->reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, size,
                query->session_id, query->generation, &completion) :
            kb2_gpu_virtgpu_map_completion_decode(
                query->reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, size,
                query->session_id, query->generation, &completion);
        if (decoded ||
            completion.mapping_id != query->result.mapping_id ||
            completion.length != query->result.mapping_length ||
            completion.rights != query->result.mapping_rights)
            return -EPROTO;
    } else if (query->result.mapping_id &&
               query->result.attachment_class == KB2_GPU_ATTACHMENT_DMA_BUF) {
        kb2_gpu_attachment_completion_t completion;
        if (kb2_gpu_attachment_completion_decode(
                query->reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                size, query->session_id, query->generation, &completion) ||
            completion.argument_id !=
                KB2_GPU_DRM_CORE_COMMAND_PRIME_HANDLE_TO_ATTACHMENT_COMPLETION_ATTACHMENT_DMA_BUFFER_ARGUMENT_ID ||
            completion.attachment.object_class != KB2_GPU_ATTACHMENT_DMA_BUF ||
            completion.attachment.exchange_id != query->result.mapping_id ||
            completion.attachment.rights != query->result.mapping_rights ||
            completion.attachment.role !=
                KB2_GPU_DRM_CORE_COMMAND_PRIME_HANDLE_TO_ATTACHMENT_COMPLETION_ATTACHMENT_DMA_BUFFER_ROLE ||
            completion.attachment.ownership != KB2_GPU_ATTACHMENT_MOVE)
            return -EPROTO;
    } else if (query->result.mapping_id) {
        return -EPROTO;
    } else {
        kb2_gpu_inline_completion_t completion;
        if (kb2_gpu_inline_completion_decode(
                query->reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                size, query->session_id, &completion))
            return -EPROTO;
        if (completion.status == KB2_GPU_STATUS_DEVICE_LOST ||
            completion.status == KB2_GPU_STATUS_SESSION_LOST)
            query->terminal_after_completion = -ENODEV;
    }
    kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
        .opcode = KB2_GPU_OPCODE_COMMAND, .generation = query->generation,
        .correlation_id = query->correlation, .payload_length = size};
    query->reply_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + size;
    return kb2_protocol_message_envelope_encode(query->reply, query->reply_size, &envelope) ?
        -EPROTO : 0;
}

static int dispatch(void *context, void *linux_file) {
    return ph_gpu_query_dispatch(context, NULL, 0, linux_file);
}

int ph_gpu_query_publish_attachment(struct ph_gpu_query *query,
                                    struct ph_ipc *ipc) {
    if (!query || !ipc)
        return -EINVAL;
    if (!query->result.mapping_id)
        return 0;
    if (query->result.attachment_class != KB2_GPU_ATTACHMENT_MEMORY &&
        query->result.attachment_class != KB2_GPU_ATTACHMENT_DMA_BUF)
        return -EPROTO;
    if (!query->aux || !query->result.mapping_page_count ||
        query->result.mapping_page_count > query->plan.mapping_page_capacity ||
        query->result.mapping_length !=
            query->result.mapping_page_count * PH_PAGE_SIZE)
        return -EPROTO;
    const uint64_t *pages = query->aux;
    for (uint64_t index = 0; index < query->result.mapping_page_count; ++index)
        if (pages[index] >= PH_RAM_SIZE / PH_PAGE_SIZE)
            return -EPROTO;
    uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_REVOKE;
    if (query->result.mapping_rights & KB2_GPU_SPAN_RIGHT_READ)
        rights |= PACHA_FD_RIGHT_MAP_READ;
    if (query->result.mapping_rights & KB2_GPU_SPAN_RIGHT_WRITE)
        rights |= PACHA_FD_RIGHT_MAP_WRITE;
    long native_fd = pacha_syscall5(
        PACHA_FD_SYSCALL_VMO_CREATE_PAGE_VIEW,
        (uint64_t)(uint32_t)ph_core.ram_fd, (uintptr_t)pages,
        query->result.mapping_page_count, rights, PACHA_FD_FLAG_CLOEXEC);
    if (native_fd < 16) {
        switch (native_fd) {
        case PACHA_SYSCALL_ERR_NOT_READY:
        case PACHA_SYSCALL_ERR_EMPTY:
            return -EAGAIN;
        case PACHA_SYSCALL_ERR_ALLOC:
            return -ENOMEM;
        case PACHA_SYSCALL_ERR_MAP:
            return -EIO;
        case PACHA_SYSCALL_ERR_CLOSED:
            return -EBADF;
        default:
            return -EINVAL;
        }
    }
    int fd = (int)native_fd;
    struct ph_ipc_packet packet = {
        .operation = query->result.attachment_class == KB2_GPU_ATTACHMENT_MEMORY ?
            PH_GPU_MAPPING_ATTACHMENT : PH_GPU_DMA_BUF_ATTACHMENT,
        .generation = query->generation,
        .correlation = query->correlation,
        .value = query->result.mapping_id,
        .fd_count = 1,
        .fds = {{.fd = (uint64_t)fd, .rights = rights,
                 .flags = PACHA_FD_FLAG_CLOEXEC}},
    };
    int result = ph_ipc_send(ipc, &packet);
    int closed = pacha_syscall1(PACHA_FD_SYSCALL_CLOSE,
        (uint64_t)(uint32_t)fd) ? -EIO : 0;
    return result ? result : closed;
}

static int complete(void *context, struct ph_ipc *ipc) {
    struct ph_gpu_query *query = context;
    if (!query->mapping || !query->reply_size) return -EPROTO;
    int result = ph_gpu_query_publish_attachment(query, ipc);
    if (result) return result;
    memcpy((unsigned char *)query->mapping + PH_GPU_QUERY_OUTPUT_OFFSET,
        query->output, sizeof(query->output));
    memcpy((unsigned char *)query->mapping + PH_GPU_QUERY_REPLY_OFFSET,
        query->reply, query->reply_size);
    struct ph_ipc_packet packet = {.operation = PH_GPU_QUERY_OPERATION,
        .generation = query->generation, .correlation = query->correlation, .value = query->reply_size};
    result = ph_ipc_send(ipc, &packet);
    /* Publish the accepted request's failure before ending admission. A lost
     * render device must not remain available for another query in this generation. */
    return result ? result : query->terminal_after_completion;
}

int ph_gpu_query_init(struct ph_gpu_query *query, uint64_t generation, uint64_t session_id,
    struct ph_lifecycle_service *service) {
    if (!query || query->generation || !generation || !service) return -EINVAL;
    struct kobox_drm_query_api api;
    void *symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_version");
    if (!symbol) return -ENOENT;
    memcpy(&api.version, &symbol, sizeof(api.version));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_get_cap");
    if (!symbol) return -ENOENT;
    memcpy(&api.get_cap, &symbol, sizeof(api.get_cap));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_set_client_cap");
    if (!symbol) return -ENOENT;
    memcpy(&api.set_client_cap, &symbol, sizeof(api.set_client_cap));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_master");
    if (!symbol) return -ENOENT;
    memcpy(&api.master, &symbol, sizeof(api.master));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_resources");
    if (!symbol) return -ENOENT;
    memcpy(&api.resources, &symbol, sizeof(api.resources));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_connector");
    if (!symbol) return -ENOENT;
    memcpy(&api.connector, &symbol, sizeof(api.connector));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_encoder");
    if (!symbol) return -ENOENT;
    memcpy(&api.encoder, &symbol, sizeof(api.encoder));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_set_crtc");
    if (!symbol) return -ENOENT;
    memcpy(&api.set_crtc, &symbol, sizeof(api.set_crtc));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_page_flip");
    if (!symbol) return -ENOENT;
    memcpy(&api.page_flip, &symbol, sizeof(api.page_flip));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_create_dumb");
    if (!symbol) return -ENOENT;
    memcpy(&api.create_dumb, &symbol, sizeof(api.create_dumb));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_add_fb2");
    if (!symbol) return -ENOENT;
    memcpy(&api.add_fb2, &symbol, sizeof(api.add_fb2));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_poll_events");
    if (!symbol) return -ENOENT;
    memcpy(&api.poll_events, &symbol, sizeof(api.poll_events));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_read_events");
    if (!symbol) return -ENOENT;
    memcpy(&api.read_events, &symbol, sizeof(api.read_events));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_gem_close");
    if (!symbol) return -ENOENT;
    memcpy(&api.gem_close, &symbol, sizeof(api.gem_close));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_service_prime_export");
    if (!symbol) return -ENOENT;
    memcpy(&api.prime_export, &symbol, sizeof(api.prime_export));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_service_prime_import");
    if (!symbol) return -ENOENT;
    memcpy(&api.prime_import, &symbol, sizeof(api.prime_import));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_syncobj_create");
    if (!symbol) return -ENOENT;
    memcpy(&api.syncobj_create, &symbol, sizeof(api.syncobj_create));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_syncobj_destroy");
    if (!symbol) return -ENOENT;
    memcpy(&api.syncobj_destroy, &symbol, sizeof(api.syncobj_destroy));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_syncobj_wait");
    if (!symbol) return -ENOENT;
    memcpy(&api.syncobj_wait, &symbol, sizeof(api.syncobj_wait));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_syncobj_array");
    if (!symbol) return -ENOENT;
    memcpy(&api.syncobj_array, &symbol, sizeof(api.syncobj_array));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_virtgpu_getparam");
    if (!symbol) return -ENOENT;
    memcpy(&api.virtgpu_getparam, &symbol, sizeof(api.virtgpu_getparam));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_virtgpu_get_caps");
    if (!symbol) return -ENOENT;
    memcpy(&api.virtgpu_get_caps, &symbol, sizeof(api.virtgpu_get_caps));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_virtgpu_context_init");
    if (!symbol) return -ENOENT;
    memcpy(&api.virtgpu_context_init, &symbol, sizeof(api.virtgpu_context_init));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_virtgpu_execbuffer");
    if (!symbol) return -ENOENT;
    memcpy(&api.virtgpu_execbuffer, &symbol, sizeof(api.virtgpu_execbuffer));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_virtgpu_resource_create");
    if (!symbol) return -ENOENT;
    memcpy(&api.virtgpu_resource_create, &symbol, sizeof(api.virtgpu_resource_create));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_virtgpu_resource_info");
    if (!symbol) return -ENOENT;
    memcpy(&api.virtgpu_resource_info, &symbol, sizeof(api.virtgpu_resource_info));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_virtgpu_transfer");
    if (!symbol) return -ENOENT;
    memcpy(&api.virtgpu_transfer, &symbol, sizeof(api.virtgpu_transfer));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_virtgpu_wait");
    if (!symbol) return -ENOENT;
    memcpy(&api.virtgpu_wait, &symbol, sizeof(api.virtgpu_wait));
    symbol = ph_image_lookup(&ph_core, "kobox_linux_drm_service_map");
    if (!symbol) return -ENOENT;
    memcpy(&api.virtgpu_map, &symbol, sizeof(api.virtgpu_map));
    memset(query, 0, sizeof(*query));
    query->generation = generation;
    query->session_id = session_id;
    query->api = api;
    *service = (struct ph_lifecycle_service){.context = query,
        .prepare = prepare, .dispatch = dispatch, .complete = complete, .release = release};
    return 0;
}
