#include "filed/dispatch.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "filed/fd_ipc.h"
#include "filed/exec.h"
#include "filed/exec_linux_lpr.h"
#include "filed/payload_v2.h"
#include "filed/ipc_protocol_v2.h"
#include "filed/page_cache.h"
#include "filed/tmpfs_backend.h"
#include "pacha/abi.h"
#include "pacha/error_conveyor.h"
#include "pacha/ipc.h"
#include "pacha/syscall.h"
#include "pacha/trace.h"
#include "personality/linux_lpr.h"
#include "termd/ipc_protocol_v2.h"

enum {
    FILED_BOOTSTRAP_PATCH_BYTES = 4096,
    FILED_EXEC_MAX_FDS = 256,
    FILED_EXEC_FILED_ENDPOINT_FD = 240,
    FILED_EXEC_NETD_SOCKET_ENDPOINT_FD = 241,
    FILED_EXEC_TERMD_TTY_ENDPOINT_FD = 242,
    FILED_EXEC_LPR_BOOTSTRAP_FD = LPR_BOOTSTRAP_FD,
    FILED_METRIC_OP_MAX = 0x8000u,
    FILED_PAGE_CACHE_BYTES = 16384,
    FILED_PAGE_CACHE_SLOTS = 64,
    FILED_DIR_CACHE_SLOTS = 32,
    FILED_NEGATIVE_LOOKUP_CACHE_SLOTS = 64,
    FILED_FILE_VMO_MAX_BYTES = 8u * 1024u * 1024u,
};

typedef struct filed_dispatch_metric {
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t total_cycles;
    uint64_t max_cycles;
    uint64_t reply_errors;
} filed_dispatch_metric_t;

typedef struct filed_fast_metric {
    uint64_t enqueued;
    uint64_t completed;
    uint64_t batches;
    uint64_t ring_full;
    uint64_t doorbells;
    uint64_t recv_total_ns;
    uint64_t recv_max_ns;
    uint64_t drain_total_ns;
    uint64_t drain_max_ns;
    uint64_t reply_total_ns;
    uint64_t reply_max_ns;
    uint64_t recv_total_cycles;
    uint64_t recv_max_cycles;
    uint64_t drain_total_cycles;
    uint64_t drain_max_cycles;
    uint64_t reply_total_cycles;
    uint64_t reply_max_cycles;
} filed_fast_metric_t;

typedef struct filed_fast_op_metric {
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t total_cycles;
    uint64_t max_cycles;
    uint64_t errors;
} filed_fast_op_metric_t;

typedef struct filed_page_cache_slot {
    bool valid;
    bool dirty;
    uint64_t backend_object;
    uint64_t page_index;
    uint64_t last_used;
    uint32_t bytes;
    uint32_t valid_start;
    uint32_t dirty_start;
    uint32_t dirty_end;
    uint8_t data[FILED_PAGE_CACHE_BYTES];
} filed_page_cache_slot_t;

typedef struct filed_page_cache {
    filed_page_cache_slot_t slots[FILED_PAGE_CACHE_SLOTS];
    uint64_t clock;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t direct_reads;
    uint64_t dirty_writes;
    uint64_t flushes;
    uint64_t flush_errors;
    uint64_t active_slots;
    bool configured;
} filed_page_cache_t;

typedef struct filed_dir_cache_slot {
    bool valid;
    uint64_t backend_object;
    uint64_t offset;
    uint64_t last_used;
    storage_v2_getdents_request_t entries;
} filed_dir_cache_slot_t;

typedef struct filed_dir_cache {
    filed_dir_cache_slot_t slots[FILED_DIR_CACHE_SLOTS];
    uint64_t clock;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
} filed_dir_cache_t;

typedef struct filed_negative_lookup_cache_slot {
    bool valid;
    uint64_t parent_backend_object;
    filed_generation_t parent_dir_generation;
    uint64_t last_used;
    int64_t status;
    char name[FILED_V2_NAME_BYTES];
} filed_negative_lookup_cache_slot_t;

typedef struct filed_negative_lookup_cache {
    filed_negative_lookup_cache_slot_t slots[FILED_NEGATIVE_LOOKUP_CACHE_SLOTS];
    uint64_t clock;
    uint64_t hits;
    uint64_t misses;
    uint64_t stores;
    uint64_t evictions;
} filed_negative_lookup_cache_t;

static filed_dispatch_metric_t filed_dispatch_metrics[FILED_METRIC_OP_MAX];
static filed_fast_metric_t filed_fast_metrics;
static filed_fast_op_metric_t filed_fast_op_metrics[FILED_METRIC_OP_MAX];
static filed_page_cache_t filed_page_cache;
static filed_dir_cache_t filed_dir_cache;
static filed_negative_lookup_cache_t filed_negative_lookup_cache;
static uint64_t filed_target_lookup_vfs_hits;
static uint64_t filed_target_lookup_backend_hits;
static uint64_t filed_target_lookup_misses;
static uint64_t filed_file_vmo_cache_hits;
static uint64_t filed_file_vmo_cache_misses;
static uint64_t filed_file_vmo_cache_stores;
static uint64_t filed_file_vmo_cache_evictions;
static pacha_errconv_store_t filed_errconv_store;
static int filed_errconv_store_ready;

static void filed_file_vmo_cache_invalidate_object(filed_runtime_t *runtime, uint64_t backend_object);

static bool filed_backend_object_is_tmpfs(uint64_t backend_object)
{
    return backend_object != 0 && filed_tmpfs_backend_is_object(backend_object);
}

static int filed_backend_lookup(
    filed_runtime_t *runtime,
    uint64_t parent_object_id,
    const char *name,
    uint64_t *out_object_id)
{
    if (filed_tmpfs_backend_is_object(parent_object_id)) {
        return filed_tmpfs_backend_lookup(&runtime->tmpfs, parent_object_id, name, out_object_id);
    }
    return filed_kobox_backend_lookup(&runtime->backend, parent_object_id, name, out_object_id);
}

static int filed_backend_statx(
    filed_runtime_t *runtime,
    uint64_t object_id,
    storage_v2_statx_reply_t *out_stat)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_statx(&runtime->tmpfs, object_id, out_stat);
    }
    return filed_kobox_backend_statx(&runtime->backend, object_id, out_stat);
}

static int filed_backend_pread(
    filed_runtime_t *runtime,
    uint64_t object_id,
    uint64_t offset,
    void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_pread(&runtime->tmpfs, object_id, offset, buffer, length, out_bytes);
    }
    return filed_kobox_backend_pread(&runtime->backend, object_id, offset, buffer, length, out_bytes);
}

static int filed_backend_pwrite(
    filed_runtime_t *runtime,
    uint64_t object_id,
    uint64_t offset,
    const void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_pwrite(&runtime->tmpfs, object_id, offset, buffer, length, out_bytes);
    }
    return filed_kobox_backend_pwrite(&runtime->backend, object_id, offset, buffer, length, out_bytes);
}

static int filed_backend_fsync(filed_runtime_t *runtime, uint64_t object_id)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_fsync(&runtime->tmpfs, object_id);
    }
    return filed_kobox_backend_fsync(&runtime->backend, object_id);
}

static int filed_backend_create(
    filed_runtime_t *runtime,
    uint64_t parent_object_id,
    const char *name,
    uint64_t mode,
    uint64_t *out_object_id)
{
    if (filed_tmpfs_backend_is_object(parent_object_id)) {
        return filed_tmpfs_backend_create(&runtime->tmpfs, parent_object_id, name, mode, out_object_id);
    }
    return filed_kobox_backend_create(&runtime->backend, parent_object_id, name, mode, out_object_id);
}

static int filed_backend_truncate(
    filed_runtime_t *runtime,
    uint64_t object_id,
    uint64_t size)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_truncate(&runtime->tmpfs, object_id, size);
    }
    return filed_kobox_backend_truncate(&runtime->backend, object_id, size);
}

static int filed_backend_utimens(
    filed_runtime_t *runtime,
    uint64_t object_id,
    uint32_t mask,
    int64_t atime_sec,
    int64_t atime_nsec,
    int64_t mtime_sec,
    int64_t mtime_nsec)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_utimens(
            &runtime->tmpfs,
            object_id,
            mask,
            atime_sec,
            atime_nsec,
            mtime_sec,
            mtime_nsec);
    }
    return filed_kobox_backend_utimens(
        &runtime->backend,
        object_id,
        mask,
        atime_sec,
        atime_nsec,
        mtime_sec,
        mtime_nsec);
}

static int filed_backend_chmod(
    filed_runtime_t *runtime,
    uint64_t object_id,
    uint64_t mode)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_chmod(&runtime->tmpfs, object_id, mode);
    }
    return filed_kobox_backend_chmod(&runtime->backend, object_id, mode);
}

static int filed_backend_unlink(
    filed_runtime_t *runtime,
    uint64_t parent_object_id,
    const char *name)
{
    if (filed_tmpfs_backend_is_object(parent_object_id)) {
        return filed_tmpfs_backend_unlink(&runtime->tmpfs, parent_object_id, name);
    }
    return filed_kobox_backend_unlink(&runtime->backend, parent_object_id, name);
}

static int filed_backend_link(
    filed_runtime_t *runtime,
    uint64_t old_object_id,
    uint64_t new_parent_object_id,
    const char *new_name,
    uint64_t *out_object_id)
{
    if (filed_tmpfs_backend_is_object(old_object_id) &&
        filed_tmpfs_backend_is_object(new_parent_object_id))
    {
        return filed_tmpfs_backend_link(
            &runtime->tmpfs,
            old_object_id,
            new_parent_object_id,
            new_name,
            out_object_id);
    }
    if (filed_tmpfs_backend_is_object(old_object_id) ||
        filed_tmpfs_backend_is_object(new_parent_object_id))
    {
        return -18;
    }
    (void)runtime;
    (void)old_object_id;
    (void)new_parent_object_id;
    (void)new_name;
    (void)out_object_id;
    return -95;
}

static int filed_backend_mkdir(
    filed_runtime_t *runtime,
    uint64_t parent_object_id,
    const char *name,
    uint64_t mode,
    uint64_t *out_object_id)
{
    if (filed_tmpfs_backend_is_object(parent_object_id)) {
        return filed_tmpfs_backend_mkdir(&runtime->tmpfs, parent_object_id, name, mode, out_object_id);
    }
    return filed_kobox_backend_mkdir(&runtime->backend, parent_object_id, name, mode, out_object_id);
}

static int filed_backend_symlink(
    filed_runtime_t *runtime,
    uint64_t parent_object_id,
    const char *name,
    const char *target,
    uint64_t target_length,
    uint64_t *out_object_id)
{
    if (filed_tmpfs_backend_is_object(parent_object_id)) {
        return filed_tmpfs_backend_symlink(
            &runtime->tmpfs,
            parent_object_id,
            name,
            target,
            target_length,
            out_object_id);
    }
    (void)runtime;
    (void)parent_object_id;
    (void)name;
    (void)target;
    (void)target_length;
    (void)out_object_id;
    return -95;
}

static int filed_backend_readlink(
    filed_runtime_t *runtime,
    uint64_t object_id,
    char *out_target,
    uint64_t target_capacity,
    uint64_t *out_length)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_readlink(
            &runtime->tmpfs,
            object_id,
            out_target,
            target_capacity,
            out_length);
    }
    (void)runtime;
    (void)object_id;
    (void)out_target;
    (void)target_capacity;
    (void)out_length;
    return -95;
}

static int filed_backend_rmdir(
    filed_runtime_t *runtime,
    uint64_t parent_object_id,
    const char *name)
{
    if (filed_tmpfs_backend_is_object(parent_object_id)) {
        return filed_tmpfs_backend_rmdir(&runtime->tmpfs, parent_object_id, name);
    }
    return filed_kobox_backend_rmdir(&runtime->backend, parent_object_id, name);
}

static int filed_backend_rename(
    filed_runtime_t *runtime,
    uint64_t old_parent_object_id,
    const char *old_name,
    uint64_t new_parent_object_id,
    const char *new_name,
    uint64_t *out_object_id)
{
    const bool old_tmpfs = filed_tmpfs_backend_is_object(old_parent_object_id);
    const bool new_tmpfs = filed_tmpfs_backend_is_object(new_parent_object_id);
    if (old_tmpfs != new_tmpfs) {
        return -18;
    }
    if (old_tmpfs) {
        return filed_tmpfs_backend_rename(
            &runtime->tmpfs,
            old_parent_object_id,
            old_name,
            new_parent_object_id,
            new_name,
            out_object_id);
    }
    return filed_kobox_backend_rename(
        &runtime->backend,
        old_parent_object_id,
        old_name,
        new_parent_object_id,
        new_name,
        out_object_id);
}

static int filed_backend_release_object(filed_runtime_t *runtime, uint64_t object_id)
{
    if (filed_tmpfs_backend_is_object(object_id)) {
        return filed_tmpfs_backend_release_object(&runtime->tmpfs, object_id);
    }
    return filed_kobox_backend_release_object(&runtime->backend, object_id);
}

static int filed_backend_getdents(
    filed_runtime_t *runtime,
    uint64_t dir_object_id,
    uint64_t offset,
    storage_v2_getdents_request_t *out_entries)
{
    if (filed_tmpfs_backend_is_object(dir_object_id)) {
        return filed_tmpfs_backend_getdents(&runtime->tmpfs, dir_object_id, offset, out_entries);
    }
    return filed_kobox_backend_getdents(&runtime->backend, dir_object_id, offset, out_entries);
}

static bool filed_root_getdents_splices_tmpfs(
    filed_runtime_t *runtime,
    uint64_t dir_object_id)
{
    if (runtime == NULL ||
        dir_object_id != runtime->backend.root_object_id ||
        filed_tmpfs_backend_root_object(&runtime->tmpfs) == 0 ||
        runtime->root_tmpfs_synthetic_dirent == 0)
    {
        return false;
    }
    return true;
}

static uint64_t filed_root_getdents_backend_offset(
    filed_runtime_t *runtime,
    uint64_t dir_object_id,
    uint64_t logical_offset)
{
    if (filed_root_getdents_splices_tmpfs(runtime, dir_object_id) && logical_offset > 0) {
        return logical_offset - 1u;
    }
    return logical_offset;
}

static void filed_dispatch_lock_acquire(filed_lock_t *lock)
{
    if (lock == NULL) {
        return;
    }
    while (atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire)) {
    }
}

static void filed_dispatch_lock_release(filed_lock_t *lock)
{
    if (lock == NULL) {
        return;
    }
    atomic_flag_clear_explicit(&lock->flag, memory_order_release);
}

void filed_page_cache_configure(uint64_t active_slots)
{
    if (active_slots > FILED_PAGE_CACHE_SLOTS) {
        active_slots = FILED_PAGE_CACHE_SLOTS;
    }
    if (!filed_page_cache.configured ||
        filed_page_cache.active_slots != active_slots)
    {
        for (size_t i = 0; i < FILED_PAGE_CACHE_SLOTS; ++i) {
            memset(&filed_page_cache.slots[i], 0, sizeof(filed_page_cache.slots[i]));
        }
    }
    filed_page_cache.active_slots = active_slots;
    filed_page_cache.configured = true;
}

static void filed_page_cache_ensure_configured(void)
{
    if (!filed_page_cache.configured) {
        filed_page_cache_configure(FILED_PAGE_CACHE_SLOTS);
    }
}

static uint64_t filed_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t filed_read_tsc(void)
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

static const char *filed_op_name(uint64_t op)
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
    case FILED_V2_OP_VFS_SYNC_ALL: return "sync_all";
    case FILED_V2_OP_EXEC_SELF: return "exec_self";
    case FILED_V2_OP_SERVICE_REGISTER_TERMD_SIGNAL_SUPERVISOR:
        return "register_termd_signal_supervisor";
    case FILED_V2_OP_DIAG_ERROR_GET:
        return "error_get";
    default: return "unknown";
    }
}

