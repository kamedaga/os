#include "filed/payload_v2.h"
#include "filed/ipc_protocol_v2.h"
#include "filed_smoke/bootstrap.h"
#include "pacha/abi.h"
#include "pacha/ipc.h"
#include "pacha/syscall.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    FILED_SMOKE_PAGE_SIZE = FILED_V2_PAGE_BYTES,
    FILED_SMOKE_MAX_READ = 64,
};

static int has_env_value(char **envp, const char *expected)
{
    if (envp == NULL || expected == NULL) {
        return 0;
    }
    for (char **p = envp; *p != NULL; ++p) {
        if (strcmp(*p, expected) == 0) {
            return 1;
        }
    }
    return 0;
}

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

static int filed_v2_payload_size(uint32_t op, uint32_t *out_payload_size)
{
    if (out_payload_size == NULL) {
        return -1;
    }

    switch (op) {
    case FILED_V2_OP_VFS_OPENAT: *out_payload_size = sizeof(filed_v2_path_request_t); return 0;
    case FILED_V2_OP_VFS_STAT: *out_payload_size = sizeof(filed_v2_statx_t); return 0;
    case FILED_V2_OP_VFS_PREAD:
    case FILED_V2_OP_VFS_READ:
    case FILED_V2_OP_VFS_PWRITE:
    case FILED_V2_OP_VFS_WRITE: *out_payload_size = sizeof(filed_v2_io_t); return 0;
    case FILED_V2_OP_VFS_GETDENTS: *out_payload_size = sizeof(filed_v2_getdents_t); return 0;
    case FILED_V2_OP_VFS_CLOSE:
    case FILED_V2_OP_VFS_FSYNC: *out_payload_size = sizeof(filed_v2_handle_request_t); return 0;
    case FILED_V2_OP_VFS_DUP:
    case FILED_V2_OP_VFS_GET_FLAGS:
    case FILED_V2_OP_VFS_SET_FLAGS: *out_payload_size = sizeof(filed_v2_handle_flags_t); return 0;
    case FILED_V2_OP_VFS_TRUNCATE: *out_payload_size = sizeof(filed_v2_truncate_t); return 0;
    case FILED_V2_OP_VFS_UNLINK: *out_payload_size = sizeof(filed_v2_unlink_t); return 0;
    case FILED_V2_OP_VFS_RENAME: *out_payload_size = sizeof(filed_v2_rename_t); return 0;
    case FILED_V2_OP_VFS_MKDIR: *out_payload_size = sizeof(filed_v2_mkdir_t); return 0;
    case FILED_V2_OP_VFS_RMDIR: *out_payload_size = sizeof(filed_v2_rmdir_t); return 0;
    default: return -95;
    }
}

static int filed_call(
    int endpoint_fd,
    uint32_t op,
    uint64_t request_id,
    int transfer_fd,
    uint64_t word2,
    uint64_t *out_result)
{
    if (endpoint_fd < 16 || request_id == 0 || out_result == NULL) {
        return -1;
    }

    uint32_t payload_size = 0;
    const int payload_status = filed_v2_payload_size(op, &payload_size);
    if (payload_status != 0) {
        return payload_status;
    }

    int owned_page_fd = -1;
    void *owned_page = NULL;
    if (transfer_fd < 16) {
        const int create_status = create_wire_page(&owned_page_fd, &owned_page);
        if (create_status != 0) {
            return create_status;
        }
        transfer_fd = owned_page_fd;
    }
    void *call_page = owned_page != NULL ? owned_page : NULL;
    if (call_page == NULL) {
        call_page = pacha_mmap(
            transfer_fd,
            FILED_SMOKE_PAGE_SIZE,
            PACHA_PROT_READ | PACHA_PROT_WRITE,
            PACHA_MMAP_SHARED,
            0);
        if (call_page == NULL) {
            if (owned_page_fd >= 16) {
                destroy_wire_page(owned_page_fd, owned_page);
            }
            return -2;
        }
    }

    if (op == FILED_V2_OP_VFS_OPENAT) {
        filed_v2_openat_t old_openat;
        memcpy(&old_openat, call_page, sizeof(old_openat));
        memset(call_page, 0, FILED_SMOKE_PAGE_SIZE);
        filed_v2_path_request_t *path =
            (filed_v2_path_request_t *)((uint8_t *)call_page + PACHA_SERVICE_HEADER_BYTES);
        path->dir_handle = old_openat.dir_handle;
        path->rights = old_openat.rights;
        path->flags = old_openat.open_flags;
        snprintf(path->path, sizeof(path->path), "%s", old_openat.name);
    } else if (op == FILED_V2_OP_VFS_CLOSE || op == FILED_V2_OP_VFS_FSYNC) {
        memset(call_page, 0, FILED_SMOKE_PAGE_SIZE);
        filed_v2_handle_request_t *handle =
            (filed_v2_handle_request_t *)((uint8_t *)call_page + PACHA_SERVICE_HEADER_BYTES);
        handle->handle = word2;
    } else {
        memmove((uint8_t *)call_page + PACHA_SERVICE_HEADER_BYTES, call_page, payload_size);
        memset(call_page, 0, PACHA_SERVICE_HEADER_BYTES);
    }

    pacha_service_request_header_t *header = (pacha_service_request_header_t *)call_page;
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = FILED_V2_SERVICE_ID;
    header->op = op;
    header->flags = PACHA_SERVICE_FLAG_PAGE_PAYLOAD;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = payload_size;

    struct pacha_ipc_fd fd_item;
    memset(&fd_item, 0, sizeof(fd_item));
    fd_item.fd = (uint64_t)(uint32_t)transfer_fd;
    fd_item.rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    fd_item.flags = 0;
    fd_item.transfer_flags = 0;

    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = 0,
        .word2 = 0,
        .word3 = request_id,
        .fds = &fd_item,
        .fd_count = 1,
    };
    const int reply_fd = pacha_ipc_call(endpoint_fd, &request);
    if (reply_fd < 16) {
        if (call_page != owned_page) {
            (void)pacha_munmap(call_page, FILED_SMOKE_PAGE_SIZE);
        }
        if (owned_page_fd >= 16) {
            destroy_wire_page(owned_page_fd, owned_page);
        }
        return reply_fd;
    }

    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    const int recv_status = recv_ipc_wait(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    if (recv_status != 0) {
        if (call_page != owned_page) {
            (void)pacha_munmap(call_page, FILED_SMOKE_PAGE_SIZE);
        }
        if (owned_page_fd >= 16) {
            destroy_wire_page(owned_page_fd, owned_page);
        }
        return recv_status;
    }
    const pacha_service_reply_header_t *reply_header =
        (const pacha_service_reply_header_t *)call_page;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
        reply.word3 != request_id ||
        reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        reply_header->service_id != FILED_V2_SERVICE_ID ||
        reply_header->op != op ||
        reply_header->request_id != request_id)
    {
        if (call_page != owned_page) {
            (void)pacha_munmap(call_page, FILED_SMOKE_PAGE_SIZE);
        }
        if (owned_page_fd >= 16) {
            destroy_wire_page(owned_page_fd, owned_page);
        }
        return -2;
    }
    if (reply_header->status < 0) {
        const int status = (int)reply_header->status;
        if (call_page != owned_page) {
            (void)pacha_munmap(call_page, FILED_SMOKE_PAGE_SIZE);
        }
        if (owned_page_fd >= 16) {
            destroy_wire_page(owned_page_fd, owned_page);
        }
        return status;
    }
    if (op != FILED_V2_OP_VFS_OPENAT &&
        op != FILED_V2_OP_VFS_CLOSE &&
        op != FILED_V2_OP_VFS_FSYNC)
    {
        memmove(call_page, (uint8_t *)call_page + PACHA_SERVICE_HEADER_BYTES, payload_size);
    }
    *out_result = reply_header->result;
    if (call_page != owned_page) {
        (void)pacha_munmap(call_page, FILED_SMOKE_PAGE_SIZE);
    }
    if (owned_page_fd >= 16) {
        destroy_wire_page(owned_page_fd, owned_page);
    }
    return 0;
}

