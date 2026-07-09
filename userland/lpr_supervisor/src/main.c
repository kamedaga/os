#include "lpr_supervisor/boot_config.h"
#include "lpr_supervisor/ipc_protocol_v2.h"

#include <pacha/abi.h>
#include <pacha/error_conveyor.h>
#include <pacha/ipc.h>
#include <pacha/syscall.h>
#include <pacha/trace.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    LPRS_ERR_PERM = -1,
    LPRS_ERR_SRCH = -3,
    LPRS_ERR_IO = -5,
    LPRS_ERR_CHILD = -10,
    LPRS_ERR_AGAIN = -11,
    LPRS_ERR_NOMEM = -12,
    LPRS_ERR_FAULT = -14,
    LPRS_ERR_INVAL = -22,
    LPRS_WNOHANG = 1,
};

typedef struct lprs_fd_slot {
    uint8_t active;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t file_index;
    uint64_t fd;
    uint64_t fd_flags;
} lprs_fd_slot_t;

typedef struct lprs_file {
    uint8_t active;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t refcount;
    uint64_t kind;
    uint64_t status_flags;
    uint64_t handle;
    uint64_t offset_or_counter;
} lprs_file_t;

typedef struct lprs_filedesc {
    lprs_fd_slot_t *slots;
    uint64_t slot_count;
    uint64_t slot_capacity;
    lprs_file_t *files;
    uint64_t file_count;
    uint64_t file_capacity;
} lprs_filedesc_t;

typedef struct lprs_process {
    uint8_t active;
    uint8_t waited;
    uint16_t reserved0;
    uint32_t reserved1;
    uint64_t token;
    uint64_t pid;
    uint64_t ppid;
    uint64_t sid;
    uint64_t pgrp;
    uint64_t foreground_pgrp;
    uint64_t cwd_handle;
    int process_fd;
    char ctty[LPRS_V2_CTTY_BYTES];
    char cwd[LPRS_V2_CWD_BYTES];
    lprs_filedesc_t filedesc;
    lprs_filedesc_t staged_filedesc;
    uint8_t fd_replace_active;
} lprs_process_t;

typedef struct lprs_process_status {
    uint64_t state;
    uint64_t exit_code;
    uint64_t id;
    uint64_t generation;
} lprs_process_status_t;

static int g_endpoint_fd = -1;
static uint64_t g_next_pid = 1;
static uint64_t g_next_token = 0x4c50525300000001ull;
static lprs_process_t *g_processes;
static uint64_t g_process_count;
static uint64_t g_process_capacity;
static pacha_errconv_store_t g_error_store;
static int g_error_store_ready;

static int lprs_service_one_pending_request(void);

static int lprs_status_to_errno(long status)
{
    if (status == 0) {
        return 0;
    }
    if (status == 2 || status == -2) {
        return LPRS_ERR_AGAIN;
    }
    if (status > 0) {
        return -(int)status;
    }
    return (int)status;
}

static pacha_errconv_store_t *lprs_errors(void)
{
    if (!g_error_store_ready) {
        pacha_errconv_store_init(&g_error_store, PACHA_ERRCONV_COMPONENT_LPR_SUPERVISOR);
        g_error_store_ready = 1;
    }
    return &g_error_store;
}

static uint64_t lprs_error_token(
    int64_t status,
    uint64_t op,
    uint64_t stage,
    int64_t raw_status,
    uint64_t request_id,
    uint64_t fd_count,
    uint64_t subject,
    uint64_t child_token,
    const char *text)
{
    return pacha_errconv_error_token(
        lprs_errors(),
        status,
        PACHA_ERRCONV_DOMAIN_LPRS_STATUS,
        op,
        stage,
        raw_status,
        request_id,
        fd_count,
        subject,
        child_token,
        text);
}

static int lprs_parse_fd_arg(const char *arg, int *out_fd)
{
    const char prefix[] = "--boot-fd=";
    if (arg == NULL || out_fd == NULL) {
        return LPRS_ERR_INVAL;
    }
    const size_t prefix_len = sizeof(prefix) - 1u;
    if (strncmp(arg, prefix, prefix_len) != 0) {
        return LPRS_ERR_INVAL;
    }
    const char *p = arg + prefix_len;
    if (*p == '\0') {
        return LPRS_ERR_INVAL;
    }
    uint64_t value = 0;
    while (*p != '\0') {
        if (*p < '0' || *p > '9') {
            return LPRS_ERR_INVAL;
        }
        value = value * 10u + (uint64_t)(*p - '0');
        if (value > UINT32_MAX) {
            return LPRS_ERR_INVAL;
        }
        p++;
    }
    if (value < 16) {
        return LPRS_ERR_INVAL;
    }
    *out_fd = (int)(uint32_t)value;
    return 0;
}

static int lprs_find_bootstrap_fd(int argc, char **argv, int *out_fd)
{
    if (argc <= 0 || argv == NULL || out_fd == NULL) {
        return LPRS_ERR_INVAL;
    }
    for (int i = 1; i < argc; ++i) {
        if (lprs_parse_fd_arg(argv[i], out_fd) == 0) {
            return 0;
        }
    }
    return LPRS_ERR_INVAL;
}

static int lprs_read_bootstrap(int fd, struct lprs_boot_config *out)
{
    if (fd < 16 || out == NULL) {
        return LPRS_ERR_INVAL;
    }
    memset(out, 0, sizeof(*out));
    const long got = pacha_fd_read(fd, out, sizeof(*out));
    if (got != (long)sizeof(*out)) {
        return LPRS_ERR_IO;
    }
    if (out->magic != LPRS_BOOT_CONFIG_MAGIC || out->endpoint_fd < 16) {
        return LPRS_ERR_INVAL;
    }
    return 0;
}

