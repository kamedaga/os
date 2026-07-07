#include "termd/boot_config.h"
#include "termd/ipc_protocol.h"
#include "linux_tty_island.h"
#include "lpr_supervisor/ipc_protocol.h"

#include <pacha/abi.h>
#include <pacha/error_conveyor.h>
#include <pacha/ipc.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    TERMD_ERR_INVAL = -22,
    TERMD_ERR_NODEV = -19,
    TERMD_ERR_NOTSUP = -95,
    TERMD_ERR_AGAIN = -11,
};

static const struct termd_boot_config *g_cfg;
static struct termd_linux_tty_island g_tty_island;
static int g_signal_supervisor_endpoint_fd = -1;
static uint64_t g_signal_supervisor_request_id = 0x5445524d4c505301ull;
static pacha_errconv_store_t g_error_store;
static int g_error_store_ready;

static void *termd_map_page(int page_fd)
{
    if (page_fd < 16) {
        return 0;
    }
    return pacha_mmap(
        page_fd,
        TERMD_WIRE_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
}

static pacha_errconv_store_t *termd_errors(void)
{
    if (!g_error_store_ready) {
        pacha_errconv_store_init(&g_error_store, PACHA_ERRCONV_COMPONENT_TERMD);
        g_error_store_ready = 1;
    }
    return &g_error_store;
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
    return pacha_errconv_error_token(
        termd_errors(),
        status,
        PACHA_ERRCONV_DOMAIN_TERMD_STATUS,
        op,
        stage,
        raw_status,
        request_id,
        fd_count,
        subject,
        child_token,
        text);
}

static int termd_send_reply_with_error(
    int reply_fd,
    uint64_t request_id,
    int64_t status,
    uint64_t result,
    uint64_t error_token,
    uint64_t op,
    uint64_t stage,
    const char *fallback_text)
{
    uint64_t token = error_token;
    if (status < 0 && token == 0 && op != TERMD_WIRE_OP_ERROR_GET) {
        token = termd_error_token(
            status,
            op,
            stage != PACHA_ERRCONV_STAGE_NONE ? stage : PACHA_ERRCONV_STAGE_STATUS_MAP,
            status,
            request_id,
            0,
            0,
            0,
            fallback_text != NULL ? fallback_text : "termd negative reply without token");
    }
    struct pacha_ipc_msg reply = {
        .word0 = TERMD_WIRE_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = status < 0 ? token : result,
        .word3 = request_id,
    };
    const int reply_status = pacha_ipc_reply(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    return reply_status;
}

static int termd_send_reply(int reply_fd, uint64_t request_id, int64_t status, uint64_t result)
{
    const uint64_t token = status < 0 ?
        termd_error_token(
            status,
            0,
            PACHA_ERRCONV_STAGE_STATUS_MAP,
            status,
            request_id,
            0,
            0,
            0,
            "termd negative reply") :
        0;
    return termd_send_reply_with_error(
        reply_fd,
        request_id,
        status,
        result,
        token,
        0,
        PACHA_ERRCONV_STAGE_STATUS_MAP,
        "termd negative reply");
}

static int termd_send_boot_ready(int64_t status, uint64_t result)
{
    if (g_cfg == 0 || g_cfg->ready_channel_fd < 16) {
        return TERMD_ERR_INVAL;
    }
    const struct pacha_ipc_msg msg = {
        .word0 = TERMD_BOOT_READY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = result,
        .word3 = 0,
    };
    return pacha_ipc_send((int)(uint32_t)g_cfg->ready_channel_fd, &msg);
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

static int termd_send_tty_signal_to_supervisor(uint32_t pgrp, uint32_t signo)
{
    if (g_signal_supervisor_endpoint_fd < 16 || pgrp == 0 || signo == 0) {
        return TERMD_ERR_INVAL;
    }
    const struct pacha_ipc_msg request = {
        .word0 = LPRS_WIRE_REQUEST_MAGIC,
        .word1 = LPRS_WIRE_OP_DELIVER_TTY_SIGNAL,
        .word2 = ((uint64_t)pgrp << 32) | (uint64_t)signo,
        .word3 = ++g_signal_supervisor_request_id,
    };
    const int reply_fd = pacha_ipc_call(g_signal_supervisor_endpoint_fd, &request);
    if (reply_fd < 16) {
        return reply_fd;
    }
    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    const int recv_status = pacha_ipc_recv_wait(reply_fd, &reply, UINT64_MAX);
    (void)pacha_fd_close(reply_fd);
    if (recv_status != 0) {
        return recv_status;
    }
    if (reply.word0 != LPRS_WIRE_REPLY_MAGIC || reply.word3 != request.word3) {
        return -5;
    }
    return (int)(int64_t)reply.word1;
}

static void termd_forward_pending_tty_signals(void)
{
    termd_linux_tty_island_pump(&g_tty_island);
    if (g_signal_supervisor_endpoint_fd < 16) {
        return;
    }
    for (uint64_t i = 0; i < 8u; i++) {
        termd_wire_signal_t signal_req;
        memset(&signal_req, 0, sizeof(signal_req));
        uint64_t result = 0;
        const int status = termd_linux_tty_island_take_signal(&g_tty_island, &signal_req, &result);
        if (status != 0 || result == 0 || signal_req.signo == 0 || signal_req.pgrp_id == 0) {
            break;
        }
        const int send_status =
            termd_send_tty_signal_to_supervisor(signal_req.pgrp_id, signal_req.signo);
        if (send_status != 0) {
            fprintf(stderr,
                "[termd] supervisor tty signal delivery failed pgrp=%u signo=%u status=%d\n",
                (unsigned)signal_req.pgrp_id,
                (unsigned)signal_req.signo,
                send_status);
            fflush(stderr);
            break;
        }
    }
}

static int termd_dispatch_register_signal_supervisor(
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds,
    int reply_fd)
{
    int keep_fd = -1;
    int64_t status = 0;
    uint64_t result = 0;
    uint64_t error_token = 0;
    if (request == 0 || fds == 0 ||
        request->fd_count < 2 ||
        fds[0].fd < 16 ||
        fds[0].fd == (uint64_t)(uint32_t)reply_fd)
    {
        status = TERMD_ERR_INVAL;
        error_token = termd_error_token(
            status,
            TERMD_WIRE_OP_REGISTER_SIGNAL_SUPERVISOR,
            PACHA_ERRCONV_STAGE_VALIDATION,
            status,
            request != 0 ? request->word3 : 0,
            request != 0 ? request->fd_count : 0,
            request != 0 && request->fd_count != 0 ? fds[0].fd : 0,
            0,
            "register signal supervisor invalid fd");
    } else {
        if (g_signal_supervisor_endpoint_fd >= 16) {
            (void)pacha_fd_close(g_signal_supervisor_endpoint_fd);
        }
        g_signal_supervisor_endpoint_fd = (int)(uint32_t)fds[0].fd;
        keep_fd = g_signal_supervisor_endpoint_fd;
        result = (uint64_t)(uint32_t)g_signal_supervisor_endpoint_fd;
    }
    termd_close_received_fds(request, fds, keep_fd, reply_fd);
    return termd_send_reply_with_error(
        reply_fd,
        request != 0 ? request->word3 : 0,
        status,
        result,
        error_token,
        TERMD_WIRE_OP_REGISTER_SIGNAL_SUPERVISOR,
        status < 0 ? PACHA_ERRCONV_STAGE_VALIDATION : PACHA_ERRCONV_STAGE_NONE,
        "register signal supervisor failed");
}

static int termd_dispatch(uint64_t op, void *page, uint64_t word2, uint64_t *out_result)
{
    (void)word2;
    if (out_result == 0) {
        return TERMD_ERR_INVAL;
    }
    *out_result = 0;
    switch (op) {
    case TERMD_WIRE_OP_HELLO:
        if (!g_tty_island.ready) {
            return TERMD_ERR_NODEV;
        }
        *out_result = g_tty_island.source_count;
        return 0;
    case TERMD_WIRE_OP_OPEN_PTMX:
        if (!g_tty_island.ready || !g_tty_island.ptmx_registered) {
            return TERMD_ERR_NODEV;
        }
        return termd_linux_tty_island_open_ptmx(&g_tty_island, word2, out_result);
    case TERMD_WIRE_OP_OPEN_PTS:
        if (page == 0) {
            return TERMD_ERR_INVAL;
        }
        return termd_linux_tty_island_open_pts(
            &g_tty_island,
            (const termd_wire_open_t *)page,
            out_result);
    case TERMD_WIRE_OP_OPEN_HVC:
        if (page == 0) {
            return TERMD_ERR_INVAL;
        }
        return termd_linux_tty_island_open_hvc(
            &g_tty_island,
            (const termd_wire_open_t *)page,
            out_result);
    case TERMD_WIRE_OP_OPEN_CTTY:
        if (page == 0) {
            return TERMD_ERR_INVAL;
        }
        return termd_linux_tty_island_open_ctty(
            &g_tty_island,
            (const termd_wire_open_t *)page,
            out_result);
    case TERMD_WIRE_OP_CLOSE:
        return termd_linux_tty_island_close(&g_tty_island, word2);
    case TERMD_WIRE_OP_DUP:
        return termd_linux_tty_island_dup(&g_tty_island, word2, out_result);
    case TERMD_WIRE_OP_TAKE_SIGNAL:
        if (page == 0) {
            return TERMD_ERR_INVAL;
        }
        return termd_linux_tty_island_take_signal(
            &g_tty_island,
            (termd_wire_signal_t *)page,
            out_result);
    case TERMD_WIRE_OP_READ:
    case TERMD_WIRE_OP_WRITE:
        if (page == 0) {
            return TERMD_ERR_INVAL;
        }
        return termd_linux_tty_island_io(
            &g_tty_island,
            op == TERMD_WIRE_OP_WRITE,
            (termd_wire_io_t *)page,
            out_result);
    case TERMD_WIRE_OP_POLL:
        if (page == 0) {
            return TERMD_ERR_INVAL;
        }
        return termd_linux_tty_island_poll(&g_tty_island, (termd_wire_poll_t *)page);
    case TERMD_WIRE_OP_IOCTL:
        if (page == 0) {
            return TERMD_ERR_INVAL;
        }
        return termd_linux_tty_island_ioctl(&g_tty_island, (termd_wire_ioctl_t *)page);
    default:
        return TERMD_ERR_INVAL;
    }
}

static int termd_dispatch_request(const struct pacha_ipc_msg *request, const struct pacha_ipc_fd *fds)
{
    if (request == 0 || fds == 0 || request->fd_count == 0) {
        return -1;
    }
    const int reply_fd = (int)(uint32_t)fds[request->fd_count - 1].fd;
    if (reply_fd < 16) {
        termd_close_received_fds(request, fds, -1, -1);
        return -1;
    }
    if (request->word0 != TERMD_WIRE_REQUEST_MAGIC || request->word3 == 0) {
        termd_close_received_fds(request, fds, reply_fd, -1);
        return termd_send_reply(reply_fd, request->word3, TERMD_ERR_INVAL, 0);
    }

    if (request->word1 == TERMD_WIRE_OP_ERROR_GET) {
        if (request->fd_count < 2 || fds[0].fd < 16 ||
            fds[0].fd == (uint64_t)(uint32_t)reply_fd)
        {
            termd_close_received_fds(request, fds, reply_fd, -1);
            return termd_send_reply_with_error(
                reply_fd,
                request->word3,
                TERMD_ERR_INVAL,
                0,
                0,
                TERMD_WIRE_OP_ERROR_GET,
                PACHA_ERRCONV_STAGE_VALIDATION,
                "error get invalid fd");
        }
        void *page = termd_map_page((int)(uint32_t)fds[0].fd);
        if (page == 0) {
            termd_close_received_fds(request, fds, reply_fd, -1);
            return termd_send_reply_with_error(
                reply_fd,
                request->word3,
                -5,
                0,
                0,
                TERMD_WIRE_OP_ERROR_GET,
                PACHA_ERRCONV_STAGE_MAP_PAGE,
                "error get page map failed");
        }
        const int status =
            pacha_errconv_export(termd_errors(), request->word2, page, TERMD_WIRE_PAGE_BYTES);
        (void)pacha_munmap(page, TERMD_WIRE_PAGE_BYTES);
        termd_close_received_fds(request, fds, reply_fd, -1);
        return termd_send_reply_with_error(
            reply_fd,
            request->word3,
            status,
            0,
            0,
            TERMD_WIRE_OP_ERROR_GET,
            PACHA_ERRCONV_STAGE_ERROR_GET,
            "error get export failed");
    }

    if (request->word1 == TERMD_WIRE_OP_REGISTER_SIGNAL_SUPERVISOR) {
        return termd_dispatch_register_signal_supervisor(request, fds, reply_fd);
    }

    void *page = 0;
    int page_fd = -1;
    if (request->fd_count >= 2 && fds[0].fd >= 16 && fds[0].fd != (uint64_t)(uint32_t)reply_fd) {
        page_fd = (int)(uint32_t)fds[0].fd;
        page = termd_map_page(page_fd);
        if (page == 0) {
            const uint64_t token = termd_error_token(
                -5,
                request->word1,
                PACHA_ERRCONV_STAGE_MAP_PAGE,
                -5,
                request->word3,
                request->fd_count,
                (uint64_t)(uint32_t)page_fd,
                0,
                "request page map failed");
            termd_close_received_fds(request, fds, reply_fd, -1);
            return termd_send_reply_with_error(
                reply_fd,
                request->word3,
                -5,
                0,
                token,
                request->word1,
                PACHA_ERRCONV_STAGE_MAP_PAGE,
                "request page map failed");
        }
    }

    uint64_t result = 0;
    const int status = termd_dispatch(request->word1, page, request->word2, &result);
    if ((g_cfg->flags & TERMD_BOOT_FLAG_TRACE) != 0 ||
        (status != 0 && status != TERMD_ERR_NOTSUP && status != TERMD_ERR_AGAIN))
    {
        printf(
            "[termd] op=%llu status=%d result=%llu fds=%llu island_ready=%u\n",
            (unsigned long long)request->word1,
            status,
            (unsigned long long)result,
            (unsigned long long)request->fd_count,
            (unsigned)g_tty_island.ready);
        fflush(stdout);
    }

    if (page != 0) {
        (void)pacha_munmap(page, TERMD_WIRE_PAGE_BYTES);
    }
    termd_close_received_fds(request, fds, reply_fd, -1);
    const uint64_t token = status < 0 ?
        termd_error_token(
            status,
            request->word1,
            PACHA_ERRCONV_STAGE_DISPATCH_ENTRY,
            status,
            request->word3,
            request->fd_count,
            0,
            0,
            "termd dispatch failed") :
        0;
    return termd_send_reply_with_error(
        reply_fd,
        request->word3,
        status,
        result,
        token,
        request->word1,
        PACHA_ERRCONV_STAGE_DISPATCH_ENTRY,
        "termd dispatch failed");
}

int main(void)
{
    g_cfg = (const struct termd_boot_config *)(uintptr_t)TERMD_BOOT_CONFIG_VA;
    if (g_cfg == 0 ||
        g_cfg->magic != TERMD_BOOT_CONFIG_MAGIC ||
        g_cfg->tty_endpoint_fd < 16 ||
        g_cfg->ready_channel_fd < 16) {
        fprintf(stderr, "[termd] invalid boot config\n");
        return 1;
    }

    const int island_status = termd_linux_tty_island_init(&g_tty_island, g_cfg);
    if (island_status != 0) {
        fprintf(stderr, "[termd] kobox Linux TTY island init failed status=%d\n", island_status);
    }

    if (g_tty_island.ready) {
        printf(
            "[termd] linux tty modules ready endpoint_fd=%llu loader=%s modules=%u loaded=%u sources=%u ptmx=%s\n",
            (unsigned long long)g_cfg->tty_endpoint_fd,
            g_tty_island.loader_version,
            (unsigned)g_tty_island.configured_module_count,
            (unsigned)g_tty_island.loaded_module_count,
            (unsigned)g_tty_island.source_count,
            g_tty_island.ptmx_registered ? "registered" : "missing");
    } else {
        printf(
            "[termd] linux tty modules not ready endpoint_fd=%llu loader=%s modules=%u loaded=%u load_status=%d init_status=%d\n",
            (unsigned long long)g_cfg->tty_endpoint_fd,
            g_tty_island.loader_version,
            (unsigned)g_tty_island.configured_module_count,
            (unsigned)g_tty_island.loaded_module_count,
            (int)g_tty_island.load_status,
            (int)g_tty_island.init_status);
    }
    fflush(stdout);
    const int ready_status =
        island_status != 0 ? island_status :
        (g_tty_island.ready && g_tty_island.source_count != 0 ? 0 : TERMD_ERR_NODEV);
    const int ready_send_status = termd_send_boot_ready(ready_status, g_tty_island.source_count);
    if (ready_send_status != 0) {
        fprintf(stderr, "[termd] boot ready send failed status=%d ready_status=%d\n",
            ready_send_status,
            ready_status);
        fflush(stderr);
        return 1;
    }
    if (ready_status != 0) {
        return 1;
    }

    for (;;) {
        struct pacha_ipc_msg request;
        struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS];
        memset(&request, 0, sizeof(request));
        memset(fds, 0, sizeof(fds));
        request.fds = fds;
        request.fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS;

        int status = pacha_ipc_recv(g_cfg->tty_endpoint_fd, &request);
        if (status == 0) {
            (void)termd_dispatch_request(&request, fds);
            termd_forward_pending_tty_signals();
        } else if (status != PACHA_ERR_EMPTY && status != PACHA_ERR_NOT_READY) {
            fprintf(stderr, "[termd] recv failed status=%d\n", status);
        } else {
            termd_forward_pending_tty_signals();
        }
    }
}
