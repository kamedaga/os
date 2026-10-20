#include "../userland/lpr_supervisor/src/accounts.c"
#include <assert.h>

static unsigned char *wire;
static size_t wire_size;
static const char *source[2];
static unsigned closes, live_handles, next_file;
static int fail_read;
pacha_fd_t pacha_vmo_create(uint64_t size, uint64_t rights, uint32_t flags)
{
    assert(!wire && (rights & PACHA_FD_RIGHT_TRANSFER) && !flags);
    wire_size = size; wire = calloc(1, size); assert(wire); return 20;
}
void *pacha_mmap(pacha_fd_t fd, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset)
{ assert(fd == 20 && size == wire_size && prot == 3 && flags == PACHA_MMAP_SHARED && !offset); return wire; }
int pacha_munmap(void *address, uint64_t size)
{ assert(address == wire && size == wire_size); return 0; }
int pacha_fd_close(pacha_fd_t fd) { assert(fd == 20); closes++; free(wire); wire = NULL; return 0; }
int pacha_fd_get_info(pacha_fd_t fd, struct pacha_fd_info *info)
{ (void)fd; (void)info; assert(0); return -EIO; }
int pacha_ipc_call(pacha_fd_t endpoint, const struct pacha_ipc_msg *message,
    struct pacha_ipc_msg *reply, uint64_t timeout, int *sent)
{
    assert(endpoint == 249 && message->fd_count == 1 && message->fds[0].fd == 20);
    assert(timeout == PACHA_FD_WAIT_FOREVER);
    if (sent) *sent = 1;
    kb2_fs_request_t request;
    assert(!kb2_fs_request_decode(wire, message->word1, message->word1, &request));
    kb2_fs_response_t response = {.opcode = request.opcode, .generation = request.generation,
        .sequence = request.sequence};
    if (request.opcode == KB2_FILESYSTEM_OP_OPENAT2) {
        assert(next_file < 2 && !strcmp((char *)wire + request.path.offset,
            next_file ? "/etc/group" : "/etc/passwd"));
        response.handle = ++next_file; live_handles++;
    } else if (request.opcode == KB2_FILESYSTEM_OP_PREAD) {
        assert(request.handle >= 1 && request.handle <= 2);
        const char *data = source[request.handle - 1];
        if (fail_read) response.result = -EIO;
        else {
            size_t length = strlen(data);
            assert(request.offset >= 0 && (uint64_t)request.offset <= length);
            size_t bytes = length - request.offset;
            if (bytes > request.length) bytes = request.length;
            response.result = bytes; response.data_length = bytes;
            memcpy(wire + KB2_FILESYSTEM_RESPONSE_SIZE, data + request.offset, bytes);
        }
    } else {
        assert(request.opcode == KB2_FILESYSTEM_OP_CLOSE && live_handles);
        live_handles--;
    }
    assert(!kb2_fs_response_encode(wire, wire_size, &response));
    reply->word0 = request.opcode; reply->word1 = response.result;
    reply->word2 = KB2_FILESYSTEM_RESPONSE_SIZE + response.data_length;
    reply->word3 = request.sequence; reply->fd_count = 0;
    return 0;
}
int main(void)
{
    /* Cross a full IPC read boundary, and require a final EOF read. */
    char text[20000]; memset(text, 'x', 18000); text[0] = '#'; text[18000] = '\n';
    strcpy(text + 18001, "root:x:0:0:root:/root:/bin/bash\n");
    source[0] = text; source[1] = "root:x:0:\n";
    struct pacha_accounts db = {0};
    assert(!lprs_accounts_load(249, &db));
    assert(pacha_account_by_uid(&db, 0) && !live_handles && closes == 9);
    const struct pacha_account *before = pacha_account_by_uid(&db, 0);
    next_file = 0; fail_read = 1;
    assert(lprs_accounts_load(249, &db) == -EIO && !live_handles);
    assert(pacha_account_by_uid(&db, 0) == before);
    pacha_accounts_destroy(&db);
    puts("ACCOUNTS_FILED=OK control-wire multi-read eof close-on-error immutable-snapshot");
}
