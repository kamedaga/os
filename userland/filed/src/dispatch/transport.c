#include "common.h"
#include "kobox/device_pachaos_capsule.h"
#include "linux_personality/linux_block.h"
#include "linux_subsystem/dma/dma.h"
#include "linux_subsystem/fs/fs.h"

static void filed_dispatch_dump_metrics(filed_runtime_t *runtime)
{
    filed_dump_dispatch_metrics(runtime);
    filed_dump_cache_metrics(runtime);
    filed_exec_linux_lpr_dump_metrics();
    filed_kobox_backend_dump_metrics(&runtime->backend);
    filed_file_vmo_storage_profile_dump();
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    kb_fs_read_profile_t fs_profile;
    kb_linux_block_profile_t block_profile;
    kb_pachaos_capsule_dma_profile_t dma_profile;
    kb_pachaos_capsule_irq_profile_t irq_profile;
    kb_subsystem_dma_window_profile_t dma_window_profile;
    kb_fs_read_profile_snapshot(&fs_profile);
    kb_linux_block_profile_snapshot(&block_profile);
    kb_pachaos_capsule_dma_profile_snapshot(&dma_profile);
    kb_pachaos_capsule_irq_profile_snapshot(&irq_profile);
    kb_subsystem_dma_window_profile_snapshot(&dma_window_profile);
    fprintf(stderr,
        "FILED_STORAGE_PROFILE scope=fs calls=%llu bytes=%llu total_cycles=%llu "
        "extent_calls=%llu extent_cycles=%llu device_calls=%llu device_cycles=%llu "
        "overlay_calls=%llu overlay_cycles=%llu partial_copy_calls=%llu partial_copy_cycles=%llu\n",
        (unsigned long long)fs_profile.calls,
        (unsigned long long)fs_profile.bytes,
        (unsigned long long)fs_profile.total_cycles,
        (unsigned long long)fs_profile.extent_lookup_calls,
        (unsigned long long)fs_profile.extent_lookup_cycles,
        (unsigned long long)fs_profile.device_read_calls,
        (unsigned long long)fs_profile.device_read_cycles,
        (unsigned long long)fs_profile.overlay_calls,
        (unsigned long long)fs_profile.overlay_cycles,
        (unsigned long long)fs_profile.partial_copy_calls,
        (unsigned long long)fs_profile.partial_copy_cycles);
    fprintf(stderr,
        "FILED_STORAGE_PROFILE scope=block bytes=%llu "
        "alloc_calls=%llu alloc_cycles=%llu map_calls=%llu map_cycles=%llu "
        "before_calls=%llu before_cycles=%llu submit_calls=%llu submit_cycles=%llu "
        "wait_calls=%llu wait_cycles=%llu unmap_calls=%llu unmap_cycles=%llu "
        "free_calls=%llu free_cycles=%llu total_calls=%llu total_cycles=%llu "
        "cq_poll_calls=%llu cq_poll_cycles=%llu irq_wait_calls=%llu irq_wait_cycles=%llu "
        "poll_yield_calls=%llu poll_yield_cycles=%llu "
        "post_irq_calls=%llu post_irq_cycles=%llu "
        "prp_alloc_calls=%llu prp_alloc_cycles=%llu "
        "data_map_calls=%llu data_map_cycles=%llu "
        "prp_build_calls=%llu prp_build_cycles=%llu "
        "prp_aux_map_calls=%llu prp_aux_map_cycles=%llu "
        "prp_cache_hit_calls=%llu prp_cache_hit_cycles=%llu "
        "prp_cache_miss_calls=%llu prp_cache_miss_cycles=%llu "
        "prp_cache_fallback_calls=%llu prp_cache_fallback_cycles=%llu\n",
        (unsigned long long)block_profile.disk_read_bytes,
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_REQUEST_ALLOC],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_REQUEST_ALLOC],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_DMA_MAP],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_DMA_MAP],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_BEFORE_EXECUTE],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_BEFORE_EXECUTE],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_QUEUE_SUBMIT],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_QUEUE_SUBMIT],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_COMPLETION_WAIT],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_COMPLETION_WAIT],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_DMA_UNMAP_COPYBACK],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_DMA_UNMAP_COPYBACK],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_REQUEST_FREE_TOTAL],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_REQUEST_FREE_TOTAL],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_DISK_IO_TOTAL],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_DISK_IO_TOTAL],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_CQ_POLL],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_CQ_POLL],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_IRQ_WAIT],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_IRQ_WAIT],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_POLL_YIELD],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_POLL_YIELD],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_POST_IRQ_DRAIN],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_POST_IRQ_DRAIN],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_PRP_ALLOC_INIT],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_ALLOC_INIT],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_DATA_MAP_PAGES],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_DATA_MAP_PAGES],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_PRP_BUILD],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_BUILD],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_PRP_AUX_MAP],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_AUX_MAP],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_HIT],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_HIT],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_MISS],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_MISS],
        (unsigned long long)block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_FALLBACK],
        (unsigned long long)block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_FALLBACK]);
    fprintf(stderr,
        "FILED_STORAGE_PROFILE scope=dma_copy calls=%llu bytes=%llu cycles=%llu\n",
        (unsigned long long)dma_profile.copy_back_calls,
        (unsigned long long)dma_profile.copy_back_bytes,
        (unsigned long long)dma_profile.copy_back_cycles);
    fprintf(stderr,
        "FILED_STORAGE_PROFILE scope=irq wait_calls=%llu wait_cycles=%llu "
        "fd_wait_calls=%llu fd_wait_cycles=%llu fd_wait_ready=%llu "
        "poll_calls=%llu poll_cycles=%llu poll_ready=%llu "
        "pre_poll_calls=%llu pre_poll_cycles=%llu pre_poll_ready=%llu "
        "post_poll_calls=%llu post_poll_cycles=%llu post_poll_ready=%llu "
        "handler_calls=%llu handler_cycles=%llu\n",
        (unsigned long long)irq_profile.wait_calls,
        (unsigned long long)irq_profile.wait_cycles,
        (unsigned long long)irq_profile.fd_wait_calls,
        (unsigned long long)irq_profile.fd_wait_cycles,
        (unsigned long long)irq_profile.fd_wait_ready,
        (unsigned long long)irq_profile.poll_calls,
        (unsigned long long)irq_profile.poll_cycles,
        (unsigned long long)irq_profile.poll_ready,
        (unsigned long long)irq_profile.pre_poll_calls,
        (unsigned long long)irq_profile.pre_poll_cycles,
        (unsigned long long)irq_profile.pre_poll_ready,
        (unsigned long long)irq_profile.post_poll_calls,
        (unsigned long long)irq_profile.post_poll_cycles,
        (unsigned long long)irq_profile.post_poll_ready,
        (unsigned long long)irq_profile.handler_calls,
        (unsigned long long)irq_profile.handler_cycles);
    fprintf(stderr,
        "FILED_STORAGE_PROFILE scope=dma_window begin_calls=%llu mapping_calls=%llu "
        "mapping_cycles=%llu reuse_calls=%llu reuse_cycles=%llu "
        "end_calls=%llu mapped_bytes=%llu direct_mapping_calls=%llu "
        "direct_mapping_cycles=%llu direct_mapped_bytes=%llu staged_read_calls=%llu "
        "staged_bytes=%llu staging_copy_cycles=%llu\n",
        (unsigned long long)dma_window_profile.begin_calls,
        (unsigned long long)dma_window_profile.mapping_calls,
        (unsigned long long)dma_window_profile.mapping_cycles,
        (unsigned long long)dma_window_profile.reuse_calls,
        (unsigned long long)dma_window_profile.reuse_cycles,
        (unsigned long long)dma_window_profile.end_calls,
        (unsigned long long)dma_window_profile.mapped_bytes,
        (unsigned long long)dma_window_profile.direct_mapping_calls,
        (unsigned long long)dma_window_profile.direct_mapping_cycles,
        (unsigned long long)dma_window_profile.direct_mapped_bytes,
        (unsigned long long)dma_window_profile.staged_read_calls,
        (unsigned long long)dma_window_profile.staged_bytes,
        (unsigned long long)dma_window_profile.staging_copy_cycles);
