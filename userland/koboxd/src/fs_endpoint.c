#include "fs_endpoint.h"

#include "ipc_wire.h"
#include "koboxd/control_protocol.h"
#include "koboxd/storage_protocol.h"
#include "pacha/abi.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
    KOBOXD_FS_METRIC_OP_MAX = 32,
};

typedef struct koboxd_fs_metric {
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t total_cycles;
    uint64_t max_cycles;
    uint64_t errors;
} koboxd_fs_metric_t;

static koboxd_fs_metric_t koboxd_fs_metrics[KOBOXD_FS_METRIC_OP_MAX];

static uint64_t koboxd_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t koboxd_read_tsc(void)
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

static const char *koboxd_fs_op_name(uint64_t op)
{
    switch (op) {
    case STORAGE_OP_MOUNT_ROOT: return "mount_root";
    case STORAGE_OP_LOOKUP: return "lookup";
    case STORAGE_OP_PREAD: return "pread";
    case STORAGE_OP_PWRITE: return "pwrite";
    case STORAGE_OP_STATX: return "statx";
    case STORAGE_OP_GETDENTS: return "getdents";
    case STORAGE_OP_FSYNC: return "fsync";
    case STORAGE_OP_UTIMENS: return "utimens";
    case STORAGE_OP_CHMOD: return "chmod";
    case STORAGE_OP_CREATE: return "create";
    case STORAGE_OP_TRUNCATE: return "truncate";
    case STORAGE_OP_UNLINK: return "unlink";
    case STORAGE_OP_RENAME: return "rename";
    case STORAGE_OP_MKDIR: return "mkdir";
    case STORAGE_OP_MKNOD: return "mknod";
    case STORAGE_OP_RMDIR: return "rmdir";
    case STORAGE_OP_RELEASE_OBJECT: return "release_object";
    case STORAGE_OP_SYNC_ALL: return "sync_all";
    default: return "unknown";
    }
}

static uint64_t koboxd_fs_metric_slot(uint64_t op)
{
    switch (op) {
    case STORAGE_OP_MOUNT_ROOT: return 1;
    case STORAGE_OP_LOOKUP: return 2;
    case STORAGE_OP_STATX: return 3;
    case STORAGE_OP_GETDENTS: return 4;
    case STORAGE_OP_PREAD: return 5;
    case STORAGE_OP_PWRITE: return 6;
    case STORAGE_OP_FSYNC: return 7;
    case STORAGE_OP_CREATE: return 8;
    case STORAGE_OP_TRUNCATE: return 9;
    case STORAGE_OP_UTIMENS: return 10;
    case STORAGE_OP_CHMOD: return 11;
    case STORAGE_OP_UNLINK: return 12;
    case STORAGE_OP_RENAME: return 13;
    case STORAGE_OP_MKDIR: return 14;
    case STORAGE_OP_RMDIR: return 15;
    case STORAGE_OP_RELEASE_OBJECT: return 16;
    case STORAGE_OP_SYNC_ALL: return 17;
    default: return 0;
    }
}

static uint64_t koboxd_fs_metric_op(uint64_t slot)
{
    switch (slot) {
    case 1: return STORAGE_OP_MOUNT_ROOT;
    case 2: return STORAGE_OP_LOOKUP;
    case 3: return STORAGE_OP_STATX;
    case 4: return STORAGE_OP_GETDENTS;
    case 5: return STORAGE_OP_PREAD;
    case 6: return STORAGE_OP_PWRITE;
    case 7: return STORAGE_OP_FSYNC;
    case 8: return STORAGE_OP_CREATE;
    case 9: return STORAGE_OP_TRUNCATE;
    case 10: return STORAGE_OP_UTIMENS;
    case 11: return STORAGE_OP_CHMOD;
    case 12: return STORAGE_OP_UNLINK;
    case 13: return STORAGE_OP_RENAME;
    case 14: return STORAGE_OP_MKDIR;
    case 15: return STORAGE_OP_RMDIR;
    case 16: return STORAGE_OP_RELEASE_OBJECT;
    case 17: return STORAGE_OP_SYNC_ALL;
    default: return 0;
    }
}

static void koboxd_record_fs_metric(
    uint64_t op,
    uint64_t start_ns,
    uint64_t end_ns,
    uint64_t start_cycles,
    uint64_t end_cycles,
    int64_t reply_status)
{
    const uint64_t slot = koboxd_fs_metric_slot(op);
    if (slot == 0 || slot >= KOBOXD_FS_METRIC_OP_MAX || start_ns == 0 || end_ns < start_ns) {
        return;
    }
    koboxd_fs_metric_t *metric = &koboxd_fs_metrics[slot];
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
    if (reply_status != 0) {
        metric->errors++;
    }
}

