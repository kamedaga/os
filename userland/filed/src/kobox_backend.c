#include "filed/kobox_backend.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "filed/fd_ipc.h"
#include "pacha/abi.h"

static int filed_kobox_call_with_fd(
    filed_kobox_backend_t *backend,
    uint64_t op,
    uint64_t object_id,
    int transfer_fd,
    uint64_t *out_result);

static uint64_t filed_kobox_backend_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t filed_kobox_backend_read_tsc(void)
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

static const char *filed_kobox_backend_op_name(uint64_t op)
{
    switch (op) {
    case KOBOXD_WIRE_FS_MOUNT_ROOT: return "mount_root";
    case KOBOXD_WIRE_FS_LOOKUP: return "lookup";
    case KOBOXD_WIRE_FS_OPEN: return "open";
    case KOBOXD_WIRE_FS_PREAD: return "pread";
    case KOBOXD_WIRE_FS_PWRITE: return "pwrite";
    case KOBOXD_WIRE_FS_STATX: return "statx";
    case KOBOXD_WIRE_FS_GETDENTS: return "getdents";
    case KOBOXD_WIRE_FS_FSYNC: return "fsync";
    case KOBOXD_WIRE_FS_UTIMENS: return "utimens";
    case KOBOXD_WIRE_FS_CHMOD: return "chmod";
    case KOBOXD_WIRE_FS_CREATE: return "create";
    case KOBOXD_WIRE_FS_TRUNCATE: return "truncate";
    case KOBOXD_WIRE_FS_UNLINK: return "unlink";
    case KOBOXD_WIRE_FS_RENAME: return "rename";
    case KOBOXD_WIRE_FS_MKDIR: return "mkdir";
    case KOBOXD_WIRE_FS_RMDIR: return "rmdir";
    case KOBOXD_WIRE_FS_RELEASE_OBJECT: return "release_object";
    case KOBOXD_WIRE_FS_SYNC_ALL: return "sync_all";
    default: return "unknown";
    }
}

static void filed_kobox_backend_record_metric(
    filed_kobox_backend_t *backend,
    uint64_t op,
    uint64_t start_ns,
    uint64_t end_ns,
    uint64_t start_cycles,
    uint64_t end_cycles,
    int status)
{
    if (backend == NULL ||
        op >= FILED_KOBOX_BACKEND_METRIC_OP_MAX ||
        start_ns == 0 ||
        end_ns < start_ns)
    {
        return;
    }
    filed_kobox_backend_metric_t *metric = &backend->metrics[op];
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
        metric->errors++;
    }
}

static uint64_t filed_kobox_direct_begin(uint64_t *out_cycles)
{
    if (out_cycles != NULL) {
        *out_cycles = filed_kobox_backend_read_tsc();
    }
    return filed_kobox_backend_now_ns();
}

static int filed_kobox_direct_finish(
    filed_kobox_backend_t *backend,
    uint64_t op,
    uint64_t start_ns,
    uint64_t start_cycles,
    int status)
{
    filed_kobox_backend_record_metric(
        backend,
        op,
        start_ns,
        filed_kobox_backend_now_ns(),
        start_cycles,
        filed_kobox_backend_read_tsc(),
        status);
    return status;
}

static int filed_kobox_backend_is_direct(const filed_kobox_backend_t *backend)
{
    return backend != NULL && backend->direct_ctx != NULL && backend->direct_ops != NULL;
}

void filed_kobox_backend_dump_metrics(const filed_kobox_backend_t *backend)
{
    if (backend == NULL) {
        return;
    }
    for (uint64_t op = 0; op < FILED_KOBOX_BACKEND_METRIC_OP_MAX; ++op) {
        const filed_kobox_backend_metric_t *metric = &backend->metrics[op];
        if (metric->count == 0) {
            continue;
        }
        printf(
            "[filed] metric scope=kobox_client op=%s count=%llu avg_ns=%llu max_ns=%llu avg_cycles=%llu max_cycles=%llu errors=%llu\n",
            filed_kobox_backend_op_name(op),
            (unsigned long long)metric->count,
            (unsigned long long)(metric->total_ns / metric->count),
            (unsigned long long)metric->max_ns,
            (unsigned long long)(metric->total_cycles / metric->count),
            (unsigned long long)metric->max_cycles,
            (unsigned long long)metric->errors);
    }
    printf(
        "[filed] metric scope=kobox_client op=bytes_read count=%llu\n",
        (unsigned long long)backend->bytes_read);
    printf(
        "[filed] metric scope=kobox_client op=bytes_written count=%llu\n",
        (unsigned long long)backend->bytes_written);
}

