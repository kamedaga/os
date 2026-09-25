/* Actual supervisor registry/session helpers with native FD and unixd RPC
 * mocks. Guest IPC delivery and native process exit are separate tests. */
#define main lprs_program_main
#include "../userland/lpr_supervisor/src/main.c"
#undef main
#include "../userland/libaccount/src/account.c"
#include <assert.h>

static uint64_t native_rights[256];
static unsigned registrations, signals;
static int fail_registration, bad_cap;
static struct unix_credentials registered_credentials;
static _Alignas(8) unsigned char filed_page[PACHA_SERVICE_PAGE_BYTES];
static filed_identity_t filed_identity;
static unsigned filed_registrations;
static unsigned filed_updates, unix_updates;

static int allocate_filed_fd(uint64_t rights)
{
    int fd;
    for (fd = 200; fd < 256 && native_rights[fd]; fd++) {}
    assert(fd < 256);
    native_rights[fd] = rights;
    return fd;
}
int pacha_vmo_create(uint64_t bytes, uint64_t rights, uint32_t flags)
{
    assert(bytes == sizeof(filed_page) && flags == PACHA_FD_FLAG_PRIVATE);
    memset(filed_page, 0, sizeof(filed_page));
    return allocate_filed_fd(rights);
}
void *pacha_mmap(int fd, uint64_t bytes, uint64_t prot, uint64_t flags, uint64_t offset)
{
    assert(fd >= 200 && native_rights[fd] && bytes == sizeof(filed_page) && !offset);
    assert(prot == (PACHA_PROT_READ | PACHA_PROT_WRITE) && flags == PACHA_MMAP_SHARED);
    return filed_page;
}
int pacha_ipc_call(int fd, const struct pacha_ipc_msg *message)
{
    assert(fd == 33 && message->fd_count == 1);
    pacha_service_envelope_t *header = (void *)filed_page;
    assert(header->service_id == FILED_SERVICE_ID);
    if (header->op == FILED_OP_CLIENT_REGISTER) {
        memcpy(&filed_identity, filed_page + PACHA_SERVICE_HEADER_BYTES, sizeof(filed_identity));
        filed_registrations++;
    } else {
        assert(header->op == FILED_OP_CLIENT_CREDENTIALS);
        filed_identity_update_t *update = (void *)(filed_page + PACHA_SERVICE_HEADER_BYTES);
        assert(update->client == 1 && update->identity.rights == filed_identity.rights);
        filed_identity = update->identity;
        filed_updates++;
    }
    return allocate_filed_fd(PACHA_FD_RIGHT_CLOSE);
}
int pacha_ipc_recv_wait(int fd, struct pacha_ipc_msg *response, uint64_t timeout)
{
    assert(fd >= 200 && timeout == PACHA_FD_WAIT_FOREVER);
    pacha_service_envelope_t header = *(pacha_service_envelope_t *)filed_page;
    pacha_service_reply_init((void *)filed_page, &header, 0, 0, filed_registrations, 0);
    response->word0 = PACHA_SERVICE_REPLY_MAGIC;
    response->word3 = header.request_id;
    if (header.op == FILED_OP_CLIENT_CREDENTIALS) { response->fd_count = 0; return 0; }
    response->fd_count = 1;
    response->fds[0].fd = allocate_filed_fd(LPRS_CLIENT_RIGHTS | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER);
    return 0;
}

int pacha_fd_table(uint64_t minimum, struct pacha_fd_table_info *info)
{
    assert(minimum <= 256);
    *info = (struct pacha_fd_table_info){ .capacity = 256, .maximum = 256 };
    for (unsigned fd = 16; fd < 256; fd++) info->free_slots += !native_rights[fd];
    return 0;
}

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
    assert(address == filed_page && bytes == sizeof(filed_page));
    return 0;
}