static void filed_record_dispatch_metric(
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

static void filed_record_dispatch_metric_cycles(
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

static void filed_record_fast_op_metric_cycles(
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

static filed_v2_generation_entry_t *filed_session_generation_entries(
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

static void filed_session_publish_generation(
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

static void filed_runtime_publish_generation(
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

static filed_vnode_t *filed_dispatch_find_vnode_by_id(
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

static filed_open_file_t *filed_dispatch_find_file_by_id(
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

static filed_vnode_t *filed_dispatch_handle_vnode(
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

static void filed_runtime_publish_backend_object_generation(
    filed_runtime_t *runtime,
    filed_backend_object_id_t backend_object)
{
    if (runtime == NULL || backend_object == 0) {
        return;
    }
    filed_exec_invalidate_backend_object(runtime, backend_object);
    filed_file_vmo_cache_invalidate_object(runtime, backend_object);
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

static void filed_dump_dispatch_metrics(void)
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

static uint64_t filed_page_cache_next_clock(filed_runtime_t *runtime)
{
    (void)runtime;
    if (filed_page_cache.clock == UINT64_MAX) {
        filed_page_cache.clock = 0;
    }
    ++filed_page_cache.clock;
    if (filed_page_cache.clock == 0) {
        filed_page_cache.clock = 1;
    }
    return filed_page_cache.clock;
}

static uint64_t filed_dir_cache_next_clock(void)
{
    if (filed_dir_cache.clock == UINT64_MAX) {
        filed_dir_cache.clock = 0;
    }
    ++filed_dir_cache.clock;
    if (filed_dir_cache.clock == 0) {
        filed_dir_cache.clock = 1;
    }
    return filed_dir_cache.clock;
}

static uint64_t filed_negative_lookup_cache_next_clock(void)
{
    if (filed_negative_lookup_cache.clock == UINT64_MAX) {
        filed_negative_lookup_cache.clock = 0;
    }
    ++filed_negative_lookup_cache.clock;
    if (filed_negative_lookup_cache.clock == 0) {
        filed_negative_lookup_cache.clock = 1;
    }
    return filed_negative_lookup_cache.clock;
}

static size_t filed_lookup_name_len(const char *name)
{
    if (name == NULL) {
        return 0;
    }
    return strnlen(name, FILED_V2_NAME_BYTES);
}

static filed_negative_lookup_cache_slot_t *filed_negative_lookup_cache_find(
    uint64_t parent_backend_object,
    filed_generation_t parent_dir_generation,
    const char *name,
    size_t name_len)
{
    if (parent_backend_object == 0 ||
        name == NULL ||
        name_len == 0 ||
        name_len >= FILED_V2_NAME_BYTES)
    {
        return NULL;
    }
    for (size_t i = 0; i < FILED_NEGATIVE_LOOKUP_CACHE_SLOTS; ++i) {
        filed_negative_lookup_cache_slot_t *slot = &filed_negative_lookup_cache.slots[i];
        if (slot->valid &&
            slot->parent_backend_object == parent_backend_object &&
            slot->parent_dir_generation == parent_dir_generation &&
            strncmp(slot->name, name, FILED_V2_NAME_BYTES) == 0)
        {
            return slot;
        }
    }
    return NULL;
}

static filed_negative_lookup_cache_slot_t *filed_negative_lookup_cache_choose_slot(void)
{
    filed_negative_lookup_cache_slot_t *oldest = NULL;
    for (size_t i = 0; i < FILED_NEGATIVE_LOOKUP_CACHE_SLOTS; ++i) {
        filed_negative_lookup_cache_slot_t *slot = &filed_negative_lookup_cache.slots[i];
        if (!slot->valid) {
            return slot;
        }
        if (oldest == NULL || slot->last_used < oldest->last_used) {
            oldest = slot;
        }
    }
    if (oldest != NULL) {
        filed_negative_lookup_cache.evictions++;
    }
    return oldest;
}

static bool filed_negative_lookup_cache_get(
    uint64_t parent_backend_object,
    filed_generation_t parent_dir_generation,
    const char *name,
    int64_t *out_status)
{
    const size_t name_len = filed_lookup_name_len(name);
    filed_negative_lookup_cache_slot_t *slot = filed_negative_lookup_cache_find(
        parent_backend_object,
        parent_dir_generation,
        name,
        name_len);
    if (slot == NULL) {
        filed_negative_lookup_cache.misses++;
        return false;
    }
    slot->last_used = filed_negative_lookup_cache_next_clock();
    if (out_status != NULL) {
        *out_status = slot->status;
    }
    filed_negative_lookup_cache.hits++;
    return true;
}

static void filed_negative_lookup_cache_store(
    uint64_t parent_backend_object,
    filed_generation_t parent_dir_generation,
    const char *name,
    int64_t status)
{
    const size_t name_len = filed_lookup_name_len(name);
    filed_negative_lookup_cache_slot_t *slot;
    if (parent_backend_object == 0 ||
        name == NULL ||
        name_len == 0 ||
        name_len >= FILED_V2_NAME_BYTES ||
        status == 0)
    {
        return;
    }
    slot = filed_negative_lookup_cache_find(
        parent_backend_object,
        parent_dir_generation,
        name,
        name_len);
    if (slot == NULL) {
        slot = filed_negative_lookup_cache_choose_slot();
    }
    if (slot == NULL) {
        return;
    }

    memset(slot, 0, sizeof(*slot));
    slot->valid = true;
    slot->parent_backend_object = parent_backend_object;
    slot->parent_dir_generation = parent_dir_generation;
    slot->last_used = filed_negative_lookup_cache_next_clock();
    slot->status = status;
    memcpy(slot->name, name, name_len);
    slot->name[name_len] = '\0';
    filed_negative_lookup_cache.stores++;
}

static filed_dir_cache_slot_t *filed_dir_cache_find(
    uint64_t backend_object,
    uint64_t offset)
{
    if (backend_object == 0) {
        return NULL;
    }
    for (size_t i = 0; i < FILED_DIR_CACHE_SLOTS; ++i) {
        filed_dir_cache_slot_t *slot = &filed_dir_cache.slots[i];
        if (slot->valid &&
            slot->backend_object == backend_object &&
            slot->offset == offset)
        {
            return slot;
        }
    }
    return NULL;
}

static filed_dir_cache_slot_t *filed_dir_cache_choose_slot(void)
{
    filed_dir_cache_slot_t *oldest = NULL;
    for (size_t i = 0; i < FILED_DIR_CACHE_SLOTS; ++i) {
        filed_dir_cache_slot_t *slot = &filed_dir_cache.slots[i];
        if (!slot->valid) {
            return slot;
        }
        if (oldest == NULL || slot->last_used < oldest->last_used) {
            oldest = slot;
        }
    }
    if (oldest != NULL) {
        filed_dir_cache.evictions++;
    }
    return oldest;
}

static int filed_dir_cache_get(
    uint64_t backend_object,
    uint64_t offset,
    storage_v2_getdents_request_t *out_entries)
{
    filed_dir_cache_slot_t *slot;
    if (out_entries == NULL) {
        return 0;
    }
    slot = filed_dir_cache_find(backend_object, offset);
    if (slot == NULL) {
        filed_dir_cache.misses++;
        return 0;
    }
    if (offset == 0 && slot->entries.count == 0) {
        memset(slot, 0, sizeof(*slot));
        filed_dir_cache.misses++;
        return 0;
    }
    *out_entries = slot->entries;
    slot->last_used = filed_dir_cache_next_clock();
    filed_dir_cache.hits++;
    return 1;
}

static void filed_dir_cache_store(
    uint64_t backend_object,
    uint64_t offset,
    const storage_v2_getdents_request_t *entries)
{
    filed_dir_cache_slot_t *slot;
    if (backend_object == 0 || entries == NULL) {
        return;
    }
    if (offset == 0 && entries->count == 0) {
        return;
    }
    slot = filed_dir_cache_find(backend_object, offset);
    if (slot == NULL) {
        slot = filed_dir_cache_choose_slot();
    }
    if (slot == NULL) {
        return;
    }

    memset(slot, 0, sizeof(*slot));
    slot->valid = true;
    slot->backend_object = backend_object;
    slot->offset = offset;
    slot->last_used = filed_dir_cache_next_clock();
    slot->entries = *entries;
}

static void filed_dir_cache_invalidate_dir(uint64_t backend_object)
{
    if (backend_object == 0) {
        return;
    }
    for (size_t i = 0; i < FILED_DIR_CACHE_SLOTS; ++i) {
        filed_dir_cache_slot_t *slot = &filed_dir_cache.slots[i];
        if (slot->valid && slot->backend_object == backend_object) {
            memset(slot, 0, sizeof(*slot));
        }
    }
}

static filed_page_cache_slot_t *filed_page_cache_find(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t page_index)
{
    (void)runtime;
    filed_page_cache_ensure_configured();
    if (backend_object == 0) {
        return 0;
    }
    for (size_t i = 0; i < filed_page_cache.active_slots; ++i) {
        filed_page_cache_slot_t *slot = &filed_page_cache.slots[i];
        if (slot->valid &&
            slot->backend_object == backend_object &&
            slot->page_index == page_index)
        {
            return slot;
        }
    }
    return NULL;
}

static bool filed_page_cache_object_dirty(uint64_t backend_object)
{
    filed_page_cache_ensure_configured();
    if (filed_backend_object_is_tmpfs(backend_object)) {
        return false;
    }
    if (backend_object == 0 || filed_page_cache.active_slots == 0) {
        return false;
    }
    for (size_t i = 0; i < filed_page_cache.active_slots; ++i) {
        const filed_page_cache_slot_t *slot = &filed_page_cache.slots[i];
        if (slot->valid && slot->dirty && slot->backend_object == backend_object) {
            return true;
        }
    }
    return false;
}

uint64_t filed_page_cache_dirty_count(void)
{
    uint64_t count = 0;
    filed_page_cache_ensure_configured();
    for (size_t i = 0; i < filed_page_cache.active_slots; ++i) {
        const filed_page_cache_slot_t *slot = &filed_page_cache.slots[i];
        if (slot->valid && slot->dirty) {
            count++;
        }
    }
    return count;
}

static int filed_page_cache_flush_slot(
    filed_runtime_t *runtime,
    filed_page_cache_slot_t *slot)
{
    if (slot == NULL || !slot->valid || !slot->dirty) {
        return 0;
    }
    if (runtime == NULL ||
        slot->backend_object == 0 ||
        slot->dirty_start >= slot->dirty_end ||
        slot->dirty_start < slot->valid_start ||
        slot->dirty_end > slot->bytes ||
        slot->dirty_end > FILED_PAGE_CACHE_BYTES ||
        slot->page_index > UINT64_MAX / FILED_PAGE_CACHE_BYTES)
    {
        filed_page_cache.flush_errors++;
        return -22;
    }

    const uint64_t page_start = slot->page_index * FILED_PAGE_CACHE_BYTES;
    const uint64_t offset = page_start + slot->dirty_start;
    const uint64_t length = (uint64_t)slot->dirty_end - slot->dirty_start;
    uint64_t bytes = 0;
    const int status = filed_backend_pwrite(
        runtime,
        slot->backend_object,
        offset,
        slot->data + slot->dirty_start,
        length,
        &bytes);
    if (status != 0 || bytes != length) {
        filed_page_cache.flush_errors++;
        return status != 0 ? status : -5;
    }

    slot->dirty = false;
    slot->dirty_start = 0;
    slot->dirty_end = 0;
    filed_page_cache.flushes++;
    return 0;
}

int filed_page_cache_flush_object(filed_runtime_t *runtime, uint64_t backend_object)
{
    filed_page_cache_ensure_configured();
    if (filed_backend_object_is_tmpfs(backend_object)) {
        return 0;
    }
    if (filed_page_cache.active_slots == 0) {
        return 0;
    }
    for (size_t i = 0; i < filed_page_cache.active_slots; ++i) {
        filed_page_cache_slot_t *slot = &filed_page_cache.slots[i];
        if (slot->valid && slot->dirty &&
            (backend_object == 0 || slot->backend_object == backend_object))
        {
            const int status = filed_page_cache_flush_slot(runtime, slot);
            if (status != 0) {
                return status;
            }
        }
    }
    return 0;
}

static filed_page_cache_slot_t *filed_page_cache_choose_slot(filed_runtime_t *runtime)
{
    filed_page_cache_slot_t *oldest = NULL;
    (void)runtime;
    filed_page_cache_ensure_configured();
    if (filed_page_cache.active_slots == 0) {
        return NULL;
    }
    for (size_t i = 0; i < filed_page_cache.active_slots; ++i) {
        filed_page_cache_slot_t *slot = &filed_page_cache.slots[i];
        if (!slot->valid) {
            return slot;
        }
        if (slot->dirty) {
            continue;
        }
        if (oldest == NULL || slot->last_used < oldest->last_used) {
            oldest = slot;
        }
    }
    if (oldest != NULL) {
        filed_page_cache.evictions++;
    }
    return oldest;
}

static void filed_page_cache_store(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t page_index,
    const void *data,
    uint64_t bytes)
{
    if (backend_object == 0 || data == NULL || bytes == 0) {
        return;
    }
    filed_page_cache_ensure_configured();
    if (filed_page_cache.active_slots == 0) {
        return;
    }
    if (bytes > FILED_PAGE_CACHE_BYTES) {
        bytes = FILED_PAGE_CACHE_BYTES;
    }

    filed_page_cache_slot_t *slot = filed_page_cache_find(runtime, backend_object, page_index);
    if (slot != NULL && slot->dirty) {
        return;
    }
    if (slot == NULL) {
        slot = filed_page_cache_choose_slot(runtime);
    }
    if (slot == NULL) {
        return;
    }

    memcpy(slot->data, data, (size_t)bytes);
    slot->valid = true;
    slot->dirty = false;
    slot->backend_object = backend_object;
    slot->page_index = page_index;
    slot->bytes = (uint32_t)bytes;
    slot->valid_start = 0;
    slot->dirty_start = 0;
    slot->dirty_end = 0;
    slot->last_used = filed_page_cache_next_clock(runtime);
}

void filed_page_cache_invalidate_object(filed_runtime_t *runtime, uint64_t backend_object)
{
    (void)runtime;
    filed_page_cache_ensure_configured();
    if (filed_backend_object_is_tmpfs(backend_object)) {
        return;
    }
    for (size_t i = 0; i < filed_page_cache.active_slots; ++i) {
        filed_page_cache_slot_t *slot = &filed_page_cache.slots[i];
        if (slot->valid && (backend_object == 0 || slot->backend_object == backend_object)) {
            memset(slot, 0, sizeof(*slot));
        }
    }
}

static void filed_page_cache_invalidate_page(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t page_index)
{
    filed_page_cache_slot_t *slot = filed_page_cache_find(runtime, backend_object, page_index);
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
}

static int filed_page_cache_try_dirty_write(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    const void *data,
    uint64_t length,
    bool allow_extend_slot)
{
    uint64_t total = 0;

    if (runtime == NULL || backend_object == 0 || data == NULL || length == 0) {
        return 0;
    }
    filed_page_cache_ensure_configured();
    if (filed_page_cache.active_slots == 0) {
        return 0;
    }

    while (total < length) {
        const uint64_t absolute_offset = offset + total;
        if (absolute_offset < offset) {
            return 0;
        }
        const uint64_t page_index = absolute_offset / FILED_PAGE_CACHE_BYTES;
        const uint64_t page_offset = absolute_offset % FILED_PAGE_CACHE_BYTES;
        uint64_t chunk = length - total;
        if (chunk > FILED_PAGE_CACHE_BYTES - page_offset) {
            chunk = FILED_PAGE_CACHE_BYTES - page_offset;
        }

        filed_page_cache_slot_t *slot =
            filed_page_cache_find(runtime, backend_object, page_index);
        if (slot == NULL) {
            if (!allow_extend_slot || page_offset + chunk > FILED_PAGE_CACHE_BYTES) {
                return 0;
            }
        } else if (page_offset < slot->valid_start ||
            page_offset > slot->bytes ||
            page_offset + chunk > FILED_PAGE_CACHE_BYTES)
        {
            return 0;
        }
        total += chunk;
    }

    total = 0;
    while (total < length) {
        const uint64_t absolute_offset = offset + total;
        const uint64_t page_index = absolute_offset / FILED_PAGE_CACHE_BYTES;
        const uint64_t page_offset = absolute_offset % FILED_PAGE_CACHE_BYTES;
        uint64_t chunk = length - total;
        if (chunk > FILED_PAGE_CACHE_BYTES - page_offset) {
            chunk = FILED_PAGE_CACHE_BYTES - page_offset;
        }

        filed_page_cache_slot_t *slot =
            filed_page_cache_find(runtime, backend_object, page_index);
        if (slot == NULL) {
            slot = filed_page_cache_choose_slot(runtime);
            if (slot == NULL) {
                return 0;
            }
            memset(slot, 0, sizeof(*slot));
            slot->valid = true;
            slot->backend_object = backend_object;
            slot->page_index = page_index;
            slot->valid_start = (uint32_t)page_offset;
            slot->bytes = (uint32_t)page_offset;
        }
        if (page_offset < slot->valid_start ||
            page_offset > slot->bytes ||
            page_offset + chunk > FILED_PAGE_CACHE_BYTES)
        {
            return 0;
        }
        memcpy(slot->data + page_offset, (const uint8_t *)data + total, (size_t)chunk);
        if (page_offset + chunk > slot->bytes) {
            slot->bytes = (uint32_t)(page_offset + chunk);
        }
        slot->dirty = true;
        if (slot->dirty_start == slot->dirty_end) {
            slot->dirty_start = (uint32_t)page_offset;
            slot->dirty_end = (uint32_t)(page_offset + chunk);
        } else {
            if (page_offset < slot->dirty_start) {
                slot->dirty_start = (uint32_t)page_offset;
            }
            if (page_offset + chunk > slot->dirty_end) {
                slot->dirty_end = (uint32_t)(page_offset + chunk);
            }
        }
        slot->last_used = filed_page_cache_next_clock(runtime);
        total += chunk;
    }

    filed_page_cache.dirty_writes++;
    return 1;
}

static void filed_page_cache_note_write(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    const void *data,
    uint64_t length)
{
    uint64_t total = 0;

    if (runtime == NULL || backend_object == 0 || data == NULL || length == 0) {
        return;
    }
    filed_page_cache_ensure_configured();
    if (filed_page_cache.active_slots == 0) {
        return;
    }

    while (total < length) {
        const uint64_t absolute_offset = offset + total;
        if (absolute_offset < offset) {
            return;
        }
        const uint64_t page_index = absolute_offset / FILED_PAGE_CACHE_BYTES;
        const uint64_t page_offset = absolute_offset % FILED_PAGE_CACHE_BYTES;
        filed_page_cache_slot_t *slot =
            filed_page_cache_find(runtime, backend_object, page_index);
        uint64_t chunk = length - total;
        if (chunk > FILED_PAGE_CACHE_BYTES - page_offset) {
            chunk = FILED_PAGE_CACHE_BYTES - page_offset;
        }

        if (slot != NULL && !slot->dirty) {
            if (page_offset >= slot->valid_start &&
                page_offset <= slot->bytes &&
                chunk <= FILED_PAGE_CACHE_BYTES - page_offset)
            {
                memcpy(slot->data + page_offset, (const uint8_t *)data + total, (size_t)chunk);
                if (page_offset + chunk > slot->bytes) {
                    slot->bytes = (uint32_t)(page_offset + chunk);
                }
                slot->last_used = filed_page_cache_next_clock(runtime);
            } else {
                filed_page_cache_invalidate_page(runtime, backend_object, page_index);
            }
        }
        total += chunk;
    }
}

int filed_cached_pread(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    uint64_t total = 0;
    uint8_t page_buffer[FILED_PAGE_CACHE_BYTES];

    if (runtime == NULL || backend_object == 0 || buffer == NULL || out_bytes == NULL) {
        return -1;
    }
    *out_bytes = 0;
    if (length == 0) {
        return 0;
    }
    if (filed_backend_object_is_tmpfs(backend_object)) {
        filed_page_cache.direct_reads++;
        return filed_backend_pread(
            runtime,
            backend_object,
            offset,
            buffer,
            length,
            out_bytes);
    }
    filed_page_cache_ensure_configured();
    if (length >= 65536u) {
        const int flush_status = filed_page_cache_flush_object(runtime, backend_object);
        if (flush_status != 0) {
            return flush_status;
        }
        filed_page_cache.direct_reads++;
        return filed_backend_pread(
            runtime,
            backend_object,
            offset,
            buffer,
            length,
            out_bytes);
    }
    if (filed_page_cache.active_slots == 0) {
        filed_page_cache.direct_reads++;
        return filed_backend_pread(
            runtime,
            backend_object,
            offset,
            buffer,
            length,
            out_bytes);
    }
    while (total < length) {
        const uint64_t absolute_offset = offset + total;
        if (absolute_offset < offset) {
            break;
        }
        const uint64_t page_index = absolute_offset / FILED_PAGE_CACHE_BYTES;
        const uint64_t page_offset = absolute_offset % FILED_PAGE_CACHE_BYTES;
        filed_page_cache_slot_t *slot =
            filed_page_cache_find(runtime, backend_object, page_index);

        if (slot != NULL && page_offset < slot->valid_start) {
            const int flush_status = filed_page_cache_flush_slot(runtime, slot);
            if (flush_status != 0) {
                return flush_status;
            }
            memset(slot, 0, sizeof(*slot));
            slot = NULL;
        }

        if (slot != NULL) {
            filed_page_cache.hits++;
            slot->last_used = filed_page_cache_next_clock(runtime);
        } else {
            uint64_t read_bytes = 0;
            const uint64_t page_start = absolute_offset - page_offset;
            filed_page_cache.misses++;
            memset(page_buffer, 0, sizeof(page_buffer));
            const int status = filed_backend_pread(
                runtime,
                backend_object,
                page_start,
                page_buffer,
                FILED_PAGE_CACHE_BYTES,
                &read_bytes);
            if (status != 0) {
                return status;
            }
            if (read_bytes == 0) {
                break;
            }
            filed_page_cache_store(runtime, backend_object, page_index, page_buffer, read_bytes);
            slot = filed_page_cache_find(runtime, backend_object, page_index);
            if (slot == NULL) {
                uint64_t available = read_bytes > page_offset ? read_bytes - page_offset : 0;
                uint64_t wanted = length - total;
                if (available == 0) {
                    break;
                }
                if (wanted > available) {
                    wanted = available;
                }
                memcpy(
                    (uint8_t *)buffer + total,
                    page_buffer + page_offset,
                    (size_t)wanted);
                total += wanted;
                if (read_bytes < FILED_PAGE_CACHE_BYTES) {
                    break;
                }
                continue;
            }
        }

        if (page_offset < slot->valid_start || page_offset >= slot->bytes) {
            break;
        }
        uint64_t available = (uint64_t)slot->bytes - page_offset;
        uint64_t wanted = length - total;
        if (wanted > available) {
            wanted = available;
        }
        memcpy((uint8_t *)buffer + total, slot->data + page_offset, (size_t)wanted);
        total += wanted;

        if (slot->bytes < FILED_PAGE_CACHE_BYTES) {
            break;
        }
    }

    *out_bytes = total;
    return 0;
}

static int filed_cached_pwrite_ex(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    const void *buffer,
    uint64_t length,
    uint64_t *out_bytes,
    bool allow_extend_slot)
{
    if (runtime == NULL || backend_object == 0 || buffer == NULL || out_bytes == NULL) {
        return -1;
    }
    *out_bytes = 0;
    if (length == 0) {
        return 0;
    }
    if (offset + length < offset) {
        return -75;
    }
    if (filed_backend_object_is_tmpfs(backend_object)) {
        return filed_backend_pwrite(
            runtime,
            backend_object,
            offset,
            buffer,
            length,
            out_bytes);
    }
    if (filed_page_cache_try_dirty_write(
            runtime,
            backend_object,
            offset,
            buffer,
            length,
            allow_extend_slot))
    {
        *out_bytes = length;
        return 0;
    }

    int status = filed_page_cache_flush_object(runtime, backend_object);
    if (status != 0) {
        return status;
    }

    status = filed_backend_pwrite(
        runtime,
        backend_object,
        offset,
        buffer,
        length,
        out_bytes);
    if (status == 0) {
        filed_page_cache_note_write(runtime, backend_object, offset, buffer, *out_bytes);
    }
    return status;
}

int filed_cached_pwrite(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    const void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    return filed_cached_pwrite_ex(
        runtime,
        backend_object,
        offset,
        buffer,
        length,
        out_bytes,
        false);
}

void filed_dump_cache_metrics(const filed_runtime_t *runtime)
{
    (void)runtime;
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_CACHE, PACHA_TRACE_CLASS_METRIC, 1, filed_page_cache.hits);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_CACHE, PACHA_TRACE_CLASS_METRIC, 2, filed_page_cache.misses);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_CACHE, PACHA_TRACE_CLASS_METRIC, 3, filed_page_cache.evictions);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_CACHE, PACHA_TRACE_CLASS_METRIC, 4, filed_page_cache.direct_reads);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_CACHE, PACHA_TRACE_CLASS_METRIC, 5, filed_page_cache.dirty_writes);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_CACHE, PACHA_TRACE_CLASS_METRIC, 6, filed_page_cache.flushes);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_CACHE, PACHA_TRACE_CLASS_METRIC, 7, filed_page_cache.flush_errors);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_CACHE, PACHA_TRACE_CLASS_METRIC, 8, filed_page_cache.active_slots);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_CACHE, PACHA_TRACE_CLASS_METRIC, 9, filed_dir_cache.hits);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_CACHE, PACHA_TRACE_CLASS_METRIC, 10, filed_dir_cache.misses);
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_METRIC_CACHE, PACHA_TRACE_CLASS_METRIC, 11, filed_dir_cache.evictions);
}

static pacha_errconv_store_t *filed_errors(void)
{
    if (!filed_errconv_store_ready) {
        pacha_errconv_store_init(&filed_errconv_store, PACHA_ERRCONV_COMPONENT_FILED);
        filed_errconv_store_ready = 1;
    }
    return &filed_errconv_store;
}

static uint64_t filed_error_token(
    int64_t status,
    uint64_t op,
    uint64_t stage,
    int64_t raw_status,
    uint64_t request_id,
    uint64_t fd_count,
    uint64_t subject,
    uint64_t child_token,
    const char *text)
{
    return pacha_errconv_error_token(
        filed_errors(),
        status,
        PACHA_ERRCONV_DOMAIN_FILED_STATUS,
        op,
        stage,
        raw_status,
        request_id,
        fd_count,
        subject,
        child_token,
        text);
}

