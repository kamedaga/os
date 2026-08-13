#include "common.h"
#include "kobox/device_pachaos_capsule.h"
#include "linux_personality/linux_block.h"
#include "linux_subsystem/fs/fs.h"

_Static_assert(FILED_OPENED_KIND_REGULAR == FILED_VNODE_REGULAR, "regular vnode wire kind");
_Static_assert(FILED_OPENED_KIND_DIRECTORY == FILED_VNODE_DIRECTORY, "directory vnode wire kind");
_Static_assert(FILED_OPENED_KIND_SYMLINK == FILED_VNODE_SYMLINK, "symlink vnode wire kind");
_Static_assert(FILED_OPENED_KIND_DEVICE == FILED_VNODE_DEVICE, "device vnode wire kind");

enum {
    FILED_VMO_FILL_PAGE_BYTES = 4096u,
    FILED_VMO_FILL_WINDOW_BYTES = 2u * 1024u * 1024u,
};

_Static_assert(
    (FILED_VMO_FILL_WINDOW_BYTES % FILED_VMO_FILL_PAGE_BYTES) == 0,
    "file VMO fill window must be page aligned");

#if defined(FILED_STARTUP_PROFILE) && FILED_STARTUP_PROFILE
static inline uint64_t filed_profile_file_vmo_stage_begin(void)
{
    return filed_read_tsc();
}

static void filed_profile_file_vmo_stage_end(
    filed_runtime_t *runtime,
    uint32_t stage,
    uint64_t start)
{
    const uint64_t end = filed_read_tsc();
    if (runtime == NULL || runtime->dispatch_state == NULL ||
        stage >= FILED_PROFILE_FILE_VMO_STAGE_COUNT || end < start)
    {
        return;
    }
    __atomic_fetch_add(
        &filed_file_vmo_stage_cycles[stage], end - start, __ATOMIC_RELAXED);
    __atomic_fetch_add(
        &filed_file_vmo_stage_counts[stage], 1u, __ATOMIC_RELAXED);
}
#else
static inline uint64_t filed_profile_file_vmo_stage_begin(void)
{
    return 0;
}

static inline void filed_profile_file_vmo_stage_end(
    filed_runtime_t *runtime,
    uint32_t stage,
    uint64_t start)
{
    (void)runtime;
    (void)stage;
    (void)start;
}
#endif

#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
static kb_fs_read_profile_t filed_file_vmo_fs_profile;
static kb_linux_block_profile_t filed_file_vmo_block_profile;
static kb_pachaos_capsule_dma_profile_t filed_file_vmo_dma_profile;
static kb_pachaos_capsule_irq_profile_t filed_file_vmo_irq_profile;
static uint64_t filed_file_vmo_profile_calls;
static uint64_t filed_file_vmo_profile_bytes;

static inline uint64_t filed_profile_delta(uint64_t before, uint64_t after)
{
    return after >= before ? after - before : 0;
}

static void filed_file_vmo_storage_profile_accumulate(
    const kb_fs_read_profile_t *fs_before,
    const kb_fs_read_profile_t *fs_after,
    const kb_linux_block_profile_t *block_before,
    const kb_linux_block_profile_t *block_after,
    const kb_pachaos_capsule_dma_profile_t *dma_before,
    const kb_pachaos_capsule_dma_profile_t *dma_after,
    const kb_pachaos_capsule_irq_profile_t *irq_before,
    const kb_pachaos_capsule_irq_profile_t *irq_after,
    uint64_t bytes)
{
#define FILED_PROFILE_ADD(target, before, after, field) \
    __atomic_fetch_add(&(target).field, \
        filed_profile_delta((before)->field, (after)->field), __ATOMIC_RELAXED)
    FILED_PROFILE_ADD(filed_file_vmo_fs_profile, fs_before, fs_after, calls);
    FILED_PROFILE_ADD(filed_file_vmo_fs_profile, fs_before, fs_after, bytes);
    FILED_PROFILE_ADD(filed_file_vmo_fs_profile, fs_before, fs_after, total_cycles);
    FILED_PROFILE_ADD(filed_file_vmo_fs_profile, fs_before, fs_after, extent_lookup_calls);
    FILED_PROFILE_ADD(filed_file_vmo_fs_profile, fs_before, fs_after, extent_lookup_cycles);
    FILED_PROFILE_ADD(filed_file_vmo_fs_profile, fs_before, fs_after, device_read_calls);
    FILED_PROFILE_ADD(filed_file_vmo_fs_profile, fs_before, fs_after, device_read_cycles);
    FILED_PROFILE_ADD(filed_file_vmo_fs_profile, fs_before, fs_after, overlay_calls);
    FILED_PROFILE_ADD(filed_file_vmo_fs_profile, fs_before, fs_after, overlay_cycles);
    FILED_PROFILE_ADD(filed_file_vmo_fs_profile, fs_before, fs_after, partial_copy_calls);
    FILED_PROFILE_ADD(filed_file_vmo_fs_profile, fs_before, fs_after, partial_copy_cycles);
    for (size_t i = 0; i < KB_LINUX_BLOCK_PROFILE_STAGE_COUNT; i++) {
        __atomic_fetch_add(
            &filed_file_vmo_block_profile.cycles[i],
            filed_profile_delta(block_before->cycles[i], block_after->cycles[i]),
            __ATOMIC_RELAXED);
        __atomic_fetch_add(
            &filed_file_vmo_block_profile.calls[i],
            filed_profile_delta(block_before->calls[i], block_after->calls[i]),
            __ATOMIC_RELAXED);
    }
    FILED_PROFILE_ADD(
        filed_file_vmo_block_profile, block_before, block_after, disk_read_bytes);
    FILED_PROFILE_ADD(
        filed_file_vmo_dma_profile, dma_before, dma_after, copy_back_calls);
    FILED_PROFILE_ADD(
        filed_file_vmo_dma_profile, dma_before, dma_after, copy_back_bytes);
    FILED_PROFILE_ADD(
        filed_file_vmo_dma_profile, dma_before, dma_after, copy_back_cycles);
    FILED_PROFILE_ADD(filed_file_vmo_irq_profile, irq_before, irq_after, wait_calls);
    FILED_PROFILE_ADD(filed_file_vmo_irq_profile, irq_before, irq_after, wait_cycles);
    FILED_PROFILE_ADD(filed_file_vmo_irq_profile, irq_before, irq_after, fd_wait_calls);
    FILED_PROFILE_ADD(filed_file_vmo_irq_profile, irq_before, irq_after, fd_wait_cycles);
    FILED_PROFILE_ADD(filed_file_vmo_irq_profile, irq_before, irq_after, fd_wait_ready);
    FILED_PROFILE_ADD(filed_file_vmo_irq_profile, irq_before, irq_after, poll_calls);
    FILED_PROFILE_ADD(filed_file_vmo_irq_profile, irq_before, irq_after, poll_cycles);
    FILED_PROFILE_ADD(filed_file_vmo_irq_profile, irq_before, irq_after, poll_ready);
    FILED_PROFILE_ADD(filed_file_vmo_irq_profile, irq_before, irq_after, pre_poll_calls);
    FILED_PROFILE_ADD(filed_file_vmo_irq_profile, irq_before, irq_after, pre_poll_cycles);
    FILED_PROFILE_ADD(filed_file_vmo_irq_profile, irq_before, irq_after, pre_poll_ready);
    FILED_PROFILE_ADD(filed_file_vmo_irq_profile, irq_before, irq_after, post_poll_calls);
    FILED_PROFILE_ADD(filed_file_vmo_irq_profile, irq_before, irq_after, post_poll_cycles);
    FILED_PROFILE_ADD(filed_file_vmo_irq_profile, irq_before, irq_after, post_poll_ready);
    FILED_PROFILE_ADD(filed_file_vmo_irq_profile, irq_before, irq_after, handler_calls);
    FILED_PROFILE_ADD(filed_file_vmo_irq_profile, irq_before, irq_after, handler_cycles);
#undef FILED_PROFILE_ADD
    __atomic_fetch_add(&filed_file_vmo_profile_calls, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&filed_file_vmo_profile_bytes, bytes, __ATOMIC_RELAXED);
}