#endif
#if defined(FILED_STARTUP_PROFILE) && FILED_STARTUP_PROFILE
    const uint64_t stage_cycles_a =
        pacha_trace_name_id("filed.file_vmo.stage.cycles.a");
    const uint64_t stage_cycles_b =
        pacha_trace_name_id("filed.file_vmo.stage.cycles.b");
    const uint64_t stage_cycles_c =
        pacha_trace_name_id("filed.file_vmo.stage.cycles.c");
    const uint64_t stage_counts_a =
        pacha_trace_name_id("filed.file_vmo.stage.counts.a");
    const uint64_t stage_counts_b =
        pacha_trace_name_id("filed.file_vmo.stage.counts.b");
    const uint64_t stage_counts_c =
        pacha_trace_name_id("filed.file_vmo.stage.counts.c");
    pacha_trace4(PACHA_TRACE_COMPONENT_FILED,
        PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA, PACHA_TRACE_CLASS_METRIC,
        stage_cycles_a,
        filed_file_vmo_stage_cycles[FILED_PROFILE_FILE_VMO_STAGE_PREPARE],
        filed_file_vmo_stage_cycles[FILED_PROFILE_FILE_VMO_STAGE_LOOKUP],
        filed_file_vmo_stage_cycles[FILED_PROFILE_FILE_VMO_STAGE_CREATE_TOTAL]);
    pacha_trace4(PACHA_TRACE_COMPONENT_FILED,
        PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA, PACHA_TRACE_CLASS_METRIC,
        stage_cycles_b,
        filed_file_vmo_stage_cycles[FILED_PROFILE_FILE_VMO_STAGE_VMO_CREATE],
        filed_file_vmo_stage_cycles[FILED_PROFILE_FILE_VMO_STAGE_VMO_MMAP],
        filed_file_vmo_stage_cycles[FILED_PROFILE_FILE_VMO_STAGE_PREAD]);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED,
        PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA, PACHA_TRACE_CLASS_METRIC,
        stage_cycles_c,
        filed_file_vmo_stage_cycles[FILED_PROFILE_FILE_VMO_STAGE_REPLY]);
    pacha_trace4(PACHA_TRACE_COMPONENT_FILED,
        PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA, PACHA_TRACE_CLASS_METRIC,
        stage_counts_a,
        filed_file_vmo_stage_counts[FILED_PROFILE_FILE_VMO_STAGE_PREPARE],
        filed_file_vmo_stage_counts[FILED_PROFILE_FILE_VMO_STAGE_LOOKUP],
        filed_file_vmo_stage_counts[FILED_PROFILE_FILE_VMO_STAGE_CREATE_TOTAL]);
    pacha_trace4(PACHA_TRACE_COMPONENT_FILED,
        PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA, PACHA_TRACE_CLASS_METRIC,
        stage_counts_b,
        filed_file_vmo_stage_counts[FILED_PROFILE_FILE_VMO_STAGE_VMO_CREATE],
        filed_file_vmo_stage_counts[FILED_PROFILE_FILE_VMO_STAGE_VMO_MMAP],
        filed_file_vmo_stage_counts[FILED_PROFILE_FILE_VMO_STAGE_PREAD]);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED,
        PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA, PACHA_TRACE_CLASS_METRIC,
        stage_counts_c,
        filed_file_vmo_stage_counts[FILED_PROFILE_FILE_VMO_STAGE_REPLY]);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED,
        PACHA_TRACE_EVENT_FILED_METRIC_FILE_VMO,
        PACHA_TRACE_CLASS_METRIC, 1, filed_file_vmo_cache_hits);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED,
        PACHA_TRACE_EVENT_FILED_METRIC_FILE_VMO,
        PACHA_TRACE_CLASS_METRIC, 2, filed_file_vmo_cache_misses);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED,
        PACHA_TRACE_EVENT_FILED_METRIC_FILE_VMO,
        PACHA_TRACE_CLASS_METRIC, 3, filed_file_vmo_cache_stores);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED,
        PACHA_TRACE_EVENT_FILED_METRIC_FILE_VMO,
        PACHA_TRACE_CLASS_METRIC, 4, filed_file_vmo_cache_evictions);
    pacha_trace_dump_ring();
