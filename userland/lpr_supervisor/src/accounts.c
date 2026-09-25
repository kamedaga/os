#include "accounts.h"
#include "filed/ipc_protocol.h"
#include "filed/payload.h"
#include "pacha/ipc.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct reader { int endpoint, fd; void *page; uint64_t sequence; };

static int call(struct reader *r, unsigned op, const void *payload, size_t size, uint64_t *result)
{
    pacha_service_envelope_t *h = r->page;
    memset(r->page, 0, FILED_PAGE_BYTES);
    *h = (pacha_service_envelope_t){ .magic = PACHA_SERVICE_REQUEST_MAGIC,
        .abi_version = PACHA_SERVICE_ABI_VERSION, .service_id = FILED_SERVICE_ID,
        .op = op, .request_id = ++r->sequence, .payload_size = size };
    memcpy((char *)r->page + PACHA_SERVICE_HEADER_BYTES, payload, size);
    struct pacha_ipc_fd cap = { .fd = (uint64_t)r->fd,
        .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE };
    struct pacha_ipc_msg request = { .word0 = h->magic, .word3 = r->sequence, .fds = &cap, .fd_count = 1 };
    int reply_fd = pacha_ipc_call(r->endpoint, &request);
    if (reply_fd < 16) return EIO;
    struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS] = {{0}};
    struct pacha_ipc_msg reply = { .fds = fds, .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS };
    int received = pacha_ipc_recv_wait(reply_fd, &reply, PACHA_FD_WAIT_FOREVER);
    (void)pacha_fd_close(reply_fd);
    for (unsigned i = 0; i < reply.fd_count; i++) (void)pacha_fd_close((int)fds[i].fd);
    if (received || reply.fd_count || reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
        reply.word3 != r->sequence || h->magic != PACHA_SERVICE_REPLY_MAGIC ||
        h->request_id != r->sequence || h->status > 0 || h->status < -4095) return EIO;
    *result = h->result;
    return -(int)h->status;
}

static int read_file(struct reader *r, const char *path, char **text, size_t *length)
{
    filed_path_request_t open = { .rights = FILED_RIGHT_READ };
    memcpy(open.path, path, strlen(path) + 1);
    uint64_t handle = 0, bytes = 0;
    int status = call(r, FILED_OP_VFS_OPENAT, &open, sizeof(open), &handle);
    if (status) return status;
    size_t capacity = 0;
    for (;;) {
        filed_io_t io = { .handle = handle, .offset = *length, .length = FILED_IO_BYTES };
        status = call(r, FILED_OP_VFS_PREAD, &io, sizeof(io), &bytes);
        if (status || !bytes) break;
        /* Bound boot-time memory use; reject large input, never truncate. */
        if (bytes > FILED_IO_BYTES || *length + bytes > 1024 * 1024) { status = E2BIG; break; }
        if (*length + bytes > capacity) {
            capacity = *length + (size_t)bytes;
            void *next = realloc(*text, capacity);
            if (!next) { status = ENOMEM; break; }
            *text = next;
        }
        const filed_io_t *response = (const void *)((char *)r->page + PACHA_SERVICE_HEADER_BYTES);
        memcpy(*text + *length, response->data, (size_t)bytes);
        *length += (size_t)bytes;
    }
    filed_handle_request_t close = { .handle = handle };
    int closed = call(r, FILED_OP_VFS_CLOSE, &close, sizeof(close), &bytes);
    return status ? status : closed;
}

int lprs_accounts_load(int endpoint, struct pacha_accounts *db)
{
    struct reader r = { .endpoint = endpoint };
    r.fd = pacha_vmo_create(FILED_PAGE_BYTES, PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE, PACHA_FD_FLAG_PRIVATE);
    if (r.fd < 16) return ENOMEM;
    r.page = pacha_mmap(r.fd, FILED_PAGE_BYTES, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!r.page) { (void)pacha_fd_close(r.fd); return ENOMEM; }
    char *passwd = NULL, *group = NULL;
    size_t plen = 0, glen = 0;
    int status = read_file(&r, "/etc/passwd", &passwd, &plen);
    if (status) fprintf(stderr, "[lprs-accounts] read /etc/passwd status=%d\n", status);
    if (!status) status = read_file(&r, "/etc/group", &group, &glen);
    if (status) fprintf(stderr, "[lprs-accounts] read definitions status=%d passwd=%zu group=%zu\n", status, plen, glen);
    if (!status) {
        status = pacha_accounts_parse(db, passwd ? passwd : "", plen, group ? group : "", glen);
        if (status) fprintf(stderr, "[lprs-accounts] parse status=%d passwd=%zu group=%zu\n", status, plen, glen);
    }
    free(passwd); free(group);
    (void)pacha_munmap(r.page, FILED_PAGE_BYTES);
    (void)pacha_fd_close(r.fd);
    return status;
}
