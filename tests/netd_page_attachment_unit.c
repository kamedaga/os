#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pacha/ipc.h"
#include "netd/ipc_protocol.h"

/* Model native descriptors separately from mappings: an mmap retains its VMO
 * after close, while the descriptor slot becomes available to IPC receive. */
static unsigned fd_kind[256];
static unsigned mappings;
static int fail_map;

static int install_fd(unsigned kind)
{
    for (int fd = 16; fd < 256; ++fd) {
        if (fd_kind[fd] != 0) continue;
        fd_kind[fd] = kind;
        return fd;
    }
    return -1;
}

static unsigned free_fds(void)
{
    unsigned count = 0;
    for (int fd = 16; fd < 256; ++fd) count += fd_kind[fd] == 0;
    return count;
}

int pacha_fd_close(int fd)
{
    assert(fd >= 16 && fd < 256 && fd_kind[fd] != 0);
    fd_kind[fd] = 0;
    return 0;
}

int pacha_fd_get_info(int fd, struct pacha_fd_info *out)
{
    assert(fd >= 16 && fd < 256 && fd_kind[fd] != 0);
    memset(out, 0, sizeof(*out));
    out->kind = fd_kind[fd];
    out->rights = PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_WAIT;
    return 0;
}

void *pacha_mmap(int fd, uint64_t size, uint64_t prot,
    uint64_t flags, uint64_t offset)
{
    assert(fd_kind[fd] == PACHA_FD_KIND_VMO);
    assert(prot == (PACHA_PROT_READ | PACHA_PROT_WRITE));
    assert(flags == PACHA_MMAP_SHARED && offset == 0);
    if (fail_map) return NULL;
    void *page = calloc(1, size);
    assert(page != NULL);
    ++mappings;
    return page;
}

int pacha_munmap(void *addr, uint64_t size)
{
    assert(addr != NULL && size == NETD_PAGE_BYTES && mappings != 0);
    --mappings;
    free(addr);
    return 0;
}

#include "../userland/netd/src/socket_service.c"

int main(void)
{
    /* At the observed desktop load, retaining 36 page descriptors leaves
     * only one free slot. A fork DUP needs two slots (lease plus reply). */
    for (unsigned i = 0; i < 167; ++i)
        assert(install_fd(PACHA_FD_KIND_CHANNEL) >= 16);
    for (unsigned i = 0; i < 36; ++i) {
        assert(free_fds() >= 3);
        const int page_fd = install_fd(PACHA_FD_KIND_VMO);
        const int lease_fd = install_fd(PACHA_FD_KIND_CHANNEL);
        const int reply_fd = install_fd(PACHA_FD_KIND_REPLY);
        uint64_t id = 0;
        assert(netd_page_attachment_add(page_fd, lease_fd, &id) == 0);
        assert(pacha_fd_close(reply_fd) == 0);
        struct netd_page_attachment *attachment = netd_page_attachment_find(id);
        assert(attachment != NULL);
        ((unsigned char *)attachment->page)[0] = (unsigned char)i;
    }
    if (free_fds() < 2) {
        fprintf(stderr, "FAIL: fork DUP cannot enter netd: free=%u required=2\n",
            free_fds());
        return 1;
    }
    assert(mappings == 36);
    assert(free_fds() == 240 - 167 - 36);
    unsigned index = 36;
    while (g_netd_page_attachments != NULL) {
        struct netd_page_attachment *attachment = g_netd_page_attachments;
        assert(((unsigned char *)attachment->page)[0] == --index);
        g_netd_page_attachments = attachment->next;
        netd_page_attachment_destroy(attachment);
        --g_netd_page_attachment_count;
    }
    assert(mappings == 0 && free_fds() == 240 - 167);

    /* A failed attach leaves both received descriptors with its caller. */
    fail_map = 1;
    const int page_fd = install_fd(PACHA_FD_KIND_VMO);
    const int lease_fd = install_fd(PACHA_FD_KIND_CHANNEL);
    uint64_t id = 0;
    assert(netd_page_attachment_add(page_fd, lease_fd, &id) == -5);
    assert(fd_kind[page_fd] == PACHA_FD_KIND_VMO);
    assert(fd_kind[lease_fd] == PACHA_FD_KIND_CHANNEL);
    assert(pacha_fd_close(page_fd) == 0);
    assert(pacha_fd_close(lease_fd) == 0);
    assert(g_netd_page_attachment_count == 0 && mappings == 0);
    puts("netd page attachment unit: PASS");
    return 0;
}