static void filed_smoke_best_effort_unlink(
    int endpoint_fd,
    int page_fd,
    void *page,
    uint64_t request_id,
    uint64_t dir_handle,
    const char *name)
{
    uint64_t result = 0;
    if (page == NULL || name == NULL) {
        return;
    }
    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    filed_v2_unlink_t *unlink = (filed_v2_unlink_t *)page;
    unlink->dir_handle = dir_handle;
    snprintf(unlink->name, sizeof(unlink->name), "%s", name);
    (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_UNLINK, request_id, page_fd, 0, &result);
}

static void filed_smoke_best_effort_rmdir(
    int endpoint_fd,
    int page_fd,
    void *page,
    uint64_t request_id,
    uint64_t dir_handle,
    const char *name)
{
    uint64_t result = 0;
    if (page == NULL || name == NULL) {
        return;
    }
    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    filed_v2_rmdir_t *rmdir = (filed_v2_rmdir_t *)page;
    rmdir->dir_handle = dir_handle;
    snprintf(rmdir->name, sizeof(rmdir->name), "%s", name);
    (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_RMDIR, request_id, page_fd, 0, &result);
}

static int filed_smoke_mutation_stress(
    int endpoint_fd,
    int page_fd,
    void *page,
    uint64_t root_dir_handle)
{
    uint64_t request_id = 120;
    uint64_t result = 0;

    for (unsigned int i = 0; i < 3; ++i) {
        char tmp_name[FILED_V2_NAME_BYTES];
        char done_name[FILED_V2_NAME_BYTES];
        char dir_name[FILED_V2_NAME_BYTES];
        char payload[32];
        snprintf(tmp_name, sizeof(tmp_name), "filed-mut-%u.tmp", i);
        snprintf(done_name, sizeof(done_name), "filed-mut-%u.done", i);
        snprintf(dir_name, sizeof(dir_name), "filed-mut-dir-%u", i);
        snprintf(payload, sizeof(payload), "mutation-%u", i);
        const uint64_t payload_len = strlen(payload);

        filed_smoke_best_effort_unlink(
            endpoint_fd,
            page_fd,
            page,
            request_id++,
            root_dir_handle,
            tmp_name);
        filed_smoke_best_effort_unlink(
            endpoint_fd,
            page_fd,
            page,
            request_id++,
            root_dir_handle,
            done_name);

        memset(page, 0, FILED_SMOKE_PAGE_SIZE);
        filed_v2_openat_t *openat = (filed_v2_openat_t *)page;
        openat->dir_handle = root_dir_handle;
        openat->rights =
            FILED_V2_RIGHT_LOOKUP |
            FILED_V2_RIGHT_STAT |
            FILED_V2_RIGHT_CREATE |
            FILED_V2_RIGHT_REMOVE |
            FILED_V2_RIGHT_RENAME;
        openat->open_flags = FILED_V2_OPEN_DIRECTORY;
        snprintf(openat->name, sizeof(openat->name), "%s", dir_name);
        result = 0;
        int status = filed_call(
            endpoint_fd,
            FILED_V2_OP_VFS_OPENAT,
            request_id++,
            page_fd,
            0,
            &result);
        if (status == 0 && result != 0) {
            const uint64_t old_dir_handle = result;
            filed_smoke_best_effort_unlink(
                endpoint_fd,
                page_fd,
                page,
                request_id++,
                old_dir_handle,
                "child.tmp");
            (void)filed_call(
                endpoint_fd,
                FILED_V2_OP_VFS_CLOSE,
                request_id++,
                -1,
                old_dir_handle,
                &result);
        }
        filed_smoke_best_effort_rmdir(
            endpoint_fd,
            page_fd,
            page,
            request_id++,
            root_dir_handle,
            dir_name);

        memset(page, 0, FILED_SMOKE_PAGE_SIZE);
        openat = (filed_v2_openat_t *)page;
        openat->dir_handle = root_dir_handle;
        openat->rights =
            FILED_V2_RIGHT_READ |
            FILED_V2_RIGHT_WRITE |
            FILED_V2_RIGHT_STAT;
        openat->open_flags = FILED_V2_OPEN_CREATE | FILED_V2_OPEN_TRUNCATE;
        snprintf(openat->name, sizeof(openat->name), "%s", tmp_name);
        result = 0;
        status = filed_call(
            endpoint_fd,
            FILED_V2_OP_VFS_OPENAT,
            request_id++,
            page_fd,
            0,
            &result);
        if (status != 0 || result == 0) {
            fprintf(stderr,
                "[filed-smoke] mutation create failed i=%u status=%d handle=%llu\n",
                i,
                status,
                (unsigned long long)result);
            return status != 0 ? status : -70;
        }
        const uint64_t file_handle = result;

        memset(page, 0, FILED_SMOKE_PAGE_SIZE);
        filed_v2_io_t *io = (filed_v2_io_t *)page;
        io->handle = file_handle;
        io->length = payload_len;
        memcpy(io->data, payload, (size_t)payload_len);
        result = 0;
        status = filed_call(
            endpoint_fd,
            FILED_V2_OP_VFS_WRITE,
            request_id++,
            page_fd,
            0,
            &result);
        if (status != 0 || result != payload_len) {
            fprintf(stderr,
                "[filed-smoke] mutation write failed i=%u status=%d bytes=%llu\n",
                i,
                status,
                (unsigned long long)result);
            (void)filed_call(
                endpoint_fd,
                FILED_V2_OP_VFS_CLOSE,
                request_id++,
                -1,
                file_handle,
                &result);
            return status != 0 ? status : -71;
        }

        status = filed_call(
            endpoint_fd,
            FILED_V2_OP_VFS_CLOSE,
            request_id++,
            -1,
            file_handle,
            &result);
        if (status != 0) {
            fprintf(stderr, "[filed-smoke] mutation close failed i=%u status=%d\n", i, status);
            return status;
        }

        memset(page, 0, FILED_SMOKE_PAGE_SIZE);
        filed_v2_rename_t *rename = (filed_v2_rename_t *)page;
        rename->old_dir_handle = root_dir_handle;
        rename->new_dir_handle = root_dir_handle;
        snprintf(rename->old_name, sizeof(rename->old_name), "%s", tmp_name);
        snprintf(rename->new_name, sizeof(rename->new_name), "%s", done_name);
        result = 0;
        status = filed_call(
            endpoint_fd,
            FILED_V2_OP_VFS_RENAME,
            request_id++,
            page_fd,
            0,
            &result);
        if (status != 0) {
            fprintf(stderr, "[filed-smoke] mutation rename failed i=%u status=%d\n", i, status);
            return status;
        }

        memset(page, 0, FILED_SMOKE_PAGE_SIZE);
        openat = (filed_v2_openat_t *)page;
        openat->dir_handle = root_dir_handle;
        openat->rights = FILED_V2_RIGHT_READ | FILED_V2_RIGHT_STAT;
        openat->open_flags = 0;
        snprintf(openat->name, sizeof(openat->name), "%s", done_name);
        result = 0;
        status = filed_call(
            endpoint_fd,
            FILED_V2_OP_VFS_OPENAT,
            request_id++,
            page_fd,
            0,
            &result);
        if (status != 0 || result == 0) {
            fprintf(stderr,
                "[filed-smoke] mutation reopen failed i=%u status=%d handle=%llu\n",
                i,
                status,
                (unsigned long long)result);
            return status != 0 ? status : -72;
        }
        const uint64_t read_handle = result;

        memset(page, 0, FILED_SMOKE_PAGE_SIZE);
        io = (filed_v2_io_t *)page;
        io->handle = read_handle;
        io->offset = 0;
        io->length = payload_len;
        result = 0;
        status = filed_call(
            endpoint_fd,
            FILED_V2_OP_VFS_PREAD,
            request_id++,
            page_fd,
            0,
            &result);
        if (status != 0 ||
            result != payload_len ||
            memcmp(io->data, payload, (size_t)payload_len) != 0)
        {
            fprintf(stderr,
                "[filed-smoke] mutation readback failed i=%u status=%d bytes=%llu\n",
                i,
                status,
                (unsigned long long)result);
            (void)filed_call(
                endpoint_fd,
                FILED_V2_OP_VFS_CLOSE,
                request_id++,
                -1,
                read_handle,
                &result);
            return status != 0 ? status : -73;
        }

        status = filed_call(
            endpoint_fd,
            FILED_V2_OP_VFS_CLOSE,
            request_id++,
            -1,
            read_handle,
            &result);
        if (status != 0) {
            fprintf(stderr,
                "[filed-smoke] mutation close read handle failed i=%u status=%d\n",
                i,
                status);
            return status;
        }
        filed_smoke_best_effort_unlink(
            endpoint_fd,
            page_fd,
            page,
            request_id++,
            root_dir_handle,
            done_name);

        memset(page, 0, FILED_SMOKE_PAGE_SIZE);
        filed_v2_mkdir_t *mkdir = (filed_v2_mkdir_t *)page;
        mkdir->dir_handle = root_dir_handle;
        mkdir->mode = 0755;
        snprintf(mkdir->name, sizeof(mkdir->name), "%s", dir_name);
        result = 0;
        status = filed_call(
            endpoint_fd,
            FILED_V2_OP_VFS_MKDIR,
            request_id++,
            page_fd,
            0,
            &result);
        if (status != 0) {
            fprintf(stderr, "[filed-smoke] mutation mkdir failed i=%u status=%d\n", i, status);
            return status;
        }

        memset(page, 0, FILED_SMOKE_PAGE_SIZE);
        openat = (filed_v2_openat_t *)page;
        openat->dir_handle = root_dir_handle;
        openat->rights =
            FILED_V2_RIGHT_LOOKUP |
            FILED_V2_RIGHT_STAT |
            FILED_V2_RIGHT_CREATE |
            FILED_V2_RIGHT_REMOVE;
        openat->open_flags = FILED_V2_OPEN_DIRECTORY;
        snprintf(openat->name, sizeof(openat->name), "%s", dir_name);
        result = 0;
        status = filed_call(
            endpoint_fd,
            FILED_V2_OP_VFS_OPENAT,
            request_id++,
            page_fd,
            0,
            &result);
        if (status != 0 || result == 0) {
            fprintf(stderr,
                "[filed-smoke] mutation open dir failed i=%u status=%d handle=%llu\n",
                i,
                status,
                (unsigned long long)result);
            return status != 0 ? status : -74;
        }
        const uint64_t dir_handle = result;

        memset(page, 0, FILED_SMOKE_PAGE_SIZE);
        openat = (filed_v2_openat_t *)page;
        openat->dir_handle = dir_handle;
        openat->rights = FILED_V2_RIGHT_READ | FILED_V2_RIGHT_WRITE | FILED_V2_RIGHT_STAT;
        openat->open_flags = FILED_V2_OPEN_CREATE | FILED_V2_OPEN_TRUNCATE;
        snprintf(openat->name, sizeof(openat->name), "%s", "child.tmp");
        result = 0;
        status = filed_call(
            endpoint_fd,
            FILED_V2_OP_VFS_OPENAT,
            request_id++,
            page_fd,
            0,
            &result);
        if (status != 0 || result == 0) {
            fprintf(stderr,
                "[filed-smoke] mutation create child failed i=%u status=%d handle=%llu\n",
                i,
                status,
                (unsigned long long)result);
            (void)filed_call(
                endpoint_fd,
                FILED_V2_OP_VFS_CLOSE,
                request_id++,
                -1,
                dir_handle,
                &result);
            return status != 0 ? status : -75;
        }
        const uint64_t child_handle = result;

        status = filed_call(
            endpoint_fd,
            FILED_V2_OP_VFS_CLOSE,
            request_id++,
            -1,
            child_handle,
            &result);
        if (status != 0) {
            fprintf(stderr,
                "[filed-smoke] mutation close child failed i=%u status=%d\n",
                i,
                status);
            (void)filed_call(
                endpoint_fd,
                FILED_V2_OP_VFS_CLOSE,
                request_id++,
                -1,
                dir_handle,
                &result);
            return status;
        }
        filed_smoke_best_effort_unlink(
            endpoint_fd,
            page_fd,
            page,
            request_id++,
            dir_handle,
            "child.tmp");

        status = filed_call(
            endpoint_fd,
            FILED_V2_OP_VFS_CLOSE,
            request_id++,
            -1,
            dir_handle,
            &result);
        if (status != 0) {
            fprintf(stderr, "[filed-smoke] mutation close dir failed i=%u status=%d\n", i, status);
            return status;
        }

        memset(page, 0, FILED_SMOKE_PAGE_SIZE);
        filed_v2_rmdir_t *rmdir = (filed_v2_rmdir_t *)page;
        rmdir->dir_handle = root_dir_handle;
        snprintf(rmdir->name, sizeof(rmdir->name), "%s", dir_name);
        result = 0;
        status = filed_call(
            endpoint_fd,
            FILED_V2_OP_VFS_RMDIR,
            request_id++,
            page_fd,
            0,
            &result);
        if (status != 0) {
            fprintf(stderr, "[filed-smoke] mutation rmdir failed i=%u status=%d\n", i, status);
            return status;
        }
    }

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

    filed_v2_openat_t *openat = (filed_v2_openat_t *)page;
    openat->dir_handle = 0;
    openat->rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_STAT |
        FILED_V2_RIGHT_EXEC;
    openat->open_flags = 0;
    snprintf(openat->name, sizeof(openat->name), "/sbin/./filed.elf");

    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 1, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        destroy_wire_page(page_fd, page);
        fprintf(stderr, "[filed-smoke] openat failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        return status != 0 ? status : -3;
    }
    const uint64_t handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    filed_v2_statx_t *stat = (filed_v2_statx_t *)page;
    stat->handle = handle;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_STAT, 2, page_fd, 0, &result);
    if (status != 0 || stat->size == 0 || stat->kind != 0100000u) {
        fprintf(stderr,
            "[filed-smoke] stat failed status=%d size=%llu kind=0%llo\n",
            status,
            (unsigned long long)stat->size,
            (unsigned long long)stat->kind);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 4, -1, handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -4;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    filed_v2_handle_flags_t *flags = (filed_v2_handle_flags_t *)page;
    flags->handle = handle;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_GET_FLAGS, 3, page_fd, 0, &result);
    if (status != 0 || flags->fd_flags != 0 || flags->status_flags != 0) {
        fprintf(stderr,
            "[filed-smoke] get_flags failed status=%d fd=0x%llx status=0x%llx\n",
            status,
            (unsigned long long)flags->fd_flags,
            (unsigned long long)flags->status_flags);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 8, -1, handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -6;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    flags = (filed_v2_handle_flags_t *)page;
    flags->handle = handle;
    flags->fd_flags = 0;
    flags->status_flags = FILED_V2_FILE_NONBLOCK;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_SET_FLAGS, 4, page_fd, 0, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] set_flags failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 8, -1, handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    flags = (filed_v2_handle_flags_t *)page;
    flags->handle = handle;
    flags->fd_flags = FILED_V2_FD_CLOEXEC;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_DUP, 5, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] dup failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 8, -1, handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -7;
    }
    const uint64_t dup_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    flags = (filed_v2_handle_flags_t *)page;
    flags->handle = dup_handle;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_GET_FLAGS, 6, page_fd, 0, &result);
    if (status != 0 ||
        flags->fd_flags != FILED_V2_FD_CLOEXEC ||
        flags->status_flags != FILED_V2_FILE_NONBLOCK)
    {
        fprintf(stderr,
            "[filed-smoke] dup flags failed status=%d fd=0x%llx status=0x%llx\n",
            status,
            (unsigned long long)flags->fd_flags,
            (unsigned long long)flags->status_flags);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 8, -1, dup_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 9, -1, handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -8;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 7, -1, handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close original failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 8, -1, dup_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    filed_v2_io_t *io = (filed_v2_io_t *)page;
    io->handle = dup_handle;
    io->offset = 0;
    io->length = FILED_SMOKE_MAX_READ;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_READ, 8, page_fd, 0, &result);
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
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 9, -1, dup_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -5;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 9, -1, dup_handle, &result);
    const uint64_t bytes_read = io->length;
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close failed status=%d\n", status);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = 0;
    openat->rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_WRITE |
        FILED_V2_RIGHT_STAT;
    openat->open_flags = 0;
    snprintf(openat->name, sizeof(openat->name), "/etc/os-release");

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 10, page_fd, 0, &result);
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
    io = (filed_v2_io_t *)page;
    io->handle = write_handle;
    io->offset = 0;
    io->length = 16;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_PREAD, 11, page_fd, 0, &result);
    if (status != 0 || result < 8) {
        fprintf(stderr,
            "[filed-smoke] write pread failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 17, -1, write_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -10;
    }
    const uint64_t original_len = result;
    uint8_t original[16];
    memcpy(original, io->data, sizeof(original));

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = write_handle;
    io->offset = 0;
    io->length = original_len;
    memcpy(io->data, original, (size_t)original_len);
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_PWRITE, 12, page_fd, 0, &result);
    if (status != 0 || result != original_len) {
        fprintf(stderr,
            "[filed-smoke] pwrite failed status=%d bytes=%llu expected=%llu\n",
            status,
            (unsigned long long)result,
            (unsigned long long)original_len);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 17, -1, write_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -11;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_FSYNC, 13, -1, write_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] fsync failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 17, -1, write_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = write_handle;
    io->length = 4;
    memcpy(io->data, original, 4);
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_WRITE, 14, page_fd, 0, &result);
    if (status != 0 || result != 4 || io->offset != 0) {
        fprintf(stderr,
            "[filed-smoke] write failed status=%d bytes=%llu offset=%llu\n",
            status,
            (unsigned long long)result,
            (unsigned long long)io->offset);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 17, -1, write_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -12;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = write_handle;
    io->length = 4;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_READ, 15, page_fd, 0, &result);
    if (status != 0 ||
        result != 4 ||
        memcmp(io->data, original + 4, 4) != 0)
    {
        fprintf(stderr,
            "[filed-smoke] write offset readback failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 17, -1, write_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -13;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 16, -1, write_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close write handle failed status=%d\n", status);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    filed_v2_unlink_t *unlink = (filed_v2_unlink_t *)page;
    unlink->dir_handle = 0;
    snprintf(unlink->name, sizeof(unlink->name), "%s", "filed-write-smoke.tmp");
    result = 0;
    (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_UNLINK, 17, page_fd, 0, &result);

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = 0;
    openat->rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_WRITE |
        FILED_V2_RIGHT_STAT;
    openat->open_flags =
        FILED_V2_OPEN_CREATE |
        FILED_V2_OPEN_TRUNCATE;
    snprintf(openat->name, sizeof(openat->name), "%s", "/filed-write-smoke.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 18, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] create open failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -14;
    }
    const uint64_t created_handle = result;

    const char created_payload[] = "filed-create-write";
    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = created_handle;
    io->length = sizeof(created_payload) - 1u;
    memcpy(io->data, created_payload, sizeof(created_payload) - 1u);
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_WRITE, 19, page_fd, 0, &result);
    if (status != 0 || result != sizeof(created_payload) - 1u) {
        fprintf(stderr,
            "[filed-smoke] create write failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 30, -1, created_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -15;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 20, -1, created_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close created failed status=%d\n", status);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = 0;
    openat->rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_WRITE |
        FILED_V2_RIGHT_STAT;
    openat->open_flags = 0;
    snprintf(openat->name, sizeof(openat->name), "%s", "/filed-write-smoke.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 21, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] reopen created failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -16;
    }
    const uint64_t created_read_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = created_read_handle;
    io->length = sizeof(created_payload) - 1u;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_READ, 22, page_fd, 0, &result);
    if (status != 0 ||
        result != sizeof(created_payload) - 1u ||
        memcmp(io->data, created_payload, sizeof(created_payload) - 1u) != 0)
    {
        fprintf(stderr,
            "[filed-smoke] created read failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 30, -1, created_read_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -17;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    flags = (filed_v2_handle_flags_t *)page;
    flags->handle = created_read_handle;
    flags->status_flags = FILED_V2_FILE_APPEND;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_SET_FLAGS, 23, page_fd, 0, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] append set_flags failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 30, -1, created_read_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    const char append_payload[] = "+append";
    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = created_read_handle;
    io->length = sizeof(append_payload) - 1u;
    memcpy(io->data, append_payload, sizeof(append_payload) - 1u);
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_WRITE, 24, page_fd, 0, &result);
    if (status != 0 || result != sizeof(append_payload) - 1u) {
        fprintf(stderr,
            "[filed-smoke] append write failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 30, -1, created_read_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -18;
    }

    char appended_expected[64];
    const uint64_t created_payload_len = sizeof(created_payload) - 1u;
    const uint64_t append_payload_len = sizeof(append_payload) - 1u;
    const uint64_t appended_expected_len = created_payload_len + append_payload_len;
    if (appended_expected_len > sizeof(appended_expected)) {
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 30, -1, created_read_handle, &result);
        destroy_wire_page(page_fd, page);
        return -19;
    }
    memcpy(appended_expected, created_payload, (size_t)created_payload_len);
    memcpy(
        appended_expected + created_payload_len,
        append_payload,
        (size_t)append_payload_len);

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = created_read_handle;
    io->offset = 0;
    io->length = appended_expected_len;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_PREAD, 25, page_fd, 0, &result);
    if (status != 0 ||
        result != appended_expected_len ||
        memcmp(io->data, appended_expected, (size_t)appended_expected_len) != 0)
    {
        fprintf(stderr,
            "[filed-smoke] append readback failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 30, -1, created_read_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -20;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    filed_v2_truncate_t *truncate = (filed_v2_truncate_t *)page;
    truncate->handle = created_read_handle;
    truncate->size = 5;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_TRUNCATE, 26, page_fd, 0, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] truncate failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 30, -1, created_read_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = created_read_handle;
    io->offset = 0;
    io->length = 5;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_PREAD, 27, page_fd, 0, &result);
    if (status != 0 ||
        result != 5 ||
        memcmp(io->data, created_payload, 5) != 0)
    {
        fprintf(stderr,
            "[filed-smoke] truncate readback failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 30, -1, created_read_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -21;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 28, -1, created_read_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close append handle failed status=%d\n", status);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    filed_v2_rename_t *rename = (filed_v2_rename_t *)page;
    rename->old_dir_handle = 0;
    rename->new_dir_handle = 0;
    snprintf(rename->old_name, sizeof(rename->old_name), "%s", "filed-write-smoke.tmp");
    snprintf(rename->new_name, sizeof(rename->new_name), "%s", "filed-write-smoke.done");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_RENAME, 29, page_fd, 0, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] rename failed status=%d\n", status);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = 0;
    openat->rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_STAT;
    openat->open_flags = 0;
    snprintf(openat->name, sizeof(openat->name), "%s", "/filed-write-smoke.done");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 31, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] renamed open failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -22;
    }
    const uint64_t renamed_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = renamed_handle;
    io->length = 5;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_READ, 32, page_fd, 0, &result);
    if (status != 0 ||
        result != 5 ||
        memcmp(io->data, created_payload, 5) != 0)
    {
        fprintf(stderr,
            "[filed-smoke] renamed read failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 34, -1, renamed_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -23;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 33, -1, renamed_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close renamed failed status=%d\n", status);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    unlink = (filed_v2_unlink_t *)page;
    unlink->dir_handle = 0;
    snprintf(unlink->name, sizeof(unlink->name), "%s", "filed-write-smoke.done");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_UNLINK, 35, page_fd, 0, &result);
    if (status != 0) {
        destroy_wire_page(page_fd, page);
        fprintf(stderr, "[filed-smoke] unlink failed status=%d\n", status);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = 0;
    openat->rights = FILED_V2_RIGHT_READ;
    openat->open_flags = 0;
    snprintf(openat->name, sizeof(openat->name), "%s", "/filed-write-smoke.done");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 36, page_fd, 0, &result);
    if (status == 0 || result != 0) {
        fprintf(stderr,
            "[filed-smoke] unlinked open unexpectedly succeeded status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        destroy_wire_page(page_fd, page);
        return -24;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = 0;
    openat->rights =
        FILED_V2_RIGHT_LOOKUP |
        FILED_V2_RIGHT_STAT |
        FILED_V2_RIGHT_GETDENTS |
        FILED_V2_RIGHT_CREATE |
        FILED_V2_RIGHT_REMOVE |
        FILED_V2_RIGHT_RENAME;
    openat->open_flags = FILED_V2_OPEN_DIRECTORY;
    snprintf(openat->name, sizeof(openat->name), "%s", "/");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 37, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] root dir open failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -25;
    }
    const uint64_t root_dir_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = root_dir_handle;
    openat->rights =
        FILED_V2_RIGHT_LOOKUP |
        FILED_V2_RIGHT_STAT |
        FILED_V2_RIGHT_GETDENTS;
    openat->open_flags = FILED_V2_OPEN_DIRECTORY;
    snprintf(openat->name, sizeof(openat->name), "%s", "etc");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 38, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] openat dirfd etc failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -26;
    }
    const uint64_t etc_dir_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = etc_dir_handle;
    openat->rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_STAT;
    openat->open_flags = 0;
    snprintf(openat->name, sizeof(openat->name), "%s", "os-release");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 39, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] openat nested dirfd failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, etc_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -27;
    }
    const uint64_t etc_file_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = etc_file_handle;
    io->offset = 0;
    io->length = 8;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_PREAD, 40, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] dirfd pread failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, etc_file_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, etc_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -28;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    filed_v2_getdents_t *getdents = (filed_v2_getdents_t *)page;
    getdents->dir_handle = root_dir_handle;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_GETDENTS, 41, page_fd, 0, &result);
    if (status != 0 || getdents->offset != 0 || result != getdents->count) {
        fprintf(stderr,
            "[filed-smoke] getdents first failed status=%d result=%llu offset=%llu count=%llu\n",
            status,
            (unsigned long long)result,
            (unsigned long long)getdents->offset,
            (unsigned long long)getdents->count);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, etc_file_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, etc_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -29;
    }
    const uint64_t first_dir_count = getdents->count;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    getdents = (filed_v2_getdents_t *)page;
    getdents->dir_handle = root_dir_handle;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_GETDENTS, 42, page_fd, 0, &result);
    if (status != 0 || getdents->offset != first_dir_count || result != getdents->count) {
        fprintf(stderr,
            "[filed-smoke] getdents second failed status=%d result=%llu offset=%llu expected=%llu count=%llu\n",
            status,
            (unsigned long long)result,
            (unsigned long long)getdents->offset,
            (unsigned long long)first_dir_count,
            (unsigned long long)getdents->count);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, etc_file_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, etc_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -30;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 43, -1, etc_file_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close dirfd file failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, etc_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 44, -1, etc_dir_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close etc dir failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    unlink = (filed_v2_unlink_t *)page;
    unlink->dir_handle = root_dir_handle;
    snprintf(unlink->name, sizeof(unlink->name), "%s", "filed-open-unlink.tmp");
    result = 0;
    (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_UNLINK, 45, page_fd, 0, &result);

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = root_dir_handle;
    openat->rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_WRITE |
        FILED_V2_RIGHT_STAT;
    openat->open_flags =
        FILED_V2_OPEN_CREATE |
        FILED_V2_OPEN_TRUNCATE;
    snprintf(openat->name, sizeof(openat->name), "%s", "filed-open-unlink.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 46, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] open-unlink create failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -31;
    }
    const uint64_t open_unlink_handle = result;

    const char unlink_payload[] = "unlink-open-alive";
    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = open_unlink_handle;
    io->length = sizeof(unlink_payload) - 1u;
    memcpy(io->data, unlink_payload, sizeof(unlink_payload) - 1u);
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_WRITE, 47, page_fd, 0, &result);
    if (status != 0 || result != sizeof(unlink_payload) - 1u) {
        fprintf(stderr,
            "[filed-smoke] open-unlink write failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, open_unlink_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -32;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    unlink = (filed_v2_unlink_t *)page;
    unlink->dir_handle = root_dir_handle;
    snprintf(unlink->name, sizeof(unlink->name), "%s", "filed-open-unlink.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_UNLINK, 48, page_fd, 0, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] unlink while open failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, open_unlink_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = open_unlink_handle;
    io->offset = 0;
    io->length = sizeof(unlink_payload) - 1u;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_PREAD, 49, page_fd, 0, &result);
    if (status != 0 ||
        result != sizeof(unlink_payload) - 1u ||
        memcmp(io->data, unlink_payload, sizeof(unlink_payload) - 1u) != 0)
    {
        fprintf(stderr,
            "[filed-smoke] unlinked open readback failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, open_unlink_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -33;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = root_dir_handle;
    openat->rights = FILED_V2_RIGHT_READ;
    openat->open_flags = 0;
    snprintf(openat->name, sizeof(openat->name), "%s", "filed-open-unlink.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 50, page_fd, 0, &result);
    if (status == 0 || result != 0) {
        fprintf(stderr,
            "[filed-smoke] unlink while open path unexpectedly exists status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, open_unlink_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return -34;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 51, -1, open_unlink_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close unlinked open failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    unlink = (filed_v2_unlink_t *)page;
    unlink->dir_handle = root_dir_handle;
    snprintf(unlink->name, sizeof(unlink->name), "%s", "filed-rename-old.tmp");
    result = 0;
    (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_UNLINK, 52, page_fd, 0, &result);
    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    unlink = (filed_v2_unlink_t *)page;
    unlink->dir_handle = root_dir_handle;
    snprintf(unlink->name, sizeof(unlink->name), "%s", "filed-rename-new.tmp");
    result = 0;
    (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_UNLINK, 53, page_fd, 0, &result);

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = root_dir_handle;
    openat->rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_WRITE |
        FILED_V2_RIGHT_STAT;
    openat->open_flags =
        FILED_V2_OPEN_CREATE |
        FILED_V2_OPEN_TRUNCATE;
    snprintf(openat->name, sizeof(openat->name), "%s", "filed-rename-old.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 54, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] rename old create failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -35;
    }
    const uint64_t rename_old_handle = result;

    const char rename_old_payload[] = "rename-old-live";
    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = rename_old_handle;
    io->length = sizeof(rename_old_payload) - 1u;
    memcpy(io->data, rename_old_payload, sizeof(rename_old_payload) - 1u);
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_WRITE, 55, page_fd, 0, &result);
    if (status != 0 || result != sizeof(rename_old_payload) - 1u) {
        fprintf(stderr,
            "[filed-smoke] rename old write failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, rename_old_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -36;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = root_dir_handle;
    openat->rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_WRITE |
        FILED_V2_RIGHT_STAT;
    openat->open_flags =
        FILED_V2_OPEN_CREATE |
        FILED_V2_OPEN_TRUNCATE;
    snprintf(openat->name, sizeof(openat->name), "%s", "filed-rename-new.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 56, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] rename new create failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, rename_old_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -37;
    }
    const uint64_t rename_replaced_handle = result;

    const char rename_replaced_payload[] = "rename-replaced-live";
    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = rename_replaced_handle;
    io->length = sizeof(rename_replaced_payload) - 1u;
    memcpy(io->data, rename_replaced_payload, sizeof(rename_replaced_payload) - 1u);
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_WRITE, 57, page_fd, 0, &result);
    if (status != 0 || result != sizeof(rename_replaced_payload) - 1u) {
        fprintf(stderr,
            "[filed-smoke] rename replaced write failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, rename_replaced_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, rename_old_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -38;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    rename = (filed_v2_rename_t *)page;
    rename->old_dir_handle = root_dir_handle;
    rename->new_dir_handle = root_dir_handle;
    snprintf(rename->old_name, sizeof(rename->old_name), "%s", "filed-rename-old.tmp");
    snprintf(rename->new_name, sizeof(rename->new_name), "%s", "filed-rename-new.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_RENAME, 58, page_fd, 0, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] rename replace while open failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, rename_replaced_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, rename_old_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = rename_replaced_handle;
    io->offset = 0;
    io->length = sizeof(rename_replaced_payload) - 1u;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_PREAD, 59, page_fd, 0, &result);
    if (status != 0 ||
        result != sizeof(rename_replaced_payload) - 1u ||
        memcmp(io->data, rename_replaced_payload, sizeof(rename_replaced_payload) - 1u) != 0)
    {
        fprintf(stderr,
            "[filed-smoke] replaced open readback failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, rename_replaced_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, rename_old_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -39;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = root_dir_handle;
    openat->rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_STAT;
    openat->open_flags = 0;
    snprintf(openat->name, sizeof(openat->name), "%s", "filed-rename-new.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 60, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] renamed replacement open failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, rename_replaced_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, rename_old_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -40;
    }
    const uint64_t rename_path_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = rename_path_handle;
    io->offset = 0;
    io->length = sizeof(rename_old_payload) - 1u;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_PREAD, 61, page_fd, 0, &result);
    if (status != 0 ||
        result != sizeof(rename_old_payload) - 1u ||
        memcmp(io->data, rename_old_payload, sizeof(rename_old_payload) - 1u) != 0)
    {
        fprintf(stderr,
            "[filed-smoke] renamed path readback failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, rename_path_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, rename_replaced_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, rename_old_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -41;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 62, -1, rename_path_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close renamed path failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, rename_replaced_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, rename_old_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 63, -1, rename_replaced_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close replaced handle failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, rename_old_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 64, -1, rename_old_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close rename old handle failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    filed_smoke_best_effort_unlink(
        endpoint_fd,
        page_fd,
        page,
        65,
        root_dir_handle,
        "filed-rename-new.tmp");
    filed_smoke_best_effort_unlink(
        endpoint_fd,
        page_fd,
        page,
        66,
        root_dir_handle,
        "filed-cross-rename.tmp");
    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = root_dir_handle;
    openat->rights =
        FILED_V2_RIGHT_LOOKUP |
        FILED_V2_RIGHT_STAT |
        FILED_V2_RIGHT_REMOVE |
        FILED_V2_RIGHT_RENAME;
    openat->open_flags = FILED_V2_OPEN_DIRECTORY;
    snprintf(openat->name, sizeof(openat->name), "%s", "filed-mkdir-smoke");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 67, page_fd, 0, &result);
    if (status == 0 && result != 0) {
        const uint64_t old_mkdir_dir_handle = result;
        filed_smoke_best_effort_unlink(
            endpoint_fd,
            page_fd,
            page,
            68,
            old_mkdir_dir_handle,
            "child.tmp");
        result = 0;
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 69, -1, old_mkdir_dir_handle, &result);
    }
    filed_smoke_best_effort_rmdir(
        endpoint_fd,
        page_fd,
        page,
        70,
        root_dir_handle,
        "filed-mkdir-smoke");
    filed_smoke_best_effort_unlink(
        endpoint_fd,
        page_fd,
        page,
        71,
        root_dir_handle,
        "filed-cross-replace.tmp");
    filed_smoke_best_effort_rmdir(
        endpoint_fd,
        page_fd,
        page,
        72,
        root_dir_handle,
        "filed-dir-rename-new");
    filed_smoke_best_effort_rmdir(
        endpoint_fd,
        page_fd,
        page,
        73,
        root_dir_handle,
        "filed-dir-rename-old");

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    filed_v2_mkdir_t *mkdir = (filed_v2_mkdir_t *)page;
    mkdir->dir_handle = root_dir_handle;
    mkdir->mode = 0755;
    snprintf(mkdir->name, sizeof(mkdir->name), "%s", "filed-mkdir-smoke");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_MKDIR, 74, page_fd, 0, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] mkdir failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = root_dir_handle;
    openat->rights =
        FILED_V2_RIGHT_LOOKUP |
        FILED_V2_RIGHT_STAT |
        FILED_V2_RIGHT_GETDENTS |
        FILED_V2_RIGHT_CREATE |
        FILED_V2_RIGHT_REMOVE |
        FILED_V2_RIGHT_RENAME;
    openat->open_flags = FILED_V2_OPEN_DIRECTORY;
    snprintf(openat->name, sizeof(openat->name), "%s", "filed-mkdir-smoke");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 75, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] open mkdir dir failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -42;
    }
    const uint64_t mkdir_dir_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    stat = (filed_v2_statx_t *)page;
    stat->handle = mkdir_dir_handle;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_STAT, 76, page_fd, 0, &result);
    if (status != 0 || stat->kind != 0040000u) {
        fprintf(stderr,
            "[filed-smoke] mkdir stat failed status=%d kind=0%llo\n",
            status,
            (unsigned long long)stat->kind);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -43;
    }

    const uint8_t cross_dir_payload[] = "cross-dir-rename";
    const uint8_t cross_replace_payload[] = "cross-dir-replace-open";

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = mkdir_dir_handle;
    openat->rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_WRITE |
        FILED_V2_RIGHT_STAT;
    openat->open_flags = FILED_V2_OPEN_CREATE | FILED_V2_OPEN_TRUNCATE;
    snprintf(openat->name, sizeof(openat->name), "%s", "child.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 77, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] open child in mkdir dir failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -45;
    }
    const uint64_t cross_child_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = cross_child_handle;
    io->length = sizeof(cross_dir_payload) - 1u;
    memcpy(io->data, cross_dir_payload, sizeof(cross_dir_payload) - 1u);
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_WRITE, 78, page_fd, 0, &result);
    if (status != 0 || result != sizeof(cross_dir_payload) - 1u) {
        fprintf(stderr,
            "[filed-smoke] write child in mkdir dir failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_child_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -46;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 79, -1, cross_child_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close child in mkdir dir failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = mkdir_dir_handle;
    openat->rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_WRITE |
        FILED_V2_RIGHT_STAT;
    openat->open_flags = FILED_V2_OPEN_CREATE | FILED_V2_OPEN_TRUNCATE;
    snprintf(openat->name, sizeof(openat->name), "%s", "replace.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 80, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] open cross-directory replace source failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -51;
    }
    const uint64_t cross_replace_source_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = cross_replace_source_handle;
    io->length = sizeof(cross_replace_payload) - 1u;
    memcpy(io->data, cross_replace_payload, sizeof(cross_replace_payload) - 1u);
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_WRITE, 81, page_fd, 0, &result);
    if (status != 0 || result != sizeof(cross_replace_payload) - 1u) {
        fprintf(stderr,
            "[filed-smoke] write cross-directory replace source failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_source_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -52;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = root_dir_handle;
    openat->rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_WRITE |
        FILED_V2_RIGHT_STAT;
    openat->open_flags = FILED_V2_OPEN_CREATE | FILED_V2_OPEN_TRUNCATE;
    snprintf(openat->name, sizeof(openat->name), "%s", "filed-cross-replace.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 82, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] open cross-directory replace target failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_source_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -53;
    }
    const uint64_t cross_replace_target_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    filed_v2_rmdir_t *rmdir = (filed_v2_rmdir_t *)page;
    rmdir->dir_handle = root_dir_handle;
    snprintf(rmdir->name, sizeof(rmdir->name), "%s", "filed-mkdir-smoke");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_RMDIR, 83, page_fd, 0, &result);
    if (status == 0) {
        fprintf(stderr, "[filed-smoke] non-empty rmdir unexpectedly succeeded\n");
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_target_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_source_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return -47;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    rename = (filed_v2_rename_t *)page;
    rename->old_dir_handle = mkdir_dir_handle;
    rename->new_dir_handle = root_dir_handle;
    snprintf(rename->old_name, sizeof(rename->old_name), "%s", "child.tmp");
    snprintf(rename->new_name, sizeof(rename->new_name), "%s", "filed-cross-rename.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_RENAME, 84, page_fd, 0, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] cross-directory rename failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_target_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_source_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = mkdir_dir_handle;
    openat->rights = FILED_V2_RIGHT_STAT;
    snprintf(openat->name, sizeof(openat->name), "%s", "child.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 85, page_fd, 0, &result);
    if (status == 0 || result != 0) {
        fprintf(stderr,
            "[filed-smoke] cross-directory old path unexpectedly exists status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_target_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_source_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return -48;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = root_dir_handle;
    openat->rights = FILED_V2_RIGHT_READ | FILED_V2_RIGHT_STAT;
    snprintf(openat->name, sizeof(openat->name), "%s", "filed-cross-rename.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 86, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] cross-directory new path open failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_target_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_source_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -49;
    }
    const uint64_t cross_renamed_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = cross_renamed_handle;
    io->offset = 0;
    io->length = sizeof(cross_dir_payload) - 1u;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_PREAD, 87, page_fd, 0, &result);
    if (status != 0 ||
        result != sizeof(cross_dir_payload) - 1u ||
        memcmp(io->data, cross_dir_payload, sizeof(cross_dir_payload) - 1u) != 0)
    {
        fprintf(stderr,
            "[filed-smoke] cross-directory readback failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_renamed_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_target_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_source_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -50;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 88, -1, cross_renamed_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close cross-directory file failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_target_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_source_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    rename = (filed_v2_rename_t *)page;
    rename->old_dir_handle = mkdir_dir_handle;
    rename->new_dir_handle = root_dir_handle;
    snprintf(rename->old_name, sizeof(rename->old_name), "%s", "replace.tmp");
    snprintf(rename->new_name, sizeof(rename->new_name), "%s", "filed-cross-replace.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_RENAME, 89, page_fd, 0, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] cross-directory replace while open failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_target_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_source_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = cross_replace_target_handle;
    io->offset = 0;
    io->length = 4;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_PREAD, 90, page_fd, 0, &result);
    if (status != 0 || result != 0) {
        fprintf(stderr,
            "[filed-smoke] cross-directory replaced open should read eof status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_target_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_source_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -54;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = root_dir_handle;
    openat->rights = FILED_V2_RIGHT_READ | FILED_V2_RIGHT_STAT;
    snprintf(openat->name, sizeof(openat->name), "%s", "filed-cross-replace.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 91, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] cross-directory replaced path open failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_target_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_source_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -55;
    }
    const uint64_t cross_replace_path_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = cross_replace_path_handle;
    io->offset = 0;
    io->length = sizeof(cross_replace_payload) - 1u;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_PREAD, 92, page_fd, 0, &result);
    if (status != 0 ||
        result != sizeof(cross_replace_payload) - 1u ||
        memcmp(io->data, cross_replace_payload, sizeof(cross_replace_payload) - 1u) != 0)
    {
        fprintf(stderr,
            "[filed-smoke] cross-directory replaced path readback failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_path_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_target_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_source_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -56;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 93, -1, cross_replace_path_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close cross-directory replace path failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_target_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_source_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 94, -1, cross_replace_target_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close cross-directory replaced target failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, cross_replace_source_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 95, -1, cross_replace_source_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close cross-directory replace source failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    filed_smoke_best_effort_unlink(
        endpoint_fd,
        page_fd,
        page,
        96,
        root_dir_handle,
        "filed-cross-rename.tmp");
    filed_smoke_best_effort_unlink(
        endpoint_fd,
        page_fd,
        page,
        97,
        root_dir_handle,
        "filed-cross-replace.tmp");

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    mkdir = (filed_v2_mkdir_t *)page;
    mkdir->dir_handle = root_dir_handle;
    mkdir->mode = 0755;
    snprintf(mkdir->name, sizeof(mkdir->name), "%s", "filed-dir-rename-old");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_MKDIR, 98, page_fd, 0, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] mkdir directory rename source failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = root_dir_handle;
    openat->rights =
        FILED_V2_RIGHT_LOOKUP |
        FILED_V2_RIGHT_STAT |
        FILED_V2_RIGHT_CREATE |
        FILED_V2_RIGHT_REMOVE;
    openat->open_flags = FILED_V2_OPEN_DIRECTORY;
    snprintf(openat->name, sizeof(openat->name), "%s", "filed-dir-rename-old");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 108, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] open directory before rename failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 109, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 110, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -59;
    }
    const uint64_t dir_rename_open_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    rename = (filed_v2_rename_t *)page;
    rename->old_dir_handle = root_dir_handle;
    rename->new_dir_handle = root_dir_handle;
    snprintf(rename->old_name, sizeof(rename->old_name), "%s", "filed-dir-rename-old");
    snprintf(rename->new_name, sizeof(rename->new_name), "%s", "filed-dir-rename-new");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_RENAME, 99, page_fd, 0, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] directory rename failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 110, -1, dir_rename_open_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 100, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 101, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = root_dir_handle;
    openat->rights = FILED_V2_RIGHT_STAT;
    openat->open_flags = FILED_V2_OPEN_DIRECTORY;
    snprintf(openat->name, sizeof(openat->name), "%s", "filed-dir-rename-old");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 100, page_fd, 0, &result);
    if (status == 0 || result != 0) {
        fprintf(stderr,
            "[filed-smoke] old directory rename path unexpectedly exists status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 110, -1, dir_rename_open_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 101, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 102, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return -57;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = root_dir_handle;
    openat->rights =
        FILED_V2_RIGHT_LOOKUP |
        FILED_V2_RIGHT_STAT |
        FILED_V2_RIGHT_GETDENTS |
        FILED_V2_RIGHT_CREATE |
        FILED_V2_RIGHT_REMOVE |
        FILED_V2_RIGHT_RENAME;
    openat->open_flags = FILED_V2_OPEN_DIRECTORY;
    snprintf(openat->name, sizeof(openat->name), "%s", "filed-dir-rename-new");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 101, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] new directory rename path open failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 110, -1, dir_rename_open_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 102, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 103, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -58;
    }
    const uint64_t renamed_dir_handle = result;

    const char dir_lifetime_payload[] = "directory-handle-live";
    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = dir_rename_open_handle;
    openat->rights =
        FILED_V2_RIGHT_READ |
        FILED_V2_RIGHT_WRITE |
        FILED_V2_RIGHT_STAT;
    openat->open_flags = FILED_V2_OPEN_CREATE | FILED_V2_OPEN_TRUNCATE;
    snprintf(openat->name, sizeof(openat->name), "%s", "alive.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 109, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] create through renamed open directory failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 110, -1, renamed_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 111, -1, dir_rename_open_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 112, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 113, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -60;
    }
    const uint64_t dir_lifetime_child_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = dir_lifetime_child_handle;
    io->length = sizeof(dir_lifetime_payload) - 1u;
    memcpy(io->data, dir_lifetime_payload, sizeof(dir_lifetime_payload) - 1u);
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_WRITE, 110, page_fd, 0, &result);
    if (status != 0 || result != sizeof(dir_lifetime_payload) - 1u) {
        fprintf(stderr,
            "[filed-smoke] write through renamed open directory failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 111, -1, dir_lifetime_child_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 112, -1, renamed_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 113, -1, dir_rename_open_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 114, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 115, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -61;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 111, -1, dir_lifetime_child_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close renamed directory child failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 112, -1, renamed_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 113, -1, dir_rename_open_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 114, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 115, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = renamed_dir_handle;
    openat->rights = FILED_V2_RIGHT_READ | FILED_V2_RIGHT_STAT;
    openat->open_flags = 0;
    snprintf(openat->name, sizeof(openat->name), "%s", "alive.tmp");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 112, page_fd, 0, &result);
    if (status != 0 || result == 0) {
        fprintf(stderr,
            "[filed-smoke] reopen renamed directory child failed status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 113, -1, renamed_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 114, -1, dir_rename_open_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 115, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 116, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -62;
    }
    const uint64_t dir_lifetime_reopen_handle = result;

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    io = (filed_v2_io_t *)page;
    io->handle = dir_lifetime_reopen_handle;
    io->offset = 0;
    io->length = sizeof(dir_lifetime_payload) - 1u;
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_PREAD, 113, page_fd, 0, &result);
    if (status != 0 ||
        result != sizeof(dir_lifetime_payload) - 1u ||
        memcmp(io->data, dir_lifetime_payload, sizeof(dir_lifetime_payload) - 1u) != 0)
    {
        fprintf(stderr,
            "[filed-smoke] renamed directory child readback failed status=%d bytes=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 114, -1, dir_lifetime_reopen_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 115, -1, renamed_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 116, -1, dir_rename_open_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 117, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 118, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status != 0 ? status : -63;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 114, -1, dir_lifetime_reopen_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close renamed directory child reopen failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 115, -1, renamed_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 116, -1, dir_rename_open_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 117, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 118, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }
    filed_smoke_best_effort_unlink(
        endpoint_fd,
        page_fd,
        page,
        115,
        renamed_dir_handle,
        "alive.tmp");

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 116, -1, dir_rename_open_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close old renamed directory handle failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 117, -1, renamed_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 118, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 119, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 102, -1, renamed_dir_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close renamed directory failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 103, -1, mkdir_dir_handle, &result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 104, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }
    filed_smoke_best_effort_rmdir(
        endpoint_fd,
        page_fd,
        page,
        103,
        root_dir_handle,
        "filed-dir-rename-new");

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 104, -1, mkdir_dir_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close mkdir dir failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    rmdir = (filed_v2_rmdir_t *)page;
    rmdir->dir_handle = root_dir_handle;
    snprintf(rmdir->name, sizeof(rmdir->name), "%s", "filed-mkdir-smoke");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_RMDIR, 105, page_fd, 0, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] rmdir failed status=%d\n", status);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_SMOKE_PAGE_SIZE);
    openat = (filed_v2_openat_t *)page;
    openat->dir_handle = root_dir_handle;
    openat->rights = FILED_V2_RIGHT_STAT;
    openat->open_flags = FILED_V2_OPEN_DIRECTORY;
    snprintf(openat->name, sizeof(openat->name), "%s", "filed-mkdir-smoke");
    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_OPENAT, 106, page_fd, 0, &result);
    if (status == 0 || result != 0) {
        fprintf(stderr,
            "[filed-smoke] rmdir path unexpectedly exists status=%d handle=%llu\n",
            status,
            (unsigned long long)result);
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return -44;
    }

    status = filed_smoke_mutation_stress(endpoint_fd, page_fd, page, root_dir_handle);
    if (status != 0) {
        (void)filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 99, -1, root_dir_handle, &result);
        destroy_wire_page(page_fd, page);
        return status;
    }

    result = 0;
    status = filed_call(endpoint_fd, FILED_V2_OP_VFS_CLOSE, 107, -1, root_dir_handle, &result);
    if (status != 0) {
        fprintf(stderr, "[filed-smoke] close root dir failed status=%d\n", status);
        destroy_wire_page(page_fd, page);
        return status;
    }

    destroy_wire_page(page_fd, page);

    printf("[filed-smoke] ready path=/sbin/filed.elf bytes=%llu write=ok lifecycle=ok\n",
        (unsigned long long)bytes_read);
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    if (argc < 2 ||
        argv == NULL ||
        argv[0] == NULL ||
        argv[1] == NULL ||
        strcmp(argv[0], "/sbin/filed_smoke.elf") != 0 ||
        strcmp(argv[1], "--execve-smoke") != 0 ||
        !has_env_value(envp, "PACHA_FILED_EXEC_SMOKE=1"))
    {
        fprintf(stderr,
            "[filed-smoke] exec argv/env failed argc=%d argv0=%s argv1=%s\n",
            argc,
            argv != NULL && argv[0] != NULL ? argv[0] : "(null)",
            argv != NULL && argv[1] != NULL ? argv[1] : "(null)");
        return 2;
    }

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
