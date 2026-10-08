/* SPDX-License-Identifier: MIT */
#define LPRS_BOOT_DIAGNOSTICS 1
#define main lprs_program_main
#include "../userland/lpr_supervisor/src/main.c"
#undef main
#include <assert.h>

static unsigned logs, inspections, closes, timer_reads;
static size_t longest;
static long log_status, read_status = 8;
static int timer_result = 70, info_status;
static char last_record[257];
static char records[128][257];
static uint64_t info_kind = PACHA_FD_KIND_PROCESS;

long pacha_syscall2(uint64_t nr, uint64_t address, uint64_t bytes)
{
    assert(nr == 1 && address && bytes <= 256);
    if (bytes > longest) longest = (size_t)bytes;
    memcpy(last_record, (const void *)(uintptr_t)address, (size_t)bytes);
    last_record[bytes] = 0;
    assert(logs < 128);
    memcpy(records[logs], last_record, (size_t)bytes + 1);
    ++logs;
    return log_status;
}

int pacha_timerfd_create(uint64_t initial, uint64_t interval, uint64_t rights,
    uint32_t flags)
{
    assert(initial == 2000000000ull && interval == initial);
    assert(rights == (PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE));
    assert(flags == (PACHA_FD_FLAG_CLOEXEC | PACHA_FD_FLAG_PRIVATE));
    return timer_result;
}

int pacha_fd_close(int fd)
{
    assert(fd == 70);
    ++closes;
    return 0;
}

long pacha_fd_read(int fd, void *buffer, uint64_t bytes)
{
    assert(fd == 70 && bytes == 8);
    *(uint64_t *)buffer = 1;
    ++timer_reads;
    return read_status;
}

int pacha_fd_get_info(int fd, struct pacha_fd_info *info)
{
    assert(fd == 90 || fd == 91 || fd == 92 || fd == 95);
    ++inspections;
    if (info_status) return info_status;
    *info = (struct pacha_fd_info){.kind = info_kind, .extra = UINT64_MAX,
        .size = UINT64_MAX};
    return 0;
}

int main(void)
{
    lprs_diag_slot_t slot = {.seq = UINT64_MAX - 1, .syscall_nr = UINT64_MAX,
        .arg0 = UINT64_MAX, .arg1 = UINT64_MAX, .enter_tick = UINT64_MAX};
    lprs_process_t processes[6] = {
        {.active = 1, .pid = 1, .ppid = UINT64_MAX,
         .process_fd = 90, .pending_exec_fd = -1,
         .diag_page = &slot},
        {.active = 1, .pid = 4, .process_fd = -1, .pending_exec_fd = 91},
        {.active = 1, .pid = 16, .ppid = UINT64_MAX, .process_fd = 92,
         .diag_page = &slot},
        {.active = 1, .pid = 17, .process_fd = 93},
        {.active = 0, .pid = 2, .process_fd = 94},
        {.active = 1, .pid = 5, .process_fd = 95},
    };
    memset(processes[2].comm, '\n', sizeof(processes[2].comm));
    memset(processes[2].cmdline, '\x80', sizeof(processes[2].cmdline));
    lprs_process_t before[6];
    memcpy(before, processes, sizeof(before));
    const lprs_diag_slot_t slot_before = slot;
    g_processes = processes;
    g_process_count = 6;
    assert(!lprs_boot_diag_start() && g_boot_diag_timer == 70);
    g_boot_diag_sample = UINT64_MAX - 1;
    assert(!lprs_boot_diag_tick());
    assert(inspections == 4 && timer_reads == 1 && logs == 21);
    assert(!memcmp(before, processes, sizeof(before)));
    assert(!memcmp(&slot_before, &slot, sizeof(slot)));
    assert(strstr(last_record, "pid=5") && strstr(last_record, "valid=0"));
    assert(strstr(records[11], "pid=10") && strstr(records[11], "ppid=ffffffffffffffff"));
    assert(strstr(records[11], "comm_truncated=1 cmdline_truncated=1"));
    assert(strstr(records[12], "group=comm text=????????????????????????????????\n"));
    assert(strstr(records[13], "group=cmdline text="));
    assert(strchr(records[13], '\n') == records[13] + strlen(records[13]) - 1);
    assert(strlen(strstr(records[13], "text=") + 5) == 97);
    const unsigned first_tick_logs = logs;
    assert(!lprs_boot_diag_tick());
    assert(logs == first_tick_logs + 8); /* Identity is emitted only once. */
    assert(longest <= 256);
    lprs_boot_diag_event(&processes[0], UINT64_MAX, INT64_MIN);
    assert(strstr(last_record, "event=ffffffffffffffff"));
    assert(strstr(last_record, "status=8000000000000000"));
    const unsigned event_logs = logs;
    lprs_boot_diag_event(&processes[3], 3, 0);
    lprs_boot_diag_event(NULL, 3, 0);
    assert(logs == event_logs);

    slot.seq = 1; /* An in-flight write must never be represented as valid. */
    g_process_count = 1;
    info_status = -EIO;
    assert(!lprs_boot_diag_tick());
    assert(strstr(last_record, "group=syscall valid=0"));
    info_status = 0;
    info_kind = PACHA_FD_KIND_VMO;
    assert(!lprs_boot_diag_tick());
    assert(strstr(last_record, "group=syscall valid=0"));
    assert(!memcmp(before, processes, sizeof(before)));

    char full[257];
    memset(full, 'x', sizeof(full));
    assert(!lprs_boot_diag_record(full, 256));
    unsigned written = logs;
    assert(lprs_boot_diag_record(full, 257) == -1 && logs == written);
    assert(g_boot_diag_error == -EOVERFLOW);
    g_boot_diag_error = 0;
    log_status = 1;
    assert(lprs_boot_diag_record(full, 1) == -1 && g_boot_diag_error == 1);
    log_status = -EIO;
    assert(lprs_boot_diag_record(full, 1) == -1 && g_boot_diag_error == -EIO);
    log_status = 0;
    read_status = -EIO;
    assert(lprs_boot_diag_tick() == -1 && g_boot_diag_error == -EIO);
    lprs_boot_diag_stop();
    lprs_boot_diag_stop();
    assert(closes == 1 && g_boot_diag_timer == -1);
    g_boot_diag_error = 0;
    log_status = 1;
    assert(lprs_boot_diag_start() == -1 && closes == 2 && g_boot_diag_timer == -1);
    log_status = 0;
    timer_result = 1;
    assert(lprs_boot_diag_start() == -1 && closes == 2 && g_boot_diag_timer == -1);
    printf("lprs boot diagnostic: read-only snapshots, torn validity, PID1..16, once ASCII names, timer cleanup, <=256 raw LOG PASS max=%zu\n", longest);
    return 0;
}
