static int lpr_load_bootstrap(void)
{
    if (lpr_bootstrap_checked) {
        return lpr_bootstrap_valid;
    }
    lpr_bootstrap_checked = 1;
    lpr_memset(&lpr_bootstrap, 0, sizeof(lpr_bootstrap));
    const int64_t got = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_FD_READ,
        LPR_BOOTSTRAP_FD,
        (uint64_t)(uintptr_t)&lpr_bootstrap,
        sizeof(lpr_bootstrap));
    if (got != (int64_t)sizeof(lpr_bootstrap) ||
        lpr_bootstrap.magic != LPR_BOOTSTRAP_MAGIC ||
        lpr_bootstrap.version != LPR_BOOTSTRAP_VERSION ||
        lpr_bootstrap.byte_size < sizeof(lpr_bootstrap) ||
        lpr_bootstrap.local_fd_count > LPR_FD_TABLE_MAX_SIZE ||
        lpr_bootstrap.local_fd_count > UINT64_MAX / sizeof(lpr_bootstrap_fd_t))
    {
        goto invalid;
    }

    const uint64_t expected_table_bytes =
        lpr_bootstrap.local_fd_count * sizeof(lpr_bootstrap_fd_t);
    if (lpr_bootstrap.local_fd_table_bytes != expected_table_bytes ||
        lpr_bootstrap.byte_size != sizeof(lpr_bootstrap) + expected_table_bytes)
    {
        goto invalid;
    }
    if (lpr_bootstrap.local_fd_count != 0) {
        if (lpr_bootstrap.local_fd_table_offset != sizeof(lpr_bootstrap) ||
            lpr_bootstrap.local_fd_table_offset > lpr_bootstrap.byte_size ||
            lpr_bootstrap.local_fd_table_bytes >
                lpr_bootstrap.byte_size - lpr_bootstrap.local_fd_table_offset)
        {
            goto invalid;
        }
        const int64_t mapped = lpr_pacha_syscall6(
            PACHAOS_SYSCALL_MMAP,
            LPR_BOOTSTRAP_FD,
            0,
            lpr_bootstrap.byte_size,
            PACHAOS_PROT_READ,
            PACHAOS_MMAP_SHARED,
            0);
        if (mapped < 4096) {
            goto invalid;
        }
        const struct lpr_bootstrap *mapped_bootstrap =
            (const struct lpr_bootstrap *)(uintptr_t)mapped;
        const lpr_bootstrap_fd_t *descs =
            (const lpr_bootstrap_fd_t *)((uintptr_t)mapped +
                lpr_bootstrap.local_fd_table_offset);
        const int install_ok =
            mapped_bootstrap->magic == LPR_BOOTSTRAP_MAGIC &&
            mapped_bootstrap->version == LPR_BOOTSTRAP_VERSION &&
            mapped_bootstrap->byte_size == lpr_bootstrap.byte_size &&
            mapped_bootstrap->local_fd_count == lpr_bootstrap.local_fd_count &&
            lpr_install_bootstrap_local_fds(descs, lpr_bootstrap.local_fd_count);
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)mapped,
            lpr_bootstrap.byte_size);
        if (!install_ok) {
            goto invalid;
        }
    } else if (!lpr_install_bootstrap_local_fds(0, 0)) {
        goto invalid;
    }
    if ((lpr_bootstrap.flags & LPR_BOOTSTRAP_FLAG_SUPERVISOR) != 0 &&
        lpr_bootstrap.supervisor_token != 0)
    {
        lpr_supervisor_token = lpr_bootstrap.supervisor_token;
        lpr_supervisor_enabled = 1;
        const int restore_status = lpr_supervisor_fd_table_restore(lpr_supervisor_token);
        if (restore_status != 0) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, 127);
            for (;;) {
            }
        }
    }
    lpr_bootstrap_valid = 1;
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, LPR_BOOTSTRAP_FD);
    return 1;

invalid:
    lpr_memset(&lpr_bootstrap, 0, sizeof(lpr_bootstrap));
    lpr_supervisor_enabled = 0;
    lpr_supervisor_token = 0;
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, LPR_BOOTSTRAP_FD);
    return 0;
}

static int lpr_path_is_terminated(const char *path, uint64_t capacity)
{
    return path != 0 && lpr_strnlen(path, capacity) < capacity;
}

static void lpr_cwd_set_root(void)
{
    lpr_cwd_handle = 0;
    lpr_memset(lpr_cwd_path, 0, sizeof(lpr_cwd_path));
    lpr_cwd_path[0] = '/';
}

static void lpr_cwd_init(void)
{
    if (lpr_cwd_checked) {
        return;
    }
    lpr_cwd_checked = 1;
    lpr_cwd_set_root();
    if (!lpr_load_bootstrap()) {
        return;
    }
    if (lpr_supervisor_enabled) {
        lprs_v2_process_state_t state;
        if (lpr_supervisor_get_state(&state) == 0 &&
            state.cwd[0] == '/' &&
            lpr_path_is_terminated(state.cwd, sizeof(state.cwd)))
        {
            const uint64_t len = (uint64_t)lpr_strnlen(state.cwd, sizeof(state.cwd));
            if (len < sizeof(lpr_cwd_path)) {
                lpr_memset(lpr_cwd_path, 0, sizeof(lpr_cwd_path));
                lpr_memcpy(lpr_cwd_path, state.cwd, (size_t)len + 1u);
                lpr_cwd_handle = state.cwd_handle;
            }
        }
        return;
    }
    if (lpr_bootstrap.cwd[0] == '/' &&
        lpr_path_is_terminated(lpr_bootstrap.cwd, sizeof(lpr_bootstrap.cwd)))
    {
        const uint64_t len = (uint64_t)lpr_strnlen(lpr_bootstrap.cwd, sizeof(lpr_bootstrap.cwd));
        if (len < sizeof(lpr_cwd_path)) {
            lpr_memset(lpr_cwd_path, 0, sizeof(lpr_cwd_path));
            lpr_memcpy(lpr_cwd_path, lpr_bootstrap.cwd, (size_t)len + 1u);
            lpr_cwd_handle = lpr_bootstrap.cwd_handle;
        }
    }
}

