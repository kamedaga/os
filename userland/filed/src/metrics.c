
#include "dispatch/common.h"

void filed_dispatch_lock_acquire(filed_lock_t *lock)
{
    if (lock == NULL) {
        return;
    }
    while (atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire)) {
    }
}

void filed_dispatch_lock_release(filed_lock_t *lock)
{
    if (lock == NULL) {
        return;
    }
    atomic_flag_clear_explicit(&lock->flag, memory_order_release);
}

uint64_t filed_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t filed_read_tsc(void)
{
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
#else
    return 0;
#endif
}

const char *filed_op_name(uint64_t op)
{
    switch (op) {
    case FILED_V2_OP_HELLO: return "hello";
    case FILED_V2_OP_VFS_ROOT_STAT: return "root_stat";
    case FILED_V2_OP_VFS_ROOT_GETDENTS: return "root_getdents";
    case FILED_V2_OP_VFS_OPENAT: return "openat";
    case FILED_V2_OP_VFS_STAT: return "stat";
    case FILED_V2_OP_VFS_UTIMENS: return "utimens";
    case FILED_V2_OP_VFS_CHMOD: return "chmod";
    case FILED_V2_OP_VFS_PREAD: return "pread";
    case FILED_V2_OP_VFS_GETDENTS: return "getdents";
    case FILED_V2_OP_VFS_CLOSE: return "close";
    case FILED_V2_OP_EXEC_PATH: return "exec_path";
    case FILED_V2_OP_VFS_READ: return "read";
    case FILED_V2_OP_VFS_DUP: return "dup";
    case FILED_V2_OP_VFS_GET_FLAGS: return "get_flags";
    case FILED_V2_OP_VFS_SET_FLAGS: return "set_flags";
    case FILED_V2_OP_VFS_PWRITE: return "pwrite";
    case FILED_V2_OP_VFS_WRITE: return "write";
    case FILED_V2_OP_VFS_FSYNC: return "fsync";
    case FILED_V2_OP_VFS_TRUNCATE: return "truncate";
    case FILED_V2_OP_VFS_UNLINK: return "unlink";
    case FILED_V2_OP_VFS_RENAME: return "rename";
    case FILED_V2_OP_VFS_MKDIR: return "mkdir";
    case FILED_V2_OP_VFS_RMDIR: return "rmdir";
    case FILED_V2_OP_VFS_SYMLINK: return "symlink";
    case FILED_V2_OP_VFS_READLINK: return "readlink";
    case FILED_V2_OP_VFS_LINK: return "link";
    case FILED_V2_OP_VFS_SEEK: return "seek";
    case FILED_V2_OP_DIAG_DUMP_METRICS: return "dump_metrics";
    case FILED_V2_OP_DIAG_SET_CACHE_SLOTS: return "set_cache_slots";
    case FILED_V2_OP_SESSION_OPEN: return "connect";
    case FILED_V2_OP_SERVICE_SET_NETD_SOCKET: return "set_netd_socket_endpoint";
    case FILED_V2_OP_SERVICE_SET_TERMD_TTY: return "set_termd_tty_endpoint";
    case FILED_V2_OP_DIAG_PING: return "ping";
    case FILED_V2_OP_SESSION_DOORBELL: return "fast_doorbell";
    case FILED_V2_OP_VFS_VALIDATE_OPEN_CACHE: return "validate_open_cache";
    case FILED_V2_OP_VFS_PWRITE_BATCH: return "pwrite_batch";
    case FILED_V2_OP_VFS_WRITE_BATCH: return "write_batch";
    case FILED_V2_OP_VFS_PREAD_TO_VMO: return "pread_to_vmo";
    case FILED_V2_OP_VFS_FILE_VMO: return "file_vmo";
    case FILED_V2_OP_VFS_SHARED_FILE_VMO: return "shared_file_vmo";
    case FILED_V2_OP_VFS_MEMFD_CREATE: return "memfd_create";
    case FILED_V2_OP_VFS_SYNC_ALL: return "sync_all";
    case FILED_V2_OP_EXEC_SELF: return "exec_self";
    case FILED_V2_OP_SERVICE_REGISTER_TERMD_SIGNAL_SUPERVISOR:
        return "register_termd_signal_supervisor";
    case FILED_V2_OP_DIAG_ERROR_GET:
        return "error_get";
    default: return "unknown";
    }
}