#endif
}

int filed_dispatch_runtime_init(filed_runtime_t *runtime)
{
    if (runtime == NULL) {
        return -1;
    }
    if (runtime->dispatch_state != NULL) {
        memset(runtime->dispatch_state, 0, sizeof(*runtime->dispatch_state));
    } else {
        runtime->dispatch_state = calloc(1, sizeof(*runtime->dispatch_state));
        if (runtime->dispatch_state == NULL) {
            return -12;
        }
    }
#if defined(FILED_STARTUP_PROFILE) && FILED_STARTUP_PROFILE
    pacha_trace_set_masks(
        PACHA_TRACE_COMPONENT_BIT(PACHA_TRACE_COMPONENT_FILED),
        PACHA_TRACE_CLASS_ERROR | PACHA_TRACE_CLASS_METRIC);
#endif
    return 0;
}

static filed_page_dispatch_result_t filed_dispatch_session_page(
    filed_runtime_t *runtime,
    const struct pacha_ipc_msg *request,
    void *page)
{
    switch (request->word1) {
    case FILED_OP_DIAG_PING:
        return filed_page_result(0, request->word2);
    case FILED_OP_DIAG_DUMP_METRICS:
        filed_dispatch_dump_metrics(runtime);
        return filed_page_result(0, 0);
    case FILED_OP_VFS_OPENAT:
        return filed_dispatch_openat_page(runtime, page);
    case FILED_OP_VFS_VALIDATE_OPEN_CACHE:
        return filed_dispatch_validate_open_cache_page(runtime, page);
    case FILED_OP_VFS_STAT:
        return filed_dispatch_stat_page(runtime, page);
    case FILED_OP_VFS_UTIMENS:
        return filed_dispatch_utimens_page(runtime, page);
    case FILED_OP_VFS_CHMOD:
        return filed_dispatch_chmod_page(runtime, page);
    case FILED_OP_VFS_PREAD:
        return filed_dispatch_pread_page(runtime, page);
    case FILED_OP_VFS_READ:
        return filed_dispatch_read_page(runtime, page);
    case FILED_OP_VFS_PREAD_TO_VMO:
        return filed_page_result(-95, 0);
    case FILED_OP_VFS_PWRITE:
        return filed_dispatch_pwrite_page(runtime, page);
    case FILED_OP_VFS_WRITE:
        return filed_dispatch_write_page(runtime, page);
    case FILED_OP_VFS_TRUNCATE:
        return filed_dispatch_truncate_page(runtime, page);
    case FILED_OP_VFS_MEMFD_CREATE:
        return filed_dispatch_memfd_create_page(runtime, page);
    case FILED_OP_VFS_UNLINK:
        return filed_dispatch_unlink_page(runtime, page);
    case FILED_OP_VFS_RENAME:
        return filed_dispatch_rename_page(runtime, page);
    case FILED_OP_VFS_MKDIR:
        return filed_dispatch_mkdir_page(runtime, page);
    case FILED_OP_VFS_MKNOD:
        return filed_dispatch_mknod_page(runtime, page);
    case FILED_OP_VFS_RMDIR:
        return filed_dispatch_rmdir_page(runtime, page);
    case FILED_OP_VFS_SYMLINK:
        return filed_dispatch_symlink_page(runtime, page);
    case FILED_OP_VFS_READLINK:
        return filed_dispatch_readlink_page(runtime, page);
    case FILED_OP_VFS_LINK:
        return filed_dispatch_link_page(runtime, page);
    case FILED_OP_VFS_GETDENTS:
        return filed_dispatch_getdents_page(runtime, page);
    case FILED_OP_VFS_DUP:
        return filed_dispatch_dup_page(runtime, page);
    case FILED_OP_VFS_GET_FLAGS:
        return filed_dispatch_get_flags_page(runtime, page);
    case FILED_OP_VFS_SET_FLAGS:
        return filed_dispatch_set_flags_page(runtime, page);
    case FILED_OP_VFS_FSYNC:
        return filed_dispatch_fsync_page(runtime, request);
    case FILED_OP_VFS_SEEK:
        return filed_dispatch_seek_page(runtime, page);
    case FILED_OP_EXEC_PATH:
        return filed_dispatch_exec_path_session_page(runtime, page);
    case FILED_OP_EXEC_SELF:
        return filed_page_result(-95, 0);
    case FILED_OP_VFS_CLOSE:
        return filed_dispatch_close_page(runtime, request);
    case FILED_OP_VFS_SYNC_ALL: {
        const int status = filed_dispatch_sync_all(runtime);
        filed_dispatch_log_state_checkpoint(runtime, "client_sync");
        return filed_page_result(status, 0);
    }
    case FILED_OP_SERVICE_SET_NETD_SOCKET:
    case FILED_OP_SERVICE_SET_TERMD_TTY:
    case FILED_OP_SERVICE_SET_DRMD_DRM:
    case FILED_OP_SERVICE_SET_INPUTD_INPUT:
        return filed_page_result(-95, 0);
    default:
        return filed_page_result(-95, 0);
    }
}