static int lprs_ensure_process_capacity(uint64_t needed)
{
    if (needed <= g_process_capacity) {
        return 0;
    }
    uint64_t new_capacity = g_process_capacity == 0 ? 16 : g_process_capacity;
    while (new_capacity < needed) {
        if (new_capacity > UINT64_MAX / 2u) {
            return LPRS_ERR_NOMEM;
        }
        new_capacity *= 2u;
    }
    lprs_process_t *new_processes =
        (lprs_process_t *)realloc(g_processes, (size_t)(new_capacity * sizeof(*new_processes)));
    if (new_processes == NULL) {
        return LPRS_ERR_NOMEM;
    }
    memset(new_processes + g_process_capacity, 0,
        (size_t)((new_capacity - g_process_capacity) * sizeof(*new_processes)));
    g_processes = new_processes;
    g_process_capacity = new_capacity;
    return 0;
}

static void lprs_filedesc_release(lprs_filedesc_t *fdp)
{
    if (fdp == NULL) {
        return;
    }
    free(fdp->slots);
    free(fdp->files);
    memset(fdp, 0, sizeof(*fdp));
}

static void lprs_filedesc_swap(lprs_filedesc_t *a, lprs_filedesc_t *b)
{
    lprs_filedesc_t tmp = *a;
    *a = *b;
    *b = tmp;
}

static int lprs_filedesc_ensure_slots(lprs_filedesc_t *fdp, uint64_t needed)
{
    if (needed <= fdp->slot_capacity) {
        return 0;
    }
    uint64_t new_capacity = fdp->slot_capacity == 0 ? 16 : fdp->slot_capacity;
    while (new_capacity < needed) {
        if (new_capacity > UINT64_MAX / 2u) {
            return LPRS_ERR_NOMEM;
        }
        new_capacity *= 2u;
    }
    lprs_fd_slot_t *new_slots =
        (lprs_fd_slot_t *)realloc(fdp->slots, (size_t)(new_capacity * sizeof(*new_slots)));
    if (new_slots == NULL) {
        return LPRS_ERR_NOMEM;
    }
    memset(new_slots + fdp->slot_capacity, 0,
        (size_t)((new_capacity - fdp->slot_capacity) * sizeof(*new_slots)));
    fdp->slots = new_slots;
    fdp->slot_capacity = new_capacity;
    return 0;
}

static int lprs_filedesc_ensure_files(lprs_filedesc_t *fdp, uint64_t needed)
{
    if (needed <= fdp->file_capacity) {
        return 0;
    }
    uint64_t new_capacity = fdp->file_capacity == 0 ? 16 : fdp->file_capacity;
    while (new_capacity < needed) {
        if (new_capacity > UINT64_MAX / 2u) {
            return LPRS_ERR_NOMEM;
        }
        new_capacity *= 2u;
    }
    lprs_file_t *new_files =
        (lprs_file_t *)realloc(fdp->files, (size_t)(new_capacity * sizeof(*new_files)));
    if (new_files == NULL) {
        return LPRS_ERR_NOMEM;
    }
    memset(new_files + fdp->file_capacity, 0,
        (size_t)((new_capacity - fdp->file_capacity) * sizeof(*new_files)));
    fdp->files = new_files;
    fdp->file_capacity = new_capacity;
    return 0;
}

static int lprs_filedesc_append_desc(lprs_filedesc_t *fdp, const lprs_v2_fd_desc_t *desc)
{
    if (fdp == NULL || desc == NULL || desc->kind == LPRS_V2_FD_KIND_NONE) {
        return LPRS_ERR_INVAL;
    }
    const int slot_status = lprs_filedesc_ensure_slots(fdp, fdp->slot_count + 1u);
    if (slot_status != 0) {
        return slot_status;
    }
    const int file_status = lprs_filedesc_ensure_files(fdp, fdp->file_count + 1u);
    if (file_status != 0) {
        return file_status;
    }

    if (fdp->file_count > UINT32_MAX) {
        return LPRS_ERR_NOMEM;
    }
    const uint64_t file_index = fdp->file_count++;
    lprs_file_t *file = &fdp->files[file_index];
    memset(file, 0, sizeof(*file));
    file->active = 1;
    file->refcount = 1;
    file->kind = desc->kind;
    file->status_flags = desc->status_flags;
    file->handle = desc->handle;
    file->offset_or_counter = desc->offset_or_counter;

    lprs_fd_slot_t *slot = &fdp->slots[fdp->slot_count++];
    memset(slot, 0, sizeof(*slot));
    slot->active = 1;
    slot->file_index = (uint32_t)file_index;
    slot->fd = desc->fd;
    slot->fd_flags = desc->fd_flags;
    return 0;
}

static int lprs_filedesc_read_desc(const lprs_filedesc_t *fdp, uint64_t index, lprs_v2_fd_desc_t *out)
{
    if (fdp == NULL || out == NULL || index >= fdp->slot_count) {
        return LPRS_ERR_INVAL;
    }
    const lprs_fd_slot_t *slot = &fdp->slots[index];
    if (!slot->active || slot->file_index >= fdp->file_count) {
        return LPRS_ERR_INVAL;
    }
    const lprs_file_t *file = &fdp->files[slot->file_index];
    if (!file->active) {
        return LPRS_ERR_INVAL;
    }
    memset(out, 0, sizeof(*out));
    out->fd = slot->fd;
    out->kind = file->kind;
    out->fd_flags = slot->fd_flags;
    out->status_flags = file->status_flags;
    out->handle = file->handle;
    out->offset_or_counter = file->offset_or_counter;
    return 0;
}