void filed_file_vmo_storage_profile_dump(void)
{
    fprintf(stderr,
        "FILED_STORAGE_PROFILE scope=file_vmo_fs pread_calls=%llu pread_bytes=%llu "
        "calls=%llu bytes=%llu total_cycles=%llu extent_calls=%llu extent_cycles=%llu "
        "device_calls=%llu device_cycles=%llu overlay_calls=%llu overlay_cycles=%llu "
        "partial_copy_calls=%llu partial_copy_cycles=%llu\n",
        (unsigned long long)__atomic_load_n(&filed_file_vmo_profile_calls, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_profile_bytes, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_fs_profile.calls, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_fs_profile.bytes, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_fs_profile.total_cycles, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_fs_profile.extent_lookup_calls, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_fs_profile.extent_lookup_cycles, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_fs_profile.device_read_calls, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_fs_profile.device_read_cycles, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_fs_profile.overlay_calls, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_fs_profile.overlay_cycles, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_fs_profile.partial_copy_calls, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_fs_profile.partial_copy_cycles, __ATOMIC_RELAXED));
    fprintf(stderr,
        "FILED_STORAGE_PROFILE scope=file_vmo_block bytes=%llu "
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
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.disk_read_bytes, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_REQUEST_ALLOC], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_REQUEST_ALLOC], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_DMA_MAP], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_DMA_MAP], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_BEFORE_EXECUTE], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_BEFORE_EXECUTE], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_QUEUE_SUBMIT], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_QUEUE_SUBMIT], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_COMPLETION_WAIT], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_COMPLETION_WAIT], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_DMA_UNMAP_COPYBACK], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_DMA_UNMAP_COPYBACK], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_REQUEST_FREE_TOTAL], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_REQUEST_FREE_TOTAL], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_DISK_IO_TOTAL], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_DISK_IO_TOTAL], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_CQ_POLL], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_CQ_POLL], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_IRQ_WAIT], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_IRQ_WAIT], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_POLL_YIELD], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_POLL_YIELD], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_POST_IRQ_DRAIN], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_POST_IRQ_DRAIN], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_PRP_ALLOC_INIT], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_ALLOC_INIT], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_DATA_MAP_PAGES], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_DATA_MAP_PAGES], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_PRP_BUILD], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_BUILD], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_PRP_AUX_MAP], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_AUX_MAP], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_HIT], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_HIT], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_MISS], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_MISS], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.calls[KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_FALLBACK], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_block_profile.cycles[KB_LINUX_BLOCK_PROFILE_NVME_PRP_CACHE_FALLBACK], __ATOMIC_RELAXED));
    fprintf(stderr,
        "FILED_STORAGE_PROFILE scope=file_vmo_dma_copy calls=%llu bytes=%llu cycles=%llu\n",
        (unsigned long long)__atomic_load_n(&filed_file_vmo_dma_profile.copy_back_calls, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_dma_profile.copy_back_bytes, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_dma_profile.copy_back_cycles, __ATOMIC_RELAXED));
    fprintf(stderr,
        "FILED_STORAGE_PROFILE scope=file_vmo_irq wait_calls=%llu wait_cycles=%llu "
        "fd_wait_calls=%llu fd_wait_cycles=%llu fd_wait_ready=%llu "
        "poll_calls=%llu poll_cycles=%llu poll_ready=%llu "
        "pre_poll_calls=%llu pre_poll_cycles=%llu pre_poll_ready=%llu "
        "post_poll_calls=%llu post_poll_cycles=%llu post_poll_ready=%llu "
        "handler_calls=%llu handler_cycles=%llu\n",
        (unsigned long long)__atomic_load_n(&filed_file_vmo_irq_profile.wait_calls, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_irq_profile.wait_cycles, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_irq_profile.fd_wait_calls, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_irq_profile.fd_wait_cycles, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_irq_profile.fd_wait_ready, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_irq_profile.poll_calls, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_irq_profile.poll_cycles, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_irq_profile.poll_ready, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_irq_profile.pre_poll_calls, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_irq_profile.pre_poll_cycles, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_irq_profile.pre_poll_ready, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_irq_profile.post_poll_calls, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_irq_profile.post_poll_cycles, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_irq_profile.post_poll_ready, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_irq_profile.handler_calls, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&filed_file_vmo_irq_profile.handler_cycles, __ATOMIC_RELAXED));
}
#else
void filed_file_vmo_storage_profile_dump(void)
{
}
#endif

filed_page_dispatch_result_t filed_dispatch_openat_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_openat_t *openat = (filed_openat_t *)page;
    filed_vfs_open_result_t open_result;
    int64_t reply_status = -22;
    uint64_t result = 0;

    memset(&open_result, 0, sizeof(open_result));
    if (filed_name_is_terminated(openat->name, sizeof(openat->name))) {
        reply_status = filed_openat_path(runtime, openat, &open_result);
    }
    if (reply_status == 0) {
        result = open_result.handle_id;
        openat->object_generation = open_result.object_generation;
        openat->dir_generation = open_result.dir_generation;
        openat->opened_kind = (uint64_t)open_result.kind;
        filed_runtime_publish_generation(
            runtime,
            open_result.handle_id,
            open_result.object_generation,
            open_result.dir_generation);
    }
    return filed_page_result(reply_status, result);
}

filed_page_dispatch_result_t filed_dispatch_validate_open_cache_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_validate_open_cache_t *validate =
        (filed_validate_open_cache_t *)page;
    filed_vfs_io_decision_t cached_decision;
    filed_vfs_open_result_t fresh_open;
    filed_openat_t openat;
    uint64_t valid = 0;

    if (runtime == NULL || validate == NULL) {
        return filed_page_result(-22, 0);
    }
    if (validate->cached_handle == 0 ||
        validate->reserved0 != 0 ||
        validate->reserved1 != 0 ||
        validate->object_generation == 0 ||
        !filed_name_is_terminated(validate->name, sizeof(validate->name)))
    {
        return filed_page_result(-22, 0);
    }

    memset(&cached_decision, 0, sizeof(cached_decision));
    const filed_status_t cached_status = filed_vfs_stat_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)validate->cached_handle,
        &cached_decision);
    if (cached_status != FILED_OK) {
        return filed_page_result(0, 0);
    }
    if (cached_decision.object_generation == validate->object_generation &&
        filed_vfs_validate_cached_handle_path(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)validate->cached_handle,
            validate->name,
            (uint32_t)validate->rights,
            (filed_generation_t)validate->object_generation) == FILED_OK)
    {
        validate->object_generation = cached_decision.object_generation;
        validate->dir_generation = cached_decision.dir_generation;
        return filed_page_result(0, 1);
    }

    memset(&openat, 0, sizeof(openat));
    openat.dir_handle = validate->dir_handle;
    openat.rights = validate->rights;
    openat.open_flags = validate->open_flags;
    memcpy(openat.name, validate->name, sizeof(openat.name));

    memset(&fresh_open, 0, sizeof(fresh_open));
    const int64_t open_status = filed_openat_path(runtime, &openat, &fresh_open);
    if (open_status != 0) {
        return filed_page_result(0, 0);
    }

    if (fresh_open.backend_object == cached_decision.backend_object &&
        fresh_open.object_generation == validate->object_generation &&
        cached_decision.object_generation == validate->object_generation)
    {
        if ((validate->open_flags & FILED_OPEN_DIRECTORY) == 0 ||
            (validate->dir_generation != 0 &&
                fresh_open.dir_generation == validate->dir_generation &&
                cached_decision.dir_generation == validate->dir_generation))
        {
            valid = 1;
        }
    }

    validate->object_generation = fresh_open.object_generation;
    validate->dir_generation = fresh_open.dir_generation;
    (void)filed_close_handle_runtime(runtime, fresh_open.handle_id);
    return filed_page_result(0, valid);
}

