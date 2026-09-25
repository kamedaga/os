/* Native QEMU regression for PROCESS_CREATE's failure transaction. */
#include "pacha/ipc.h"
#include "pacha/launch.h"
#include "pacha/syscall.h"
#include "filed/payload.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "filed_identity_native.h"

#define PROCESS_RIGHTS (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_KILL)
#define SOURCE_RIGHTS (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | \
    PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_POLL)

static int discard(int fd)
{
    if (fd < 16) return -1;
    const int status = (int)pacha_syscall2(PACHA_PROCESS_SYSCALL_KILL, (uint64_t)fd, 0);
    return pacha_fd_close(fd) == 0 ? status : -1;
}

static int file_call(int page_fd, void *page, unsigned op, unsigned size, uint64_t *result)
{
    pacha_service_envelope_t *header = page;
    memset(header, 0, sizeof(*header));
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = FILED_SERVICE_ID;
    header->op = op;
    header->flags = PACHA_SERVICE_FLAG_PAGE_PAYLOAD;
    header->request_id = op + 100;
    header->trace_id = header->request_id;
    header->payload_size = size;
    struct pacha_ipc_fd cap = {.fd = (uint64_t)page_fd,
        .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE};
    struct pacha_ipc_msg request = {.word0 = header->magic,
        .word3 = header->request_id, .fds = &cap, .fd_count = 1};
    const int reply_fd = pacha_ipc_call(240, &request);
    if (reply_fd < 16) return -1;
    struct pacha_ipc_msg reply = {0};
    int status;
    do { status = pacha_ipc_recv_wait(reply_fd, &reply, UINT64_MAX); }
    while (status == -2 || status == -5);
    (void)pacha_fd_close(reply_fd);
    if (status || header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        header->request_id != request.word3 || reply.word3 != request.word3 ||
        reply.word0 != header->magic || header->status) {
        fprintf(stderr, "native file call op=%u recv=%d status=%lld magic=%llx\n",
            op, status, (long long)header->status, (unsigned long long)header->magic);
        return -1;
    }
    *result = header->result;
    return 0;
}

/* Use the current service contract, independently of the legacy native libc
 * filesystem adapter. This checks authority delivered to the exec child. */
