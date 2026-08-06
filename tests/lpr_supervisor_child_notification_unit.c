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
    (void)fd;
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

    puts("LPRS_CHILD_NOTIFICATION_UNIT=OK");
    return 0;
}