static void lprs_process_release_owned(lprs_process_t *proc)
{
    if (proc == NULL) {
        return;
    }
    if (proc->process_fd >= 16) {
        (void)pacha_fd_close(proc->process_fd);
    }
    lprs_filedesc_release(&proc->filedesc);
    lprs_filedesc_release(&proc->staged_filedesc);
    memset(proc, 0, sizeof(*proc));
    proc->process_fd = -1;
}

static void lprs_process_activate_empty(lprs_process_t *proc)
{
    lprs_process_release_owned(proc);
    proc->active = 1;
    proc->process_fd = -1;
}

static void lprs_process_reap(lprs_process_t *proc)
{
    lprs_process_release_owned(proc);
    proc->waited = 1;
}

static lprs_process_t *lprs_find_by_token(uint64_t token)
{
    if (token == 0) {
        return NULL;
    }
    for (uint64_t i = 0; i < g_process_count; ++i) {
        if (g_processes[i].active && g_processes[i].token == token) {
            return &g_processes[i];
        }
    }
    return NULL;
}

static lprs_process_t *lprs_find_by_pid(uint64_t pid)
{
    if (pid == 0) {
        return NULL;
    }
    for (uint64_t i = 0; i < g_process_count; ++i) {
        if (g_processes[i].active && g_processes[i].pid == pid) {
            return &g_processes[i];
        }
    }
    return NULL;
}

static lprs_process_t *lprs_alloc_process(void)
{
    for (uint64_t i = 0; i < g_process_count; ++i) {
        if (!g_processes[i].active) {
            lprs_process_t *proc = &g_processes[i];
            lprs_process_activate_empty(proc);
            return proc;
        }
    }
    if (lprs_ensure_process_capacity(g_process_count + 1u) != 0) {
        return NULL;
    }
    lprs_process_t *proc = &g_processes[g_process_count++];
    lprs_process_activate_empty(proc);
    return proc;
}

static void lprs_copy_string(char *dst, uint64_t dst_bytes, const char *src)
{
    if (dst == NULL || dst_bytes == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, (size_t)dst_bytes, "%s", src);
}

static void lprs_write_state(const lprs_process_t *proc, lprs_v2_process_state_t *out)
{
    memset(out, 0, sizeof(*out));
    out->token = proc->token;
    out->pid = proc->pid;
    out->ppid = proc->ppid;
    out->sid = proc->sid;
    out->pgrp = proc->pgrp;
    out->foreground_pgrp = proc->foreground_pgrp;
    out->cwd_handle = proc->cwd_handle;
    lprs_copy_string(out->ctty, sizeof(out->ctty), proc->ctty);
    lprs_copy_string(out->cwd, sizeof(out->cwd), proc->cwd);
}

static int lprs_register_exec(void *page, uint64_t *out_token)
{
    if (page == NULL || out_token == NULL) {
        return LPRS_ERR_INVAL;
    }
    lprs_v2_register_exec_t *req = (lprs_v2_register_exec_t *)page;
    lprs_process_t *proc = lprs_alloc_process();
    if (proc == NULL) {
        return LPRS_ERR_NOMEM;
    }
    proc->token = g_next_token++;
    proc->pid = g_next_pid++;
    proc->ppid = req->state.ppid;
    proc->sid = proc->pid;
    proc->pgrp = proc->pid;
    proc->foreground_pgrp = proc->pgrp;
    proc->cwd_handle = req->state.cwd_handle;
    if (req->state.sid != 0) {
        proc->sid = req->state.sid;
    }
    if (req->state.pgrp != 0) {
        proc->pgrp = req->state.pgrp;
    }
    if (req->state.foreground_pgrp != 0) {
        proc->foreground_pgrp = req->state.foreground_pgrp;
    }
    lprs_copy_string(proc->ctty, sizeof(proc->ctty), req->state.ctty);
    lprs_copy_string(proc->cwd, sizeof(proc->cwd), req->state.cwd[0] != '\0' ? req->state.cwd : "/");
    lprs_write_state(proc, &req->state);
    *out_token = proc->token;
    return 0;
}

static int lprs_register_process_fd_handle(uint64_t token, int process_fd)
{
    if (process_fd < 16) {
        return LPRS_ERR_INVAL;
    }
    lprs_process_t *proc = lprs_find_by_token(token);
    if (proc == NULL) {
        return LPRS_ERR_SRCH;
    }
    if (proc->process_fd >= 16) {
        (void)pacha_fd_close(proc->process_fd);
    }
    proc->process_fd = process_fd;
    return 0;
}

static int lprs_get_process_state(uint64_t token, void *page)
{
    if (page == NULL) {
        return LPRS_ERR_INVAL;
    }
    lprs_process_t *proc = lprs_find_by_token(token);
    if (proc == NULL) {
        return LPRS_ERR_SRCH;
    }
    lprs_write_state(proc, (lprs_v2_process_state_t *)page);
    return 0;
}