static int filed_kobox_wire_page(
    filed_kobox_backend_t *backend,
    filed_wire_page_t **out_page)
{
    if (backend == NULL || out_page == NULL) {
        return -1;
    }

    if (!backend->wire_page_ready) {
        int status = filed_ipc_create_wire_page(KOBOXD_WIRE_FS_PAGE_BYTES, &backend->wire_page);
        if (status != 0) {
            return status;
        }
        backend->wire_page_ready = 1;
    }

    memset(backend->wire_page.addr, 0, (size_t)backend->wire_page.size);
    *out_page = &backend->wire_page;
    return 0;
}

static int filed_kobox_wire_page_fd(const filed_kobox_backend_t *backend, const filed_wire_page_t *page)
{
    (void)backend;
    return page != NULL ? page->fd : -1;
}

static int filed_kobox_call_with_fd(
    filed_kobox_backend_t *backend,
    uint64_t op,
    uint64_t object_id,
    int transfer_fd,
    uint64_t *out_result)
{
    if (backend == NULL || backend->fs_fd < 16 || out_result == NULL) {
        return -1;
    }

    struct pacha_ipc_fd fd_item;
    memset(&fd_item, 0, sizeof(fd_item));
    if (transfer_fd >= 16) {
        fd_item.fd = (uint64_t)(uint32_t)transfer_fd;
        fd_item.rights =
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE;
        fd_item.flags = 0;
        fd_item.transfer_flags = 0;
    }

    const uint64_t request_id = ++backend->calls;
    const struct pacha_ipc_msg request = {
        .word0 = KOBOXD_WIRE_ENDPOINT_MAGIC,
        .word1 = op,
        .word2 = object_id,
        .word3 = request_id,
        .fds = transfer_fd >= 16 ? &fd_item : NULL,
        .fd_count = transfer_fd >= 16 ? 1 : 0,
    };

    const uint64_t start_ns = filed_kobox_backend_now_ns();
    const uint64_t start_cycles = filed_kobox_backend_read_tsc();
    int status = filed_ipc_send_wait(backend->fs_fd, &request);
    if (status != 0) {
        filed_kobox_backend_record_metric(
            backend,
            op,
            start_ns,
            filed_kobox_backend_now_ns(),
            start_cycles,
            filed_kobox_backend_read_tsc(),
            status);
        return status;
    }

    for (unsigned int attempt = 0; attempt < 128; ++attempt) {
        struct pacha_ipc_msg reply;
        memset(&reply, 0, sizeof(reply));

        status = filed_ipc_recv_wait(backend->fs_fd, &reply);
        if (status != 0) {
            filed_kobox_backend_record_metric(
                backend,
                op,
                start_ns,
                filed_kobox_backend_now_ns(),
                start_cycles,
                filed_kobox_backend_read_tsc(),
                status);
            return status;
        }
        if (reply.word0 == KOBOXD_WIRE_REPLY_MAGIC &&
            reply.word3 == request_id)
        {
            if ((int64_t)reply.word1 < 0) {
                status = (int)(int64_t)reply.word1;
                filed_kobox_backend_record_metric(
                    backend,
                    op,
                    start_ns,
                    filed_kobox_backend_now_ns(),
                    start_cycles,
                    filed_kobox_backend_read_tsc(),
                    status);
                return status;
            }
            *out_result = reply.word2;
            filed_kobox_backend_record_metric(
                backend,
                op,
                start_ns,
                filed_kobox_backend_now_ns(),
                start_cycles,
                filed_kobox_backend_read_tsc(),
                0);
            return 0;
        }
    }

    filed_kobox_backend_record_metric(
        backend,
        op,
        start_ns,
        filed_kobox_backend_now_ns(),
        start_cycles,
        filed_kobox_backend_read_tsc(),
        -2);
    return -2;
}

void filed_kobox_backend_init(filed_kobox_backend_t *backend, int fs_fd)
{
    if (backend == NULL) {
        return;
    }

    memset(backend, 0, sizeof(*backend));
    backend->fs_fd = fs_fd;
    backend->root_object_id = KOBOXD_WIRE_FS_ROOT_OBJECT_ID;
}

