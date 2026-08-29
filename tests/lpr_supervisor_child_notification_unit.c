#include <stdint.h>
#include <stdio.h>

#define main lprs_supervisor_program_main
#include "../userland/lpr_supervisor/src/main.c"
#undef main

static uint64_t g_syscall_count;
static uint64_t g_syscall_nr;
static uint64_t g_syscall_a0;
static uint64_t g_syscall_a1;
static long g_syscall_result;
static unsigned char g_waiter_page[PACHA_SERVICE_PAGE_BYTES];
static int g_closed_fds[2];
static uint64_t g_closed_fd_count;
static int g_reply_fd;
static struct pacha_ipc_msg g_reply;

long pacha_syscall2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    g_syscall_count++;
    g_syscall_nr = nr;
    g_syscall_a0 = a0;
    g_syscall_a1 = a1;
    return g_syscall_result;
}

int pacha_fd_close(int fd)
{
    if (g_closed_fd_count < 2) {
        g_closed_fds[g_closed_fd_count++] = fd;
    }
    return 0;
}

void *pacha_mmap(int fd, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset)
{
    (void)fd;
    (void)size;
    (void)prot;
    (void)flags;
    (void)offset;
    return g_waiter_page;
}

int pacha_munmap(void *addr, uint64_t size)
{
    (void)addr;
    (void)size;
    return 0;
}

