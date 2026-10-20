/* SPDX-License-Identifier: MIT */
/* Exercise the production supervisor through the new filed VMO codec. Native
 * IPC is mocked; filed's actual outbound grant is separately tested by filed. */
#define main lprs_program_main
#include "../userland/lpr_supervisor/src/main.c"
#undef main
#include <assert.h>
#include "filed/flags.h"

static unsigned char *wire;
static size_t wire_size;
static struct pacha_fd_info descriptors[128];
static kb2_fs_request_t request;
static uint64_t issued_rights, issued_kind, issued_handle;
static unsigned issued_count, calls, closes;
static int inspect_error, response_error;

static void create_fd(int fd, uint64_t kind, uint64_t rights)
{
    assert(fd >= 16 && fd < 128 && !descriptors[fd].kind);
    descriptors[fd] = (struct pacha_fd_info){.kind = kind, .rights = rights};
}

int pacha_fd_table(uint64_t minimum, struct pacha_fd_table_info *info)
{
    assert(!minimum);
    *info = (struct pacha_fd_table_info){.capacity = 128, .maximum = 128,
        .free_slots = 112};
    return 0;
}

pacha_fd_t pacha_vmo_create(uint64_t bytes, uint64_t rights, uint32_t flags)
{
    assert(bytes >= KB2_FILESYSTEM_REQUEST_SIZE + KB2_FILESYSTEM_CREDENTIALS_SIZE + 8);
    assert(!(bytes & 4095) && !flags && !wire);
    assert(rights == (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE));
    wire = calloc(1, bytes);
    assert(wire);
    wire_size = bytes;
    create_fd(80, PACHA_FD_KIND_VMO, rights);
    return 80;
}

void *pacha_mmap(pacha_fd_t fd, uint64_t bytes, uint64_t prot, uint64_t flags, uint64_t offset)
{
    assert(fd == 80 && bytes == wire_size && !offset);
    assert(prot == (PACHA_PROT_READ | PACHA_PROT_WRITE) && flags == PACHA_MMAP_SHARED);
    return wire;
}

int pacha_munmap(void *mapping, uint64_t bytes)
{
    assert(mapping == wire && bytes == wire_size);
    free(wire);
    wire = NULL;
    wire_size = 0;
    return 0;
}

int pacha_fd_close(pacha_fd_t fd)
{
    assert(fd >= 16 && fd < 128 && descriptors[fd].kind);
    descriptors[fd] = (struct pacha_fd_info){0};
    ++closes;
    return 0;
}

int pacha_fd_get_info(pacha_fd_t fd, struct pacha_fd_info *info)
{
    assert(fd == 72 && descriptors[fd].kind);
    if (inspect_error) return -EIO;
    *info = descriptors[fd];
    return 0;
}

int pacha_ipc_call(pacha_fd_t endpoint, const struct pacha_ipc_msg *message, struct pacha_ipc_msg *reply, uint64_t timeout, int *sent)
{
    assert(endpoint == 33 && message->fd_count == 1 && message->fds[0].fd == 80);
    assert(message->fds[0].transfer_flags == PACHA_IPC_TRANSFER_CLOEXEC);
    assert(message->fds[0].rights == (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE));
    assert(!kb2_fs_request_decode(wire, message->word1, message->word1, &request));
    assert(request.opcode == KB2_FILESYSTEM_OP_CLIENT_REGISTER && request.generation == 1);
    assert(request.opcode == message->word0 && request.sequence == message->word3);
    assert(!request.client && request.credential_generation == 1 && request.offset == 77);
    assert(request.second_handle == 555);
    assert(request.capabilities == (FILED_RIGHT_LOOKUP | FILED_RIGHT_READ));
    assert(request.data.length == KB2_FILESYSTEM_CREDENTIALS_SIZE && request.groups.length == 8);
    kb2_fs_credentials_t credentials;
    assert(!kb2_fs_credentials_decode(wire + request.data.offset,
        request.data.length, &credentials));
    assert(credentials.uid == 123 && credentials.euid == 123 && credentials.fsuid == 123);
    assert(credentials.gid == 456 && credentials.fsgid == 456 && credentials.umask == 0022);
    assert(!credentials.cap_effective && !credentials.cap_permitted);
    const unsigned char groups[] = {42, 0, 0, 0, 43, 0, 0, 0};
    assert(!memcmp(wire + request.groups.offset, groups, sizeof(groups)));
    ++calls;
    if (sent) *sent = 1;
    assert(timeout == PACHA_FD_WAIT_FOREVER);
    kb2_fs_response_t response = {.opcode = request.opcode, .generation = request.generation,
        .sequence = request.sequence, .result = response_error,
        .handle = response_error ? 0 : issued_handle};
    assert(!kb2_fs_response_encode(wire, wire_size, &response));
    reply->word0 = request.opcode;
    reply->word1 = (uint64_t)response.result;
    reply->word2 = KB2_FILESYSTEM_RESPONSE_SIZE;
    reply->word3 = request.sequence;
    assert(issued_count <= 2 && reply->fd_capacity >= issued_count);
    reply->fd_count = issued_count;
    for (unsigned i = 0; i < issued_count; ++i) {
        create_fd(72 + (int)i, issued_kind, issued_rights);
        reply->fds[i] = (struct pacha_ipc_fd){.fd = 72 + i, .rights = issued_rights,
            .transfer_flags = PACHA_IPC_TRANSFER_MOVE | PACHA_IPC_TRANSFER_CLOEXEC};
    }
    return 0;
}

