/* SPDX-License-Identifier: MIT */
#include "../src/storage_owner.c"
#include <assert.h>

static struct ph_ipc_packet replies[3];
static unsigned sends, receives, reply_count, close_count;
static uint64_t sent_operation[8], sent_id[8], sent_client[8];
static long send_error;
static int close_error;

long pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t address)
{
    assert(fd == 60);
    struct pacha_ipc_msg *message = (void *)(uintptr_t)address;
    if (nr == PACHA_IPC_SYSCALL_SEND) {
        if (send_error) return send_error;
        assert(sends < 8 && message->word1 == 9);
        sent_operation[sends] = message->word0;
        sent_id[sends] = message->word2;
        sent_client[sends++] = message->word3;
        if (message->word0 == PH_FS_CLIENT_BIND) {
            assert(message->word3 == 8192 && message->fd_count == 2);
            assert(message->fds[0].fd == 80 && message->fds[1].fd == 81);
            assert(message->fds[0].rights == (PACHA_FD_RIGHT_INSPECT |
                PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE));
            assert(message->fds[1].rights == PH_FS_CLIENT_DOORBELL_RIGHTS);
            for (unsigned i = 0; i < 2; ++i)
                assert(message->fds[i].flags == PACHA_FD_FLAG_CLOEXEC &&
                    !message->fds[i].transfer_flags);
        } else assert(!message->fd_count);
        return 0;
    }
    assert(nr == PACHA_IPC_SYSCALL_RECV && receives < reply_count);
    struct ph_ipc_packet *packet = &replies[receives++];
    message->word0 = packet->operation; message->word1 = packet->generation;
    message->word2 = packet->correlation; message->word3 = packet->value;
    message->fd_count = packet->fd_count;
    memcpy(message->fds, packet->fds, packet->fd_count * sizeof(*packet->fds));
    return 0;
}
long pacha_syscall1(uint64_t nr, uint64_t fd)
{
    assert(nr == PACHA_FD_SYSCALL_CLOSE && fd == 70);
    ++close_count;
    return close_error ? PACHA_SYSCALL_ERR_MAP : 0;
}
long pacha_syscall4(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; abort(); }
long pacha_fd_wait_many(struct pacha_pollfd *fds, uint64_t count, uint64_t timeout)
{ (void)fds; (void)count; (void)timeout; abort(); }

static void reset(struct filed_storage_owner *owner)
{
    /* This fixture's mock peer is terminal between cases. Production frees
     * ambiguous charges only after confirmed sandbox termination. */
    while (owner->clients) {
        struct owner_client_queue *next = owner->clients->next;
        free(owner->clients); owner->clients = next;
    }
    *owner = (struct filed_storage_owner){.started = 1, .identity.generation = 9,
        .next_exchange = 2, .config = {.queue_size = 8, .queue_arena_bytes = 65536,
            .sandbox_ram_bytes = 64 * 1024 * 1024},
        .ipc = {.fd = 60, .generation = 9, .admitted = 1}};
    sends = receives = close_count = 0;
    send_error = close_error = 0;
    replies[0] = (struct ph_ipc_packet){.operation = PH_FS_CLIENT_BOUND,
        .generation = 9, .correlation = 3};
    reply_count = 1;
}