static int filed_send_reply_v2_payload(
    int reply_fd,
    void *page,
    const pacha_service_request_header_t *header,
    int64_t status,
    uint64_t result,
    uint64_t error_token,
    uint32_t payload_size)
{
    const uint64_t reply_result = status < 0 ? error_token : result;
    if (page != NULL) {
        pacha_service_reply_header_init(
            (pacha_service_reply_header_t *)page,
            header,
            status,
            PACHA_SERVICE_ERROR_FILED_VFS,
            reply_result,
            status == 0 ? payload_size : 0);
    }
    const struct pacha_ipc_msg reply = {
        .word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = reply_result,
        .word3 = header != NULL ? header->request_id : 0,
    };
    const int reply_status = pacha_ipc_reply(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    return reply_status;
}

static int filed_send_reply_v2(
    int reply_fd,
    void *page,
    const pacha_service_request_header_t *header,
    int64_t status,
    uint64_t result,
    uint64_t error_token)
{
    return filed_send_reply_v2_payload(reply_fd, page, header, status, result, error_token, 0);
}

static int filed_send_session_reply_v2(int channel_fd, uint64_t request_id, int64_t status, uint64_t result)
{
    const uint64_t token = status < 0 ?
        filed_error_token(
            status,
            FILED_V2_OP_SESSION_DOORBELL,
            PACHA_ERRCONV_STAGE_STATUS_MAP,
            status,
            request_id,
            0,
            0,
            0,
            "filed v2 session negative reply") :
        0;
    const struct pacha_ipc_msg reply = {
        .word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = status < 0 ? token : result,
        .word3 = request_id,
    };
    return filed_ipc_send_wait(channel_fd, &reply);
}

static void filed_file_vmo_cache_clear_entry(filed_file_vmo_cache_entry_t *entry)
{
    if (entry == NULL || !entry->active) {
        return;
    }
    if (entry->vmo_fd >= 16) {
        (void)pacha_fd_close(entry->vmo_fd);
    }
    memset(entry, 0, sizeof(*entry));
    entry->vmo_fd = -1;
}

static void filed_file_vmo_cache_invalidate_object(filed_runtime_t *runtime, uint64_t backend_object)
{
    if (runtime == NULL || backend_object == 0) {
        return;
    }
    for (uint64_t i = 0; i < FILED_RUNTIME_FILE_VMO_CACHE_SLOTS; ++i) {
        filed_file_vmo_cache_entry_t *entry = &runtime->file_vmo_cache[i];
        if (entry->active && entry->backend_object == backend_object) {
            filed_file_vmo_cache_clear_entry(entry);
        }
    }
}

static filed_file_vmo_cache_entry_t *filed_file_vmo_cache_lookup(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t object_generation,
    uint64_t file_offset,
    uint64_t length)
{
    if (runtime == NULL || backend_object == 0 || length == 0) {
        return NULL;
    }
    for (uint64_t i = 0; i < FILED_RUNTIME_FILE_VMO_CACHE_SLOTS; ++i) {
        filed_file_vmo_cache_entry_t *entry = &runtime->file_vmo_cache[i];
        if (entry->active &&
            entry->backend_object == backend_object &&
            entry->object_generation == object_generation &&
            entry->file_offset == file_offset &&
            entry->length == length)
        {
            entry->clock = ++runtime->file_vmo_cache_clock;
            filed_file_vmo_cache_hits++;
            return entry;
        }
    }
    filed_file_vmo_cache_misses++;
    return NULL;
}

static filed_file_vmo_cache_entry_t *filed_file_vmo_cache_slot(filed_runtime_t *runtime)
{
    uint64_t slot = 0;
    uint64_t oldest = UINT64_MAX;
    for (uint64_t i = 0; i < FILED_RUNTIME_FILE_VMO_CACHE_SLOTS; ++i) {
        filed_file_vmo_cache_entry_t *entry = &runtime->file_vmo_cache[i];
        if (!entry->active) {
            return entry;
        }
        if (entry->clock < oldest) {
            oldest = entry->clock;
            slot = i;
        }
    }
    filed_file_vmo_cache_evictions++;
    filed_file_vmo_cache_clear_entry(&runtime->file_vmo_cache[slot]);
    return &runtime->file_vmo_cache[slot];
}

static int filed_send_exec_reply_v2(
    int reply_fd,
    uint64_t request_id,
    int process_fd,
    int thread_fd,
    int transfer_process_fd)
{
    uint64_t process_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_KILL;
    if (transfer_process_fd) {
        process_rights |= PACHA_FD_RIGHT_TRANSFER;
    }
    struct pacha_ipc_fd fds[2] = {
        {
            .fd = (uint64_t)(uint32_t)process_fd,
            .rights = process_rights,
            .flags = 0,
            .transfer_flags = PACHA_IPC_TRANSFER_MOVE | PACHA_IPC_TRANSFER_CLOEXEC,
        },
        {
            .fd = (uint64_t)(uint32_t)thread_fd,
            .rights =
                PACHA_FD_RIGHT_INSPECT |
                PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_WAIT |
                PACHA_FD_RIGHT_KILL,
            .flags = 0,
            .transfer_flags = PACHA_IPC_TRANSFER_MOVE | PACHA_IPC_TRANSFER_CLOEXEC,
        },
    };
    struct pacha_ipc_msg reply = {
        .word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = 0,
        .word2 = (uint64_t)(uint32_t)process_fd,
        .word3 = request_id,
        .fds = fds,
        .fd_count = 2,
    };
    const int status = pacha_ipc_reply(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    if (status != 0) {
        (void)pacha_syscall2(
            PACHA_PROCESS_SYSCALL_KILL,
            (uint64_t)(uint32_t)process_fd,
            1);
        (void)pacha_fd_close(thread_fd);
        (void)pacha_fd_close(process_fd);
    }
    return status;
}

static int filed_send_exec_self_reply_v2(
    int reply_fd,
    uint64_t request_id,
    int process_fd,
    int thread_fd,
    int bootstrap_fd)
{
    struct pacha_ipc_fd fds[3] = {
        {
            .fd = (uint64_t)(uint32_t)process_fd,
            .rights =
                PACHA_FD_RIGHT_INSPECT |
                PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_WAIT |
                PACHA_FD_RIGHT_POLL |
                PACHA_FD_RIGHT_KILL |
                PACHA_FD_RIGHT_MAP_INTO |
                PACHA_FD_RIGHT_SET_CONTEXT,
            .flags = 0,
            .transfer_flags = PACHA_IPC_TRANSFER_MOVE | PACHA_IPC_TRANSFER_CLOEXEC,
        },
        {
            .fd = (uint64_t)(uint32_t)thread_fd,
            .rights =
                PACHA_FD_RIGHT_INSPECT |
                PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_WAIT |
                PACHA_FD_RIGHT_KILL |
                PACHA_FD_RIGHT_SET_CONTEXT,
            .flags = 0,
            .transfer_flags = PACHA_IPC_TRANSFER_MOVE | PACHA_IPC_TRANSFER_CLOEXEC,
        },
        {
            .fd = (uint64_t)(uint32_t)bootstrap_fd,
            .rights =
                PACHA_FD_RIGHT_INSPECT |
                PACHA_FD_RIGHT_DUP |
                PACHA_FD_RIGHT_SET_FLAGS |
                PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_READ |
                PACHA_FD_RIGHT_MAP_READ,
            .flags = 0,
            .transfer_flags = PACHA_IPC_TRANSFER_MOVE,
        },
    };
    struct pacha_ipc_msg reply = {
        .word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = 0,
        .word2 = (uint64_t)(uint32_t)process_fd,
        .word3 = request_id,
        .fds = fds,
        .fd_count = 3,
    };
    const int status = pacha_ipc_reply(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    if (status != 0) {
        (void)pacha_syscall2(
            PACHA_PROCESS_SYSCALL_KILL,
            (uint64_t)(uint32_t)process_fd,
            1);
        (void)pacha_fd_close(thread_fd);
        (void)pacha_fd_close(process_fd);
        (void)pacha_fd_close(bootstrap_fd);
    }
    return status;
}

static int filed_dispatch_set_inherit(int fd, int enabled)
{
    if (fd < 0 || fd >= FILED_EXEC_MAX_FDS) {
        return -22;
    }
    const long status = pacha_fd_fcntl(
        fd,
        PACHA_FD_FCNTL_SET_FLAGS,
        enabled ? PACHA_FD_FLAG_INHERIT : 0,
        PACHA_FD_FLAG_INHERIT);
    return status == 0 ? 0 : -22;
}

typedef struct filed_dispatch_saved_fd {
    int fd;
    uint64_t rights;
    uint64_t flags;
} filed_dispatch_saved_fd_t;

static void filed_dispatch_saved_fd_init(filed_dispatch_saved_fd_t *saved)
{
    if (saved == NULL) {
        return;
    }
    saved->fd = -1;
    saved->rights = 0;
    saved->flags = 0;
}

static void filed_dispatch_close_owned_fd(int *fd)
{
    if (fd == NULL || *fd < 0) {
        return;
    }
    (void)pacha_fd_close(*fd);
    *fd = -1;
}

static int filed_dispatch_save_target_fd(int target_fd, filed_dispatch_saved_fd_t *saved)
{
    if (saved == NULL || target_fd < 0 || target_fd >= FILED_EXEC_MAX_FDS) {
        return -22;
    }
    filed_dispatch_saved_fd_init(saved);

    struct pacha_fd_info info;
    memset(&info, 0, sizeof(info));
    if (pacha_fd_get_info(target_fd, &info) != 0) {
        return 0;
    }

    const long dup_fd = pacha_fd_fcntl(
        target_fd,
        PACHA_FD_FCNTL_DUP,
        16,
        info.rights);
    if (dup_fd < 16) {
        return -13;
    }
    saved->fd = (int)dup_fd;
    saved->rights = info.rights;
    saved->flags = info.flags;
    return 0;
}

static void filed_dispatch_restore_target_fd(int target_fd, filed_dispatch_saved_fd_t *saved)
{
    if (saved == NULL || target_fd < 0 || target_fd >= FILED_EXEC_MAX_FDS) {
        return;
    }
    (void)pacha_fd_close(target_fd);
    if (saved->fd < 0) {
        return;
    }

    const long dup_fd = pacha_fd_fcntl(
        saved->fd,
        PACHA_FD_FCNTL_DUP,
        (uint64_t)(uint32_t)target_fd,
        saved->rights);
    if (dup_fd == target_fd) {
        (void)pacha_fd_fcntl(
            (int)dup_fd,
            PACHA_FD_FCNTL_SET_FLAGS,
            saved->flags,
            PACHA_FD_FLAG_CLOEXEC |
                PACHA_FD_FLAG_NONBLOCK |
                PACHA_FD_FLAG_INHERIT |
                PACHA_FD_FLAG_PRIVATE);
    } else if (dup_fd >= 0) {
        (void)pacha_fd_close((int)dup_fd);
    }
    (void)pacha_fd_close(saved->fd);
    filed_dispatch_saved_fd_init(saved);
}

static int filed_dispatch_exec_default_stdio_valid(const filed_v2_exec_path_t *exec)
{
    if (exec == NULL) {
        return 0;
    }
    const int wants_default_stdio =
        (exec->flags & FILED_V2_EXEC_LINUX_DEFAULT_STDIO) != 0;
    if (!wants_default_stdio) {
        return filed_v2_exec_string_ref_empty(exec->ctty);
    }
    if ((exec->flags & (FILED_V2_EXEC_LINUX_LPR | FILED_V2_EXEC_LINUX_BOOTSTRAP)) !=
        (FILED_V2_EXEC_LINUX_LPR | FILED_V2_EXEC_LINUX_BOOTSTRAP))
    {
        return 0;
    }
    return filed_v2_exec_string_ref_valid(exec, exec->ctty);
}

static const filed_v2_exec_lpr_fd_t *filed_dispatch_lpr_fd_table_entries(
    const filed_v2_exec_lpr_fd_table_t *table)
{
    return (const filed_v2_exec_lpr_fd_t *)((const unsigned char *)table + sizeof(*table));
}

static int filed_dispatch_lpr_fd_desc_valid(const filed_v2_exec_lpr_fd_t *fd)
{
    if (fd == NULL || fd->fd > LPR_LINUX_FD_MAX) {
        return 0;
    }
    switch (fd->kind) {
    case FILED_V2_EXEC_LPR_FD_FILED:
    case FILED_V2_EXEC_LPR_FD_TTY:
        return fd->handle != 0;
    case FILED_V2_EXEC_LPR_FD_PIPE:
    case FILED_V2_EXEC_LPR_FD_EVENT:
        return 1;
    default:
        return 0;
    }
}

static int filed_dispatch_exec_lpr_fd_table_valid(
    const filed_v2_exec_path_t *exec,
    const filed_v2_exec_lpr_fd_table_t *table,
    int allow_table)
{
    if (exec == NULL) {
        return 0;
    }
    const int wants_table = (exec->flags & FILED_V2_EXEC_LPR_FD_TABLE) != 0;
    if (!wants_table) {
        return exec->lpr_fd_table_bytes == 0 && table == NULL;
    }
    if (!allow_table ||
        table == NULL ||
        exec->lpr_fd_table_bytes < sizeof(*table) ||
        (exec->flags & (FILED_V2_EXEC_LINUX_LPR | FILED_V2_EXEC_LINUX_BOOTSTRAP)) !=
            (FILED_V2_EXEC_LINUX_LPR | FILED_V2_EXEC_LINUX_BOOTSTRAP))
    {
        return 0;
    }
    if (table->magic != FILED_V2_EXEC_LPR_FD_TABLE_MAGIC ||
        table->version != FILED_V2_EXEC_LPR_FD_TABLE_VERSION ||
        table->reserved0 != 0 ||
        table->reserved1 != 0 ||
        table->byte_size < sizeof(*table) ||
        table->byte_size > exec->lpr_fd_table_bytes)
    {
        return 0;
    }
    if (table->fd_count > LPR_LINUX_FD_LIMIT ||
        table->fd_count > (UINT64_MAX - sizeof(*table)) / sizeof(filed_v2_exec_lpr_fd_t))
    {
        return 0;
    }
    const uint64_t expected_size =
        sizeof(*table) + table->fd_count * sizeof(filed_v2_exec_lpr_fd_t);
    if (table->byte_size != expected_size) {
        return 0;
    }
    const filed_v2_exec_lpr_fd_t *entries = filed_dispatch_lpr_fd_table_entries(table);
    for (uint64_t i = 0; i < table->fd_count; ++i) {
        if (!filed_dispatch_lpr_fd_desc_valid(&entries[i])) {
            return 0;
        }
        if (i != 0 && entries[i].fd <= entries[i - 1u].fd) {
            return 0;
        }
    }
    return 1;
}

static int filed_dispatch_create_lpr_bootstrap_fd(
    const filed_v2_exec_path_t *exec,
    const filed_v2_exec_lpr_fd_table_t *fd_table)
{
    if (exec == NULL) {
        return -22;
    }
    const uint64_t local_fd_count = fd_table != NULL ? fd_table->fd_count : 0;
    if (local_fd_count > (UINT64_MAX - sizeof(struct lpr_bootstrap)) / sizeof(lpr_bootstrap_fd_t)) {
        return -22;
    }
    const uint64_t local_fd_bytes = local_fd_count * sizeof(lpr_bootstrap_fd_t);
    const uint64_t bootstrap_bytes = sizeof(struct lpr_bootstrap) + local_fd_bytes;
    const uint64_t rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_READ |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int fd = pacha_vmo_create(bootstrap_bytes, rights, 0);
    if (fd < 16) {
        return fd < 0 ? fd : -12;
    }
    struct lpr_bootstrap *bootstrap = pacha_mmap(
        fd,
        bootstrap_bytes,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (bootstrap == NULL) {
        (void)pacha_fd_close(fd);
        return -12;
    }
    memset(bootstrap, 0, sizeof(*bootstrap));
    bootstrap->magic = LPR_BOOTSTRAP_MAGIC;
    bootstrap->version = LPR_BOOTSTRAP_VERSION;
    bootstrap->byte_size = bootstrap_bytes;
    bootstrap->local_fd_table_offset = sizeof(struct lpr_bootstrap);
    bootstrap->local_fd_table_bytes = local_fd_bytes;
    bootstrap->local_fd_count = local_fd_count;
    bootstrap->linux_pid = exec->linux_pid;
    bootstrap->linux_ppid = exec->linux_ppid;
    bootstrap->linux_sid = exec->linux_sid;
    bootstrap->linux_pgrp = exec->linux_pgrp;
    bootstrap->linux_next_pid = exec->linux_next_pid;
    bootstrap->cwd_handle = exec->cwd_handle;
    bootstrap->supervisor_token = exec->lpr_supervisor_token;
    bootstrap->supervisor_endpoint_fd = LPR_SUPERVISOR_ENDPOINT_FD;
    bootstrap->fd_table_token = exec->lpr_fd_table_token;
    if (exec->lpr_supervisor_token != 0) {
        bootstrap->flags |= LPR_BOOTSTRAP_FLAG_SUPERVISOR;
    }
    if (fd_table != NULL && local_fd_count != 0) {
        const filed_v2_exec_lpr_fd_t *in = filed_dispatch_lpr_fd_table_entries(fd_table);
        lpr_bootstrap_fd_t *out =
            (lpr_bootstrap_fd_t *)((unsigned char *)bootstrap + bootstrap->local_fd_table_offset);
        for (uint64_t i = 0; i < local_fd_count; ++i) {
            out[i].fd = in[i].fd;
            out[i].kind = in[i].kind;
            out[i].flags = in[i].flags;
            out[i].handle = in[i].handle;
            out[i].offset_or_counter = in[i].offset_or_counter;
        }
    }
    if ((exec->flags & FILED_V2_EXEC_LINUX_DEFAULT_STDIO) != 0) {
        bootstrap->flags |= LPR_BOOTSTRAP_FLAG_DEFAULT_STDIO;
        const char *ctty = filed_v2_exec_string(exec, exec->ctty);
        if (ctty != NULL) {
            snprintf(bootstrap->ctty, sizeof(bootstrap->ctty), "%s", ctty);
        }
    }
    if (!filed_v2_exec_string_ref_empty(exec->cwd)) {
        const char *cwd = filed_v2_exec_string(exec, exec->cwd);
        if (cwd != NULL) {
            snprintf(bootstrap->cwd, sizeof(bootstrap->cwd), "%s", cwd);
        }
    }
    (void)pacha_munmap(bootstrap, bootstrap_bytes);
    return fd;
}

static int filed_dispatch_prepare_inherit_fd_to_target(
    int source_fd,
    uint64_t target_raw,
    int *out_fd,
    filed_dispatch_saved_fd_t *saved)
{
    if (out_fd != NULL) {
        *out_fd = -1;
    }
    if (source_fd < 0 ||
        target_raw >= FILED_EXEC_MAX_FDS ||
        out_fd == NULL ||
        saved == NULL)
    {
        return -22;
    }

    const int target_fd = (int)(uint32_t)target_raw;
    struct pacha_fd_info source_info;
    memset(&source_info, 0, sizeof(source_info));
    if (pacha_fd_get_info(source_fd, &source_info) != 0) {
        return -13;
    }

    const uint64_t inherit_flags =
        (source_info.flags & ~PACHA_FD_FLAG_CLOEXEC) |
        PACHA_FD_FLAG_INHERIT;
    const uint64_t flag_mask =
        PACHA_FD_FLAG_CLOEXEC |
        PACHA_FD_FLAG_NONBLOCK |
        PACHA_FD_FLAG_INHERIT |
        PACHA_FD_FLAG_PRIVATE;

    if (source_fd == target_fd) {
        const long status = pacha_fd_fcntl(
            source_fd,
            PACHA_FD_FCNTL_SET_FLAGS,
            inherit_flags,
            flag_mask);
        if (status != 0) {
            return -13;
        }
        *out_fd = source_fd;
        return 0;
    }

    int status = filed_dispatch_save_target_fd(target_fd, saved);
    if (status != 0) {
        return status;
    }
    (void)pacha_fd_close(target_fd);

    const long dup_fd = pacha_fd_fcntl(
        source_fd,
        PACHA_FD_FCNTL_DUP,
        (uint64_t)(uint32_t)target_fd,
        source_info.rights);
    if (dup_fd != target_fd) {
        if (dup_fd >= 0) {
            (void)pacha_fd_close((int)dup_fd);
        }
        filed_dispatch_restore_target_fd(target_fd, saved);
        return -13;
    }
    (void)pacha_fd_close(source_fd);

    const long flag_status = pacha_fd_fcntl(
        (int)dup_fd,
        PACHA_FD_FCNTL_SET_FLAGS,
        inherit_flags,
        flag_mask);
    if (flag_status != 0) {
        filed_dispatch_restore_target_fd(target_fd, saved);
        return -13;
    }

    *out_fd = (int)dup_fd;
    return 0;
}

static int filed_dispatch_dup_endpoint_to_fixed(
    int source_fd,
    int target_fd,
    int *out_fd)
{
    if (out_fd != NULL) {
        *out_fd = -1;
    }
    if (source_fd < 16 || target_fd < 16) {
        return -22;
    }
    (void)pacha_fd_close(target_fd);
    const uint64_t endpoint_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_SEND |
        PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_CALL |
        PACHA_FD_RIGHT_TRANSFER;
    const long dup_fd = pacha_fd_fcntl(
        source_fd,
        PACHA_FD_FCNTL_DUP,
        (uint64_t)(uint32_t)target_fd,
        endpoint_rights);
    if (dup_fd != target_fd) {
        fprintf(stderr,
            "[filed] endpoint dup failed source=%d target=%d result=%ld\n",
            source_fd,
            target_fd,
            dup_fd);
        if (dup_fd >= 16) {
            (void)pacha_fd_close((int)dup_fd);
        }
        return -24;
    }
    if (filed_dispatch_set_inherit((int)dup_fd, 1) != 0) {
        fprintf(stderr,
            "[filed] endpoint inherit failed fd=%ld source=%d target=%d\n",
            dup_fd,
            source_fd,
            target_fd);
        (void)pacha_fd_close((int)dup_fd);
        return -13;
    }
    if (out_fd != NULL) {
        *out_fd = (int)dup_fd;
    }
    return 0;
}

static int filed_dispatch_prepare_endpoint_to_fixed(
    int source_fd,
    int target_fd,
    int *out_fd,
    int *out_borrowed)
{
    if (out_fd != NULL) {
        *out_fd = -1;
    }
    if (out_borrowed != NULL) {
        *out_borrowed = 0;
    }
    if (source_fd < 16 || target_fd < 16 || out_fd == NULL || out_borrowed == NULL) {
        return -22;
    }
    if (source_fd == target_fd) {
        const int status = filed_dispatch_set_inherit(source_fd, 1);
        if (status != 0) {
            return status;
        }
        *out_fd = source_fd;
        *out_borrowed = 1;
        return 0;
    }
    return filed_dispatch_dup_endpoint_to_fixed(source_fd, target_fd, out_fd);
}

static void filed_dispatch_close_prepared_endpoint(int *fd, int borrowed)
{
    if (fd == NULL || *fd < 16) {
        return;
    }
    (void)filed_dispatch_set_inherit(*fd, 0);
    if (!borrowed) {
        (void)pacha_fd_close(*fd);
    }
    *fd = -1;
}

static int64_t filed_status_to_wire(filed_status_t status)
{
    switch (status) {
    case FILED_OK:
        return 0;
    case FILED_ERR_NOT_FOUND:
        return -2;
    case FILED_ERR_NOT_DIR:
        return -20;
    case FILED_ERR_IS_DIR:
        return -21;
    case FILED_ERR_EXISTS:
        return -17;
    case FILED_ERR_DENIED:
        return -13;
    case FILED_ERR_INVALID:
        return -22;
    case FILED_ERR_CROSS_MOUNT:
        return -18;
    case FILED_ERR_NOT_EMPTY:
        return -39;
    case FILED_ERR_IO:
        return -5;
    case FILED_ERR_UNSUPPORTED:
        return -95;
    case FILED_ERR_BAD_FORMAT:
    case FILED_ERR_INVALID_IMAGE:
        return -8;
    case FILED_ERR_LOOP:
        return -40;
    case FILED_ERR_OVERFLOW:
        return -75;
    case FILED_ERR_FULL:
        return -28;
    }
    return -22;
}

static int filed_release_reclaimed_object(
    filed_runtime_t *runtime,
    const filed_vfs_reclaim_result_t *reclaim)
{
    if (runtime == NULL || reclaim == NULL || !reclaim->released || reclaim->backend_object == 0) {
        return 0;
    }
    return filed_backend_release_object(runtime, reclaim->backend_object);
}

static int64_t filed_close_handle_runtime(
    filed_runtime_t *runtime,
    filed_handle_id_t handle_id)
{
    filed_vfs_reclaim_result_t reclaim;
    if (runtime == NULL) {
        return filed_status_to_wire(FILED_ERR_INVALID);
    }

    memset(&reclaim, 0, sizeof(reclaim));
    const filed_status_t status = filed_vfs_close_handle_ex(&runtime->vfs, handle_id, &reclaim);
    if (status != FILED_OK) {
        return filed_status_to_wire(status);
    }
    if (reclaim.released && reclaim.backend_object != 0) {
        const int flush_status = filed_page_cache_flush_object(runtime, reclaim.backend_object);
        if (flush_status != 0) {
            return flush_status;
        }
    }
    return filed_release_reclaimed_object(runtime, &reclaim);
}

static void filed_write_u64_le(void *base, uint64_t offset, uint64_t value)
{
    unsigned char *p = (unsigned char *)base + offset;
    for (unsigned int i = 0; i < 8; ++i) {
        p[i] = (unsigned char)(value >> (i * 8u));
    }
}

static filed_vnode_kind_t filed_kind_from_unix_type(uint64_t kind)
{
    switch (kind & 0170000u) {
    case 0040000u:
        return FILED_VNODE_DIRECTORY;
    case 0100000u:
        return FILED_VNODE_REGULAR;
    case 0120000u:
        return FILED_VNODE_SYMLINK;
    case 0010000u:
        return FILED_VNODE_FIFO;
    case 0020000u:
    case 0060000u:
        return FILED_VNODE_DEVICE;
    default:
        return FILED_VNODE_REGULAR;
    }
}

static uint32_t filed_v2_rights_to_vfs(uint64_t rights)
{
    const uint64_t known =
        FILED_V2_RIGHT_LOOKUP |
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_WRITE |
        FILED_V2_RIGHT_EXEC |
        FILED_V2_RIGHT_STAT |
        FILED_V2_RIGHT_GETDENTS |
        FILED_V2_RIGHT_CREATE |
        FILED_V2_RIGHT_REMOVE |
        FILED_V2_RIGHT_RENAME;
    return (uint32_t)(rights & known);
}

static uint32_t filed_v2_open_flags_to_vfs(uint64_t flags)
{
    const uint64_t known =
        FILED_V2_OPEN_CREATE |
        FILED_V2_OPEN_EXCLUSIVE |
        FILED_V2_OPEN_TRUNCATE |
        FILED_V2_OPEN_DIRECTORY |
        FILED_V2_OPEN_NOFOLLOW |
        FILED_V2_OPEN_CLOEXEC |
        FILED_V2_OPEN_APPEND |
        FILED_V2_OPEN_NONBLOCK |
        FILED_V2_OPEN_SYNC;
    return (uint32_t)(flags & known);
}

static uint32_t filed_v2_fd_flags_to_vfs(uint64_t flags)
{
    return (uint32_t)(flags & FILED_V2_FD_CLOEXEC);
}

static uint32_t filed_v2_file_status_flags_to_vfs(uint64_t flags)
{
    const uint64_t known =
        FILED_V2_FILE_APPEND |
        FILED_V2_FILE_NONBLOCK |
        FILED_V2_FILE_SYNC;
    return (uint32_t)(flags & known);
}

static uint64_t filed_vfs_fd_flags_to_wire(uint32_t flags)
{
    return (uint64_t)(flags & FILED_FD_CLOEXEC);
}

static uint64_t filed_vfs_file_status_flags_to_wire(uint32_t flags)
{
    const uint32_t known =
        FILED_FILE_APPEND |
        FILED_FILE_NONBLOCK |
        FILED_FILE_SYNC;
    return (uint64_t)(flags & known);
}

static int filed_v2_flags_are_known(uint64_t fd_flags, uint64_t status_flags)
{
    const uint64_t known_fd = FILED_V2_FD_CLOEXEC;
    const uint64_t known_status =
        FILED_V2_FILE_APPEND |
        FILED_V2_FILE_NONBLOCK |
        FILED_V2_FILE_SYNC;
    return (fd_flags & ~known_fd) == 0 && (status_flags & ~known_status) == 0;
}

static void *filed_map_request_page(
    const struct pacha_ipc_msg *request,
    uint64_t size,
    int *out_fd)
{
    if (request == NULL ||
        out_fd == NULL ||
        request->fd_count < 1 ||
        request->fds == NULL ||
        request->fds[0].fd < 16)
    {
        return NULL;
    }

    *out_fd = (int)request->fds[0].fd;
    return pacha_mmap(
        *out_fd,
        size,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
}

typedef struct filed_page_dispatch_result {
    int64_t status;
    uint64_t result;
    int process_fd;
    int thread_fd;
} filed_page_dispatch_result_t;

static filed_page_dispatch_result_t filed_page_result(int64_t status, uint64_t result)
{
    filed_page_dispatch_result_t out;
    memset(&out, 0, sizeof(out));
    out.status = status;
    out.result = result;
    out.process_fd = -1;
    out.thread_fd = -1;
    return out;
}

static int filed_write_stat_from_backend(
    filed_v2_statx_t *out,
    const storage_v2_statx_reply_t *stat,
    uint64_t handle_id,
    uint64_t object_generation,
    uint64_t dir_generation)
{
    if (out == NULL || stat == NULL) {
        return -22;
    }
    memset(out, 0, sizeof(*out));
    out->handle = handle_id;
    out->mode = stat->mode;
    out->size = stat->size;
    out->blocks = stat->blocks;
    out->nlink = stat->nlink;
    out->kind = stat->kind;
    out->atime_sec = stat->atime_sec;
    out->atime_nsec = stat->atime_nsec;
    out->mtime_sec = stat->mtime_sec;
    out->mtime_nsec = stat->mtime_nsec;
    out->ctime_sec = stat->ctime_sec;
    out->ctime_nsec = stat->ctime_nsec;
    out->object_generation = object_generation;
    out->dir_generation = dir_generation;
    return 0;
}

static filed_vfs_stat_snapshot_t filed_stat_snapshot_from_backend(
    const storage_v2_statx_reply_t *stat,
    uint64_t handle_id,
    uint64_t object_generation,
    uint64_t dir_generation)
{
    filed_vfs_stat_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (stat == NULL) {
        return snapshot;
    }
    snapshot.valid = true;
    snapshot.handle_id = handle_id;
    snapshot.mode = stat->mode;
    snapshot.size = stat->size;
    snapshot.blocks = stat->blocks;
    snapshot.nlink = stat->nlink;
    snapshot.kind = stat->kind;
    snapshot.times_valid = true;
    snapshot.atime_sec = stat->atime_sec;
    snapshot.atime_nsec = stat->atime_nsec;
    snapshot.mtime_sec = stat->mtime_sec;
    snapshot.mtime_nsec = stat->mtime_nsec;
    snapshot.ctime_sec = stat->ctime_sec;
    snapshot.ctime_nsec = stat->ctime_nsec;
    snapshot.object_generation = (filed_generation_t)object_generation;
    snapshot.dir_generation = (filed_generation_t)dir_generation;
    return snapshot;
}

static filed_vfs_stat_snapshot_t filed_directory_snapshot_from_create(
    uint64_t handle_id,
    uint64_t mode,
    uint64_t object_generation,
    uint64_t dir_generation)
{
    filed_vfs_stat_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.valid = true;
    snapshot.handle_id = handle_id;
    snapshot.mode = 0040000u | (mode & 07777u);
    snapshot.size = 0;
    snapshot.blocks = 0;
    snapshot.nlink = 2;
    snapshot.kind = 0040000u;
    snapshot.times_valid = true;
    snapshot.object_generation = (filed_generation_t)object_generation;
    snapshot.dir_generation = (filed_generation_t)dir_generation;
    return snapshot;
}

static filed_vfs_stat_snapshot_t filed_symlink_snapshot_from_create(
    uint64_t handle_id,
    uint64_t target_length,
    uint64_t object_generation,
    uint64_t dir_generation)
{
    filed_vfs_stat_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.valid = true;
    snapshot.handle_id = handle_id;
    snapshot.mode = 0120000u | 0777u;
    snapshot.size = target_length;
    snapshot.blocks = 0;
    snapshot.nlink = 1;
    snapshot.kind = 0120000u;
    snapshot.times_valid = true;
    snapshot.object_generation = (filed_generation_t)object_generation;
    snapshot.dir_generation = (filed_generation_t)dir_generation;
    return snapshot;
}

static int filed_write_stat_from_snapshot(
    filed_v2_statx_t *out,
    const filed_vfs_stat_snapshot_t *snapshot,
    uint64_t handle_id)
{
    if (out == NULL || snapshot == NULL || !snapshot->valid) {
        return -22;
    }
    memset(out, 0, sizeof(*out));
    out->handle = handle_id;
    out->mode = snapshot->mode;
    out->size = snapshot->size;
    out->blocks = snapshot->blocks;
    out->nlink = snapshot->nlink;
    out->kind = snapshot->kind;
    if (snapshot->times_valid) {
        out->atime_sec = snapshot->atime_sec;
        out->atime_nsec = snapshot->atime_nsec;
        out->mtime_sec = snapshot->mtime_sec;
        out->mtime_nsec = snapshot->mtime_nsec;
        out->ctime_sec = snapshot->ctime_sec;
        out->ctime_nsec = snapshot->ctime_nsec;
    }
    out->object_generation = snapshot->object_generation;
    out->dir_generation = snapshot->dir_generation;
    return 0;
}

static int filed_backend_object_for_handle(
    filed_runtime_t *runtime,
    filed_handle_id_t handle_id,
    filed_vfs_io_decision_t *out_decision)
{
    if (runtime == NULL || out_decision == NULL) {
        return -22;
    }
    filed_status_t status = filed_vfs_stat_prepare(&runtime->vfs, handle_id, out_decision);
    return filed_status_to_wire(status);
}

static int filed_name_is_terminated(const char *name, size_t capacity)
{
    return name != NULL && memchr(name, '\0', capacity) != NULL;
}

enum {
    FILED_WALK_RIGHTS =
        FILED_RIGHT_LOOKUP |
        FILED_RIGHT_STAT |
        FILED_RIGHT_GETDENTS,
};

static const char *filed_skip_slashes(const char *path)
{
    while (path != NULL && *path == '/') {
        ++path;
    }
    return path;
}

static int filed_path_is_single_component(const char *path)
{
    path = filed_skip_slashes(path);
    if (path == NULL || *path == '\0') {
        return 0;
    }
    while (*path != '\0' && *path != '/') {
        ++path;
    }
    path = filed_skip_slashes(path);
    return path != NULL && *path == '\0';
}

static int filed_path_component_is_tmp(const char *component, size_t len)
{
    return len == 3u &&
        component[0] == 't' &&
        component[1] == 'm' &&
        component[2] == 'p';
}

static void filed_close_walk_handle(
    filed_runtime_t *runtime,
    filed_handle_id_t handle_id,
    int owned)
{
    if (runtime != NULL &&
        owned &&
        handle_id != 0 &&
        handle_id != runtime->root_handle_id &&
        (!runtime->tmpfs_root_handle_valid || handle_id != runtime->tmpfs_root_handle_id))
    {
        (void)filed_close_handle_runtime(runtime, handle_id);
    }
}

static int64_t filed_lookup_and_open_component(
    filed_runtime_t *runtime,
    filed_handle_id_t parent_handle,
    const char *name,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open);

static int64_t filed_lookup_component_stat(
    filed_runtime_t *runtime,
    filed_handle_id_t parent_handle,
    const char *name,
    uint64_t *out_object_id,
    storage_v2_statx_reply_t *out_stat)
{
    filed_vfs_io_decision_t parent_decision;
    filed_status_t status;
    int64_t reply_status;

    if (runtime == NULL || name == NULL || out_object_id == NULL || out_stat == NULL) {
        return filed_status_to_wire(FILED_ERR_INVALID);
    }
    *out_object_id = 0;
    memset(out_stat, 0, sizeof(*out_stat));
    status = filed_vfs_lookup_prepare(&runtime->vfs, parent_handle, &parent_decision);
    reply_status = filed_status_to_wire(status);
    if (status != FILED_OK) {
        return reply_status;
    }
    reply_status = filed_backend_lookup(runtime, parent_decision.backend_object, name, out_object_id);
    if (reply_status != 0) {
        return reply_status;
    }
    return filed_backend_statx(runtime, *out_object_id, out_stat);
}

static int64_t filed_splice_symlink_target(
    filed_runtime_t *runtime,
    uint64_t object_id,
    const char *rest,
    char *out_path,
    size_t out_path_size)
{
    char target[FILED_V2_SYMLINK_TARGET_BYTES];
    uint64_t target_length = 0;
    int64_t reply_status;
    size_t rest_length;

    if (runtime == NULL || object_id == 0 || rest == NULL || out_path == NULL || out_path_size == 0) {
        return filed_status_to_wire(FILED_ERR_INVALID);
    }
    memset(target, 0, sizeof(target));
    reply_status = filed_backend_readlink(
        runtime,
        object_id,
        target,
        sizeof(target) - 1u,
        &target_length);
    if (reply_status != 0) {
        return reply_status;
    }
    if (target_length == 0 || target_length >= sizeof(target)) {
        return filed_status_to_wire(FILED_ERR_INVALID);
    }
    target[target_length] = '\0';
    rest = filed_skip_slashes(rest);
    rest_length = strlen(rest);
    if (rest_length != 0) {
        if (target_length + 1u + rest_length >= out_path_size) {
            return filed_status_to_wire(FILED_ERR_INVALID);
        }
        memset(out_path, 0, out_path_size);
        memcpy(out_path, target, (size_t)target_length);
        out_path[target_length] = '/';
        memcpy(out_path + target_length + 1u, rest, rest_length + 1u);
    } else {
        if (target_length >= out_path_size) {
            return filed_status_to_wire(FILED_ERR_INVALID);
        }
        memset(out_path, 0, out_path_size);
        memcpy(out_path, target, (size_t)target_length + 1u);
    }
    return 0;
}

static int64_t filed_resolve_parent_path(
    filed_runtime_t *runtime,
    filed_handle_id_t base_dir_handle,
    const char *path,
    uint32_t parent_rights,
    filed_handle_id_t *out_parent_handle,
    int *out_parent_owned,
    char *out_name,
    size_t out_name_size)
{
    int absolute;
    filed_handle_id_t current_handle;
    int current_owned = 0;
    unsigned int symlink_budget = 16;
    char symlink_path[FILED_V2_PATH_BYTES];

    if (runtime == NULL ||
        path == NULL ||
        out_parent_handle == NULL ||
        out_parent_owned == NULL ||
        out_name == NULL ||
        out_name_size == 0 ||
        path[0] == '\0')
    {
        return filed_status_to_wire(FILED_ERR_INVALID);
    }

    absolute = path[0] == '/';
    current_handle =
        absolute || base_dir_handle == 0 ?
            runtime->root_handle_id :
            base_dir_handle;

    *out_parent_handle = 0;
    *out_parent_owned = 0;
    out_name[0] = '\0';

    if (absolute) {
        path = filed_skip_slashes(path);
        if (*path == '\0') {
            return filed_status_to_wire(FILED_ERR_INVALID);
        }
    }

    for (;;) {
        char component[FILED_V2_NAME_BYTES];
        const char *component_start;
        const char *after_slashes;
        size_t component_len;
        int has_more;
        int trailing_slash;

        path = filed_skip_slashes(path);
        if (*path == '\0') {
            filed_close_walk_handle(runtime, current_handle, current_owned);
            return filed_status_to_wire(FILED_ERR_INVALID);
        }

        component_start = path;
        while (*path != '\0' && *path != '/') {
            ++path;
        }
        component_len = (size_t)(path - component_start);
        if (component_len == 0 || component_len >= sizeof(component)) {
            filed_close_walk_handle(runtime, current_handle, current_owned);
            return filed_status_to_wire(FILED_ERR_INVALID);
        }

        after_slashes = filed_skip_slashes(path);
        has_more = *after_slashes != '\0';
        trailing_slash = *path == '/' && !has_more;

        memset(component, 0, sizeof(component));
        memcpy(component, component_start, component_len);

        if (current_handle == runtime->root_handle_id &&
            !current_owned &&
            runtime->tmpfs_root_handle_valid &&
            has_more &&
            filed_path_component_is_tmp(component, component_len))
        {
            current_handle = runtime->tmpfs_root_handle_id;
            path = after_slashes;
            continue;
        }

        if (component_len == 1 && component[0] == '.') {
            if (!has_more) {
                filed_close_walk_handle(runtime, current_handle, current_owned);
                return filed_status_to_wire(FILED_ERR_INVALID);
            }
            path = after_slashes;
            continue;
        }

        if (component_len == 2 && component[0] == '.' && component[1] == '.') {
            filed_vfs_open_result_t parent_open;
            filed_status_t status;
            uint32_t next_rights = FILED_WALK_RIGHTS;

            if (filed_path_is_single_component(after_slashes)) {
                next_rights |= parent_rights;
            }

            if (!has_more) {
                filed_close_walk_handle(runtime, current_handle, current_owned);
                return filed_status_to_wire(FILED_ERR_INVALID);
            }

            memset(&parent_open, 0, sizeof(parent_open));
            status = filed_vfs_open_parent(
                &runtime->vfs,
                current_handle,
                next_rights,
                FILED_OPEN_DIRECTORY,
                &parent_open);
            filed_close_walk_handle(runtime, current_handle, current_owned);
            if (status != FILED_OK) {
                return filed_status_to_wire(status);
            }
            current_handle = parent_open.handle_id;
            current_owned = 1;
            path = after_slashes;
            continue;
        }

        if (!has_more) {
            if (trailing_slash || component_len >= out_name_size) {
                filed_close_walk_handle(runtime, current_handle, current_owned);
                return filed_status_to_wire(FILED_ERR_INVALID);
            }
            memset(out_name, 0, out_name_size);
            memcpy(out_name, component, component_len);
            *out_parent_handle = current_handle;
            *out_parent_owned = current_owned;
            return 0;
        } else {
            filed_vfs_open_result_t next_open;
            uint32_t next_rights = FILED_WALK_RIGHTS;
            uint64_t object_id = 0;
            storage_v2_statx_reply_t stat;
            int64_t symlink_status;
            if (filed_path_is_single_component(after_slashes)) {
                next_rights |= parent_rights;
            }
            symlink_status = filed_lookup_component_stat(
                runtime,
                current_handle,
                component,
                &object_id,
                &stat);
            if (symlink_status != 0) {
                filed_close_walk_handle(runtime, current_handle, current_owned);
                return symlink_status;
            }
            if (filed_kind_from_unix_type(stat.kind) == FILED_VNODE_SYMLINK) {
                if (symlink_budget == 0) {
                    filed_close_walk_handle(runtime, current_handle, current_owned);
                    return filed_status_to_wire(FILED_ERR_LOOP);
                }
                --symlink_budget;
                symlink_status = filed_splice_symlink_target(
                    runtime,
                    object_id,
                    after_slashes,
                    symlink_path,
                    sizeof(symlink_path));
                if (symlink_status != 0) {
                    filed_close_walk_handle(runtime, current_handle, current_owned);
                    return symlink_status;
                }
                if (symlink_path[0] == '/') {
                    filed_close_walk_handle(runtime, current_handle, current_owned);
                    current_handle = runtime->root_handle_id;
                    current_owned = 0;
                }
                path = symlink_path;
                continue;
            }
            const int64_t reply_status = filed_lookup_and_open_component(
                runtime,
                current_handle,
                component,
                next_rights,
                FILED_OPEN_DIRECTORY,
                &next_open);
            filed_close_walk_handle(runtime, current_handle, current_owned);
            if (reply_status != 0) {
                return reply_status;
            }
            current_handle = next_open.handle_id;
            current_owned = 1;
            path = after_slashes;
        }
    }
}

static int64_t filed_lookup_and_open_component(
    filed_runtime_t *runtime,
    filed_handle_id_t parent_handle,
    const char *name,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open)
{
    filed_vfs_io_decision_t parent_decision;
    uint64_t object_id = 0;
    storage_v2_statx_reply_t backend_stat;
    filed_status_t status;
    int64_t reply_status;
    const bool can_use_negative_lookup_cache =
        (open_flags & (FILED_OPEN_CREATE | FILED_OPEN_EXCLUSIVE | FILED_OPEN_TRUNCATE)) == 0;

    if (can_use_negative_lookup_cache) {
        status = filed_vfs_open_cached_child(
            &runtime->vfs,
            parent_handle,
            name,
            rights,
            open_flags,
            out_open);
        if (status == FILED_OK) {
            return 0;
        }
        if (status != FILED_ERR_NOT_FOUND) {
            return filed_status_to_wire(status);
        }
    }

    status = filed_vfs_lookup_prepare(&runtime->vfs, parent_handle, &parent_decision);
    reply_status = filed_status_to_wire(status);
    if (status != FILED_OK) {
        return reply_status;
    }

    if (can_use_negative_lookup_cache &&
        filed_negative_lookup_cache_get(
            parent_decision.backend_object,
            parent_decision.dir_generation,
            name,
            &reply_status))
    {
        return reply_status;
    }

    reply_status = filed_backend_lookup(
        runtime,
        parent_decision.backend_object,
        name,
        &object_id);
    if (reply_status != 0 &&
        (open_flags & FILED_OPEN_CREATE) != 0)
    {
        reply_status = filed_backend_create(
            runtime,
            parent_decision.backend_object,
            name,
            0100644u,
            &object_id);
        if (reply_status != 0) {
            return reply_status;
        }
        filed_dir_cache_invalidate_dir(parent_decision.backend_object);
        memset(&backend_stat, 0, sizeof(backend_stat));
        reply_status = filed_backend_statx(
            runtime,
            object_id,
            &backend_stat);
        if (reply_status != 0) {
            return reply_status;
        }
        status = filed_vfs_create_backend_child(
            &runtime->vfs,
            parent_handle,
            object_id,
            filed_kind_from_unix_type(backend_stat.kind),
            name,
            rights,
            open_flags,
            out_open);
        if (status == FILED_OK) {
            const filed_vfs_stat_snapshot_t snapshot =
                filed_stat_snapshot_from_backend(
                    &backend_stat,
                    out_open->handle_id,
                    out_open->object_generation,
                    out_open->dir_generation);
            (void)filed_vfs_update_stat_snapshot(
                &runtime->vfs,
                object_id,
                &snapshot);
        }
        return filed_status_to_wire(status);
    }
    if (reply_status != 0) {
        if (can_use_negative_lookup_cache && reply_status == filed_status_to_wire(FILED_ERR_NOT_FOUND)) {
            filed_negative_lookup_cache_store(
                parent_decision.backend_object,
                parent_decision.dir_generation,
                name,
                reply_status);
        }
        return reply_status;
    }
    if ((open_flags & (FILED_OPEN_CREATE | FILED_OPEN_EXCLUSIVE)) ==
        (FILED_OPEN_CREATE | FILED_OPEN_EXCLUSIVE))
    {
        return filed_status_to_wire(FILED_ERR_EXISTS);
    }

    memset(&backend_stat, 0, sizeof(backend_stat));
    reply_status = filed_backend_statx(
        runtime,
        object_id,
        &backend_stat);
    if (reply_status != 0) {
        return reply_status;
    }

    status = filed_vfs_open_backend_child(
        &runtime->vfs,
        parent_handle,
        object_id,
        filed_kind_from_unix_type(backend_stat.kind),
        name,
        rights,
        open_flags,
        out_open);
    if (status == FILED_OK) {
        filed_vfs_stat_snapshot_t current_snapshot;
        memset(&current_snapshot, 0, sizeof(current_snapshot));
        if (filed_page_cache_object_dirty(object_id) &&
            filed_vfs_get_stat_snapshot(
                &runtime->vfs,
                out_open->handle_id,
                &current_snapshot) == FILED_OK &&
            current_snapshot.valid)
        {
            out_open->object_generation = current_snapshot.object_generation;
            out_open->dir_generation = current_snapshot.dir_generation;
        } else {
            const filed_vfs_stat_snapshot_t snapshot =
                filed_stat_snapshot_from_backend(
                    &backend_stat,
                    out_open->handle_id,
                    out_open->object_generation,
                    out_open->dir_generation);
            (void)filed_vfs_update_stat_snapshot(
                &runtime->vfs,
                object_id,
                &snapshot);
        }
    }
    if (status == FILED_OK && (open_flags & FILED_OPEN_TRUNCATE) != 0) {
        reply_status = filed_page_cache_flush_object(runtime, object_id);
        if (reply_status != 0) {
            (void)filed_vfs_close_handle(&runtime->vfs, out_open->handle_id);
            memset(out_open, 0, sizeof(*out_open));
            return reply_status;
        }
        reply_status = filed_backend_truncate(
            runtime,
            object_id,
            0);
        if (reply_status != 0) {
            (void)filed_vfs_close_handle(&runtime->vfs, out_open->handle_id);
            memset(out_open, 0, sizeof(*out_open));
            return reply_status;
        }
        filed_page_cache_invalidate_object(runtime, object_id);
        (void)filed_vfs_note_truncate(&runtime->vfs, out_open->handle_id, 0);
        {
            filed_vfs_stat_snapshot_t snapshot;
            memset(&snapshot, 0, sizeof(snapshot));
            if (filed_vfs_get_stat_snapshot(&runtime->vfs, out_open->handle_id, &snapshot) == FILED_OK) {
                out_open->object_generation = snapshot.object_generation;
                out_open->dir_generation = snapshot.dir_generation;
            }
        }
    }
    return filed_status_to_wire(status);
}

static int64_t filed_openat_path(
    filed_runtime_t *runtime,
    const filed_v2_openat_t *openat,
    filed_vfs_open_result_t *out_open)
{
    const uint32_t rights = filed_v2_rights_to_vfs(openat->rights);
    const uint32_t open_flags = filed_v2_open_flags_to_vfs(openat->open_flags);
    const char *path = openat->name;
    const int absolute = path[0] == '/';
    filed_handle_id_t current_handle =
        absolute || openat->dir_handle == 0 ?
            runtime->root_handle_id :
            (filed_handle_id_t)(uint32_t)openat->dir_handle;
    int current_owned = 0;
    unsigned int symlink_budget = 16;
    char symlink_path[FILED_V2_PATH_BYTES];

    if (path[0] == '\0') {
        return filed_status_to_wire(FILED_ERR_INVALID);
    }

    if (absolute) {
        path = filed_skip_slashes(path);
        if (*path == '\0') {
            const filed_status_t status = filed_vfs_open_root(
                &runtime->vfs,
                runtime->root_mount_id,
                rights,
                open_flags | FILED_OPEN_DIRECTORY,
                out_open);
            return filed_status_to_wire(status);
        }
    }

    for (;;) {
        char component[FILED_V2_NAME_BYTES];
        const char *component_start;
        const char *after_slashes;
        size_t component_len;
        int has_more;
        int require_directory;
        int final_component;

        path = filed_skip_slashes(path);
        if (*path == '\0') {
            filed_close_walk_handle(runtime, current_handle, current_owned);
            return filed_status_to_wire(FILED_ERR_INVALID);
        }

        component_start = path;
        while (*path != '\0' && *path != '/') {
            ++path;
        }
        component_len = (size_t)(path - component_start);
        if (component_len == 0 || component_len >= sizeof(component)) {
            filed_close_walk_handle(runtime, current_handle, current_owned);
            return filed_status_to_wire(FILED_ERR_INVALID);
        }

        after_slashes = filed_skip_slashes(path);
        has_more = *after_slashes != '\0';
        require_directory = (*path == '/');
        final_component = !has_more;

        memset(component, 0, sizeof(component));
        memcpy(component, component_start, component_len);

        if (component_len == 1 && component[0] == '.') {
            if (final_component) {
                const filed_status_t status = filed_vfs_open_existing(
                    &runtime->vfs,
                    current_handle,
                    rights,
                    open_flags | (require_directory ? FILED_OPEN_DIRECTORY : 0),
                    out_open);
                filed_close_walk_handle(runtime, current_handle, current_owned);
                return filed_status_to_wire(status);
            }
            path = after_slashes;
            continue;
        }

        if (component_len == 2 && component[0] == '.' && component[1] == '.') {
            filed_vfs_open_result_t parent_open;
            const uint32_t next_rights = final_component ? rights : FILED_WALK_RIGHTS;
            const uint32_t next_flags =
                final_component ?
                    (open_flags | (require_directory ? FILED_OPEN_DIRECTORY : 0)) :
                    FILED_OPEN_DIRECTORY;
            filed_status_t status;

            memset(&parent_open, 0, sizeof(parent_open));
            status = filed_vfs_open_parent(
                &runtime->vfs,
                current_handle,
                next_rights,
                next_flags,
                &parent_open);
            filed_close_walk_handle(runtime, current_handle, current_owned);
            if (status != FILED_OK) {
                return filed_status_to_wire(status);
            }
            if (final_component) {
                *out_open = parent_open;
                return 0;
            }
            current_handle = parent_open.handle_id;
            current_owned = 1;
            path = after_slashes;
            continue;
        }

        if (final_component) {
            const int64_t reply_status = filed_lookup_and_open_component(
                runtime,
                current_handle,
                component,
                rights,
                open_flags | (require_directory ? FILED_OPEN_DIRECTORY : 0),
                out_open);
            filed_close_walk_handle(runtime, current_handle, current_owned);
            return reply_status;
        } else {
            filed_vfs_open_result_t next_open;
            uint32_t next_rights = FILED_WALK_RIGHTS;
            uint64_t object_id = 0;
            storage_v2_statx_reply_t stat;
            int64_t symlink_status;
            if ((open_flags & FILED_OPEN_CREATE) != 0 &&
                filed_path_is_single_component(after_slashes))
            {
                next_rights |= FILED_RIGHT_CREATE;
            }
            symlink_status = filed_lookup_component_stat(
                runtime,
                current_handle,
                component,
                &object_id,
                &stat);
            if (symlink_status != 0) {
                filed_close_walk_handle(runtime, current_handle, current_owned);
                return symlink_status;
            }
            if (filed_kind_from_unix_type(stat.kind) == FILED_VNODE_SYMLINK) {
                if (symlink_budget == 0) {
                    filed_close_walk_handle(runtime, current_handle, current_owned);
                    return filed_status_to_wire(FILED_ERR_LOOP);
                }
                --symlink_budget;
                symlink_status = filed_splice_symlink_target(
                    runtime,
                    object_id,
                    after_slashes,
                    symlink_path,
                    sizeof(symlink_path));
                if (symlink_status != 0) {
                    filed_close_walk_handle(runtime, current_handle, current_owned);
                    return symlink_status;
                }
                if (symlink_path[0] == '/') {
                    filed_close_walk_handle(runtime, current_handle, current_owned);
                    current_handle = runtime->root_handle_id;
                    current_owned = 0;
                }
                path = symlink_path;
                continue;
            }
            const int64_t reply_status = filed_lookup_and_open_component(
                runtime,
                current_handle,
                component,
                next_rights,
                FILED_OPEN_DIRECTORY,
                &next_open);
            filed_close_walk_handle(runtime, current_handle, current_owned);
            if (reply_status != 0) {
                return reply_status;
            }
            current_handle = next_open.handle_id;
            current_owned = 1;
            path = after_slashes;
        }
    }
}

static filed_page_dispatch_result_t filed_dispatch_openat_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_openat_t *openat = (filed_v2_openat_t *)page;
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
        openat->reserved0 = 0;
        filed_runtime_publish_generation(
            runtime,
            open_result.handle_id,
            open_result.object_generation,
            open_result.dir_generation);
    }
    return filed_page_result(reply_status, result);
}

static filed_page_dispatch_result_t filed_dispatch_validate_open_cache_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_validate_open_cache_t *validate =
        (filed_v2_validate_open_cache_t *)page;
    filed_vfs_io_decision_t cached_decision;
    filed_vfs_open_result_t fresh_open;
    filed_v2_openat_t openat;
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
        if ((validate->open_flags & FILED_V2_OPEN_DIRECTORY) == 0 ||
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

static filed_page_dispatch_result_t filed_dispatch_stat_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_statx_t *wire_stat = (filed_v2_statx_t *)page;
    filed_vfs_io_decision_t decision;
    filed_vfs_stat_snapshot_t snapshot;
    storage_v2_statx_reply_t backend_stat;
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

static filed_page_dispatch_result_t filed_dispatch_utimens_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_utimens_t *utimens = (filed_v2_utimens_t *)page;
    if (runtime == NULL || utimens == NULL) {
        return filed_page_result(-22, 0);
    }
    if ((utimens->mask & ~((uint64_t)FILED_V2_UTIMENS_ATIME | (uint64_t)FILED_V2_UTIMENS_MTIME)) != 0) {
        return filed_page_result(-22, 0);
    }
    filed_vfs_io_decision_t decision;
    int backend_status = filed_backend_object_for_handle(
        runtime,
        (filed_handle_id_t)(uint32_t)utimens->handle,
        &decision);
    if (backend_status != 0) {
        return filed_page_result(backend_status, 0);
    }
    backend_status = filed_backend_utimens(
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
    filed_status_t status = filed_vfs_update_times(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)utimens->handle,
        (uint32_t)utimens->mask,
        utimens->atime_sec,
        utimens->atime_nsec,
        utimens->mtime_sec,
        utimens->mtime_nsec);
    return filed_page_result(filed_status_to_wire(status), 0);
}

static int filed_ensure_stat_snapshot(
    filed_runtime_t *runtime,
    filed_handle_id_t handle_id)
{
    filed_vfs_io_decision_t decision;
    filed_vfs_stat_snapshot_t snapshot;
    storage_v2_statx_reply_t backend_stat;
    filed_status_t status = filed_vfs_stat_prepare(&runtime->vfs, handle_id, &decision);
    if (status != FILED_OK) {
        return filed_status_to_wire(status);
    }
    memset(&snapshot, 0, sizeof(snapshot));
    status = filed_vfs_get_stat_snapshot(&runtime->vfs, handle_id, &snapshot);
    if (status != FILED_OK) {
        return filed_status_to_wire(status);
    }
    if (snapshot.valid) {
        return 0;
    }
    memset(&backend_stat, 0, sizeof(backend_stat));
    int64_t reply_status = filed_backend_statx(
        runtime,
        decision.backend_object,
        &backend_stat);
    if (reply_status != 0) {
        return (int)reply_status;
    }
    snapshot = filed_stat_snapshot_from_backend(
        &backend_stat,
        handle_id,
        decision.object_generation,
        decision.dir_generation);
    status = filed_vfs_update_stat_snapshot(
        &runtime->vfs,
        decision.backend_object,
        &snapshot);
    return filed_status_to_wire(status);
}

static filed_page_dispatch_result_t filed_dispatch_chmod_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_chmod_t *chmod_req = (filed_v2_chmod_t *)page;
    if (runtime == NULL || chmod_req == NULL || chmod_req->reserved0 != 0 || chmod_req->reserved1 != 0) {
        return filed_page_result(-22, 0);
    }
    int status = filed_ensure_stat_snapshot(
        runtime,
        (filed_handle_id_t)(uint32_t)chmod_req->handle);
    if (status != 0) {
        return filed_page_result(status, 0);
    }
    filed_vfs_io_decision_t decision;
    status = filed_backend_object_for_handle(
        runtime,
        (filed_handle_id_t)(uint32_t)chmod_req->handle,
        &decision);
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
    filed_status_t vfs_status = filed_vfs_update_mode(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)chmod_req->handle,
        chmod_req->mode);
    return filed_page_result(filed_status_to_wire(vfs_status), 0);
}

static filed_page_dispatch_result_t filed_dispatch_pread_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_io_t *io = (filed_v2_io_t *)page;
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
        if (length > FILED_V2_IO_BYTES) {
            length = FILED_V2_IO_BYTES;
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
            filed_runtime_publish_generation(
                runtime,
                (filed_handle_id_t)(uint32_t)io->handle,
                decision.object_generation,
                decision.dir_generation);
        }
    }
    return filed_page_result(reply_status, bytes);
}

