#include "fs_endpoint.h"

#include "ipc_wire.h"
#include "koboxd/ipc_protocol.h"
#include "pacha/abi.h"

#include <stdio.h>
#include <string.h>

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
        KOBOXD_WIRE_FS_PAGE_BYTES,
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
    koboxd_wire_fs_lookup_t *lookup = (koboxd_wire_fs_lookup_t *)ctx->mapped;
    lookup->name[KOBOXD_WIRE_FS_NAME_BYTES - 1] = '\0';
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
    koboxd_wire_fs_statx_t *wire_stat = (koboxd_wire_fs_statx_t *)ctx->mapped;
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
    ctx->result = stat.size;
}

static void handle_pread(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) {
        return;
    }
    koboxd_wire_fs_io_t *io = (koboxd_wire_fs_io_t *)ctx->mapped;
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
    koboxd_wire_fs_io_t *io = (koboxd_wire_fs_io_t *)ctx->mapped;
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
    koboxd_wire_fs_create_t *create = (koboxd_wire_fs_create_t *)ctx->mapped;
    create->name[KOBOXD_WIRE_FS_NAME_BYTES - 1] = '\0';
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
    koboxd_wire_fs_truncate_t *truncate = (koboxd_wire_fs_truncate_t *)ctx->mapped;
    ctx->reply_status = koboxd_fs_backend_truncate(
        ctx->backend,
        truncate->object_id,
        truncate->size);
}

static void handle_unlink(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) {
        return;
    }
    koboxd_wire_fs_unlink_t *unlink = (koboxd_wire_fs_unlink_t *)ctx->mapped;
    unlink->name[KOBOXD_WIRE_FS_NAME_BYTES - 1] = '\0';
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
    koboxd_wire_fs_mkdir_t *mkdir = (koboxd_wire_fs_mkdir_t *)ctx->mapped;
    mkdir->name[KOBOXD_WIRE_FS_NAME_BYTES - 1] = '\0';
    uint64_t object_id = 0;
    ctx->reply_status = koboxd_fs_backend_mkdir(
        ctx->backend,
        mkdir->parent_object_id,
        mkdir->name,
        (uint16_t)mkdir->mode,
        &object_id);
    ctx->result = object_id;
}

static void handle_rmdir(koboxd_fs_request_ctx_t *ctx)
{
    if (map_fs_wire_page(ctx) != 0) {
        return;
    }
    koboxd_wire_fs_rmdir_t *rmdir = (koboxd_wire_fs_rmdir_t *)ctx->mapped;
    rmdir->name[KOBOXD_WIRE_FS_NAME_BYTES - 1] = '\0';
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
    koboxd_wire_fs_rename_t *rename = (koboxd_wire_fs_rename_t *)ctx->mapped;
    rename->old_name[KOBOXD_WIRE_FS_NAME_BYTES - 1] = '\0';
    rename->new_name[KOBOXD_WIRE_FS_NAME_BYTES - 1] = '\0';
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
    koboxd_wire_fs_getdents_t *wire_dir = (koboxd_wire_fs_getdents_t *)ctx->mapped;
    koboxd_fs_object_t entries[KOBOXD_WIRE_FS_DIRENT_CAPACITY];
    memset(entries, 0, sizeof(entries));
    size_t count = 0;
    size_t capacity = (size_t)wire_dir->capacity;
    if (capacity > KOBOXD_WIRE_FS_DIRENT_CAPACITY) {
        capacity = KOBOXD_WIRE_FS_DIRENT_CAPACITY;
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
    case KOBOXD_WIRE_FS_MOUNT_ROOT:
        handle_mount_root(ctx);
        break;
    case KOBOXD_WIRE_FS_LOOKUP:
        handle_lookup(ctx);
        break;
    case KOBOXD_WIRE_FS_STATX:
        handle_statx(ctx);
        break;
    case KOBOXD_WIRE_FS_PREAD:
        handle_pread(ctx);
        break;
    case KOBOXD_WIRE_FS_PWRITE:
        handle_pwrite(ctx);
        break;
    case KOBOXD_WIRE_FS_FSYNC:
        ctx->reply_status = koboxd_fs_backend_fsync(ctx->backend, ctx->request->word2);
        break;
    case KOBOXD_WIRE_FS_RELEASE_OBJECT:
        ctx->reply_status = koboxd_fs_backend_release_object(ctx->backend, ctx->request->word2);
        break;
    case KOBOXD_WIRE_FS_CREATE:
        handle_create(ctx);
        break;
    case KOBOXD_WIRE_FS_TRUNCATE:
        handle_truncate(ctx);
        break;
    case KOBOXD_WIRE_FS_UNLINK:
        handle_unlink(ctx);
        break;
    case KOBOXD_WIRE_FS_MKDIR:
        handle_mkdir(ctx);
        break;
    case KOBOXD_WIRE_FS_RMDIR:
        handle_rmdir(ctx);
        break;
    case KOBOXD_WIRE_FS_RENAME:
        handle_rename(ctx);
        break;
    case KOBOXD_WIRE_FS_GETDENTS:
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
        (void)pacha_munmap(ctx->mapped, KOBOXD_WIRE_FS_PAGE_BYTES);
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
    if (request.word0 != KOBOXD_WIRE_ENDPOINT_MAGIC) {
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
    koboxd_fs_backend_lock(fs_backend);
    dispatch_fs_request(&ctx);
    koboxd_fs_backend_unlock(fs_backend);
    cleanup_fs_request(&ctx);

    return koboxd_send_status_reply_ex(endpoint->endpoint_fd, request.word3, ctx.reply_status, ctx.result);
}