int main(void)
{
    struct filed_storage_owner owner = {0};
    struct ph_fs_channel_config first, second;
    reset(&owner);
    assert(!filed_storage_owner_client_config(&owner, 7, 0, &first));
    assert(!filed_storage_owner_client_config(&owner, 8, 0, &second));
    assert(first.channel_id == 3 && second.channel_id == 4 && first.generation == 9);
    assert(first.arena_bytes == 8192 && second.arena_bytes == 8192);
    assert(sent_operation[0] == PH_FS_CLIENT_PREPARE && sent_client[0] == 7);
    assert(sent_operation[1] == PH_FS_CLIENT_PREPARE && sent_client[1] == 8);
    /* Configuration publishes reservations in ID order; attachment can be
     * reversed without relying on the clients' scheduling order. */
    replies[0].correlation = 4;
    assert(!filed_storage_owner_client_bind(&owner, &second, 80, 81));
    receives = 0; replies[0].correlation = 3;
    assert(!filed_storage_owner_client_bind(&owner, &first, 80, 81));
    assert(sent_operation[2] == PH_FS_CLIENT_BIND && sent_id[2] == 4);
    assert(sent_operation[3] == PH_FS_CLIENT_BIND && sent_id[3] == 3);
    receives = 0; replies[1] = replies[0];
    replies[0] = (struct ph_ipc_packet){.operation = PH_FS_OWNER_NOTIFY,
        .generation = 9, .value = 2};
    replies[1].operation = PH_FS_CLIENT_RELEASED; reply_count = 2;
    assert(!filed_storage_owner_client_release(&owner, first.channel_id) && receives == 2);
    assert(owner.client_count == 1 && owner.client_bytes == owner.clients->charge);
    assert(filed_storage_owner_client_release(&owner, first.channel_id) == -ENOENT);
    assert(!filed_storage_owner_client_config(&owner, 7, 0, &first) && first.channel_id == 5);
    reset(&owner);
    assert(!filed_storage_owner_client_config(&owner, 7, 0, &first));
    replies[0].correlation = 4;
    assert(filed_storage_owner_client_bind(&owner, &first, 80, 81) == -EPROTO && owner.failed);
    assert(filed_storage_owner_client_release(&owner, first.channel_id) == -EPIPE);
    reset(&owner);
    assert(!filed_storage_owner_client_config(&owner, 7, 0, &first));
    replies[0].fd_count = 1; replies[0].fds[0].fd = 70; close_error = 1;
    assert(filed_storage_owner_client_bind(&owner, &first, 80, 81) == -EPROTO);
    assert(owner.failed && !owner.ipc.admitted && owner.ipc.rejected.fds[0].fd == 70);
    assert(owner.client_count == 1 && owner.client_bytes); /* Bad ACK cannot refund admission. */
    close_error = 0;
    assert(!ph_ipc_packet_release(&owner.ipc.rejected) && close_count == 2);
    reset(&owner);
    send_error = PACHA_SYSCALL_ERR_MAP;
    assert(filed_storage_owner_client_config(&owner, 7, 0, &first) == -EIO && owner.failed);
    reset(&owner);
    owner.next_exchange = UINT64_MAX;
    assert(filed_storage_owner_client_config(&owner, 7, 0, &first) == -EOVERFLOW && !sends);
    reset(&owner);
    assert(filed_storage_owner_client_config(&owner, 7, owner.config.sandbox_ram_bytes, &first) == -ENOMEM);
    assert(!sends && owner.next_exchange == 2 && !owner.failed && !owner.clients);
    assert(!filed_storage_owner_client_config(&owner, 7, 0, &first));
    uint64_t charge = owner.client_bytes;
    uint64_t admitted_arena = first.arena_bytes;
    owner.config.sandbox_ram_bytes = charge * 4;
    assert(filed_storage_owner_client_config(&owner, 8, admitted_arena, &second) == -EAGAIN && sends == 1);
    assert(owner.next_exchange == 3 && !owner.failed && owner.client_count == 1);
    replies[0].operation = PH_FS_CLIENT_RELEASED;
    assert(!filed_storage_owner_client_release(&owner, first.channel_id));
    assert(!owner.client_bytes && !owner.client_count);
    assert(!filed_storage_owner_client_config(&owner, 8, admitted_arena, &second));
    assert(second.channel_id == 4 && owner.client_bytes == charge);
    reset(&owner);
    assert(!filed_storage_owner_client_config(&owner, 7, 1024 * 1024, &first));
    assert(first.arena_bytes == 1024 * 1024);
    reset(&owner);
    puts("filed client queue control: fresh IDs, reservation before attach, exact capabilities, matching ACK, ambiguous failure inventory PASS");
    return 0;
}
