#include "ipc_wire.h"

#include "koboxd/control_protocol_v2.h"
#include "pacha/abi.h"

const uint64_t koboxd_service_channel_rights =
    PACHA_FD_RIGHT_INSPECT |
    PACHA_FD_RIGHT_DUP |
    PACHA_FD_RIGHT_WAIT |
    PACHA_FD_RIGHT_POLL |
    PACHA_FD_RIGHT_SET_FLAGS |
    PACHA_FD_RIGHT_CLOSE |
    PACHA_FD_RIGHT_SEND |
    PACHA_FD_RIGHT_RECV |
    PACHA_FD_RIGHT_CALL |
    PACHA_FD_RIGHT_TRANSFER;

int koboxd_recv_ipc_wait(int fd, struct pacha_ipc_msg *msg)
{
    if (fd < 16 || msg == NULL) {
        return -1;
    }
    for (unsigned i = 0; i < 262144; i++) {
        const int status = pacha_ipc_recv(fd, msg);
        if (status == 0) {
            return 0;
        }
        if (status != PACHA_ERR_EMPTY && status != PACHA_ERR_NOT_READY && status != -2) {
            return status;
        }
        struct pacha_pollfd pollfd = {
            .fd = fd,
            .events = PACHA_FD_EVENT_READABLE,
            .revents = 0,
        };
        (void)pacha_fd_wait_many(&pollfd, 1, 1);
    }
    return -2;
}

int koboxd_send_ipc_wait(int fd, const struct pacha_ipc_msg *msg)
{
    if (fd < 16 || msg == NULL) {
        return -1;
    }
    for (unsigned i = 0; i < 262144; i++) {
        const int status = pacha_ipc_send(fd, msg);
        if (status == 0) {
            return 0;
        }
        if (status != PACHA_ERR_EMPTY && status != PACHA_ERR_NOT_READY && status != -2) {
            return status;
        }
        struct pacha_pollfd pollfd = {
            .fd = fd,
            .events = PACHA_FD_EVENT_WRITABLE,
            .revents = 0,
        };
        (void)pacha_fd_wait_many(&pollfd, 1, 1);
    }
    return -2;
}

int koboxd_send_endpoint_fd(int control_fd, uint64_t request_id, uint64_t endpoint_kind, int client_fd)
{
    struct pacha_ipc_fd fd_item = {
        .fd = (uint64_t)(uint32_t)client_fd,
        .rights = koboxd_service_channel_rights,
        .flags = 0,
        .transfer_flags = PACHA_IPC_TRANSFER_MOVE | PACHA_IPC_TRANSFER_CLOEXEC,
    };
    struct pacha_ipc_msg reply = {
        .word0 = KOBOXD_V2_REPLY_MAGIC,
        .word1 = 0,
        .word2 = endpoint_kind,
        .word3 = request_id,
        .fds = &fd_item,
        .fd_count = 1,
    };
    return koboxd_send_ipc_wait(control_fd, &reply);
}

int koboxd_send_status_reply(int fd, uint64_t request_id, uint64_t word2)
{
    const struct pacha_ipc_msg reply = {
        .word0 = KOBOXD_V2_REPLY_MAGIC,
        .word1 = 0,
        .word2 = word2,
        .word3 = request_id,
    };
    return koboxd_send_ipc_wait(fd, &reply);
}

int koboxd_send_status_reply_ex(int fd, uint64_t request_id, int64_t status, uint64_t result)
{
    const struct pacha_ipc_msg reply = {
        .word0 = KOBOXD_V2_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = result,
        .word3 = request_id,
    };
    return koboxd_send_ipc_wait(fd, &reply);
}

void *koboxd_map_wire_vmo_from_msg(const struct pacha_ipc_msg *request, uint64_t size, int *out_fd)
{
    if (request == NULL ||
        request->fd_count < 1 ||
        request->fds == NULL ||
        request->fds[0].fd < 16 ||
        out_fd == NULL)
    {
        return NULL;
    }
    *out_fd = (int)request->fds[0].fd;
    return pacha_mmap(*out_fd, size, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
}

int koboxd_create_service_channel_pair(struct pacha_ipc_channel_pair *pair)
{
    if (pair == NULL) {
        return -1;
    }
    pair->a = -1;
    pair->b = -1;
    const int status = pacha_ipc_channel_create(pair, koboxd_service_channel_rights, 0);
    if (status != 0 || pair->a < 16 || pair->b < 16) {
        return status != 0 ? status : -1;
    }
    return 0;
}