static int filed_session_fast_validate(
    const filed_session_t *session,
    filed_fast_header_t **out_header,
    filed_fast_request_t **out_requests,
    filed_fast_completion_t **out_completions)
{
    if (session == NULL ||
        session->page == NULL ||
        session->page_size < FILED_SESSION_PAGE_BYTES ||
        out_header == NULL ||
        out_requests == NULL ||
        out_completions == NULL)
    {
        return -22;
    }

    filed_fast_header_t *header = (filed_fast_header_t *)session->page;
    if (header->magic != FILED_FAST_MAGIC ||
        header->version != FILED_FAST_VERSION ||
        header->request_capacity != FILED_FAST_REQUEST_CAPACITY ||
        header->completion_capacity != FILED_FAST_COMPLETION_CAPACITY ||
        header->payload_slot_count != FILED_FAST_PAYLOAD_SLOT_COUNT ||
        header->payload_slot_size != FILED_PAGE_BYTES ||
        header->payload_offset != FILED_FAST_PAYLOAD_OFFSET ||
        header->generation_offset != FILED_FAST_GENERATION_OFFSET ||
        header->generation_capacity != FILED_FAST_GENERATION_CAPACITY ||
        header->payload_offset + header->payload_slot_count * header->payload_slot_size > session->page_size)
    {
        return -71;
    }

    *out_header = header;
    *out_requests = (filed_fast_request_t *)((uint8_t *)session->page + sizeof(*header));
    *out_completions = (filed_fast_completion_t *)((uint8_t *)(*out_requests) +
        sizeof(**out_requests) * header->request_capacity);
    return 0;
}

static void *filed_session_fast_payload(
    const filed_session_t *session,
    const filed_fast_header_t *header,
    uint64_t payload_slot)
{
    if (session == NULL ||
        header == NULL ||
        payload_slot >= header->payload_slot_count ||
        header->payload_slot_size != FILED_PAGE_BYTES)
    {
        return NULL;
    }
    const uint64_t offset = header->payload_offset + payload_slot * header->payload_slot_size;
    if (offset + FILED_PAGE_BYTES > session->page_size) {
        return NULL;
    }
    return (uint8_t *)session->page + offset;
}

static filed_page_dispatch_result_t filed_dispatch_session_write_batch(
    filed_runtime_t *runtime,
    filed_session_t *session,
    filed_fast_header_t *header,
    uint64_t batch_count,
    bool append)
{
    if (batch_count == 0 || batch_count > header->payload_slot_count) {
        return filed_page_result(-22, 0);
    }

    uint64_t total = 0;
    int64_t status = 0;
    const uint64_t op = append ? FILED_OP_VFS_WRITE : FILED_OP_VFS_PWRITE;
    for (uint64_t slot = 0; slot < batch_count; slot++) {
        void *payload = filed_session_fast_payload(session, header, slot);
        if (payload == NULL) {
            status = -22;
            break;
        }

        filed_io_t *io = (filed_io_t *)payload;
        const uint64_t requested = io->length;
        const uint64_t start_cycles = filed_read_tsc();
        const filed_page_dispatch_result_t result = append ?
            filed_dispatch_write_page(runtime, payload) :
            filed_dispatch_pwrite_page(runtime, payload);
        filed_record_dispatch_metric_cycles(
            runtime,
            op,
            start_cycles,
            filed_read_tsc(),
            result.status);

        if (result.status != 0) {
            status = result.status;
            break;
        }
        total += result.result;
        if (result.result < requested) {
            break;
        }
    }
    return filed_page_result(status, total);
}

static uint64_t filed_dispatch_session_fast_drain(
    filed_runtime_t *runtime,
    filed_session_t *session,
    int *out_status)
{
    filed_fast_header_t *header = NULL;
    filed_fast_request_t *requests = NULL;
    filed_fast_completion_t *completions = NULL;
    uint64_t completed = 0;
    int status = filed_session_fast_validate(session, &header, &requests, &completions);
    if (status != 0) {
        if (out_status != NULL) {
            *out_status = status;
        }
        return 0;
    }

    while (header->request_head != header->request_tail) {
        if (header->completion_tail - header->completion_head >= header->completion_capacity) {
            filed_fast_metrics.ring_full++;
            break;
        }

        filed_fast_request_t *fast_request =
            &requests[header->request_head % header->request_capacity];
        const uint64_t fast_op_start_cycles = filed_read_tsc();
        void *payload = filed_session_fast_payload(session, header, fast_request->payload_slot);
        filed_page_dispatch_result_t result = filed_page_result(-22, 0);
        if (payload != NULL && fast_request->request_id != 0) {
            struct pacha_ipc_msg pseudo_request;
            memset(&pseudo_request, 0, sizeof(pseudo_request));
            pseudo_request.word0 = PACHA_SERVICE_REQUEST_MAGIC;
            pseudo_request.word1 = fast_request->opcode;
            pseudo_request.word2 = fast_request->word2;
            pseudo_request.word3 = fast_request->request_id;

            const uint64_t start_cycles = filed_read_tsc();
            if (fast_request->opcode == FILED_OP_VFS_PWRITE_BATCH ||
                fast_request->opcode == FILED_OP_VFS_WRITE_BATCH)
            {
                result = filed_dispatch_session_write_batch(
                    runtime,
                    session,
                    header,
                    fast_request->word2,
                    fast_request->opcode == FILED_OP_VFS_WRITE_BATCH);
                filed_record_dispatch_metric_cycles(
                    runtime,
                    pseudo_request.word1,
                    start_cycles,
                    filed_read_tsc(),
                    result.status);
            } else {
                result = filed_dispatch_session_page(runtime, &pseudo_request, payload);
                (void)filed_maintain_vnode_cache(runtime);
                if (result.status == 0 && result.result != 0 &&
                    (fast_request->opcode == FILED_OP_VFS_OPENAT ||
                     fast_request->opcode == FILED_OP_VFS_DUP ||
                     fast_request->opcode == FILED_OP_VFS_MEMFD_CREATE))
                {
                    const uint32_t owner_session =
                        (uint32_t)(session - runtime->sessions) + 1u;
                    const filed_status_t owner_status = filed_vfs_set_handle_owner(
                        &runtime->vfs,
                        (filed_handle_id_t)(uint32_t)result.result,
                        owner_session);
                    if (owner_status != FILED_OK) {
                        (void)filed_close_handle_runtime(
                            runtime, (filed_handle_id_t)(uint32_t)result.result);
                        result = filed_page_result(-5, 0);
                    }
                }
                filed_record_dispatch_metric_cycles(
                    runtime,
                    pseudo_request.word1,
                    start_cycles,
                    filed_read_tsc(),
                    result.status);
            }
        }

        filed_fast_completion_t *completion =
            &completions[header->completion_tail % header->completion_capacity];
        memset(completion, 0, sizeof(*completion));
        completion->request_id = fast_request->request_id;
        completion->status = result.status;
        completion->result = result.result;
        completion->bytes = result.result;
        __sync_synchronize();
        header->completion_tail++;
        header->request_head++;
        completed++;
        filed_record_fast_op_metric_cycles(
            runtime,
            fast_request->opcode,
            fast_op_start_cycles,
            filed_read_tsc(),
            result.status);
    }

    header->completion_seq += completed;
    if (completed != 0) {
        filed_fast_metrics.batches++;
        filed_fast_metrics.enqueued += completed;
        filed_fast_metrics.completed += completed;
    }
    if (out_status != NULL) {
        *out_status = 0;
    }
    return completed;
}