static void koboxd_dump_fs_metrics(void)
{
    for (uint64_t op = 0; op < KOBOXD_FS_METRIC_OP_MAX; ++op) {
        const koboxd_fs_metric_t *metric = &koboxd_fs_metrics[op];
        if (metric->count == 0) {
            continue;
        }
        printf(
            "[koboxd] metric scope=fs op=%s count=%llu avg_ns=%llu max_ns=%llu avg_cycles=%llu max_cycles=%llu errors=%llu\n",
            koboxd_fs_op_name(koboxd_fs_metric_op(op)),
            (unsigned long long)metric->count,
            (unsigned long long)(metric->total_ns / metric->count),
            (unsigned long long)metric->max_ns,
            (unsigned long long)(metric->total_cycles / metric->count),
            (unsigned long long)metric->max_cycles,
            (unsigned long long)metric->errors);
    }
}

typedef struct koboxd_fs_request_ctx {
    koboxd_fs_backend_t *backend;
    const struct pacha_ipc_msg *request;
    void *mapped;
    int vmo_fd;
    int64_t reply_status;
    uint64_t result;
} koboxd_fs_request_ctx_t;

static int map_fs_wire_page(koboxd_fs_request_ctx_t *ctx)
{
    if (ctx == NULL) {
        return -22;
    }
    ctx->mapped = koboxd_map_wire_vmo_from_msg(
        ctx->request,
        STORAGE_PAGE_BYTES,
        &ctx->vmo_fd);
    if (ctx->mapped == NULL) {
        ctx->reply_status = -22;
        return -22;
    }
    return 0;
}

static void handle_mount_root(koboxd_fs_request_ctx_t *ctx)
{
    ctx->result = ctx->backend->mount_result.observed_ext4_magic;
}

static void handle_lookup(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) {
        return;
    }
    storage_lookup_request_t *lookup = (storage_lookup_request_t *)ctx->mapped;
    lookup->name[STORAGE_NAME_BYTES - 1] = '\0';
    uint64_t object_id = 0;
    ctx->reply_status = koboxd_fs_backend_lookup(
        ctx->backend,
        lookup->parent_object_id,
        lookup->name,
        &object_id);
    ctx->result = object_id;
}

static void handle_statx(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) {
        return;
    }
    storage_statx_reply_t *wire_stat = (storage_statx_reply_t *)ctx->mapped;
    koboxd_fs_object_t stat;
    ctx->reply_status = koboxd_fs_backend_statx(
        ctx->backend,
        ctx->request->word2,
        &stat);
    if (ctx->reply_status != 0) {
        return;
    }
    memset(wire_stat, 0, sizeof(*wire_stat));
    wire_stat->object_id = stat.object_id;
    wire_stat->mode = stat.mode;
    wire_stat->size = stat.size;
    wire_stat->blocks = stat.blocks;
    wire_stat->nlink = stat.nlink;
    wire_stat->kind = (stat.mode & 0170000u);
    wire_stat->atime_sec = stat.atime_sec;
    wire_stat->atime_nsec = stat.atime_nsec;
    wire_stat->mtime_sec = stat.mtime_sec;
    wire_stat->mtime_nsec = stat.mtime_nsec;
    wire_stat->ctime_sec = stat.ctime_sec;
    wire_stat->ctime_nsec = stat.ctime_nsec;
    wire_stat->rdev = stat.rdev;
    ctx->result = stat.size;
}

static void handle_pread(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) {
        return;
    }
    storage_io_request_t *io = (storage_io_request_t *)ctx->mapped;
    size_t length = (size_t)io->length;
    if (length > sizeof(io->data)) {
        length = sizeof(io->data);
    }
    ctx->reply_status = koboxd_fs_backend_pread(
        ctx->backend,
        io->object_id,
        io->offset,
        io->data,
        length,
        sizeof(io->data));
    if (ctx->reply_status >= 0) {
        ctx->result = (uint64_t)ctx->reply_status;
        ctx->reply_status = 0;
    }
}

static void handle_pwrite(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) {
        return;
    }
    storage_io_request_t *io = (storage_io_request_t *)ctx->mapped;
    size_t length = (size_t)io->length;
    if (length > sizeof(io->data)) {
        length = sizeof(io->data);
    }
    ctx->reply_status = koboxd_fs_backend_pwrite(
        ctx->backend,
        io->object_id,
        io->offset,
        io->data,
        length);
    if (ctx->reply_status >= 0) {
        ctx->result = (uint64_t)ctx->reply_status;
        ctx->reply_status = 0;
    }
}

