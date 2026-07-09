#include "common.h"

filed_page_dispatch_result_t filed_dispatch_openat_page(
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

filed_page_dispatch_result_t filed_dispatch_validate_open_cache_page(
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

filed_page_dispatch_result_t filed_dispatch_stat_page(
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

filed_page_dispatch_result_t filed_dispatch_utimens_page(
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

int filed_ensure_stat_snapshot(
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

filed_page_dispatch_result_t filed_dispatch_chmod_page(
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

filed_page_dispatch_result_t filed_dispatch_pread_page(
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

filed_page_dispatch_result_t filed_dispatch_pread_to_vmo_page(
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
    entry->clock = ++filed_file_vmo_cache.clock;
    filed_cache_note_attachment(runtime, decision->backend_object, FILED_CACHE_ATTACHMENT_VMO);
    filed_file_vmo_cache_stores++;
    *out_entry = entry;
    return filed_page_result(0, bytes);
}

int filed_dispatch_file_vmo_v2(
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
        (void)filed_error_token(
            result.status,
            header->op,
            PACHA_STATUS_STAGE_STATUS_MAP,
            result.status,
            header->request_id,
            request->fd_count,
            file_vmo->handle,
            0,
            "filed v2 file-vmo negative reply");
    }
    const int reply_status = pacha_ipc_reply(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    return reply_status;
}

filed_page_dispatch_result_t filed_dispatch_read_page(
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

filed_page_dispatch_result_t filed_dispatch_pwrite_page(
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

filed_page_dispatch_result_t filed_dispatch_write_page(
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

filed_page_dispatch_result_t filed_dispatch_seek_page(
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
    printf(
        "[filed] sync_all page_cache_dirty=%llu backend_dirty_hint=%llu backend_status=%d\n",
        (unsigned long long)dirty_count,
        (unsigned long long)backend_dirty_hint,
        backend_status);
    fflush(stdout);
    return backend_status;
}

filed_page_dispatch_result_t filed_dispatch_truncate_page(
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