static int lprs_fork_begin(uint64_t parent_token, void *page)
{
    if (page == NULL) {
        return LPRS_ERR_INVAL;
    }
    lprs_process_t *parent = lprs_find_by_token(parent_token);
    if (parent == NULL) {
        return LPRS_ERR_SRCH;
    }
    const uint64_t parent_pid = parent->pid;
    const uint64_t parent_sid = parent->sid;
    const uint64_t parent_pgrp = parent->pgrp;
    const uint64_t parent_foreground_pgrp = parent->foreground_pgrp;
    const uint64_t parent_cwd_handle = parent->cwd_handle;
    char parent_ctty[LPRS_V2_CTTY_BYTES];
    char parent_cwd[LPRS_V2_CWD_BYTES];
    lprs_copy_string(parent_ctty, sizeof(parent_ctty), parent->ctty);
    lprs_copy_string(parent_cwd, sizeof(parent_cwd), parent->cwd);

    lprs_process_t *child = lprs_alloc_process();
    if (child == NULL) {
        return LPRS_ERR_NOMEM;
    }
    child->token = g_next_token++;
    child->pid = g_next_pid++;
    child->ppid = parent_pid;
    child->sid = parent_sid;
    child->pgrp = parent_pgrp;
    child->foreground_pgrp = parent_foreground_pgrp;
    child->cwd_handle = parent_cwd_handle;
    lprs_copy_string(child->ctty, sizeof(child->ctty), parent_ctty);
    lprs_copy_string(child->cwd, sizeof(child->cwd), parent_cwd);

    lprs_v2_fork_t *fork = (lprs_v2_fork_t *)page;
    memset(fork, 0, sizeof(*fork));
    fork->parent_token = parent_token;
    fork->child_token = child->token;
    fork->child_pid = child->pid;
    fork->child_ppid = child->ppid;
    fork->child_sid = child->sid;
    fork->child_pgrp = child->pgrp;
    return 0;
}

static int lprs_signal_process_fd(int process_fd, uint64_t signal)
{
    if (process_fd < 16) {
        return LPRS_ERR_SRCH;
    }
    if (signal == 0) {
        return 0;
    }
    return lprs_status_to_errno(pacha_syscall2(
        PACHA_PROCESS_SYSCALL_SIGNAL,
        (uint64_t)(uint32_t)process_fd,
        signal));
}

static int lprs_wait_process_fd(int process_fd, uint64_t options, uint64_t *out_exit_code)
{
    if (process_fd < 16 || out_exit_code == NULL) {
        return LPRS_ERR_INVAL;
    }
    lprs_process_status_t status;
    memset(&status, 0, sizeof(status));
    for (;;) {
        const long wait_status = pacha_syscall2(
            PACHA_PROCESS_SYSCALL_WAIT,
            (uint64_t)(uint32_t)process_fd,
            (uint64_t)(uintptr_t)&status);
        if (wait_status == 0) {
            *out_exit_code = status.exit_code & 0xffu;
            return 0;
        }
        const int err = lprs_status_to_errno(wait_status);
        if (err != LPRS_ERR_AGAIN) {
            return err;
        }
        if ((options & LPRS_WNOHANG) != 0) {
            return LPRS_ERR_AGAIN;
        }
        struct pacha_pollfd pollfd[2];
        memset(pollfd, 0, sizeof(pollfd));
        pollfd[0].fd = process_fd;
        pollfd[0].events = PACHA_FD_EVENT_READABLE;
        pollfd[1].fd = g_endpoint_fd;
        pollfd[1].events = PACHA_FD_EVENT_READABLE;
        const long wait_many = pacha_fd_wait_many(pollfd, 2, UINT64_MAX);
        if (wait_many < 0) {
            const int wait_many_err = lprs_status_to_errno(wait_many);
            if (wait_many_err != LPRS_ERR_AGAIN) {
                return wait_many_err;
            }
        }
        if ((pollfd[1].revents & PACHA_FD_EVENT_READABLE) != 0) {
            while (lprs_service_one_pending_request() == 0) {
            }
        }
    }
}

static int lprs_child_matches(const lprs_process_t *parent, const lprs_process_t *child, int64_t requested)
{
    if (parent == NULL || child == NULL || !child->active || child->waited || child->ppid != parent->pid) {
        return 0;
    }
    if (requested == -1) {
        return 1;
    }
    if (requested > 0) {
        return child->pid == (uint64_t)requested;
    }
    if (requested == 0) {
        return child->pgrp == parent->pgrp;
    }
    return child->pgrp == (uint64_t)(-requested);
}

static int lprs_wait4(void *page, uint64_t *out_result)
{
    if (page == NULL || out_result == NULL) {
        return LPRS_ERR_INVAL;
    }
    *out_result = 0;
    lprs_v2_wait4_t *req = (lprs_v2_wait4_t *)page;
    lprs_process_t *parent = lprs_find_by_token(req->token);
    if (parent == NULL) {
        return LPRS_ERR_SRCH;
    }
    if ((req->options & ~((uint64_t)LPRS_WNOHANG)) != 0) {
        return LPRS_ERR_INVAL;
    }
    uint64_t selected_index = UINT64_MAX;
    for (uint64_t i = 0; i < g_process_count; ++i) {
        if (lprs_child_matches(parent, &g_processes[i], req->requested_pid)) {
            selected_index = i;
            break;
        }
    }
    if (selected_index == UINT64_MAX) {
        return LPRS_ERR_CHILD;
    }
    lprs_process_t *selected = &g_processes[selected_index];
    const uint64_t selected_pid = selected->pid;
    const int selected_process_fd = selected->process_fd;
    uint64_t exit_code = 0;
    const int wait_status = lprs_wait_process_fd(selected_process_fd, req->options, &exit_code);
    if (wait_status == LPRS_ERR_AGAIN && (req->options & LPRS_WNOHANG) != 0) {
        req->result_pid = 0;
        req->status = 0;
        req->exit_code = 0;
        *out_result = 0;
        return 0;
    }
    if (wait_status != 0) {
        return wait_status;
    }
    if (selected_index >= g_process_count) {
        return LPRS_ERR_CHILD;
    }
    selected = &g_processes[selected_index];
    if (!selected->active || selected->pid != selected_pid || selected->process_fd != selected_process_fd) {
        return LPRS_ERR_CHILD;
    }
    req->result_pid = (int64_t)selected_pid;
    req->exit_code = exit_code;
    req->status = (exit_code & 0xffu) << 8;
    *out_result = LPRS_V2_WAIT4_RESULT_PACK(selected_pid, req->status);
    lprs_process_reap(selected);
    return 0;
}