static filed_page_dispatch_result_t filed_dispatch_pread_to_vmo_page(
    filed_runtime_t *runtime,
    void *page,
    int vmo_fd)
{
    filed_v2_pread_vmo_t *pread_vmo = (filed_v2_pread_vmo_t *)page;
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

    unsigned char *mapped = pacha_mmap(
        vmo_fd,
        pread_vmo->vmo_offset + length,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (mapped == NULL) {
        return filed_page_result(-12, 0);
    }

    reply_status = filed_cached_pread(
        runtime,
        decision.backend_object,
        decision.offset,
        mapped + pread_vmo->vmo_offset,
        length,
        &bytes);
    (void)pacha_munmap(mapped, pread_vmo->vmo_offset + length);
    if (reply_status == 0) {
        filed_runtime_publish_generation(
            runtime,
            (filed_handle_id_t)(uint32_t)pread_vmo->handle,
            decision.object_generation,
            decision.dir_generation);
    }
    return filed_page_result(reply_status, bytes);
}

static filed_page_dispatch_result_t filed_create_file_vmo_cache_entry(
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
        PACHA_FD_RIGHT_MAP_WRITE;
    const int vmo_fd = pacha_vmo_create(length, rights, 0);
    if (vmo_fd < 16) {
        return filed_page_result(-12, 0);
    }
    unsigned char *mapped = pacha_mmap(
        vmo_fd,
        length,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (mapped == NULL) {
        (void)pacha_fd_close(vmo_fd);
        return filed_page_result(-12, 0);
    }

    uint64_t bytes = 0;
    const int64_t reply_status = filed_cached_pread(
        runtime,
        decision->backend_object,
        file_offset,
        mapped,
        length,
        &bytes);
    (void)pacha_munmap(mapped, length);
    if (reply_status != 0) {
        (void)pacha_fd_close(vmo_fd);
        return filed_page_result(reply_status, 0);
    }

    filed_file_vmo_cache_entry_t *entry = filed_file_vmo_cache_slot(runtime);
    memset(entry, 0, sizeof(*entry));
    entry->active = 1;
    entry->vmo_fd = vmo_fd;
    entry->backend_object = decision->backend_object;
    entry->object_generation = decision->object_generation;
    entry->file_offset = file_offset;
    entry->length = length;
    entry->clock = ++runtime->file_vmo_cache_clock;
    filed_file_vmo_cache_stores++;
    *out_entry = entry;
    return filed_page_result(0, bytes);
}

static int filed_dispatch_file_vmo_v2(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request,
    void *reply_page,
    const pacha_service_request_header_t *header)
{
    if (runtime == NULL ||
        request == NULL ||
        reply_page == NULL ||
        header == NULL ||
        header->payload_size < sizeof(filed_v2_file_vmo_request_t))
    {
        return filed_send_reply_v2(reply_fd, reply_page, header, -22, 0, 0);
    }

    const filed_v2_file_vmo_request_t *file_vmo =
        (const filed_v2_file_vmo_request_t *)((const uint8_t *)reply_page + PACHA_SERVICE_HEADER_BYTES);
    filed_page_dispatch_result_t result = filed_page_result(-22, 0);
    filed_file_vmo_cache_entry_t *entry = NULL;
    if (file_vmo->length != 0 &&
        file_vmo->length <= FILED_FILE_VMO_MAX_BYTES &&
        file_vmo->reserved0 == 0 &&
        file_vmo->reserved1 == 0 &&
        file_vmo->flags == 0)
    {
        filed_vfs_io_decision_t decision;
        filed_status_t status = filed_vfs_pread_prepare(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)file_vmo->handle,
            file_vmo->file_offset,
            file_vmo->length,
            &decision);
        int64_t reply_status = filed_status_to_wire(status);
        if (status == FILED_OK && decision.length == file_vmo->length) {
            entry = filed_file_vmo_cache_lookup(
                runtime,
                decision.backend_object,
                decision.object_generation,
                decision.offset,
                decision.length);
            if (entry != NULL) {
                result = filed_page_result(0, decision.length);
            } else {
                result = filed_create_file_vmo_cache_entry(
                    runtime,
                    &decision,
                    decision.offset,
                    decision.length,
                    &entry);
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

    pacha_service_reply_header_init(
        (pacha_service_reply_header_t *)reply_page,
        header,
        result.status,
        PACHA_SERVICE_ERROR_FILED_VFS,
        result.status < 0 ? 0 : result.result,
        0);
    struct pacha_ipc_fd fd = {
        .fd = (uint64_t)(uint32_t)(entry != NULL ? entry->vmo_fd : -1),
        .rights =
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ,
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
        const uint64_t token = filed_error_token(
            result.status,
            header->op,
            PACHA_ERRCONV_STAGE_STATUS_MAP,
            result.status,
            header->request_id,
            request->fd_count,
            file_vmo->handle,
            0,
            "filed v2 file-vmo negative reply");
        pacha_service_reply_header_t *reply_header = (pacha_service_reply_header_t *)reply_page;
        reply_header->result = token;
        reply.word2 = token;
    }
    const int reply_status = pacha_ipc_reply(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    return reply_status;
}

static filed_page_dispatch_result_t filed_dispatch_read_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_io_t *io = (filed_v2_io_t *)page;
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
        if (length > FILED_V2_IO_BYTES) {
            length = FILED_V2_IO_BYTES;
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
            filed_runtime_publish_generation(
                runtime,
                (filed_handle_id_t)(uint32_t)io->handle,
                decision.object_generation,
                decision.dir_generation);
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

static filed_page_dispatch_result_t filed_dispatch_pwrite_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_io_t *io = (filed_v2_io_t *)page;
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
        if (length > FILED_V2_IO_BYTES) {
            length = FILED_V2_IO_BYTES;
        }
        if (decision.offset == UINT64_MAX) {
            filed_vfs_stat_snapshot_t snapshot;
            storage_v2_statx_reply_t backend_stat;
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

static filed_page_dispatch_result_t filed_dispatch_write_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_io_t *io = (filed_v2_io_t *)page;
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
        if (length > FILED_V2_IO_BYTES) {
            length = FILED_V2_IO_BYTES;
        }
        if (decision.offset == UINT64_MAX) {
            filed_vfs_stat_snapshot_t snapshot;
            storage_v2_statx_reply_t backend_stat;
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

static filed_page_dispatch_result_t filed_dispatch_seek_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_seek_t *seek = (filed_v2_seek_t *)page;
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
                storage_v2_statx_reply_t backend_stat;
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

static filed_page_dispatch_result_t filed_dispatch_fsync_page(
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
        reply_status = filed_page_cache_flush_object(runtime, decision.backend_object);
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

    const uint64_t dirty_count = filed_page_cache_dirty_count();
    const uint64_t backend_dirty_hint = filed_kobox_backend_dirty_hint(&runtime->backend);
    const int flush_status = filed_page_cache_flush_object(runtime, 0);
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
    printf(
        "[filed] sync_all page_cache_dirty=%llu backend_dirty_hint=%llu backend_status=%d\n",
        (unsigned long long)dirty_count,
        (unsigned long long)backend_dirty_hint,
        backend_status);
    fflush(stdout);
    return backend_status;
}

static filed_page_dispatch_result_t filed_dispatch_truncate_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_truncate_t *truncate = (filed_v2_truncate_t *)page;
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
            reply_status = filed_page_cache_flush_object(runtime, decision.backend_object);
            if (reply_status == 0) {
                reply_status = filed_backend_truncate(
                    runtime,
                    decision.backend_object,
                    truncate->size);
                if (reply_status == 0) {
                    filed_page_cache_invalidate_object(runtime, decision.backend_object);
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

static uint64_t filed_lookup_cache_target_object(
    filed_runtime_t *runtime,
    filed_handle_id_t parent_handle,
    uint64_t parent_backend_object,
    const char *name)
{
    uint64_t object_id = 0;
    if (runtime == NULL || parent_backend_object == 0 || name == NULL) {
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
        filed_target_lookup_backend_hits++;
    } else {
        filed_target_lookup_misses++;
    }
    return object_id;
}

static void filed_invalidate_mutated_object(
    filed_runtime_t *runtime,
    uint64_t backend_object)
{
    filed_page_cache_invalidate_object(runtime, backend_object);
}

static int filed_flush_mutated_object(
    filed_runtime_t *runtime,
    uint64_t backend_object)
{
    return filed_page_cache_flush_object(runtime, backend_object);
}

static filed_page_dispatch_result_t filed_dispatch_unlink_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_unlink_t *unlink = (filed_v2_unlink_t *)page;
    filed_vfs_io_decision_t decision;
    filed_status_t status = FILED_ERR_INVALID;
    int64_t reply_status = -22;
    uint64_t target_object = 0;
    if (unlink->reserved0 == 0 &&
        filed_name_is_terminated(unlink->name, sizeof(unlink->name)))
    {
        filed_handle_id_t dir_handle = 0;
        int dir_owned = 0;
        char name[FILED_V2_NAME_BYTES];
        const filed_handle_id_t base_dir =
            unlink->dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)unlink->dir_handle;

        memset(name, 0, sizeof(name));
        reply_status = filed_resolve_parent_path(
            runtime,
            base_dir,
            unlink->name,
            FILED_RIGHT_REMOVE,
            &dir_handle,
            &dir_owned,
            name,
            sizeof(name));
        if (reply_status == 0) {
            status = filed_vfs_unlink_prepare(&runtime->vfs, dir_handle, name, &decision);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK) {
                target_object = filed_lookup_cache_target_object(
                    runtime,
                    dir_handle,
                    decision.backend_object,
                    name);
                reply_status = filed_flush_mutated_object(runtime, target_object);
                if (reply_status == 0) {
                    reply_status = filed_backend_unlink(
                        runtime,
                        decision.backend_object,
                        name);
                }
                if (reply_status == 0) {
                    filed_dir_cache_invalidate_dir(decision.backend_object);
                    filed_invalidate_mutated_object(runtime, target_object);
                    filed_vfs_reclaim_result_t reclaim;
                    memset(&reclaim, 0, sizeof(reclaim));
                    status = filed_vfs_unlink_commit_ex(&runtime->vfs, dir_handle, name, &reclaim);
                    if (status != FILED_OK) {
                        reply_status = filed_status_to_wire(status);
                    } else {
                        filed_runtime_publish_backend_object_generation(
                            runtime,
                            decision.backend_object);
                        if (target_object != 0) {
                            filed_runtime_publish_backend_object_generation(
                                runtime,
                                target_object);
                        }
                        const int release_status = filed_release_reclaimed_object(runtime, &reclaim);
                        if (release_status != 0) {
                            reply_status = release_status;
                        }
                    }
                }
            }
            filed_close_walk_handle(runtime, dir_handle, dir_owned);
        }
    }

    return filed_page_result(reply_status, 0);
}

static filed_page_dispatch_result_t filed_dispatch_mkdir_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_mkdir_t *mkdir = (filed_v2_mkdir_t *)page;
    filed_vfs_io_decision_t decision;
    filed_status_t status = FILED_ERR_INVALID;
    int64_t reply_status = -22;
    uint64_t object_id = 0;
    if (filed_name_is_terminated(mkdir->name, sizeof(mkdir->name))) {
        filed_handle_id_t dir_handle = 0;
        int dir_owned = 0;
        char name[FILED_V2_NAME_BYTES];
        const filed_handle_id_t base_dir =
            mkdir->dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)mkdir->dir_handle;

        memset(name, 0, sizeof(name));
        reply_status = filed_resolve_parent_path(
            runtime,
            base_dir,
            mkdir->name,
            FILED_RIGHT_CREATE,
            &dir_handle,
            &dir_owned,
            name,
            sizeof(name));
        if (reply_status == 0) {
            status = filed_vfs_create_prepare(&runtime->vfs, dir_handle, name, &decision);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK) {
                reply_status = filed_backend_mkdir(
                    runtime,
                    decision.backend_object,
                    name,
                    mkdir->mode,
                    &object_id);
                if (reply_status == 0 && object_id != 0) {
                    filed_dir_cache_invalidate_dir(decision.backend_object);
                    filed_vfs_open_result_t opened;
                    memset(&opened, 0, sizeof(opened));
                    if (filed_vfs_create_backend_child(
                            &runtime->vfs,
                            dir_handle,
                            object_id,
                            FILED_VNODE_DIRECTORY,
                            name,
                            FILED_RIGHT_LOOKUP |
                                FILED_RIGHT_STAT |
                                FILED_RIGHT_GETDENTS |
                                FILED_RIGHT_CREATE |
                                FILED_RIGHT_REMOVE |
                                FILED_RIGHT_RENAME,
                            FILED_OPEN_DIRECTORY,
                            &opened) == FILED_OK)
                    {
                        const filed_vfs_stat_snapshot_t snapshot =
                            filed_directory_snapshot_from_create(
                                opened.handle_id,
                                mkdir->mode,
                                opened.object_generation,
                                opened.dir_generation);
                        (void)filed_vfs_update_stat_snapshot(
                            &runtime->vfs,
                            object_id,
                            &snapshot);
                        filed_runtime_publish_backend_object_generation(
                            runtime,
                            decision.backend_object);
                        (void)filed_vfs_close_handle(&runtime->vfs, opened.handle_id);
                    }
                }
            }
            filed_close_walk_handle(runtime, dir_handle, dir_owned);
        }
    }

    return filed_page_result(reply_status, 0);
}

static filed_page_dispatch_result_t filed_dispatch_symlink_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_symlink_t *symlink = (filed_v2_symlink_t *)page;
    filed_vfs_io_decision_t decision;
    filed_status_t status = FILED_ERR_INVALID;
    int64_t reply_status = -22;
    uint64_t object_id = 0;
    if (symlink->target_length > 0 &&
        symlink->target_length < sizeof(symlink->target) &&
        symlink->target[symlink->target_length] == '\0' &&
        filed_name_is_terminated(symlink->name, sizeof(symlink->name)))
    {
        filed_handle_id_t dir_handle = 0;
        int dir_owned = 0;
        char name[FILED_V2_NAME_BYTES];
        const filed_handle_id_t base_dir =
            symlink->dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)symlink->dir_handle;

        memset(name, 0, sizeof(name));
        reply_status = filed_resolve_parent_path(
            runtime,
            base_dir,
            symlink->name,
            FILED_RIGHT_CREATE,
            &dir_handle,
            &dir_owned,
            name,
            sizeof(name));
        if (reply_status == 0) {
            status = filed_vfs_create_prepare(&runtime->vfs, dir_handle, name, &decision);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK) {
                reply_status = filed_backend_symlink(
                    runtime,
                    decision.backend_object,
                    name,
                    symlink->target,
                    symlink->target_length,
                    &object_id);
                if (reply_status == 0 && object_id != 0) {
                    filed_dir_cache_invalidate_dir(decision.backend_object);
                    filed_vfs_open_result_t opened;
                    memset(&opened, 0, sizeof(opened));
                    if (filed_vfs_create_backend_child(
                            &runtime->vfs,
                            dir_handle,
                            object_id,
                            FILED_VNODE_SYMLINK,
                            name,
                            FILED_RIGHT_LOOKUP | FILED_RIGHT_READ | FILED_RIGHT_STAT,
                            FILED_OPEN_NOFOLLOW,
                            &opened) == FILED_OK)
                    {
                        const filed_vfs_stat_snapshot_t snapshot =
                            filed_symlink_snapshot_from_create(
                                opened.handle_id,
                                symlink->target_length,
                                opened.object_generation,
                                opened.dir_generation);
                        (void)filed_vfs_update_stat_snapshot(
                            &runtime->vfs,
                            object_id,
                            &snapshot);
                        filed_runtime_publish_backend_object_generation(
                            runtime,
                            decision.backend_object);
                        (void)filed_vfs_close_handle(&runtime->vfs, opened.handle_id);
                    }
                }
            }
            filed_close_walk_handle(runtime, dir_handle, dir_owned);
        }
    }

    return filed_page_result(reply_status, object_id);
}

static filed_page_dispatch_result_t filed_dispatch_readlink_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_readlink_t *readlink = (filed_v2_readlink_t *)page;
    filed_vfs_io_decision_t parent_decision;
    uint64_t object_id = 0;
    storage_v2_statx_reply_t backend_stat;
    int64_t reply_status = -22;
    uint64_t target_length = 0;
    if (filed_name_is_terminated(readlink->name, sizeof(readlink->name))) {
        filed_handle_id_t dir_handle = 0;
        int dir_owned = 0;
        char name[FILED_V2_NAME_BYTES];
        const filed_handle_id_t base_dir =
            readlink->dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)readlink->dir_handle;

        memset(name, 0, sizeof(name));
        reply_status = filed_resolve_parent_path(
            runtime,
            base_dir,
            readlink->name,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_READ,
            &dir_handle,
            &dir_owned,
            name,
            sizeof(name));
        if (reply_status == 0) {
            filed_status_t status = filed_vfs_lookup_prepare(&runtime->vfs, dir_handle, &parent_decision);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK) {
                reply_status = filed_backend_lookup(
                    runtime,
                    parent_decision.backend_object,
                    name,
                    &object_id);
            }
            if (reply_status == 0) {
                memset(&backend_stat, 0, sizeof(backend_stat));
                reply_status = filed_backend_statx(runtime, object_id, &backend_stat);
            }
            if (reply_status == 0 &&
                filed_kind_from_unix_type(backend_stat.kind) != FILED_VNODE_SYMLINK)
            {
                reply_status = filed_status_to_wire(FILED_ERR_INVALID);
            }
            if (reply_status == 0) {
                memset(readlink->target, 0, sizeof(readlink->target));
                reply_status = filed_backend_readlink(
                    runtime,
                    object_id,
                    readlink->target,
                    sizeof(readlink->target) - 1u,
                    &target_length);
                if (reply_status == 0) {
                    readlink->target_length = target_length;
                    if (target_length < sizeof(readlink->target)) {
                        readlink->target[target_length] = '\0';
                    }
                }
            }
            filed_close_walk_handle(runtime, dir_handle, dir_owned);
        }
    }
    return filed_page_result(reply_status, target_length);
}

