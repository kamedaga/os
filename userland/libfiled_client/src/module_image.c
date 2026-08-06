#include "filed_client/module_image.h"

#include "filed/flags.h"
#include "filed/ipc_protocol.h"
#include "filed/payload.h"
#include "pacha/ipc.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static uint64_t next_request_id;

static void *payload(void *page)
{
    return (unsigned char *)page + PACHA_SERVICE_HEADER_BYTES;
}

static int create_page(int *out_fd, void **out_page)
{
    const uint64_t rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    const int fd = pacha_vmo_create(FILED_PAGE_BYTES, rights, 0);
    if (fd < 16) return fd;
    void *page = pacha_mmap(fd, FILED_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (page == NULL) {
        (void)pacha_fd_close(fd);
        return -5;
    }
    memset(page, 0, FILED_PAGE_BYTES);
    *out_fd = fd;
    *out_page = page;
    return 0;
}

static void destroy_page(int fd, void *page)
{
    if (page != NULL) (void)pacha_munmap(page, FILED_PAGE_BYTES);
    if (fd >= 16) (void)pacha_fd_close(fd);
}

static int call(
    int endpoint_fd,
    uint32_t op,
    int page_fd,
    void *page,
    uint64_t payload_size,
    uint64_t *out_result,
    struct pacha_ipc_fd *out_fd)
{
    if (endpoint_fd < 16 || page_fd < 16 || page == NULL) return -22;
    const uint64_t request_id = ++next_request_id;
    pacha_service_envelope_t *header = page;
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = FILED_SERVICE_ID;
    header->op = op;
    header->flags = PACHA_SERVICE_FLAG_PAGE_PAYLOAD;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = payload_size;
    header->fd_count = 0;

    struct pacha_ipc_fd request_fd = {
        .fd = (uint64_t)(uint32_t)page_fd,
        .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE,
    };
    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word3 = request_id,
        .fds = &request_fd,
        .fd_count = 1,
    };
    const int reply_endpoint = pacha_ipc_call(endpoint_fd, &request);
    if (reply_endpoint < 16) return reply_endpoint;

    struct pacha_ipc_fd reply_fd;
    memset(&reply_fd, 0, sizeof(reply_fd));
    struct pacha_ipc_msg reply = {
        .fds = &reply_fd,
        .fd_capacity = out_fd != NULL ? 1u : 0u,
    };
    const int recv_status = pacha_ipc_recv_wait(
        reply_endpoint, &reply, PACHA_FD_WAIT_FOREVER);
    (void)pacha_fd_close(reply_endpoint);
    if (recv_status != 0) return recv_status;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
        header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        header->service_id != FILED_SERVICE_ID || header->op != op ||
        header->request_id != request_id || header->status != 0)
        return header->status != 0 ? (int)header->status : -5;
    if (out_result != NULL) *out_result = header->result;
    if (out_fd != NULL) {
        if (reply.fd_count != 1 || reply_fd.fd < 16) return -5;
        *out_fd = reply_fd;
    }
    return 0;
}

static int close_handle(int endpoint_fd, uint64_t handle)
{
    int page_fd = -1;
    void *page = NULL;
    int status = create_page(&page_fd, &page);
    if (status != 0) return status;
    filed_handle_request_t *request = payload(page);
    request->handle = handle;
    status = call(endpoint_fd, FILED_OP_VFS_CLOSE, page_fd, page,
        sizeof(*request), NULL, NULL);
    destroy_page(page_fd, page);
    return status;
}

int filed_client_load_module_image(
    int endpoint_fd,
    const char *path,
    const char *name,
    struct filed_client_module_image *out)
{
    if (endpoint_fd < 16 || path == NULL || name == NULL || out == NULL)
        return -22;
    memset(out, 0, sizeof(*out));

    int page_fd = -1;
    void *page = NULL;
    int status = create_page(&page_fd, &page);
    if (status != 0) return status;
    filed_path_request_t *open = payload(page);
    open->dir_handle = 0;
    open->rights = FILED_RIGHT_READ | FILED_RIGHT_STAT;
    open->flags = FILED_OPEN_CLOEXEC;
    if (snprintf(open->path, sizeof(open->path), "%s", path) >= (int)sizeof(open->path)) {
        destroy_page(page_fd, page);
        return -36;
    }
    uint64_t handle = 0;
    status = call(endpoint_fd, FILED_OP_VFS_OPENAT, page_fd, page,
        sizeof(*open), &handle, NULL);
    if (status != 0) {
        destroy_page(page_fd, page);
        return status;
    }

    memset(page, 0, FILED_PAGE_BYTES);
    filed_statx_t *stat = payload(page);
    stat->handle = handle;
    status = call(endpoint_fd, FILED_OP_VFS_STAT, page_fd, page,
        sizeof(*stat), NULL, NULL);
    const uint64_t image_size = stat->size;
    destroy_page(page_fd, page);
    if (status != 0 || image_size == 0 ||
        image_size > FILED_CLIENT_MODULE_MAX_BYTES) {
        (void)close_handle(endpoint_fd, handle);
        return status != 0 ? status : -7;
    }

    page_fd = -1;
    page = NULL;
    status = create_page(&page_fd, &page);
    if (status != 0) {
        (void)close_handle(endpoint_fd, handle);
        return status;
    }
    filed_file_vmo_request_t *request = payload(page);
    request->handle = handle;
    request->length = image_size;
    struct pacha_ipc_fd image_fd;
    memset(&image_fd, 0, sizeof(image_fd));
    uint64_t loaded = 0;
    status = call(endpoint_fd, FILED_OP_VFS_FILE_VMO, page_fd, page,
        sizeof(*request), &loaded, &image_fd);
    destroy_page(page_fd, page);
    const int close_status = close_handle(endpoint_fd, handle);
    if (status != 0) return status;
    if (close_status != 0 || loaded == 0 || loaded > FILED_CLIENT_MODULE_MAX_BYTES) {
        (void)pacha_fd_close((int)image_fd.fd);
        return close_status != 0 ? close_status : -5;
    }
    const uint64_t map_bytes = (loaded + 4095u) & ~4095ull;
    const unsigned char *mapped = pacha_mmap((int)image_fd.fd, map_bytes,
        PACHA_PROT_READ, PACHA_MMAP_SHARED, 0);
    if (mapped == NULL) {
        (void)pacha_fd_close((int)image_fd.fd);
        return -5;
    }
    unsigned char *copy = malloc((size_t)loaded);
    if (copy == NULL) {
        (void)pacha_munmap((void *)mapped, map_bytes);
        (void)pacha_fd_close((int)image_fd.fd);
        return -12;
    }
    memcpy(copy, mapped, (size_t)loaded);
    (void)pacha_munmap((void *)mapped, map_bytes);
    (void)pacha_fd_close((int)image_fd.fd);
    out->data = copy;
    out->size = (size_t)loaded;
    out->name = name;
    return 0;
}

void filed_client_release_module_image(struct filed_client_module_image *image)
{
    if (image == NULL) return;
    free(image->data);
    memset(image, 0, sizeof(*image));
}