static int lprs_setpgid(void *page)
{
    if (page == NULL) {
        return LPRS_ERR_INVAL;
    }
    lprs_v2_pid_op_t *req = (lprs_v2_pid_op_t *)page;
    lprs_process_t *caller = lprs_find_by_token(req->token);
    if (caller == NULL) {
        return LPRS_ERR_SRCH;
    }
    if (req->pid < 0 || req->value < 0) {
        return LPRS_ERR_INVAL;
    }
    const uint64_t target_pid = req->pid == 0 ? caller->pid : (uint64_t)req->pid;
    const uint64_t target_pgrp = req->value == 0 ? target_pid : (uint64_t)req->value;
    lprs_process_t *target = lprs_find_by_pid(target_pid);
    if (target == NULL) {
        return LPRS_ERR_SRCH;
    }
    if (target != caller && target->ppid != caller->pid) {
        return LPRS_ERR_PERM;
    }
    target->pgrp = target_pgrp;
    req->result = target->pgrp;
    return 0;
}

static int lprs_setsid(void *page)
{
    if (page == NULL) {
        return LPRS_ERR_INVAL;
    }
    lprs_v2_pid_op_t *req = (lprs_v2_pid_op_t *)page;
    lprs_process_t *caller = lprs_find_by_token(req->token);
    if (caller == NULL) {
        return LPRS_ERR_SRCH;
    }
    if (caller->pgrp == caller->pid) {
        return LPRS_ERR_PERM;
    }
    caller->sid = caller->pid;
    caller->pgrp = caller->pid;
    caller->foreground_pgrp = caller->pgrp;
    req->result = caller->sid;
    return 0;
}

static int lprs_getpgid_or_sid(void *page, int want_sid)
{
    if (page == NULL) {
        return LPRS_ERR_INVAL;
    }
    lprs_v2_pid_op_t *req = (lprs_v2_pid_op_t *)page;
    lprs_process_t *caller = lprs_find_by_token(req->token);
    if (caller == NULL || req->pid < 0) {
        return caller == NULL ? LPRS_ERR_SRCH : LPRS_ERR_INVAL;
    }
    const uint64_t target_pid = req->pid == 0 ? caller->pid : (uint64_t)req->pid;
    lprs_process_t *target = lprs_find_by_pid(target_pid);
    if (target == NULL) {
        return LPRS_ERR_SRCH;
    }
    req->result = want_sid ? target->sid : target->pgrp;
    return 0;
}

static int lprs_kill(void *page)
{
    if (page == NULL) {
        return LPRS_ERR_INVAL;
    }
    lprs_v2_kill_t *req = (lprs_v2_kill_t *)page;
    lprs_process_t *caller = lprs_find_by_token(req->token);
    if (caller == NULL || req->signal > 64u) {
        return caller == NULL ? LPRS_ERR_SRCH : LPRS_ERR_INVAL;
    }
    uint64_t delivered = 0;
    int first_error = 0;
    for (uint64_t i = 0; i < g_process_count; ++i) {
        lprs_process_t *proc = &g_processes[i];
        if (!proc->active || proc->process_fd < 16) {
            continue;
        }
        int match = 0;
        if (req->pid == -1) {
            match = 1;
        } else if (req->pid == 0) {
            match = proc->pgrp == caller->pgrp;
        } else if (req->pid < -1) {
            match = proc->pgrp == (uint64_t)(-req->pid);
        } else {
            match = proc->pid == (uint64_t)req->pid;
        }
        if (!match) {
            continue;
        }
        const int status = lprs_signal_process_fd(proc->process_fd, req->signal);
        if (status == 0) {
            delivered++;
        } else if (first_error == 0) {
            first_error = status;
        }
    }
    req->delivered = delivered;
    if (delivered != 0) {
        return 0;
    }
    return first_error != 0 ? first_error : LPRS_ERR_SRCH;
}

static int lprs_deliver_tty_signal_fields(uint64_t pgrp, uint64_t signo, uint64_t *out_delivered)
{
    if (pgrp == 0 || signo == 0 || signo > 64u) {
        return LPRS_ERR_INVAL;
    }

    uint64_t delivered = 0;
    int first_error = 0;
    for (uint64_t i = 0; i < g_process_count; ++i) {
        lprs_process_t *proc = &g_processes[i];
        if (!proc->active || proc->process_fd < 16 || proc->pgrp != (uint64_t)pgrp) {
            continue;
        }
        const int status = lprs_signal_process_fd(proc->process_fd, signo);
        if (status == 0) {
            delivered++;
        } else if (first_error == 0) {
            first_error = status;
        }
    }
    if (out_delivered != NULL) {
        *out_delivered = delivered;
    }
    return delivered != 0 ? 0 : (first_error != 0 ? first_error : LPRS_ERR_SRCH);
}

static int lprs_cwd_get(uint64_t token, void *page)
{
    if (page == NULL) {
        return LPRS_ERR_INVAL;
    }
    lprs_process_t *proc = lprs_find_by_token(token);
    if (proc == NULL) {
        return LPRS_ERR_SRCH;
    }
    lprs_v2_cwd_t *cwd = (lprs_v2_cwd_t *)page;
    memset(cwd, 0, sizeof(*cwd));
    cwd->token = proc->token;
    cwd->cwd_handle = proc->cwd_handle;
    lprs_copy_string(cwd->cwd, sizeof(cwd->cwd), proc->cwd);
    return 0;
}

static int lprs_cwd_set(void *page)
{
    if (page == NULL) {
        return LPRS_ERR_INVAL;
    }
    lprs_v2_cwd_t *cwd = (lprs_v2_cwd_t *)page;
    lprs_process_t *proc = lprs_find_by_token(cwd->token);
    if (proc == NULL) {
        return LPRS_ERR_SRCH;
    }
    proc->cwd_handle = cwd->cwd_handle;
    lprs_copy_string(proc->cwd, sizeof(proc->cwd), cwd->cwd);
    return 0;
}

