#include "client.h"
#include "cache.h"
#include <unixd/profile.h>
#include "../lpr_filed_internal.h"

static int native_page_create(struct unix_control **out)
{
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t fd = lpr_pacha_syscall3(PACHAOS_SYSCALL_VMO_CREATE,
        UNIX_CONTROL_BYTES, rights, PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC);
    if (fd < 16) return fd ? (int)pacha_kernel_status_to_errno(fd) : -LPR_LINUX_EIO;
    const int64_t mapped = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP, (uint64_t)fd, 0,
        UNIX_CONTROL_BYTES, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)fd);
        return -LPR_LINUX_ENOMEM;
    }
    *out = (struct unix_control *)(uintptr_t)mapped;
    return (int)fd;
}

static void native_page_destroy(int fd, struct unix_control *page)
{
    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)page, UNIX_CONTROL_BYTES);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
}

static int native_call(int endpoint, const struct pacha_ipc_msg *request)
{
    /* Threads share the process session's bounded request queue. A full
     * queue has not accepted the request (or MOVE capabilities); wait for
     * writable and retry only that explicit backpressure result. */
    const int64_t fd = lpr_native_ipc_call_wait((uint64_t)(uint32_t)endpoint, request);
    return fd >= 16 ? (int)fd : fd ? (int)pacha_kernel_status_to_errno(fd) : -LPR_LINUX_EIO;
}

static int native_receive(int fd, struct pacha_ipc_msg *reply)
{
    /* Finish the same reply after a native signal cancels its waiter. */
    return (int)pacha_kernel_status_to_errno(lpr_native_ipc_recv_wait((uint64_t)(uint32_t)fd, reply));
}

static void native_close(int fd)
{
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
}

int lpr_unix_client_open(struct lpr_unix_client *out)
{
    if (!out) return -LPR_LINUX_EINVAL;
    *out = (struct lpr_unix_client){ .fd = -1 };
    lpr_linux_process_state_init();
    if (!lpr_supervisor_enabled || !lpr_supervisor_token) return -LPR_LINUX_ENOTCONN;
    void *page = NULL;
    const int page_fd = lpr_create_standalone_wire_page(&page);
    if (page_fd < 16) return page_fd;
    const uint64_t token = lpr_supervisor_token;
    const int64_t status = lpr_process_client_unix_session(&lpr_request_id, lpr_pacha_status_to_errno,
        token, page_fd, page, &out->session, &out->fd);
    lpr_destroy_standalone_wire_page(page_fd, page);
    if (status == 0) out->process_token = token;
    return (int)status;
}

void lpr_unix_client_close(struct lpr_unix_client *client)
{
    if (!client) return;
    /* PRIVATE FDs are absent after fork. A copied number may already have
     * been reused in the child, and must not be closed there. */
    if (client->process_token == lpr_supervisor_token && client->fd >= 16) {
        lpr_unix_cache_forget(client);
        native_close(client->fd);
    }
    *client = (struct lpr_unix_client){ .fd = -1 };
}

int lpr_unix_client_call(const struct lpr_unix_client *client, struct unix_control *request,
    const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received)
{
    if (received) *received = 0;
    if (!client || !client->session || !client->process_token ||
        client->process_token != lpr_supervisor_token) return -LPR_LINUX_ENOTCONN;
    static const struct unix_client_io io = {
        native_page_create, native_page_destroy, native_call, native_receive, native_close,
    };
    struct lpr_unix_cache_lease lease;
    UP_BEGIN(total, UP_RPC, request->operation, UP_TOTAL);
    UP_BEGIN(setup, UP_RPC, request->operation, UP_SETUP);
    int status = lpr_unix_cache_begin(&lease, client, 0, 0, &io);
    UP_END(setup);
    if (status) return status;
    if (lease.buffer.session != client->session || lease.buffer.endpoint != client->fd) {
        lease.buffer.token = 0;
        lease.buffer.session = client->session;
        lease.buffer.endpoint = client->fd;
    }
    status = unix_client_exchange_page(&io, client->fd, request, send, send_count,
        receive, capacity, received, &lease.buffer);
    UP_BEGIN(cleanup, UP_RPC, request->operation, UP_CLEANUP);
    lpr_unix_cache_end(&lease, status == 0);
    return status;
}

int lpr_unix_client_register_thread(const struct lpr_unix_client *client,
    uint64_t request_id, uint64_t *out_owner)
{
    if (!out_owner || !request_id) return -LPR_LINUX_EINVAL;
    *out_owner = 0;
    if (!client || !client->session || !client->process_token ||
        client->process_token != lpr_supervisor_token) return -LPR_LINUX_ENOTCONN;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER;
    const int64_t thread_fd = lpr_pacha_syscall4(PACHAOS_SYSCALL_FD_DUP,
        PACHAOS_THREAD_SELF_FD, 16, rights, PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC);
    if (thread_fd < 16) return thread_fd ? (int)pacha_kernel_status_to_errno(thread_fd) : -LPR_LINUX_EIO;
    const struct pacha_ipc_fd observed = { .fd = (uint64_t)thread_fd,
        .rights = rights & ~PACHA_FD_RIGHT_TRANSFER, .transfer_flags = PACHA_IPC_TRANSFER_PRIVATE };
    struct unix_control request = { .operation = UNIX_OP_THREAD_REGISTER, .request = request_id };
    unsigned count = 0;
    const int status = lpr_unix_client_call(client, &request, &observed, 1, NULL, 0, &count);
    native_close((int)thread_fd);
    if (status != 0) return status;
    if (!request.result || request.result == UNIX_TRANSPORT_RECOVERING) return -LPR_LINUX_EIO;
    *out_owner = request.result;
    return 0;
}
