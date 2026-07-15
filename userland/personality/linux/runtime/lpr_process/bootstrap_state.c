#include "../lpr_filed_internal.h"

#define LPR_IMAGE_ABI_MISMATCH_EXIT_STATUS 127ull

static void lpr_image_abi_mismatch_exit(uint64_t actual_version)
{
    pacha_trace5(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_IMAGE_ABI_MISMATCH,
        PACHA_TRACE_CLASS_ERROR,
        actual_version,
        LPR_IMAGE_ABI_VERSION,
        offsetof(lpr_manifest_t, image_abi_version),
        sizeof(lpr_manifest_t),
        LPR_IMAGE_ABI_MISMATCH_EXIT_STATUS);
    (void)lpr_pacha_syscall1(
        PACHAOS_SYSCALL_PROCESS_EXIT,
        LPR_IMAGE_ABI_MISMATCH_EXIT_STATUS);
    for (;;) {
    }
}

int lpr_load_manifest(void)
{
    if (lpr_manifest_checked) {
        return lpr_manifest_valid;
    }
    lpr_manifest_checked = 1;
    lpr_memset(&lpr_process_manifest, 0, sizeof(lpr_process_manifest));
    const int64_t got = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_FD_READ,
        LPR_BOOTSTRAP_FD,
        (uint64_t)(uintptr_t)&lpr_process_manifest,
        sizeof(lpr_process_manifest));
    if (got != (int64_t)sizeof(lpr_process_manifest) ||
        lpr_process_manifest.magic != LPR_MANIFEST_MAGIC)
    {
        goto invalid;
    }
    if (lpr_process_manifest.image_abi_version != LPR_IMAGE_ABI_VERSION) {
        lpr_image_abi_mismatch_exit(lpr_process_manifest.image_abi_version);
    }
    if (lpr_process_manifest.byte_size < sizeof(lpr_process_manifest) ||
        lpr_process_manifest.entry_count > LPR_FD_TABLE_MAX_SIZE)
    {
        goto invalid;
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        LPR_BOOTSTRAP_FD,
        0,
        lpr_process_manifest.byte_size,
        PACHAOS_PROT_READ,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        goto invalid;
    }
    const lpr_manifest_t *mapped_manifest =
        (const lpr_manifest_t *)(uintptr_t)mapped;
    const int install_ok =
        lpr_manifest_validate(mapped_manifest, lpr_process_manifest.byte_size) &&
        lpr_install_manifest_fds(mapped_manifest);
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_MUNMAP,
        (uint64_t)(uintptr_t)mapped,
        lpr_process_manifest.byte_size);
    if (!install_ok) goto invalid;
    if ((lpr_process_manifest.flags & LPR_MANIFEST_FLAG_SUPERVISOR) != 0 &&
        lpr_process_manifest.supervisor_token != 0)
    {
        lpr_supervisor_token = lpr_process_manifest.supervisor_token;
        lpr_supervisor_enabled = 1;
        const int64_t commit_status = lpr_supervisor_call_token(
            LPRS_OP_PROCESS_EXEC_COMMIT_DONE,
            lpr_supervisor_token,
            -1,
            0);
        if (commit_status != 0) {
            (void)lpr_pacha_syscall1(
                PACHAOS_SYSCALL_PROCESS_EXIT,
                LPR_IMAGE_ABI_MISMATCH_EXIT_STATUS);
            for (;;) {
            }
        }
    }
    lpr_manifest_valid = 1;
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, LPR_BOOTSTRAP_FD);
    return 1;

invalid:
    lpr_memset(&lpr_process_manifest, 0, sizeof(lpr_process_manifest));
    lpr_supervisor_enabled = 0;
    lpr_supervisor_token = 0;
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, LPR_BOOTSTRAP_FD);
    return 0;
}

int lpr_path_is_terminated(const char *path, uint64_t capacity)
{
    return path != 0 && lpr_strnlen(path, capacity) < capacity;
}

void lpr_cwd_set_root(void)
{
    lpr_cwd_handle = 0;
    lpr_memset(lpr_cwd_path, 0, sizeof(lpr_cwd_path));
    lpr_cwd_path[0] = '/';
}

void lpr_cwd_init(void)
{
    if (lpr_cwd_checked) {
        return;
    }
    lpr_cwd_checked = 1;
    lpr_cwd_set_root();
    if (!lpr_load_manifest()) {
        return;
    }
    /* The exec bootstrap owns the transferred cwd handle.  The supervisor's
     * process metadata is only a fallback because its fork copy may be stale. */
    if (lpr_process_manifest.cwd[0] == '/' &&
        lpr_path_is_terminated(lpr_process_manifest.cwd, sizeof(lpr_process_manifest.cwd)))
    {
        const uint64_t len = (uint64_t)lpr_strnlen(
            lpr_process_manifest.cwd, sizeof(lpr_process_manifest.cwd));
        if (len < sizeof(lpr_cwd_path)) {
            lpr_memset(lpr_cwd_path, 0, sizeof(lpr_cwd_path));
            lpr_memcpy(lpr_cwd_path, lpr_process_manifest.cwd, (size_t)len + 1u);
            lpr_cwd_handle = lpr_process_manifest.cwd_handle;
        }
        return;
    }
    if (lpr_supervisor_enabled) {
        lprs_process_state_t state;
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
    }
}

void lpr_linux_process_state_init(void)
{
    if (lpr_linux_process_state_checked) {
        return;
    }
    lpr_linux_process_state_checked = 1;

    const int64_t kernel_pid_raw = lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID);
    int32_t fallback_pid = kernel_pid_raw > 0 && kernel_pid_raw <= INT32_MAX ?
        (int32_t)kernel_pid_raw :
        1;
    if (lpr_load_manifest()) {
        if (lpr_supervisor_enabled) {
            lprs_process_state_t state;
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
            lpr_linux_current_pid = (int32_t)lpr_process_manifest.linux_pid;
            if (lpr_linux_current_pid <= 0) {
                lpr_linux_current_pid = fallback_pid;
            }
            lpr_linux_current_ppid = (int32_t)lpr_process_manifest.linux_ppid;
            lpr_linux_current_sid = (int32_t)lpr_process_manifest.linux_sid;
            lpr_linux_current_pgrp = (int32_t)lpr_process_manifest.linux_pgrp;
            lpr_linux_next_pid = (int32_t)lpr_process_manifest.linux_next_pid;
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

lpr_linux_process_entry_t *lpr_linux_process_find(int32_t linux_pid)
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

int32_t lpr_linux_alloc_child_pid(void)
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

int lpr_linux_process_register(
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

void lpr_linux_process_clear_children(void)
{
    lpr_memset(lpr_linux_processes, 0, sizeof(lpr_linux_processes));
}