filed_page_dispatch_result_t filed_dispatch_stat_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_statx_t *wire_stat = (filed_statx_t *)page;
    filed_vfs_io_decision_t decision;
    filed_vfs_stat_snapshot_t snapshot;
    storage_statx_reply_t backend_stat;
    const uint64_t handle_id = wire_stat->handle;
    filed_status_t status = filed_vfs_stat_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)handle_id,
        &decision);
    int64_t reply_status = filed_status_to_wire(status);
    uint64_t result = 0;
    if (status == FILED_OK) {
        memset(&snapshot, 0, sizeof(snapshot));
        status = filed_vfs_get_stat_snapshot(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)handle_id,
            &snapshot);
        reply_status = filed_status_to_wire(status);
        if (status == FILED_OK && snapshot.valid) {
            (void)filed_write_stat_from_snapshot(wire_stat, &snapshot, handle_id);
            result = snapshot.size;
            filed_runtime_publish_generation(
                runtime,
                (filed_handle_id_t)(uint32_t)handle_id,
                snapshot.object_generation,
                snapshot.dir_generation);
            return filed_page_result(reply_status, result);
        }
    }
    if (status == FILED_OK) {
        memset(&backend_stat, 0, sizeof(backend_stat));
        reply_status = filed_backend_statx(
            runtime,
            decision.backend_object,
            &backend_stat);
        if (reply_status == 0) {
            snapshot = filed_stat_snapshot_from_backend(
                &backend_stat,
                handle_id,
                decision.object_generation,
                decision.dir_generation);
            (void)filed_vfs_update_stat_snapshot(
                &runtime->vfs,
                decision.backend_object,
                &snapshot);
            filed_vfs_stat_snapshot_t merged_snapshot;
            memset(&merged_snapshot, 0, sizeof(merged_snapshot));
            if (filed_vfs_get_stat_snapshot(
                    &runtime->vfs,
                    (filed_handle_id_t)(uint32_t)handle_id,
                    &merged_snapshot) == FILED_OK &&
                merged_snapshot.valid)
            {
                (void)filed_write_stat_from_snapshot(wire_stat, &merged_snapshot, handle_id);
            } else {
                (void)filed_write_stat_from_backend(
                    wire_stat,
                    &backend_stat,
                    handle_id,
                    decision.object_generation,
                    decision.dir_generation);
            }
            result = backend_stat.size;
            filed_runtime_publish_generation(
                runtime,
                (filed_handle_id_t)(uint32_t)handle_id,
                decision.object_generation,
                decision.dir_generation);
        }
    }
    return filed_page_result(reply_status, result);
}

filed_page_dispatch_result_t filed_dispatch_utimens_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_utimens_t *utimens = (filed_utimens_t *)page;
    if (runtime == NULL || utimens == NULL) {
        return filed_page_result(-22, 0);
    }
    if ((utimens->mask & ~((uint64_t)FILED_UTIMENS_ATIME | (uint64_t)FILED_UTIMENS_MTIME)) != 0) {
        return filed_page_result(-22, 0);
    }
    if (utimens->mask == 0) {
        return filed_page_result(0, 0);
    }
    if (((utimens->mask & FILED_UTIMENS_ATIME) != 0 &&
            (utimens->atime_nsec < 0 || utimens->atime_nsec >= 1000000000ll)) ||
        ((utimens->mask & FILED_UTIMENS_MTIME) != 0 &&
            (utimens->mtime_nsec < 0 || utimens->mtime_nsec >= 1000000000ll)))
    {
        return filed_page_result(-22, 0);
    }
    filed_vfs_io_decision_t decision;
    filed_status_t vfs_status = filed_vfs_setattr_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)utimens->handle,
        &decision);
    if (vfs_status != FILED_OK) {
        return filed_page_result(filed_status_to_wire(vfs_status), 0);
    }
    int backend_status = filed_backend_utimens(
        runtime,
        decision.backend_object,
        (uint32_t)utimens->mask,
        utimens->atime_sec,
        utimens->atime_nsec,
        utimens->mtime_sec,
        utimens->mtime_nsec);
    if (backend_status != 0) {
        return filed_page_result(backend_status, 0);
    }
    vfs_status = filed_vfs_update_times(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)utimens->handle,
        (uint32_t)utimens->mask,
        utimens->atime_sec,
        utimens->atime_nsec,
        utimens->mtime_sec,
        utimens->mtime_nsec);
    return filed_page_result(filed_status_to_wire(vfs_status), 0);
}

static int filed_prepare_chmod_snapshot(
    filed_runtime_t *runtime,
    filed_handle_id_t handle_id,
    const filed_vfs_io_decision_t *decision,
    bool *out_snapshot_cached,
    filed_vfs_stat_snapshot_t *out_snapshot)
{
    bool snapshot_valid = false;
    storage_statx_reply_t backend_stat;
    if (runtime == NULL ||
        decision == NULL ||
        out_snapshot_cached == NULL ||
        out_snapshot == NULL)
    {
        return -22;
    }
    *out_snapshot_cached = false;
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    filed_status_t status = filed_vfs_setattr_snapshot_valid(
        &runtime->vfs,
        handle_id,
        &snapshot_valid);
    if (status != FILED_OK) {
        return filed_status_to_wire(status);
    }
    if (snapshot_valid) {
        *out_snapshot_cached = true;
        return 0;
    }
    memset(&backend_stat, 0, sizeof(backend_stat));
    int64_t reply_status = filed_backend_statx(
        runtime,
        decision->backend_object,
        &backend_stat);
    if (reply_status != 0) {
        return (int)reply_status;
    }
    *out_snapshot = filed_stat_snapshot_from_backend(
        &backend_stat,
        handle_id,
        decision->object_generation,
        decision->dir_generation);
    return out_snapshot->valid ? 0 : -5;
}

filed_page_dispatch_result_t filed_dispatch_chmod_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_chmod_t *chmod_req = (filed_chmod_t *)page;
    bool snapshot_cached = false;
    filed_vfs_stat_snapshot_t fetched_snapshot;
    if (runtime == NULL ||
        chmod_req == NULL ||
        chmod_req->reserved0 != 0 ||
        chmod_req->reserved1 != 0 ||
        (chmod_req->mode & ~07777ull) != 0)
    {
        return filed_page_result(-22, 0);
    }
    filed_vfs_io_decision_t decision;
    filed_status_t vfs_status = filed_vfs_setattr_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)chmod_req->handle,
        &decision);
    if (vfs_status != FILED_OK) {
        return filed_page_result(filed_status_to_wire(vfs_status), 0);
    }
    int status = filed_prepare_chmod_snapshot(
        runtime,
        (filed_handle_id_t)(uint32_t)chmod_req->handle,
        &decision,
        &snapshot_cached,
        &fetched_snapshot);
    if (status != 0) {
        return filed_page_result(status, 0);
    }
    status = filed_backend_chmod(
        runtime,
        decision.backend_object,
        chmod_req->mode);
    if (status != 0) {
        return filed_page_result(status, 0);
    }
    if (!snapshot_cached) {
        fetched_snapshot.mode =
            (fetched_snapshot.mode & 0170000ull) | (chmod_req->mode & 07777ull);
        vfs_status = filed_vfs_update_stat_snapshot(
            &runtime->vfs,
            decision.backend_object,
            &fetched_snapshot);
        if (vfs_status != FILED_OK) {
            return filed_page_result(filed_status_to_wire(vfs_status), 0);
        }
    }
    vfs_status = filed_vfs_update_mode(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)chmod_req->handle,
        chmod_req->mode);
    return filed_page_result(filed_status_to_wire(vfs_status), 0);
}

