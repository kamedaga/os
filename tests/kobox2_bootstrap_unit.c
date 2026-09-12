/* SPDX-License-Identifier: MIT */
/* Bootstrap transaction oracle; native IPC itself has separate unit/native Gates. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/kobox2_adapter/bootstrap.c"

static struct ph_ipc_packet queue[PH_BOOTSTRAP_MAX_ITEMS + 1];
static size_t sent, consumed;
static int send_error, receive_error, receive_fault;
static uint64_t wrong_kind, short_size, close_fail;
static unsigned int closed[PACHA_FD_TABLE_LIMIT];

long pacha_syscall1(uint64_t nr, uint64_t fd) {
    assert(nr == PACHA_FD_SYSCALL_CLOSE && fd >= 16 && fd < PACHA_FD_TABLE_LIMIT);
    ++closed[fd];
    return fd == close_fail ? PACHA_SYSCALL_ERR_MAP : 0;
}

long pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t address) {
    assert(nr == PACHA_FD_SYSCALL_GET_INFO && fd >= 16 && fd < PACHA_FD_TABLE_LIMIT);
    *(struct pacha_fd_info *)(uintptr_t)address = (struct pacha_fd_info){
        .kind = fd == wrong_kind ? PACHA_FD_KIND_EVENT : PACHA_FD_KIND_VMO,
        .size = fd == short_size ? 1 : 4096};
    return 0;
}

int ph_ipc_send(struct ph_ipc *ipc, struct ph_ipc_packet *packet) {
    assert(ipc->admitted && packet->generation == ipc->generation);
    if (send_error) return send_error;
    assert(sent < sizeof(queue) / sizeof(queue[0]));
    queue[sent++] = *packet;
    return 0;
}

int ph_ipc_receive(struct ph_ipc *ipc, uint64_t generation, struct ph_ipc_packet *packet) {
    assert(ipc->admitted && generation == ipc->generation && !packet->fd_count);
    if (receive_fault) ipc->admitted = 0;
    if (receive_error) return receive_error;
    if (consumed == sent) return -EAGAIN;
    *packet = queue[consumed++];
    /* Native receive installs unique new FD numbers and clears MOVE flags. */
    for (size_t i = 0; i < packet->fd_count; ++i) packet->fds[i].fd += 500;
    return 0;
}

int ph_ipc_revoke(struct ph_ipc *ipc, uint64_t generation) {
    if (ipc->generation != generation) return -ESTALE;
    ipc->admitted = 0;
    return 0;
}

int ph_ipc_packet_release(struct ph_ipc_packet *packet) {
    int result = 0;
    for (size_t i = 0; i < packet->fd_count; ++i) {
        if (packet->fds[i].fd == PH_IPC_NO_FD) continue;
        if (pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, packet->fds[i].fd)) result = -EIO;
        else packet->fds[i].fd = PH_IPC_NO_FD;
    }
    if (!result) *packet = (struct ph_ipc_packet){0};
    return result;
}

static struct ph_bootstrap_item items[PH_BOOTSTRAP_MAX_ITEMS];

static void reset(size_t artifacts, size_t resources) {
    sent = consumed = 0;
    send_error = receive_error = receive_fault = 0;
    wrong_kind = short_size = close_fail = 0;
    memset(closed, 0, sizeof(closed));
    memset(queue, 0, sizeof(queue));
    memset(items, 0, sizeof(items));
    for (size_t i = 0; i < 2 + artifacts + resources; ++i) {
        items[i] = (struct ph_bootstrap_item){.size = i < 2 + artifacts ? 33 + i : 0,
            .capability = {.fd = 100 + i, .rights = PH_BOOTSTRAP_BLOB_RIGHTS,
                .flags = PACHA_FD_FLAG_CLOEXEC}};
    }
}

