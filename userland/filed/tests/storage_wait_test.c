/* SPDX-License-Identifier: MIT */
#include "../src/storage_owner.c"
#include <assert.h>

static struct ph_ipc_packet packets[3];
static unsigned packet_count, received, closed;
static long receive_status;
static int close_error;

static long protocol_oracle2(uint64_t, uint64_t, uint64_t);
static long protocol_oracle4(uint64_t nr, uint64_t fd, uint64_t address,
    uint64_t timeout, uint64_t flags)
{
    assert(nr == PACHA_IPC_SYSCALL_RECV_WAIT && fd == 60);
    assert(timeout == PACHA_FD_WAIT_FOREVER && !flags);
    ++received;
    if (receive_status) return receive_status;
    assert(received <= packet_count);
    struct pacha_ipc_msg *message = (void *)(uintptr_t)address;
    const struct ph_ipc_packet *packet = &packets[received - 1];
    assert(message->fd_capacity == PACHA_IPC_MAX_TRANSFER_FDS);
    message->word0 = packet->operation; message->word1 = packet->generation;
    message->word2 = packet->correlation; message->word3 = packet->value;
    message->fd_count = packet->fd_count;
    memcpy(message->fds, packet->fds, packet->fd_count * sizeof(packet->fds[0]));
    return 0;
}

static long protocol_oracle2(uint64_t nr, uint64_t a, uint64_t b)
{
    /* A data wait must not fall back to nonblocking RECV or process polling. */
    (void)nr; (void)a; (void)b; abort();
}

long pacha_syscall1(uint64_t nr, uint64_t fd)
{
    assert(nr == PACHA_FD_SYSCALL_CLOSE && fd == 70);
    ++closed;
    return close_error ? PACHA_SYSCALL_ERR_MAP : 0;
}

static void reset(struct filed_storage_owner *owner, struct owner_queue *queue)
{
    *owner = (struct filed_storage_owner){.identity.generation = 9,
        .management.b = -1, .ipc = {.fd = 60, .generation = 9, .admitted = 1}};
    *queue = (struct owner_queue){.owner = owner, .notify_operation = PH_FS_QUEUE_NOTIFY};
    queue->channel.config.channel_id = 4;
    packets[0] = (struct ph_ipc_packet){.operation = PH_FS_QUEUE_NOTIFY,
        .generation = 9, .value = 2};
    packet_count = 1; received = closed = 0; receive_status = close_error = 0;
}

int main(void)
{
    struct filed_storage_owner owner;
    struct owner_queue queue;
    reset(&owner, &queue);
    assert(!queue_wait(&queue, 2) && received == 1 && !closed);
    reset(&owner, &queue);
    packets[1] = packets[0]; packets[0].operation = PH_FS_OWNER_NOTIFY; packet_count = 2;
    assert(!queue_wait(&queue, 2) && received == 2);
    reset(&owner, &queue);
    queue.notify_operation = PH_FS_EXCHANGE_NOTIFY;
    packets[0].operation = PH_FS_EXCHANGE_NOTIFY; packets[0].correlation = 4;
    assert(!queue_wait(&queue, 2));
    reset(&owner, &queue);
    packets[0].operation = PH_FS_EXCHANGE_NOTIFY; packets[0].correlation = 5;
    assert(queue_wait(&queue, 2) == -EPROTO);
    reset(&owner, &queue);
    packets[0].value = 3;
    assert(queue_wait(&queue, 2) == -EPROTO);
    reset(&owner, &queue);
    receive_status = PACHA_SYSCALL_ERR_CLOSED;
    assert(queue_wait(&queue, 2) == -EPIPE && received == 1);
    reset(&owner, &queue);
    receive_status = PACHA_SYSCALL_ERR_NOT_READY;
    assert(queue_wait(&queue, 2) == -EAGAIN);
    reset(&owner, &queue);
    receive_status = PACHA_SYSCALL_ERR_ALLOC;
    assert(queue_wait(&queue, 2) == -ENOMEM);
    reset(&owner, &queue);
    packets[0].generation = 8;
    assert(queue_wait(&queue, 2) == -ESTALE && !owner.ipc.admitted);
    reset(&owner, &queue);
    packets[0].fd_count = 1; packets[0].fds[0].fd = 70;
    assert(queue_wait(&queue, 2) == -EPROTO && closed == 1);
    reset(&owner, &queue);
    packets[0].fd_count = 1; packets[0].fds[0].fd = 70; close_error = 1;
    assert(queue_wait(&queue, 2) == -EPROTO && !owner.ipc.admitted &&
        owner.ipc.rejected.fd_count == 1 && owner.ipc.rejected.fds[0].fd == 70);
    close_error = 0;
    assert(!ph_ipc_packet_release(&owner.ipc.rejected) && closed == 2);
    reset(&owner, &queue);
    packets[0].generation = 8; packets[0].fd_count = 1; packets[0].fds[0].fd = 70;
    close_error = 1;
    assert(queue_wait(&queue, 2) == -EIO && owner.ipc.rejected.fds[0].fd == 70);
    close_error = 0;
    assert(!ph_ipc_packet_release(&owner.ipc.rejected) && closed == 2);
    reset(&owner, &queue);
    owner.management.b = 61;
    assert(queue_wait(&queue, 2) == -EPROTO && !received);
    puts("filed storage wait: single RECV_WAIT, stale lanes, peer death, generation, ancillary cleanup PASS");
}

long pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b) {
    assert(nr != PACHA_IPC_SYSCALL_SEND && nr != PACHA_IPC_SYSCALL_RECV);
    return protocol_oracle2(nr, a, b);
}
long pacha_syscall4(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    assert(nr != PACHA_IPC_SYSCALL_RECV_WAIT);
    return protocol_oracle4(nr, a, b, c, d);
}
long pacha_ipc_registers(uint64_t nr, uint64_t fd, uint64_t flags, uint64_t timeout,
    uint64_t words[4], struct pacha_ipc_extension *extension, int *committed) {
    const int sending = nr == PACHA_IPC_SYSCALL_SEND;
    struct pacha_ipc_msg message = {
        .word0=words[0], .word1=words[1], .word2=words[2], .word3=words[3],
        .fds=extension ? (void *)(uintptr_t)(sending ? extension->send_array : extension->receive_array) : 0,
        .fd_count=extension && sending ? extension->send_count : 0,
        .fd_capacity=extension ? extension->receive_capacity : 0,
    };
    long status = nr == PACHA_IPC_SYSCALL_RECV_WAIT ?
        protocol_oracle4(nr, fd, (uintptr_t)&message, timeout, flags) :
        protocol_oracle2(nr, fd, (uintptr_t)&message);
    words[0]=words[1]=words[2]=words[3]=0;
    if (!sending && !status) {
        words[0]=message.word0; words[1]=message.word1;
        words[2]=message.word2; words[3]=message.word3;
    }
    if (extension) extension->received_count = message.fd_count;
    if (committed) *committed=0;
    return status;
}
