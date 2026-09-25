#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_socket.c"

lpr_state_t lpr_state;
static int transport_error, server_error, foreign_alive, moved;
static unsigned foreign_closes, lease_closes, sender_closes, reply_closes;
static uint64_t request_id;

void *lpr_memset(void *dst, int value, size_t bytes) { return memset(dst, value, bytes); }
void lpr_state_lock(volatile uint32_t *word) { assert(!*word); *word = 1; }
void lpr_state_unlock(volatile uint32_t *word) { assert(*word); *word = 0; }
uint64_t lpr_next_request_id(volatile uint64_t *counter) { return ++*counter; }
uint64_t pacha_trace_name_id(const char *name) { (void)name; return 0; }
void pacha_trace_emit(uint32_t c, uint32_t e, uint32_t cls, uint32_t n,
    uint64_t a, uint64_t b, uint64_t d, uint64_t f, uint64_t g, uint64_t h)
{ (void)c; (void)e; (void)cls; (void)n; (void)a; (void)b; (void)d; (void)f; (void)g; (void)h; }
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE);
    if (fd == 100) lease_closes++;
    else if (fd == 101) {
        if (foreign_alive) { foreign_closes++; foreign_alive = 0; }
        else sender_closes++;
    } else { assert(fd == 90); reply_closes++; }
    return 0;
}
int64_t lpr_pacha_syscall3(uint64_t nr, uint64_t raw, uint64_t rights, uint64_t flags)
{
    assert(nr == PACHAOS_SYSCALL_IPC_CHANNEL_CREATE);
    assert(rights & PACHA_FD_RIGHT_TRANSFER);
    assert(flags == PACHA_FD_FLAG_CLOEXEC);
    uint64_t *pair = (void *)(uintptr_t)raw;
    pair[0] = 100; pair[1] = 101;
    return 0;
}
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t raw)
{
    assert(nr == PACHAOS_SYSCALL_IPC_CALL && fd == LPR_NETD_ENDPOINT_FD);
    const struct pacha_ipc_msg *request = (void *)(uintptr_t)raw;
    assert(request->word1 == NETD_OP_PAGE_ATTACH && request->fd_count == 2);
    assert(request->fds[0].fd == 20 && request->fds[1].fd == 101);
    request_id = request->word3;
    if (transport_error) return transport_error; /* No transfer took place. */
    moved = (request->fds[1].transfer_flags & PACHA_IPC_TRANSFER_MOVE) != 0;
    /* IPC_CALL consumes MOVE descriptors before returning a reply FD.
     * Another thread immediately reuses the released sender slot for its RPC.
     */
    if (moved) foreign_alive = 1;
    return 90;
}
int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t fd, uint64_t raw, uint64_t timeout, uint64_t flags)
{
    assert(nr == PACHAOS_SYSCALL_IPC_RECV_WAIT && fd == 90);
    assert(timeout == UINT64_MAX && flags == 0);
    struct pacha_ipc_msg *reply = (void *)(uintptr_t)raw;
    reply->word0 = PACHA_SERVICE_REPLY_MAGIC;
    reply->word1 = (uint64_t)(int64_t)server_error;
    reply->word2 = server_error ? 0 : 42;
    reply->word3 = request_id;
    return 0;
}
static int scenario(int send_error, int reply_error)
{
    memset(&lpr_state, 0, sizeof(lpr_state));
    transport_error = send_error; server_error = reply_error;
    foreign_alive = moved = 0;
    foreign_closes = lease_closes = sender_closes = reply_closes = 0;
    lpr_netd_page_slot_t *slot = &lpr_state.netd_rpc.page_slots[0];
    slot->busy = 1; slot->page_fd = 20; slot->page = (void *)(uintptr_t)4096;
    slot->lease_fd = -1;
    const int64_t status = lpr_netd_page_attach(20);
    const int expected_error = send_error ? send_error : reply_error;
    if (status != expected_error || foreign_closes != 0 || sender_closes != 1 ||
        lease_closes != (unsigned)(expected_error != 0) ||
        reply_closes != (unsigned)(send_error == 0) ||
        (!expected_error && (slot->lease_fd != 100 || slot->attachment_id != 42)))
    {
        fprintf(stderr, "FAIL: send=%d reply=%d status=%lld foreign_closes=%u sender_closes=%u lease_closes=%u\n",
            send_error, reply_error, (long long)status, foreign_closes, sender_closes, lease_closes);
        return 0;
    }
    return 1;
}
int main(void)
{
    int ok = scenario(0, 0);
    ok &= scenario(0, -LPR_LINUX_EINVAL);
    ok &= scenario(-LPR_LINUX_EPIPE, 0);
    if (!ok) return 1;
    puts("LPR_NETD_ATTACHMENT_FD_UNIT=OK");
    return 0;
}