void filed_kobox_backend_init_direct(
    filed_kobox_backend_t *backend,
    void *direct_ctx,
    const filed_kobox_direct_ops_t *direct_ops)
{
    if (backend == NULL) {
        return;
    }

    memset(backend, 0, sizeof(*backend));
    backend->fs_fd = -1;
    backend->direct_ctx = direct_ctx;
    backend->direct_ops = direct_ops;
    backend->root_object_id = KOBOXD_WIRE_FS_ROOT_OBJECT_ID;
}

int filed_kobox_backend_mount_root(filed_kobox_backend_t *backend)
{
    uint64_t magic = 0;

    if (backend == NULL) {
        return -1;
    }
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->mount_root == NULL) {
            return -95;
        }
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->mount_root(backend->direct_ctx, &magic);
        if (status != 0) {
            return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_MOUNT_ROOT, start_ns, start_cycles, status);
        }
        backend->ext4_magic = magic;
        return filed_kobox_direct_finish(
            backend,
            KOBOXD_WIRE_FS_MOUNT_ROOT,
            start_ns,
            start_cycles,
            magic == 0xef53u ? 0 : -3);
    }

    const int status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_MOUNT_ROOT,
        0,
        -1,
        &magic);
    if (status != 0) {
        return status;
    }

    backend->ext4_magic = magic;
    return magic == 0xef53u ? 0 : -3;
}

int filed_kobox_backend_lookup(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint64_t *out_object_id)
{
    filed_wire_page_t *page = NULL;
    uint64_t object_id = 0;

    if (backend == NULL || parent_object_id == 0 || name == NULL || out_object_id == NULL) {
        return -1;
    }
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->lookup == NULL) {
            return -95;
        }
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->lookup(
            backend->direct_ctx,
            parent_object_id,
            name,
            out_object_id);
        return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_LOOKUP, start_ns, start_cycles, status);
    }

    int status = filed_kobox_wire_page(backend, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_lookup_t *lookup = (koboxd_wire_fs_lookup_t *)page->addr;
    lookup->parent_object_id = parent_object_id;
    snprintf(lookup->name, sizeof(lookup->name), "%s", name);

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_LOOKUP,
        0,
        filed_kobox_wire_page_fd(backend, page),
        &object_id);
    if (status != 0) {
        return status;
    }

    *out_object_id = object_id;
    return 0;
}

int filed_kobox_backend_statx(
    filed_kobox_backend_t *backend,
    uint64_t object_id,
    koboxd_wire_fs_statx_t *out_stat)
{
    filed_wire_page_t *page = NULL;
    uint64_t ignored = 0;

    if (backend == NULL || object_id == 0 || out_stat == NULL) {
        return -1;
    }
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->statx == NULL) {
            return -95;
        }
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->statx(backend->direct_ctx, object_id, out_stat);
        return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_STATX, start_ns, start_cycles, status);
    }

    int status = filed_kobox_wire_page(backend, &page);
    if (status != 0) {
        return status;
    }

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_STATX,
        object_id,
        filed_kobox_wire_page_fd(backend, page),
        &ignored);
    if (status == 0) {
        *out_stat = *(koboxd_wire_fs_statx_t *)page->addr;
    }
    return status;
}

int filed_kobox_backend_pread(
    filed_kobox_backend_t *backend,
    uint64_t object_id,
    uint64_t offset,
    void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    filed_wire_page_t *page = NULL;
    uint64_t bytes = 0;

    if (backend == NULL || object_id == 0 || buffer == NULL || out_bytes == NULL) {
        return -1;
    }
    *out_bytes = 0;
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->pread == NULL) {
            return -95;
        }
        uint64_t bytes = 0;
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->pread(
            backend->direct_ctx,
            object_id,
            offset,
            buffer,
            length,
            &bytes);
        if (status == 0) {
            if (bytes > length) {
                bytes = length;
            }
            backend->bytes_read += bytes;
            *out_bytes = bytes;
        }
        return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_PREAD, start_ns, start_cycles, status);
    }

    int status = filed_kobox_wire_page(backend, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_io_t *io = (koboxd_wire_fs_io_t *)page->addr;
    io->object_id = object_id;
    io->offset = offset;
    io->length = length > KOBOXD_WIRE_FS_IO_BYTES ? KOBOXD_WIRE_FS_IO_BYTES : length;

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_PREAD,
        0,
        filed_kobox_wire_page_fd(backend, page),
        &bytes);
    if (status == 0 && bytes != 0) {
        if (bytes > length) {
            bytes = length;
        }
        memcpy(buffer, io->data, (size_t)bytes);
        backend->bytes_read += bytes;
        *out_bytes = bytes;
    }

    return status;
}

