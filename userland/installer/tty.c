/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "tty.h"

#include <termd/ipc_protocol.h>
#include <pacha/ipc.h>
#include <pacha/service_abi.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>

static int termd_call(struct installer_tty *tty, uint32_t op,
    size_t payload_size, int passed_fd, uint64_t *result) {
    if (!tty || !tty->page || payload_size >
        PACHA_SERVICE_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES) return -EINVAL;
    const uint64_t id = ++tty->request_id;
    pacha_service_envelope_t *header = tty->page;
    *header = (pacha_service_envelope_t){
        .magic = PACHA_SERVICE_REQUEST_MAGIC,
        .header_size = sizeof(pacha_service_envelope_t),
        .service_id = TERMD_SERVICE_ID, .op = op,
        .flags = PACHA_SERVICE_FLAG_PAGE_PAYLOAD,
        .request_id = id, .trace_id = id,
        .payload_size = payload_size};
    struct pacha_ipc_fd fds[2] = {{
        .fd = (uint64_t)tty->page_fd,
        .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE}};
    unsigned count = 1;
    if (passed_fd >= 16) {
        fds[count++] = (struct pacha_ipc_fd){
            .fd = (uint64_t)passed_fd,
            .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_TRANSFER |
                PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV |
                PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL};
    }
    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC, .word3 = id,
        .fds = fds, .fd_count = count};
    const int reply_fd = pacha_ipc_call(tty->endpoint_fd, &request);
    if (reply_fd < 16) return reply_fd < 0 ? reply_fd : -EIO;
    struct pacha_ipc_msg reply = {0};
    int status = pacha_ipc_recv_wait(reply_fd, &reply,
        PACHA_FD_WAIT_FOREVER);
    (void)pacha_fd_close(reply_fd);
    if (status) return status;
    if (reply.fd_count || reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
        reply.word3 != id || header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        header->header_size != sizeof(*header) ||
        header->service_id != TERMD_SERVICE_ID || header->op != op ||
        header->request_id != id) return -EPROTO;
    if (result) *result = header->result;
    return (int)header->status;
}

static ssize_t tty_write(void *context, const char *buffer, size_t length) {
    struct installer_tty *tty = context;
    size_t written = 0;
    while (written < length) {
        const size_t chunk = length - written > TERMD_IO_BYTES ?
            TERMD_IO_BYTES : length - written;
        termd_io_request_t *body = (void *)((uint8_t *)tty->page +
            PACHA_SERVICE_HEADER_BYTES);
        memset(body, 0, sizeof(*body));
        body->handle = tty->handle;
        body->length = chunk;
        body->flags = TERMD_IO_F_NOWAIT;
        memcpy(body->data, buffer + written, chunk);
        uint64_t sent = 0;
        int status = termd_call(tty, TERMD_OP_HANDLE_WRITE,
            sizeof(*body), -1, &sent);
        if (status || !sent || sent > chunk) {
            errno = status < 0 ? -status : EIO;
            return written ? (ssize_t)written : -1;
        }
        written += (size_t)sent;
    }
    return (ssize_t)written;
}

static int tty_ioctl(struct installer_tty *tty, uint32_t request,
    void *data, size_t bytes) {
    if (!tty || !tty->page || bytes > TERMD_IOCTL_DATA_BYTES) return -EINVAL;
    termd_ioctl_request_t *body = (void *)((uint8_t *)tty->page +
        PACHA_SERVICE_HEADER_BYTES);
    memset(body, 0, sizeof(*body));
    body->handle = tty->handle;
    body->request = request;
    if (data && bytes) memcpy(body->data, data, bytes);
    int status = termd_call(tty, TERMD_OP_HANDLE_IOCTL,
        sizeof(*body), -1, NULL);
    if (!status && data && bytes) memcpy(data, body->data, bytes);
    return status;
}

