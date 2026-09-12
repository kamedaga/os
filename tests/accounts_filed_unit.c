#include "../userland/lpr_supervisor/src/accounts.c"
#include <assert.h>

static _Alignas(8) unsigned char wire[FILED_PAGE_BYTES];
static const char *source[2];
static unsigned closes, live_handles, next_file;
static int fail_read;
int pacha_vmo_create(uint64_t size, uint64_t rights, uint32_t flags)
{
    assert(size == sizeof(wire) && (rights & PACHA_FD_RIGHT_TRANSFER) && flags == PACHA_FD_FLAG_PRIVATE);
    return 20;
}
void *pacha_mmap(int fd, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset)
{ assert(fd == 20 && size == sizeof(wire) && prot == 3 && flags == PACHA_MMAP_SHARED && !offset); return wire; }
int pacha_munmap(void *address, uint64_t size)
{ assert(address == wire && size == sizeof(wire)); return 0; }
int pacha_fd_close(int fd) { assert(fd == 20 || fd == 30); closes++; return 0; }
int pacha_ipc_call(int endpoint, const struct pacha_ipc_msg *message)
{
    assert(endpoint == 249 && message->fd_count == 1 && message->fds[0].fd == 20);
    pacha_service_envelope_t *h = (void *)wire;
    assert(pacha_service_request_is_valid(h, FILED_SERVICE_ID) && h->request_id == message->word3);
    void *body = wire + PACHA_SERVICE_HEADER_BYTES;
    uint64_t result = 0; int status = 0;
    if (h->op == FILED_OP_VFS_OPENAT) {
        const filed_path_request_t *open = body;
        assert(h->payload_size == sizeof(*open));
        assert(open->rights == FILED_RIGHT_READ && !open->dir_handle && !open->flags);
        assert(next_file < 2 && !strcmp(open->path, next_file ? "/etc/group" : "/etc/passwd"));
        result = ++next_file; live_handles++;
    } else if (h->op == FILED_OP_VFS_PREAD) {
        filed_io_t *io = body;
        assert(h->payload_size == sizeof(*io) && io->handle >= 1 && io->handle <= 2);
        const char *data = source[io->handle - 1];
        if (fail_read) status = -EIO;
        else {
            size_t length = strlen(data);
            assert(io->offset <= length);
            result = length - io->offset;
            if (result > io->length) result = io->length;
            memcpy(io->data, data + io->offset, (size_t)result);
        }
    } else {
        assert(h->op == FILED_OP_VFS_CLOSE && h->payload_size == sizeof(filed_handle_request_t));
        assert(live_handles); live_handles--;
    }
    pacha_service_envelope_t request = *h;
    pacha_service_reply_init(h, &request, status, 0, result, 0);
    return 30;
}
int pacha_ipc_recv_wait(int fd, struct pacha_ipc_msg *reply, uint64_t timeout)
{
    assert(fd == 30 && timeout == PACHA_FD_WAIT_FOREVER);
    pacha_service_envelope_t *h = (void *)wire;
    reply->word0 = PACHA_SERVICE_REPLY_MAGIC; reply->word3 = h->request_id;
    reply->fd_count = 0; return 0;
}
int main(void)
{
    /* Cross a full IPC read boundary, and require a final EOF read. */
    char text[9000]; memset(text, 'x', 8000); text[0] = '#'; text[8000] = '\n';
    strcpy(text + 8001, "root:x:0:0:root:/root:/bin/bash\n");
    source[0] = text; source[1] = "root:x:0:\n";
    struct pacha_accounts db = {0};
    assert(!lprs_accounts_load(249, &db));
    assert(pacha_account_by_uid(&db, 0) && !live_handles && closes == 10);
    const struct pacha_account *before = pacha_account_by_uid(&db, 0);
    next_file = 0; fail_read = 1;
    assert(lprs_accounts_load(249, &db) == EIO && !live_handles);
    assert(pacha_account_by_uid(&db, 0) == before);
    pacha_accounts_destroy(&db);
    puts("ACCOUNTS_FILED=OK control-wire multi-read eof close-on-error immutable-snapshot");
}