int unix_client_call(int endpoint, struct unix_control *request,
    const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received)
{
    assert(endpoint == 32 && request->request);
    if (request->operation == UNIX_OP_PROCESS_CREDENTIALS) {
        assert(request->socket && !send_count && !capacity);
        registered_credentials = request->credentials;
        unix_updates++;
        *received = 0;
        return 0;
    }
    assert(request->operation == UNIX_OP_PROCESS_REGISTER);
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
    struct pacha_accounts accounts = {0};
    const char *passwd = "root:x:0:0:root:/root:/bin/bash\nsvc:x:81:81:service:/run/svc:/sbin/nologin\n";
    const char *group = "root:x:0:\nsvc:x:81:\nshared:x:42:root,svc\n";
    assert(!pacha_accounts_parse(&accounts, passwd, strlen(passwd), group, strlen(group)));
    lprs_credentials_t service;
    assert(!lprs_resolve_account(&accounts, "svc", &service));
    assert(service.uid == 81 && service.gid == 81 && service.group_count == 1 && service.groups[0] == 42);
    assert(lprs_resolve_account(&accounts, "absent", &service) == -2);
    char oversized[4096];
    size_t used = (size_t)snprintf(oversized, sizeof(oversized), "root:x:0:\nsvc:x:81:\n");
    for (unsigned i = 0; i <= LPRS_MAX_GROUPS; i++)
        used += (size_t)snprintf(oversized + used, sizeof(oversized) - used,
            "g%u:x:%u:svc\n", i, 1000 + i);
    struct pacha_accounts many = {0};
    assert(!pacha_accounts_parse(&many, passwd, strlen(passwd), oversized, used));
    lprs_credentials_t unchanged = service;
    assert(lprs_resolve_account(&many, "svc", &service) == -ERANGE);
    assert(!memcmp(&service, &unchanged, sizeof(service)));
    pacha_accounts_destroy(&many);
    g_accounts = accounts;
    g_accounts_ready = 1;
    g_process_capacity = 3;
    g_processes = calloc(g_process_capacity, sizeof(*g_processes));
    assert(g_processes);
    g_unix_admin_fd = 32;
    g_filed_admin_fd = 33;
    native_rights[33] = PACHA_FD_RIGHT_CLOSE;
    native_rights[32] = PACHA_FD_RIGHT_CLOSE;
    lprs_register_exec_t registration = { .state.pid = 999, .account = "root", .filed_rights = 1023 };
    uint64_t token = 0;
    strcpy(registration.account, "missing");
    assert(lprs_register_exec(&registration, &token) == -2 && !token && g_next_pid == 1);
    registration.account[0] = 0;
    assert(lprs_register_exec(&registration, &token) == -22 && !token);
    strcpy(registration.account, "root");
    registration.credential_rights = 4;
    assert(lprs_register_exec(&registration, &token) == -22 && !token);
    registration.credential_rights = 0;
    assert(lprs_register_exec(&registration, &token) == 0);
    lprs_process_t *parent = lprs_find_by_token(token);
    assert(parent && parent->pid == 1 && parent->credentials.pid == 1);
    assert(!parent->credentials.uid && !parent->credentials.gid && parent->credentials.generation);
    assert(parent->group_count == 1 && parent->groups[0] == 42);
    assert(registration.state.credentials.group_count == 1 && registration.state.credentials.groups[0] == 42);
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
    assert(filed_identity.pid == 1 && filed_identity.generation == parent->credentials.generation);
    assert(parent->filed_fd >= 200 && parent->filed_session == 1);
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

    lprs_credential_request_t change = { .token = token,
        .operation = LPRS_CREDENTIAL_UID, .ids = {81} };
    assert(lprs_change_credentials(parent, &change) == -1);
    assert(!filed_updates && !unix_updates); /* UID zero grants nothing. */
    parent->credential_rights = LPRS_CREDENTIAL_SETUID | LPRS_CREDENTIAL_SETGID;
    assert(!lprs_change_credentials(parent, &change));
    assert(filed_updates == 1 && unix_updates == 1);
    assert(filed_identity.uid == 81 && registered_credentials.uid == 81 && change.credentials.uid == 81);
    assert(filed_identity.generation == identity_generation);
    assert(lprs_authorize(token, 0, LPRS_OP_PROCESS_CREDENTIALS, token) == 0);
    assert(lprs_authorize(0, 0, LPRS_OP_PROCESS_CREDENTIALS, token) == -1);

    /* Fork copies the registry, not the parent's private session authority. */
    lprs_fork_t fork = {0};
    assert(lprs_fork_begin(token, &fork) == 0);
    lprs_process_t *child = lprs_find_by_token(fork.child_token);
    assert(child && child->credentials.pid == 2 && child->unix_fd < 16 && !child->unix_session);
    assert(child->credentials.uid == 81 && child->credential_rights == parent->credential_rights &&
        child->filed_rights == parent->filed_rights);
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

    /* State is an output, never an application-supplied credential update. */
    registration = (lprs_register_exec_t){ .account = "root", .state.credentials = {
        .uid = 999, .group_count = 2, .groups = {0,999} } };
    assert(lprs_register_exec(&registration, &token) == 0);
    parent = lprs_find_by_token(token);
    assert(registration.state.credentials.uid == 0);
    parent->credentials.uid = 1001; parent->credentials.euid = 1002; parent->credentials.suid = 1003;
    parent->credentials.gid = 2001; parent->credentials.egid = 2002; parent->credentials.sgid = 2003;
    lprs_process_state_t state;
    lprs_write_state(parent, &state);
    const lprs_credentials_t expected = { .uid=1001, .euid=1002, .suid=1003,
        .gid=2001, .egid=2002, .sgid=2003, .group_count=1, .groups={42} };
    assert(memcmp(&state.credentials, &expected, sizeof(expected)) == 0);
    assert(lprs_fork_begin(token, &fork) == 0);
    child = lprs_find_by_token(fork.child_token);
    lprs_write_state(child, &state);
    assert(memcmp(&state.credentials, &expected, sizeof(expected)) == 0);
    child->pending_exec_fd = 41;
    native_rights[41] = PACHA_FD_RIGHT_CLOSE;
    assert(lprs_exec_commit_done(child->token) == 0);
    lprs_write_state(child, &state);
    assert(memcmp(&state.credentials, &expected, sizeof(expected)) == 0);
    lprs_process_reap(child);
    lprs_process_reap(parent);
    pacha_fd_close(32);
    pacha_fd_close(33);
    for (int fd = 16; fd < 256; fd++) assert(!native_rights[fd]);
    free(g_processes);
    puts("lprs unix session: trusted registry, private delivery, retry/exec reuse, fork isolation, zombie release passed");
}
