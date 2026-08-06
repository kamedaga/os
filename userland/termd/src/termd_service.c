#include "termd_service.h"

#include "lpr_supervisor/ipc_protocol.h"
#include "termd/ipc_protocol.h"

#include <pacha/abi.h>
#include <pacha/service_abi.h>
#include <pacha/status.h>
#include <pacha/trace.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void *termd_map_page(int page_fd)
{
    if (page_fd < 16) {
        return 0;
    }
    return pacha_mmap(
        page_fd,
        TERMD_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
}

static uint64_t termd_error_token(
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
        PACHA_TRACE_COMPONENT_TERMD,
        PACHA_TRACE_EVENT_GENERIC_ERROR,
        PACHA_TRACE_CLASS_ERROR,
        op,
        stage,
        (uint64_t)status,
        (uint64_t)raw_status,
        request_id,
        fd_count);
    pacha_trace4(
        PACHA_TRACE_COMPONENT_TERMD,
        PACHA_TRACE_EVENT_GENERIC_ERROR,
        PACHA_TRACE_CLASS_ERROR,
        subject,
        child_token,
        text != NULL ? pacha_trace_name_id(text) : 0,
        0);
    return 0;
}

static int termd_send_reply_with_error(
    int reply_fd,
    void *page,
    const pacha_service_envelope_t *request_header,
    uint64_t request_id,
    int64_t status,
    uint64_t result,
    uint64_t error_token,
    uint64_t op,
    uint64_t stage,
    const char *fallback_text)
{
    (void)error_token;
    if (status < 0 && op != TERMD_OP_DIAG_ERROR_GET) {
        (void)termd_error_token(
            status,
            op,
            stage != PACHA_STATUS_STAGE_NONE ? stage : PACHA_STATUS_STAGE_STATUS_MAP,
            status,
            request_id,
            0,
            0,
            0,
            fallback_text != NULL ? fallback_text : "termd negative reply without token");
    }
    if (page != NULL) {
        pacha_service_reply_init(
            (pacha_service_envelope_t *)page,
            request_header,
            status,
            PACHA_SERVICE_ERROR_TERMD_TTY,
            status < 0 ? 0 : result,
            0);
    }
    struct pacha_ipc_msg reply = {
        .word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = status < 0 ? 0 : result,
        .word3 = request_id,
    };
    const int reply_status = pacha_ipc_reply(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    return reply_status;
}

static int termd_send_reply(int reply_fd, uint64_t request_id, int64_t status, uint64_t result)
{
    if (status < 0) {
        (void)termd_error_token(
            status,
            0,
            PACHA_STATUS_STAGE_STATUS_MAP,
            status,
            request_id,
            0,
            0,
            0,
            "termd negative reply");
    }
    return termd_send_reply_with_error(
        reply_fd,
        NULL,
        NULL,
        request_id,
        status,
        result,
        0,
        0,
        PACHA_STATUS_STAGE_STATUS_MAP,
        "termd negative reply");
}

static void termd_close_received_fds(
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds,
    int keep_fd,
    int keep_fd2)
{
    if (request == 0 || fds == 0) {
        return;
    }
    for (uint64_t i = 0; i < request->fd_count; i++) {
        const int fd = (int)(uint32_t)fds[i].fd;
        if (fd >= 16 && fd != keep_fd && fd != keep_fd2) {
            (void)pacha_fd_close(fd);
        }
    }
}

static int termd_send_tty_signal_to_supervisor(termd_service_t *service, uint32_t pgrp, uint32_t signo)
{
    if (service == NULL ||
        service->signal_supervisor_endpoint_fd < 16 ||
        pgrp == 0 ||
        signo == 0)
    {
        return TERMD_ERR_INVAL;
    }
    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int page_fd = pacha_vmo_create(PACHA_SERVICE_PAGE_BYTES, rights, 0);
    if (page_fd < 16) {
        return page_fd;
    }
    void *page = pacha_mmap(
        page_fd,
        PACHA_SERVICE_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (page == NULL) {
        (void)pacha_fd_close(page_fd);
        return -5;
    }
    const uint64_t request_id = ++service->signal_supervisor_request_id;
    pacha_service_envelope_t *header = (pacha_service_envelope_t *)page;
    memset(header, 0, sizeof(*header));
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = LPRS_SERVICE_ID;
    header->op = LPRS_OP_SIGNAL_DELIVER_TTY;
    header->flags = PACHA_SERVICE_FLAG_PAGE_PAYLOAD;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = sizeof(lprs_tty_signal_t);
    lprs_tty_signal_t *payload =
        (lprs_tty_signal_t *)((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES);
    memset(payload, 0, sizeof(*payload));
    payload->pgrp = pgrp;
    payload->signal = signo;

    struct pacha_ipc_fd fd;
    memset(&fd, 0, sizeof(fd));
    fd.fd = (uint64_t)(uint32_t)page_fd;
    fd.rights = rights;
    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = 0,
        .word2 = 0,
        .word3 = request_id,
        .fds = &fd,
        .fd_count = 1,
    };
    const int reply_fd = pacha_ipc_call(service->signal_supervisor_endpoint_fd, &request);
    if (reply_fd < 16) {
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
        (void)pacha_fd_close(page_fd);
        return reply_fd;
    }
    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    const int recv_status = pacha_ipc_recv_wait(reply_fd, &reply, UINT64_MAX);
    (void)pacha_fd_close(reply_fd);
    if (recv_status != 0) {
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
        (void)pacha_fd_close(page_fd);
        return recv_status;
    }
    const pacha_service_envelope_t *reply_header =
        (const pacha_service_envelope_t *)page;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
        reply.word3 != request.word3 ||
        reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        reply_header->request_id != request.word3)
    {
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
        (void)pacha_fd_close(page_fd);
        return -5;
    }
    const int status = (int)(int64_t)reply.word1;
    (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return status;
}

static int termd_dispatch_register_signal_supervisor(
    termd_service_t *service,
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds,
    const pacha_service_envelope_t *header,
    void *page,
    int reply_fd)
{
    int keep_fd = -1;
    int64_t status = 0;
    uint64_t result = 0;
    uint64_t error_token = 0;
    if (service == NULL ||
        request == 0 ||
        fds == 0 ||
        request->fd_count < 3 ||
        fds[1].fd < 16 ||
        fds[1].fd == (uint64_t)(uint32_t)reply_fd)
    {
        status = TERMD_ERR_INVAL;
        error_token = termd_error_token(
            status,
            TERMD_OP_SIGNAL_REGISTER_SUPERVISOR,
            PACHA_STATUS_STAGE_VALIDATION,
            status,
            header != NULL ? header->request_id : 0,
            request != 0 ? request->fd_count : 0,
            request != 0 && request->fd_count > 1 ? fds[1].fd : 0,
            0,
            "register signal supervisor invalid fd");
    } else {
        if (service->signal_supervisor_endpoint_fd >= 16) {
            (void)pacha_fd_close(service->signal_supervisor_endpoint_fd);
        }
        service->signal_supervisor_endpoint_fd = (int)(uint32_t)fds[1].fd;
        keep_fd = service->signal_supervisor_endpoint_fd;
        result = (uint64_t)(uint32_t)service->signal_supervisor_endpoint_fd;
    }
    termd_close_received_fds(request, fds, keep_fd, reply_fd);
    return termd_send_reply_with_error(
        reply_fd,
        page,
        header,
        header != NULL ? header->request_id : 0,
        status,
        result,
        error_token,
        TERMD_OP_SIGNAL_REGISTER_SUPERVISOR,
        status < 0 ? PACHA_STATUS_STAGE_VALIDATION : PACHA_STATUS_STAGE_NONE,
        "register signal supervisor failed");
}

static int termd_dispatch_tty(
    termd_service_t *service,
    uint64_t op,
    void *payload,
    int notify_fd,
    uint64_t *out_result)
{
    if (service == NULL || service->tty == NULL || out_result == 0) {
        return TERMD_ERR_INVAL;
    }
    struct termd_linux_tty_island *tty = service->tty;
    *out_result = 0;
    switch (op) {
    case TERMD_OP_HELLO:
        if (!tty->ready) {
            return TERMD_ERR_NODEV;
        }
        *out_result = tty->source_count;
        return 0;
    case TERMD_OP_OPEN_PTMX:
        if (!tty->ready || !tty->ptmx_registered) {
            return TERMD_ERR_NODEV;
        }
        if (payload == 0) {
            return TERMD_ERR_INVAL;
        }
        return termd_linux_tty_island_open_ptmx(
            tty,
            ((const termd_open_request_t *)payload)->flags,
            notify_fd,
            out_result);
    case TERMD_OP_OPEN_PTS:
        if (payload == 0) {
            return TERMD_ERR_INVAL;
        }
        return termd_linux_tty_island_open_pts(
            tty,
            (const termd_open_request_t *)payload,
            notify_fd,
            out_result);
    case TERMD_OP_OPEN_HVC:
        if (payload == 0) {
            return TERMD_ERR_INVAL;
        }
        return termd_linux_tty_island_open_hvc(
            tty,
            (const termd_open_request_t *)payload,
            notify_fd,
            out_result);
    case TERMD_OP_OPEN_CTTY:
        if (payload == 0) {
            return TERMD_ERR_INVAL;
        }
        return termd_linux_tty_island_open_ctty(
            tty,
            (const termd_open_request_t *)payload,
            notify_fd,
            out_result);
    case TERMD_OP_HANDLE_CLOSE:
        if (payload == 0) {
            return TERMD_ERR_INVAL;
        }
        return termd_linux_tty_island_close(
            tty,
            ((const termd_handle_request_t *)payload)->handle);
    case TERMD_OP_HANDLE_DUP:
        if (payload == 0) {
            return TERMD_ERR_INVAL;
        }
        return notify_fd >= 16 ?
            termd_linux_tty_island_transfer_dup(
                tty,
                ((const termd_handle_request_t *)payload)->handle,
                notify_fd,
                out_result) :
            termd_linux_tty_island_dup(
                tty,
                ((const termd_handle_request_t *)payload)->handle,
                out_result);
    case TERMD_OP_SIGNAL_TAKE:
        if (payload == 0) {
            return TERMD_ERR_INVAL;
        }
        return termd_linux_tty_island_take_signal(
            tty,
            (termd_signal_request_t *)payload,
            out_result);
    case TERMD_OP_HANDLE_READ:
    case TERMD_OP_HANDLE_WRITE:
        if (payload == 0) {
            return TERMD_ERR_INVAL;
        }
        return termd_linux_tty_island_io(
            tty,
            op == TERMD_OP_HANDLE_WRITE,
            (termd_io_request_t *)payload,
            out_result);
    case TERMD_OP_HANDLE_POLL:
        if (payload == 0) {
            return TERMD_ERR_INVAL;
        }
        return termd_linux_tty_island_poll(tty, (termd_poll_request_t *)payload);
    case TERMD_OP_HANDLE_IOCTL:
        if (payload == 0) {
            return TERMD_ERR_INVAL;
        }
        return termd_linux_tty_island_ioctl(tty, (termd_ioctl_request_t *)payload);
    default:
        return TERMD_ERR_INVAL;
    }
}

void termd_service_init(
    termd_service_t *service,
    const struct termd_boot_config *cfg,
    struct termd_linux_tty_island *tty)
{
    if (service == NULL) {
        return;
    }
    memset(service, 0, sizeof(*service));
    service->cfg = cfg;
    service->tty = tty;
    service->signal_supervisor_endpoint_fd = -1;
    service->signal_supervisor_request_id = 0x5445524d4c505301ull;
}

int termd_service_send_boot_ready(termd_service_t *service, int64_t status, uint64_t result)
{
    if (service == NULL || service->cfg == 0 || service->cfg->ready_channel_fd < 16) {
        return TERMD_ERR_INVAL;
    }
    const struct pacha_ipc_msg msg = {
        .word0 = TERMD_BOOT_READY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = result,
        .word3 = 0,
    };
    return pacha_ipc_send((int)(uint32_t)service->cfg->ready_channel_fd, &msg);
}

void termd_service_forward_pending_tty_signals(termd_service_t *service)
{
    if (service == NULL || service->tty == NULL) {
        return;
    }
    termd_linux_tty_island_pump(service->tty);
    if (service->signal_supervisor_endpoint_fd < 16) {
        return;
    }
    for (uint64_t i = 0; i < 8u; i++) {
        termd_signal_request_t signal_req;
        memset(&signal_req, 0, sizeof(signal_req));
        uint64_t result = 0;
        const int status = termd_linux_tty_island_take_signal(service->tty, &signal_req, &result);
        if (status != 0 || result == 0 || signal_req.signo == 0 || signal_req.pgrp_id == 0) {
            break;
        }
        const int send_status =
            termd_send_tty_signal_to_supervisor(service, signal_req.pgrp_id, signal_req.signo);
        if (send_status != 0) {
            pacha_trace3(PACHA_TRACE_COMPONENT_TERMD, PACHA_TRACE_EVENT_TERMD_TTY_STATE, PACHA_TRACE_CLASS_ERROR, signal_req.pgrp_id, signal_req.signo, (uint64_t)send_status);
            break;
        }
    }
}

int termd_service_dispatch_request(
    termd_service_t *service,
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds)
{
    if (service == NULL || request == 0 || fds == 0 || request->fd_count == 0) {
        return -1;
    }
    const int reply_fd = (int)(uint32_t)fds[request->fd_count - 1].fd;
    if (reply_fd < 16) {
        termd_close_received_fds(request, fds, -1, -1);
        return -1;
    }
    if (request->word0 != PACHA_SERVICE_REQUEST_MAGIC ||
        request->word3 == 0 ||
        request->fd_count < 2 ||
        fds[0].fd < 16 ||
        fds[0].fd == (uint64_t)(uint32_t)reply_fd)
    {
        termd_close_received_fds(request, fds, reply_fd, -1);
        return termd_send_reply(reply_fd, request->word3, TERMD_ERR_INVAL, 0);
    }

    const int page_fd = (int)(uint32_t)fds[0].fd;
    void *page = termd_map_page(page_fd);
    if (page == 0) {
        const uint64_t token = termd_error_token(
            -5,
            0,
            PACHA_STATUS_STAGE_MAP_PAGE,
            -5,
            request->word3,
            request->fd_count,
            (uint64_t)(uint32_t)page_fd,
            0,
            "request page map failed");
        termd_close_received_fds(request, fds, reply_fd, -1);
        return termd_send_reply_with_error(
            reply_fd,
            NULL,
            NULL,
            request->word3,
            -5,
            0,
            token,
            0,
            PACHA_STATUS_STAGE_MAP_PAGE,
            "request page map failed");
    }

    pacha_service_envelope_t header;
    memcpy(&header, page, sizeof(header));
    if (!pacha_service_request_is_valid(&header, TERMD_SERVICE_ID) ||
        header.request_id == 0 ||
        header.request_id != request->word3)
    {
        pacha_service_reply_init(
            (pacha_service_envelope_t *)page,
            &header,
            TERMD_ERR_INVAL,
            PACHA_SERVICE_ERROR_ABI,
            0,
            0);
        (void)pacha_munmap(page, TERMD_PAGE_BYTES);
        termd_close_received_fds(request, fds, reply_fd, -1);
        return termd_send_reply(reply_fd, request->word3, TERMD_ERR_INVAL, 0);
    }

    void *payload = (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES;
    if (header.op == TERMD_OP_DIAG_ERROR_GET) {
        const int status = PACHA_STATUS_ENOTSUP;
        termd_close_received_fds(request, fds, reply_fd, -1);
        const int reply_status = termd_send_reply_with_error(
            reply_fd,
            page,
            &header,
            header.request_id,
            status,
            0,
            0,
            TERMD_OP_DIAG_ERROR_GET,
            PACHA_STATUS_STAGE_DIAGNOSTIC,
            "error get export failed");
        (void)pacha_munmap(page, TERMD_PAGE_BYTES);
        return reply_status;
    }

    if (header.op == TERMD_OP_SIGNAL_REGISTER_SUPERVISOR) {
        const int reply_status =
            termd_dispatch_register_signal_supervisor(service, request, fds, &header, page, reply_fd);
        (void)pacha_munmap(page, TERMD_PAGE_BYTES);
        return reply_status;
    }

    const int open_op = header.op == TERMD_OP_OPEN_PTMX ||
        header.op == TERMD_OP_OPEN_PTS || header.op == TERMD_OP_OPEN_HVC ||
        header.op == TERMD_OP_OPEN_CTTY;
    const int retains_attachment = open_op || header.op == TERMD_OP_HANDLE_DUP;
    const int notify_fd = retains_attachment && request->fd_count == 3 && fds[1].fd >= 16 ?
        (int)(uint32_t)fds[1].fd : -1;
    uint64_t result = 0;
    const int status = termd_dispatch_tty(
        service, header.op, payload, notify_fd, &result);
    if ((service->cfg->flags & TERMD_BOOT_FLAG_TRACE) != 0 ||
        (status != 0 && status != TERMD_ERR_NOTSUP && status != TERMD_ERR_AGAIN))
    {
        printf(
            "[termd] op=%llu status=%d result=%llu fds=%llu island_ready=%u\n",
            (unsigned long long)header.op,
            status,
            (unsigned long long)result,
            (unsigned long long)request->fd_count,
            (unsigned)(service->tty != NULL && service->tty->ready));
        fflush(stdout);
    }

    termd_close_received_fds(
        request, fds, reply_fd,
        status == 0 && retains_attachment ? notify_fd : -1);
    const uint64_t token = status < 0 ?
        termd_error_token(
            status,
            header.op,
            PACHA_STATUS_STAGE_DISPATCH,
            status,
            header.request_id,
            request->fd_count,
            0,
            0,
            "termd dispatch failed") :
        0;
    const int reply_status = termd_send_reply_with_error(
        reply_fd,
        page,
        &header,
        header.request_id,
        status,
        result,
        token,
        header.op,
        PACHA_STATUS_STAGE_DISPATCH,
        "termd dispatch failed");
    (void)pacha_munmap(page, TERMD_PAGE_BYTES);
    return reply_status;
}