void filed_record_dispatch_metric(
    filed_runtime_t *runtime,
    uint64_t op,
    uint64_t start_ns,
    uint64_t end_ns,
    uint64_t start_cycles,
    uint64_t end_cycles,
    int status)
{
    if (op >= FILED_METRIC_OP_MAX || start_ns == 0 || end_ns < start_ns) {
        return;
    }
    filed_dispatch_metric_t *metric = &filed_dispatch_metrics[op];
    const uint64_t elapsed_ns = end_ns - start_ns;
    metric->count++;
    metric->total_ns += elapsed_ns;
    if (elapsed_ns > metric->max_ns) {
        metric->max_ns = elapsed_ns;
    }
    if (start_cycles != 0 && end_cycles >= start_cycles) {
        const uint64_t elapsed_cycles = end_cycles - start_cycles;
        metric->total_cycles += elapsed_cycles;
        if (elapsed_cycles > metric->max_cycles) {
            metric->max_cycles = elapsed_cycles;
        }
    }
    if (status != 0) {
        metric->reply_errors++;
    }
}

void filed_record_dispatch_metric_cycles(
    filed_runtime_t *runtime,
    uint64_t op,
    uint64_t start_cycles,
    uint64_t end_cycles,
    int status)
{
    if (op >= FILED_METRIC_OP_MAX) {
        return;
    }
    filed_dispatch_metric_t *metric = &filed_dispatch_metrics[op];
    metric->count++;
    if (start_cycles != 0 && end_cycles >= start_cycles) {
        const uint64_t elapsed_cycles = end_cycles - start_cycles;
        metric->total_cycles += elapsed_cycles;
        if (elapsed_cycles > metric->max_cycles) {
            metric->max_cycles = elapsed_cycles;
        }
    }
    if (status != 0) {
        metric->reply_errors++;
    }
}

void filed_record_fast_op_metric_cycles(
    filed_runtime_t *runtime,
    uint64_t op,
    uint64_t start_cycles,
    uint64_t end_cycles,
    int status)
{
    if (op >= FILED_METRIC_OP_MAX) {
        return;
    }
    filed_fast_op_metric_t *metric = &filed_fast_op_metrics[op];
    metric->count++;
    if (start_cycles != 0 && end_cycles >= start_cycles) {
        const uint64_t elapsed_cycles = end_cycles - start_cycles;
        metric->total_cycles += elapsed_cycles;
        if (elapsed_cycles > metric->max_cycles) {
            metric->max_cycles = elapsed_cycles;
        }
    }
    if (status != 0) {
        metric->errors++;
    }
}

filed_v2_generation_entry_t *filed_session_generation_entries(
    const filed_session_t *session,
    const filed_v2_fast_header_t *header)
{
    if (session == NULL ||
        header == NULL ||
        session->page == NULL ||
        header->generation_offset != FILED_V2_FAST_GENERATION_OFFSET ||
        header->generation_capacity != FILED_V2_FAST_GENERATION_CAPACITY ||
        header->generation_offset + header->generation_capacity * sizeof(filed_v2_generation_entry_t) > session->page_size)
    {
        return NULL;
    }
    return (filed_v2_generation_entry_t *)((uint8_t *)session->page + header->generation_offset);
}

void filed_session_publish_generation(
    filed_session_t *session,
    filed_handle_id_t handle_id,
    filed_generation_t object_generation,
    filed_generation_t dir_generation)
{
    if (session == NULL ||
        !session->active ||
        handle_id == 0 ||
        object_generation == 0)
    {
        return;
    }

    filed_v2_fast_header_t *header = (filed_v2_fast_header_t *)session->page;
    filed_v2_generation_entry_t *entries =
        filed_session_generation_entries(session, header);
    if (entries == NULL) {
        return;
    }

    uint64_t free_slot = header->generation_capacity;
    for (uint64_t i = 0; i < header->generation_capacity; ++i) {
        if (entries[i].handle == (uint64_t)handle_id) {
            free_slot = i;
            break;
        }
        if (free_slot == header->generation_capacity && entries[i].handle == 0) {
            free_slot = i;
        }
    }
    if (free_slot == header->generation_capacity) {
        free_slot = ((uint64_t)handle_id) % header->generation_capacity;
    }

    filed_v2_generation_entry_t *entry = &entries[free_slot];
    ++entry->seq;
    __sync_synchronize();
    entry->handle = handle_id;
    entry->object_generation = object_generation;
    entry->dir_generation = dir_generation;
    __sync_synchronize();
    ++entry->seq;
}