int filed_kobox_backend_pwrite(
    filed_kobox_backend_t *backend,
    uint64_t object_id,
    uint64_t offset,
    const void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    filed_wire_page_t *page = NULL;
    uint64_t bytes = 0;

    if (backend == NULL || object_id == 0 || out_bytes == NULL) {
        return -1;
    }
    if (buffer == NULL && length != 0) {
        return -1;
    }
    *out_bytes = 0;
    if (length == 0) {
        return 0;
    }
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->pwrite == NULL) {
            return -95;
        }
        uint64_t bytes = 0;
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->pwrite(
            backend->direct_ctx,
            object_id,
            offset,
            buffer,
            length,
            &bytes);
        if (status == 0) {
            if (bytes > length) {
                bytes = length;
            }
            backend->bytes_written += bytes;
            *out_bytes = bytes;
        }
        return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_PWRITE, start_ns, start_cycles, status);
    }

    int status = filed_kobox_wire_page(backend, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_io_t *io = (koboxd_wire_fs_io_t *)page->addr;
    io->object_id = object_id;
    io->offset = offset;
    io->length = length > KOBOXD_WIRE_FS_IO_BYTES ? KOBOXD_WIRE_FS_IO_BYTES : length;
    memcpy(io->data, buffer, (size_t)io->length);

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_PWRITE,
        0,
        filed_kobox_wire_page_fd(backend, page),
        &bytes);
    if (status == 0) {
        if (bytes > length) {
            bytes = length;
        }
        backend->bytes_written += bytes;
        *out_bytes = bytes;
    }

    return status;
}

int filed_kobox_backend_fsync(
    filed_kobox_backend_t *backend,
    uint64_t object_id)
{
    uint64_t ignored = 0;

    if (backend == NULL || object_id == 0) {
        return -1;
    }
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->fsync == NULL) {
            return -95;
        }
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->fsync(backend->direct_ctx, object_id);
        return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_FSYNC, start_ns, start_cycles, status);
    }

    return filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_FSYNC,
        object_id,
        -1,
        &ignored);
}

int filed_kobox_backend_create(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint64_t mode,
    uint64_t *out_object_id)
{
    filed_wire_page_t *page = NULL;
    uint64_t object_id = 0;

    if (backend == NULL || parent_object_id == 0 || name == NULL || out_object_id == NULL) {
        return -1;
    }
    *out_object_id = 0;
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->create == NULL) {
            return -95;
        }
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->create(
            backend->direct_ctx,
            parent_object_id,
            name,
            mode,
            out_object_id);
        return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_CREATE, start_ns, start_cycles, status);
    }

    int status = filed_kobox_wire_page(backend, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_create_t *create = (koboxd_wire_fs_create_t *)page->addr;
    create->parent_object_id = parent_object_id;
    create->mode = mode;
    snprintf(create->name, sizeof(create->name), "%s", name);

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_CREATE,
        0,
        filed_kobox_wire_page_fd(backend, page),
        &object_id);
    if (status != 0) {
        return status;
    }

    *out_object_id = object_id;
    return 0;
}

int filed_kobox_backend_truncate(
    filed_kobox_backend_t *backend,
    uint64_t object_id,
    uint64_t size)
{
    filed_wire_page_t *page = NULL;
    uint64_t ignored = 0;

    if (backend == NULL || object_id == 0) {
        return -1;
    }
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->truncate == NULL) {
            return -95;
        }
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->truncate(backend->direct_ctx, object_id, size);
        return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_TRUNCATE, start_ns, start_cycles, status);
    }

    int status = filed_kobox_wire_page(backend, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_truncate_t *truncate = (koboxd_wire_fs_truncate_t *)page->addr;
    truncate->object_id = object_id;
    truncate->size = size;

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_TRUNCATE,
        0,
        filed_kobox_wire_page_fd(backend, page),
        &ignored);
    return status;
}