static void send_all(struct ph_ipc *ipc, size_t artifacts, size_t resources) {
    struct ph_bootstrap_sender sender = {0};
    assert(!ph_bootstrap_sender_init(&sender, 7, items, artifacts, resources));
    assert(ph_bootstrap_sender_init(&sender, 7, items, artifacts, resources) == -EINVAL);
    send_error = -EAGAIN;
    assert(ph_bootstrap_send_next(&sender, ipc) == -EAGAIN && !sender.next);
    send_error = -ENOMEM;
    assert(ph_bootstrap_send_next(&sender, ipc) == -ENOMEM && !sender.next);
    send_error = 0;
    while (!sender.finished) assert(!ph_bootstrap_send_next(&sender, ipc));
    assert(sent == 3 + artifacts + resources);
    assert(ph_bootstrap_send_next(&sender, ipc) == -EALREADY);
    for (size_t i = 0; i < 2 + artifacts + resources; ++i) assert(items[i].capability.fd == 100 + i);
}

static void valid_bundle(void) {
    reset(64, 64);
    struct ph_ipc ipc = {.generation = 7, .fd = 16, .admitted = 1};
    send_all(&ipc, 64, 64);
    struct ph_bootstrap_receiver receiver = {0};
    assert(!ph_bootstrap_receiver_init(&receiver, 7, 64, 64));
    assert(ph_bootstrap_release(&receiver) == -EBUSY);
    receive_error = -ENOMEM;
    assert(ph_bootstrap_receive_next(&receiver, &ipc) == -ENOMEM && !receiver.received);
    receive_error = 0;
    for (size_t i = 0; i < PH_BOOTSTRAP_MAX_ITEMS; ++i) {
        assert(!ph_bootstrap_receive_next(&receiver, &ipc));
        assert(receiver.received == i + 1 && !receiver.complete);
        assert(receiver.items[i].capability.fd == 600 + i);
        assert(receiver.items[i].size == items[i].size);
    }
    assert(!ph_bootstrap_receive_next(&receiver, &ipc) && receiver.complete);
    assert(ph_bootstrap_receive_next(&receiver, &ipc) == -EALREADY);
    close_fail = 601;
    assert(ph_bootstrap_release(&receiver) == -EIO && !receiver.complete && receiver.failed);
    assert(closed[600] == 1 && closed[601] == 1 && closed[729] == 1);
    close_fail = 0;
    assert(!ph_bootstrap_release(&receiver) && !receiver.generation);
    assert(closed[600] == 1 && closed[601] == 2 && closed[729] == 1);
    assert(!ph_bootstrap_release(&receiver));
}

static void malformed_bundle(void) {
    for (unsigned int scenario = 0; scenario < 10; ++scenario) {
        reset(1, 1);
        struct ph_ipc ipc = {.generation = 7, .fd = 16, .admitted = 1};
        send_all(&ipc, 1, 1);
        struct ph_bootstrap_receiver receiver = {0};
        assert(!ph_bootstrap_receiver_init(&receiver, 7, 1, 1));
        assert(!ph_bootstrap_receive_next(&receiver, &ipc));
        switch (scenario) {
        case 0: queue[1].correlation = 0; break;
        case 1: queue[1].operation = PH_BOOTSTRAP_FINISH; break;
        case 2: queue[1].value = 0; break;
        case 3: queue[1].value = (UINT64_C(1) << 32); break;
        case 4: queue[1].fds[0].rights |= PACHA_FD_RIGHT_MAP_WRITE; break;
        case 5: queue[1].fds[0].flags = PACHA_FD_FLAG_INHERIT; break;
        case 6: queue[1].fd_count = 2; queue[1].fds[1] = queue[2].fds[0]; break;
        case 7: wrong_kind = 601; break;
        case 8: short_size = 601; break;
        case 9: receive_error = -ENOMEM; receive_fault = 1; break;
        }
        assert(ph_bootstrap_receive_next(&receiver, &ipc) < 0);
        assert(!ipc.admitted && !receiver.generation && closed[600] == 1);
        if (scenario != 9) assert(closed[601] == 1);
        if (scenario == 6) assert(closed[602] == 1);
    }
}

