/* SPDX-License-Identifier: MIT */
/* Native syscall oracle: ownership/failure coverage, not process isolation. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/kobox2_adapter/ipc.c"

static struct pacha_fd_info info;
static struct ph_ipc_packet incoming;
static long info_status, send_status, recv_status;
static unsigned int inspections, sends, receives, closes[PACHA_FD_TABLE_LIMIT];
static unsigned int close_fail_fd;
static int copyout_failure;

long pacha_syscall1(uint64_t nr, uint64_t fd) {
    assert(nr == PACHA_FD_SYSCALL_CLOSE && valid_fd(fd));
    ++closes[fd];
    return fd == close_fail_fd ? PACHA_SYSCALL_ERR_MAP : 0;
}

long pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t address) {
    assert(valid_fd(fd));
    if (nr == PACHA_FD_SYSCALL_GET_INFO) {
        ++inspections;
        if (!info_status) *(struct pacha_fd_info *)(uintptr_t)address = info;
        return info_status;
    }
    struct pacha_ipc_msg *message = (void *)(uintptr_t)address;
    assert(!message->flags);
    if (nr == PACHA_IPC_SYSCALL_SEND) {
        ++sends;
        assert(message->word0 == 1 && message->word1 == 7 && message->word2 == 19);
        assert(message->word3 == 123 && message->fd_count == 2);
        assert(message->fds[0].fd == 100 && message->fds[1].fd == 101);
        return send_status;
    }
    assert(nr == PACHA_IPC_SYSCALL_RECV);
    ++receives;
    assert(message->fd_capacity == PACHA_IPC_MAX_TRANSFER_FDS && message->fds);
    if (copyout_failure) {
        message->fds[0].fd = 55; /* Already rolled back by the native kernel. */
        message->fd_count = 1;
        return PACHA_SYSCALL_ERR_INVALID;
    }
    if (recv_status) return recv_status;
    message->word0 = incoming.operation;
    message->word1 = incoming.generation;
    message->word2 = incoming.correlation;
    message->word3 = incoming.value;
    message->fd_count = incoming.fd_count;
    memcpy(message->fds, incoming.fds, incoming.fd_count * sizeof(incoming.fds[0]));
    return 0;
}

static void reset(void) {
    info = (struct pacha_fd_info){.kind = PACHA_FD_KIND_CHANNEL,
        .rights = PH_IPC_CHANNEL_RIGHTS};
    incoming = (struct ph_ipc_packet){.operation = 1, .generation = 7,
        .correlation = 19, .value = 123};
    for (unsigned int i = 0; i < PACHA_IPC_MAX_TRANSFER_FDS; ++i)
        incoming.fds[incoming.fd_count++].fd = 200 + i;
    info_status = send_status = recv_status = 0;
    inspections = sends = receives = close_fail_fd = 0;
    copyout_failure = 0;
    memset(closes, 0, sizeof(closes));
}

static void initialization(void) {
    reset();
    struct ph_ipc ipc = {0};
    assert(ph_ipc_init(NULL, 16, 7) == -EINVAL);
    assert(ph_ipc_init(&ipc, -1, 7) == -EINVAL);
    assert(ph_ipc_init(&ipc, 15, 7) == -EINVAL);
    assert(ph_ipc_init(&ipc, PACHA_FD_TABLE_LIMIT, 7) == -EINVAL);
    assert(ph_ipc_init(&ipc, 16, 0) == -EINVAL && !inspections);
    info_status = PACHA_SYSCALL_ERR_INVALID;
    assert(ph_ipc_init(&ipc, 16, 7) == -EINVAL && !ipc.fd);
    info_status = 0;
    info.kind = PACHA_FD_KIND_ENDPOINT;
    assert(ph_ipc_init(&ipc, 16, 7) == -ENOTSOCK && !ipc.generation);
    info.kind = PACHA_FD_KIND_CHANNEL;
    for (unsigned int bit = 0; bit < 9; ++bit) {
        if (!(PH_IPC_CHANNEL_RIGHTS & (1u << bit))) continue;
        info.rights = PH_IPC_CHANNEL_RIGHTS & ~(1u << bit);
        assert(ph_ipc_init(&ipc, 16, 7) == -EACCES && !ipc.admitted);
    }
    info.rights = PH_IPC_CHANNEL_RIGHTS;
    assert(!ph_ipc_init(&ipc, 16, 7));
    unsigned int before = inspections;
    assert(ph_ipc_init(&ipc, 17, 8) == -EBUSY && inspections == before);
    assert(!ph_ipc_destroy(&ipc, 7) && closes[16] == 1);
    assert(!ph_ipc_destroy(&ipc, 7) && closes[16] == 1);
    assert(ph_ipc_init(&ipc, 16, 7) == -ESTALE && inspections == before);
    assert(!ph_ipc_init(&ipc, 16, 8)); /* Native FD reuse, fresh channel. */
    assert(ph_ipc_revoke(&ipc, 7) == -ESTALE && ipc.admitted);
    assert(ph_ipc_destroy(&ipc, 7) == -ESTALE && closes[16] == 1);
    assert(!ph_ipc_destroy(&ipc, 8) && closes[16] == 2);
}