static void handle_create(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) {
        return;
    }
    storage_create_request_t *create = (storage_create_request_t *)ctx->mapped;
    create->name[STORAGE_NAME_BYTES - 1] = '\0';
    uint64_t object_id = 0;
    ctx->reply_status = koboxd_fs_backend_create(
        ctx->backend,
        create->parent_object_id,
        create->name,
        (uint16_t)create->mode,
        &object_id);
    ctx->result = object_id;
}

static void handle_truncate(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) {
        return;
    }
    storage_truncate_request_t *truncate = (storage_truncate_request_t *)ctx->mapped;
    ctx->reply_status = koboxd_fs_backend_truncate(
        ctx->backend,
        truncate->object_id,
        truncate->size);
}

static void handle_utimens(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) {
        return;
    }
    storage_utimens_request_t *utimens = (storage_utimens_request_t *)ctx->mapped;
    ctx->reply_status = koboxd_fs_backend_utimens(
        ctx->backend,
        utimens->object_id,
        (uint32_t)utimens->mask,
        utimens->atime_sec,
        utimens->atime_nsec,
        utimens->mtime_sec,
        utimens->mtime_nsec);
}

static void handle_chmod(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) {
        return;
    }
    storage_chmod_request_t *chmod_req = (storage_chmod_request_t *)ctx->mapped;
    ctx->reply_status = koboxd_fs_backend_chmod(
        ctx->backend,
        chmod_req->object_id,
        (uint16_t)chmod_req->mode);
}

static void handle_unlink(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) {
        return;
    }
    storage_unlink_request_t *unlink = (storage_unlink_request_t *)ctx->mapped;
    unlink->name[STORAGE_NAME_BYTES - 1] = '\0';
    ctx->reply_status = koboxd_fs_backend_unlink(
        ctx->backend,
        unlink->parent_object_id,
        unlink->name);
}

static void handle_mkdir(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) {
        return;
    }
    storage_mkdir_request_t *mkdir = (storage_mkdir_request_t *)ctx->mapped;
    mkdir->name[STORAGE_NAME_BYTES - 1] = '\0';
    uint64_t object_id = 0;
    ctx->reply_status = koboxd_fs_backend_mkdir(
        ctx->backend,
        mkdir->parent_object_id,
        mkdir->name,
        (uint16_t)mkdir->mode,
        &object_id);
    ctx->result = object_id;
}

static void handle_mknod(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) return;
    storage_mknod_request_t *mknod = (storage_mknod_request_t *)ctx->mapped;
    mknod->name[STORAGE_NAME_BYTES - 1] = '\0';
    uint64_t object_id = 0;
    ctx->reply_status = koboxd_fs_backend_mknod(
        ctx->backend,
        mknod->parent_object_id,
        mknod->name,
        (uint16_t)mknod->mode,
        mknod->dev,
        &object_id);
    ctx->result = object_id;
}

static void handle_rmdir(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) {
        return;
    }
    storage_rmdir_request_t *rmdir = (storage_rmdir_request_t *)ctx->mapped;
    rmdir->name[STORAGE_NAME_BYTES - 1] = '\0';
    ctx->reply_status = koboxd_fs_backend_rmdir(
        ctx->backend,
        rmdir->parent_object_id,
        rmdir->name);
}

static void handle_rename(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) {
        return;
    }
    storage_rename_request_t *rename = (storage_rename_request_t *)ctx->mapped;
    rename->old_name[STORAGE_NAME_BYTES - 1] = '\0';
    rename->new_name[STORAGE_NAME_BYTES - 1] = '\0';
    uint64_t object_id = 0;
    ctx->reply_status = koboxd_fs_backend_rename(
        ctx->backend,
        rename->old_parent_object_id,
        rename->old_name,
        rename->new_parent_object_id,
        rename->new_name,
        &object_id);
    ctx->result = object_id;
}

static void handle_getdents(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) {
        return;
    }
    storage_getdents_request_t *wire_dir = (storage_getdents_request_t *)ctx->mapped;
    koboxd_fs_object_t entries[STORAGE_DIRENT_CAPACITY];
    memset(entries, 0, sizeof(entries));
    size_t count = 0;
    size_t capacity = (size_t)wire_dir->capacity;
    if (capacity > STORAGE_DIRENT_CAPACITY) {
        capacity = STORAGE_DIRENT_CAPACITY;
    }
    ctx->reply_status = koboxd_fs_backend_getdents(
        ctx->backend,
        wire_dir->dir_object_id,
        wire_dir->offset,
        entries,
        capacity,
        &count);
    if (ctx->reply_status != 0) {
        return;
    }
    wire_dir->count = count;
    for (size_t i = 0; i < count; i++) {
        wire_dir->entries[i].object_id = entries[i].object_id;
        wire_dir->entries[i].kind = entries[i].mode & 0170000u;
        wire_dir->entries[i].name_len = strlen(entries[i].name);
        snprintf(wire_dir->entries[i].name, sizeof(wire_dir->entries[i].name), "%s", entries[i].name);
    }
    ctx->result = count;
}