static filed_page_dispatch_result_t filed_dispatch_link_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_link_t *link = (filed_v2_link_t *)page;
    filed_vfs_io_decision_t old_parent;
    filed_vfs_io_decision_t new_parent;
    filed_vfs_open_result_t opened;
    storage_v2_statx_reply_t backend_stat;
    uint64_t old_object_id = 0;
    uint64_t linked_object_id = 0;
    int64_t reply_status = -22;

    if (filed_name_is_terminated(link->old_name, sizeof(link->old_name)) &&
        filed_name_is_terminated(link->new_name, sizeof(link->new_name)))
    {
        filed_handle_id_t old_dir_handle = 0;
        filed_handle_id_t new_dir_handle = 0;
        int old_dir_owned = 0;
        int new_dir_owned = 0;
        char old_name[FILED_V2_NAME_BYTES];
        char new_name[FILED_V2_NAME_BYTES];
        const filed_handle_id_t old_base_dir =
            link->old_dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)link->old_dir_handle;
        const filed_handle_id_t new_base_dir =
            link->new_dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)link->new_dir_handle;

        memset(old_name, 0, sizeof(old_name));
        memset(new_name, 0, sizeof(new_name));
        reply_status = filed_resolve_parent_path(
            runtime,
            old_base_dir,
            link->old_name,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT,
            &old_dir_handle,
            &old_dir_owned,
            old_name,
            sizeof(old_name));
        if (reply_status == 0) {
            reply_status = filed_resolve_parent_path(
                runtime,
                new_base_dir,
                link->new_name,
                FILED_RIGHT_CREATE,
                &new_dir_handle,
                &new_dir_owned,
                new_name,
                sizeof(new_name));
        }
        if (reply_status == 0) {
            filed_status_t status = filed_vfs_link_prepare(
                &runtime->vfs,
                old_dir_handle,
                new_dir_handle,
                old_name,
                new_name,
                &old_parent,
                &new_parent);
            reply_status = filed_status_to_wire(status);
        }
        if (reply_status == 0) {
            reply_status = filed_backend_lookup(
                runtime,
                old_parent.backend_object,
                old_name,
                &old_object_id);
        }
        if (reply_status == 0) {
            memset(&backend_stat, 0, sizeof(backend_stat));
            reply_status = filed_backend_statx(runtime, old_object_id, &backend_stat);
        }
        if (reply_status == 0) {
            reply_status = filed_backend_link(
                runtime,
                old_object_id,
                new_parent.backend_object,
                new_name,
                &linked_object_id);
        }
        if (reply_status == 0 && linked_object_id != 0) {
            filed_dir_cache_invalidate_dir(new_parent.backend_object);
            memset(&opened, 0, sizeof(opened));
            filed_status_t status = filed_vfs_link_commit(
                &runtime->vfs,
                new_dir_handle,
                linked_object_id,
                filed_kind_from_unix_type(backend_stat.kind),
                new_name,
                FILED_RIGHT_LOOKUP | FILED_RIGHT_READ | FILED_RIGHT_WRITE | FILED_RIGHT_STAT,
                (filed_kind_from_unix_type(backend_stat.kind) == FILED_VNODE_SYMLINK) ? FILED_OPEN_NOFOLLOW : 0,
                &opened);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK) {
                storage_v2_statx_reply_t linked_stat;
                memset(&linked_stat, 0, sizeof(linked_stat));
                if (filed_backend_statx(runtime, linked_object_id, &linked_stat) == 0) {
                    filed_vfs_stat_snapshot_t snapshot;
                    memset(&snapshot, 0, sizeof(snapshot));
                    snapshot.valid = true;
                    snapshot.handle_id = opened.handle_id;
                    snapshot.mode = linked_stat.mode;
                    snapshot.size = linked_stat.size;
                    snapshot.blocks = linked_stat.blocks;
                    snapshot.nlink = linked_stat.nlink;
                    snapshot.kind = linked_stat.kind;
                    snapshot.times_valid = true;
                    snapshot.atime_sec = linked_stat.atime_sec;
                    snapshot.atime_nsec = linked_stat.atime_nsec;
                    snapshot.mtime_sec = linked_stat.mtime_sec;
                    snapshot.mtime_nsec = linked_stat.mtime_nsec;
                    snapshot.ctime_sec = linked_stat.ctime_sec;
                    snapshot.ctime_nsec = linked_stat.ctime_nsec;
                    snapshot.object_generation = opened.object_generation;
                    snapshot.dir_generation = opened.dir_generation;
                    (void)filed_vfs_update_stat_snapshot(&runtime->vfs, linked_object_id, &snapshot);
                }
                filed_runtime_publish_backend_object_generation(runtime, linked_object_id);
                filed_runtime_publish_backend_object_generation(runtime, new_parent.backend_object);
                (void)filed_vfs_close_handle(&runtime->vfs, opened.handle_id);
            }
        }
        if (new_dir_handle != 0) {
            filed_close_walk_handle(runtime, new_dir_handle, new_dir_owned);
        }
        if (old_dir_handle != 0) {
            filed_close_walk_handle(runtime, old_dir_handle, old_dir_owned);
        }
    }

    return filed_page_result(reply_status, linked_object_id);
}

static filed_page_dispatch_result_t filed_dispatch_rmdir_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_rmdir_t *rmdir = (filed_v2_rmdir_t *)page;
    filed_vfs_io_decision_t decision;
    filed_status_t status = FILED_ERR_INVALID;
    int64_t reply_status = -22;
    uint64_t target_object = 0;
    if (rmdir->reserved0 == 0 &&
        filed_name_is_terminated(rmdir->name, sizeof(rmdir->name)))
    {
        filed_handle_id_t dir_handle = 0;
        int dir_owned = 0;
        char name[FILED_V2_NAME_BYTES];
        const filed_handle_id_t base_dir =
            rmdir->dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)rmdir->dir_handle;

        memset(name, 0, sizeof(name));
        reply_status = filed_resolve_parent_path(
            runtime,
            base_dir,
            rmdir->name,
            FILED_RIGHT_REMOVE,
            &dir_handle,
            &dir_owned,
            name,
            sizeof(name));
        if (reply_status == 0) {
            status = filed_vfs_unlink_prepare(&runtime->vfs, dir_handle, name, &decision);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK) {
                target_object = filed_lookup_cache_target_object(
                    runtime,
                    dir_handle,
                    decision.backend_object,
                    name);
                reply_status = filed_flush_mutated_object(runtime, target_object);
                if (reply_status == 0) {
                    reply_status = filed_backend_rmdir(
                        runtime,
                        decision.backend_object,
                        name);
                }
                if (reply_status == 0) {
                    filed_dir_cache_invalidate_dir(decision.backend_object);
                    filed_invalidate_mutated_object(runtime, target_object);
                    filed_vfs_reclaim_result_t reclaim;
                    memset(&reclaim, 0, sizeof(reclaim));
                    status = filed_vfs_unlink_commit_ex(&runtime->vfs, dir_handle, name, &reclaim);
                    if (status != FILED_OK) {
                        reply_status = filed_status_to_wire(status);
                    } else {
                        filed_runtime_publish_backend_object_generation(
                            runtime,
                            decision.backend_object);
                        if (target_object != 0) {
                            filed_runtime_publish_backend_object_generation(
                                runtime,
                                target_object);
                        }
                        const int release_status = filed_release_reclaimed_object(runtime, &reclaim);
                        if (release_status != 0) {
                            reply_status = release_status;
                        }
                    }
                }
            }
            filed_close_walk_handle(runtime, dir_handle, dir_owned);
        }
    }

    return filed_page_result(reply_status, 0);
}