int installer_tty_get_size(struct installer_tty *tty,
    unsigned *rows, unsigned *columns) {
    enum { LINUX_TIOCGWINSZ = 0x5413u };
    if (!tty || !tty->handle || !rows || !columns) return -EINVAL;
    termd_ioctl_request_t *body = (void *)((uint8_t *)tty->page +
        PACHA_SERVICE_HEADER_BYTES);
    memset(body, 0, sizeof(*body));
    body->handle = tty->handle;
    body->request = LINUX_TIOCGWINSZ;
    int status = termd_call(tty, TERMD_OP_HANDLE_IOCTL,
        sizeof(*body), -1, NULL);
    if (status) return status;
    if (!body->result0 || !body->result1 ||
        body->result0 > 1000 || body->result1 > 1000) return -ERANGE;
    *rows = (unsigned)body->result0;
    *columns = (unsigned)body->result1;
    return 0;
}

int installer_tty_enter_ui(struct installer_tty *tty) {
    enum { LINUX_TCGETS = 0x5401u, LINUX_TCSETS = 0x5402u };
    if (!tty || tty->raw_active) return tty ? 0 : -EINVAL;
    _Static_assert(sizeof(struct termios) <= TERMD_IOCTL_DATA_BYTES,
        "termios must fit termd ioctl payload");
    int status = tty_ioctl(tty, LINUX_TCGETS,
        &tty->saved_termios, sizeof(tty->saved_termios));
    if (status) return status;
    struct termios raw = tty->saved_termios;
    raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP |
        INLCR | IGNCR | ICRNL | IXON);
    raw.c_oflag &= ~OPOST;
    raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    status = tty_ioctl(tty, LINUX_TCSETS, &raw, sizeof(raw));
    if (!status) tty->raw_active = 1;
    return status;
}

int installer_tty_leave_ui(struct installer_tty *tty) {
    enum { LINUX_TCSETS = 0x5402u };
    if (!tty) return -EINVAL;
    if (!tty->raw_active) return 0;
    int status = tty_ioctl(tty, LINUX_TCSETS,
        &tty->saved_termios, sizeof(tty->saved_termios));
    if (!status) tty->raw_active = 0;
    return status;
}

int installer_tty_read_key(void *context, uint64_t timeout_ns) {
    enum { UI_INPUT_POLL_NS = 20000000 };
    struct installer_tty *tty = context;
    if (!tty || tty->notify_fd < 16 || tty->timeout_fd < 16) return -EINVAL;
    for (;;) {
        termd_io_request_t *body = (void *)((uint8_t *)tty->page +
            PACHA_SERVICE_HEADER_BYTES);
        memset(body, 0, sizeof(*body));
        body->handle = tty->handle;
        body->length = 1;
        body->flags = TERMD_IO_F_NOWAIT;
        uint64_t received = 0;
        int status = termd_call(tty, TERMD_OP_HANDLE_READ,
            sizeof(*body), -1, &received);
        if (!status) {
            if (received != 1) return -EIO;
            return body->data[0];
        }
        if (status != -EAGAIN) return status;

        struct pacha_pollfd waits[2] = {{
            .fd = tty->notify_fd,
            .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP,
        }, {
            .fd = tty->timeout_fd,
            .events = PACHA_FD_EVENT_READABLE,
        }};
        const uint64_t timer_ns = timeout_ns == UINT64_MAX ?
            UI_INPUT_POLL_NS : (timeout_ns ? timeout_ns : 1);
        status = pacha_timerfd_settime(tty->timeout_fd, timer_ns, 0, 0);
        if (status) return status;
        long ready = pacha_fd_wait_many(waits, 2,
            PACHA_FD_WAIT_FOREVER);
        if (ready <= 0 || (waits[0].revents & PACHA_FD_EVENT_HANGUP))
            return -EIO;
        if (waits[1].revents & PACHA_FD_EVENT_READABLE) {
            uint64_t expirations = 0;
            (void)pacha_fd_read(tty->timeout_fd, &expirations,
                sizeof(expirations));
            if (timeout_ns != UINT64_MAX) return -ETIMEDOUT;
            continue;
        }
        if (waits[0].revents & PACHA_FD_EVENT_READABLE) {
            (void)pacha_timerfd_settime(tty->timeout_fd, 0, 0, 0);
            struct pacha_ipc_msg event = {0};
            if (pacha_ipc_recv(tty->notify_fd, &event)) return -EIO;
        }
    }
}