filed_page_dispatch_result_t filed_dispatch_pread_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_io_t *io = (filed_io_t *)page;
    filed_vfs_io_decision_t decision;
    uint64_t bytes = 0;
    filed_status_t status = filed_vfs_pread_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)io->handle,
        io->offset,
        io->length,
        &decision);
    int64_t reply_status = filed_status_to_wire(status);
    if (status == FILED_OK) {
        uint64_t length = decision.length;
        if (length > FILED_IO_BYTES) {
            length = FILED_IO_BYTES;
        }
        reply_status = filed_cached_pread(
            runtime,
            decision.backend_object,
            decision.offset,
            io->data,
            length,
            &bytes);
        if (reply_status == 0) {
            io->length = bytes;
        }
    }
    return filed_page_result(reply_status, bytes);
}

int filed_vmo_fill_window_plan(
    uint64_t vmo_offset,
    uint64_t remaining,
    uint64_t *out_map_offset,
    uint64_t *out_data_offset,
    uint64_t *out_map_length,
    uint64_t *out_chunk)
{
    if (remaining == 0 ||
        out_map_offset == NULL ||
        out_data_offset == NULL ||
        out_map_length == NULL ||
        out_chunk == NULL)
    {
        return -22;
    }

    const uint64_t map_offset =
        vmo_offset & ~(uint64_t)(FILED_VMO_FILL_PAGE_BYTES - 1u);
    const uint64_t data_offset = vmo_offset - map_offset;
    uint64_t chunk = FILED_VMO_FILL_WINDOW_BYTES - data_offset;
    if (chunk > remaining) {
        chunk = remaining;
    }

    *out_map_offset = map_offset;
    *out_data_offset = data_offset;
    *out_map_length = data_offset + chunk;
    *out_chunk = chunk;
    return 0;
}

static int filed_pread_into_vmo_windows(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t file_offset,
    int vmo_fd,
    uint64_t vmo_offset,
    uint64_t length,
    const char *kind,
    uint64_t *out_bytes)
{
    if (runtime == NULL || backend_object == 0 || vmo_fd < 16 ||
        kind == NULL || out_bytes == NULL ||
        file_offset + length < file_offset ||
        vmo_offset + length < vmo_offset)
    {
        return -22;
    }
    *out_bytes = 0;

    uint64_t total = 0;
    while (total < length) {
        uint64_t map_offset = 0;
        uint64_t data_offset = 0;
        uint64_t map_length = 0;
        uint64_t chunk = 0;
        const int plan_status = filed_vmo_fill_window_plan(
            vmo_offset + total,
            length - total,
            &map_offset,
            &data_offset,
            &map_length,
            &chunk);
        if (plan_status != 0) {
            return plan_status;
        }

        uint64_t map_profile_stage = filed_profile_file_vmo_stage_begin();
        unsigned char *mapped = pacha_mmap(
            vmo_fd,
            map_length,
            PACHA_PROT_READ | PACHA_PROT_WRITE,
            PACHA_MMAP_SHARED,
            map_offset);
        filed_profile_file_vmo_stage_end(
            runtime,
            FILED_PROFILE_FILE_VMO_STAGE_VMO_MMAP,
            map_profile_stage);
        if (mapped == NULL) {
            uint64_t reclaimed_bytes = 0;
            const uint32_t reclaimed_entries =
                filed_file_vmo_cache_reclaim_snapshots(
                    runtime, &reclaimed_bytes);
            fprintf(stderr,
                "[filed] file_vmo_window_retry kind=%s total=%llu "
                "map_offset=%llu map_length=%llu reclaimed_entries=%u "
                "reclaimed_bytes=%llu\n",
                kind,
                (unsigned long long)total,
                (unsigned long long)map_offset,
                (unsigned long long)map_length,
                reclaimed_entries,
                (unsigned long long)reclaimed_bytes);
            map_profile_stage = filed_profile_file_vmo_stage_begin();
            mapped = pacha_mmap(
                vmo_fd,
                map_length,
                PACHA_PROT_READ | PACHA_PROT_WRITE,
                PACHA_MMAP_SHARED,
                map_offset);
            filed_profile_file_vmo_stage_end(
                runtime,
                FILED_PROFILE_FILE_VMO_STAGE_VMO_MMAP,
                map_profile_stage);
        }
        if (mapped == NULL) {
            fprintf(stderr,
                "FILED_STORAGE_FAULT layer=file_vmo_window_mmap "
                "status=-12 kind=%s total=%llu map_offset=%llu "
                "map_length=%llu requested_length=%llu\n",
                kind,
                (unsigned long long)total,
                (unsigned long long)map_offset,
                (unsigned long long)map_length,
                (unsigned long long)length);
            return -12;
        }

        const int dma_window_status =
            kb_linux_block_dma_read_window_begin(mapped, (size_t)map_length);
        uint64_t bytes = 0;
        const int read_status = filed_cached_pread(
            runtime,
            backend_object,
            file_offset + total,
            mapped + data_offset,
            chunk,
            &bytes);
        if (dma_window_status == 0) {
            kb_linux_block_dma_read_window_end();
        }
        const int unmap_status = pacha_munmap(mapped, map_length);
        if (unmap_status != 0) {
            fprintf(stderr,
                "FILED_STORAGE_FAULT layer=file_vmo_window_munmap "
                "status=%d kind=%s total=%llu map_offset=%llu "
                "map_length=%llu\n",
                unmap_status,
                kind,
                (unsigned long long)total,
                (unsigned long long)map_offset,
                (unsigned long long)map_length);
            return unmap_status;
        }
        if (read_status != 0) {
            return read_status;
        }
        if (bytes > chunk) {
            fprintf(stderr,
                "FILED_STORAGE_FAULT layer=file_vmo_window_pread "
                "status=-5 kind=%s total=%llu chunk=%llu bytes=%llu\n",
                kind,
                (unsigned long long)total,
                (unsigned long long)chunk,
                (unsigned long long)bytes);
            return -5;
        }
        total += bytes;
        *out_bytes = total;
        if (bytes < chunk) {
            break;
        }
    }
    return 0;
}

filed_page_dispatch_result_t filed_dispatch_pread_to_vmo_page(
    filed_runtime_t *runtime,
    void *page,
    int vmo_fd)
{
    filed_pread_vmo_t *pread_vmo = (filed_pread_vmo_t *)page;
    filed_vfs_io_decision_t decision;
    uint64_t bytes = 0;

    if (vmo_fd < 16 ||
        pread_vmo->reserved0 != 0 ||
        pread_vmo->reserved1 != 0 ||
        pread_vmo->vmo_offset + pread_vmo->length < pread_vmo->vmo_offset)
    {
        return filed_page_result(-22, 0);
    }
    if (pread_vmo->length == 0) {
        return filed_page_result(0, 0);
    }

    filed_status_t status = filed_vfs_pread_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)pread_vmo->handle,
        pread_vmo->file_offset,
        pread_vmo->length,
        &decision);
    int64_t reply_status = filed_status_to_wire(status);
    if (status != FILED_OK) {
        return filed_page_result(reply_status, 0);
    }

    const uint64_t length = decision.length;
    if (length == 0 || pread_vmo->vmo_offset + length < pread_vmo->vmo_offset) {
        return filed_page_result(0, 0);
    }

    reply_status = filed_pread_into_vmo_windows(
        runtime,
        decision.backend_object,
        decision.offset,
        vmo_fd,
        pread_vmo->vmo_offset,
        length,
        "pread_to_vmo",
        &bytes);
    return filed_page_result(reply_status, bytes);
}

