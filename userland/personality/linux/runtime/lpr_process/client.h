#include <pacha/abi.h>
#pragma once
#include <stdint.h>
int64_t lpr_process_client_service_session(uint32_t op, uint64_t *counter,
    int64_t (*convert)(int64_t), uint64_t token,
    pacha_fd_t page_fd, void *page, uint64_t *session, pacha_fd_t *fd);


void *lpr_process_client_payload(void *page);
/* Runs once in the initial thread, before Linux code can execute. The
 * bootstrap handle is not the long-lived authenticated control channel. */
int64_t lpr_process_client_activate(uint64_t *request_counter,
    int64_t (*status_to_errno)(int64_t), pacha_fd_t bootstrap_fd, uint64_t token,
    pacha_fd_t page_fd, void *page);

/* Returns an owned private, non-CLOEXEC handoff for the next image. The
 * current control channel remains usable until successful native exec. */
int64_t lpr_process_client_prepare_exec(uint64_t *request_counter,
    int64_t (*status_to_errno)(int64_t), uint64_t token,
    pacha_fd_t page_fd, void *page, pacha_fd_t *out_bootstrap_fd);

/* Obtain this process's unixd session through its authenticated control
 * channel. Caller owns the returned PRIVATE|CLOEXEC, nontransferable FD. */
int64_t lpr_process_client_unix_session(uint64_t *request_counter,
    int64_t (*status_to_errno)(int64_t), uint64_t token,
    pacha_fd_t page_fd, void *page, uint64_t *out_session, pacha_fd_t *out_fd);

int64_t lpr_process_client_call(
    uint64_t *request_counter,
    int64_t (*status_to_errno)(int64_t status),
    uint32_t op,
    pacha_fd_t page_fd,
    void *page,
    uint32_t payload_size,
    pacha_fd_t transfer_fd,
    uint64_t *out_result);

int64_t lpr_process_client_call_with_reply_fd(
    uint64_t *request_counter,
    int64_t (*status_to_errno)(int64_t status),
    uint32_t op,
    pacha_fd_t page_fd,
    void *page,
    uint32_t payload_size,
    pacha_fd_t transfer_fd,
    uint64_t *out_result,
    pacha_fd_t *out_reply_fd);

/* Names the rights the receiver gets over transfer_fd.  Needed when the
 * receiver has to map the descriptor, which the default set deliberately
 * withholds from process capabilities. */
int64_t lpr_process_client_call_with_transfer_rights(
    uint64_t *request_counter,
    int64_t (*status_to_errno)(int64_t status),
    uint32_t op,
    pacha_fd_t page_fd,
    void *page,
    uint32_t payload_size,
    pacha_fd_t transfer_fd,
    uint64_t transfer_rights,
    uint64_t *out_result,
    pacha_fd_t *out_reply_fd);

int64_t lpr_process_client_call_token(
    uint64_t *request_counter,
    int64_t (*status_to_errno)(int64_t status),
    pacha_fd_t (*create_page)(void **out_page),
    void (*destroy_page)(pacha_fd_t fd, void *page),
    uint32_t op,
    uint64_t token,
    pacha_fd_t transfer_fd,
    uint64_t *out_result);
