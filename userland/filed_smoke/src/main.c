#include "filed/ipc_protocol.h"
#include "filed_smoke/bootstrap.h"
#include "pacha/abi.h"
#include "pacha/ipc.h"
#include "pacha/syscall.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    FILED_SMOKE_PAGE_SIZE = FILED_WIRE_PAGE_BYTES,
    FILED_SMOKE_MAX_READ = 64,
};

static int find_bootstrap_fd(char **argv, int *out_fd)
{
    if (argv == NULL || out_fd == NULL) {
        return -1;
    }

    *out_fd = -1;
    char **p = argv;
    while (*p != NULL) {
        ++p;
    }
    ++p;
    while (*p != NULL) {
        ++p;
    }
    ++p;

    const uint64_t *auxv = (const uint64_t *)(const void *)p;
    for (unsigned int i = 0; i < 64; ++i) {
        const uint64_t type = auxv[i * 2u];
        const uint64_t value = auxv[i * 2u + 1u];
        if (type == 0) {
            break;
        }
        if (type == PACHA_AT_BOOTSTRAP_FD) {
            if (value < 16) {
                return -2;
            }
            *out_fd = (int)value;
            return 0;
        }
    }

    return -2;
}

static int recv_ipc_wait(int fd, struct pacha_ipc_msg *msg)
{
    if (fd < 16 || msg == NULL) {
        return -1;
    }

    for (unsigned int i = 0; i < 262144; ++i) {
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

static int create_wire_page(int *out_fd, void **out_page)
{
    if (out_fd == NULL || out_page == NULL) {
        return -1;
    }
    *out_fd = -1;
    *out_page = NULL;

    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int fd = pacha_vmo_create(FILED_SMOKE_PAGE_SIZE, rights, 0);
    if (fd < 16) {
        return fd;
    }
    void *page = pacha_mmap(
        fd,
        FILED_SMOKE_PAGE_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (page == NULL) {
        (void)pacha_fd_close(fd);
        return -2;
    }
    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    *out_fd = fd;
    *out_page = page;
    return 0;
}

static void destroy_wire_page(int fd, void *page)
{
    if (page != NULL) {
        (void)pacha_munmap(page, FILED_SMOKE_PAGE_SIZE);
    }
    if (fd >= 16) {
        (void)pacha_fd_close(fd);
    }
}

static int filed_call(
    int endpoint_fd,
    uint64_t op,
    uint64_t request_id,
    int transfer_fd,
    uint64_t word2,
    uint64_t *out_result)
{
    if (endpoint_fd < 16 || request_id == 0 || out_result == NULL) {
        return -1;
    }

    struct pacha_ipc_fd fd_item;
    memset(&fd_item, 0, sizeof(fd_item));
    if (transfer_fd >= 16) {
        fd_item.fd = (uint64_t)(uint32_t)transfer_fd;
        fd_item.rights =
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE;
        fd_item.flags = 0;
        fd_item.transfer_flags = 0;
    }

    const struct pacha_ipc_msg request = {
        .word0 = FILED_WIRE_REQUEST_MAGIC,
        .word1 = op,
        .word2 = word2,
        .word3 = request_id,
        .fds = transfer_fd >= 16 ? &fd_item : NULL,
        .fd_count = transfer_fd >= 16 ? 1 : 0,
    };
    const int reply_fd = pacha_ipc_call(endpoint_fd, &request);
    if (reply_fd < 16) {
        return reply_fd;
    }

    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    const int recv_status = recv_ipc_wait(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    if (recv_status != 0) {
        return recv_status;
    }
    if (reply.word0 != FILED_WIRE_REPLY_MAGIC || reply.word3 != request_id) {
        return -2;
    }
    if ((int64_t)reply.word1 < 0) {
        return (int)(int64_t)reply.word1;
    }
    *out_result = reply.word2;
    return 0;
}

static int smoke_filed_endpoint(int endpoint_fd)
{
    int page_fd = -1;
    void *page = NULL;
    uint64_t result = 0;
    int status = create_wire_page(&page_fd, &page);
    if (status != 0) {
        return status;
    }

    filed_wire_openat_t *openat = (filed_wire_openat_t *)page;
    openat->dir_handle = 0;
    openat->rights =
        FILED_WIRE_RIGHT_READ |
        FILED_WIRE_RIGHT_STAT |
        FILED_WIRE_RIGHT_EXEC;
    openat->open_flags = 0;
    snprintf(openat->name, sizeof(openat->name), "/sbin/./filed.elf");

    status = filed_call(endpoint_fd, FILED_WIRE_OP_OPENAT, 1, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        destroy_wire_page(page_fd, page);
        fprintf(stderr, "[filed-smoke] openat failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        return status != 0 ? status : -3;
    }
    const uint64_t handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    filed_wire_statx_t *stat = (filed_wire_statx_t *)page;
    stat->handle = handle;
    result = 0;
    status = filed_call(endpoint_fd, FILED_WIRE_OP_STAT, 2, page_fd, 0, &result);
    if (status != 0 || stat->size == 0 || stat->kind != 0100000u) {
        fprintf(stderr,
            "[filed-smoke] stat failed status=%d size=%llu kind=0%llo\n",
            status,
            (unsigned long long)stat->size,
            (unsigned long long)stat->kind);
        (void)filed_call(endpoint_fd, FILED_WIRE_OP_CLOSE, 4, -1, handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -4;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    filed_wire_handle_flags_t *flags = (filed_wire_handle_flags_t *)page;
    flags->handle = handle;
    result = 0;
    status = filed_call(endpoint_fd, FILED_WIRE_OP_GET_FLAGS, 3, page_fd, 0, &result);
    if (status != 0 || flags->fd_flags != 0 || flags->status_flags != 0) {
        fprintf(stderr,
            "[filed-smoke] get_flags failed status=%d fd=0x%llx status=0x%llx\n",
            status,
            (unsigned long long)flags->fd_flags,
            (unsigned long long)flags->status_flags);
        (void)filed_call(endpoint_fd, FILED_WIRE_OP_CLOSE, 8, -1, handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -6;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    flags = (filed_wire_handle_flags_t *)page;
    flags->handle = handle;
    flags->fd_flags = 0;
    flags->status_flags = FILED_WIRE_FILE_NONBLOCK;
    result = 0;
    status = filed_call(endpoint_fd, FILED_WIRE_OP_SET_FLAGS, 4, page_fd, 0, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] set_flags failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_WIRE_OP_CLOSE, 8, -1, handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    flags = (filed_wire_handle_flags_t *)page;
    flags->handle = handle;
    flags->fd_flags = FILED_WIRE_FD_CLOEXEC;
    result = 0;
    status = filed_call(endpoint_fd, FILED_WIRE_OP_DUP, 5, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] dup failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_WIRE_OP_CLOSE, 8, -1, handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -7;
    }
    const uint64_t dup_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    flags = (filed_wire_handle_flags_t *)page;
    flags->handle = dup_handle;
    result = 0;
    status = filed_call(endpoint_fd, FILED_WIRE_OP_GET_FLAGS, 6, page_fd, 0, &result);
    if (status != 0 ||
        flags->fd_flags != FILED_WIRE_FD_CLOEXEC ||
        flags->status_flags != FILED_WIRE_FILE_NONBLOCK)
    {
        fprintf(stderr,
            "[filed-smoke] dup flags failed status=%d fd=0x%llx status=0x%llx\n",
            status,
            (unsigned long long)flags->fd_flags,
            (unsigned long long)flags->status_flags);
        (void)filed_call(endpoint_fd, FILED_WIRE_OP_CLOSE, 8, -1, dup_handle, &result);
        (void)filed_call(endpoint_fd, FILED_WIRE_OP_CLOSE, 9, -1, handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -8;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_WIRE_OP_CLOSE, 7, -1, handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close original failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_WIRE_OP_CLOSE, 8, -1, dup_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    filed_wire_io_t *io = (filed_wire_io_t *)page;
    io->handle = dup_handle;
    io->offset = 0;
    io->length = FILED_SMOKE_MAX_READ;
    result = 0;
    status = filed_call(endpoint_fd, FILED_WIRE_OP_READ, 8, page_fd, 0, &result);
    if (status != 0 || result < 4 ||
        io->data[0] != 0x7f ||
        io->data[1] != 'E' ||
        io->data[2] != 'L' ||
        io->data[3] != 'F')
    {
        fprintf(stderr,
            "[filed-smoke] read failed status=%d bytes=%llu magic=%02x %02x %02x %02x\n",
            status,
            (unsigned long long)result,
            io->data[0],
            io->data[1],
            io->data[2],
            io->data[3]);
        (void)filed_call(endpoint_fd, FILED_WIRE_OP_CLOSE, 9, -1, dup_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -5;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_WIRE_OP_CLOSE, 9, -1, dup_handle, &result);
    const uint64_t bytes_read = io->length;
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close failed status=%d\n", status);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_wire_openat_t *)page;
    openat->dir_handle = 0;
    openat->rights =
        FILED_WIRE_RIGHT_READ |
        FILED_WIRE_RIGHT_WRITE |
        FILED_WIRE_RIGHT_STAT;
    openat->open_flags = 0;
    snprintf(openat->name, sizeof(openat->name), "/etc/os-release");

    result = 0;
    status = filed_call(endpoint_fd, FILED_WIRE_OP_OPENAT, 10, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] write-open failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -9;
    }
    const uint64_t write_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_wire_io_t *)page;
    io->handle = write_handle;
    io->offset = 0;
    io->length = 16;
    result = 0;
    status = filed_call(endpoint_fd, FILED_WIRE_OP_PREAD, 11, page_fd, 0, &result);
    if (status != 0 || result < 8) {
        fprintf(stderr,
            "[filed-smoke] write pread failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_WIRE_OP_CLOSE, 17, -1, write_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -10;
    }
    const uint64_t original_len = result;
    uint8_t original[16];
    memcpy(original, io->data, sizeof(original));

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_wire_io_t *)page;
    io->handle = write_handle;
    io->offset = 0;
    io->length = original_len;
    memcpy(io->data, original, (size_t)original_len);
    result = 0;
    status = filed_call(endpoint_fd, FILED_WIRE_OP_PWRITE, 12, page_fd, 0, &result);
    if (status != 0 || result != original_len) {
        fprintf(stderr,
            "[filed-smoke] pwrite failed status=%d bytes=%llu expected=%llu\n",
            status,
            (unsigned long long)result,
            (unsigned long long)original_len);
        (void)filed_call(endpoint_fd, FILED_WIRE_OP_CLOSE, 17, -1, write_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -11;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_WIRE_OP_FSYNC, 13, -1, write_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] fsync failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_WIRE_OP_CLOSE, 17, -1, write_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_wire_io_t *)page;
    io->handle = write_handle;
    io->length = 4;
    memcpy(io->data, original, 4);
    result = 0;
    status = filed_call(endpoint_fd, FILED_WIRE_OP_WRITE, 14, page_fd, 0, &result);
    if (status != 0 || result != 4 || io->offset != 0) {
        fprintf(stderr,
            "[filed-smoke] write failed status=%d bytes=%llu offset=%llu\n",
            status,
            (unsigned long long)result,
            (unsigned long long)io->offset);
        (void)filed_call(endpoint_fd, FILED_WIRE_OP_CLOSE, 17, -1, write_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -12;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_wire_io_t *)page;
    io->handle = write_handle;
    io->length = 4;
    result = 0;
    status = filed_call(endpoint_fd, FILED_WIRE_OP_READ, 15, page_fd, 0, &result);
    if (status != 0 ||
        result != 4 ||
        memcmp(io->data, original + 4, 4) != 0)
    {
        fprintf(stderr,
            "[filed-smoke] write offset readback failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_WIRE_OP_CLOSE, 17, -1, write_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -13;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_WIRE_OP_CLOSE, 16, -1, write_handle, &result);
    destroy_wire_page(page_fd, page);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close write handle failed status=%d\n", status);
        return status;
    }

    printf("[filed-smoke] ready path=/sbin/filed.elf bytes=%llu write=ok\n",
        (unsigned long long)bytes_read);
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;

    int bootstrap_fd = -1;
    int status = find_bootstrap_fd(argv, &bootstrap_fd);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] bootstrap fd missing status=%d\n", status);
        return 1;
    }

    filed_smoke_bootstrap_t bootstrap;
    memset(&bootstrap, 0, sizeof(bootstrap));
    const long got = pacha_fd_read(bootstrap_fd, &bootstrap, sizeof(bootstrap));
    if (got != (long)sizeof(bootstrap) ||
        bootstrap.magic != FILED_SMOKE_BOOTSTRAP_MAGIC ||
        bootstrap.public_endpoint_fd < 16)
    {
        fprintf(stderr,
            "[filed-smoke] bootstrap invalid got=%ld magic=0x%llx endpoint=%llu\n",
            got,
            (unsigned long long)bootstrap.magic,
            (unsigned long long)bootstrap.public_endpoint_fd);
        return 1;
    }

    status = smoke_filed_endpoint((int)(uint32_t)bootstrap.public_endpoint_fd);
    return status == 0 ? 0 : 1;
}
