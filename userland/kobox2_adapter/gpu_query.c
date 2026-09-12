/* SPDX-License-Identifier: MIT */
#include "gpu_query.h"

#include <errno.h>

static int release(void *context) {
    struct ph_gpu_query *query = context;
    if (!query->mapping) return 0;
    long result = pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP,
        (uintptr_t)query->mapping, PH_GPU_QUERY_VMO_SIZE);
    if (result) return -EIO;
    query->mapping = NULL;
    return 0;
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
    return ph_gpu_query_prepare_snapshot(query, packet->value, packet->correlation);
}

int ph_gpu_query_prepare_snapshot(struct ph_gpu_query *query, size_t size,
    uint64_t expected_correlation) {
    if (!query || size < KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE || size > sizeof(query->request))
        return -EPROTO;
    memset(query->output, 0, sizeof(query->output));
    memset(query->reply, 0, sizeof(query->reply));
    query->terminal_after_completion = 0;
    kb2_protocol_message_envelope_t envelope;
    if (kb2_protocol_message_envelope_decode(query->request, size, &envelope) ||
        envelope.protocol_id != KB2_GPU_PROTOCOL_ID || envelope.opcode != KB2_GPU_OPCODE_COMMAND ||
        envelope.flags || envelope.generation != query->generation ||
        !envelope.correlation_id || envelope.correlation_id <= query->correlation ||
        (expected_correlation && envelope.correlation_id != expected_correlation) ||
        envelope.payload_length != size - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE) return -EPROTO;
    const kb2_gpu_region_t output = {.region_id = PH_GPU_QUERY_OUTPUT_REGION,
        .rights = KB2_GPU_SPAN_RIGHT_WRITE, .length = sizeof(query->output)};
    int result = kobox_drm_query_prepare(&query->plan, query->generation, query->session_id,
        query->request + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, envelope.payload_length, &output);
    if (result) return result;
    query->correlation = envelope.correlation_id;
    return 0;
}

static int dispatch(void *context, void *linux_service) {
    struct ph_gpu_query *query = context;
    size_t size = 0;
    int result = kobox_drm_query_execute(&query->plan, &query->api, linux_service,
        query->output, sizeof(query->output), query->reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
        sizeof(query->reply) - KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, &size);
    if (result) return result;
    kb2_gpu_inline_completion_t completion;
    if (kb2_gpu_inline_completion_decode(query->reply + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
        size, query->session_id, &completion)) return -EPROTO;
    if (completion.status == KB2_GPU_STATUS_DEVICE_LOST || completion.status == KB2_GPU_STATUS_SESSION_LOST)
        query->terminal_after_completion = -ENODEV;
    kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
        .opcode = KB2_GPU_OPCODE_COMMAND, .generation = query->generation,
        .correlation_id = query->correlation, .payload_length = size};
    query->reply_size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + size;
    return kb2_protocol_message_envelope_encode(query->reply, query->reply_size, &envelope) ?
        -EPROTO : 0;
}

static int complete(void *context, struct ph_ipc *ipc) {
    struct ph_gpu_query *query = context;
    if (!query->mapping || !query->reply_size) return -EPROTO;
    memcpy((unsigned char *)query->mapping + PH_GPU_QUERY_OUTPUT_OFFSET,
        query->output, sizeof(query->output));
    memcpy((unsigned char *)query->mapping + PH_GPU_QUERY_REPLY_OFFSET,
        query->reply, query->reply_size);
    struct ph_ipc_packet packet = {.operation = PH_GPU_QUERY_OPERATION,
        .generation = query->generation, .correlation = query->correlation, .value = query->reply_size};
    int result = ph_ipc_send(ipc, &packet);
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
    memset(query, 0, sizeof(*query));
    query->generation = generation;
    query->session_id = session_id;
    query->api = api;
    *service = (struct ph_lifecycle_service){.context = query,
        .prepare = prepare, .dispatch = dispatch, .complete = complete, .release = release};
    return 0;
}