filed_page_dispatch_result_t filed_create_file_vmo_cache_entry(
    filed_runtime_t *runtime,
    const filed_vfs_io_decision_t *decision,
    uint64_t file_offset,
    uint64_t length,
    filed_file_vmo_cache_entry_t **out_entry)
{
    if (runtime == NULL || decision == NULL || out_entry == NULL || length == 0) {
        return filed_page_result(-22, 0);
    }
    *out_entry = NULL;
    const uint64_t rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_MAP_EXEC;
    /* Reclaim cached owners before asking the kernel to back the next image.
     * Large ELF dependencies (notably libLLVM) can otherwise fail even though
     * the cache has enough evictable bytes: the old implementation allocated
     * first and only enforced the byte budget after the read completed. */
    filed_file_vmo_cache_entry_t *entry =
        filed_file_vmo_cache_slot_for_length(runtime, length);
    if (entry == NULL) {
        return filed_page_result(-28, 0);
    }
    uint64_t profile_stage = filed_profile_file_vmo_stage_begin();
    int vmo_fd = pacha_vmo_create(length, rights, 0);
    if (vmo_fd < 16) {
        uint64_t reclaimed_bytes = 0;
        const uint32_t reclaimed_entries =
            filed_file_vmo_cache_reclaim_snapshots(runtime, &reclaimed_bytes);
        if (reclaimed_entries != 0) {
            fprintf(stderr,
                "[filed] file_vmo_allocation_retry kind=snapshot length=%llu "
                "reclaimed_entries=%u reclaimed_bytes=%llu\n",
                (unsigned long long)length,
                reclaimed_entries,
                (unsigned long long)reclaimed_bytes);
            vmo_fd = pacha_vmo_create(length, rights, 0);
        }
    }
    filed_profile_file_vmo_stage_end(
        runtime, FILED_PROFILE_FILE_VMO_STAGE_VMO_CREATE, profile_stage);
    if (vmo_fd < 16) {
        return filed_page_result(-12, 0);
    }
    uint64_t bytes = 0;
    profile_stage = filed_profile_file_vmo_stage_begin();
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    kb_fs_read_profile_t fs_profile_before;
    kb_fs_read_profile_t fs_profile_after;
    kb_linux_block_profile_t block_profile_before;
    kb_linux_block_profile_t block_profile_after;
    kb_pachaos_capsule_dma_profile_t dma_profile_before;
    kb_pachaos_capsule_dma_profile_t dma_profile_after;
    kb_pachaos_capsule_irq_profile_t irq_profile_before;
    kb_pachaos_capsule_irq_profile_t irq_profile_after;
    kb_fs_read_profile_snapshot(&fs_profile_before);
    kb_linux_block_profile_snapshot(&block_profile_before);
    kb_pachaos_capsule_dma_profile_snapshot(&dma_profile_before);
    kb_pachaos_capsule_irq_profile_snapshot(&irq_profile_before);
#endif
    const int64_t reply_status = filed_pread_into_vmo_windows(
        runtime,
        decision->backend_object,
        file_offset,
        vmo_fd,
        0,
        length,
        "snapshot",
        &bytes);
#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
    kb_fs_read_profile_snapshot(&fs_profile_after);
    kb_linux_block_profile_snapshot(&block_profile_after);
    kb_pachaos_capsule_dma_profile_snapshot(&dma_profile_after);
    kb_pachaos_capsule_irq_profile_snapshot(&irq_profile_after);
    filed_file_vmo_storage_profile_accumulate(
        &fs_profile_before,
        &fs_profile_after,
        &block_profile_before,
        &block_profile_after,
        &dma_profile_before,
        &dma_profile_after,
        &irq_profile_before,
        &irq_profile_after,
        bytes);
#endif
    filed_profile_file_vmo_stage_end(
        runtime, FILED_PROFILE_FILE_VMO_STAGE_PREAD, profile_stage);
    if (reply_status != 0) {
        (void)pacha_fd_close(vmo_fd);
        return filed_page_result(reply_status, 0);
    }

    memset(entry, 0, sizeof(*entry));
    entry->active = 1;
    entry->vmo_fd = vmo_fd;
    entry->backend_object = decision->backend_object;
    entry->object_generation = decision->object_generation;
    entry->file_offset = file_offset;
    entry->length = length;
    entry->clock = ++filed_file_vmo_cache.clock;
    filed_cache_note_attachment(runtime, decision->backend_object, FILED_CACHE_ATTACHMENT_VMO);
    filed_file_vmo_cache_stores++;
    *out_entry = entry;
    return filed_page_result(0, bytes);
}

int filed_dispatch_file_vmo(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request,
    void *reply_page,
    const pacha_service_envelope_t *header)
{
    if (runtime == NULL ||
        request == NULL ||
        reply_page == NULL ||
        header == NULL ||
        header->payload_size < sizeof(filed_file_vmo_request_t))
    {
        return filed_send_reply(reply_fd, reply_page, header, -22, 0, 0);
    }

    const filed_file_vmo_request_t *file_vmo =
        (const filed_file_vmo_request_t *)((const uint8_t *)reply_page + PACHA_SERVICE_HEADER_BYTES);
    filed_page_dispatch_result_t result = filed_page_result(-22, 0);
    filed_file_vmo_cache_entry_t *entry = NULL;
    if (file_vmo->length != 0 &&
        file_vmo->length <= FILED_FILE_VMO_MAX_BYTES &&
        file_vmo->reserved0 == 0 &&
        file_vmo->reserved1 == 0 &&
        file_vmo->flags == 0)
    {
        filed_vfs_io_decision_t decision;
        uint64_t profile_stage = filed_profile_file_vmo_stage_begin();
        filed_status_t status = filed_vfs_pread_prepare(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)file_vmo->handle,
            file_vmo->file_offset,
            file_vmo->length,
            &decision);
        filed_profile_file_vmo_stage_end(
            runtime, FILED_PROFILE_FILE_VMO_STAGE_PREPARE, profile_stage);
        int64_t reply_status = filed_status_to_wire(status);
        if (status == FILED_OK && decision.length == file_vmo->length) {
            profile_stage = filed_profile_file_vmo_stage_begin();
            entry = filed_file_vmo_cache_lookup(
                runtime,
                decision.backend_object,
                decision.object_generation,
                decision.offset,
                decision.length);
            filed_profile_file_vmo_stage_end(
                runtime, FILED_PROFILE_FILE_VMO_STAGE_LOOKUP, profile_stage);
            if (entry != NULL) {
                result = filed_page_result(0, decision.length);
            } else {
                profile_stage = filed_profile_file_vmo_stage_begin();
                result = filed_create_file_vmo_cache_entry(
                    runtime,
                    &decision,
                    decision.offset,
                    decision.length,
                    &entry);
                filed_profile_file_vmo_stage_end(
                    runtime, FILED_PROFILE_FILE_VMO_STAGE_CREATE_TOTAL, profile_stage);
            }
            if (result.status == 0) {
                filed_runtime_publish_generation(
                    runtime,
                    (filed_handle_id_t)(uint32_t)file_vmo->handle,
                    decision.object_generation,
                    decision.dir_generation);
            }
        } else {
            result = filed_page_result(reply_status, 0);
        }
    }

    pacha_service_reply_init(
        (pacha_service_envelope_t *)reply_page,
        header,
        result.status,
        PACHA_SERVICE_ERROR_FILED_VFS,
        result.status < 0 ? 0 : result.result,
        0);
    struct pacha_ipc_fd fd = {
        .fd = (uint64_t)(uint32_t)(entry != NULL ? entry->vmo_fd : -1),
        .rights =
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE |
            PACHA_FD_RIGHT_MAP_EXEC,
        .flags = 0,
        .transfer_flags = PACHA_IPC_TRANSFER_CLOEXEC,
    };
    struct pacha_ipc_msg reply = {
        .word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = (uint64_t)result.status,
        .word2 = result.status < 0 ? 0 : result.result,
        .word3 = header->request_id,
        .fds = result.status == 0 && entry != NULL && entry->vmo_fd >= 16 ? &fd : NULL,
        .fd_count = result.status == 0 && entry != NULL && entry->vmo_fd >= 16 ? 1u : 0u,
    };
    if (result.status < 0) {
        (void)filed_error_token(
            result.status,
            header->op,
            PACHA_STATUS_STAGE_STATUS_MAP,
            result.status,
            header->request_id,
            request->fd_count,
            file_vmo->handle,
            0,
            "filed file-vmo negative reply");
    }
    const uint64_t profile_stage = filed_profile_file_vmo_stage_begin();
    const int reply_status = pacha_ipc_reply(reply_fd, &reply);
    filed_profile_file_vmo_stage_end(
        runtime, FILED_PROFILE_FILE_VMO_STAGE_REPLY, profile_stage);
    (void)pacha_fd_close(reply_fd);
    return reply_status;
}