void filed_runtime_publish_generation(
    filed_runtime_t *runtime,
    filed_handle_id_t handle_id,
    filed_generation_t object_generation,
    filed_generation_t dir_generation)
{
    if (runtime == NULL || handle_id == 0 || object_generation == 0) {
        return;
    }
    for (uint64_t i = 0; i < FILED_RUNTIME_MAX_SESSIONS; ++i) {
        filed_session_publish_generation(
            &runtime->sessions[i],
            handle_id,
            object_generation,
            dir_generation);
    }
}

filed_vnode_t *filed_dispatch_find_vnode_by_id(
    filed_vfs_t *vfs,
    filed_vnode_id_t vnode_id)
{
    if (vfs == NULL || vnode_id == 0) {
        return NULL;
    }
    for (uint32_t i = 0; i < FILED_MAX_VNODES; ++i) {
        if (vfs->vnodes[i].active && vfs->vnodes[i].id == vnode_id) {
            return &vfs->vnodes[i];
        }
    }
    return NULL;
}

filed_open_file_t *filed_dispatch_find_file_by_id(
    filed_vfs_t *vfs,
    filed_file_id_t file_id)
{
    if (vfs == NULL || file_id == 0) {
        return NULL;
    }
    for (uint32_t i = 0; i < FILED_MAX_FILES; ++i) {
        if (vfs->files[i].active && vfs->files[i].id == file_id) {
            return &vfs->files[i];
        }
    }
    return NULL;
}

filed_vnode_t *filed_dispatch_handle_vnode(
    filed_vfs_t *vfs,
    const filed_handle_t *handle)
{
    if (vfs == NULL || handle == NULL || !handle->active) {
        return NULL;
    }
    if (handle->target_kind == FILED_HANDLE_VNODE) {
        return filed_dispatch_find_vnode_by_id(vfs, handle->target_id);
    }
    if (handle->target_kind == FILED_HANDLE_FILE) {
        filed_open_file_t *file =
            filed_dispatch_find_file_by_id(vfs, handle->target_id);
        if (file == NULL) {
            return NULL;
        }
        return filed_dispatch_find_vnode_by_id(vfs, file->vnode_id);
    }
    return NULL;
}

void filed_runtime_publish_backend_object_generation(
    filed_runtime_t *runtime,
    filed_backend_object_id_t backend_object)
{
    if (runtime == NULL || backend_object == 0) {
        return;
    }
    filed_exec_invalidate_backend_object(runtime, backend_object);
    for (uint32_t i = 0; i < FILED_MAX_HANDLES; ++i) {
        filed_handle_t *handle = &runtime->vfs.handles[i];
        filed_vnode_t *vnode = filed_dispatch_handle_vnode(&runtime->vfs, handle);
        if (vnode == NULL || vnode->backend_object != backend_object) {
            continue;
        }
        filed_dispatch_lock_acquire(&vnode->lock);
        const filed_generation_t object_generation = vnode->object_generation;
        const filed_generation_t dir_generation = vnode->dir_generation;
        filed_dispatch_lock_release(&vnode->lock);
        filed_runtime_publish_generation(
            runtime,
            handle->id,
            object_generation,
            dir_generation);
    }
}

