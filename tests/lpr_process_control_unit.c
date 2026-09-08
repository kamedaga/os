#include "../userland/personality/linux/runtime/lpr_process/client.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static _Alignas(pacha_service_envelope_t) unsigned char page[PACHA_SERVICE_PAGE_BYTES];
static unsigned live_control, live_reply, live_bootstrap, live_unix, called, cancelled;
static int bad_session;
static uint64_t missing_unix_rights;
static int bad_rights, bad_flags;
static int prepare_busy;
static int activation_bootstrap = 246;
static uint32_t current_op;
static uint64_t current_request;

void *lpr_memset(void *p, int value, size_t size) { return memset(p, value, size); }

void lpr_trace_error_record(uint64_t domain, uint64_t op, uint64_t stage,
    int64_t status, int64_t raw, uint64_t request, uint64_t count,
    uint64_t subject, uint64_t child, const char *text)
{
    (void)domain; (void)op; (void)stage; (void)status; (void)raw;
    (void)request; (void)count; (void)subject; (void)child; (void)text;
}

int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE);
    if (fd == 20) { assert(live_control); live_control = 0; }
    else if (fd == 21) { assert(live_bootstrap); live_bootstrap = 0; }
    else if (fd == 22) { assert(live_unix); live_unix = 0; }
    else { assert(fd == 30 && live_reply); live_reply = 0; }
    return 0;
}

int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    if (nr == PACHAOS_SYSCALL_FD_GET_INFO) {
        assert((a0 == 20 && live_control) || (a0 == 21 && live_bootstrap) || (a0 == 22 && live_unix));
        struct pacha_fd_info *info = (struct pacha_fd_info *)(uintptr_t)a1;
        *info = (struct pacha_fd_info){ .kind = PACHA_FD_KIND_CHANNEL,
            .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_CALL |
                (a0 == 22 ? (PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL) & ~missing_unix_rights : 0) |
                (bad_rights ? PACHA_FD_RIGHT_DUP : 0),
            .flags = bad_flags == 1 ? 0 : bad_flags == 2 ? PACHA_FD_FLAG_PRIVATE :
                bad_flags == 3 ? (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC) :
                PACHA_FD_FLAG_PRIVATE | (a0 != 21 ? PACHA_FD_FLAG_CLOEXEC : 0) };
        return 0;
    }
    assert(nr == PACHAOS_SYSCALL_IPC_CALL && !live_reply);
    const struct pacha_ipc_msg *request = (const struct pacha_ipc_msg *)(uintptr_t)a1;
    pacha_service_envelope_t *header = (pacha_service_envelope_t *)page;
    current_op = header->op;
    assert(a0 == (current_op == LPRS_OP_PROCESS_ACTIVATE ? (uint64_t)activation_bootstrap : 20));
    if (current_op != LPRS_OP_PROCESS_ACTIVATE) assert(live_control);
    assert(request->word3 == header->request_id && request->fd_count == 1);
    current_request = header->request_id;
    header->magic = PACHA_SERVICE_REPLY_MAGIC;
    header->status = 0;
    if (current_op == LPRS_OP_PROCESS_EXEC_COMMIT_CANCEL) cancelled++;
    live_reply = 1; called++;
    return 30;
}

int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t fd, uint64_t address, uint64_t timeout, uint64_t flags)
{
    assert(nr == PACHAOS_SYSCALL_IPC_RECV_WAIT && fd == 30 && live_reply && timeout == UINT64_MAX && !flags);
    struct pacha_ipc_msg *reply = (struct pacha_ipc_msg *)(uintptr_t)address;
    reply->word0 = PACHA_SERVICE_REPLY_MAGIC;
    reply->word3 = current_request;
    if (current_op == LPRS_OP_PROCESS_UNIX_SESSION) {
        assert(reply->fd_capacity == 1 && !live_unix);
        reply->fd_count = 1;
        reply->fds[0] = (struct pacha_ipc_fd){ .fd = 22 };
        reply->word2 = bad_session ? 0 : 700;
        live_unix = 1;
    }
    if (current_op == LPRS_OP_PROCESS_EXEC_PREPARE) {
        if (prepare_busy) { reply->word1 = (uint64_t)-LPR_LINUX_EAGAIN; return 0; }
        assert(reply->fd_capacity == 1 && !live_bootstrap);
        reply->fd_count = 1;
        reply->fds[0] = (struct pacha_ipc_fd){ .fd = 21 };
        live_bootstrap = 1;
    }
    if (current_op == LPRS_OP_PROCESS_ACTIVATE) {
        assert(reply->fd_capacity == 1 && !live_control);
        reply->fd_count = 1;
        reply->fds[0] = (struct pacha_ipc_fd){ .fd = 20 };
        live_control = 1;
    }
    return 0;
}

