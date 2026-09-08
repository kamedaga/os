#include "notify.h"
#include "../lpr_filed_internal.h"
#include <errno.h>

static int next_request(struct lpr_unix_notifier *notifier, uint64_t *out)
{
    if (notifier->request == UINT64_MAX) return -EOVERFLOW;
    *out = ++notifier->request;
    return 0;
}

static void close_notification(void *context, int fd)
{
    (void)context;
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
}

static int resolve_notification(void *context, uint64_t socket, uint32_t notification, int *out)
{
    struct lpr_unix_notifier *notifier = context;
    *out = -1;
    struct unix_control request = { .operation = UNIX_OP_WAIT_SYNC, .socket = socket, .argument = notification };
    int status = next_request(notifier, &request.request);
    if (status != 0) return status;
    struct pacha_ipc_fd cap = {0};
    unsigned count = 0;
    status = lpr_unix_client_call(notifier->client, &request, NULL, 0, &cap, 1, &count);
    if (status != 0) return status;
    struct pacha_fd_info info;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_SEND;
    if (count != 1 || request.result != notification || cap.fd < 16 || cap.fd >= PACHAOS_FD_TABLE_LIMIT ||
        lpr_pacha_syscall2(PACHAOS_SYSCALL_FD_GET_INFO, cap.fd, (uint64_t)(uintptr_t)&info) != 0 ||
        info.kind != PACHA_FD_KIND_CHANNEL || info.rights != rights ||
        info.flags != (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC)) {
        if (count && cap.fd >= 16) close_notification(context, (int)cap.fd);
        return -EPROTO;
    }
    *out = (int)cap.fd;
    return 0;
}

static int send_notification(void *context, int fd, uint64_t token)
{
    (void)context;
    const struct pacha_ipc_msg message = { .word0 = UNIX_NOTIFY_MAGIC,
        .word1 = (uint32_t)token, .word2 = (uint32_t)(token >> 32) };
    const int64_t status = lpr_pacha_syscall2(PACHAOS_SYSCALL_IPC_SEND,
        (uint64_t)(uint32_t)fd, (uint64_t)(uintptr_t)&message);
    /* With no transferred FDs, native ALLOC means a full message queue:
     * a wake is already pending. Other EAGAIN-like errors do not prove that. */
    if (status == PACHA_SYSCALL_ERR_ALLOC || status == -PACHA_SYSCALL_ERR_ALLOC) return -EAGAIN;
    const int error = (int)pacha_kernel_status_to_errno(status);
    return error == -EAGAIN ? -EIO : error;
}

static int relay_notification(void *context, uint64_t socket, uint32_t notification)
{
    struct lpr_unix_notifier *notifier = context;
    struct unix_control request = { .operation = UNIX_OP_WAIT_NOTIFY, .socket = socket, .argument = notification };
    int status = next_request(notifier, &request.request);
    if (status != 0) return status;
    unsigned count = 0;
    return lpr_unix_client_call(notifier->client, &request, NULL, 0, NULL, 0, &count);
}

static struct unix_notify_platform platform(struct lpr_unix_notifier *notifier)
{
    return (struct unix_notify_platform){ .context = notifier, .resolve = resolve_notification,
        .send = send_notification, .relay = relay_notification, .close = close_notification };
}

void lpr_unix_notifier_init(struct lpr_unix_notifier *notifier, const struct lpr_unix_client *client)
{
    if (!notifier) return;
    *notifier = (struct lpr_unix_notifier){ .client = client, .process_token = client ? client->process_token : 0 };
}

void lpr_unix_notifier_destroy(struct lpr_unix_notifier *notifier)
{
    if (!notifier) return;
    if (notifier->process_token && notifier->process_token == lpr_supervisor_token) {
        const struct unix_notify_platform io = platform(notifier);
        unix_notify_cache_clear(&notifier->cache, &io);
    }
    /* A fork child must not close copied numbers: private FDs were excluded. */
    *notifier = (struct lpr_unix_notifier){0};
}

int lpr_unix_notifier_signal(struct lpr_unix_notifier *notifier, uint64_t socket,
    const struct unix_tx *tx, const struct unix_rx *rx)
{
    if (!notifier || !notifier->client || !notifier->process_token ||
        notifier->process_token != lpr_supervisor_token ||
        notifier->client->process_token != notifier->process_token) return -ENOTCONN;
    const struct unix_notify_platform io = platform(notifier);
    return unix_notify_direction(&notifier->cache, &io, socket, tx, rx);
}