static int read_file(void)
{
    const int page_fd = pacha_vmo_create(FILED_PAGE_BYTES, PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE | PACHA_FD_RIGHT_TRANSFER, 0);
    if (page_fd < 16) return -1;
    void *page = pacha_mmap(page_fd, FILED_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!page) { (void)pacha_fd_close(page_fd); return -1; }
    memset(page, 0, FILED_PAGE_BYTES);
    void *payload = (char *)page + PACHA_SERVICE_HEADER_BYTES;
    filed_path_request_t *open = payload;
    open->rights = FILED_RIGHT_READ | FILED_RIGHT_STAT;
    strcpy(open->path, "/etc/os-release");
    uint64_t handle = 0, count = 0;
    int status = file_call(page_fd, page, FILED_OP_VFS_OPENAT, sizeof(*open), &handle);
    if (!status) {
        memset(payload, 0, sizeof(filed_io_t));
        filed_io_t *io = payload;
        io->handle = handle;
        io->length = 64;
        status = file_call(page_fd, page, FILED_OP_VFS_PREAD, sizeof(*io), &count);
        if (!count) status = -1;
        filed_handle_request_t *close = payload;
        memset(close, 0, sizeof(*close));
        close->handle = handle;
        if (file_call(page_fd, page, FILED_OP_VFS_CLOSE, sizeof(*close), &count)) status = -1;
    }
    (void)pacha_munmap(page, FILED_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return status;
}

static int exec_child(const char *path, int final)
{
    struct pacha_fd_info info;
    if (pacha_fd_get_info(240, &info) != 0 ||
        info.rights != (PACHA_LAUNCH_CLIENT | PACHA_FD_RIGHT_TRANSFER)) return 20;
    struct pacha_fd_table_info table;
    if (pacha_fd_table(0, &table) != 0) return 21;
    for (unsigned fd = 16; fd < table.capacity; ++fd)
        if (fd != 240 && pacha_fd_get_info((int)fd, &info) == 0) {
            fprintf(stderr, "native exec unexpected fd=%u rights=%llu\n", fd,
                (unsigned long long)info.rights);
            return 22;
        }
    if (table.free_slots != table.capacity - 17) return 26;
    if (read_file() != 0) return 23;
    if (final) {
        puts("NATIVE_EXEC_GRANTS=OK twice filesystem client-only no-ambient-inherit");
        return 0;
    }
    /* This descriptor must not appear in the next native exec. */
    if (pacha_eventfd_create(0, SOURCE_RIGHTS, PACHA_FD_FLAG_INHERIT) < 16) return 24;
    char *args[] = {(char *)path, "--exec-final", NULL};
    execve(path, args, NULL);
    fprintf(stderr, "native explicit exec failed errno=%d\n", errno);
    return 25;
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        const int status = exec_child(argv[0], !strcmp(argv[1], "--exec-final"));
        if (status) fprintf(stderr, "NATIVE_EXEC_GRANTS=FAIL stage=%s status=%d errno=%d\n",
            argv[1], status, errno);
        return status;
    }
    if (filed_identity_native() != 0) return 30;
    struct pacha_fd_table_info before, after;
    if (pacha_fd_table(0, &before) != 0) return 1;
    const int source = pacha_eventfd_create(0, SOURCE_RIGHTS,
        PACHA_FD_FLAG_INHERIT | PACHA_FD_FLAG_PRIVATE);
    if (source < 16) return 2;
    struct pacha_fd_info original, current;
    if (pacha_fd_get_info(source, &original) != 0) return 3;
    const struct pacha_process_fd_grant grant = {
        .source_fd = (uint64_t)source, .target_fd = 4095,
        .rights = PACHA_FD_RIGHT_INSPECT, .flags = PACHA_FD_FLAG_PRIVATE,
    };
    if (discard(pacha_process_create(PROCESS_RIGHTS, 0, NULL, 0)) != 0 ||
        discard(pacha_process_create(PROCESS_RIGHTS, 0, &grant, 1)) != 0) return 4;
    struct pacha_process_fd_grant invalid = grant;
    invalid.rights |= PACHA_FD_RIGHT_RECV;
    if (pacha_process_create(PROCESS_RIGHTS, 0, &invalid, 1) >= 16) return 5;
    const struct pacha_process_fd_grant duplicate[] = {grant, grant};
    if (pacha_process_create(PROCESS_RIGHTS, 0, duplicate, 2) >= 16) return 6;
    if (pacha_syscall5(PACHA_PROCESS_SYSCALL_CREATE, 0, PROCESS_RIGHTS, 0,
            UINT64_MAX - 7, 1) != PACHA_SYSCALL_ERR_INVALID) return 7;
    const long no_transfer = pacha_fd_fcntl(source, PACHA_FD_FCNTL_DUP, 16,
        PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE);
    if (no_transfer < 16) return 8;
    invalid = grant;
    invalid.source_fd = (uint64_t)no_transfer;
    if (pacha_process_create(PROCESS_RIGHTS, 0, &invalid, 1) >= 16) return 9;
    (void)pacha_fd_close((int)no_transfer);

    int fillers[PACHA_FD_TABLE_LIMIT];
    unsigned count = 0;
    for (;;) {
        const long fd = pacha_fd_fcntl(source, PACHA_FD_FCNTL_DUP, 16, SOURCE_RIGHTS);
        if (fd < 16) break;
        if (count >= PACHA_FD_TABLE_LIMIT) return 10;
        fillers[count++] = (int)fd;
    }
    if (!count || pacha_fd_table(0, &after) != 0 || after.free_slots != 0) return 11;
    /* Each request installs a high, non-CLOSE grant in a new child, then fails
     * only when publishing the process handle into this full parent table. */
    for (unsigned i = 0; i < 512; ++i)
        if (pacha_syscall5(PACHA_PROCESS_SYSCALL_CREATE, 0, PROCESS_RIGHTS, 0,
                (uint64_t)(uintptr_t)&grant, 1) != PACHA_SYSCALL_ERR_ALLOC) return 12;
    (void)pacha_fd_close(fillers[--count]);
    if (discard(pacha_process_create(PROCESS_RIGHTS, 0, &grant, 1)) != 0) return 13;
    while (count) (void)pacha_fd_close(fillers[--count]);
    if (pacha_fd_get_info(source, &current) != 0 ||
        memcmp(&original, &current, sizeof(original)) != 0) return 14;
    (void)pacha_fd_close(source);
    if (pacha_fd_table(0, &after) != 0 || before.capacity != after.capacity ||
        before.free_slots != after.free_slots) return 15;
    puts("PROCESS_CREATE_GRANTS=OK empty private attenuation no-transfer duplicate bad-pointer handle-full-512 parent-unchanged");
    fflush(stdout);
    if (pacha_eventfd_create(0, SOURCE_RIGHTS, PACHA_FD_FLAG_INHERIT) < 16) return 16;
    char *args[] = {argv[0], "--exec-child", NULL};
    execve(argv[0], args, NULL);
    fprintf(stderr, "native explicit exec failed errno=%d\n", errno);
    return 17;
}