static void sending(void) {
    reset();
    struct ph_ipc ipc = {0};
    assert(!ph_ipc_init(&ipc, 16, 7));
    struct ph_ipc_packet packet = {.operation = 1, .generation = 7,
        .correlation = 19, .value = 123, .fd_count = 2,
        .fds = {{.fd = 100}, {.fd = 101, .transfer_flags = PACHA_IPC_TRANSFER_MOVE}}};
    struct ph_ipc_packet original = packet;
    const long failures[] = {PACHA_SYSCALL_ERR_NOT_READY, PACHA_SYSCALL_ERR_ALLOC,
        PACHA_SYSCALL_ERR_INVALID, PACHA_SYSCALL_ERR_MAP, PACHA_SYSCALL_ERR_CLOSED, 99};
    const int errors[] = {-EAGAIN, -ENOMEM, -EINVAL, -EIO, -EPIPE, -EPROTO};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        send_status = failures[i];
        assert(ph_ipc_send(&ipc, &packet) == errors[i]);
        assert(!memcmp(&packet, &original, sizeof(packet)));
    }
    sends = 0;
    packet.generation = 6;
    assert(ph_ipc_send(&ipc, &packet) == -ESTALE);
    packet = original; packet.fd_count = PACHA_IPC_MAX_TRANSFER_FDS + 1;
    assert(ph_ipc_send(&ipc, &packet) == -EINVAL);
    packet = original; packet.fds[1].fd = 100;
    assert(ph_ipc_send(&ipc, &packet) == -EINVAL); /* SHARE + MOVE alias. */
    packet.fds[0].transfer_flags = PACHA_IPC_TRANSFER_MOVE;
    packet.fds[1].transfer_flags = 0;
    assert(ph_ipc_send(&ipc, &packet) == -EINVAL); /* Reverse order. */
    packet = original; packet.fds[1].fd = 16;
    assert(ph_ipc_send(&ipc, &packet) == -EINVAL); /* Own endpoint MOVE. */
    packet = original; packet.fds[1].flags = UINT64_C(1) << 32;
    assert(ph_ipc_send(&ipc, &packet) == -EINVAL);
    packet = original; packet.fds[1].transfer_flags = UINT64_C(1) << 32;
    assert(ph_ipc_send(&ipc, &packet) == -EINVAL);
    assert(!sends);
    packet = original; send_status = 0;
    assert(!ph_ipc_send(&ipc, &packet));
    assert(packet.fds[0].fd == 100 && packet.fds[1].fd == PH_IPC_NO_FD);
    assert(!ph_ipc_packet_release(&packet));
    assert(closes[100] == 1 && !closes[101]);
    assert(!ph_ipc_revoke(&ipc, 7));
    assert(ph_ipc_send(&ipc, &original) == -ESHUTDOWN && sends == 1);
    assert(!ph_ipc_destroy(&ipc, 7));
}