int pacha_ipc_reply(int reply_fd, const struct pacha_ipc_msg *reply)
{
    g_reply_fd = reply_fd;
    g_reply = *reply;
    return 0;
}

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void)
{
    lprs_process_t processes[3] = {0};
    lprs_process_state_t state;
    uint64_t selected_index = UINT64_MAX;
    uint64_t exit_state = 0;
    uint64_t exit_code = 0;
    uint64_t match_count = 0;

    g_processes = processes;
    g_process_count = 3;
    g_process_capacity = 3;

    processes[0].active = 1;
    processes[0].pid = 41;
    processes[0].token = 123;
    processes[0].process_fd = 83;
    processes[1].active = 1;
    processes[1].pid = 42;
    processes[1].ppid = 41;
    processes[1].process_fd = 84;

    lprs_process_list_t process_list = {
        .token = 123,
        .offset = 0,
        .capacity = 3,
    };
    CHECK(lprs_list_processes(&process_list) == 0);
    CHECK(process_list.count == 2);
    CHECK(process_list.pids[0] == 41);
    CHECK(process_list.pids[1] == 42);
    process_list.offset = 1;
    process_list.capacity = 1;
    process_list.count = 0;
    CHECK(lprs_list_processes(&process_list) == 0);
    CHECK(process_list.count == 1);
    CHECK(process_list.pids[0] == 42);
    process_list.token = 999;
    CHECK(lprs_list_processes(&process_list) == PACHA_STATUS_ESRCH);

    lprs_notify_exited_child(&processes[1], LPRS_NATIVE_PROCESS_EXITED, 37);
    CHECK(processes[1].exit_ready == 1);
    CHECK(processes[1].exit_status == 37);
    CHECK(processes[1].exit_notified == 1);
    CHECK(g_syscall_count == 1);
    CHECK(g_syscall_nr == PACHA_PROCESS_SYSCALL_SIGNAL);
    CHECK(g_syscall_a0 == 83);
    CHECK(g_syscall_a1 == 17);

    lprs_notify_exited_child(&processes[1], LPRS_NATIVE_PROCESS_EXITED, 73);
    CHECK(processes[1].exit_status == 37);
    CHECK(g_syscall_count == 1);

    CHECK(lprs_find_exited_child(
        &processes[0], -1, &selected_index, &exit_state, &exit_code, &match_count) == 0);
    CHECK(selected_index == 1);
    CHECK(exit_state == LPRS_NATIVE_PROCESS_EXITED);
    CHECK(exit_code == 37);
    CHECK(match_count == 1);

    lprs_kill_t kill_request = {
        .token = processes[0].token,
        .pid = (int64_t)processes[1].pid,
        .signal = 0,
    };
    const uint64_t signal_calls = g_syscall_count;
    CHECK(lprs_kill(&kill_request) == PACHA_STATUS_ESRCH);
    CHECK(kill_request.delivered == 0);
    CHECK(g_syscall_count == signal_calls);

    lprs_write_state(&processes[0], &state);
    CHECK((state.flags & LPRS_PROCESS_STATE_HAS_CHILDREN) != 0);
    CHECK((state.flags & LPRS_PROCESS_STATE_SIGCHLD_PENDING) == 0);

    processes[2].active = 1;
    processes[2].pid = 43;
    processes[2].ppid = 41;
    processes[2].process_fd = 85;
    g_syscall_result = -7;
    lprs_notify_exited_child(&processes[2], LPRS_NATIVE_PROCESS_EXITED, 91);
    CHECK(processes[2].exit_ready == 1);
    CHECK(processes[2].exit_status == 91);
    CHECK(processes[2].exit_notified == 0);
    CHECK(g_syscall_count == 2);
    CHECK(g_syscall_nr == PACHA_PROCESS_SYSCALL_SIGNAL);
    CHECK(g_syscall_a0 == 83);
    CHECK(g_syscall_a1 == 17);

    lprs_notify_exited_child(&processes[2], LPRS_NATIVE_PROCESS_EXITED, 92);
    CHECK(processes[2].exit_status == 91);
    CHECK(g_syscall_count == 2);

    lprs_write_state(&processes[0], &state);
    CHECK((state.flags & LPRS_PROCESS_STATE_HAS_CHILDREN) != 0);
    CHECK((state.flags & LPRS_PROCESS_STATE_SIGCHLD_PENDING) != 0);

    lprs_waiter_t waiters[1] = {0};
    g_waiters = waiters;
    g_waiter_count = 1;
    g_waiter_capacity = 1;
    waiters[0].active = 1;
    waiters[0].page_fd = 90;
    waiters[0].reply_fd = 91;
    waiters[0].header.request_id = 92;
    waiters[0].request.token = processes[0].token;

    g_syscall_result = 0;
    g_closed_fd_count = 0;
    g_reply_fd = -1;
    memset(&g_reply, 0, sizeof(g_reply));
    kill_request.pid = (int64_t)processes[0].pid;
    kill_request.signal = 15;
    CHECK(lprs_kill(&kill_request) == 0);
    CHECK(kill_request.delivered == 1);
    CHECK(waiters[0].active == 0);
    CHECK(g_closed_fd_count == 2);
    CHECK(g_closed_fds[0] == 90);
    CHECK(g_closed_fds[1] == 91);
    CHECK(g_reply_fd == 91);
    CHECK((int64_t)g_reply.word1 == -PACHA_LINUX_EINTR);
    CHECK(g_reply.word3 == 92);
    const pacha_service_envelope_t *reply_header =
        (const pacha_service_envelope_t *)g_waiter_page;
    CHECK(reply_header->status == -PACHA_LINUX_EINTR);

    memset(waiters, 0, sizeof(waiters));
    waiters[0].active = 1;
    waiters[0].page_fd = 93;
    waiters[0].reply_fd = 94;
    waiters[0].header.request_id = 95;
    waiters[0].request.token = processes[0].token;
    kill_request.signal = 9;
    CHECK(lprs_kill(&kill_request) == 0);
    CHECK(kill_request.delivered == 1);
    CHECK(waiters[0].active == 1);

    lprs_process_t pdeath_processes[3] = {0};
    g_processes = pdeath_processes;
    g_process_count = 3;
    g_process_capacity = 3;
    pdeath_processes[0].active = 1;
    pdeath_processes[0].pid = 100;
    pdeath_processes[0].token = 200;
    pdeath_processes[0].process_fd = 120;
    pdeath_processes[1].active = 1;
    pdeath_processes[1].pid = 101;
    pdeath_processes[1].ppid = 100;
    pdeath_processes[1].token = 201;
    pdeath_processes[1].process_fd = 121;
    pdeath_processes[2].active = 1;
    pdeath_processes[2].pid = 102;
    pdeath_processes[2].ppid = 100;
    pdeath_processes[2].token = 202;
    pdeath_processes[2].process_fd = 122;

    lprs_pdeathsig_t pdeath_request = {
        .token = 201,
        .signal = 9,
    };
    CHECK(lprs_set_pdeathsig(&pdeath_request) == 0);
    CHECK(pdeath_processes[1].pdeath_signal == 9);
    pdeath_request.signal = 0;
    pdeath_request.result = 0;
    CHECK(lprs_get_pdeathsig(&pdeath_request) == 0);
    CHECK(pdeath_request.result == 9);

    /* CapabilityOS exec replaces the current process image in place.  The
     * staged process handle is temporary, while Linux prctl state belongs to
     * the surviving logical process. */
    g_closed_fd_count = 0;
    CHECK(lprs_exec_commit_begin(201, 130) == 0);
    CHECK(lprs_exec_commit_done(201) == 0);
    CHECK(pdeath_processes[1].process_fd == 121);
    CHECK(pdeath_processes[1].pending_exec_fd == -1);
    CHECK(pdeath_processes[1].pdeath_signal == 9);
    CHECK(g_closed_fd_count == 1 && g_closed_fds[0] == 130);

    g_syscall_count = 0;
    g_syscall_result = 0;
    lprs_orphan_children(100);
    CHECK(g_syscall_count == 1);
    CHECK(g_syscall_nr == PACHA_PROCESS_SYSCALL_KILL);
    CHECK(g_syscall_a0 == 121);
    CHECK(g_syscall_a1 == 137);
    CHECK(pdeath_processes[1].pdeath_signal == 0);
    CHECK(pdeath_processes[1].ppid == 0);
    CHECK(pdeath_processes[2].ppid == 0);

    puts("LPRS_CHILD_NOTIFICATION_UNIT=OK");
    return 0;
}