int filed_dispatch_shared_file_vmo(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request,
    void *reply_page,
    const pacha_service_envelope_t *header)
{
    if (runtime == NULL || request == NULL || reply_page == NULL || header == NULL ||
        header->payload_size < sizeof(filed_file_vmo_request_t))
    {
        return filed_send_reply(reply_fd, reply_page, header, -22, 0, 0);
    }

    const filed_file_vmo_request_t *shared_vmo =
        (const filed_file_vmo_request_t *)((const uint8_t *)reply_page + PACHA_SERVICE_HEADER_BYTES);
    int64_t reply_status = -22;
    filed_file_vmo_cache_entry_t *entry = NULL;
    filed_vfs_io_decision_t decision;
    filed_vfs_stat_snapshot_t snapshot;
    memset(&decision, 0, sizeof(decision));
    memset(&snapshot, 0, sizeof(snapshot));

    const uint64_t known_flags = FILED_FILE_VMO_WRITE | FILED_FILE_VMO_EXEC;
    const int writable = (shared_vmo->flags & FILED_FILE_VMO_WRITE) != 0;
    const int executable = (shared_vmo->flags & FILED_FILE_VMO_EXEC) != 0;
    if (shared_vmo->length != 0 &&
        (shared_vmo->file_offset & 4095u) == 0 &&
        shared_vmo->file_offset <= UINT64_MAX - shared_vmo->length &&
        shared_vmo->reserved0 == 0 &&
        shared_vmo->reserved1 == 0 &&
        (shared_vmo->flags & ~known_flags) == 0)
    {
        filed_status_t status = filed_vfs_pread_prepare(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)shared_vmo->handle,
            shared_vmo->file_offset,
            shared_vmo->length,
            &decision);
        reply_status = filed_status_to_wire(status);
        if (status == FILED_OK && writable) {
            filed_vfs_io_decision_t write_decision;
            memset(&write_decision, 0, sizeof(write_decision));
            status = filed_vfs_pwrite_prepare(
                &runtime->vfs,
                (filed_handle_id_t)(uint32_t)shared_vmo->handle,
                shared_vmo->file_offset,
                shared_vmo->length,
                &write_decision);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK && write_decision.backend_object != decision.backend_object) {
                reply_status = -5;
                status = FILED_ERR_IO;
            }
        }
        if (status == FILED_OK) {
            status = filed_vfs_get_stat_snapshot(
                &runtime->vfs,
                (filed_handle_id_t)(uint32_t)shared_vmo->handle,
                &snapshot);
            reply_status = filed_status_to_wire(status);
        }
        if (status == FILED_OK && !snapshot.valid) {
            storage_statx_reply_t backend_stat;
            memset(&backend_stat, 0, sizeof(backend_stat));
            reply_status = filed_backend_statx(runtime, decision.backend_object, &backend_stat);
            if (reply_status == 0) {
                snapshot = filed_stat_snapshot_from_backend(
                    &backend_stat,
                    shared_vmo->handle,
                    decision.object_generation,
                    decision.dir_generation);
                (void)filed_vfs_update_stat_snapshot(
                    &runtime->vfs,
                    decision.backend_object,
                    &snapshot);
            }
        }
        if (status == FILED_OK && reply_status == 0 && snapshot.valid) {
            reply_status = filed_cache_create_shared_vmo(
                runtime,
                decision.backend_object,
                decision.object_generation,
                snapshot.size,
                shared_vmo->file_offset + shared_vmo->length,
                &entry);
            if (reply_status == 0 && entry != NULL) {
                if (writable) {
                    entry->writable_lent = 1;
                }
                filed_runtime_publish_generation(
                    runtime,
                    (filed_handle_id_t)(uint32_t)shared_vmo->handle,
                    decision.object_generation,
                    decision.dir_generation);
            }
        }
    }

    pacha_service_reply_init(
        (pacha_service_envelope_t *)reply_page,
        header,
        reply_status,
        PACHA_SERVICE_ERROR_FILED_VFS,
        reply_status == 0 && entry != NULL ? entry->logical_size : 0,
        0);
    uint64_t transfer_rights = PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_MAP_READ;
    if (writable) {
        transfer_rights |= PACHA_FD_RIGHT_MAP_WRITE;
    }
    if (executable) {
        transfer_rights |= PACHA_FD_RIGHT_MAP_EXEC;
    }
    struct pacha_ipc_fd fd = {
        .fd = (uint64_t)(uint32_t)(entry != NULL ? entry->vmo_fd : -1),
        .rights = transfer_rights,
        .flags = 0,
        .transfer_flags = PACHA_IPC_TRANSFER_CLOEXEC,
    };
    struct pacha_ipc_msg reply = {
        .word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = (uint64_t)reply_status,
        .word2 = reply_status == 0 && entry != NULL ? entry->logical_size : 0,
        .word3 = header->request_id,
        .fds = reply_status == 0 && entry != NULL && entry->vmo_fd >= 16 ? &fd : NULL,
        .fd_count = reply_status == 0 && entry != NULL && entry->vmo_fd >= 16 ? 1u : 0u,
    };
    const int send_status = pacha_ipc_reply(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    return send_status;
}

filed_page_dispatch_result_t filed_dispatch_memfd_create_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_memfd_create_t *memfd = (filed_memfd_create_t *)page;
    const uint64_t known_flags =
        FILED_MEMFD_CLOEXEC |
        FILED_MEMFD_ALLOW_SEALING;
    if (runtime == NULL || memfd == NULL || !runtime->tmpfs_root_handle_valid ||
        memfd->reserved0 != 0 ||
        (memfd->flags & ~known_flags) != 0 ||
        memchr(memfd->name, '\0', sizeof(memfd->name)) == NULL)
    {
        return filed_page_result(-22, 0);
    }

    const uint64_t root_object = filed_tmpfs_backend_root_object(&runtime->tmpfs);
    if (root_object == 0) {
        return filed_page_result(-5, 0);
    }
    char internal_name[FILED_NAME_BYTES];
    const uint64_t sequence = ++runtime->memfd_sequence;
    const int name_length = snprintf(
        internal_name,
        sizeof(internal_name),
        ".memfd-%llu",
        (unsigned long long)sequence);
    if (name_length <= 0 || (size_t)name_length >= sizeof(internal_name)) {
        return filed_page_result(-75, 0);
    }

    uint64_t object_id = 0;
    int reply_status = filed_tmpfs_backend_create(
        &runtime->tmpfs,
        root_object,
        internal_name,
        0600,
        &object_id);
    if (reply_status != 0) {
        return filed_page_result(reply_status, 0);
    }

    filed_vfs_open_result_t opened;
    memset(&opened, 0, sizeof(opened));
    const uint32_t open_flags =
        (memfd->flags & FILED_MEMFD_CLOEXEC) != 0 ? FILED_OPEN_CLOEXEC : 0;
    filed_status_t status = filed_vfs_create_backend_child(
        &runtime->vfs,
        runtime->tmpfs_root_handle_id,
        object_id,
        FILED_VNODE_REGULAR,
        internal_name,
        FILED_RIGHT_READ | FILED_RIGHT_WRITE | FILED_RIGHT_STAT,
        open_flags,
        &opened);
    if (status != FILED_OK) {
        (void)filed_tmpfs_backend_unlink(&runtime->tmpfs, root_object, internal_name);
        (void)filed_tmpfs_backend_release_object(&runtime->tmpfs, object_id);
        return filed_page_result(filed_status_to_wire(status), 0);
    }

    storage_statx_reply_t backend_stat;
    memset(&backend_stat, 0, sizeof(backend_stat));
    reply_status = filed_tmpfs_backend_statx(&runtime->tmpfs, object_id, &backend_stat);
    if (reply_status == 0) {
        const filed_vfs_stat_snapshot_t snapshot = filed_stat_snapshot_from_backend(
            &backend_stat,
            opened.handle_id,
            opened.object_generation,
            opened.dir_generation);
        (void)filed_vfs_update_stat_snapshot(&runtime->vfs, object_id, &snapshot);
        reply_status = filed_tmpfs_backend_unlink(&runtime->tmpfs, root_object, internal_name);
    }
    if (reply_status == 0) {
        status = filed_vfs_unlink_commit(
            &runtime->vfs,
            runtime->tmpfs_root_handle_id,
            internal_name);
        reply_status = filed_status_to_wire(status);
    }
    filed_cache_invalidate(runtime, root_object);
    if (reply_status != 0) {
        (void)filed_close_handle_runtime(runtime, opened.handle_id);
        return filed_page_result(reply_status, 0);
    }
    return filed_page_result(0, opened.handle_id);
}