static void dispatch_fs_request(koboxd_fs_request_ctx_t *ctx)
{
    switch (ctx->request->word1) {
    case STORAGE_OP_MOUNT_ROOT:
        handle_mount_root(ctx);
        break;
    case STORAGE_OP_LOOKUP:
        handle_lookup(ctx);
        break;
    case STORAGE_OP_STATX:
        handle_statx(ctx);
        break;
    case STORAGE_OP_PREAD:
        handle_pread(ctx);
        break;
    case STORAGE_OP_PWRITE:
        handle_pwrite(ctx);
        break;
    case STORAGE_OP_FSYNC:
        ctx->reply_status = koboxd_fs_backend_fsync(ctx->backend, ctx->request->word2);
        break;
    case STORAGE_OP_RELEASE_OBJECT:
        ctx->reply_status = koboxd_fs_backend_release_object(ctx->backend, ctx->request->word2);
        break;
    case STORAGE_OP_SYNC_ALL:
        ctx->reply_status = koboxd_fs_backend_sync_all(ctx->backend);
        break;
    case STORAGE_OP_CREATE:
        handle_create(ctx);
        break;
    case STORAGE_OP_TRUNCATE:
        handle_truncate(ctx);
        break;
    case STORAGE_OP_UTIMENS:
        handle_utimens(ctx);
        break;
    case STORAGE_OP_CHMOD:
        handle_chmod(ctx);
        break;
    case STORAGE_OP_UNLINK:
        handle_unlink(ctx);
        break;
    case STORAGE_OP_MKDIR:
        handle_mkdir(ctx);
        break;
    case STORAGE_OP_MKNOD:
        handle_mknod(ctx);
        break;
    case STORAGE_OP_RMDIR:
        handle_rmdir(ctx);
        break;
    case STORAGE_OP_RENAME:
        handle_rename(ctx);
        break;
    case STORAGE_OP_GETDENTS:
        handle_getdents(ctx);
        break;
    default:
        ctx->reply_status = -95;
        break;
    }
}

static void cleanup_fs_request(koboxd_fs_request_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->mapped != NULL) {
        (void)pacha_munmap(ctx->mapped, STORAGE_PAGE_BYTES);
    }
    if (ctx->vmo_fd >= 16) {
        (void)pacha_fd_close(ctx->vmo_fd);
    }
}

int koboxd_fs_endpoint_serve_once(
    koboxd_ipc_service_t *ipc_service,
    koboxd_fs_backend_t *fs_backend)
{
    const koboxd_ipc_endpoint_t *endpoint =
        koboxd_ipc_service_endpoint_const(ipc_service, KOBOXD_IPC_ENDPOINT_FS_BACKEND);
    if (endpoint == NULL || endpoint->endpoint_fd < 16 || fs_backend == NULL) {
        return -1;
    }
    struct pacha_ipc_fd fds[1];
    memset(fds, 0, sizeof(fds));
    struct pacha_ipc_msg request = {
        .fds = fds,
        .fd_capacity = 1,
    };
    int status = koboxd_recv_ipc_wait(endpoint->endpoint_fd, &request);
    if (status != 0) {
        return status;
    }
    if (request.word0 != PACHA_SERVICE_REQUEST_MAGIC) {
        return -2;
    }

    koboxd_fs_request_ctx_t ctx = {
        .backend = fs_backend,
        .request = &request,
        .mapped = NULL,
        .vmo_fd = -1,
        .reply_status = 0,
        .result = 0,
    };
    const uint64_t op = request.word1;
    const uint64_t start_ns = koboxd_now_ns();
    const uint64_t start_cycles = koboxd_read_tsc();
    koboxd_fs_backend_lock(fs_backend);
    dispatch_fs_request(&ctx);
    koboxd_fs_backend_unlock(fs_backend);
    koboxd_record_fs_metric(
        op,
        start_ns,
        koboxd_now_ns(),
        start_cycles,
        koboxd_read_tsc(),
        ctx.reply_status);
    cleanup_fs_request(&ctx);

    if (op == STORAGE_OP_SYNC_ALL) {
        koboxd_dump_fs_metrics();
    }

    return koboxd_send_status_reply_ex(endpoint->endpoint_fd, request.word3, ctx.reply_status, ctx.result);
}
