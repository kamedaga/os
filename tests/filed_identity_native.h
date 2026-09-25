/* Real filed IPC rejection and explicit delegation, run by the trusted native
 * test launcher. Requests under test use only the newly issued client ends. */
#include "filed/identity.h"

static int identity_call(int endpoint, int page_fd, void *page, unsigned op,
    const void *payload, unsigned size, int lease_fd, uint64_t *result, int *received)
{
    static uint64_t sequence = 9000;
    memset(page, 0, FILED_PAGE_BYTES);
    pacha_service_envelope_t *header = page;
    *header = (pacha_service_envelope_t){ .magic = PACHA_SERVICE_REQUEST_MAGIC,
        .abi_version = PACHA_SERVICE_ABI_VERSION, .service_id = FILED_SERVICE_ID,
        .op = op, .request_id = ++sequence, .trace_id = sequence, .payload_size = size };
    if (size) memcpy((char *)page + PACHA_SERVICE_HEADER_BYTES, payload, size);
    struct pacha_ipc_fd caps[2] = {{ .fd = (uint64_t)page_fd,
        .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE }};
    if (lease_fd >= 16) caps[1] = (struct pacha_ipc_fd){ .fd = (uint64_t)lease_fd,
        .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_RECV |
            PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL };
    struct pacha_ipc_msg request = { .word0 = header->magic, .word3 = sequence,
        .fds = caps, .fd_count = lease_fd >= 16 ? 2 : 1 };
    int reply_fd = pacha_ipc_call(endpoint, &request);
    if (reply_fd < 16) return -1000;
    struct pacha_ipc_fd incoming[PACHA_IPC_MAX_TRANSFER_FDS] = {{0}};
    struct pacha_ipc_msg reply = { .fds = incoming, .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS };
    int status = pacha_ipc_recv_wait(reply_fd, &reply, UINT64_MAX);
    (void)pacha_fd_close(reply_fd);
    if (status) return -1001;
    status = reply.word0 == PACHA_SERVICE_REPLY_MAGIC && reply.word3 == sequence &&
        header->magic == PACHA_SERVICE_REPLY_MAGIC && header->request_id == sequence ?
        (int)header->status : -1002;
    if (result) *result = header->result;
    for (unsigned i = 0; i < reply.fd_count; i++) {
        if (!status && received && i == 0) *received = (int)incoming[i].fd;
        else (void)pacha_fd_close((int)incoming[i].fd);
    }
    return status;
}