static int64_t status_to_errno(int64_t status) { return status; }

int main(void)
{
    uint64_t request = 0;
    assert(lpr_process_client_call(&request, status_to_errno, LPRS_OP_HELLO, 40,
        page, 0, -1, NULL) == -LPR_LINUX_ENOTCONN);
    assert(!called);
    for (unsigned test = 0; test < 4; test++) {
        bad_rights = test == 0;
        bad_flags = test == 1 ? 1 : test == 2 ? 2 : 0;
        int64_t result = lpr_process_client_activate(&request, status_to_errno, 246, 101, 40, page);
        if (test < 3) {
            assert(result == -LPR_LINUX_EIO && !live_control && lpr_process_control_fd < 16);
        } else {
            assert(!result && live_control && lpr_process_control_fd == 20);
            assert(lpr_process_client_call(&request, status_to_errno, LPRS_OP_HELLO, 40,
                page, 0, -1, NULL) == 0);
        }
        assert(!live_reply);
    }
    for (unsigned test = 0; test < 7; test++) {
        bad_rights = test == 0;
        bad_flags = test == 1 ? 1 : test == 2 ? 2 : 0;
        bad_session = test == 3;
        missing_unix_rights = test == 4 ? PACHA_FD_RIGHT_WAIT : test == 5 ? PACHA_FD_RIGHT_POLL : 0;
        uint64_t session = 123;
        int fd = 99;
        const int64_t result = lpr_process_client_unix_session(&request, status_to_errno,
            101, 40, page, &session, &fd);
        if (test < 6) assert(result == -LPR_LINUX_EIO && session == 0 && fd == -1 && !live_unix);
        else {
            assert(result == 0 && session == 700 && fd == 22 && live_unix);
            lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, 22);
        }
        assert(!live_reply && live_control && !cancelled);
    }
    int handoff = -1;
    prepare_busy = 1;
    assert(lpr_process_client_prepare_exec(&request, status_to_errno, 101, 40, page,
        &handoff) == -LPR_LINUX_EAGAIN);
    assert(!cancelled && !live_bootstrap && handoff == -1 && live_control);
    prepare_busy = 0;
    for (unsigned test = 0; test < 4; test++) {
        bad_rights = test == 0;
        bad_flags = test == 1 ? 1 : test == 2 ? 3 : 0;
        int64_t result = lpr_process_client_prepare_exec(&request, status_to_errno, 101, 40, page, &handoff);
        if (test < 3) {
            assert(result == -LPR_LINUX_EIO && !live_bootstrap && handoff == -1);
            assert(cancelled == test + 1);
        } else assert(result == 0 && live_bootstrap && handoff == 21);
        assert(live_control && lpr_process_control_fd == 20 && !live_reply);
        assert(lpr_process_client_call(&request, status_to_errno, LPRS_OP_HELLO,
            40, page, 0, -1, NULL) == 0);
    }
    /* Model successful exec's CLOEXEC close, then the new image activating
     * using the dynamic handoff named in its manifest. */
    lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, 20);
    lpr_process_control_fd = -1;
    activation_bootstrap = handoff;
    assert(lpr_process_client_activate(&request, status_to_errno, handoff, 101, 40, page) == 0);
    lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, handoff);
    assert(live_control && !live_bootstrap && !live_reply);
    lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, 20);
    puts("lpr process control: private/CLOEXEC validation, exec handoff, prepare rejection cleanup, old route retained passed");
}