static void lpr_linux_process_state_init(void)
{
    if (lpr_linux_process_state_checked) {
        return;
    }
    lpr_linux_process_state_checked = 1;

    const int64_t kernel_pid_raw = lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID);
    int32_t fallback_pid = kernel_pid_raw > 0 && kernel_pid_raw <= INT32_MAX ?
        (int32_t)kernel_pid_raw :
        1;
    if (lpr_load_bootstrap()) {
        if (lpr_supervisor_enabled) {
            lprs_v2_process_state_t state;
            const int state_status = lpr_supervisor_get_state(&state);
            if (state_status != 0 ||
                state.pid == 0 ||
                state.pid > INT32_MAX ||
                state.ppid > INT32_MAX ||
                state.sid > INT32_MAX ||
                state.pgrp > INT32_MAX)
            {
                (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, 127);
                for (;;) {
                }
            } else {
                lpr_linux_current_pid = (int32_t)state.pid;
                lpr_linux_current_ppid = (int32_t)state.ppid;
                lpr_linux_current_sid = (int32_t)state.sid;
                lpr_linux_current_pgrp = (int32_t)state.pgrp;
                lpr_linux_next_pid = lpr_linux_current_pid + 1;
            }
        } else {
            lpr_linux_current_pid = (int32_t)lpr_bootstrap.linux_pid;
            if (lpr_linux_current_pid <= 0) {
                lpr_linux_current_pid = fallback_pid;
            }
            lpr_linux_current_ppid = (int32_t)lpr_bootstrap.linux_ppid;
            lpr_linux_current_sid = (int32_t)lpr_bootstrap.linux_sid;
            lpr_linux_current_pgrp = (int32_t)lpr_bootstrap.linux_pgrp;
            lpr_linux_next_pid = (int32_t)lpr_bootstrap.linux_next_pid;
        }
    } else {
        lpr_linux_current_pid = fallback_pid;
        lpr_linux_current_ppid = 0;
        lpr_linux_current_sid = lpr_linux_current_pid;
        lpr_linux_current_pgrp = lpr_linux_current_pid;
        lpr_linux_next_pid = lpr_linux_current_pid + 1;
    }
    if (lpr_linux_current_sid <= 0) {
        lpr_linux_current_sid = lpr_linux_current_pid;
    }
    if (lpr_linux_current_pgrp <= 0) {
        lpr_linux_current_pgrp = lpr_linux_current_pid;
    }
    if (lpr_linux_next_pid <= lpr_linux_current_pid) {
        lpr_linux_next_pid = lpr_linux_current_pid + 1;
    }
}

static lpr_linux_process_entry_t *lpr_linux_process_find(int32_t linux_pid)
{
    if (linux_pid <= 0) {
        return 0;
    }
    for (uint64_t i = 0; i < LPR_LINUX_PROCESS_TABLE_SIZE; i++) {
        if (lpr_linux_processes[i].active &&
            lpr_linux_processes[i].linux_pid == linux_pid)
        {
            return &lpr_linux_processes[i];
        }
    }
    return 0;
}

static lpr_linux_process_entry_t *lpr_linux_process_slot(void)
{
    for (uint64_t i = 0; i < LPR_LINUX_PROCESS_TABLE_SIZE; i++) {
        if (!lpr_linux_processes[i].active) {
            return &lpr_linux_processes[i];
        }
    }
    return 0;
}

static int32_t lpr_linux_alloc_child_pid(void)
{
    lpr_linux_process_state_init();
    for (uint64_t tries = 0; tries < 32768u; tries++) {
        int32_t pid = lpr_linux_next_pid++;
        if (pid <= 1) {
            lpr_linux_next_pid = 2;
            pid = lpr_linux_next_pid++;
        }
        if (pid == lpr_linux_current_pid || lpr_linux_process_find(pid) != 0) {
            continue;
        }
        return pid;
    }
    return -1;
}

static int lpr_linux_process_register(
    int32_t linux_pid,
    int32_t linux_ppid,
    int32_t linux_sid,
    int32_t linux_pgrp,
    int process_fd)
{
    if (linux_pid <= 0 || process_fd < 16) {
        return -LPR_LINUX_EINVAL;
    }
    lpr_linux_process_entry_t *entry = lpr_linux_process_find(linux_pid);
    if (entry == 0) {
        entry = lpr_linux_process_slot();
    }
    if (entry == 0) {
        return -LPR_LINUX_EAGAIN;
    }
    lpr_memset(entry, 0, sizeof(*entry));
    entry->active = 1;
    entry->linux_pid = linux_pid;
    entry->linux_ppid = linux_ppid;
    entry->linux_sid = linux_sid;
    entry->linux_pgrp = linux_pgrp;
    entry->process_fd = process_fd;
    lpr_trace_process_event("process_register", (uint64_t)(uint32_t)linux_pid, (uint64_t)(uint32_t)process_fd, 0);
    return 0;
}

static void lpr_linux_process_clear_children(void)
{
    lpr_memset(lpr_linux_processes, 0, sizeof(lpr_linux_processes));
}