static filed_page_dispatch_result_t filed_dispatch_rename_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_rename_t *rename = (filed_v2_rename_t *)page;
    filed_vfs_io_decision_t old_parent;
    filed_vfs_io_decision_t new_parent;
    filed_status_t status = FILED_ERR_INVALID;
    int64_t reply_status = -22;
    uint64_t object_id = 0;
    uint64_t replaced_object_id = 0;
    if (filed_name_is_terminated(rename->old_name, sizeof(rename->old_name)) &&
        filed_name_is_terminated(rename->new_name, sizeof(rename->new_name)))
    {
        filed_handle_id_t old_dir_handle = 0;
        filed_handle_id_t new_dir_handle = 0;
        int old_dir_owned = 0;
        int new_dir_owned = 0;
        char old_name[FILED_V2_NAME_BYTES];
        char new_name[FILED_V2_NAME_BYTES];
        const filed_handle_id_t old_base_dir =
            rename->old_dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)rename->old_dir_handle;
        const filed_handle_id_t new_base_dir =
            rename->new_dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)rename->new_dir_handle;

        memset(old_name, 0, sizeof(old_name));
        memset(new_name, 0, sizeof(new_name));
        reply_status = filed_resolve_parent_path(
            runtime,
            old_base_dir,
            rename->old_name,
            FILED_RIGHT_RENAME,
            &old_dir_handle,
            &old_dir_owned,
            old_name,
            sizeof(old_name));
        if (reply_status == 0) {
            reply_status = filed_resolve_parent_path(
                runtime,
                new_base_dir,
                rename->new_name,
                FILED_RIGHT_RENAME,
                &new_dir_handle,
                &new_dir_owned,
                new_name,
                sizeof(new_name));
        }
        if (reply_status == 0) {
            status = filed_vfs_rename_prepare(
                &runtime->vfs,
                old_dir_handle,
                new_dir_handle,
                old_name,
                new_name,
                &old_parent,
                &new_parent);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK) {
                replaced_object_id = filed_lookup_cache_target_object(
                    runtime,
                    new_dir_handle,
                    new_parent.backend_object,
                    new_name);
                object_id = filed_lookup_cache_target_object(
                    runtime,
                    old_dir_handle,
                    old_parent.backend_object,
                    old_name);
                if (replaced_object_id != 0 &&
                    replaced_object_id != object_id)
                {
                    reply_status = filed_flush_mutated_object(runtime, replaced_object_id);
                }
                if (reply_status == 0) {
                    reply_status = filed_backend_rename(
                        runtime,
                        old_parent.backend_object,
                        old_name,
                        new_parent.backend_object,
                        new_name,
                        &object_id);
                }
                if (reply_status == 0) {
                    filed_dir_cache_invalidate_dir(old_parent.backend_object);
                    if (new_parent.backend_object != old_parent.backend_object) {
                        filed_dir_cache_invalidate_dir(new_parent.backend_object);
                    }
                    if (replaced_object_id != 0 && replaced_object_id != object_id) {
                        filed_invalidate_mutated_object(runtime, replaced_object_id);
                    }
                    filed_vfs_reclaim_result_t reclaim;
                    memset(&reclaim, 0, sizeof(reclaim));
                    status = filed_vfs_rename_commit_ex(
                        &runtime->vfs,
                        old_dir_handle,
                        new_dir_handle,
                        old_name,
                        new_name,
                        object_id,
                        &reclaim);
                    if (status != FILED_OK) {
                        reply_status = filed_status_to_wire(status);
                    } else {
                        filed_runtime_publish_backend_object_generation(
                            runtime,
                            old_parent.backend_object);
                        if (new_parent.backend_object != old_parent.backend_object) {
                            filed_runtime_publish_backend_object_generation(
                                runtime,
                                new_parent.backend_object);
                        }
                        if (object_id != 0) {
                            filed_runtime_publish_backend_object_generation(
                                runtime,
                                object_id);
                        }
                        if (replaced_object_id != 0 && replaced_object_id != object_id) {
                            filed_runtime_publish_backend_object_generation(
                                runtime,
                                replaced_object_id);
                        }
                        const int release_status = filed_release_reclaimed_object(runtime, &reclaim);
                        if (release_status != 0) {
                            reply_status = release_status;
                        }
                    }
                }
            }
        }
        filed_close_walk_handle(runtime, old_dir_handle, old_dir_owned);
        filed_close_walk_handle(runtime, new_dir_handle, new_dir_owned);
    }

    return filed_page_result(reply_status, object_id);
}

static filed_page_dispatch_result_t filed_dispatch_getdents_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_getdents_t *out = (filed_v2_getdents_t *)page;
    storage_v2_getdents_request_t backend_entries;
    filed_vfs_io_decision_t decision;
    filed_status_t status = filed_vfs_getdents_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)out->dir_handle,
        &decision);
    bool splice_tmpfs = false;
    uint64_t backend_offset = 0;
    if (status == FILED_OK) {
        splice_tmpfs = filed_root_getdents_splices_tmpfs(runtime, decision.backend_object);
        backend_offset = filed_root_getdents_backend_offset(
            runtime,
            decision.backend_object,
            decision.offset);
    }
    int64_t reply_status = filed_status_to_wire(status);
    uint64_t result = 0;
    memset(&backend_entries, 0, sizeof(backend_entries));
    backend_entries.capacity = FILED_V2_DIRENT_CAPACITY;
    if (status == FILED_OK &&
        !filed_dir_cache_get(decision.backend_object, backend_offset, &backend_entries))
    {
        reply_status = filed_backend_getdents(
            runtime,
            decision.backend_object,
            backend_offset,
            &backend_entries);
        if (reply_status == 0) {
            filed_dir_cache_store(decision.backend_object, backend_offset, &backend_entries);
        }
    }
    if (reply_status == 0) {
        const uint64_t backend_start = splice_tmpfs && decision.offset == 0 ? 1u : 0u;
        const uint64_t backend_capacity = FILED_V2_DIRENT_CAPACITY - backend_start;
        const uint64_t backend_count =
            backend_entries.count > backend_capacity ?
                backend_capacity :
                backend_entries.count;
        const uint64_t count = backend_start + backend_count;
        const uint64_t dir_handle = out->dir_handle;
        memset(out, 0, sizeof(*out));
        out->dir_handle = dir_handle;
        out->offset = decision.offset;
        out->count = count;
        out->dir_generation = decision.dir_generation;
        if (backend_start != 0) {
            out->entries[0].handle = 0;
            out->entries[0].kind = 0040000u;
            out->entries[0].name_len = 3;
            snprintf(
                out->entries[0].name,
                sizeof(out->entries[0].name),
                "%s",
                "tmp");
        }
        for (uint64_t i = 0; i < backend_count; ++i) {
            const uint64_t out_index = backend_start + i;
            out->entries[out_index].handle = 0;
            out->entries[out_index].kind = backend_entries.entries[i].kind;
            out->entries[out_index].name_len = backend_entries.entries[i].name_len;
            snprintf(
                out->entries[out_index].name,
                sizeof(out->entries[out_index].name),
                "%s",
                backend_entries.entries[i].name);
        }
        result = count;
        status = filed_vfs_getdents_commit(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)dir_handle,
            count);
        if (status != FILED_OK) {
            reply_status = filed_status_to_wire(status);
        } else {
            filed_runtime_publish_generation(
                runtime,
                (filed_handle_id_t)(uint32_t)dir_handle,
                decision.object_generation,
                decision.dir_generation);
        }
    }
    return filed_page_result(reply_status, result);
}

static filed_page_dispatch_result_t filed_dispatch_close_page(
    filed_runtime_t *runtime,
    const struct pacha_ipc_msg *request)
{
    const filed_handle_id_t handle_id = (filed_handle_id_t)(uint32_t)request->word2;
    if (handle_id == runtime->root_handle_id) {
        return filed_page_result(-13, 0);
    }
    return filed_page_result(filed_close_handle_runtime(runtime, handle_id), 0);
}

static filed_page_dispatch_result_t filed_dispatch_dup_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_handle_flags_t *wire_flags = (filed_v2_handle_flags_t *)page;
    filed_handle_id_t dup_handle = 0;
    int64_t reply_status = -22;
    uint64_t result = 0;
    if (wire_flags->reserved0 == 0 &&
        filed_v2_flags_are_known(wire_flags->fd_flags, wire_flags->status_flags))
    {
        const filed_status_t status = filed_vfs_dup_handle(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)wire_flags->handle,
            filed_v2_fd_flags_to_vfs(wire_flags->fd_flags),
            &dup_handle);
        reply_status = filed_status_to_wire(status);
        if (status == FILED_OK) {
            wire_flags->handle = dup_handle;
            result = dup_handle;
            filed_vfs_io_decision_t decision;
            if (filed_vfs_stat_prepare(&runtime->vfs, dup_handle, &decision) == FILED_OK) {
                filed_runtime_publish_generation(
                    runtime,
                    dup_handle,
                    decision.object_generation,
                    decision.dir_generation);
            }
        }
    }
    return filed_page_result(reply_status, result);
}

static filed_page_dispatch_result_t filed_dispatch_get_flags_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_handle_flags_t *wire_flags = (filed_v2_handle_flags_t *)page;
    filed_vfs_handle_flags_t flags;
    const uint64_t handle = wire_flags->handle;
    uint64_t result = 0;
    filed_status_t status;
    memset(&flags, 0, sizeof(flags));
    status = filed_vfs_get_handle_flags(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)handle,
        &flags);
    if (status == FILED_OK) {
        memset(wire_flags, 0, sizeof(*wire_flags));
        wire_flags->handle = handle;
        wire_flags->fd_flags = filed_vfs_fd_flags_to_wire(flags.fd_flags);
        wire_flags->status_flags =
            filed_vfs_file_status_flags_to_wire(flags.status_flags);
        result = wire_flags->fd_flags;
    }
    return filed_page_result(filed_status_to_wire(status), result);
}

static filed_page_dispatch_result_t filed_dispatch_set_flags_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_handle_flags_t *wire_flags = (filed_v2_handle_flags_t *)page;
    filed_vfs_handle_flags_t flags;
    filed_status_t status = FILED_ERR_INVALID;
    if (wire_flags->reserved0 == 0 &&
        filed_v2_flags_are_known(wire_flags->fd_flags, wire_flags->status_flags))
    {
        flags.fd_flags = filed_v2_fd_flags_to_vfs(wire_flags->fd_flags);
        flags.status_flags =
            filed_v2_file_status_flags_to_vfs(wire_flags->status_flags);
        status = filed_vfs_set_handle_flags(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)wire_flags->handle,
            &flags);
    }
    return filed_page_result(filed_status_to_wire(status), 0);
}

static filed_page_dispatch_result_t filed_dispatch_exec_path_session_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_v2_exec_path_t *exec = (filed_v2_exec_path_t *)page;
    const uint64_t known_flags =
        FILED_V2_EXEC_INHERIT_HANDLES |
        FILED_V2_EXEC_LINUX_LPR |
        FILED_V2_EXEC_LINUX_BOOTSTRAP |
        FILED_V2_EXEC_LINUX_DEFAULT_STDIO |
        FILED_V2_EXEC_TRANSFER_PROCESS_FD;
    int64_t reply_status = -22;
    int process_fd = -1;
    int thread_fd = -1;
    int exec_filed_endpoint_fd = -1;
    int exec_netd_socket_endpoint_fd = -1;
    int exec_termd_tty_endpoint_fd = -1;
    int exec_lpr_bootstrap_fd = -1;
    int exec_filed_endpoint_borrowed = 0;
    int exec_netd_socket_endpoint_borrowed = 0;
    int exec_termd_tty_endpoint_borrowed = 0;
    filed_dispatch_saved_fd_t lpr_bootstrap_saved;
    filed_handle_id_t inherit_handles[FILED_V2_EXEC_MAX_INHERIT_HANDLES];
    filed_dispatch_saved_fd_init(&lpr_bootstrap_saved);
    memset(inherit_handles, 0, sizeof(inherit_handles));

    if ((exec->flags & ~known_flags) != 0 ||
        exec->inherit_fd_count != 0 ||
        exec->fd_patch_count != 0 ||
        exec->inherit_handle_count > FILED_V2_EXEC_MAX_INHERIT_HANDLES ||
        exec->argc > FILED_V2_EXEC_MAX_ARGS ||
        exec->envc > FILED_V2_EXEC_MAX_ENVS ||
        exec->string_bytes > FILED_V2_EXEC_STRING_BYTES ||
        !filed_name_is_terminated(exec->path, sizeof(exec->path)) ||
        (!filed_v2_exec_string_ref_empty(exec->cwd) &&
            !filed_v2_exec_string_ref_valid(exec, exec->cwd)) ||
        !filed_dispatch_exec_default_stdio_valid(exec) ||
        !filed_dispatch_exec_lpr_fd_table_valid(exec, NULL, 0))
    {
        goto out;
    }
    for (uint64_t i = 0; i < exec->argc; ++i) {
        if (!filed_v2_exec_string_ref_valid(exec, exec->argv[i])) {
            goto out;
        }
    }
    for (uint64_t i = 0; i < exec->envc; ++i) {
        if (!filed_v2_exec_string_ref_valid(exec, exec->envp[i])) {
            goto out;
        }
    }
    if ((exec->flags & FILED_V2_EXEC_INHERIT_HANDLES) == 0 &&
        exec->inherit_handle_count != 0)
    {
        goto out;
    }

    for (uint64_t i = 0; i < exec->inherit_handle_count; ++i) {
        filed_handle_id_t dup_handle = 0;
        const filed_status_t dup_status = filed_vfs_dup_handle_for_exec(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)exec->inherit_handles[i],
            &dup_handle);
        if (dup_status != FILED_OK) {
            reply_status = filed_status_to_wire(dup_status);
            goto out;
        }
        inherit_handles[i] = dup_handle;
    }

    if (runtime->client_endpoint_fd >= 16) {
        reply_status = filed_dispatch_prepare_endpoint_to_fixed(
            runtime->client_endpoint_fd,
            FILED_EXEC_FILED_ENDPOINT_FD,
            &exec_filed_endpoint_fd,
            &exec_filed_endpoint_borrowed);
        if (reply_status != 0) {
            goto out;
        }
    }
    if ((exec->flags & FILED_V2_EXEC_LINUX_LPR) != 0 &&
        runtime->netd_socket_endpoint_fd >= 16)
    {
        reply_status = filed_dispatch_prepare_endpoint_to_fixed(
            runtime->netd_socket_endpoint_fd,
            FILED_EXEC_NETD_SOCKET_ENDPOINT_FD,
            &exec_netd_socket_endpoint_fd,
            &exec_netd_socket_endpoint_borrowed);
        if (reply_status != 0) {
            goto out;
        }
    }
    if ((exec->flags & FILED_V2_EXEC_LINUX_LPR) != 0 &&
        runtime->termd_tty_endpoint_fd >= 16)
    {
        reply_status = filed_dispatch_prepare_endpoint_to_fixed(
            runtime->termd_tty_endpoint_fd,
            FILED_EXEC_TERMD_TTY_ENDPOINT_FD,
            &exec_termd_tty_endpoint_fd,
            &exec_termd_tty_endpoint_borrowed);
        if (reply_status != 0) {
            goto out;
        }
    }
    if ((exec->flags & (FILED_V2_EXEC_LINUX_LPR | FILED_V2_EXEC_LINUX_BOOTSTRAP)) ==
        (FILED_V2_EXEC_LINUX_LPR | FILED_V2_EXEC_LINUX_BOOTSTRAP))
    {
        const int bootstrap_source_fd = filed_dispatch_create_lpr_bootstrap_fd(exec, NULL);
        if (bootstrap_source_fd < 16) {
            reply_status = bootstrap_source_fd < 0 ? bootstrap_source_fd : -12;
            goto out;
        }
        reply_status = filed_dispatch_prepare_inherit_fd_to_target(
            bootstrap_source_fd,
            FILED_EXEC_LPR_BOOTSTRAP_FD,
            &exec_lpr_bootstrap_fd,
            &lpr_bootstrap_saved);
        if (reply_status != 0) {
            (void)pacha_fd_close(bootstrap_source_fd);
            goto out;
        }
    }

    filed_v2_openat_t openat;
    memset(&openat, 0, sizeof(openat));
    openat.dir_handle = exec->dir_handle;
    openat.rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_EXEC |
        FILED_V2_RIGHT_STAT;
    snprintf(openat.name, sizeof(openat.name), "%s", exec->path);

    filed_vfs_open_result_t open_result;
    memset(&open_result, 0, sizeof(open_result));
    reply_status = filed_openat_path(runtime, &openat, &open_result);
    if (reply_status != 0) {
        goto out;
    }

    reply_status = filed_exec_handle(
        runtime,
        open_result.handle_id,
        exec,
        NULL,
        0,
        -1,
        &process_fd,
        &thread_fd);
    filed_close_walk_handle(runtime, open_result.handle_id, 1);

out:
    filed_dispatch_close_prepared_endpoint(&exec_filed_endpoint_fd, exec_filed_endpoint_borrowed);
    filed_dispatch_close_prepared_endpoint(&exec_netd_socket_endpoint_fd, exec_netd_socket_endpoint_borrowed);
    filed_dispatch_close_prepared_endpoint(&exec_termd_tty_endpoint_fd, exec_termd_tty_endpoint_borrowed);
    if (exec_lpr_bootstrap_fd >= 0) {
        if (lpr_bootstrap_saved.fd >= 0) {
            filed_dispatch_restore_target_fd(exec_lpr_bootstrap_fd, &lpr_bootstrap_saved);
        } else {
            filed_dispatch_close_owned_fd(&exec_lpr_bootstrap_fd);
        }
    } else if (lpr_bootstrap_saved.fd >= 0) {
        (void)pacha_fd_close(lpr_bootstrap_saved.fd);
        filed_dispatch_saved_fd_init(&lpr_bootstrap_saved);
    }
    if (reply_status != 0) {
        for (uint64_t i = 0; i < FILED_V2_EXEC_MAX_INHERIT_HANDLES; ++i) {
            if (inherit_handles[i] != 0) {
                (void)filed_vfs_close_handle(&runtime->vfs, inherit_handles[i]);
            }
        }
    }
    if (process_fd >= 16) {
        (void)pacha_fd_close(process_fd);
    }
    if (thread_fd >= 16) {
        (void)pacha_fd_close(thread_fd);
    }
    return filed_page_result(reply_status, 0);
}

static int filed_dispatch_exec_path(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    pacha_service_request_header_t v2_header;
    memset(&v2_header, 0, sizeof(v2_header));
    v2_header.magic = PACHA_SERVICE_REQUEST_MAGIC;
    v2_header.abi_version = PACHA_SERVICE_ABI_VERSION;
    v2_header.service_id = FILED_V2_SERVICE_ID;
    v2_header.op = FILED_V2_OP_EXEC_PATH;
    v2_header.request_id = request->word3;
    v2_header.trace_id = request->word3;
    v2_header.payload_size = sizeof(filed_v2_exec_path_t);

    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_V2_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply_v2(reply_fd, NULL, &v2_header, -22, 0, 0);
    }

    filed_v2_exec_path_t *exec = (filed_v2_exec_path_t *)page;
    const uint64_t known_flags =
        FILED_V2_EXEC_BOOTSTRAP_FD |
        FILED_V2_EXEC_INHERIT_FDS |
        FILED_V2_EXEC_PATCH_BOOTSTRAP_FDS |
        FILED_V2_EXEC_INHERIT_HANDLES |
        FILED_V2_EXEC_LINUX_LPR |
        FILED_V2_EXEC_LINUX_BOOTSTRAP |
        FILED_V2_EXEC_LINUX_DEFAULT_STDIO |
        FILED_V2_EXEC_TRANSFER_PROCESS_FD;
    const uint64_t exec_flags = exec->flags;
    int64_t reply_status = -22;
    int process_fd = -1;
    int thread_fd = -1;
    int bootstrap_fd = -1;
    int exec_filed_endpoint_fd = -1;
    int exec_netd_socket_endpoint_fd = -1;
    int exec_termd_tty_endpoint_fd = -1;
    int exec_lpr_bootstrap_fd = -1;
    int exec_filed_endpoint_borrowed = 0;
    int exec_netd_socket_endpoint_borrowed = 0;
    int exec_termd_tty_endpoint_borrowed = 0;
    int inherit_fds[FILED_V2_EXEC_MAX_INHERIT_FDS];
    filed_dispatch_saved_fd_t inherit_saved[FILED_V2_EXEC_MAX_INHERIT_FDS];
    filed_dispatch_saved_fd_t lpr_bootstrap_saved;
    filed_handle_id_t inherit_handles[FILED_V2_EXEC_MAX_INHERIT_HANDLES];
    memset(inherit_fds, 0xff, sizeof(inherit_fds));
    for (uint64_t i = 0; i < FILED_V2_EXEC_MAX_INHERIT_FDS; ++i) {
        filed_dispatch_saved_fd_init(&inherit_saved[i]);
    }
    filed_dispatch_saved_fd_init(&lpr_bootstrap_saved);
    memset(inherit_handles, 0, sizeof(inherit_handles));

    if ((exec_flags & ~known_flags) != 0 ||
        exec->inherit_fd_count > FILED_V2_EXEC_MAX_INHERIT_FDS ||
        exec->inherit_handle_count > FILED_V2_EXEC_MAX_INHERIT_HANDLES ||
        exec->fd_patch_count > FILED_V2_EXEC_MAX_FD_PATCHES ||
        exec->argc > FILED_V2_EXEC_MAX_ARGS ||
        exec->envc > FILED_V2_EXEC_MAX_ENVS ||
        exec->string_bytes > FILED_V2_EXEC_STRING_BYTES ||
        !filed_name_is_terminated(exec->path, sizeof(exec->path)) ||
        (!filed_v2_exec_string_ref_empty(exec->cwd) &&
            !filed_v2_exec_string_ref_valid(exec, exec->cwd)) ||
        !filed_dispatch_exec_default_stdio_valid(exec) ||
        !filed_dispatch_exec_lpr_fd_table_valid(exec, NULL, 0))
    {
        fprintf(stderr,
            "[filed] exec_path invalid path=%.*s flags=0x%llx inherit_fds=%llu inherit_handles=%llu fd_patches=%llu argc=%llu envc=%llu string_bytes=%llu\n",
            (int)sizeof(exec->path),
            exec->path,
            (unsigned long long)exec_flags,
            (unsigned long long)exec->inherit_fd_count,
            (unsigned long long)exec->inherit_handle_count,
            (unsigned long long)exec->fd_patch_count,
            (unsigned long long)exec->argc,
            (unsigned long long)exec->envc,
            (unsigned long long)exec->string_bytes);
        goto out;
    }
    for (uint64_t i = 0; i < exec->argc; ++i) {
        if (!filed_v2_exec_string_ref_valid(exec, exec->argv[i])) {
            goto out;
        }
    }
    for (uint64_t i = 0; i < exec->envc; ++i) {
        if (!filed_v2_exec_string_ref_valid(exec, exec->envp[i])) {
            goto out;
        }
    }
    const uint64_t inherit_fd_count = exec->inherit_fd_count;
    const uint64_t inherit_handle_count = exec->inherit_handle_count;

    const uint64_t has_bootstrap =
        (exec_flags & FILED_V2_EXEC_BOOTSTRAP_FD) != 0 ? 1u : 0u;
    if ((exec_flags & FILED_V2_EXEC_INHERIT_FDS) == 0 && inherit_fd_count != 0) {
        goto out;
    }
    if ((exec_flags & FILED_V2_EXEC_PATCH_BOOTSTRAP_FDS) == 0 && exec->fd_patch_count != 0) {
        goto out;
    }
    if ((exec_flags & FILED_V2_EXEC_PATCH_BOOTSTRAP_FDS) != 0 && has_bootstrap == 0) {
        goto out;
    }
    if ((exec_flags & FILED_V2_EXEC_INHERIT_HANDLES) == 0 && inherit_handle_count != 0) {
        goto out;
    }

    const uint64_t expected_fd_count = 1u + inherit_fd_count + has_bootstrap + 1u;
    if (request->fd_count != expected_fd_count || request->fds == NULL) {
        fprintf(stderr,
            "[filed] exec_path fd_count invalid path=%s got=%llu expected=%llu inherit=%llu bootstrap=%llu\n",
            exec->path,
            (unsigned long long)request->fd_count,
            (unsigned long long)expected_fd_count,
            (unsigned long long)inherit_fd_count,
            (unsigned long long)has_bootstrap);
        goto out;
    }

    for (uint64_t i = 0; i < inherit_fd_count; ++i) {
        const uint64_t fd_index = 1u + i;
        if (request->fds[fd_index].fd >= FILED_EXEC_MAX_FDS) {
            goto out;
        }
        reply_status = filed_dispatch_prepare_inherit_fd_to_target(
            (int)request->fds[fd_index].fd,
            exec->inherit_fd_targets[i],
            &inherit_fds[i],
            &inherit_saved[i]);
        if (reply_status != 0) {
            goto out;
        }
    }

    if ((exec_flags & FILED_V2_EXEC_BOOTSTRAP_FD) != 0) {
        const uint64_t fd_index = 1u + inherit_fd_count;
        if (request->fds[fd_index].fd < 16) {
            goto out;
        }
        bootstrap_fd = (int)request->fds[fd_index].fd;
    }

    for (uint64_t i = 0; i < inherit_handle_count; ++i) {
        filed_handle_id_t dup_handle = 0;
        const filed_status_t dup_status = filed_vfs_dup_handle_for_exec(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)exec->inherit_handles[i],
            &dup_handle);
        if (dup_status != FILED_OK) {
            reply_status = filed_status_to_wire(dup_status);
            goto out;
        }
        inherit_handles[i] = dup_handle;
    }

    if (runtime->client_endpoint_fd >= 16)
    {
        reply_status = filed_dispatch_prepare_endpoint_to_fixed(
            runtime->client_endpoint_fd,
            FILED_EXEC_FILED_ENDPOINT_FD,
            &exec_filed_endpoint_fd,
            &exec_filed_endpoint_borrowed);
        if (reply_status != 0) {
            goto out;
        }
    }
    if ((exec_flags & FILED_V2_EXEC_LINUX_LPR) != 0 &&
        runtime->netd_socket_endpoint_fd >= 16)
    {
        reply_status = filed_dispatch_prepare_endpoint_to_fixed(
            runtime->netd_socket_endpoint_fd,
            FILED_EXEC_NETD_SOCKET_ENDPOINT_FD,
            &exec_netd_socket_endpoint_fd,
            &exec_netd_socket_endpoint_borrowed);
        if (reply_status != 0) {
            goto out;
        }
    }
    if ((exec_flags & FILED_V2_EXEC_LINUX_LPR) != 0 &&
        runtime->termd_tty_endpoint_fd >= 16)
    {
        reply_status = filed_dispatch_prepare_endpoint_to_fixed(
            runtime->termd_tty_endpoint_fd,
            FILED_EXEC_TERMD_TTY_ENDPOINT_FD,
            &exec_termd_tty_endpoint_fd,
            &exec_termd_tty_endpoint_borrowed);
        if (reply_status != 0) {
            goto out;
        }
    }
    if ((exec_flags & (FILED_V2_EXEC_LINUX_LPR | FILED_V2_EXEC_LINUX_BOOTSTRAP)) ==
        (FILED_V2_EXEC_LINUX_LPR | FILED_V2_EXEC_LINUX_BOOTSTRAP))
    {
        const int bootstrap_source_fd = filed_dispatch_create_lpr_bootstrap_fd(exec, NULL);
        if (bootstrap_source_fd < 16) {
            reply_status = bootstrap_source_fd < 0 ? bootstrap_source_fd : -12;
            goto out;
        }
        reply_status = filed_dispatch_prepare_inherit_fd_to_target(
            bootstrap_source_fd,
            FILED_EXEC_LPR_BOOTSTRAP_FD,
            &exec_lpr_bootstrap_fd,
            &lpr_bootstrap_saved);
        if (reply_status != 0) {
            (void)pacha_fd_close(bootstrap_source_fd);
            goto out;
        }
    }

    if ((exec_flags & FILED_V2_EXEC_PATCH_BOOTSTRAP_FDS) != 0) {
        void *bootstrap_page = pacha_mmap(
            bootstrap_fd,
            FILED_BOOTSTRAP_PATCH_BYTES,
            PACHA_PROT_READ | PACHA_PROT_WRITE,
            PACHA_MMAP_SHARED,
            0);
        if (bootstrap_page == NULL) {
            goto out;
        }
        reply_status = 0;
        for (uint64_t i = 0; i < exec->fd_patch_count; ++i) {
            const filed_v2_exec_fd_patch_t *patch = &exec->fd_patches[i];
            uint64_t value = 0;
            if (patch->reserved0 != 0 || patch->offset > FILED_BOOTSTRAP_PATCH_BYTES - 8u) {
                reply_status = -22;
                break;
            }
            if (patch->kind == FILED_V2_EXEC_PATCH_INHERIT_FD) {
                if (patch->index >= inherit_fd_count) {
                    reply_status = -22;
                    break;
                }
                value = (uint64_t)(uint32_t)inherit_fds[patch->index];
            } else if (patch->kind == FILED_V2_EXEC_PATCH_BOOTSTRAP_FD) {
                if (patch->index != 0) {
                    reply_status = -22;
                    break;
                }
                value = (uint64_t)(uint32_t)bootstrap_fd;
            } else if (patch->kind == FILED_V2_EXEC_PATCH_INHERIT_HANDLE) {
                if (patch->index >= inherit_handle_count) {
                    reply_status = -22;
                    break;
                }
                value = (uint64_t)(uint32_t)inherit_handles[patch->index];
            } else {
                reply_status = -22;
                break;
            }
            filed_write_u64_le(bootstrap_page, patch->offset, value);
            reply_status = 0;
        }
        (void)pacha_munmap(bootstrap_page, FILED_BOOTSTRAP_PATCH_BYTES);
        if (reply_status != 0) {
            goto out;
        }
    }

    filed_v2_openat_t openat;
    memset(&openat, 0, sizeof(openat));
    openat.dir_handle = exec->dir_handle;
    openat.rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_EXEC |
        FILED_V2_RIGHT_STAT;
    snprintf(openat.name, sizeof(openat.name), "%s", exec->path);

    filed_vfs_open_result_t open_result;
    memset(&open_result, 0, sizeof(open_result));
    reply_status = filed_openat_path(runtime, &openat, &open_result);
    if (reply_status != 0) {
        fprintf(stderr,
            "[filed] exec_path open failed path=%s status=%lld\n",
            exec->path,
            (long long)reply_status);
        goto out;
    }

    reply_status = filed_exec_handle(
        runtime,
        open_result.handle_id,
        exec,
        inherit_fds,
        inherit_fd_count,
        bootstrap_fd,
        &process_fd,
        &thread_fd);
    filed_close_walk_handle(runtime, open_result.handle_id, 1);
    if (reply_status != 0) {
        fprintf(stderr,
            "[filed] exec_path exec failed path=%s status=%lld\n",
            exec->path,
            (long long)reply_status);
        goto out;
    }