filed_page_dispatch_result_t filed_dispatch_read_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_io_t *io = (filed_io_t *)page;
    filed_vfs_io_decision_t decision;
    uint64_t bytes = 0;
    filed_status_t status = filed_vfs_read_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)io->handle,
        io->length,
        &decision);
    int64_t reply_status = filed_status_to_wire(status);
    if (status == FILED_OK) {
        uint64_t length = decision.length;
        if (length > FILED_IO_BYTES) {
            length = FILED_IO_BYTES;
        }
        reply_status = filed_cached_pread(
            runtime,
            decision.backend_object,
            decision.offset,
            io->data,
            length,
            &bytes);
        if (reply_status == 0) {
            io->offset = decision.offset;
            io->length = bytes;
            status = filed_vfs_read_commit(
                &runtime->vfs,
                (filed_handle_id_t)(uint32_t)io->handle,
                bytes);
            if (status != FILED_OK) {
                reply_status = filed_status_to_wire(status);
            }
        }
    }
    return filed_page_result(reply_status, bytes);
}

filed_page_dispatch_result_t filed_dispatch_pwrite_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_io_t *io = (filed_io_t *)page;
    filed_vfs_io_decision_t decision;
    uint64_t bytes = 0;
    filed_status_t status = filed_vfs_pwrite_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)io->handle,
        io->offset,
        io->length,
        &decision);
    int64_t reply_status = filed_status_to_wire(status);
    if (status == FILED_OK) {
        uint64_t length = decision.length;
        if (length > FILED_IO_BYTES) {
            length = FILED_IO_BYTES;
        }
        if (decision.offset == UINT64_MAX) {
            filed_vfs_stat_snapshot_t snapshot;
            storage_statx_reply_t backend_stat;
            memset(&snapshot, 0, sizeof(snapshot));
            status = filed_vfs_get_stat_snapshot(
                &runtime->vfs,
                (filed_handle_id_t)(uint32_t)io->handle,
                &snapshot);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK && snapshot.valid) {
                decision.offset = snapshot.size;
            } else if (status == FILED_OK) {
                memset(&backend_stat, 0, sizeof(backend_stat));
                reply_status = filed_backend_statx(
                    runtime,
                    decision.backend_object,
                    &backend_stat);
                if (reply_status != 0) {
                    return filed_page_result(reply_status, bytes);
                }
                snapshot = filed_stat_snapshot_from_backend(
                    &backend_stat,
                    io->handle,
                    decision.object_generation,
                    decision.dir_generation);
                (void)filed_vfs_update_stat_snapshot(
                    &runtime->vfs,
                    decision.backend_object,
                    &snapshot);
                decision.offset = backend_stat.size;
            } else {
                return filed_page_result(reply_status, bytes);
            }
        }
        reply_status = filed_cached_pwrite_ex(
            runtime,
            decision.backend_object,
            decision.offset,
            io->data,
            length,
            &bytes,
            true);
        if (reply_status == 0) {
            io->offset = decision.offset;
            io->length = bytes;
            status = filed_vfs_note_write(
                &runtime->vfs,
                (filed_handle_id_t)(uint32_t)io->handle,
                decision.offset,
                bytes);
            if (status != FILED_OK) {
                reply_status = filed_status_to_wire(status);
            } else {
                filed_runtime_publish_backend_object_generation(
                    runtime,
                    decision.backend_object);
            }
        }
    }
    return filed_page_result(reply_status, bytes);
}

filed_page_dispatch_result_t filed_dispatch_write_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_io_t *io = (filed_io_t *)page;
    filed_vfs_io_decision_t decision;
    uint64_t bytes = 0;
    filed_status_t status = filed_vfs_write_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)io->handle,
        io->length,
        &decision);
    int64_t reply_status = filed_status_to_wire(status);
    if (status == FILED_OK) {
        uint64_t length = decision.length;
        if (length > FILED_IO_BYTES) {
            length = FILED_IO_BYTES;
        }
        if (decision.offset == UINT64_MAX) {
            filed_vfs_stat_snapshot_t snapshot;
            storage_statx_reply_t backend_stat;
            memset(&snapshot, 0, sizeof(snapshot));
            status = filed_vfs_get_stat_snapshot(
                &runtime->vfs,
                (filed_handle_id_t)(uint32_t)io->handle,
                &snapshot);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK && snapshot.valid) {
                decision.offset = snapshot.size;
            } else if (status == FILED_OK) {
                memset(&backend_stat, 0, sizeof(backend_stat));
                reply_status = filed_backend_statx(
                    runtime,
                    decision.backend_object,
                    &backend_stat);
                if (reply_status != 0) {
                    return filed_page_result(reply_status, bytes);
                }
                snapshot = filed_stat_snapshot_from_backend(
                    &backend_stat,
                    io->handle,
                    decision.object_generation,
                    decision.dir_generation);
                (void)filed_vfs_update_stat_snapshot(
                    &runtime->vfs,
                    decision.backend_object,
                    &snapshot);
                decision.offset = backend_stat.size;
            } else {
                return filed_page_result(reply_status, bytes);
            }
        }
        reply_status = filed_cached_pwrite_ex(
            runtime,
            decision.backend_object,
            decision.offset,
            io->data,
            length,
            &bytes,
            true);
        if (reply_status == 0) {
            io->offset = decision.offset;
            io->length = bytes;
            status = filed_vfs_note_write(
                &runtime->vfs,
                (filed_handle_id_t)(uint32_t)io->handle,
                decision.offset,
                bytes);
            if (status != FILED_OK) {
                reply_status = filed_status_to_wire(status);
                return filed_page_result(reply_status, bytes);
            }
            filed_runtime_publish_backend_object_generation(
                runtime,
                decision.backend_object);
            status = filed_vfs_write_commit(
                &runtime->vfs,
                (filed_handle_id_t)(uint32_t)io->handle,
                bytes);
            if (status != FILED_OK) {
                reply_status = filed_status_to_wire(status);
            }
        }
    }
    return filed_page_result(reply_status, bytes);
}

filed_page_dispatch_result_t filed_dispatch_seek_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_seek_t *seek = (filed_seek_t *)page;
    filed_vfs_io_decision_t decision;
    uint64_t file_size = 0;
    int64_t new_offset = 0;
    filed_status_t status = FILED_ERR_INVALID;
    int64_t reply_status = -22;

    if (seek->reserved0 == 0 && seek->whence <= 2) {
        if (seek->whence == 2) {
            filed_vfs_stat_snapshot_t snapshot;
            status = filed_vfs_stat_prepare(
                &runtime->vfs,
                (filed_handle_id_t)(uint32_t)seek->handle,
                &decision);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK) {
                memset(&snapshot, 0, sizeof(snapshot));
                status = filed_vfs_get_stat_snapshot(
                    &runtime->vfs,
                    (filed_handle_id_t)(uint32_t)seek->handle,
                    &snapshot);
                reply_status = filed_status_to_wire(status);
            }
            if (status == FILED_OK && snapshot.valid) {
                file_size = snapshot.size;
            } else if (status == FILED_OK) {
                storage_statx_reply_t backend_stat;
                memset(&backend_stat, 0, sizeof(backend_stat));
                reply_status = filed_backend_statx(
                    runtime,
                    decision.backend_object,
                    &backend_stat);
                if (reply_status == 0) {
                    snapshot = filed_stat_snapshot_from_backend(
                        &backend_stat,
                        seek->handle,
                        decision.object_generation,
                        decision.dir_generation);
                    (void)filed_vfs_update_stat_snapshot(
                        &runtime->vfs,
                        decision.backend_object,
                        &snapshot);
                    file_size = backend_stat.size;
                }
            }
        } else {
            reply_status = 0;
        }

        if (reply_status == 0) {
            status = filed_vfs_seek(
                &runtime->vfs,
                (filed_handle_id_t)(uint32_t)seek->handle,
                seek->offset,
                (int)seek->whence,
                file_size,
                &new_offset);
            reply_status = filed_status_to_wire(status);
        }
    }
    return filed_page_result(reply_status, reply_status == 0 ? (uint64_t)new_offset : 0);
}

