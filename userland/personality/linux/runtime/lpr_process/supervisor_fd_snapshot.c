#include "supervisor_fd_snapshot.h"

#include "client.h"
#include "../support/string.h"

#include <pacha/service_abi.h>
#include <personality/linux_lpr.h>

static int64_t lpr_snapshot_call(
    const lpr_supervisor_fd_snapshot_ops_t *ops,
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size)
{
    return lpr_process_client_call(
        ops->request_counter,
        ops->status_to_errno,
        op,
        page_fd,
        page,
        payload_size,
        -1,
        0);
}

static int64_t lpr_snapshot_call_token(
    const lpr_supervisor_fd_snapshot_ops_t *ops,
    uint32_t op,
    uint64_t token)
{
    return lpr_process_client_call_token(
        ops->request_counter,
        ops->status_to_errno,
        ops->create_page,
        ops->destroy_page,
        op,
        token,
        -1,
        0);
}

int lpr_supervisor_fd_snapshot_replace(
    uint64_t token,
    const lpr_supervisor_fd_snapshot_ops_t *ops,
    void *ctx)
{
    if (token == 0 || ops == 0 || ops->count_fds == 0 || ops->next_fd == 0) {
        return -LPR_LINUX_EINVAL;
    }
    uint64_t count = 0;
    int status = ops->count_fds(ctx, &count);
    if (status != 0) {
        return status;
    }
    int64_t call_status = lpr_snapshot_call_token(ops, LPRS_V2_OP_FD_TABLE_REPLACE_BEGIN, token);
    if (call_status != 0) {
        return (int)call_status;
    }

    uint64_t emitted = 0;
    uint64_t cursor = 0;
    void *page = 0;
    int page_fd = -1;
    lprs_v2_fd_table_page_t *table = 0;
    while (emitted < count) {
        if (page == 0) {
            page_fd = ops->create_page(&page);
            if (page_fd < 0) {
                return page_fd;
            }
            lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
            table = (lprs_v2_fd_table_page_t *)lpr_process_client_payload(page);
            table->token = token;
            table->start_index = emitted;
            table->total_count = count;
        }
        int has = 0;
        status = ops->next_fd(ctx, &cursor, &table->entries[table->count], &has);
        if (status != 0 || !has) {
            ops->destroy_page(page_fd, page);
            return status != 0 ? status : -LPR_LINUX_EIO;
        }
        table->count++;
        emitted++;
        if (table->count == LPRS_V2_FD_TABLE_PAGE_MAX || emitted == count) {
            call_status = lpr_snapshot_call(
                ops,
                LPRS_V2_OP_FD_TABLE_REPLACE_CHUNK,
                page_fd,
                page,
                (uint32_t)(sizeof(*table) + table->count * sizeof(table->entries[0])));
            ops->destroy_page(page_fd, page);
            page = 0;
            page_fd = -1;
            table = 0;
            if (call_status != 0) {
                return (int)call_status;
            }
        }
    }
    call_status = lpr_snapshot_call_token(ops, LPRS_V2_OP_FD_TABLE_REPLACE_COMMIT, token);
    return call_status == 0 ? 0 : (int)call_status;
}

int lpr_supervisor_fd_snapshot_restore(
    uint64_t token,
    const lpr_supervisor_fd_snapshot_ops_t *ops,
    void *ctx)
{
    if (token == 0 || ops == 0 || ops->install_fd == 0) {
        return -LPR_LINUX_EINVAL;
    }
    uint64_t start = 0;
    for (;;) {
        void *page = 0;
        const int page_fd = ops->create_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
        lprs_v2_fd_table_page_t *table = (lprs_v2_fd_table_page_t *)lpr_process_client_payload(page);
        table->token = token;
        table->start_index = start;
        const int64_t call_status = lpr_snapshot_call(
            ops,
            LPRS_V2_OP_FD_TABLE_GET_CHUNK,
            page_fd,
            page,
            sizeof(*table));
        if (call_status != 0) {
            ops->destroy_page(page_fd, page);
            return (int)call_status;
        }
        if (table->count > LPRS_V2_FD_TABLE_PAGE_MAX ||
            start > table->total_count ||
            table->count > table->total_count - start)
        {
            ops->destroy_page(page_fd, page);
            return -LPR_LINUX_EIO;
        }
        for (uint64_t i = 0; i < table->count; i += 1) {
            const int status = ops->install_fd(ctx, &table->entries[i]);
            if (status != 0) {
                ops->destroy_page(page_fd, page);
                return status;
            }
        }
        const uint64_t got = table->count;
        start += got;
        const uint64_t total = table->total_count;
        ops->destroy_page(page_fd, page);
        if (start >= total) {
            return 0;
        }
        if (got == 0) {
            return -LPR_LINUX_EIO;
        }
    }
}