static void abort_partial(void) {
    reset(1, 1);
    struct ph_ipc ipc = {.generation = 7, .fd = 16, .admitted = 1};
    send_all(&ipc, 1, 1);
    struct ph_bootstrap_receiver receiver = {0};
    assert(!ph_bootstrap_receiver_init(&receiver, 7, 1, 1));
    assert(!ph_bootstrap_receive_next(&receiver, &ipc));
    queue[1].correlation = 100;
    close_fail = 601;
    assert(ph_bootstrap_receive_next(&receiver, &ipc) == -EIO);
    assert(receiver.failed && receiver.pending.fds[0].fd == 601 && closed[600] == 1);
    assert(ph_bootstrap_receive_next(&receiver, &ipc) == -ESHUTDOWN && consumed == 2);
    ipc.generation = 8;
    assert(ph_bootstrap_abort(&receiver, &ipc) == -ESTALE && closed[601] == 1);
    ipc.generation = 7;
    close_fail = 0;
    assert(!ph_bootstrap_abort(&receiver, &ipc) && !receiver.generation);
    assert(closed[600] == 1 && closed[601] == 2);
}

static void invalid_plan(void) {
    reset(1, 0);
    struct ph_bootstrap_sender sender = {0};
    struct ph_bootstrap_receiver receiver = {0};
    assert(ph_bootstrap_receiver_init(&receiver, 7, 0, 0) == -EINVAL);
    assert(ph_bootstrap_receiver_init(&receiver, 7, 65, 0) == -EINVAL);
    assert(ph_bootstrap_receiver_init(&receiver, 7, 1, 65) == -EINVAL);
    items[0].capability.transfer_flags = PACHA_IPC_TRANSFER_MOVE;
    assert(ph_bootstrap_sender_init(&sender, 7, items, 1, 0) == -EINVAL);
    items[0].capability.transfer_flags = 0;
    items[0].capability.rights |= PACHA_FD_RIGHT_MAP_WRITE;
    assert(ph_bootstrap_sender_init(&sender, 7, items, 1, 0) == -EACCES);
    assert(!sender.generation);
}

static void invalid_finish(void) {
    for (unsigned int scenario = 0; scenario < 4; ++scenario) {
        reset(1, 1);
        struct ph_ipc ipc = {.generation = 7, .fd = 16, .admitted = 1};
        send_all(&ipc, 1, 1);
        struct ph_bootstrap_receiver receiver = {0};
        assert(!ph_bootstrap_receiver_init(&receiver, 7, 1, 1));
        for (unsigned int i = 0; i < 3; ++i) assert(!ph_bootstrap_receive_next(&receiver, &ipc));
        if (scenario == 0) {
            queue[3].value = 1; /* Resource handles must not claim blob bytes. */
        } else {
            assert(!ph_bootstrap_receive_next(&receiver, &ipc));
            if (scenario == 1) queue[4].value = 1;
            if (scenario == 2) queue[4].operation = PH_BOOTSTRAP_BLOB;
            if (scenario == 3) {
                queue[4].fd_count = 1;
                queue[4].fds[0] = (struct pacha_ipc_fd){.fd = 105};
            }
        }
        assert(ph_bootstrap_receive_next(&receiver, &ipc) == -EPROTO);
        assert(!ipc.admitted && !receiver.generation);
        for (unsigned int i = 600; i < 604; ++i) assert(closed[i] == 1);
        if (scenario == 3) assert(closed[605] == 1);
    }
}

int main(void) {
    invalid_plan();
    valid_bundle();
    malformed_bundle();
    abort_partial();
    invalid_finish();
    puts("kobox2 bootstrap transaction unit: PASS");
    return 0;
}