filed_page_dispatch_result_t filed_dispatch_fsync_page(
    filed_runtime_t *runtime,
    const struct pacha_ipc_msg *request)
{
    filed_vfs_io_decision_t decision;
    const filed_handle_id_t handle_id = (filed_handle_id_t)(uint32_t)request->word2;
    filed_status_t status = filed_vfs_fsync_prepare(
        &runtime->vfs,
        handle_id,
        &decision);
    int64_t reply_status = filed_status_to_wire(status);
    if (status == FILED_OK) {
        reply_status = filed_cache_flush_object(runtime, decision.backend_object);
        if (reply_status == 0) {
            reply_status = filed_backend_fsync(
                runtime,
                decision.backend_object);
        }
    }
    return filed_page_result(reply_status, 0);
}

int filed_dispatch_sync_all(filed_runtime_t *runtime)
{
    if (runtime == NULL) {
        return -22;
    }

    const uint64_t dirty_count = filed_cache_dirty_count(runtime);
    const uint64_t backend_dirty_hint = filed_kobox_backend_dirty_hint(&runtime->backend);
    const int flush_status = filed_cache_flush_object(runtime, 0);
    if (flush_status != 0) {
        printf(
            "[filed] sync_all page_cache_dirty=%llu backend_dirty_hint=%llu status=%d\n",
            (unsigned long long)dirty_count,
            (unsigned long long)backend_dirty_hint,
            flush_status);
        fflush(stdout);
        return flush_status;
    }
    const int backend_status = filed_kobox_backend_sync_all(&runtime->backend);
    if (backend_status != 0) {
        printf(
            "[filed] sync_all page_cache_dirty=%llu backend_dirty_hint=%llu backend_status=%d\n",
            (unsigned long long)dirty_count,
            (unsigned long long)backend_dirty_hint,
            backend_status);
        fflush(stdout);
    }
    return backend_status;
}

void filed_dispatch_log_state_checkpoint(filed_runtime_t *runtime, const char *source)
{
    if (runtime == NULL || source == NULL) {
        return;
    }
    uint32_t active_handles = 0;
    uint32_t active_sessions = 0;
    for (uint32_t i = 0; i < FILED_MAX_HANDLES; ++i)
        active_handles += runtime->vfs.handles[i].active ? 1u : 0u;
    for (uint32_t i = 0; i < FILED_RUNTIME_MAX_SESSIONS; ++i)
        active_sessions += runtime->sessions[i].active ? 1u : 0u;
    printf("[filed] state_checkpoint source=%s active_handles=%u active_sessions=%u\n",
           source, active_handles, active_sessions);
    filed_kobox_object_stats_t object_stats;
    if (filed_kobox_backend_object_stats(&runtime->backend, &object_stats) == 0) {
        printf(
            "[filed] kobox_objects source=%s used=%u referenced=%u cached=%u "
            "evictions=%llu capacity=%u\n",
            source,
            object_stats.used,
            object_stats.referenced,
            object_stats.cached,
            (unsigned long long)object_stats.evictions,
            object_stats.capacity);
    }
    uint32_t file_vmo_entries = 0;
    uint32_t file_vmo_shared = 0;
    uint64_t file_vmo_bytes = 0;
    for (uint32_t i = 0; i < FILED_RUNTIME_FILE_VMO_CACHE_SLOTS; ++i) {
        const filed_file_vmo_cache_entry_t *entry = &filed_file_vmo_cache.entries[i];
        if (!entry->active) {
            continue;
        }
        file_vmo_entries++;
        file_vmo_shared += entry->shared ? 1u : 0u;
        file_vmo_bytes = entry->length > UINT64_MAX - file_vmo_bytes ?
            UINT64_MAX : file_vmo_bytes + entry->length;
    }
    printf(
        "[filed] file_vmo_cache source=%s entries=%u shared=%u bytes=%llu "
        "hits=%llu misses=%llu stores=%llu evictions=%llu budget=%u\n",
        source,
        file_vmo_entries,
        file_vmo_shared,
        (unsigned long long)file_vmo_bytes,
        (unsigned long long)filed_file_vmo_cache_hits,
        (unsigned long long)filed_file_vmo_cache_misses,
        (unsigned long long)filed_file_vmo_cache_stores,
        (unsigned long long)filed_file_vmo_cache_evictions,
        (unsigned)FILED_FILE_VMO_CACHE_TOTAL_BYTES);
    fflush(stdout);
}

filed_page_dispatch_result_t filed_dispatch_truncate_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_truncate_t *truncate = (filed_truncate_t *)page;
    filed_vfs_io_decision_t decision;
    filed_status_t status = FILED_ERR_INVALID;
    int64_t reply_status = -22;
    if (truncate->reserved0 == 0 && truncate->reserved1 == 0) {
        status = filed_vfs_truncate_prepare(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)truncate->handle,
            truncate->size,
            &decision);
        reply_status = filed_status_to_wire(status);
        if (status == FILED_OK) {
            reply_status = filed_cache_flush_object(runtime, decision.backend_object);
            if (reply_status == 0) {
                reply_status = filed_backend_truncate(
                    runtime,
                    decision.backend_object,
                    truncate->size);
                if (reply_status == 0) {
                    filed_cache_invalidate(runtime, decision.backend_object);
                    (void)filed_vfs_note_truncate(
                        &runtime->vfs,
                        (filed_handle_id_t)(uint32_t)truncate->handle,
                        truncate->size);
                    filed_runtime_publish_backend_object_generation(
                        runtime,
                        decision.backend_object);
                }
            }
        }
    }

    return filed_page_result(reply_status, 0);
}

uint64_t filed_lookup_cache_target_object(
    filed_runtime_t *runtime,
    filed_handle_id_t parent_handle,
    uint64_t parent_backend_object,
    const char *name,
    bool *out_lookup_owned)
{
    uint64_t object_id = 0;
    if (out_lookup_owned != NULL) {
        *out_lookup_owned = false;
    }
    if (runtime == NULL || parent_backend_object == 0 || name == NULL ||
        out_lookup_owned == NULL)
    {
        return 0;
    }
    if (parent_handle != 0 &&
        filed_vfs_cached_child_backend_object(
            &runtime->vfs,
            parent_handle,
            name,
            &object_id) == FILED_OK &&
        object_id != 0)
    {
        filed_target_lookup_vfs_hits++;
        return object_id;
    }
    if (            filed_backend_lookup(
            runtime,
            parent_backend_object,
            name,
            &object_id) != 0)
    {
        filed_target_lookup_misses++;
        return 0;
    }
    if (object_id != 0) {
        *out_lookup_owned = true;
        filed_target_lookup_backend_hits++;
    } else {
        filed_target_lookup_misses++;
    }
    return object_id;
}

void filed_invalidate_mutated_object(
    filed_runtime_t *runtime,
    uint64_t backend_object)
{
    filed_cache_invalidate(runtime, backend_object);
}

int filed_flush_mutated_object(
    filed_runtime_t *runtime,
    uint64_t backend_object)
{
    return filed_cache_flush_object(runtime, backend_object);
}