static int filed_identity_native(void)
{
    int status = 0, owner = -1, root = -1, page_fd = -1, snapshot_fd = -1;
    struct pacha_ipc_channel_pair lease = { .a = -1, .b = -1 };
    void *page = NULL;
    unsigned char *backing = NULL, *copy = NULL;
    uint64_t owner_id = 0, root_id = 0, handle = 0, duplicate = 0, ignored = 0;
    page_fd = pacha_vmo_create(FILED_PAGE_BYTES, PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE, 0);
    if (page_fd < 16) return 1;
    page = pacha_mmap(page_fd, FILED_PAGE_BYTES, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!page) { (void)pacha_fd_close(page_fd); return 2; }
#define ID_CHECK(expression) do { if (!(expression)) { status = __LINE__; goto out; } } while (0)
    filed_identity_t identity = { .generation = UINT64_C(0x544553540001), .pid = 9001,
        .uid = 1001, .gid = 1001, .euid = 1001, .egid = 1001, .suid = 1001, .sgid = 1001,
        .rights = FILED_RIGHT_LOOKUP | FILED_RIGHT_READ | FILED_RIGHT_STAT };
    ID_CHECK(identity_call(240, page_fd, page, FILED_OP_CLIENT_REGISTER, &identity, sizeof(identity), -1, &owner_id, &owner) == 0);
    identity = (filed_identity_t){ .generation = UINT64_C(0x544553540002), .pid = 9002 };
    ID_CHECK(identity_call(240, page_fd, page, FILED_OP_CLIENT_REGISTER, &identity, sizeof(identity), -1, &root_id, &root) == 0);
    ID_CHECK(owner >= 16 && root >= 16 && owner_id && root_id && owner_id != root_id);
    /* Supplying UID zero does not turn an ordinary connection into an issuer. */
    ID_CHECK(identity_call(owner, page_fd, page, FILED_OP_CLIENT_REGISTER, &identity, sizeof(identity), -1, NULL, NULL) == -1);
    ID_CHECK(identity_call(root, page_fd, page, FILED_OP_CLIENT_REGISTER, &identity, sizeof(identity), -1, NULL, NULL) == -1);
    filed_path_request_t open = { .rights = FILED_RIGHT_READ | FILED_RIGHT_STAT, .path = "/etc/os-release" };
    ID_CHECK(identity_call(root, page_fd, page, FILED_OP_VFS_OPENAT, &open, sizeof(open), -1, NULL, NULL) == -13);
    ID_CHECK(identity_call(owner, page_fd, page, FILED_OP_VFS_OPENAT, &open, sizeof(open), -1, &handle, NULL) == 0 && handle);
    filed_io_t io = { .handle = handle, .length = 16 };
    ID_CHECK(identity_call(root, page_fd, page, FILED_OP_VFS_PREAD, &io, sizeof(io), -1, NULL, NULL) == -9);
    filed_handle_request_t close = { .handle = handle };
    ID_CHECK(identity_call(root, page_fd, page, FILED_OP_VFS_CLOSE, &close, sizeof(close), -1, NULL, NULL) == -9);
    ID_CHECK(identity_call(owner, page_fd, page, FILED_OP_VFS_PREAD, &io, sizeof(io), -1, &ignored, NULL) == 0 && ignored == 16);
    ID_CHECK(identity_call(owner, page_fd, page, FILED_OP_VFS_PWRITE, &io, sizeof(io), -1, NULL, NULL) == -13);
    filed_file_vmo_request_t snapshot = { .handle = handle, .length = 16 };
    ID_CHECK(identity_call(owner, page_fd, page, FILED_OP_VFS_FILE_VMO, &snapshot, sizeof(snapshot), -1, &ignored, &snapshot_fd) == 0 && snapshot_fd >= 16 && ignored == 16);
    ID_CHECK(pacha_mmap(snapshot_fd, 4096, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0) == NULL);
    ID_CHECK(pacha_mmap(snapshot_fd, 4096, PACHA_PROT_READ | PACHA_PROT_EXEC, PACHA_MMAP_PRIVATE, 0) == NULL);
    backing = pacha_mmap(snapshot_fd, 4096, PACHA_PROT_READ, PACHA_MMAP_SHARED, 0);
    copy = pacha_mmap(snapshot_fd, 4096, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_PRIVATE, 0);
    ID_CHECK(backing && copy && memcmp(backing, copy, 16) == 0);
    unsigned char original = backing[0];
    copy[0] = (unsigned char)(original ^ 0xffu);
    ID_CHECK(copy[0] != original && backing[0] == original);
    snapshot.flags = FILED_FILE_VMO_WRITE;
    ID_CHECK(identity_call(owner, page_fd, page, FILED_OP_VFS_SHARED_FILE_VMO, &snapshot, sizeof(snapshot), -1, NULL, NULL) == -13);
    snapshot.flags = FILED_FILE_VMO_EXEC;
    ID_CHECK(identity_call(owner, page_fd, page, FILED_OP_VFS_SHARED_FILE_VMO, &snapshot, sizeof(snapshot), -1, NULL, NULL) == -13);
    const uint64_t lease_rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CALL | PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL;
    ID_CHECK(pacha_ipc_channel_create(&lease, lease_rights, 0) == 0);
    filed_handle_flags_t dup = { .handle = handle };
    ID_CHECK(identity_call(owner, page_fd, page, FILED_OP_VFS_TRANSFER_DUP, &dup, sizeof(dup), lease.b, &duplicate, NULL) == 0 && duplicate);
    io.handle = duplicate;
    ID_CHECK(identity_call(root, page_fd, page, FILED_OP_VFS_PREAD, &io, sizeof(io), -1, NULL, NULL) == -9);
    struct pacha_ipc_msg adopt = { .word0 = FILED_LEASE_MAGIC, .word1 = duplicate, .word2 = root_id, .word3 = 1 };
    int reply_fd = pacha_ipc_call(lease.a, &adopt);
    ID_CHECK(reply_fd >= 16);
    struct pacha_ipc_msg response = {0};
    int replied = pacha_ipc_recv_wait(reply_fd, &response, UINT64_MAX);
    (void)pacha_fd_close(reply_fd);
    ID_CHECK(replied == 0 && response.word0 == FILED_LEASE_MAGIC && response.word1 == 0);
    ID_CHECK(identity_call(root, page_fd, page, FILED_OP_VFS_PREAD, &io, sizeof(io), -1, &ignored, NULL) == 0 && ignored == 16);
    ID_CHECK(identity_call(root, page_fd, page, FILED_OP_VFS_PWRITE, &io, sizeof(io), -1, NULL, NULL) == -13);
    ID_CHECK(identity_call(root, page_fd, page, FILED_OP_VFS_OPENAT, &open, sizeof(open), -1, NULL, NULL) == -13);
    ID_CHECK(identity_call(owner, page_fd, page, FILED_OP_VFS_CLOSE, &close, sizeof(close), -1, NULL, NULL) == 0);
    puts("FILED_IDENTITY=OK forged-uid root-no-grant foreign-handle read-only explicit-lease private-cow cache-write-denied exec-denied");
out:
    if (status) fprintf(stderr, "FILED_IDENTITY=FAIL line=%d\n", status);
    if (owner >= 16) (void)pacha_fd_close(owner);
    if (root >= 16) (void)pacha_fd_close(root);
    if (lease.a >= 16) (void)pacha_fd_close(lease.a);
    if (lease.b >= 16) (void)pacha_fd_close(lease.b);
    if (backing) (void)pacha_munmap(backing, 4096);
    if (copy) (void)pacha_munmap(copy, 4096);
    if (snapshot_fd >= 16) (void)pacha_fd_close(snapshot_fd);
    (void)pacha_munmap(page, FILED_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
#undef ID_CHECK
    return status;
}
