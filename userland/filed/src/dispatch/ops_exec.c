#include "common.h"

filed_page_dispatch_result_t filed_dispatch_exec_path_session_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_exec_path_t *exec = (filed_exec_path_t *)page;
    const uint64_t known_flags =
        FILED_EXEC_INHERIT_HANDLES |
        FILED_EXEC_LINUX_LPR |
        FILED_EXEC_LINUX_BOOTSTRAP |
        FILED_EXEC_LINUX_DEFAULT_STDIO |
        FILED_EXEC_TRANSFER_PROCESS_FD;
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
    filed_handle_id_t inherit_handles[FILED_EXEC_MAX_INHERIT_HANDLES];
    filed_dispatch_saved_fd_init(&lpr_bootstrap_saved);
    memset(inherit_handles, 0, sizeof(inherit_handles));

    if ((exec->flags & ~known_flags) != 0 ||
        exec->inherit_fd_count != 0 ||
        exec->fd_patch_count != 0 ||
        exec->inherit_handle_count > FILED_EXEC_MAX_INHERIT_HANDLES ||
        exec->argc > FILED_EXEC_MAX_ARGS ||
        exec->envc > FILED_EXEC_MAX_ENVS ||
        exec->string_bytes > FILED_EXEC_STRING_BYTES ||
        !filed_name_is_terminated(exec->path, sizeof(exec->path)) ||
        (!filed_exec_string_ref_empty(exec->cwd) &&
            !filed_exec_string_ref_valid(exec, exec->cwd)) ||
        !filed_dispatch_exec_default_stdio_valid(exec) ||
        !filed_dispatch_exec_lpr_fd_table_valid(exec, NULL, 0))
    {
        goto out;
    }
    for (uint64_t i = 0; i < exec->argc; ++i) {
        if (!filed_exec_string_ref_valid(exec, exec->argv[i])) {
            goto out;
        }
    }
    for (uint64_t i = 0; i < exec->envc; ++i) {
        if (!filed_exec_string_ref_valid(exec, exec->envp[i])) {
            goto out;
        }
    }
    if ((exec->flags & FILED_EXEC_INHERIT_HANDLES) == 0 &&
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
    if ((exec->flags & FILED_EXEC_LINUX_LPR) != 0 &&
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
    if ((exec->flags & FILED_EXEC_LINUX_LPR) != 0 &&
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
    if ((exec->flags & (FILED_EXEC_LINUX_LPR | FILED_EXEC_LINUX_BOOTSTRAP)) ==
        (FILED_EXEC_LINUX_LPR | FILED_EXEC_LINUX_BOOTSTRAP))
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

    filed_openat_t openat;
    memset(&openat, 0, sizeof(openat));
    openat.dir_handle = exec->dir_handle;
    openat.rights =
        FILED_RIGHT_READ |
        FILED_RIGHT_EXEC |
        FILED_RIGHT_STAT;
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
        for (uint64_t i = 0; i < FILED_EXEC_MAX_INHERIT_HANDLES; ++i) {
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

int filed_dispatch_exec_path(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    pacha_service_envelope_t request_header;
    memset(&request_header, 0, sizeof(request_header));
    request_header.magic = PACHA_SERVICE_REQUEST_MAGIC;
    request_header.abi_version = PACHA_SERVICE_ABI_VERSION;
    request_header.service_id = FILED_SERVICE_ID;
    request_header.op = FILED_OP_EXEC_PATH;
    request_header.request_id = request->word3;
    request_header.trace_id = request->word3;
    request_header.payload_size = sizeof(filed_exec_path_t);

    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply(reply_fd, NULL, &request_header, -22, 0, 0);
    }

    filed_exec_path_t *exec = (filed_exec_path_t *)page;
    const uint64_t known_flags =
        FILED_EXEC_BOOTSTRAP_FD |
        FILED_EXEC_INHERIT_FDS |
        FILED_EXEC_PATCH_BOOTSTRAP_FDS |
        FILED_EXEC_INHERIT_HANDLES |
        FILED_EXEC_LINUX_LPR |
        FILED_EXEC_LINUX_BOOTSTRAP |
        FILED_EXEC_LINUX_DEFAULT_STDIO |
        FILED_EXEC_TRANSFER_PROCESS_FD;
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
    int inherit_fds[FILED_EXEC_MAX_INHERIT_FDS];
    filed_dispatch_saved_fd_t inherit_saved[FILED_EXEC_MAX_INHERIT_FDS];
    filed_dispatch_saved_fd_t lpr_bootstrap_saved;
    filed_handle_id_t inherit_handles[FILED_EXEC_MAX_INHERIT_HANDLES];
    memset(inherit_fds, 0xff, sizeof(inherit_fds));
    for (uint64_t i = 0; i < FILED_EXEC_MAX_INHERIT_FDS; ++i) {
        filed_dispatch_saved_fd_init(&inherit_saved[i]);
    }
    filed_dispatch_saved_fd_init(&lpr_bootstrap_saved);
    memset(inherit_handles, 0, sizeof(inherit_handles));

    if ((exec_flags & ~known_flags) != 0 ||
        exec->inherit_fd_count > FILED_EXEC_MAX_INHERIT_FDS ||
        exec->inherit_handle_count > FILED_EXEC_MAX_INHERIT_HANDLES ||
        exec->fd_patch_count > FILED_EXEC_MAX_FD_PATCHES ||
        exec->argc > FILED_EXEC_MAX_ARGS ||
        exec->envc > FILED_EXEC_MAX_ENVS ||
        exec->string_bytes > FILED_EXEC_STRING_BYTES ||
        !filed_name_is_terminated(exec->path, sizeof(exec->path)) ||
        (!filed_exec_string_ref_empty(exec->cwd) &&
            !filed_exec_string_ref_valid(exec, exec->cwd)) ||
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
        if (!filed_exec_string_ref_valid(exec, exec->argv[i])) {
            goto out;
        }
    }
    for (uint64_t i = 0; i < exec->envc; ++i) {
        if (!filed_exec_string_ref_valid(exec, exec->envp[i])) {
            goto out;
        }
    }
    const uint64_t inherit_fd_count = exec->inherit_fd_count;
    const uint64_t inherit_handle_count = exec->inherit_handle_count;

    const uint64_t has_bootstrap =
        (exec_flags & FILED_EXEC_BOOTSTRAP_FD) != 0 ? 1u : 0u;
    if ((exec_flags & FILED_EXEC_INHERIT_FDS) == 0 && inherit_fd_count != 0) {
        goto out;
    }
    if ((exec_flags & FILED_EXEC_PATCH_BOOTSTRAP_FDS) == 0 && exec->fd_patch_count != 0) {
        goto out;
    }
    if ((exec_flags & FILED_EXEC_PATCH_BOOTSTRAP_FDS) != 0 && has_bootstrap == 0) {
        goto out;
    }
    if ((exec_flags & FILED_EXEC_INHERIT_HANDLES) == 0 && inherit_handle_count != 0) {
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

    if ((exec_flags & FILED_EXEC_BOOTSTRAP_FD) != 0) {
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
    if ((exec_flags & FILED_EXEC_LINUX_LPR) != 0 &&
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
    if ((exec_flags & FILED_EXEC_LINUX_LPR) != 0 &&
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
    if ((exec_flags & (FILED_EXEC_LINUX_LPR | FILED_EXEC_LINUX_BOOTSTRAP)) ==
        (FILED_EXEC_LINUX_LPR | FILED_EXEC_LINUX_BOOTSTRAP))
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

    if ((exec_flags & FILED_EXEC_PATCH_BOOTSTRAP_FDS) != 0) {
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
            const filed_exec_fd_patch_t *patch = &exec->fd_patches[i];
            uint64_t value = 0;
            if (patch->reserved0 != 0 || patch->offset > FILED_BOOTSTRAP_PATCH_BYTES - 8u) {
                reply_status = -22;
                break;
            }
            if (patch->kind == FILED_EXEC_PATCH_INHERIT_FD) {
                if (patch->index >= inherit_fd_count) {
                    reply_status = -22;
                    break;
                }
                value = (uint64_t)(uint32_t)inherit_fds[patch->index];
            } else if (patch->kind == FILED_EXEC_PATCH_BOOTSTRAP_FD) {
                if (patch->index != 0) {
                    reply_status = -22;
                    break;
                }
                value = (uint64_t)(uint32_t)bootstrap_fd;
            } else if (patch->kind == FILED_EXEC_PATCH_INHERIT_HANDLE) {
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

    filed_openat_t openat;
    memset(&openat, 0, sizeof(openat));
    openat.dir_handle = exec->dir_handle;
    openat.rights =
        FILED_RIGHT_READ |
        FILED_RIGHT_EXEC |
        FILED_RIGHT_STAT;
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
    const uint64_t reply_result = reply_status == 0 ? (uint64_t)(uint32_t)process_fd : 0;
    if (reply_status != 0) {
        (void)filed_error_token(
            reply_status,
            FILED_OP_EXEC_PATH,
            PACHA_STATUS_STAGE_STATUS_MAP,
            reply_status,
            request->word3,
            0,
            0,
            0,
            "filed exec_path negative reply");
    }
    pacha_service_reply_init(
        (pacha_service_envelope_t *)page,
        &request_header,
        reply_status,
        PACHA_SERVICE_ERROR_FILED_EXEC,
        reply_result,
        0);
    (void)pacha_munmap(page, FILED_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    if (bootstrap_fd >= 16) {
        (void)pacha_fd_close(bootstrap_fd);
    }
    for (uint64_t i = 0; i < FILED_EXEC_MAX_INHERIT_FDS; ++i) {
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
        for (uint64_t i = 0; i < FILED_EXEC_MAX_INHERIT_HANDLES; ++i) {
            if (inherit_handles[i] != 0) {
                (void)filed_vfs_close_handle(&runtime->vfs, inherit_handles[i]);
            }
        }
    }
    if (reply_status == 0 && process_fd >= 16 && thread_fd >= 16) {
        const int send_status = filed_send_exec_reply(
            reply_fd,
            request->word3,
            process_fd,
            thread_fd,
            (exec_flags & FILED_EXEC_TRANSFER_PROCESS_FD) != 0);
        if (send_status != 0) {
            for (uint64_t i = 0; i < FILED_EXEC_MAX_INHERIT_HANDLES; ++i) {
                if (inherit_handles[i] != 0) {
                    (void)filed_vfs_close_handle(&runtime->vfs, inherit_handles[i]);
                }
            }
        }
        return send_status;
    }
    return filed_send_reply(reply_fd, NULL, &request_header, reply_status, 0, 0);
}

int filed_dispatch_exec_self(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request)
{
    pacha_service_envelope_t request_header;
    memset(&request_header, 0, sizeof(request_header));
    request_header.magic = PACHA_SERVICE_REQUEST_MAGIC;
    request_header.abi_version = PACHA_SERVICE_ABI_VERSION;
    request_header.service_id = FILED_SERVICE_ID;
    request_header.op = FILED_OP_EXEC_SELF;
    request_header.request_id = request->word3;
    request_header.trace_id = request->word3;
    request_header.payload_size = sizeof(filed_exec_path_t);

    int page_fd = -1;
    void *page = filed_map_request_page(request, FILED_PAGE_BYTES, &page_fd);
    if (page == NULL) {
        return filed_send_reply(reply_fd, NULL, &request_header, -22, 0, 0);
    }

    filed_exec_path_t *exec = (filed_exec_path_t *)page;
    const uint64_t known_flags =
        FILED_EXEC_LINUX_LPR |
        FILED_EXEC_LINUX_BOOTSTRAP |
        FILED_EXEC_SELF |
        FILED_EXEC_LPR_FD_TABLE;
    int64_t reply_status = -22;
    int process_fd = -1;
    int thread_fd = -1;
    int bootstrap_fd = -1;
    int lpr_fd_table_fd = -1;
    void *lpr_fd_table_page = NULL;
    const filed_exec_lpr_fd_table_t *lpr_fd_table = NULL;
    const int wants_lpr_fd_table = (exec->flags & FILED_EXEC_LPR_FD_TABLE) != 0;
    const uint64_t expected_request_fd_count = wants_lpr_fd_table ? 3u : 2u;

    if ((exec->flags & ~known_flags) != 0 ||
        (exec->flags & (FILED_EXEC_LINUX_LPR | FILED_EXEC_LINUX_BOOTSTRAP | FILED_EXEC_SELF)) !=
            (FILED_EXEC_LINUX_LPR | FILED_EXEC_LINUX_BOOTSTRAP | FILED_EXEC_SELF) ||
        exec->inherit_fd_count != 0 ||
        exec->inherit_handle_count != 0 ||
        exec->fd_patch_count != 0 ||
        exec->argc > FILED_EXEC_MAX_ARGS ||
        exec->envc > FILED_EXEC_MAX_ENVS ||
        exec->string_bytes > FILED_EXEC_STRING_BYTES ||
        request->fd_count != expected_request_fd_count ||
        request->fds == NULL ||
        !filed_name_is_terminated(exec->path, sizeof(exec->path)) ||
        (!filed_exec_string_ref_empty(exec->cwd) &&
            !filed_exec_string_ref_valid(exec, exec->cwd)) ||
        !filed_exec_string_ref_empty(exec->ctty) ||
        (wants_lpr_fd_table &&
            (exec->lpr_fd_table_bytes < sizeof(filed_exec_lpr_fd_table_t) ||
                request->fds[1].fd < 16)) ||
        (!wants_lpr_fd_table &&
            !filed_dispatch_exec_lpr_fd_table_valid(exec, NULL, 1)))
    {
        goto out;
    }
    for (uint64_t i = 0; i < exec->argc; ++i) {
        if (!filed_exec_string_ref_valid(exec, exec->argv[i])) {
            goto out;
        }
    }
    for (uint64_t i = 0; i < exec->envc; ++i) {
        if (!filed_exec_string_ref_valid(exec, exec->envp[i])) {
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
        lpr_fd_table = (const filed_exec_lpr_fd_table_t *)lpr_fd_table_page;
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

    filed_openat_t openat;
    memset(&openat, 0, sizeof(openat));
    openat.dir_handle = exec->dir_handle;
    openat.rights =
        FILED_RIGHT_READ |
        FILED_RIGHT_EXEC |
        FILED_RIGHT_STAT;
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
    (void)pacha_munmap(page, FILED_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    if (reply_status == 0 && process_fd >= 16 && thread_fd >= 16 && bootstrap_fd >= 16) {
        return filed_send_exec_self_reply(reply_fd, request->word3, process_fd, thread_fd, bootstrap_fd);
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

int filed_dispatch_session_open(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request,
    void *reply_page,
    const pacha_service_envelope_t *header,
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
        return filed_send_reply(reply_fd, reply_page, header, -22, 0, 0);
    }

    int64_t reply_status = -24;
    uint64_t result = 0;
    const int channel_fd = (int)request->fds[1].fd;
    const int page_fd = (int)request->fds[2].fd;
    void *session_page = pacha_mmap(
        page_fd,
        FILED_SESSION_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (session_page == NULL) {
        return filed_send_reply(reply_fd, reply_page, header, -14, 0, 0);
    }

    filed_fast_header_t *fast = (filed_fast_header_t *)session_page;
    if (fast->magic != FILED_FAST_MAGIC ||
        fast->version != FILED_FAST_VERSION ||
        fast->request_capacity != FILED_FAST_REQUEST_CAPACITY ||
        fast->completion_capacity != FILED_FAST_COMPLETION_CAPACITY ||
        fast->payload_slot_count != FILED_FAST_PAYLOAD_SLOT_COUNT ||
        fast->payload_slot_size != FILED_PAGE_BYTES ||
        fast->payload_offset != FILED_FAST_PAYLOAD_OFFSET ||
        fast->generation_offset != FILED_FAST_GENERATION_OFFSET ||
        fast->generation_capacity != FILED_FAST_GENERATION_CAPACITY ||
        fast->payload_offset + fast->payload_slot_count * fast->payload_slot_size > FILED_SESSION_PAGE_BYTES)
    {
        (void)pacha_munmap(session_page, FILED_SESSION_PAGE_BYTES);
        return filed_send_reply(reply_fd, reply_page, header, -71, 0, 0);
    }

    for (uint64_t i = 0; i < FILED_RUNTIME_MAX_SESSIONS; ++i) {
        filed_session_t *session = &runtime->sessions[i];
        if (session->active) {
            continue;
        }
        session->channel_fd = channel_fd;
        session->page_fd = page_fd;
        session->page = session_page;
        session->page_size = FILED_SESSION_PAGE_BYTES;
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
        (void)pacha_munmap(session_page, FILED_SESSION_PAGE_BYTES);
    }
    return filed_send_reply(reply_fd, reply_page, header, reply_status, result, 0);
}

uint64_t filed_import_termd_error(
    filed_runtime_t *runtime,
    uint64_t child_token,
    uint64_t request_id,
    int64_t status,
    uint64_t fd_count,
    uint64_t subject,
    const char *text)
{
    (void)runtime;
    return filed_error_token(
        status,
        FILED_OP_SERVICE_REGISTER_TERMD_SIGNAL_SUPERVISOR,
        PACHA_STATUS_STAGE_CHILD_STATUS,
        status,
        request_id,
        fd_count,
        subject,
        child_token,
        text);
}

int filed_dispatch_register_termd_signal_supervisor(
    filed_runtime_t *runtime,
    int reply_fd,
    const struct pacha_ipc_msg *request,
    void *reply_page,
    const pacha_service_envelope_t *header)
{
    if (runtime == NULL ||
        request == NULL ||
        request->fds == NULL ||
        request->fd_count < 3 ||
        request->fds[1].fd < 16 ||
        runtime->termd_tty_endpoint_fd < 16 ||
        header == NULL ||
        header->payload_size < sizeof(filed_service_endpoint_request_t))
    {
        return filed_send_reply(reply_fd, reply_page, header, -22, 0, 0);
    }

    const int supervisor_endpoint_fd = (int)(uint32_t)request->fds[1].fd;
    const uint64_t page_rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int page_fd = pacha_vmo_create(TERMD_PAGE_BYTES, page_rights, 0);
    if (page_fd < 16) {
        const uint64_t token = filed_error_token(
            page_fd,
            header->op,
            PACHA_STATUS_STAGE_KERNEL_SYSCALL,
            page_fd,
            header->request_id,
            request->fd_count,
            0,
            0,
            "termd signal supervisor page create failed");
        return filed_send_reply(reply_fd, reply_page, header, page_fd, 0, token);
    }
    void *page = pacha_mmap(
        page_fd,
        TERMD_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (page == NULL) {
        const uint64_t token = filed_error_token(
            -5,
            header->op,
            PACHA_STATUS_STAGE_MAP_PAGE,
            -5,
            header->request_id,
            request->fd_count,
            (uint64_t)(uint32_t)page_fd,
            0,
            "termd signal supervisor page map failed");
        (void)pacha_fd_close(page_fd);
        return filed_send_reply(reply_fd, reply_page, header, -5, 0, token);
    }

    memset(page, 0, TERMD_PAGE_BYTES);
    pacha_service_envelope_t *termd_header = (pacha_service_envelope_t *)page;
    termd_header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    termd_header->abi_version = PACHA_SERVICE_ABI_VERSION;
    termd_header->service_id = TERMD_SERVICE_ID;
    termd_header->op = TERMD_OP_SIGNAL_REGISTER_SUPERVISOR;
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
            PACHA_STATUS_STAGE_CHILD_RPC_CALL,
            termd_reply_fd,
            header->request_id,
            request->fd_count,
            (uint64_t)(uint32_t)runtime->termd_tty_endpoint_fd,
            0,
            "termd signal supervisor call failed");
        (void)pacha_munmap(page, TERMD_PAGE_BYTES);
        (void)pacha_fd_close(page_fd);
        return filed_send_reply(reply_fd, reply_page, header, termd_reply_fd, 0, token);
    }

    struct pacha_ipc_msg termd_reply;
    memset(&termd_reply, 0, sizeof(termd_reply));
    const int recv_status =
        pacha_ipc_recv_wait(termd_reply_fd, &termd_reply, PACHA_FD_WAIT_FOREVER);
    (void)pacha_fd_close(termd_reply_fd);
    const pacha_service_envelope_t *reply_header =
        (const pacha_service_envelope_t *)page;

    int64_t status = recv_status;
    uint64_t result = 0;
    uint64_t error_token = 0;
    if (recv_status == 0) {
        if (termd_reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
            termd_reply.word3 != termd_request.word3 ||
            reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
            reply_header->service_id != TERMD_SERVICE_ID ||
            reply_header->op != TERMD_OP_SIGNAL_REGISTER_SUPERVISOR ||
            reply_header->request_id != termd_request.word3)
        {
            status = -5;
            error_token = filed_error_token(
                status,
                header->op,
                PACHA_STATUS_STAGE_REPLY_MAGIC,
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
                    TERMD_OP_SIGNAL_REGISTER_SUPERVISOR,
                    "termd signal supervisor returned error");
            }
        }
    } else {
        error_token = filed_error_token(
            status,
            header->op,
            PACHA_STATUS_STAGE_CHILD_RPC_RECV,
            status,
            header->request_id,
            request->fd_count,
            (uint64_t)(uint32_t)termd_reply_fd,
            0,
            "termd signal supervisor reply recv failed");
    }

    (void)pacha_munmap(page, TERMD_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return filed_send_reply(reply_fd, reply_page, header, status, result, error_token);
}