out:
    filed_dispatch_close_prepared_endpoint(&exec_filed_endpoint_fd, exec_filed_endpoint_borrowed);
    filed_dispatch_close_prepared_endpoint(&exec_netd_socket_endpoint_fd, exec_netd_socket_endpoint_borrowed);
    filed_dispatch_close_prepared_endpoint(&exec_termd_tty_endpoint_fd, exec_termd_tty_endpoint_borrowed);
    if (exec_lpr_bootstrap_fd >= 0) {
        if (lpr_bootstrap_saved.fd >= 0) {
            filed_dispatch_restore_target_fd(exec_lpr_bootstrap_fd, &lpr_bootstrap_saved);
        } else {
            filed_dispatch_close_owned_fd(&exec_lpr_bootstrap_fd);
        }
    } else if (lpr_bootstrap_saved.fd >= 0) {
        (void)pacha_fd_close(lpr_bootstrap_saved.fd);
        filed_dispatch_saved_fd_init(&lpr_bootstrap_saved);
    }
    uint64_t v2_error_token = 0;
    const uint64_t reply_result = reply_status == 0 ?
        (uint64_t)(uint32_t)process_fd :
        filed_error_token(
            reply_status,
            FILED_V2_OP_EXEC_PATH,
            PACHA_ERRCONV_STAGE_STATUS_MAP,
            reply_status,
            request->word3,
            0,
            0,
            0,
            "filed exec_path negative reply");
    if (reply_status != 0) {
        v2_error_token = reply_result;
    }
    pacha_service_reply_header_init(
        (pacha_service_reply_header_t *)page,
        &v2_header,
        reply_status,
        PACHA_SERVICE_ERROR_FILED_EXEC,
        reply_result,
        0);
    (void)pacha_munmap(page, FILED_V2_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    if (bootstrap_fd >= 16) {
        (void)pacha_fd_close(bootstrap_fd);
    }
    for (uint64_t i = 0; i < FILED_V2_EXEC_MAX_INHERIT_FDS; ++i) {
        if (inherit_fds[i] >= 0) {
            if (inherit_saved[i].fd >= 0) {
                filed_dispatch_restore_target_fd(inherit_fds[i], &inherit_saved[i]);
            } else {
                filed_dispatch_close_owned_fd(&inherit_fds[i]);
            }
        } else if (inherit_saved[i].fd >= 0) {
            (void)pacha_fd_close(inherit_saved[i].fd);
            filed_dispatch_saved_fd_init(&inherit_saved[i]);
        }
    }
    if (reply_status != 0) {
        for (uint64_t i = 0; i < FILED_V2_EXEC_MAX_INHERIT_HANDLES; ++i) {
            if (inherit_handles[i] != 0) {
                (void)filed_vfs_close_handle(&runtime->vfs, inherit_handles[i]);
            }
        }
    }
    if (reply_status == 0 && process_fd >= 16 && thread_fd >= 16) {
        const int send_status = filed_send_exec_reply_v2(
            reply_fd,
            request->word3,
            process_fd,
            thread_fd,
            (exec_flags & FILED_V2_EXEC_TRANSFER_PROCESS_FD) != 0);
        if (send_status != 0) {
            for (uint64_t i = 0; i < FILED_V2_EXEC_MAX_INHERIT_HANDLES; ++i) {
                if (inherit_handles[i] != 0) {
                    (void)filed_vfs_close_handle(&runtime->vfs, inherit_handles[i]);
                }
            }
        }
        return send_status;
    }
    return filed_send_reply_v2(reply_fd, NULL, &v2_header, reply_status, 0, v2_error_token);
}

static int filed_dispatch_exec_self(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    pacha_service_request_header_t v2_header;
    memset(&v2_header, 0, sizeof(v2_header));
    v2_header.magic = PACHA_SERVICE_REQUEST_MAGIC;
    v2_header.abi_version = PACHA_SERVICE_ABI_VERSION;
    v2_header.service_id = FILED_V2_SERVICE_ID;
    v2_header.op = FILED_V2_OP_EXEC_SELF;
    v2_header.request_id = request->word3;
    v2_header.trace_id = request->word3;
    v2_header.payload_size = sizeof(filed_v2_exec_path_t);

    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_V2_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply_v2(reply_fd, NULL, &v2_header, -22, 0, 0);
    }

    filed_v2_exec_path_t *exec = (filed_v2_exec_path_t *)page;
    const uint64_t known_flags =
        FILED_V2_EXEC_LINUX_LPR |
        FILED_V2_EXEC_LINUX_BOOTSTRAP |
        FILED_V2_EXEC_SELF |
        FILED_V2_EXEC_LPR_FD_TABLE;
    int64_t reply_status = -22;
    int process_fd = -1;
    int thread_fd = -1;
    int bootstrap_fd = -1;
    int lpr_fd_table_fd = -1;
    void *lpr_fd_table_page = NULL;
    const filed_v2_exec_lpr_fd_table_t *lpr_fd_table = NULL;
    const int wants_lpr_fd_table = (exec->flags & FILED_V2_EXEC_LPR_FD_TABLE) != 0;
    const uint64_t expected_request_fd_count = wants_lpr_fd_table ? 3u : 2u;

    if ((exec->flags & ~known_flags) != 0 ||
        (exec->flags & (FILED_V2_EXEC_LINUX_LPR | FILED_V2_EXEC_LINUX_BOOTSTRAP | FILED_V2_EXEC_SELF)) !=
            (FILED_V2_EXEC_LINUX_LPR | FILED_V2_EXEC_LINUX_BOOTSTRAP | FILED_V2_EXEC_SELF) ||
        exec->inherit_fd_count != 0 ||
        exec->inherit_handle_count != 0 ||
        exec->fd_patch_count != 0 ||
        exec->argc > FILED_V2_EXEC_MAX_ARGS ||
        exec->envc > FILED_V2_EXEC_MAX_ENVS ||
        exec->string_bytes > FILED_V2_EXEC_STRING_BYTES ||
        request->fd_count != expected_request_fd_count ||
        request->fds == NULL ||
        !filed_name_is_terminated(exec->path, sizeof(exec->path)) ||
        (!filed_v2_exec_string_ref_empty(exec->cwd) &&
            !filed_v2_exec_string_ref_valid(exec, exec->cwd)) ||
        !filed_v2_exec_string_ref_empty(exec->ctty) ||
        (wants_lpr_fd_table &&
            (exec->lpr_fd_table_bytes < sizeof(filed_v2_exec_lpr_fd_table_t) ||
                request->fds[1].fd < 16)) ||
        (!wants_lpr_fd_table &&
            !filed_dispatch_exec_lpr_fd_table_valid(exec, NULL, 1)))
    {
        goto out;
    }
    for (uint64_t i = 0; i < exec->argc; ++i) {
        if (!filed_v2_exec_string_ref_valid(exec, exec->argv[i])) {
            goto out;
        }
    }
    for (uint64_t i = 0; i < exec->envc; ++i) {
        if (!filed_v2_exec_string_ref_valid(exec, exec->envp[i])) {
            goto out;
        }
    }

    if (wants_lpr_fd_table) {
        lpr_fd_table_fd = (int)request->fds[1].fd;
        lpr_fd_table_page = pacha_mmap(
            lpr_fd_table_fd,
            exec->lpr_fd_table_bytes,
            PACHA_PROT_READ,
            PACHA_MMAP_SHARED,
            0);
        if (lpr_fd_table_page == NULL) {
            goto out;
        }
        lpr_fd_table = (const filed_v2_exec_lpr_fd_table_t *)lpr_fd_table_page;
        if (!filed_dispatch_exec_lpr_fd_table_valid(exec, lpr_fd_table, 1)) {
            goto out;
        }
    }

    bootstrap_fd = filed_dispatch_create_lpr_bootstrap_fd(exec, lpr_fd_table);
    if (bootstrap_fd < 16) {
        reply_status = bootstrap_fd < 0 ? bootstrap_fd : -12;
        bootstrap_fd = -1;
        goto out;
    }

    filed_v2_openat_t openat;
    memset(&openat, 0, sizeof(openat));
    openat.dir_handle = exec->dir_handle;
    openat.rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_EXEC |
        FILED_V2_RIGHT_STAT;
    snprintf(openat.name, sizeof(openat.name), "%s", exec->path);

    filed_vfs_open_result_t open_result;
    memset(&open_result, 0, sizeof(open_result));
    reply_status = filed_openat_path(runtime, &openat, &open_result);
    if (reply_status != 0) {
        fprintf(stderr,
            "[filed] exec_self open failed path=%s status=%lld\n",
            exec->path,
            (long long)reply_status);
        goto out;
    }

    reply_status = filed_exec_linux_lpr_prepare_self(
        runtime,
        open_result.handle_id,
        exec,
        &process_fd,
        &thread_fd);
    filed_close_walk_handle(runtime, open_result.handle_id, 1);
    if (reply_status != 0) {
        fprintf(stderr,
            "[filed] exec_self prepare failed path=%s status=%lld\n",
            exec->path,
            (long long)reply_status);
        goto out;
    }