static void check_session(uint64_t rights, unsigned count, uint64_t kind,
    uint64_t handle, int inspect_failure, int error, int expected)
{
    assert(!wire);
    for (unsigned i = 16; i < 128; ++i) assert(!descriptors[i].kind);
    issued_rights = rights;
    issued_count = count;
    issued_kind = kind;
    issued_handle = handle;
    inspect_error = inspect_failure;
    response_error = error;
    calls = closes = 0;
    lprs_process_t process = {.active = 1, .pid = 77, .generation = 999, .process_fd = 24,
        .control_fd = 25, .filed_fd = -1, .unix_fd = -1,
        .filed_rights = FILED_RIGHT_LOOKUP | FILED_RIGHT_READ,
        .credentials = {.pid = 77, .uid = 123, .euid = 123, .suid = 123,
            .gid = 456, .egid = 456, .sgid = 456, .generation = 555},
        .groups = {42, 43}, .group_count = 2};
    lprs_reply_cap_t reply = {.fd = -1};
    uint64_t session = 0;
    g_filed_admin_fd = 33;
    assert(lprs_filed_session(&process, &reply, &session) == expected);
    assert(calls == 1 && !wire);
    if (!expected) {
        assert(process.filed_fd == 72 && process.filed_session == handle && session == handle);
        assert(reply.fd == 72 && reply.rights == FILED_CLIENT_CHANNEL_RIGHTS);
        assert(reply.transfer_flags == (PACHA_IPC_TRANSFER_PRIVATE | PACHA_IPC_TRANSFER_CLOEXEC));
        assert(!(reply.rights & (PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_SET_FLAGS | PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_SEND)));
        assert(closes == 1 && descriptors[72].rights == FILED_CLIENT_REGISTER_RIGHTS);
        assert(!lprs_filed_session(&process, &reply, &session) && calls == 1);
        lprs_unix_release(&process);
        assert(process.filed_fd == -1 && !process.filed_session);
    } else {
        assert(process.filed_fd == -1 && !process.filed_session && !session && reply.fd == -1);
    }
    assert(closes == count + 1);
    for (unsigned i = 16; i < 128; ++i) assert(!descriptors[i].kind);
}

int main(void)
{
    const uint64_t required = FILED_CLIENT_REGISTER_RIGHTS;
    assert(required == (FILED_CLIENT_CHANNEL_RIGHTS | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER));
    assert(!(required & PACHA_FD_RIGHT_SET_FLAGS));
    check_session(required, 1, PACHA_FD_KIND_CHANNEL, 999, 0, 0, 0);
    /* Every missing required bit and every surplus bit must fail closed; the
     * previous sender's unnecessary SET_FLAGS grant is one of these cases. */
    for (unsigned bit = 0; bit < 64; ++bit) {
        uint64_t mask = UINT64_C(1) << bit;
        check_session(required ^ mask, 1, PACHA_FD_KIND_CHANNEL, 999, 0, 0, -EPROTO);
    }
    check_session(required, 0, PACHA_FD_KIND_CHANNEL, 999, 0, 0, -EPROTO);
    check_session(required, 2, PACHA_FD_KIND_CHANNEL, 999, 0, 0, -EPROTO);
    check_session(required, 1, PACHA_FD_KIND_VMO, 999, 0, 0, -EPROTO);
    check_session(required, 1, PACHA_FD_KIND_CHANNEL, 0, 0, 0, -EPROTO);
    check_session(required, 1, PACHA_FD_KIND_CHANNEL, 999, 1, 0, -EPROTO);
    check_session(required, 1, PACHA_FD_KIND_CHANNEL, 999, 0, -EACCES, -EACCES);
    puts("lpr supervisor filed session: actual new codec, strict rights, attenuation and owned cleanup PASS");
    return 0;
}
