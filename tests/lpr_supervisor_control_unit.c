/* Production authorization and handoff lifecycle with a native FD mock.
 * Does not stand in for fork/exec handoff in a guest. */
#define main lprs_program_main
#include "../userland/lpr_supervisor/src/main.c"
#undef main
#include <assert.h>

static uint64_t native_rights[256];
static unsigned closed;
static int fail_create;
static unsigned killed;
static _Alignas(pacha_service_envelope_t) unsigned char activation_page[PACHA_SERVICE_PAGE_BYTES];
static struct pacha_ipc_fd activated_cap;
static unsigned activated;

int pacha_fd_table(uint64_t minimum, struct pacha_fd_table_info *info)
{
    assert(minimum <= 256);
    *info = (struct pacha_fd_table_info){ .capacity = 256, .maximum = 256 };
    for (unsigned fd = 16; fd < 256; fd++) info->free_slots += !native_rights[fd];
    return 0;
}

long pacha_syscall2(uint64_t number, uint64_t a0, uint64_t a1)
{
    assert(number == PACHA_PROCESS_SYSCALL_KILL && a0 == 94 && a1 == 1 && native_rights[94]);
    killed++;
    return 0;
}

void *pacha_mmap(int fd, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset)
{
    assert(fd == 70 && size == sizeof(activation_page) && !offset);
    assert(prot == (PACHA_PROT_READ | PACHA_PROT_WRITE) && flags == PACHA_MMAP_SHARED);
    return activation_page;
}

int pacha_munmap(void *page, uint64_t size)
{
    assert(page == activation_page && size == sizeof(activation_page));
    return 0;
}

int pacha_ipc_reply(int fd, const struct pacha_ipc_msg *reply)
{
    assert(fd == 71 && reply->word0 == PACHA_SERVICE_REPLY_MAGIC && !reply->word1);
    assert(reply->word3 == 55 && reply->fd_count == 1);
    activated_cap = reply->fds[0];
    activated++;
    return 0;
}

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
    closed++;
    return 0;
}

int pacha_ipc_channel_create(struct pacha_ipc_channel_pair *pair, uint64_t rights, uint32_t flags)
{
    assert(rights == LPRS_CHANNEL_RIGHTS && !flags);
    if (fail_create) return PACHA_ERR_ALLOC;
    int a = -1, b = -1;
    for (int fd = 16; fd < 256; fd++) {
        if (native_rights[fd]) continue;
        if (a < 0) a = fd;
        else { b = fd; break; }
    }
    assert(b >= 16);
    native_rights[a] = native_rights[b] = rights;
    *pair = (struct pacha_ipc_channel_pair){ a, b };
    return 0;
}