static int lprs_fd_replace_begin(uint64_t token)
{
    lprs_process_t *proc = lprs_find_by_token(token);
    if (proc == NULL) {
        return LPRS_ERR_SRCH;
    }
    lprs_filedesc_release(&proc->staged_filedesc);
    proc->fd_replace_active = 1;
    return 0;
}

static int lprs_fd_replace_chunk(void *page)
{
    if (page == NULL) {
        return LPRS_ERR_INVAL;
    }
    lprs_v2_fd_table_page_t *req = (lprs_v2_fd_table_page_t *)page;
    if (req->count > LPRS_V2_FD_TABLE_PAGE_MAX) {
        return LPRS_ERR_INVAL;
    }
    lprs_process_t *proc = lprs_find_by_token(req->token);
    if (proc == NULL || !proc->fd_replace_active) {
        return proc == NULL ? LPRS_ERR_SRCH : LPRS_ERR_INVAL;
    }
    for (uint64_t i = 0; i < req->count; ++i) {
        const int status = lprs_filedesc_append_desc(&proc->staged_filedesc, &req->entries[i]);
        if (status != 0) {
            return status;
        }
    }
    return 0;
}

static int lprs_fd_replace_commit(uint64_t token)
{
    lprs_process_t *proc = lprs_find_by_token(token);
    if (proc == NULL || !proc->fd_replace_active) {
        return proc == NULL ? LPRS_ERR_SRCH : LPRS_ERR_INVAL;
    }
    lprs_filedesc_swap(&proc->filedesc, &proc->staged_filedesc);
    lprs_filedesc_release(&proc->staged_filedesc);
    proc->fd_replace_active = 0;
    return 0;
}

static int lprs_fd_get_chunk(void *page)
{
    if (page == NULL) {
        return LPRS_ERR_INVAL;
    }
    lprs_v2_fd_table_page_t *req = (lprs_v2_fd_table_page_t *)page;
    lprs_process_t *proc = lprs_find_by_token(req->token);
    if (proc == NULL) {
        return LPRS_ERR_SRCH;
    }
    const uint64_t start = req->start_index;
    const uint64_t max_count = LPRS_V2_FD_TABLE_PAGE_MAX;
    uint64_t count = 0;
    if (start < proc->filedesc.slot_count) {
        count = proc->filedesc.slot_count - start;
        if (count > max_count) {
            count = max_count;
        }
        for (uint64_t i = 0; i < count; ++i) {
            const int status =
                lprs_filedesc_read_desc(&proc->filedesc, start + i, &req->entries[i]);
            if (status != 0) {
                return status;
            }
        }
    }
    req->total_count = proc->filedesc.slot_count;
    req->count = count;
    return 0;
}