int filed_dispatch_session_once(filed_runtime_t *runtime, uint64_t session_index)
{
    if (runtime == NULL || session_index >= FILED_RUNTIME_MAX_SESSIONS) {
        return -1;
    }
    filed_session_t *session = &runtime->sessions[session_index];
    if (!session->active || session->channel_fd < 16 || session->page == NULL) {
        return -1;
    }

    struct pacha_ipc_msg request;
    memset(&request, 0, sizeof(request));
    const uint64_t recv_start_ns = filed_now_ns();
    const uint64_t recv_start_cycles = filed_read_tsc();
    const int recv_status = pacha_ipc_recv(session->channel_fd, &request);
    const uint64_t recv_end_cycles = filed_read_tsc();
    const uint64_t recv_end_ns = filed_now_ns();
    if (recv_status != 0) {
        return recv_status;
    }
    const int request_is =
        request.word0 == PACHA_SERVICE_REQUEST_MAGIC &&
        request.word1 == FILED_OP_SESSION_DOORBELL &&
        request.word3 != 0;
    if (!request_is) {
        return filed_send_session_reply(session->channel_fd, request.word3, -22, 0);
    }

    int drain_status = 0;
    const uint64_t drain_start_ns = filed_now_ns();
    const uint64_t drain_start_cycles = filed_read_tsc();
    const uint64_t completed = filed_dispatch_session_fast_drain(runtime, session, &drain_status);
    const uint64_t drain_end_cycles = filed_read_tsc();
    const uint64_t drain_end_ns = filed_now_ns();
    const uint64_t reply_start_ns = filed_now_ns();
    const uint64_t reply_start_cycles = filed_read_tsc();
    const int reply_status = filed_send_session_reply(
        session->channel_fd,
        request.word3,
        drain_status,
        completed);
    const uint64_t reply_end_cycles = filed_read_tsc();
    const uint64_t reply_end_ns = filed_now_ns();

    filed_fast_metrics.doorbells++;
    if (recv_end_ns >= recv_start_ns) {
        const uint64_t elapsed = recv_end_ns - recv_start_ns;
        filed_fast_metrics.recv_total_ns += elapsed;
        if (elapsed > filed_fast_metrics.recv_max_ns) {
            filed_fast_metrics.recv_max_ns = elapsed;
        }
    }
    if (recv_start_cycles != 0 && recv_end_cycles >= recv_start_cycles) {
        const uint64_t elapsed = recv_end_cycles - recv_start_cycles;
        filed_fast_metrics.recv_total_cycles += elapsed;
        if (elapsed > filed_fast_metrics.recv_max_cycles) {
            filed_fast_metrics.recv_max_cycles = elapsed;
        }
    }
    if (drain_end_ns >= drain_start_ns) {
        const uint64_t elapsed = drain_end_ns - drain_start_ns;
        filed_fast_metrics.drain_total_ns += elapsed;
        if (elapsed > filed_fast_metrics.drain_max_ns) {
            filed_fast_metrics.drain_max_ns = elapsed;
        }
    }
    if (drain_start_cycles != 0 && drain_end_cycles >= drain_start_cycles) {
        const uint64_t elapsed = drain_end_cycles - drain_start_cycles;
        filed_fast_metrics.drain_total_cycles += elapsed;
        if (elapsed > filed_fast_metrics.drain_max_cycles) {
            filed_fast_metrics.drain_max_cycles = elapsed;
        }
    }
    if (reply_end_ns >= reply_start_ns) {
        const uint64_t elapsed = reply_end_ns - reply_start_ns;
        filed_fast_metrics.reply_total_ns += elapsed;
        if (elapsed > filed_fast_metrics.reply_max_ns) {
            filed_fast_metrics.reply_max_ns = elapsed;
        }
    }
    if (reply_start_cycles != 0 && reply_end_cycles >= reply_start_cycles) {
        const uint64_t elapsed = reply_end_cycles - reply_start_cycles;
        filed_fast_metrics.reply_total_cycles += elapsed;
        if (elapsed > filed_fast_metrics.reply_max_cycles) {
            filed_fast_metrics.reply_max_cycles = elapsed;
        }
    }
    return reply_status;
}

static void filed_close_received_fds_except3(
    const struct pacha_ipc_msg *request,
    int keep_fd,
    int keep_fd2,
    int keep_fd3)
{
    if (request == NULL || request->fds == NULL) {
        return;
    }
    for (uint64_t i = 0; i < request->fd_count; i++) {
        const int fd = (int)(uint32_t)request->fds[i].fd;
        if (fd >= 16 && fd != keep_fd && fd != keep_fd2 && fd != keep_fd3) {
            (void)pacha_fd_close(fd);
        }
    }
}

