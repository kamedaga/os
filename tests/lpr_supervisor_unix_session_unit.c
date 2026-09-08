/* Actual supervisor registry/session helpers with native FD and unixd RPC
 * mocks. Guest IPC delivery and native process exit are separate tests. */
#define main lprs_program_main
#include "../userland/lpr_supervisor/src/main.c"
#undef main
#include <assert.h>

static uint64_t native_rights[256];
static unsigned registrations, signals;
static int fail_registration, bad_cap;
static struct unix_credentials registered_credentials;

int64_t pacha_kernel_status_to_errno(int64_t status) { return status; }

int pacha_fd_get_info(int fd, struct pacha_fd_info *info)
{
    if (fd < 16 || fd >= 256 || !native_rights[fd]) return -1;
    *info = (struct pacha_fd_info){ .kind = PACHA_FD_KIND_CHANNEL, .rights = native_rights[fd] };
    return 0;
}

int pacha_fd_close(int fd)
{
    assert(fd >= 16 && fd < 256 && native_rights[fd]);
    native_rights[fd] = 0;
    return 0;
}

long pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    assert(nr == PACHA_PROCESS_SYSCALL_SIGNAL && a0 == 31 && a1 == LPRS_SIGCHLD);
    signals++;
    return 0;
}

int pacha_munmap(void *address, uint64_t bytes)
{
    (void)address; (void)bytes;
    assert(!"no diagnostic mapping in this fixture");
    return -1;
}

int unix_client_call(int endpoint, struct unix_control *request,
    const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received)
{
    assert(endpoint == 32 && request->request && request->operation == UNIX_OP_PROCESS_REGISTER);
    assert(!send && !send_count && capacity == 1);
    assert(!request->socket && !request->argument && !request->transaction);
    registrations++;
    registered_credentials = request->credentials;
    if (fail_registration) return -PACHA_LINUX_ENOMEM;
    int fd;
    for (fd = 16; fd < 256 && native_rights[fd]; fd++) {}
    assert(fd < 256);
    native_rights[fd] = LPRS_UNIX_CLIENT_RIGHTS | PACHA_FD_RIGHT_DUP |
        (bad_cap == 1 ? 0 : PACHA_FD_RIGHT_TRANSFER);
    if (bad_cap == 2) native_rights[fd] &= ~PACHA_FD_RIGHT_WAIT;
    if (bad_cap == 3) native_rights[fd] &= ~PACHA_FD_RIGHT_POLL;
    *receive = (struct pacha_ipc_fd){ .fd = (uint64_t)fd };
    *received = 1;
    request->result = 100 + registrations;
    return 0;
}

int main(void)
{
    g_process_capacity = 3;
    g_processes = calloc(g_process_capacity, sizeof(*g_processes));
    assert(g_processes);
    g_unix_admin_fd = 32;
    native_rights[32] = PACHA_FD_RIGHT_CLOSE;
    lprs_register_exec_t registration = { .state.pid = 999 };
    uint64_t token = 0;
    assert(lprs_register_exec(&registration, &token) == 0);
    lprs_process_t *parent = lprs_find_by_token(token);
    assert(parent && parent->pid == 1 && parent->credentials.pid == 1);
    assert(!parent->credentials.uid && !parent->credentials.gid && parent->credentials.generation);
    lprs_reply_cap_t reply = { .fd = -1 };
    uint64_t session = 0;
    assert(lprs_unix_session(parent, &reply, &session) == PACHA_STATUS_EAGAIN && !registrations);
    parent->process_fd = 31; parent->control_fd = 30;
    native_rights[30] = native_rights[31] = PACHA_FD_RIGHT_CLOSE;
    assert(lprs_authorize(0, 0, LPRS_OP_PROCESS_UNIX_SESSION, token) == -PACHA_LINUX_EPERM);
    assert(lprs_authorize(token, 1, LPRS_OP_PROCESS_UNIX_SESSION, token) == -PACHA_LINUX_EPERM);
    assert(lprs_authorize(token, 0, LPRS_OP_PROCESS_UNIX_SESSION, token) == 0);
    fail_registration = 1;
    assert(lprs_unix_session(parent, &reply, &session) == -PACHA_LINUX_ENOMEM);
    assert(parent->unix_fd < 16 && !parent->unix_session);
    fail_registration = 0;
    for (bad_cap = 1; bad_cap <= 3; bad_cap++) {
        assert(lprs_unix_session(parent, &reply, &session) == -PACHA_LINUX_EIO);
        assert(parent->unix_fd < 16 && !parent->unix_session && !native_rights[16]);
    }
    bad_cap = 0;
    assert(lprs_unix_session(parent, &reply, &session) == 0);
    assert(registered_credentials.pid == 1 && !registered_credentials.uid && !registered_credentials.euid);
    assert(session && session == parent->unix_session && reply.fd == parent->unix_fd);
    assert(reply.rights == LPRS_UNIX_CLIENT_RIGHTS && reply.transfer_flags ==
        (PACHA_IPC_TRANSFER_PRIVATE | PACHA_IPC_TRANSFER_CLOEXEC));
    const unsigned created = registrations;
    const int retained = reply.fd;
    const uint64_t identity_generation = parent->credentials.generation;
    for (unsigned i = 0; i < 1000; i++) {
        assert(lprs_unix_session(parent, &reply, &session) == 0);
        assert(registrations == created && reply.fd == retained && native_rights[retained]);
    }
    parent->pending_exec_fd = 41;
    native_rights[41] = PACHA_FD_RIGHT_CLOSE;
    assert(lprs_exec_commit_done(token) == 0 && !native_rights[41]);
    assert(parent->credentials.generation == identity_generation);
    assert(lprs_unix_session(parent, &reply, &session) == 0 && registrations == created);
    assert(lprs_exec_commit_cancel(token) == 0 && native_rights[retained]);

    /* Fork copies the registry, not the parent's private session authority. */
    lprs_fork_t fork = {0};
    assert(lprs_fork_begin(token, &fork) == 0);
    lprs_process_t *child = lprs_find_by_token(fork.child_token);
    assert(child && child->credentials.pid == 2 && child->unix_fd < 16 && !child->unix_session);
    assert(child->credentials.generation != parent->credentials.generation);
    assert(lprs_authorize(token, 0, LPRS_OP_PROCESS_UNIX_SESSION, child->token) == -PACHA_LINUX_EPERM);
    child->process_fd = 43; child->control_fd = 44;
    native_rights[43] = native_rights[44] = PACHA_FD_RIGHT_CLOSE;
    assert(lprs_unix_session(child, &reply, &session) == 0);
    assert(registered_credentials.pid == 2 && session != parent->unix_session && reply.fd != retained);
    const int child_retained = reply.fd;
    lprs_notify_exited_child(child, LPRS_NATIVE_PROCESS_EXITED, 0);
    assert(signals == 1 && child->active && child->exit_ready); /* unreaped zombie */
    assert(child->unix_fd < 16 && !child->unix_session && !native_rights[child_retained]);
    assert(native_rights[retained]);
    assert(lprs_unix_session(child, &reply, &session) == PACHA_STATUS_ESRCH);
    lprs_process_reap(child);
    lprs_process_reap(parent);
    assert(!native_rights[retained]);
    pacha_fd_close(32);
    for (int fd = 16; fd < 256; fd++) assert(!native_rights[fd]);
    free(g_processes);
    puts("lprs unix session: trusted registry, private delivery, retry/exec reuse, fork isolation, zombie release passed");
}