void filed_dump_dispatch_metrics(filed_runtime_t *runtime)
{
    for (uint64_t op = 0; op < FILED_METRIC_OP_MAX; ++op) {
        const filed_dispatch_metric_t *metric = &filed_dispatch_metrics[op];
        if (metric->count == 0) {
            continue;
        }
        (void)filed_op_name(op);
        pacha_trace6(
            PACHA_TRACE_COMPONENT_FILED,
            PACHA_TRACE_EVENT_FILED_METRIC_DISPATCH,
            PACHA_TRACE_CLASS_METRIC,
            op,
            metric->count,
            metric->total_ns / metric->count,
            metric->max_ns,
            metric->total_cycles / metric->count,
            metric->max_cycles);
        pacha_trace2(
            PACHA_TRACE_COMPONENT_FILED,
            PACHA_TRACE_EVENT_FILED_METRIC_DISPATCH,
            PACHA_TRACE_CLASS_METRIC,
            op,
            metric->reply_errors);
    }
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_FAST, PACHA_TRACE_CLASS_METRIC, 1, filed_fast_metrics.enqueued);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_FAST, PACHA_TRACE_CLASS_METRIC, 2, filed_fast_metrics.completed);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_FAST, PACHA_TRACE_CLASS_METRIC, 3, filed_fast_metrics.batches);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_FAST, PACHA_TRACE_CLASS_METRIC, 4, filed_fast_metrics.ring_full);
    if (filed_fast_metrics.doorbells != 0) {
        pacha_trace6(
            PACHA_TRACE_COMPONENT_FILED,
            PACHA_TRACE_EVENT_FILED_METRIC_FAST,
            PACHA_TRACE_CLASS_METRIC,
            5,
            filed_fast_metrics.doorbells,
            filed_fast_metrics.recv_total_ns / filed_fast_metrics.doorbells,
            filed_fast_metrics.recv_max_ns,
            filed_fast_metrics.recv_total_cycles / filed_fast_metrics.doorbells,
            filed_fast_metrics.recv_max_cycles);
        pacha_trace6(
            PACHA_TRACE_COMPONENT_FILED,
            PACHA_TRACE_EVENT_FILED_METRIC_FAST,
            PACHA_TRACE_CLASS_METRIC,
            6,
            filed_fast_metrics.doorbells,
            filed_fast_metrics.drain_total_ns / filed_fast_metrics.doorbells,
            filed_fast_metrics.drain_max_ns,
            filed_fast_metrics.drain_total_cycles / filed_fast_metrics.doorbells,
            filed_fast_metrics.drain_max_cycles);
    }
    if (filed_fast_metrics.doorbells != 0) {
        pacha_trace6(
            PACHA_TRACE_COMPONENT_FILED,
            PACHA_TRACE_EVENT_FILED_METRIC_FAST,
            PACHA_TRACE_CLASS_METRIC,
            7,
            filed_fast_metrics.doorbells,
            filed_fast_metrics.reply_total_ns / filed_fast_metrics.doorbells,
            filed_fast_metrics.reply_max_ns,
            filed_fast_metrics.reply_total_cycles / filed_fast_metrics.doorbells,
            filed_fast_metrics.reply_max_cycles);
    }
    for (uint64_t op = 0; op < FILED_METRIC_OP_MAX; ++op) {
        const filed_fast_op_metric_t *metric = &filed_fast_op_metrics[op];
        if (metric->count == 0) {
            continue;
        }
        (void)filed_op_name(op);
        pacha_trace6(
            PACHA_TRACE_COMPONENT_FILED,
            PACHA_TRACE_EVENT_FILED_METRIC_FAST_OP,
            PACHA_TRACE_CLASS_METRIC,
            op,
            metric->count,
            metric->total_ns / metric->count,
            metric->max_ns,
            metric->total_cycles / metric->count,
            metric->max_cycles);
        pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_FAST_OP, PACHA_TRACE_CLASS_METRIC, op, metric->errors);
    }
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_LOOKUP, PACHA_TRACE_CLASS_METRIC, 1, filed_target_lookup_vfs_hits);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_LOOKUP, PACHA_TRACE_CLASS_METRIC, 2, filed_target_lookup_backend_hits);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_LOOKUP, PACHA_TRACE_CLASS_METRIC, 3, filed_target_lookup_misses);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_LOOKUP, PACHA_TRACE_CLASS_METRIC, 4, filed_negative_lookup_cache.hits);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_LOOKUP, PACHA_TRACE_CLASS_METRIC, 5, filed_negative_lookup_cache.misses);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_LOOKUP, PACHA_TRACE_CLASS_METRIC, 6, filed_negative_lookup_cache.stores);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_LOOKUP, PACHA_TRACE_CLASS_METRIC, 7, filed_negative_lookup_cache.evictions);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_FILE_VMO, PACHA_TRACE_CLASS_METRIC, 1, filed_file_vmo_cache_hits);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_FILE_VMO, PACHA_TRACE_CLASS_METRIC, 2, filed_file_vmo_cache_misses);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_FILE_VMO, PACHA_TRACE_CLASS_METRIC, 3, filed_file_vmo_cache_stores);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_FILE_VMO, PACHA_TRACE_CLASS_METRIC, 4, filed_file_vmo_cache_evictions);
}