int main(void)
{
    lprs_process_t processes[3] = {
        { .active = 1, .token = 101, .pid = 10, .process_fd = 90, .control_fd = -1,
            .bootstrap_server_fd = -1, .bootstrap_client_fd = -1, .pending_exec_fd = -1 },
        { .active = 1, .token = 102, .pid = 11, .ppid = 10, .process_fd = -1, .control_fd = -1,
            .bootstrap_server_fd = -1, .bootstrap_client_fd = -1, .pending_exec_fd = -1 },
        { .active = 1, .token = 103, .pid = 12, .process_fd = 92, .control_fd = 93,
            .bootstrap_server_fd = -1, .bootstrap_client_fd = -1, .pending_exec_fd = -1 },
    };
    g_processes = processes; g_process_count = 3;
    native_rights[90] = native_rights[92] = native_rights[93] = PACHA_FD_RIGHT_CLOSE;
    lprs_reply_cap_t bootstrap = { .fd = -1 }, control = { .fd = -1 };
    assert(lprs_authorize(0, 0, LPRS_OP_PROCESS_REGISTER_EXEC, 0) == 0);
    assert(lprs_authorize(0, 0, LPRS_OP_PROCESS_GET_STATE, 101) == -PACHA_LINUX_EPERM);
    assert(lprs_prepare_control(&processes[0], &bootstrap) == 0);
    assert(bootstrap.rights == (LPRS_CLIENT_RIGHTS | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SET_FLAGS));
    assert(!bootstrap.transfer_flags);
    assert(lprs_prepare_control(&processes[0], &control) == PACHA_STATUS_EAGAIN);
    assert(lprs_authorize(101, 1, LPRS_OP_PROCESS_ACTIVATE, 101) == 0);
    assert(lprs_authorize(101, 1, LPRS_OP_PROCESS_GET_STATE, 101) == -PACHA_LINUX_EPERM);
    assert(lprs_authorize(101, 1, LPRS_OP_PROCESS_ACTIVATE, 102) == -PACHA_LINUX_EPERM);
    fail_create = 1;
    assert(lprs_activate_control(&processes[0], &control) == PACHA_STATUS_ENOMEM);
    assert(native_rights[bootstrap.fd] && processes[0].control_fd < 16 && !closed);
    fail_create = 0;
    assert(lprs_activate_control(&processes[0], &control) == 0);
    assert(control.rights == LPRS_CLIENT_RIGHTS && control.transfer_flags ==
        (PACHA_IPC_TRANSFER_PRIVATE | PACHA_IPC_TRANSFER_MOVE | PACHA_IPC_TRANSFER_CLOEXEC));
    assert(closed == 2 && processes[0].bootstrap_server_fd < 16);
    assert(lprs_authorize(101, 1, LPRS_OP_PROCESS_ACTIVATE, 101) == -PACHA_LINUX_EPERM);
    assert(lprs_authorize(101, 0, LPRS_OP_PROCESS_ACTIVATE, 101) == -PACHA_LINUX_EPERM);
    assert(lprs_authorize(101, 0, LPRS_OP_PROCESS_GET_STATE, 101) == 0);
    assert(lprs_authorize(101, 0, LPRS_OP_PROCESS_GET_STATE, 103) == -PACHA_LINUX_EPERM);
    assert(lprs_authorize(101, 0, LPRS_OP_PROCESS_REGISTER_EXEC, 101) == -PACHA_LINUX_EPERM);
    assert(lprs_authorize(101, 0, LPRS_OP_SIGNAL_DELIVER_TTY, 101) == -PACHA_LINUX_EPERM);
    assert(lprs_authorize(101, 0, LPRS_OP_PROCESS_FORK_PARENT_REGISTER, 102) == 0);
    assert(lprs_authorize(103, 0, LPRS_OP_PROCESS_FORK_PARENT_REGISTER, 102) == -PACHA_LINUX_EPERM);
    assert(lprs_authorize(101, 0, LPRS_OP_PROCESS_FORK_CANCEL, 102) == 0);

    assert(lprs_prepare_control(&processes[1], &bootstrap) == 0);
    assert(lprs_activate_control(&processes[1], &bootstrap) == PACHA_STATUS_EAGAIN);
    processes[1].process_fd = 91;
    native_rights[91] = PACHA_FD_RIGHT_CLOSE;
    processes[1].activation_page_fd = 70;
    processes[1].activation_reply_fd = 71;
    processes[1].activation_header = (pacha_service_envelope_t){ .request_id = 55,
        .abi_version = PACHA_SERVICE_ABI_VERSION, .service_id = LPRS_SERVICE_ID,
        .op = LPRS_OP_PROCESS_ACTIVATE };
    native_rights[70] = native_rights[71] = PACHA_FD_RIGHT_CLOSE;
    lprs_complete_activation(&processes[1]);
    assert(activated == 1 && activated_cap.rights == LPRS_CLIENT_RIGHTS &&
        activated_cap.transfer_flags == (PACHA_IPC_TRANSFER_MOVE | PACHA_IPC_TRANSFER_PRIVATE |
            PACHA_IPC_TRANSFER_CLOEXEC));
    assert(!native_rights[70] && !native_rights[71] && processes[1].activation_page_fd < 16);
    assert(((pacha_service_envelope_t *)activation_page)->status == 0);
    assert(lprs_authorize(101, 0, LPRS_OP_PROCESS_FORK_CANCEL, 102) == -PACHA_LINUX_EPERM);
    assert(lprs_authorize(102, 0, LPRS_OP_PROCESS_GET_STATE, 101) == -PACHA_LINUX_EPERM);
    assert(lprs_authorize(102, 0, LPRS_OP_PROCESS_GET_STATE, 102) == 0);
    assert(lprs_authorize(0, 0, LPRS_OP_PROCESS_EXEC_COMMIT_BEGIN, 102) == -PACHA_LINUX_EPERM);

    const int old_server = processes[0].control_fd;
    assert(lprs_prepare_exec_control(&processes[0], &bootstrap) == 0);
    assert(bootstrap.rights == LPRS_CLIENT_RIGHTS &&
        bootstrap.transfer_flags == PACHA_IPC_TRANSFER_PRIVATE);
    assert(native_rights[old_server]); /* exec prepare has not revoked the old image */
    const int abandoned_server = processes[0].bootstrap_server_fd;
    const int abandoned_client = processes[0].bootstrap_client_fd;
    assert(lprs_exec_commit_cancel(101) == 0);
    assert(!native_rights[abandoned_server] && !native_rights[abandoned_client]);
    assert(native_rights[old_server] && processes[0].control_fd == old_server);
    assert(lprs_authorize(101, 0, LPRS_OP_PROCESS_GET_STATE, 101) == 0);
    assert(lprs_prepare_exec_control(&processes[0], &bootstrap) == 0);
    lprs_reply_cap_t new_control;
    assert(lprs_activate_control(&processes[0], &new_control) == PACHA_STATUS_EAGAIN);
    native_rights[94] = PACHA_FD_RIGHT_CLOSE;
    assert(lprs_exec_commit_begin(101, 94) == 0);
    assert(lprs_exec_commit_cancel(101) == 0);
    assert(killed == 1 && !native_rights[94] && native_rights[old_server]);
    assert(lprs_prepare_exec_control(&processes[0], &bootstrap) == 0);
    native_rights[94] = PACHA_FD_RIGHT_CLOSE;
    assert(lprs_exec_commit_begin(101, 94) == 0);
    assert(lprs_activate_control(&processes[0], &new_control) == 0);
    assert(!native_rights[old_server]); /* activation revokes the old image */
    assert(lprs_exec_commit_done(101) == 0 && !native_rights[94]);
    assert(processes[0].process_fd == 90 && native_rights[90] && killed == 1);
    pacha_fd_close(new_control.fd); /* MOVE reply closes the service's client copy */
    unsigned baseline = 0;
    for (int fd = 16; fd < 256; fd++) baseline += !!native_rights[fd];
    for (unsigned cycle = 0; cycle < 1000; cycle++) {
        const uint64_t generation = processes[0].generation;
        assert(lprs_prepare_exec_control(&processes[0], &bootstrap) == 0);
        native_rights[94] = PACHA_FD_RIGHT_CLOSE;
        assert(lprs_exec_commit_begin(101, 94) == 0);
        assert(lprs_activate_control(&processes[0], &new_control) == 0);
        pacha_fd_close(new_control.fd);
        assert(lprs_exec_commit_done(101) == 0);
        assert(processes[0].generation > generation && processes[0].process_fd == 90);
        unsigned live = 0;
        for (int fd = 16; fd < 256; fd++) live += !!native_rights[fd];
        assert(live == baseline && !native_rights[94] && killed == 1);
    }
    processes[0].exit_ready = 1;
    assert(lprs_authorize(101, 0, LPRS_OP_PROCESS_GET_STATE, 101) == PACHA_STATUS_ESRCH);
    pacha_fd_close(92); pacha_fd_close(93);
    processes[2].process_fd = processes[2].control_fd = -1;
    processes[2].ppid = 10;
    assert(lprs_prepare_control(&processes[2], &bootstrap) == 0);
    const int orphan_server = processes[2].bootstrap_server_fd;
    lprs_orphan_children(10);
    assert(!processes[2].active && !native_rights[orphan_server] && !native_rights[bootstrap.fd]);
    for (int fd = 16; fd < 256; fd++) if (native_rights[fd]) pacha_fd_close(fd);
    for (int fd = 16; fd < 240; fd++) native_rights[fd] = PACHA_FD_RIGHT_CLOSE;
    processes[2].active = 1;
    assert(lprs_prepare_control(&processes[2], &bootstrap) == -PACHA_LINUX_EMFILE);
    assert(processes[2].bootstrap_server_fd < 16);
    memset(native_rights, 0, sizeof(native_rights));
    puts("lprs control: one-shot bootstrap, private rights, actor/subject isolation, fork relation, exec revocation, admission passed");
}
