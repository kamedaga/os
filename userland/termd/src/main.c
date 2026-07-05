#include "termd/boot_config.h"
#include "termd/ipc_protocol.h"
#include "linux_tty_island.h"

#include <pacha/abi.h>
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

static int termd_send_reply(int reply_fd, uint64_t request_id, int64_t status, uint64_t result)
{
    struct pacha_ipc_msg reply = {
        .word0 = TERMD_WIRE_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = result,
        .word3 = request_id,
    };
    const int reply_status = pacha_ipc_reply(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    return reply_status;
}

static void termd_close_received_fds(
    const struct pacha_ipc_msg *request,
    const struct pacha_ipc_fd *fds,
    int keep_fd)
{
    if (request == 0 || fds == 0) {
        return;
    }
    for (uint64_t i = 0; i < request->fd_count; i++) {
        const int fd = (int)(uint32_t)fds[i].fd;
        if (fd >= 16 && fd != keep_fd) {
            (void)pacha_fd_close(fd);
        }
    }
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
    case TERMD_WIRE_OP_CLOSE:
        return termd_linux_tty_island_close(&g_tty_island, word2);
    case TERMD_WIRE_OP_DUP:
        return termd_linux_tty_island_dup(&g_tty_island, word2, out_result);
    case TERMD_WIRE_OP_TAKE_SIGNAL:
        return TERMD_ERR_NOTSUP;
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
        termd_close_received_fds(request, fds, -1);
        return -1;
    }
    if (request->word0 != TERMD_WIRE_REQUEST_MAGIC || request->word3 == 0) {
        termd_close_received_fds(request, fds, reply_fd);
        return termd_send_reply(reply_fd, request->word3, TERMD_ERR_INVAL, 0);
    }

    void *page = 0;
    int page_fd = -1;
    if (request->fd_count >= 2 && fds[0].fd >= 16 && fds[0].fd != (uint64_t)(uint32_t)reply_fd) {
        page_fd = (int)(uint32_t)fds[0].fd;
        page = termd_map_page(page_fd);
        if (page == 0) {
            termd_close_received_fds(request, fds, reply_fd);
            return termd_send_reply(reply_fd, request->word3, -5, 0);
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
    termd_close_received_fds(request, fds, reply_fd);
    return termd_send_reply(reply_fd, request->word3, status, result);
}

int main(void)
{
    g_cfg = (const struct termd_boot_config *)(uintptr_t)TERMD_BOOT_CONFIG_VA;
    if (g_cfg == 0 || g_cfg->magic != TERMD_BOOT_CONFIG_MAGIC || g_cfg->tty_endpoint_fd < 16) {
        fprintf(stderr, "[termd] invalid boot config\n");
        return 1;
    }

    const int island_status = termd_linux_tty_island_init(&g_tty_island, g_cfg);
    if (island_status != 0) {
        fprintf(stderr, "[termd] kobox Linux TTY island init failed status=%d\n", island_status);
        return 1;
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
        } else if (status != PACHA_ERR_EMPTY && status != PACHA_ERR_NOT_READY) {
            fprintf(stderr, "[termd] recv failed status=%d\n", status);
        }
    }
}
