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

int filed_send_reply_payload(
    int reply_fd,
    void *page,
    const pacha_service_envelope_t *header,
    int64_t status,
    uint64_t result,
    uint64_t error_token,
    uint32_t payload_size)
{
    (void)error_token;
    const uint64_t reply_result = status < 0 ? 0 : result;
    if (page != NULL) {
        pacha_service_reply_init(
            (pacha_service_envelope_t *)page,
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

int filed_send_reply(
    int reply_fd,
    void *page,
    const pacha_service_envelope_t *header,
    int64_t status,
    uint64_t result,
    uint64_t error_token)
{
    return filed_send_reply_payload(reply_fd, page, header, status, result, error_token, 0);
}

int filed_send_session_reply(int channel_fd, uint64_t request_id, int64_t status, uint64_t result)
{
    if (status < 0) {
        (void)filed_error_token(
            status,
            FILED_OP_SESSION_DOORBELL,
            PACHA_STATUS_STAGE_STATUS_MAP,
            status,
            request_id,
            0,
            0,
            0,
            "filed session negative reply");
    }
    const struct pacha_ipc_msg reply = {
        .word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = status < 0 ? 0 : result,
        .word3 = request_id,
    };
    return filed_ipc_send_wait(channel_fd, &reply);
}

int filed_send_exec_reply(
    int reply_fd,
    uint64_t request_id,
    int process_fd,
    int thread_fd,
    int transfer_process_fd,
    int thread_startable)
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
    uint64_t thread_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_KILL;
    if (thread_startable) {
        thread_rights |= PACHA_FD_RIGHT_START;
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
            .rights = thread_rights,
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

int filed_send_exec_self_reply(
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
                PACHA_FD_RIGHT_TRANSFER |
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


void filed_dispatch_close_owned_fd(int *fd)
{
    if (fd == NULL || *fd < 0) {
        return;
    }
    (void)pacha_fd_close(*fd);
    *fd = -1;
}