int installer_tty_open(struct installer_tty *tty, int termd_fd,
    unsigned pts_index, FILE **output) {
    if (!tty || termd_fd < 16 || !output) return -EINVAL;
    *tty = (struct installer_tty){.endpoint_fd = termd_fd,
        .page_fd = -1, .notify_fd = -1, .timeout_fd = -1};
    *output = NULL;
    const uint64_t rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    tty->page_fd = pacha_vmo_create(PACHA_SERVICE_PAGE_BYTES, rights, 0);
    if (tty->page_fd < 16) return tty->page_fd < 0 ? tty->page_fd : -EIO;
    tty->page = pacha_mmap(tty->page_fd, PACHA_SERVICE_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!tty->page) { installer_tty_close(tty); return -ENOMEM; }
    struct pacha_ipc_channel_pair pair = {.a = -1, .b = -1};
    const uint64_t channel_rights = PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL;
    int status = pacha_ipc_channel_create(&pair, channel_rights, 0);
    if (status) { installer_tty_close(tty); return status; }
    termd_open_request_t *body = (void *)((uint8_t *)tty->page +
        PACHA_SERVICE_HEADER_BYTES);
    memset(body, 0, sizeof(*body));
    body->pts_index = pts_index;
    status = termd_call(tty, TERMD_OP_OPEN_PTS, sizeof(*body), pair.b,
        &tty->handle);
    (void)pacha_fd_close(pair.b);
    if (status) {
        (void)pacha_fd_close(pair.a);
        installer_tty_close(tty);
        return status;
    }
    tty->notify_fd = pair.a;
    const cookie_io_functions_t writer = {.write = tty_write};
    *output = fopencookie(tty, "w", writer);
    if (!*output) {
        if (*output) fclose(*output);
        installer_tty_close(tty);
        return -ENOMEM;
    }
    const uint64_t timer_rights = PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WRITE |
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE;
    tty->timeout_fd = pacha_timerfd_create(0, 0, timer_rights, 0);
    if (tty->timeout_fd < 16) {
        const int failure = tty->timeout_fd < 0 ? tty->timeout_fd : -EIO;
        fclose(*output);
        *output = NULL;
        installer_tty_close(tty);
        return failure;
    }
    setvbuf(*output, NULL, _IONBF, 0);
    return 0;
}

void installer_tty_close(struct installer_tty *tty) {
    if (!tty) return;
    (void)installer_tty_leave_ui(tty);
    if (tty->handle && tty->page) {
        termd_handle_request_t *body = (void *)((uint8_t *)tty->page +
            PACHA_SERVICE_HEADER_BYTES);
        memset(body, 0, sizeof(*body));
        body->handle = tty->handle;
        (void)termd_call(tty, TERMD_OP_HANDLE_CLOSE,
            sizeof(*body), -1, NULL);
    }
    if (tty->notify_fd >= 16) (void)pacha_fd_close(tty->notify_fd);
    if (tty->timeout_fd >= 16) (void)pacha_fd_close(tty->timeout_fd);
    if (tty->page) (void)pacha_munmap(tty->page, PACHA_SERVICE_PAGE_BYTES);
    if (tty->page_fd >= 16) (void)pacha_fd_close(tty->page_fd);
    *tty = (struct installer_tty){.endpoint_fd = -1,
        .page_fd = -1, .notify_fd = -1, .timeout_fd = -1};
}