static void *lprs_map_request_page(const struct pacha_ipc_msg *request, int *out_page_fd)
{
    if (out_page_fd != NULL) {
        *out_page_fd = -1;
    }
    if (request == NULL ||
        request->fds == NULL ||
        request->fd_count < 1 ||
        request->fds[0].fd < 16)
    {
        return NULL;
    }
    const int page_fd = (int)(uint32_t)request->fds[0].fd;
    void *page = pacha_mmap(
        page_fd,
        PACHA_SERVICE_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (page == NULL) {
        return NULL;
    }
    if (out_page_fd != NULL) {
        *out_page_fd = page_fd;
    }
    return page;
}

static void lprs_close_unowned_fds(const struct pacha_ipc_msg *request, int keep_fd, int reply_fd)
{
    if (request == NULL || request->fds == NULL) {
        return;
    }
    for (uint64_t i = 0; i < request->fd_count; ++i) {
        const int fd = (int)(uint32_t)request->fds[i].fd;
        if (fd >= 16 && fd != keep_fd && fd != reply_fd) {
            (void)pacha_fd_close(fd);
        }
    }
}

static uint64_t lprs_v2_token_from_payload(const void *payload, uint32_t payload_size)
{
    if (payload == NULL || payload_size < sizeof(lprs_v2_token_request_t)) {
        return 0;
    }
    return ((const lprs_v2_token_request_t *)payload)->token;
}

static int lprs_dispatch_v2(
    struct pacha_ipc_msg *request,
    uint64_t *out_result,
    int *out_keep_fd,
    uint64_t *out_error_token,
    uint64_t *out_request_id)
{
    if (request == NULL || out_result == NULL || out_keep_fd == NULL) {
        return LPRS_ERR_INVAL;
    }
    *out_result = 0;
    *out_keep_fd = -1;
    if (out_error_token != NULL) {
        *out_error_token = 0;
    }
    if (out_request_id != NULL) {
        *out_request_id = request->word3;
    }

    int page_fd = -1;
    void *page = lprs_map_request_page(request, &page_fd);
    if (page == NULL) {
        if (out_error_token != NULL) {
            *out_error_token = lprs_error_token(
                LPRS_ERR_FAULT,
                0,
                PACHA_ERRCONV_STAGE_MAP_PAGE,
                LPRS_ERR_FAULT,
                request->word3,
                request->fd_count,
                request->fd_count != 0 ? request->fds[0].fd : 0,
                0,
                "lpr supervisor v2 request page map failed");
        }
        return LPRS_ERR_FAULT;
    }

    pacha_service_request_header_t header;
    memcpy(&header, page, sizeof(header));
    if (out_request_id != NULL) {
        *out_request_id = header.request_id;
    }
    int status = LPRS_ERR_INVAL;
    uint32_t reply_payload_size = 0;
    if (!pacha_service_request_header_is_v2(&header, LPRS_V2_SERVICE_ID)) {
        pacha_service_reply_header_init(
            (pacha_service_reply_header_t *)page,
            &header,
            LPRS_ERR_INVAL,
            PACHA_SERVICE_ERROR_ABI,
            0,
            0);
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
        return LPRS_ERR_INVAL;
    }

    void *payload = (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES;
    const uint64_t token = lprs_v2_token_from_payload(payload, header.payload_size);
    switch (header.op) {
    case LPRS_V2_OP_HELLO:
        *out_result = PACHA_SERVICE_ABI_VERSION;
        status = 0;
        break;
    case LPRS_V2_OP_PROCESS_REGISTER_EXEC:
        if (header.payload_size < sizeof(lprs_v2_register_exec_t)) {
            status = LPRS_ERR_INVAL;
        } else {
            status = lprs_register_exec(payload, out_result);
            reply_payload_size = sizeof(lprs_v2_register_exec_t);
        }
        break;
    case LPRS_V2_OP_PROCESS_REGISTER_FD:
    case LPRS_V2_OP_PROCESS_FORK_PARENT_REGISTER:
        if (token == 0 || request->fds == NULL || request->fd_count < 2 ||
            request->fds[1].fd < 16)
        {
            status = LPRS_ERR_INVAL;
        } else {
            status =
                lprs_register_process_fd_handle(token, (int)(uint32_t)request->fds[1].fd);
            if (status == 0) {
                *out_keep_fd = (int)(uint32_t)request->fds[1].fd;
            }
        }
        break;
    case LPRS_V2_OP_PROCESS_GET_STATE:
        status = token == 0 ? LPRS_ERR_INVAL : lprs_get_process_state(token, payload);
        reply_payload_size = status == 0 ? sizeof(lprs_v2_process_state_t) : 0;
        break;
    case LPRS_V2_OP_PROCESS_FORK_BEGIN:
        status = token == 0 ? LPRS_ERR_INVAL : lprs_fork_begin(token, payload);
        if (status == 0) {
            *out_result = ((lprs_v2_fork_t *)payload)->child_token;
            reply_payload_size = sizeof(lprs_v2_fork_t);
        }
        break;
    case LPRS_V2_OP_PROCESS_FORK_CHILD_READY:
    case LPRS_V2_OP_PROCESS_EXEC_COMMIT_BEGIN:
    case LPRS_V2_OP_PROCESS_EXEC_COMMIT_DONE:
        status = token == 0 ? LPRS_ERR_INVAL :
            (lprs_find_by_token(token) != NULL ? 0 : LPRS_ERR_SRCH);
        break;
    case LPRS_V2_OP_PROCESS_WAIT4:
        if (header.payload_size < sizeof(lprs_v2_wait4_t)) {
            status = LPRS_ERR_INVAL;
        } else {
            status = lprs_wait4(payload, out_result);
            reply_payload_size = sizeof(lprs_v2_wait4_t);
        }
        break;
    case LPRS_V2_OP_PROCESS_SETPGID:
        status = header.payload_size < sizeof(lprs_v2_pid_op_t) ?
            LPRS_ERR_INVAL :
            lprs_setpgid(payload);
        reply_payload_size = status == 0 ? sizeof(lprs_v2_pid_op_t) : 0;
        break;
    case LPRS_V2_OP_PROCESS_SETSID:
        status = header.payload_size < sizeof(lprs_v2_pid_op_t) ?
            LPRS_ERR_INVAL :
            lprs_setsid(payload);
        reply_payload_size = status == 0 ? sizeof(lprs_v2_pid_op_t) : 0;
        break;
    case LPRS_V2_OP_PROCESS_GETPGID:
        status = header.payload_size < sizeof(lprs_v2_pid_op_t) ?
            LPRS_ERR_INVAL :
            lprs_getpgid_or_sid(payload, 0);
        reply_payload_size = status == 0 ? sizeof(lprs_v2_pid_op_t) : 0;
        break;
    case LPRS_V2_OP_PROCESS_GETSID:
        status = header.payload_size < sizeof(lprs_v2_pid_op_t) ?
            LPRS_ERR_INVAL :
            lprs_getpgid_or_sid(payload, 1);
        reply_payload_size = status == 0 ? sizeof(lprs_v2_pid_op_t) : 0;
        break;
    case LPRS_V2_OP_SIGNAL_KILL:
        status = header.payload_size < sizeof(lprs_v2_kill_t) ?
            LPRS_ERR_INVAL :
            lprs_kill(payload);
        reply_payload_size = status == 0 ? sizeof(lprs_v2_kill_t) : 0;
        break;
    case LPRS_V2_OP_SIGNAL_DELIVER_TTY:
        if (header.payload_size < sizeof(lprs_v2_tty_signal_t)) {
            status = LPRS_ERR_INVAL;
        } else {
            lprs_v2_tty_signal_t *sig = (lprs_v2_tty_signal_t *)payload;
            status = lprs_deliver_tty_signal_fields(sig->pgrp, sig->signal, &sig->delivered);
            reply_payload_size = sizeof(*sig);
        }
        break;
    case LPRS_V2_OP_FD_TABLE_REPLACE_BEGIN:
        status = token == 0 ? LPRS_ERR_INVAL : lprs_fd_replace_begin(token);
        break;
    case LPRS_V2_OP_FD_TABLE_REPLACE_CHUNK:
        status = header.payload_size < sizeof(lprs_v2_fd_table_page_t) ?
            LPRS_ERR_INVAL :
            lprs_fd_replace_chunk(payload);
        break;
    case LPRS_V2_OP_FD_TABLE_REPLACE_COMMIT:
        status = token == 0 ? LPRS_ERR_INVAL : lprs_fd_replace_commit(token);
        break;
    case LPRS_V2_OP_FD_TABLE_GET_CHUNK:
        status = header.payload_size < sizeof(lprs_v2_fd_table_page_t) ?
            LPRS_ERR_INVAL :
            lprs_fd_get_chunk(payload);
        reply_payload_size = status == 0 ?
            (uint32_t)(sizeof(lprs_v2_fd_table_page_t) +
                ((lprs_v2_fd_table_page_t *)payload)->count * sizeof(lprs_v2_fd_desc_t)) :
            0;
        break;
    case LPRS_V2_OP_CWD_GET:
        status = token == 0 ? LPRS_ERR_INVAL : lprs_cwd_get(token, payload);
        reply_payload_size = status == 0 ? sizeof(lprs_v2_cwd_t) : 0;
        break;
    case LPRS_V2_OP_CWD_SET:
        status = header.payload_size < sizeof(lprs_v2_cwd_t) ? LPRS_ERR_INVAL : lprs_cwd_set(payload);
        break;
    case LPRS_V2_OP_DIAG_ERROR_GET:
        status = token == 0 ?
            LPRS_ERR_INVAL :
            pacha_errconv_export(lprs_errors(), token, payload, LPRS_V2_PAYLOAD_BYTES);
        reply_payload_size = status == 0 ? LPRS_V2_PAYLOAD_BYTES : 0;
        break;
    case LPRS_V2_OP_DIAG_DUMP:
    default:
        status = LPRS_ERR_INVAL;
        break;
    }

    if (status < 0 && header.op != LPRS_V2_OP_DIAG_ERROR_GET &&
        out_error_token != NULL && *out_error_token == 0)
    {
        *out_error_token = lprs_error_token(
            status,
            header.op,
            PACHA_ERRCONV_STAGE_DISPATCH_ENTRY,
            status,
            header.request_id,
            request->fd_count,
            token,
            0,
            "lpr supervisor v2 dispatch failed");
    }
    pacha_service_reply_header_init(
        (pacha_service_reply_header_t *)page,
        &header,
        status,
        status == LPRS_ERR_INVAL ? PACHA_SERVICE_ERROR_ABI : PACHA_SERVICE_ERROR_LPR_TRANSLATION,
        *out_result,
        reply_payload_size);
    (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
    (void)page_fd;
    return status;
}

static int lprs_reply_v2(
    int reply_fd,
    uint64_t request_id,
    int64_t status,
    uint64_t result,
    uint64_t error_token)
{
    const struct pacha_ipc_msg reply = {
        .word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = status < 0 ? error_token : result,
        .word3 = request_id,
    };
    const int reply_status = pacha_ipc_reply(reply_fd, &reply);
    (void)pacha_fd_close(reply_fd);
    return reply_status;
}

static int lprs_handle_received_request(struct pacha_ipc_msg *request)
{
    if (request == NULL) {
        return LPRS_ERR_INVAL;
    }
    const int reply_fd =
        request->fd_count != 0 ? (int)(uint32_t)request->fds[request->fd_count - 1u].fd : -1;
    if (reply_fd < 16) {
        lprs_close_unowned_fds(request, -1, -1);
        return LPRS_ERR_INVAL;
    }
    int keep_fd = -1;
    uint64_t result = 0;
    uint64_t error_token = 0;
    uint64_t request_id = request->word3;
    const int dispatch_status =
        request->word0 == PACHA_SERVICE_REQUEST_MAGIC ?
            lprs_dispatch_v2(request, &result, &keep_fd, &error_token, &request_id) :
            LPRS_ERR_INVAL;
    lprs_close_unowned_fds(request, keep_fd, reply_fd);
    (void)lprs_reply_v2(reply_fd, request_id, dispatch_status, result, error_token);
    return 0;
}

static int lprs_service_one_pending_request(void)
{
    struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS];
    struct pacha_ipc_msg request;
    memset(fds, 0, sizeof(fds));
    memset(&request, 0, sizeof(request));
    request.fds = fds;
    request.fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS;
    const int status = pacha_ipc_recv(g_endpoint_fd, &request);
    if (status != 0) {
        return status == PACHA_ERR_EMPTY || status == PACHA_ERR_NOT_READY ? LPRS_ERR_AGAIN : status;
    }
    return lprs_handle_received_request(&request);
}

int main(int argc, char **argv)
{
    (void)argc;
    int bootstrap_fd = -1;
    int status = lprs_find_bootstrap_fd(argc, argv, &bootstrap_fd);
    if (status != 0) {
        pacha_trace1(PACHA_TRACE_COMPONENT_LPR_SUPERVISOR, PACHA_TRACE_EVENT_LPRS_BOOTSTRAP, PACHA_TRACE_CLASS_ERROR, (uint64_t)status);
        return 1;
    }
    struct lprs_boot_config cfg;
    status = lprs_read_bootstrap(bootstrap_fd, &cfg);
    if (status != 0) {
        pacha_trace1(PACHA_TRACE_COMPONENT_LPR_SUPERVISOR, PACHA_TRACE_EVENT_LPRS_BOOTSTRAP, PACHA_TRACE_CLASS_ERROR, (uint64_t)status);
        return 1;
    }
    g_endpoint_fd = (int)(uint32_t)cfg.endpoint_fd;

    for (;;) {
        struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS];
        struct pacha_ipc_msg request;
        memset(fds, 0, sizeof(fds));
        memset(&request, 0, sizeof(request));
        request.fds = fds;
        request.fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS;
        status = pacha_ipc_recv_wait(g_endpoint_fd, &request, UINT64_MAX);
        if (status != 0) {
            continue;
        }
        (void)lprs_handle_received_request(&request);
    }
}