static void filed_close_received_fds_except(
    const struct pacha_ipc_msg *request,
    int keep_fd,
    int keep_fd2)
{
    filed_close_received_fds_except3(request, keep_fd, keep_fd2, -1);
}

typedef struct filed_route_result {
    int replied;
    int reply_status;
    int keep_fd;
    int64_t status;
    uint64_t result;
} filed_route_result_t;

static filed_route_result_t filed_route_pending(void)
{
    filed_route_result_t result;
    memset(&result, 0, sizeof(result));
    result.keep_fd = -1;
    return result;
}

static filed_route_result_t filed_route_replied(int reply_status)
{
    filed_route_result_t result = filed_route_pending();
    result.replied = 1;
    result.reply_status = reply_status;
    return result;
}

static filed_route_result_t filed_dispatch_client_vfs(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request,
    void *page,
    const pacha_service_envelope_t *header)
{
    filed_route_result_t route = filed_route_pending();
    void *payload = (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES;

    switch (header->op) {
    case FILED_OP_VFS_OPENAT:
        if (header->payload_size < sizeof(filed_path_request_t)) {
            route.status = -22;
        } else {
            filed_path_request_t *path = (filed_path_request_t *)payload;
            filed_openat_t openat;
            memset(&openat, 0, sizeof(openat));
            openat.dir_handle = path->dir_handle;
            openat.rights = path->rights;
            openat.open_flags = path->flags;
            openat.create_mode = path->create_mode_or_result_kind;
            memcpy(openat.name, path->path, sizeof(openat.name));
            const filed_page_dispatch_result_t open_result =
                filed_dispatch_openat_page(runtime, &openat);
            route.status = open_result.status;
            route.result = open_result.result;
            path->create_mode_or_result_kind = openat.opened_kind;
        }
        break;
    case FILED_OP_VFS_CLOSE:
        if (header->payload_size < sizeof(filed_handle_request_t)) {
            route.status = -22;
        } else {
            const filed_handle_request_t *handle = (const filed_handle_request_t *)payload;
            const filed_handle_id_t handle_id = (filed_handle_id_t)(uint32_t)handle->handle;
            route.status = handle_id == runtime->root_handle_id ?
                -13 :
                filed_close_handle_runtime(runtime, handle_id);
        }
        break;
    case FILED_OP_VFS_STAT:
        if (header->payload_size < sizeof(filed_statx_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_stat_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_READ:
        if (header->payload_size < sizeof(filed_io_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_read_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_PREAD:
        if (header->payload_size < sizeof(filed_io_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_pread_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_WRITE:
        if (header->payload_size < sizeof(filed_io_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_write_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_PWRITE:
        if (header->payload_size < sizeof(filed_io_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_pwrite_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_GETDENTS:
        if (header->payload_size < sizeof(filed_getdents_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_getdents_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_SEEK:
        if (header->payload_size < sizeof(filed_seek_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_seek_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_DUP:
        if (header->payload_size < sizeof(filed_handle_flags_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_dup_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_GET_FLAGS:
        if (header->payload_size < sizeof(filed_handle_flags_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_get_flags_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_SET_FLAGS:
        if (header->payload_size < sizeof(filed_handle_flags_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_set_flags_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_FSYNC:
        if (header->payload_size < sizeof(filed_handle_request_t)) {
            route.status = -22;
        } else {
            const filed_handle_request_t *handle = (const filed_handle_request_t *)payload;
            struct pacha_ipc_msg pseudo_request;
            memset(&pseudo_request, 0, sizeof(pseudo_request));
            pseudo_request.word2 = handle->handle;
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_fsync_page(runtime, &pseudo_request);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_TRUNCATE:
        if (header->payload_size < sizeof(filed_truncate_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_truncate_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_UNLINK:
        if (header->payload_size < sizeof(filed_unlink_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_unlink_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_RENAME:
        if (header->payload_size < sizeof(filed_rename_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_rename_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_MKDIR:
        if (header->payload_size < sizeof(filed_mkdir_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_mkdir_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_MKNOD:
        if (header->payload_size < sizeof(filed_mknod_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_mknod_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_RMDIR:
        if (header->payload_size < sizeof(filed_rmdir_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_rmdir_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_SYMLINK:
        if (header->payload_size < sizeof(filed_symlink_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_symlink_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_READLINK:
        if (header->payload_size < sizeof(filed_readlink_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_readlink_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_LINK:
        if (header->payload_size < sizeof(filed_link_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_link_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_UTIMENS:
        if (header->payload_size < sizeof(filed_utimens_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_utimens_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_CHMOD:
        if (header->payload_size < sizeof(filed_chmod_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_chmod_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_MEMFD_CREATE:
        if (header->payload_size < sizeof(filed_memfd_create_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_memfd_create_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_FILE_VMO: {
        const int reply_status = filed_dispatch_file_vmo(
            runtime,
            reply_fd,
            request,
            page,
            header);
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
        filed_close_received_fds_except(request, reply_fd, -1);
        return filed_route_replied(reply_status);
    }
    case FILED_OP_VFS_SHARED_FILE_VMO: {
        const int reply_status = filed_dispatch_shared_file_vmo(
            runtime,
            reply_fd,
            request,
            page,
            header);
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
        filed_close_received_fds_except(request, reply_fd, -1);
        return filed_route_replied(reply_status);
    }
    case FILED_OP_VFS_PREAD_TO_VMO:
        if (header->payload_size < sizeof(filed_pread_vmo_t) ||
            request->fd_count < 2 ||
            request->fds == NULL ||
            request->fds[1].fd < 16)
        {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_pread_to_vmo_page(runtime, payload, (int)request->fds[1].fd);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_OP_VFS_SYNC_ALL:
        route.status = filed_dispatch_sync_all(runtime);
        filed_dispatch_log_state_checkpoint(runtime, "client_sync");
        break;
    default:
        route.status = -95;
        break;
    }
    return route;
}

static int filed_dispatch_client(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    if (runtime == NULL ||
        request == NULL ||
        request->fds == NULL ||
        request->fd_count < 2 ||
        request->fds[0].fd < 16 ||
        request->fds[0].fd == (uint64_t)(uint32_t)reply_fd)
    {
        pacha_service_envelope_t header;
        memset(&header, 0, sizeof(header));
        header.magic = PACHA_SERVICE_REQUEST_MAGIC;
        header.abi_version = PACHA_SERVICE_ABI_VERSION;
        header.service_id = FILED_SERVICE_ID;
        header.request_id = request != NULL ? request->word3 : 0;
        header.trace_id = request != NULL ? request->word3 : 0;
        filed_close_received_fds_except(request, reply_fd, -1);
        return filed_send_reply(reply_fd, NULL, &header, -22, 0, 0);
    }

    const int page_fd = (int)(uint32_t)request->fds[0].fd;
    void *page = pacha_mmap(
        page_fd,
        PACHA_SERVICE_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (page == NULL) {
        filed_close_received_fds_except(request, reply_fd, -1);
        const uint64_t token = filed_error_token(
            -5,
            0,
            PACHA_STATUS_STAGE_MAP_PAGE,
            -5,
            request->word3,
            request->fd_count,
            (uint64_t)(uint32_t)page_fd,
            0,
            "filed request page map failed");
        pacha_service_envelope_t header;
        memset(&header, 0, sizeof(header));
        header.magic = PACHA_SERVICE_REQUEST_MAGIC;
        header.abi_version = PACHA_SERVICE_ABI_VERSION;
        header.service_id = FILED_SERVICE_ID;
        header.request_id = request->word3;
        header.trace_id = request->word3;
        return filed_send_reply(reply_fd, NULL, &header, -5, 0, token);
    }

    pacha_service_envelope_t header;
    memcpy(&header, page, sizeof(header));
    if (!pacha_service_request_is_valid(&header, FILED_SERVICE_ID) ||
        header.request_id == 0 ||
        header.request_id != request->word3)
    {
        pacha_service_reply_init(
            (pacha_service_envelope_t *)page,
            &header,
            -22,
            PACHA_SERVICE_ERROR_ABI,
            0,
            0);
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
        filed_close_received_fds_except(request, reply_fd, -1);
        return filed_send_reply(reply_fd, NULL, &header, -22, 0, 0);
    }

    int keep_fd = -1;
    int64_t status = 0;
    uint64_t result = 0;
    uint64_t error_token = 0;
    switch (header.op) {
    case FILED_OP_HELLO:
        result = PACHA_SERVICE_ABI_VERSION;
        break;
    case FILED_OP_SESSION_OPEN: {
        int keep_session_channel_fd = -1;
        int keep_session_page_fd = -1;
        const int reply_status = filed_dispatch_session_open(
            runtime,
            reply_fd,
            request,
            page,
            &header,
            &keep_session_channel_fd,
            &keep_session_page_fd);
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
        filed_close_received_fds_except3(
            request,
            keep_session_channel_fd,
            keep_session_page_fd,
            reply_fd);
        return reply_status;
    }
    case FILED_OP_DIAG_ERROR_GET:
        status = PACHA_STATUS_ENOTSUP;
        break;
    case FILED_OP_VFS_TRANSFER_DUP: {
        if (header.payload_size < sizeof(filed_handle_flags_t) ||
            request->fd_count != 3 || request->fds[1].fd < 16)
        {
            status = -22;
            break;
        }
        void *payload = (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES;
        const int lease_fd = (int)(uint32_t)request->fds[1].fd;
        const filed_page_dispatch_result_t dup =
            filed_dispatch_dup_page(runtime, payload);
        status = dup.status;
        result = dup.result;
        if (status == 0) {
            const filed_status_t lease_status = filed_vfs_set_handle_lease(
                &runtime->vfs, (filed_handle_id_t)result, lease_fd);
            if (lease_status == FILED_OK) {
                keep_fd = lease_fd;
            } else {
                (void)filed_close_handle_runtime(
                    runtime, (filed_handle_id_t)result);
                status = filed_status_to_wire(lease_status);
                result = 0;
            }
        }
        break;
    }
    case FILED_OP_VFS_OPENAT:
    case FILED_OP_VFS_CLOSE:
    case FILED_OP_VFS_STAT:
    case FILED_OP_VFS_READ:
    case FILED_OP_VFS_PREAD:
    case FILED_OP_VFS_WRITE:
    case FILED_OP_VFS_PWRITE:
    case FILED_OP_VFS_GETDENTS:
    case FILED_OP_VFS_SEEK:
    case FILED_OP_VFS_DUP:
    case FILED_OP_VFS_GET_FLAGS:
    case FILED_OP_VFS_SET_FLAGS:
    case FILED_OP_VFS_FSYNC:
    case FILED_OP_VFS_TRUNCATE:
    case FILED_OP_VFS_UNLINK:
    case FILED_OP_VFS_RENAME:
    case FILED_OP_VFS_MKDIR:
    case FILED_OP_VFS_MKNOD:
    case FILED_OP_VFS_RMDIR:
    case FILED_OP_VFS_SYMLINK:
    case FILED_OP_VFS_READLINK:
    case FILED_OP_VFS_LINK:
    case FILED_OP_VFS_UTIMENS:
    case FILED_OP_VFS_CHMOD:
    case FILED_OP_VFS_FILE_VMO:
    case FILED_OP_VFS_SHARED_FILE_VMO:
    case FILED_OP_VFS_MEMFD_CREATE:
    case FILED_OP_VFS_PREAD_TO_VMO:
    case FILED_OP_VFS_SYNC_ALL: {
        const filed_route_result_t route = filed_dispatch_client_vfs(
            runtime,
            reply_fd,
            request,
            page,
            &header);
        (void)filed_maintain_vnode_cache(runtime);
        if (route.replied) {
            return route.reply_status;
        }
        keep_fd = route.keep_fd;
        status = route.status;
        result = route.result;
        break;
    }
    case FILED_OP_EXEC_PATH:
        if (header.payload_size < sizeof(filed_exec_path_t)) {
            status = -22;
        } else {
            memmove(page, (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES, sizeof(filed_exec_path_t));
            struct pacha_ipc_msg exec_request = *request;
            exec_request.word0 = PACHA_SERVICE_REQUEST_MAGIC;
            exec_request.word1 = FILED_OP_EXEC_PATH;
            exec_request.word2 = 0;
            exec_request.word3 = header.request_id;
            (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
            return filed_dispatch_exec_path(runtime, reply_fd, &exec_request);
        }
        break;
    case FILED_OP_EXEC_SELF:
        if (header.payload_size < sizeof(filed_exec_path_t)) {
            status = -22;
        } else {
            memmove(page, (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES, sizeof(filed_exec_path_t));
            struct pacha_ipc_msg exec_request = *request;
            exec_request.word0 = PACHA_SERVICE_REQUEST_MAGIC;
            exec_request.word1 = FILED_OP_EXEC_SELF;
            exec_request.word2 = 0;
            exec_request.word3 = header.request_id;
            (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
            return filed_dispatch_exec_self(runtime, reply_fd, &exec_request);
        }
        break;
    case FILED_OP_DIAG_DUMP_METRICS:
        filed_dispatch_dump_metrics(runtime);
        break;
    case FILED_OP_DIAG_SET_CACHE_SLOTS: {
        const filed_diag_request_t *diag =
            (const filed_diag_request_t *)((const uint8_t *)page + PACHA_SERVICE_HEADER_BYTES);
        if (header.payload_size < sizeof(*diag)) {
            status = -22;
        } else {
            status = filed_cache_flush_object(runtime, 0);
            if (status == 0) {
                filed_cache_configure(runtime, diag->subject);
                result = filed_page_cache.active_slots;
            }
        }
        break;
    }
    case FILED_OP_SERVICE_SET_NETD_SOCKET:
    case FILED_OP_SERVICE_SET_TERMD_TTY:
    case FILED_OP_SERVICE_SET_DRMD_DRM:
    case FILED_OP_SERVICE_SET_INPUTD_INPUT:
        if (header.payload_size < sizeof(filed_service_endpoint_request_t) ||
            request->fd_count < 3 ||
            request->fds[1].fd < 16)
        {
            status = -22;
        } else {
            const int endpoint_fd = (int)(uint32_t)request->fds[1].fd;
            if (header.op == FILED_OP_SERVICE_SET_NETD_SOCKET) {
                if (runtime->netd_socket_endpoint_fd >= 16) {
                    (void)pacha_fd_close(runtime->netd_socket_endpoint_fd);
                }
                runtime->netd_socket_endpoint_fd = endpoint_fd;
            } else if (header.op == FILED_OP_SERVICE_SET_TERMD_TTY) {
                if (runtime->termd_tty_endpoint_fd >= 16) {
                    (void)pacha_fd_close(runtime->termd_tty_endpoint_fd);
                }
                runtime->termd_tty_endpoint_fd = endpoint_fd;
            } else if (header.op == FILED_OP_SERVICE_SET_INPUTD_INPUT) {
                if (runtime->inputd_input_endpoint_fd >= 16) {
                    (void)pacha_fd_close(runtime->inputd_input_endpoint_fd);
                }
                runtime->inputd_input_endpoint_fd = endpoint_fd;
            } else {
                if (runtime->drmd_drm_endpoint_fd >= 16) {
                    (void)pacha_fd_close(runtime->drmd_drm_endpoint_fd);
                }
                runtime->drmd_drm_endpoint_fd = endpoint_fd;
            }
            keep_fd = endpoint_fd;
            result = (uint64_t)(uint32_t)endpoint_fd;
        }
        break;
    case FILED_OP_SERVICE_REGISTER_TERMD_SIGNAL_SUPERVISOR: {
        const int reply_status = filed_dispatch_register_termd_signal_supervisor(
            runtime,
            reply_fd,
            request,
            page,
            &header);
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
        filed_close_received_fds_except(request, reply_fd, -1);
        return reply_status;
    }
    default:
        status = -95;
        break;
    }

    if (status < 0) {
        error_token = filed_error_token(
            status,
            header.op,
            PACHA_STATUS_STAGE_DISPATCH,
            status,
            header.request_id,
            request->fd_count,
            0,
            0,
            "filed dispatch failed");
    }
    filed_close_received_fds_except(request, reply_fd, keep_fd);
    const int reply_status =
        filed_send_reply(reply_fd, page, &header, status, result, error_token);
    (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
    return reply_status;
}

int filed_dispatch_client_once(filed_runtime_t *runtime, int client_fd)
{
    struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS];
    struct pacha_ipc_msg request;

    if (runtime == NULL || client_fd < 16) {
        return -1;
    }

    memset(fds, 0, sizeof(fds));
    memset(&request, 0, sizeof(request));
    request.fds = fds;
    request.fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS;

    int status = pacha_ipc_recv(client_fd, &request);
    if (status != 0) {
        return status;
    }
    if (request.fd_count < 1 || fds[request.fd_count - 1].fd < 16) {
        return -22;
    }
    const int reply_fd = (int)fds[request.fd_count - 1].fd;
    if (request.word0 != PACHA_SERVICE_REQUEST_MAGIC) {
        pacha_service_envelope_t header;
        memset(&header, 0, sizeof(header));
        header.magic = PACHA_SERVICE_REQUEST_MAGIC;
        header.abi_version = PACHA_SERVICE_ABI_VERSION;
        header.service_id = FILED_SERVICE_ID;
        header.request_id = request.word3;
        header.trace_id = request.word3;
        return filed_send_reply(reply_fd, NULL, &header, -22, 0, 0);
    }
    return filed_dispatch_client(runtime, reply_fd, &request);
}