int filed_kobox_backend_utimens(
    filed_kobox_backend_t *backend,
    uint64_t object_id,
    uint32_t mask,
    int64_t atime_sec,
    int64_t atime_nsec,
    int64_t mtime_sec,
    int64_t mtime_nsec)
{
    filed_wire_page_t *page = NULL;
    uint64_t ignored = 0;

    if (backend == NULL || object_id == 0) {
        return -1;
    }
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->utimens == NULL) {
            return -95;
        }
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->utimens(
            backend->direct_ctx,
            object_id,
            mask,
            atime_sec,
            atime_nsec,
            mtime_sec,
            mtime_nsec);
        return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_UTIMENS, start_ns, start_cycles, status);
    }

    int status = filed_kobox_wire_page(backend, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_utimens_t *utimens = (koboxd_wire_fs_utimens_t *)page->addr;
    utimens->object_id = object_id;
    utimens->mask = mask;
    utimens->atime_sec = atime_sec;
    utimens->atime_nsec = atime_nsec;
    utimens->mtime_sec = mtime_sec;
    utimens->mtime_nsec = mtime_nsec;

    return filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_UTIMENS,
        0,
        filed_kobox_wire_page_fd(backend, page),
        &ignored);
}

int filed_kobox_backend_chmod(
    filed_kobox_backend_t *backend,
    uint64_t object_id,
    uint64_t mode)
{
    filed_wire_page_t *page = NULL;
    uint64_t ignored = 0;

    if (backend == NULL || object_id == 0) {
        return -1;
    }
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->chmod == NULL) {
            return -95;
        }
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->chmod(
            backend->direct_ctx,
            object_id,
            mode);
        return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_CHMOD, start_ns, start_cycles, status);
    }

    int status = filed_kobox_wire_page(backend, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_chmod_t *chmod_req = (koboxd_wire_fs_chmod_t *)page->addr;
    chmod_req->object_id = object_id;
    chmod_req->mode = mode;

    return filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_CHMOD,
        0,
        filed_kobox_wire_page_fd(backend, page),
        &ignored);
}

int filed_kobox_backend_unlink(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name)
{
    filed_wire_page_t *page = NULL;
    uint64_t ignored = 0;

    if (backend == NULL || parent_object_id == 0 || name == NULL) {
        return -1;
    }
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->unlink == NULL) {
            return -95;
        }
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->unlink(
            backend->direct_ctx,
            parent_object_id,
            name);
        return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_UNLINK, start_ns, start_cycles, status);
    }

    int status = filed_kobox_wire_page(backend, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_unlink_t *unlink = (koboxd_wire_fs_unlink_t *)page->addr;
    unlink->parent_object_id = parent_object_id;
    snprintf(unlink->name, sizeof(unlink->name), "%s", name);

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_UNLINK,
        0,
        filed_kobox_wire_page_fd(backend, page),
        &ignored);
    return status;
}

int filed_kobox_backend_mkdir(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint64_t mode,
    uint64_t *out_object_id)
{
    filed_wire_page_t *page = NULL;
    uint64_t object_id = 0;

    if (backend == NULL || parent_object_id == 0 || name == NULL || out_object_id == NULL) {
        return -1;
    }
    *out_object_id = 0;
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->mkdir == NULL) {
            return -95;
        }
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->mkdir(
            backend->direct_ctx,
            parent_object_id,
            name,
            mode,
            out_object_id);
        return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_MKDIR, start_ns, start_cycles, status);
    }

    int status = filed_kobox_wire_page(backend, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_mkdir_t *mkdir = (koboxd_wire_fs_mkdir_t *)page->addr;
    mkdir->parent_object_id = parent_object_id;
    mkdir->mode = mode;
    snprintf(mkdir->name, sizeof(mkdir->name), "%s", name);

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_MKDIR,
        0,
        filed_kobox_wire_page_fd(backend, page),
        &object_id);
    if (status != 0) {
        return status;
    }

    *out_object_id = object_id;
    return 0;
}

int filed_kobox_backend_rmdir(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name)
{
    filed_wire_page_t *page = NULL;
    uint64_t ignored = 0;

    if (backend == NULL || parent_object_id == 0 || name == NULL) {
        return -1;
    }
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->rmdir == NULL) {
            return -95;
        }
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->rmdir(
            backend->direct_ctx,
            parent_object_id,
            name);
        return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_RMDIR, start_ns, start_cycles, status);
    }

    int status = filed_kobox_wire_page(backend, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_rmdir_t *rmdir = (koboxd_wire_fs_rmdir_t *)page->addr;
    rmdir->parent_object_id = parent_object_id;
    snprintf(rmdir->name, sizeof(rmdir->name), "%s", name);

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_RMDIR,
        0,
        filed_kobox_wire_page_fd(backend, page),
        &ignored);
    return status;
}