static void receiving(void) {
    reset();
    struct ph_ipc ipc = {0};
    struct ph_ipc_packet out = {.operation = 99, .generation = 123};
    struct ph_ipc_packet before = out;
    assert(!ph_ipc_init(&ipc, 16, 7));
    recv_status = PACHA_SYSCALL_ERR_EMPTY;
    assert(ph_ipc_receive(&ipc, 7, &out) == -EAGAIN);
    assert(!memcmp(&out, &before, sizeof(out)));
    recv_status = PACHA_SYSCALL_ERR_ALLOC;
    assert(ph_ipc_receive(&ipc, 7, &out) == -ENOMEM);
    copyout_failure = 1;
    assert(ph_ipc_receive(&ipc, 7, &out) == -EINVAL && !closes[55]);
    assert(!ipc.rejected.fd_count && !memcmp(&out, &before, sizeof(out)));
    copyout_failure = 0; recv_status = 0;
    unsigned int calls = receives;
    assert(ph_ipc_receive(&ipc, 6, &out) == -ESTALE && receives == calls);
    assert(!ph_ipc_receive(&ipc, 7, &out));
    assert(!memcmp(&out, &incoming, sizeof(out)) && !ipc.rejected.fd_count);
    calls = receives;
    assert(ph_ipc_receive(&ipc, 7, &out) == -EBUSY && receives == calls);
    out.fds[0].fd = PH_IPC_NO_FD; /* Caller takes ownership separately. */
    close_fail_fd = 201;
    assert(ph_ipc_packet_release(&out) == -EIO && !closes[200]);
    assert(out.fds[1].fd == 201 && out.fds[2].fd == PH_IPC_NO_FD);
    close_fail_fd = 0;
    assert(!ph_ipc_packet_release(&out) && !out.fd_count);
    assert(closes[201] == 2 && closes[202] == 1);
    assert(!ph_ipc_packet_release(&out) && closes[201] == 2);
    assert(!ph_ipc_destroy(&ipc, 7));
}

static void rejected_cleanup(void) {
    for (unsigned int stale = 0; stale < 2; ++stale) {
        reset();
        struct ph_ipc ipc = {0};
        struct ph_ipc_packet out = {.value = 876};
        struct ph_ipc_packet before = out;
        assert(!ph_ipc_init(&ipc, 16, 7));
        if (stale) incoming.generation = 6;
        else incoming.operation = 0;
        close_fail_fd = 200;
        assert(ph_ipc_receive(&ipc, 7, &out) == -EIO);
        assert(!ipc.admitted && ipc.rejected.fds[0].fd == 200);
        assert(!memcmp(&out, &before, sizeof(out)));
        for (unsigned int i = 200; i < 219; ++i) assert(closes[i] == 1);
        assert(ph_ipc_receive(&ipc, 7, &out) == -ESHUTDOWN && receives == 1);
        assert(ph_ipc_destroy(&ipc, 7) == -EIO && closes[16] == 1 && !ipc.fd);
        assert(ph_ipc_init(&ipc, 16, 8) == -EBUSY);
        close_fail_fd = 0;
        assert(!ph_ipc_destroy(&ipc, 7) && closes[16] == 1 && closes[200] == 3);
        for (unsigned int i = 201; i < 219; ++i) assert(closes[i] == 1);
        assert(!ph_ipc_init(&ipc, 16, 8));
        close_fail_fd = 16;
        assert(ph_ipc_destroy(&ipc, 8) == -EIO && ipc.fd == 16 && !ipc.admitted);
        close_fail_fd = 0;
        assert(!ph_ipc_destroy(&ipc, 8) && closes[16] == 3);
    }
    reset();
    struct ph_ipc ipc = {0};
    struct ph_ipc_packet out = {0};
    assert(!ph_ipc_init(&ipc, 16, 7));
    incoming.generation = 6;
    assert(ph_ipc_receive(&ipc, 7, &out) == -ESTALE && !ipc.rejected.fd_count);
    assert(!ph_ipc_destroy(&ipc, 7));
}

int main(void) {
    initialization();
    sending();
    receiving();
    rejected_cleanup();
    puts("kobox2 native IPC ownership/generation unit: PASS");
    return 0;
}
