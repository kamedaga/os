#include "common.h"

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

uint64_t filed_error_token(
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
    pacha_trace6(
        PACHA_TRACE_COMPONENT_FILED,
        PACHA_TRACE_EVENT_GENERIC_ERROR,
        PACHA_TRACE_CLASS_ERROR,
        op,
        stage,
        (uint64_t)status,
        (uint64_t)raw_status,
        request_id,
        fd_count);
    pacha_trace4(
        PACHA_TRACE_COMPONENT_FILED,
        PACHA_TRACE_EVENT_GENERIC_ERROR,
        PACHA_TRACE_CLASS_ERROR,
        subject,
        child_token,
        text != NULL ? pacha_trace_name_id(text) : 0,
        0);
    return 0;
}

int filed_send_reply_v2_payload(
    int reply_fd,
    void *page,
    const pacha_service_request_header_t *header,
    int64_t status,
    uint64_t result,
    uint64_t error_token,
    uint32_t payload_size)
{
    (void)error_token;
    const uint64_t reply_result = status < 0 ? 0 : result;
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

int filed_send_reply_v2(
    int reply_fd,
    void *page,
    const pacha_service_request_header_t *header,
    int64_t status,
    uint64_t result,
    uint64_t error_token)
{
    return filed_send_reply_v2_payload(reply_fd, page, header, status, result, error_token, 0);
}

int filed_send_session_reply_v2(int channel_fd, uint64_t request_id, int64_t status, uint64_t result)
{
    if (status < 0) {
        (void)filed_error_token(
            status,
            FILED_V2_OP_SESSION_DOORBELL,
            PACHA_STATUS_STAGE_STATUS_MAP,
            status,
            request_id,
            0,
            0,
            0,
            "filed v2 session negative reply");
    }
    const struct pacha_ipc_msg reply = {
        .word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = status < 0 ? 0 : result,
        .word3 = request_id,
    };
    return filed_ipc_send_wait(channel_fd, &reply);
}

int filed_send_exec_reply_v2(
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

int filed_send_exec_self_reply_v2(
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

int filed_dispatch_set_inherit(int fd, int enabled)
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

void filed_dispatch_saved_fd_init(filed_dispatch_saved_fd_t *saved)
{
    if (saved == NULL) {
        return;
    }
    saved->fd = -1;
    saved->rights = 0;
    saved->flags = 0;
}

void filed_dispatch_close_owned_fd(int *fd)
{
    if (fd == NULL || *fd < 0) {
        return;
    }
    (void)pacha_fd_close(*fd);
    *fd = -1;
}

int filed_dispatch_save_target_fd(int target_fd, filed_dispatch_saved_fd_t *saved)
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

void filed_dispatch_restore_target_fd(int target_fd, filed_dispatch_saved_fd_t *saved)
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

int filed_dispatch_exec_default_stdio_valid(const filed_v2_exec_path_t *exec)
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

int filed_dispatch_lpr_fd_desc_valid(const filed_v2_exec_lpr_fd_t *fd)
{
    if (fd == NULL || fd->fd > LPR_LINUX_FD_MAX) {
        return 0;
    }
    switch (fd->kind) {
    case FILED_V2_EXEC_LPR_FD_FILED:
    case FILED_V2_EXEC_LPR_FD_TTY:
    case FILED_V2_EXEC_LPR_FD_SOCKET:
        return fd->handle != 0;
    case FILED_V2_EXEC_LPR_FD_PIPE:
    case FILED_V2_EXEC_LPR_FD_EVENT:
        return 1;
    default:
        return 0;
    }
}

int filed_dispatch_exec_lpr_fd_table_valid(
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

int filed_dispatch_create_lpr_bootstrap_fd(
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

int filed_dispatch_prepare_inherit_fd_to_target(
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

int filed_dispatch_dup_endpoint_to_fixed(
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

int filed_dispatch_prepare_endpoint_to_fixed(
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

void filed_dispatch_close_prepared_endpoint(int *fd, int borrowed)
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