int filed_kobox_backend_rename(
    filed_kobox_backend_t *backend,
    uint64_t old_parent_object_id,
    const char *old_name,
    uint64_t new_parent_object_id,
    const char *new_name,
    uint64_t *out_object_id)
{
    filed_wire_page_t *page = NULL;
    uint64_t object_id = 0;

    if (backend == NULL ||
        old_parent_object_id == 0 ||
        old_name == NULL ||
        new_parent_object_id == 0 ||
        new_name == NULL ||
        out_object_id == NULL)
    {
        return -1;
    }
    *out_object_id = 0;
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->rename == NULL) {
            return -95;
        }
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->rename(
            backend->direct_ctx,
            old_parent_object_id,
            old_name,
            new_parent_object_id,
            new_name,
            out_object_id);
        return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_RENAME, start_ns, start_cycles, status);
    }

    int status = filed_kobox_wire_page(backend, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_rename_t *rename = (koboxd_wire_fs_rename_t *)page->addr;
    rename->old_parent_object_id = old_parent_object_id;
    rename->new_parent_object_id = new_parent_object_id;
    snprintf(rename->old_name, sizeof(rename->old_name), "%s", old_name);
    snprintf(rename->new_name, sizeof(rename->new_name), "%s", new_name);

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_RENAME,
        0,
        filed_kobox_wire_page_fd(backend, page),
        &object_id);
    if (status != 0) {
        return status;
    }
    *out_object_id = object_id;
    return 0;
}

int filed_kobox_backend_release_object(
    filed_kobox_backend_t *backend,
    uint64_t object_id)
{
    uint64_t ignored = 0;

    if (backend == NULL || object_id == 0) {
        return -1;
    }
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->release_object == NULL) {
            return -95;
        }
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->release_object(backend->direct_ctx, object_id);
        return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_RELEASE_OBJECT, start_ns, start_cycles, status);
    }

    return filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_RELEASE_OBJECT,
        object_id,
        -1,
        &ignored);
}

int filed_kobox_backend_getdents(
    filed_kobox_backend_t *backend,
    uint64_t dir_object_id,
    uint64_t offset,
    koboxd_wire_fs_getdents_t *out_entries)
{
    filed_wire_page_t *page = NULL;
    uint64_t ignored = 0;

    if (backend == NULL || dir_object_id == 0 || out_entries == NULL) {
        return -1;
    }
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->getdents == NULL) {
            return -95;
        }
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->getdents(
            backend->direct_ctx,
            dir_object_id,
            offset,
            out_entries);
        return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_GETDENTS, start_ns, start_cycles, status);
    }

    int status = filed_kobox_wire_page(backend, &page);
    if (status != 0) {
        return status;
    }

    koboxd_wire_fs_getdents_t *request = (koboxd_wire_fs_getdents_t *)page->addr;
    request->dir_object_id = dir_object_id;
    request->offset = offset;
    request->capacity = KOBOXD_WIRE_FS_DIRENT_CAPACITY;

    status = filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_GETDENTS,
        0,
        filed_kobox_wire_page_fd(backend, page),
        &ignored);
    if (status == 0) {
        *out_entries = *request;
    }

    return status;
}

int filed_kobox_backend_sync_all(filed_kobox_backend_t *backend)
{
    uint64_t ignored = 0;

    if (backend == NULL) {
        return -1;
    }
    if (filed_kobox_backend_is_direct(backend)) {
        if (backend->direct_ops->sync_all == NULL) {
            return 0;
        }
        uint64_t start_cycles = 0;
        const uint64_t start_ns = filed_kobox_direct_begin(&start_cycles);
        const int status = backend->direct_ops->sync_all(backend->direct_ctx);
        return filed_kobox_direct_finish(backend, KOBOXD_WIRE_FS_SYNC_ALL, start_ns, start_cycles, status);
    }

    return filed_kobox_call_with_fd(
        backend,
        KOBOXD_WIRE_FS_SYNC_ALL,
        0,
        -1,
        &ignored);
}