out:
    if (lpr_fd_table_page != NULL) {
        (void)pacha_munmap(lpr_fd_table_page, exec->lpr_fd_table_bytes);
    }
    if (lpr_fd_table_fd >= 16) {
        (void)pacha_fd_close(lpr_fd_table_fd);
    }
    (void)pacha_munmap(page, FILED_V2_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    if (reply_status == 0 && process_fd >= 16 && thread_fd >= 16 && bootstrap_fd >= 16) {
        return filed_send_exec_self_reply_v2(reply_fd, request->word3, process_fd, thread_fd, bootstrap_fd);
    }
    if (process_fd >= 16) {
        (void)pacha_syscall2(
            PACHA_PROCESS_SYSCALL_KILL,
            (uint64_t)(uint32_t)process_fd,
            1);
        (void)pacha_fd_close(process_fd);
    }
    if (thread_fd >= 16) {
        (void)pacha_fd_close(thread_fd);
    }
    if (bootstrap_fd >= 16) {
        (void)pacha_fd_close(bootstrap_fd);
    }
    const struct pacha_ipc_msg reply = {
        .word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = (uint64_t)reply_status,
        .word2 = 0,
        .word3 = request->word3,
    };
    const int send_status = pacha_ipc_reply(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    return send_status;
}

static int filed_dispatch_session_open_v2(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request,
    void *reply_page,
    const pacha_service_request_header_t *header,
    int *out_keep_fd,
    int *out_keep_fd2)
{
    if (out_keep_fd != NULL) {
        *out_keep_fd = -1;
    }
    if (out_keep_fd2 != NULL) {
        *out_keep_fd2 = -1;
    }
    if (runtime == NULL ||
        request == NULL ||
        reply_page == NULL ||
        header == NULL ||
        request->fds == NULL ||
        request->fd_count < 4 ||
        request->fds[1].fd < 16 ||
        request->fds[2].fd < 16)
    {
        return filed_send_reply_v2(reply_fd, reply_page, header, -22, 0, 0);
    }

    int64_t reply_status = -24;
    uint64_t result = 0;
    const int channel_fd = (int)request->fds[1].fd;
    const int page_fd = (int)request->fds[2].fd;
    void *session_page = pacha_mmap(
        page_fd,
        FILED_V2_SESSION_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (session_page == NULL) {
        return filed_send_reply_v2(reply_fd, reply_page, header, -14, 0, 0);
    }

    filed_v2_fast_header_t *fast = (filed_v2_fast_header_t *)session_page;
    if (fast->magic != FILED_V2_FAST_MAGIC ||
        fast->version != FILED_V2_FAST_VERSION ||
        fast->request_capacity != FILED_V2_FAST_REQUEST_CAPACITY ||
        fast->completion_capacity != FILED_V2_FAST_COMPLETION_CAPACITY ||
        fast->payload_slot_count != FILED_V2_FAST_PAYLOAD_SLOT_COUNT ||
        fast->payload_slot_size != FILED_V2_PAGE_BYTES ||
        fast->payload_offset != FILED_V2_FAST_PAYLOAD_OFFSET ||
        fast->generation_offset != FILED_V2_FAST_GENERATION_OFFSET ||
        fast->generation_capacity != FILED_V2_FAST_GENERATION_CAPACITY ||
        fast->payload_offset + fast->payload_slot_count * fast->payload_slot_size > FILED_V2_SESSION_PAGE_BYTES)
    {
        (void)pacha_munmap(session_page, FILED_V2_SESSION_PAGE_BYTES);
        return filed_send_reply_v2(reply_fd, reply_page, header, -71, 0, 0);
    }

    for (uint64_t i = 0; i < FILED_RUNTIME_MAX_SESSIONS; ++i) {
        filed_session_t *session = &runtime->sessions[i];
        if (session->active) {
            continue;
        }
        session->channel_fd = channel_fd;
        session->page_fd = page_fd;
        session->page = session_page;
        session->page_size = FILED_V2_SESSION_PAGE_BYTES;
        session->active = 1;
        if (out_keep_fd != NULL) {
            *out_keep_fd = channel_fd;
        }
        if (out_keep_fd2 != NULL) {
            *out_keep_fd2 = page_fd;
        }
        reply_status = 0;
        result = i + 1u;
        break;
    }

    if (reply_status != 0) {
        (void)pacha_munmap(session_page, FILED_V2_SESSION_PAGE_BYTES);
    }
    return filed_send_reply_v2(reply_fd, reply_page, header, reply_status, result, 0);
}

static uint64_t filed_import_termd_error(
    filed_runtime_t *runtime,
    uint64_t child_token,
    uint64_t request_id,
    int64_t status,
    uint64_t fd_count,
    uint64_t subject,
    const char *text)
{
    pacha_errconv_frame_t parent;
    memset(&parent, 0, sizeof(parent));
    parent.domain = PACHA_ERRCONV_DOMAIN_TERMD_STATUS;
    parent.component = PACHA_ERRCONV_COMPONENT_FILED;
    parent.op = FILED_V2_OP_SERVICE_REGISTER_TERMD_SIGNAL_SUPERVISOR;
    parent.stage = PACHA_ERRCONV_STAGE_CHILD_STATUS;
    parent.status = status;
    parent.raw_status = status;
    parent.request_id = request_id;
    parent.fd_count = fd_count;
    parent.subject = subject;
    parent.child_token = child_token;
    pacha_errconv_frame_text(&parent, text);

    if (runtime == NULL || runtime->termd_tty_endpoint_fd < 16 || child_token == 0) {
        return pacha_errconv_begin(filed_errors(), status, &parent);
    }

    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int page_fd = pacha_vmo_create(TERMD_V2_PAGE_BYTES, rights, 0);
    if (page_fd < 16) {
        const uint64_t token = pacha_errconv_begin(filed_errors(), status, &parent);
        pacha_errconv_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.domain = PACHA_ERRCONV_DOMAIN_KERNEL_STATUS;
        frame.component = PACHA_ERRCONV_COMPONENT_FILED;
        frame.op = TERMD_V2_OP_DIAG_ERROR_GET;
        frame.stage = PACHA_ERRCONV_STAGE_KERNEL_SYSCALL;
        frame.status = page_fd;
        frame.raw_status = page_fd;
        frame.request_id = request_id;
        frame.child_token = child_token;
        pacha_errconv_frame_text(&frame, "create child error page failed");
        (void)pacha_errconv_append(filed_errors(), token, &frame);
        return token;
    }

    void *page = pacha_mmap(
        page_fd,
        TERMD_V2_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (page == NULL) {
        const uint64_t token = pacha_errconv_begin(filed_errors(), status, &parent);
        pacha_errconv_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.domain = PACHA_ERRCONV_DOMAIN_KERNEL_STATUS;
        frame.component = PACHA_ERRCONV_COMPONENT_FILED;
        frame.op = TERMD_V2_OP_DIAG_ERROR_GET;
        frame.stage = PACHA_ERRCONV_STAGE_MAP_PAGE;
        frame.status = -5;
        frame.raw_status = -5;
        frame.request_id = request_id;
        frame.child_token = child_token;
        pacha_errconv_frame_text(&frame, "map child error page failed");
        (void)pacha_errconv_append(filed_errors(), token, &frame);
        (void)pacha_fd_close(page_fd);
        return token;
    }
    memset(page, 0, TERMD_V2_PAGE_BYTES);
    const uint64_t get_request_id = request_id ^ 0x45524745545f5445ull;
    pacha_service_request_header_t *header = (pacha_service_request_header_t *)page;
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = TERMD_V2_SERVICE_ID;
    header->op = TERMD_V2_OP_DIAG_ERROR_GET;
    header->flags = PACHA_SERVICE_FLAG_PAGE_PAYLOAD | PACHA_SERVICE_FLAG_DIAGNOSTIC;
    header->request_id = get_request_id;
    header->trace_id = request_id;
    header->payload_size = sizeof(termd_v2_handle_request_t);
    termd_v2_handle_request_t *payload =
        (termd_v2_handle_request_t *)((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES);
    payload->arg0 = child_token;

    struct pacha_ipc_fd fd_item;
    memset(&fd_item, 0, sizeof(fd_item));
    fd_item.fd = (uint64_t)(uint32_t)page_fd;
    fd_item.rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const struct pacha_ipc_msg get_request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = 0,
        .word2 = 0,
        .word3 = get_request_id,
        .fds = &fd_item,
        .fd_count = 1,
    };
    const int error_reply_fd = pacha_ipc_call(runtime->termd_tty_endpoint_fd, &get_request);
    int64_t get_status = error_reply_fd;
    if (error_reply_fd >= 16) {
        struct pacha_ipc_msg get_reply;
        memset(&get_reply, 0, sizeof(get_reply));
        get_status = pacha_ipc_recv_wait(error_reply_fd, &get_reply, PACHA_FD_WAIT_FOREVER);
        (void)pacha_fd_close(error_reply_fd);
        const pacha_service_reply_header_t *reply_header =
            (const pacha_service_reply_header_t *)page;
        if (get_status == 0 &&
            (get_reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
             get_reply.word3 != get_request.word3 ||
             reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
             reply_header->service_id != TERMD_V2_SERVICE_ID ||
             reply_header->op != TERMD_V2_OP_DIAG_ERROR_GET ||
             reply_header->request_id != get_request.word3))
        {
            get_status = -5;
        } else if (get_status == 0) {
            get_status = reply_header->status;
        }
    }

    uint64_t token = 0;
    if (get_status == 0) {
        token = pacha_errconv_import_page(
            filed_errors(),
            status,
            &parent,
            (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES,
            TERMD_V2_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES);
    } else {
        token = pacha_errconv_begin(filed_errors(), status, &parent);
        pacha_errconv_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.domain = PACHA_ERRCONV_DOMAIN_TERMD_STATUS;
        frame.component = PACHA_ERRCONV_COMPONENT_FILED;
        frame.op = TERMD_V2_OP_DIAG_ERROR_GET;
        frame.stage = PACHA_ERRCONV_STAGE_ERROR_GET;
        frame.status = get_status;
        frame.raw_status = get_status;
        frame.request_id = get_request.word3;
        frame.child_token = child_token;
        pacha_errconv_frame_text(&frame, "child error get failed");
        (void)pacha_errconv_append(filed_errors(), token, &frame);
    }

    (void)pacha_munmap(page, TERMD_V2_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return token;
}

static int filed_dispatch_register_termd_signal_supervisor_v2(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request,
    void *reply_page,
    const pacha_service_request_header_t *header)
{
    if (runtime == NULL ||
        request == NULL ||
        request->fds == NULL ||
        request->fd_count < 3 ||
        request->fds[1].fd < 16 ||
        runtime->termd_tty_endpoint_fd < 16 ||
        header == NULL ||
        header->payload_size < sizeof(filed_v2_service_endpoint_request_t))
    {
        return filed_send_reply_v2(reply_fd, reply_page, header, -22, 0, 0);
    }

    const int supervisor_endpoint_fd = (int)(uint32_t)request->fds[1].fd;
    const uint64_t page_rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int page_fd = pacha_vmo_create(TERMD_V2_PAGE_BYTES, page_rights, 0);
    if (page_fd < 16) {
        const uint64_t token = filed_error_token(
            page_fd,
            header->op,
            PACHA_ERRCONV_STAGE_KERNEL_SYSCALL,
            page_fd,
            header->request_id,
            request->fd_count,
            0,
            0,
            "termd signal supervisor page create failed");
        return filed_send_reply_v2(reply_fd, reply_page, header, page_fd, 0, token);
    }
    void *page = pacha_mmap(
        page_fd,
        TERMD_V2_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (page == NULL) {
        const uint64_t token = filed_error_token(
            -5,
            header->op,
            PACHA_ERRCONV_STAGE_MAP_PAGE,
            -5,
            header->request_id,
            request->fd_count,
            (uint64_t)(uint32_t)page_fd,
            0,
            "termd signal supervisor page map failed");
        (void)pacha_fd_close(page_fd);
        return filed_send_reply_v2(reply_fd, reply_page, header, -5, 0, token);
    }

    memset(page, 0, TERMD_V2_PAGE_BYTES);
    pacha_service_request_header_t *termd_header = (pacha_service_request_header_t *)page;
    termd_header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    termd_header->abi_version = PACHA_SERVICE_ABI_VERSION;
    termd_header->service_id = TERMD_V2_SERVICE_ID;
    termd_header->op = TERMD_V2_OP_SIGNAL_REGISTER_SUPERVISOR;
    termd_header->request_id = header->request_id;
    termd_header->trace_id = header->trace_id != 0 ? header->trace_id : header->request_id;
    termd_header->fd_count = 1;

    struct pacha_ipc_fd termd_fds[2];
    memset(termd_fds, 0, sizeof(termd_fds));
    termd_fds[0].fd = (uint64_t)(uint32_t)page_fd;
    termd_fds[0].rights = page_rights;
    termd_fds[1].fd = (uint64_t)(uint32_t)supervisor_endpoint_fd;
    termd_fds[1].rights =
        PACHA_FD_RIGHT_CALL |
        PACHA_FD_RIGHT_CLOSE;

    const struct pacha_ipc_msg termd_request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = 0,
        .word2 = 0,
        .word3 = header->request_id,
        .fds = termd_fds,
        .fd_count = 2,
    };
    const int termd_reply_fd =
        pacha_ipc_call(runtime->termd_tty_endpoint_fd, &termd_request);
    if (termd_reply_fd < 16) {
        const uint64_t token = filed_error_token(
            termd_reply_fd,
            header->op,
            PACHA_ERRCONV_STAGE_CHILD_RPC_CALL,
            termd_reply_fd,
            header->request_id,
            request->fd_count,
            (uint64_t)(uint32_t)runtime->termd_tty_endpoint_fd,
            0,
            "termd signal supervisor call failed");
        (void)pacha_munmap(page, TERMD_V2_PAGE_BYTES);
        (void)pacha_fd_close(page_fd);
        return filed_send_reply_v2(reply_fd, reply_page, header, termd_reply_fd, 0, token);
    }

    struct pacha_ipc_msg termd_reply;
    memset(&termd_reply, 0, sizeof(termd_reply));
    const int recv_status =
        pacha_ipc_recv_wait(termd_reply_fd, &termd_reply, PACHA_FD_WAIT_FOREVER);
    (void)pacha_fd_close(termd_reply_fd);
    const pacha_service_reply_header_t *reply_header =
        (const pacha_service_reply_header_t *)page;

    int64_t status = recv_status;
    uint64_t result = 0;
    uint64_t error_token = 0;
    if (recv_status == 0) {
        if (termd_reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
            termd_reply.word3 != termd_request.word3 ||
            reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
            reply_header->service_id != TERMD_V2_SERVICE_ID ||
            reply_header->op != TERMD_V2_OP_SIGNAL_REGISTER_SUPERVISOR ||
            reply_header->request_id != termd_request.word3)
        {
            status = -5;
            error_token = filed_error_token(
                status,
                header->op,
                PACHA_ERRCONV_STAGE_REPLY_MAGIC,
                (int64_t)termd_reply.word0,
                header->request_id,
                request->fd_count,
                termd_reply.word3,
                0,
                "termd signal supervisor reply mismatch");
        } else {
            status = reply_header->status;
            result = reply_header->result;
            if (status < 0) {
                error_token = filed_import_termd_error(
                    runtime,
                    reply_header->result,
                    header->request_id,
                    status,
                    request->fd_count,
                    TERMD_V2_OP_SIGNAL_REGISTER_SUPERVISOR,
                    "termd signal supervisor returned error");
            }
        }
    } else {
        error_token = filed_error_token(
            status,
            header->op,
            PACHA_ERRCONV_STAGE_CHILD_RPC_RECV,
            status,
            header->request_id,
            request->fd_count,
            (uint64_t)(uint32_t)termd_reply_fd,
            0,
            "termd signal supervisor reply recv failed");
    }

    (void)pacha_munmap(page, TERMD_V2_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return filed_send_reply_v2(reply_fd, reply_page, header, status, result, error_token);
}

static filed_page_dispatch_result_t filed_dispatch_session_page(
    filed_runtime_t *runtime,
    const struct pacha_ipc_msg *request,
    void *page)
{
    switch (request->word1) {
    case FILED_V2_OP_DIAG_PING:
        return filed_page_result(0, request->word2);
    case FILED_V2_OP_VFS_OPENAT:
        return filed_dispatch_openat_page(runtime, page);
    case FILED_V2_OP_VFS_VALIDATE_OPEN_CACHE:
        return filed_dispatch_validate_open_cache_page(runtime, page);
    case FILED_V2_OP_VFS_STAT:
        return filed_dispatch_stat_page(runtime, page);
    case FILED_V2_OP_VFS_UTIMENS:
        return filed_dispatch_utimens_page(runtime, page);
    case FILED_V2_OP_VFS_CHMOD:
        return filed_dispatch_chmod_page(runtime, page);
    case FILED_V2_OP_VFS_PREAD:
        return filed_dispatch_pread_page(runtime, page);
    case FILED_V2_OP_VFS_READ:
        return filed_dispatch_read_page(runtime, page);
    case FILED_V2_OP_VFS_PREAD_TO_VMO:
        return filed_page_result(-95, 0);
    case FILED_V2_OP_VFS_PWRITE:
        return filed_dispatch_pwrite_page(runtime, page);
    case FILED_V2_OP_VFS_WRITE:
        return filed_dispatch_write_page(runtime, page);
    case FILED_V2_OP_VFS_TRUNCATE:
        return filed_dispatch_truncate_page(runtime, page);
    case FILED_V2_OP_VFS_UNLINK:
        return filed_dispatch_unlink_page(runtime, page);
    case FILED_V2_OP_VFS_RENAME:
        return filed_dispatch_rename_page(runtime, page);
    case FILED_V2_OP_VFS_MKDIR:
        return filed_dispatch_mkdir_page(runtime, page);
    case FILED_V2_OP_VFS_RMDIR:
        return filed_dispatch_rmdir_page(runtime, page);
    case FILED_V2_OP_VFS_SYMLINK:
        return filed_dispatch_symlink_page(runtime, page);
    case FILED_V2_OP_VFS_READLINK:
        return filed_dispatch_readlink_page(runtime, page);
    case FILED_V2_OP_VFS_LINK:
        return filed_dispatch_link_page(runtime, page);
    case FILED_V2_OP_VFS_GETDENTS:
        return filed_dispatch_getdents_page(runtime, page);
    case FILED_V2_OP_VFS_DUP:
        return filed_dispatch_dup_page(runtime, page);
    case FILED_V2_OP_VFS_GET_FLAGS:
        return filed_dispatch_get_flags_page(runtime, page);
    case FILED_V2_OP_VFS_SET_FLAGS:
        return filed_dispatch_set_flags_page(runtime, page);
    case FILED_V2_OP_VFS_FSYNC:
        return filed_dispatch_fsync_page(runtime, request);
    case FILED_V2_OP_VFS_SEEK:
        return filed_dispatch_seek_page(runtime, page);
    case FILED_V2_OP_EXEC_PATH:
        return filed_dispatch_exec_path_session_page(runtime, page);
    case FILED_V2_OP_EXEC_SELF:
        return filed_page_result(-95, 0);
    case FILED_V2_OP_VFS_CLOSE:
        return filed_dispatch_close_page(runtime, request);
    case FILED_V2_OP_VFS_SYNC_ALL:
        return filed_page_result(filed_dispatch_sync_all(runtime), 0);
    case FILED_V2_OP_SERVICE_SET_NETD_SOCKET:
    case FILED_V2_OP_SERVICE_SET_TERMD_TTY:
        return filed_page_result(-95, 0);
    default:
        return filed_page_result(-95, 0);
    }
}

static int filed_session_fast_validate(
    const filed_session_t *session,
    filed_v2_fast_header_t **out_header,
    filed_v2_fast_request_t **out_requests,
    filed_v2_fast_completion_t **out_completions)
{
    if (session == NULL ||
        session->page == NULL ||
        session->page_size < FILED_V2_SESSION_PAGE_BYTES ||
        out_header == NULL ||
        out_requests == NULL ||
        out_completions == NULL)
    {
        return -22;
    }

    filed_v2_fast_header_t *header = (filed_v2_fast_header_t *)session->page;
    if (header->magic != FILED_V2_FAST_MAGIC ||
        header->version != FILED_V2_FAST_VERSION ||
        header->request_capacity != FILED_V2_FAST_REQUEST_CAPACITY ||
        header->completion_capacity != FILED_V2_FAST_COMPLETION_CAPACITY ||
        header->payload_slot_count != FILED_V2_FAST_PAYLOAD_SLOT_COUNT ||
        header->payload_slot_size != FILED_V2_PAGE_BYTES ||
        header->payload_offset != FILED_V2_FAST_PAYLOAD_OFFSET ||
        header->generation_offset != FILED_V2_FAST_GENERATION_OFFSET ||
        header->generation_capacity != FILED_V2_FAST_GENERATION_CAPACITY ||
        header->payload_offset + header->payload_slot_count * header->payload_slot_size > session->page_size)
    {
        return -71;
    }

    *out_header = header;
    *out_requests = (filed_v2_fast_request_t *)((uint8_t *)session->page + sizeof(*header));
    *out_completions = (filed_v2_fast_completion_t *)((uint8_t *)(*out_requests) +
        sizeof(**out_requests) * header->request_capacity);
    return 0;
}

static void *filed_session_fast_payload(
    const filed_session_t *session,
    const filed_v2_fast_header_t *header,
    uint64_t payload_slot)
{
    if (session == NULL ||
        header == NULL ||
        payload_slot >= header->payload_slot_count ||
        header->payload_slot_size != FILED_V2_PAGE_BYTES)
    {
        return NULL;
    }
    const uint64_t offset = header->payload_offset + payload_slot * header->payload_slot_size;
    if (offset + FILED_V2_PAGE_BYTES > session->page_size) {
        return NULL;
    }
    return (uint8_t *)session->page + offset;
}

static filed_page_dispatch_result_t filed_dispatch_session_write_batch(
    filed_runtime_t *runtime,
    filed_session_t *session,
    filed_v2_fast_header_t *header,
    uint64_t batch_count,
    bool append)
{
    if (batch_count == 0 || batch_count > header->payload_slot_count) {
        return filed_page_result(-22, 0);
    }

    uint64_t total = 0;
    int64_t status = 0;
    const uint64_t op = append ? FILED_V2_OP_VFS_WRITE : FILED_V2_OP_VFS_PWRITE;
    for (uint64_t slot = 0; slot < batch_count; slot++) {
        void *payload = filed_session_fast_payload(session, header, slot);
        if (payload == NULL) {
            status = -22;
            break;
        }

        filed_v2_io_t *io = (filed_v2_io_t *)payload;
        const uint64_t requested = io->length;
        const uint64_t start_cycles = filed_read_tsc();
        const filed_page_dispatch_result_t result = append ?
            filed_dispatch_write_page(runtime, payload) :
            filed_dispatch_pwrite_page(runtime, payload);
        filed_record_dispatch_metric_cycles(
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
    filed_v2_fast_header_t *header = NULL;
    filed_v2_fast_request_t *requests = NULL;
    filed_v2_fast_completion_t *completions = NULL;
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

        filed_v2_fast_request_t *fast_request =
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
            if (fast_request->opcode == FILED_V2_OP_VFS_PWRITE_BATCH ||
                fast_request->opcode == FILED_V2_OP_VFS_WRITE_BATCH)
            {
                result = filed_dispatch_session_write_batch(
                    runtime,
                    session,
                    header,
                    fast_request->word2,
                    fast_request->opcode == FILED_V2_OP_VFS_WRITE_BATCH);
                filed_record_dispatch_metric_cycles(
                    pseudo_request.word1,
                    start_cycles,
                    filed_read_tsc(),
                    result.status);
            } else {
                result = filed_dispatch_session_page(runtime, &pseudo_request, payload);
                filed_record_dispatch_metric_cycles(
                    pseudo_request.word1,
                    start_cycles,
                    filed_read_tsc(),
                    result.status);
            }
        }

        filed_v2_fast_completion_t *completion =
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
    const int request_is_v2 =
        request.word0 == PACHA_SERVICE_REQUEST_MAGIC &&
        request.word1 == FILED_V2_OP_SESSION_DOORBELL &&
        request.word3 != 0;
    if (!request_is_v2) {
        return filed_send_session_reply_v2(session->channel_fd, request.word3, -22, 0);
    }

    int drain_status = 0;
    const uint64_t drain_start_ns = filed_now_ns();
    const uint64_t drain_start_cycles = filed_read_tsc();
    const uint64_t completed = filed_dispatch_session_fast_drain(runtime, session, &drain_status);
    const uint64_t drain_end_cycles = filed_read_tsc();
    const uint64_t drain_end_ns = filed_now_ns();
    const uint64_t reply_start_ns = filed_now_ns();
    const uint64_t reply_start_cycles = filed_read_tsc();
    const int reply_status = filed_send_session_reply_v2(
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

typedef struct filed_v2_route_result {
    int replied;
    int reply_status;
    int keep_fd;
    int64_t status;
    uint64_t result;
} filed_v2_route_result_t;

static filed_v2_route_result_t filed_v2_route_pending(void)
{
    filed_v2_route_result_t result;
    memset(&result, 0, sizeof(result));
    result.keep_fd = -1;
    return result;
}

static filed_v2_route_result_t filed_v2_route_replied(int reply_status)
{
    filed_v2_route_result_t result = filed_v2_route_pending();
    result.replied = 1;
    result.reply_status = reply_status;
    return result;
}

static filed_v2_route_result_t filed_dispatch_client_vfs_v2(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request,
    void *page,
    const pacha_service_request_header_t *header)
{
    filed_v2_route_result_t route = filed_v2_route_pending();
    void *payload = (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES;

    switch (header->op) {
    case FILED_V2_OP_VFS_OPENAT:
        if (header->payload_size < sizeof(filed_v2_path_request_t)) {
            route.status = -22;
        } else {
            filed_v2_path_request_t *path = (filed_v2_path_request_t *)payload;
            filed_v2_openat_t openat;
            memset(&openat, 0, sizeof(openat));
            openat.dir_handle = path->dir_handle;
            openat.rights = path->rights;
            openat.open_flags = path->flags;
            memcpy(openat.name, path->path, sizeof(openat.name));
            const filed_page_dispatch_result_t open_result =
                filed_dispatch_openat_page(runtime, &openat);
            route.status = open_result.status;
            route.result = open_result.result;
        }
        break;
    case FILED_V2_OP_VFS_CLOSE:
        if (header->payload_size < sizeof(filed_v2_handle_request_t)) {
            route.status = -22;
        } else {
            const filed_v2_handle_request_t *handle = (const filed_v2_handle_request_t *)payload;
            const filed_handle_id_t handle_id = (filed_handle_id_t)(uint32_t)handle->handle;
            route.status = handle_id == runtime->root_handle_id ?
                -13 :
                filed_close_handle_runtime(runtime, handle_id);
        }
        break;
    case FILED_V2_OP_VFS_STAT:
        if (header->payload_size < sizeof(filed_v2_statx_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_stat_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_READ:
        if (header->payload_size < sizeof(filed_v2_io_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_read_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_PREAD:
        if (header->payload_size < sizeof(filed_v2_io_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_pread_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_WRITE:
        if (header->payload_size < sizeof(filed_v2_io_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_write_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_PWRITE:
        if (header->payload_size < sizeof(filed_v2_io_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_pwrite_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_GETDENTS:
        if (header->payload_size < sizeof(filed_v2_getdents_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_getdents_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_SEEK:
        if (header->payload_size < sizeof(filed_v2_seek_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_seek_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_DUP:
        if (header->payload_size < sizeof(filed_v2_handle_flags_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_dup_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_GET_FLAGS:
        if (header->payload_size < sizeof(filed_v2_handle_flags_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_get_flags_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_SET_FLAGS:
        if (header->payload_size < sizeof(filed_v2_handle_flags_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_set_flags_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_FSYNC:
        if (header->payload_size < sizeof(filed_v2_handle_request_t)) {
            route.status = -22;
        } else {
            const filed_v2_handle_request_t *handle = (const filed_v2_handle_request_t *)payload;
            struct pacha_ipc_msg pseudo_request;
            memset(&pseudo_request, 0, sizeof(pseudo_request));
            pseudo_request.word2 = handle->handle;
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_fsync_page(runtime, &pseudo_request);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_TRUNCATE:
        if (header->payload_size < sizeof(filed_v2_truncate_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_truncate_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_UNLINK:
        if (header->payload_size < sizeof(filed_v2_unlink_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_unlink_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_RENAME:
        if (header->payload_size < sizeof(filed_v2_rename_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_rename_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_MKDIR:
        if (header->payload_size < sizeof(filed_v2_mkdir_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_mkdir_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_RMDIR:
        if (header->payload_size < sizeof(filed_v2_rmdir_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_rmdir_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_SYMLINK:
        if (header->payload_size < sizeof(filed_v2_symlink_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_symlink_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_READLINK:
        if (header->payload_size < sizeof(filed_v2_readlink_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_readlink_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_LINK:
        if (header->payload_size < sizeof(filed_v2_link_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_link_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_UTIMENS:
        if (header->payload_size < sizeof(filed_v2_utimens_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_utimens_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_CHMOD:
        if (header->payload_size < sizeof(filed_v2_chmod_t)) {
            route.status = -22;
        } else {
            const filed_page_dispatch_result_t page_result =
                filed_dispatch_chmod_page(runtime, payload);
            route.status = page_result.status;
            route.result = page_result.result;
        }
        break;
    case FILED_V2_OP_VFS_FILE_VMO: {
        const int reply_status = filed_dispatch_file_vmo_v2(
            runtime,
            reply_fd,
            request,
            page,
            header);
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
        filed_close_received_fds_except(request, reply_fd, -1);
        return filed_v2_route_replied(reply_status);
    }
    case FILED_V2_OP_VFS_PREAD_TO_VMO:
        if (header->payload_size < sizeof(filed_v2_pread_vmo_t) ||
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
    case FILED_V2_OP_VFS_SYNC_ALL:
        route.status = filed_dispatch_sync_all(runtime);
        break;
    default:
        route.status = -95;
        break;
    }
    return route;
}

static int filed_dispatch_client_v2(
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
        pacha_service_request_header_t header;
        memset(&header, 0, sizeof(header));
        header.magic = PACHA_SERVICE_REQUEST_MAGIC;
        header.abi_version = PACHA_SERVICE_ABI_VERSION;
        header.service_id = FILED_V2_SERVICE_ID;
        header.request_id = request != NULL ? request->word3 : 0;
        header.trace_id = request != NULL ? request->word3 : 0;
        filed_close_received_fds_except(request, reply_fd, -1);
        return filed_send_reply_v2(reply_fd, NULL, &header, -22, 0, 0);
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
            PACHA_ERRCONV_STAGE_MAP_PAGE,
            -5,
            request->word3,
            request->fd_count,
            (uint64_t)(uint32_t)page_fd,
            0,
            "filed v2 request page map failed");
        pacha_service_request_header_t header;
        memset(&header, 0, sizeof(header));
        header.magic = PACHA_SERVICE_REQUEST_MAGIC;
        header.abi_version = PACHA_SERVICE_ABI_VERSION;
        header.service_id = FILED_V2_SERVICE_ID;
        header.request_id = request->word3;
        header.trace_id = request->word3;
        return filed_send_reply_v2(reply_fd, NULL, &header, -5, 0, token);
    }

    pacha_service_request_header_t header;
    memcpy(&header, page, sizeof(header));
    if (!pacha_service_request_header_is_v2(&header, FILED_V2_SERVICE_ID) ||
        header.request_id == 0 ||
        header.request_id != request->word3)
    {
        pacha_service_reply_header_init(
            (pacha_service_reply_header_t *)page,
            &header,
            -22,
            PACHA_SERVICE_ERROR_ABI,
            0,
            0);
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
        filed_close_received_fds_except(request, reply_fd, -1);
        return filed_send_reply_v2(reply_fd, NULL, &header, -22, 0, 0);
    }

    int keep_fd = -1;
    int64_t status = 0;
    uint64_t result = 0;
    uint64_t error_token = 0;
    switch (header.op) {
    case FILED_V2_OP_HELLO:
        result = PACHA_SERVICE_ABI_VERSION;
        break;
    case FILED_V2_OP_SESSION_OPEN: {
        int keep_session_channel_fd = -1;
        int keep_session_page_fd = -1;
        const int reply_status = filed_dispatch_session_open_v2(
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
    case FILED_V2_OP_DIAG_ERROR_GET:
        if (header.payload_size < sizeof(filed_v2_diag_request_t)) {
            status = -22;
        } else {
            filed_v2_diag_request_t *diag =
                (filed_v2_diag_request_t *)((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES);
            status = pacha_errconv_export(
                filed_errors(),
                diag->subject,
                diag,
                PACHA_SERVICE_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES);
            if (status == 0) {
                const int reply_status = filed_send_reply_v2_payload(
                    reply_fd,
                    page,
                    &header,
                    status,
                    0,
                    0,
                    PACHA_SERVICE_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES);
                (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
                filed_close_received_fds_except(request, reply_fd, -1);
                return reply_status;
            }
        }
        break;
    case FILED_V2_OP_VFS_OPENAT:
    case FILED_V2_OP_VFS_CLOSE:
    case FILED_V2_OP_VFS_STAT:
    case FILED_V2_OP_VFS_READ:
    case FILED_V2_OP_VFS_PREAD:
    case FILED_V2_OP_VFS_WRITE:
    case FILED_V2_OP_VFS_PWRITE:
    case FILED_V2_OP_VFS_GETDENTS:
    case FILED_V2_OP_VFS_SEEK:
    case FILED_V2_OP_VFS_DUP:
    case FILED_V2_OP_VFS_GET_FLAGS:
    case FILED_V2_OP_VFS_SET_FLAGS:
    case FILED_V2_OP_VFS_FSYNC:
    case FILED_V2_OP_VFS_TRUNCATE:
    case FILED_V2_OP_VFS_UNLINK:
    case FILED_V2_OP_VFS_RENAME:
    case FILED_V2_OP_VFS_MKDIR:
    case FILED_V2_OP_VFS_RMDIR:
    case FILED_V2_OP_VFS_SYMLINK:
    case FILED_V2_OP_VFS_READLINK:
    case FILED_V2_OP_VFS_LINK:
    case FILED_V2_OP_VFS_UTIMENS:
    case FILED_V2_OP_VFS_CHMOD:
    case FILED_V2_OP_VFS_FILE_VMO:
    case FILED_V2_OP_VFS_PREAD_TO_VMO:
    case FILED_V2_OP_VFS_SYNC_ALL: {
        const filed_v2_route_result_t route = filed_dispatch_client_vfs_v2(
            runtime,
            reply_fd,
            request,
            page,
            &header);
        if (route.replied) {
            return route.reply_status;
        }
        keep_fd = route.keep_fd;
        status = route.status;
        result = route.result;
        break;
    }
    case FILED_V2_OP_EXEC_PATH:
        if (header.payload_size < sizeof(filed_v2_exec_path_t)) {
            status = -22;
        } else {
            memmove(page, (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES, sizeof(filed_v2_exec_path_t));
            struct pacha_ipc_msg exec_request = *request;
            exec_request.word0 = PACHA_SERVICE_REQUEST_MAGIC;
            exec_request.word1 = FILED_V2_OP_EXEC_PATH;
            exec_request.word2 = 0;
            exec_request.word3 = header.request_id;
            (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
            return filed_dispatch_exec_path(runtime, reply_fd, &exec_request);
        }
        break;
    case FILED_V2_OP_EXEC_SELF:
        if (header.payload_size < sizeof(filed_v2_exec_path_t)) {
            status = -22;
        } else {
            memmove(page, (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES, sizeof(filed_v2_exec_path_t));
            struct pacha_ipc_msg exec_request = *request;
            exec_request.word0 = PACHA_SERVICE_REQUEST_MAGIC;
            exec_request.word1 = FILED_V2_OP_EXEC_SELF;
            exec_request.word2 = 0;
            exec_request.word3 = header.request_id;
            (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
            return filed_dispatch_exec_self(runtime, reply_fd, &exec_request);
        }
        break;
    case FILED_V2_OP_DIAG_DUMP_METRICS:
        filed_dump_dispatch_metrics();
        filed_dump_cache_metrics(runtime);
        filed_exec_linux_lpr_dump_metrics();
        filed_kobox_backend_dump_metrics(&runtime->backend);
        break;
    case FILED_V2_OP_DIAG_SET_CACHE_SLOTS: {
        const filed_v2_diag_request_t *diag =
            (const filed_v2_diag_request_t *)((const uint8_t *)page + PACHA_SERVICE_HEADER_BYTES);
        if (header.payload_size < sizeof(*diag)) {
            status = -22;
        } else {
            status = filed_page_cache_flush_object(runtime, 0);
            if (status == 0) {
                filed_page_cache_configure(diag->subject);
                result = filed_page_cache.active_slots;
            }
        }
        break;
    }
    case FILED_V2_OP_SERVICE_SET_NETD_SOCKET:
    case FILED_V2_OP_SERVICE_SET_TERMD_TTY:
        if (header.payload_size < sizeof(filed_v2_service_endpoint_request_t) ||
            request->fd_count < 3 ||
            request->fds[1].fd < 16)
        {
            status = -22;
        } else {
            const int endpoint_fd = (int)(uint32_t)request->fds[1].fd;
            if (header.op == FILED_V2_OP_SERVICE_SET_NETD_SOCKET) {
                if (runtime->netd_socket_endpoint_fd >= 16) {
                    (void)pacha_fd_close(runtime->netd_socket_endpoint_fd);
                }
                runtime->netd_socket_endpoint_fd = endpoint_fd;
            } else {
                if (runtime->termd_tty_endpoint_fd >= 16) {
                    (void)pacha_fd_close(runtime->termd_tty_endpoint_fd);
                }
                runtime->termd_tty_endpoint_fd = endpoint_fd;
            }
            keep_fd = endpoint_fd;
            result = (uint64_t)(uint32_t)endpoint_fd;
        }
        break;
    case FILED_V2_OP_SERVICE_REGISTER_TERMD_SIGNAL_SUPERVISOR: {
        const int reply_status = filed_dispatch_register_termd_signal_supervisor_v2(
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
            PACHA_ERRCONV_STAGE_DISPATCH_ENTRY,
            status,
            header.request_id,
            request->fd_count,
            0,
            0,
            "filed v2 dispatch failed");
    }
    filed_close_received_fds_except(request, reply_fd, keep_fd);
    const int reply_status =
        filed_send_reply_v2(reply_fd, page, &header, status, result, error_token);
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
        pacha_service_request_header_t header;
        memset(&header, 0, sizeof(header));
        header.magic = PACHA_SERVICE_REQUEST_MAGIC;
        header.abi_version = PACHA_SERVICE_ABI_VERSION;
        header.service_id = FILED_V2_SERVICE_ID;
        header.request_id = request.word3;
        header.trace_id = request.word3;
        return filed_send_reply_v2(reply_fd, NULL, &header, -22, 0, 0);
    }
    return filed_dispatch_client_v2(runtime, reply_fd, &request);
}
