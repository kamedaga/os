#pragma once
#include "unixd/ipc_protocol.h"
#include "pacha/ipc.h"

/* Native daemons and freestanding LPR have different syscall wrappers.
 * Share wire validation and capability ownership. All callbacks return
 * negative Linux errno on failure; page_create returns native FDs on success. */
struct unix_client_io {
    pacha_fd_t (*page_create)(struct unix_control **page);
    void (*page_destroy)(pacha_fd_t fd, struct unix_control *page);
    int (*call)(pacha_fd_t endpoint, const struct pacha_ipc_msg *request, struct pacha_ipc_msg *reply);
    void (*close)(pacha_fd_t fd);
};

struct unix_client_buffer {
    pacha_fd_t fd;
    struct unix_control *page;
    uint64_t token, session;
    pacha_fd_t endpoint;
};
/* Borrowed, exclusively held page. The caller must discard it after any
 * failed exchange: a lost reply can leave the server still using that VMO. */
int unix_client_exchange_page(const struct unix_client_io *io, pacha_fd_t endpoint,
    struct unix_control *request, const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received,
    struct unix_client_buffer *buffer);

int unix_client_exchange(const struct unix_client_io *io, pacha_fd_t endpoint,
    struct unix_control *request, const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received);

/* Native control client. The caller supplies a nonzero request ID and owns
 * endpoint/send caps. Received caps pass to the caller only on success.
 * Transport failure does not imply that the operation was rolled back;
 * retry-sensitive operations must reuse their broker operation ID. */
int unix_client_call(pacha_fd_t endpoint, struct unix_control *request,
    const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received);
