#include "lpr_supervisor/boot_config.h"
#include "lpr_supervisor/ipc_protocol.h"
#include "unixd/client.h"
#include "filed/identity.h"
#include "filed/ipc_protocol.h"
#include "accounts.h"
#include "credentials.h"

#include <pacha/abi.h>
#include <pacha/ipc.h>
#include <pacha/status.h>
#include <pacha/syscall.h>
#include <pacha/trace.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LPRS_SIGNAL_DIAG
#define LPRS_SIGNAL_DIAG 0
#endif

enum {
    LPRS_WNOHANG = 1,
    LPRS_SIGCHLD = 17,
    LPRS_SIGCONT = 18,
    LPRS_SIGSTOP = 19,
    LPRS_NATIVE_PROCESS_EXITED = 2,
    LPRS_NATIVE_PROCESS_KILLED = 3,
    LPRS_WAIT_FD_CAPACITY = 256,
    LPRS_DISPATCH_DEFERRED = 1,
};

typedef struct lprs_process {
    uint8_t active;
    uint8_t waited;
    uint8_t exit_notified;
    uint8_t exit_ready;
    uint32_t exit_status;
    uint32_t exit_state;
    uint64_t token;
    uint64_t generation;
    uint64_t child_sequence;
    uint64_t pid;
    uint64_t ppid;
    uint64_t sid;
    uint64_t pgrp;
    uint64_t foreground_pgrp;
    uint64_t cwd_handle;
    uint32_t pdeath_signal;
    int process_fd;
    int pending_exec_fd;
    int control_fd;
    int bootstrap_server_fd;
    int bootstrap_client_fd;
    int activation_page_fd;
    int activation_reply_fd;
    int unix_fd;
    int filed_fd;
    uint64_t filed_session;
    uint64_t unix_session;
    struct unix_credentials credentials;
    uint32_t group_count;
    uint32_t groups[LPRS_MAX_GROUPS];
    uint32_t filed_rights;
    uint32_t credential_rights;
    pacha_service_envelope_t activation_header;
    char ctty[LPRS_CTTY_BYTES];
    char cwd[LPRS_CWD_BYTES];
    char comm[LPRS_PROCESS_COMM_BYTES];
    char cmdline[LPRS_PROCESS_CMDLINE_BYTES];
    const lprs_diag_slot_t *diag_page;
    int diag_fd;
} lprs_process_t;

typedef struct lprs_process_status {
    uint64_t state;
    uint64_t exit_code;
    uint64_t id;
    uint64_t generation;
} lprs_process_status_t;

typedef struct lprs_waiter {
    uint8_t active;
    int page_fd;
    int reply_fd;
    pacha_service_envelope_t header;
    lprs_wait4_t request;
} lprs_waiter_t;

typedef struct lprs_reply_cap {
    int fd;
    uint64_t rights;
    uint64_t transfer_flags;
} lprs_reply_cap_t;

#define LPRS_CHANNEL_RIGHTS (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | \
    PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SET_FLAGS | \
    PACHA_FD_RIGHT_CALL | PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_SEND | \
    PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL)
/* A bounded process request queue needs writable observation for backpressure.
 * These rights do not grant receive, duplication or transfer authority. */
#define LPRS_CLIENT_RIGHTS (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_CALL | \
    PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL)
#define LPRS_UNIX_CLIENT_RIGHTS LPRS_CLIENT_RIGHTS

static int g_endpoint_fd = -1;
static int g_unix_admin_fd = -1;
static int g_filed_admin_fd = -1;
static struct pacha_accounts g_accounts;
static int g_accounts_ready;

static int lprs_resolve_account(const struct pacha_accounts *db, const char *name,
    lprs_credentials_t *out)
{
    const struct pacha_account *account = pacha_account_by_name(db, name);
    if (!account) return -2;
    lprs_credentials_t identity = { .uid = account->uid, .euid = account->uid,
        .suid = account->uid, .gid = account->gid, .egid = account->gid, .sgid = account->gid };
    size_t count = LPRS_MAX_GROUPS;
    int status = pacha_account_groups(db, name, identity.groups, &count);
    if (status) return -status;
    identity.group_count = (uint32_t)count;
    *out = identity;
    return 0;
}
static uint64_t g_unix_request;
/* Shared diagnostic page.  Processes write their own slot without a system
 * call; the supervisor only ever reads, and maps it once for its own lifetime
 * so answering a query costs no extra round trip to the target. */



static uint64_t g_next_pid = 1;
static uint64_t g_next_token = 0x4c50525300000001ull;
static uint64_t g_next_generation = 1;
static lprs_process_t *g_processes;
static uint64_t g_process_count;
static uint64_t g_process_capacity;
static lprs_waiter_t *g_waiters;
static uint64_t g_waiter_count;
static uint64_t g_waiter_capacity;

static int lprs_service_one_pending_request(int endpoint, uint64_t actor, int bootstrap);
static void lprs_refresh_exited_children(void);
static void lprs_complete_waiters(void);
static void lprs_interrupt_waiters(uint64_t token);
static int lprs_signal_process_fd(int process_fd, uint64_t signal);
static void lprs_complete_activation(lprs_process_t *proc);

static int lprs_status_to_errno(long status)
{
    return (int)pacha_kernel_status_to_errno(status);
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
    pacha_trace6(
        PACHA_TRACE_COMPONENT_LPR_SUPERVISOR,
        PACHA_TRACE_EVENT_GENERIC_ERROR,
        PACHA_TRACE_CLASS_ERROR,
        op,
        stage,
        (uint64_t)status,
        (uint64_t)raw_status,
        request_id,
        fd_count);
    pacha_trace4(
        PACHA_TRACE_COMPONENT_LPR_SUPERVISOR,
        PACHA_TRACE_EVENT_GENERIC_ERROR,
        PACHA_TRACE_CLASS_ERROR,
        subject,
        child_token,
        text != NULL ? pacha_trace_name_id(text) : 0,
        0);
    return 0;
}

static int lprs_parse_fd_arg(const char *arg, int *out_fd)
{
    const char prefix[] = "--boot-fd=";
    if (arg == NULL || out_fd == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    const size_t prefix_len = sizeof(prefix) - 1u;
    if (strncmp(arg, prefix, prefix_len) != 0) {
        return PACHA_STATUS_EINVAL;
    }
    const char *p = arg + prefix_len;
    if (*p == '\0') {
        return PACHA_STATUS_EINVAL;
    }
    uint64_t value = 0;
    while (*p != '\0') {
        if (*p < '0' || *p > '9') {
            return PACHA_STATUS_EINVAL;
        }
        value = value * 10u + (uint64_t)(*p - '0');
        if (value > UINT32_MAX) {
            return PACHA_STATUS_EINVAL;
        }
        p++;
    }
    if (value < 16) {
        return PACHA_STATUS_EINVAL;
    }
    *out_fd = (int)(uint32_t)value;
    return 0;
}

static int lprs_find_bootstrap_fd(int argc, char **argv, int *out_fd)
{
    if (argc <= 0 || argv == NULL || out_fd == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    for (int i = 1; i < argc; ++i) {
        if (lprs_parse_fd_arg(argv[i], out_fd) == 0) {
            return 0;
        }
    }
    return PACHA_STATUS_EINVAL;
}

static int lprs_read_bootstrap(int fd, struct lprs_boot_config *out)
{
    if (fd < 16 || out == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    const long got = pacha_fd_read(fd, out, sizeof(*out));
    if (got != (long)sizeof(*out)) {
        return PACHA_STATUS_EIO;
    }
    if (out->magic != LPRS_BOOT_CONFIG_MAGIC || out->endpoint_fd < 16 ||
        out->endpoint_fd >= PACHA_FD_TABLE_LIMIT || out->unix_admin_fd < 16 || out->unix_admin_fd >= PACHA_FD_TABLE_LIMIT ||
        out->endpoint_fd == out->unix_admin_fd || out->filed_admin_fd < 16 ||
        out->filed_admin_fd >= PACHA_FD_TABLE_LIMIT || out->filed_admin_fd == out->endpoint_fd ||
        out->filed_admin_fd == out->unix_admin_fd || out->flags != 0) {
        return PACHA_STATUS_EINVAL;
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
            return PACHA_STATUS_ENOMEM;
        }
        new_capacity *= 2u;
    }
    lprs_process_t *new_processes =
        (lprs_process_t *)realloc(g_processes, (size_t)(new_capacity * sizeof(*new_processes)));
    if (new_processes == NULL) {
        return PACHA_STATUS_ENOMEM;
    }
    memset(new_processes + g_process_capacity, 0,
        (size_t)((new_capacity - g_process_capacity) * sizeof(*new_processes)));
    g_processes = new_processes;
    g_process_capacity = new_capacity;
    return 0;
}

static int lprs_ensure_waiter_capacity(uint64_t needed)
{
    if (needed <= g_waiter_capacity) {
        return 0;
    }
    uint64_t new_capacity = g_waiter_capacity == 0 ? 16 : g_waiter_capacity;
    while (new_capacity < needed) {
        if (new_capacity > UINT64_MAX / 2u) {
            return PACHA_STATUS_ENOMEM;
        }
        new_capacity *= 2u;
    }
    lprs_waiter_t *new_waiters =
        (lprs_waiter_t *)realloc(g_waiters, (size_t)(new_capacity * sizeof(*new_waiters)));
    if (new_waiters == NULL) {
        return PACHA_STATUS_ENOMEM;
    }
    memset(new_waiters + g_waiter_capacity, 0,
        (size_t)((new_capacity - g_waiter_capacity) * sizeof(*new_waiters)));
    g_waiters = new_waiters;
    g_waiter_capacity = new_capacity;
    return 0;
}

static int lprs_queue_waiter(
    int page_fd,
    int reply_fd,
    const pacha_service_envelope_t *header,
    const lprs_wait4_t *request)
{
    if (page_fd < 16 || reply_fd < 16 || header == NULL || request == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_waiter_t *waiter = NULL;
    for (uint64_t i = 0; i < g_waiter_count; ++i) {
        if (!g_waiters[i].active) {
            waiter = &g_waiters[i];
            break;
        }
    }
    if (waiter == NULL) {
        const int capacity_status = lprs_ensure_waiter_capacity(g_waiter_count + 1u);
        if (capacity_status != 0) {
            return capacity_status;
        }
        waiter = &g_waiters[g_waiter_count++];
    }
    memset(waiter, 0, sizeof(*waiter));
    waiter->active = 1;
    waiter->page_fd = page_fd;
    waiter->reply_fd = reply_fd;
    waiter->header = *header;
    waiter->request = *request;
    return 0;
}

static void lprs_discard_pending_exec(lprs_process_t *proc)
{
    if (proc == NULL || proc->pending_exec_fd < 16) return;
    (void)pacha_syscall2(
        PACHA_PROCESS_SYSCALL_KILL,
        (uint64_t)(uint32_t)proc->pending_exec_fd,
        1);
    (void)pacha_fd_close(proc->pending_exec_fd);
    proc->pending_exec_fd = -1;
}

/* Both halves of an attached page: the mapping and the descriptor the
 * supervisor kept.  Dropping only the mapping leaked one descriptor per exec,
 * which a package install exhausted quickly enough to break unrelated work. */
static void lprs_diag_release(lprs_process_t *proc)
{
    if (proc->diag_page != NULL) {
        (void)pacha_munmap((void *)proc->diag_page, PACHA_SERVICE_PAGE_BYTES);
        proc->diag_page = NULL;
    }
    if (proc->diag_fd >= 16) {
        (void)pacha_fd_close(proc->diag_fd);
    }
    proc->diag_fd = -1;
}

static void lprs_unix_release(lprs_process_t *proc)
{
    if (proc->filed_fd >= 16) (void)pacha_fd_close(proc->filed_fd);
    proc->filed_fd = -1;
    proc->filed_session = 0;
    if (proc->unix_fd >= 16) (void)pacha_fd_close(proc->unix_fd);
    proc->unix_fd = -1;
    proc->unix_session = 0;
}

static void lprs_process_release_owned(lprs_process_t *proc)
{
    if (proc == NULL) {
        return;
    }
    if (proc->process_fd >= 16) {
        (void)pacha_fd_close(proc->process_fd);
    }
    if (proc->control_fd >= 16) (void)pacha_fd_close(proc->control_fd);
    if (proc->bootstrap_server_fd >= 16) (void)pacha_fd_close(proc->bootstrap_server_fd);
    if (proc->bootstrap_client_fd >= 16) (void)pacha_fd_close(proc->bootstrap_client_fd);
    if (proc->activation_page_fd >= 16) (void)pacha_fd_close(proc->activation_page_fd);
    if (proc->activation_reply_fd >= 16) (void)pacha_fd_close(proc->activation_reply_fd);
    lprs_unix_release(proc);
    lprs_diag_release(proc);
    lprs_discard_pending_exec(proc);
    memset(proc, 0, sizeof(*proc));
    proc->process_fd = -1;
    proc->pending_exec_fd = -1;
    proc->control_fd = -1;
    proc->bootstrap_server_fd = -1;
    proc->bootstrap_client_fd = -1;
    proc->activation_page_fd = -1;
    proc->activation_reply_fd = -1;
    proc->unix_fd = -1;
    proc->filed_fd = -1;
}

static void lprs_process_activate_empty(lprs_process_t *proc)
{
    lprs_process_release_owned(proc);
    proc->active = 1;
    proc->process_fd = -1;
    proc->pending_exec_fd = -1;
}

static void lprs_process_reap(lprs_process_t *proc)
{
    lprs_process_release_owned(proc);
    proc->waited = 1;
}

static void lprs_orphan_children(uint64_t parent_pid)
{
    if (parent_pid == 0) {
        return;
    }
    for (uint64_t i = 0; i < g_process_count; ++i) {
        lprs_process_t *child = &g_processes[i];
        if (!child->active || child->ppid != parent_pid) {
            continue;
        }
        if (child->exit_ready || (child->process_fd < 16 && child->pending_exec_fd < 16)) {
            lprs_process_reap(child);
        } else {
            if (child->pdeath_signal != 0 && child->process_fd >= 16) {
                (void)lprs_signal_process_fd(
                    child->process_fd, child->pdeath_signal);
                child->pdeath_signal = 0;
            }
            child->ppid = 0;
        }
    }
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

static int lprs_channel_admit(void)
{
    struct pacha_fd_table_info info;
    if (pacha_fd_table(0, &info) != 0) return -PACHA_LINUX_EMFILE;
    /* Keep room for an entire incoming request after allocating a pair. */
    const uint64_t needed = PACHA_IPC_MAX_TRANSFER_FDS + 2;
    if (info.free_slots < needed) {
        const uint64_t target = info.capacity + needed - info.free_slots;
        if (target > info.maximum || pacha_fd_table(target, &info) != 0)
            return -PACHA_LINUX_EMFILE;
    }
    return info.free_slots >= needed ? 0 : -PACHA_LINUX_EMFILE;
}

static int lprs_filed_session(lprs_process_t *proc, lprs_reply_cap_t *reply, uint64_t *out_session)
{
    if (!proc || !proc->active || proc->exit_ready) return PACHA_STATUS_ESRCH;
    if (proc->process_fd < 16 || proc->control_fd < 16) return PACHA_STATUS_EAGAIN;
    if (proc->filed_fd < 16) {
        if (g_filed_admin_fd < 16) return -PACHA_LINUX_ENOTCONN;
        int status = lprs_channel_admit();
        if (status) return status;
        const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
        int page_fd = pacha_vmo_create(PACHA_SERVICE_PAGE_BYTES, rights, PACHA_FD_FLAG_PRIVATE);
        if (page_fd < 16) return PACHA_STATUS_ENOMEM;
        void *page = pacha_mmap(page_fd, PACHA_SERVICE_PAGE_BYTES,
            PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
        if (!page) { (void)pacha_fd_close(page_fd); return PACHA_STATUS_ENOMEM; }
        pacha_service_envelope_t *header = page;
        *header = (pacha_service_envelope_t){ .magic = PACHA_SERVICE_REQUEST_MAGIC,
            .abi_version = PACHA_SERVICE_ABI_VERSION, .service_id = FILED_SERVICE_ID,
            .op = FILED_OP_CLIENT_REGISTER, .request_id = proc->token,
            .trace_id = proc->token, .payload_size = sizeof(filed_identity_t) };
        const struct unix_credentials *c = &proc->credentials;
        filed_identity_t identity = { .generation = c->generation, .pid = c->pid,
            .uid = c->uid, .gid = c->gid, .euid = c->euid, .egid = c->egid,
            .suid = c->suid, .sgid = c->sgid,
            .rights = proc->filed_rights };
        memcpy((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES, &identity, sizeof(identity));
        struct pacha_ipc_fd cap = { .fd = (uint64_t)page_fd, .rights = rights & ~PACHA_FD_RIGHT_TRANSFER };
        struct pacha_ipc_msg message = { .word0 = PACHA_SERVICE_REQUEST_MAGIC,
            .word3 = proc->token, .fds = &cap, .fd_count = 1 };
        int reply_fd = pacha_ipc_call(g_filed_admin_fd, &message);
        status = -PACHA_LINUX_EIO;
        struct pacha_ipc_fd received[PACHA_IPC_MAX_TRANSFER_FDS] = {{0}};
        struct pacha_ipc_msg response = { .fds = received, .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS };
        if (reply_fd >= 16) {
            int received_status = pacha_ipc_recv_wait(reply_fd, &response, PACHA_FD_WAIT_FOREVER);
            (void)pacha_fd_close(reply_fd);
            if (!received_status) {
                struct pacha_fd_info info;
                uint64_t required = LPRS_CLIENT_RIGHTS | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER;
                if (response.word0 == PACHA_SERVICE_REPLY_MAGIC && response.word3 == proc->token &&
                    header->magic == PACHA_SERVICE_REPLY_MAGIC && header->request_id == proc->token &&
                    header->status == 0 && header->result && response.fd_count == 1 &&
                    received[0].fd >= 16 && received[0].fd < PACHA_FD_TABLE_LIMIT &&
                    pacha_fd_get_info((int)received[0].fd, &info) == 0 &&
                    info.kind == PACHA_FD_KIND_CHANNEL && info.rights == required) {
                    proc->filed_fd = (int)received[0].fd;
                    proc->filed_session = header->result;
                    received[0].fd = 0;
                    status = 0;
                } else if (header->status < 0) status = (int)header->status;
                for (unsigned i = 0; i < response.fd_count; i++)
                    if (received[i].fd >= 16) (void)pacha_fd_close((int)received[i].fd);
            }
        }
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
        (void)pacha_fd_close(page_fd);
        if (status) return status;
    }
    *out_session = proc->filed_session;
    *reply = (lprs_reply_cap_t){ .fd = proc->filed_fd, .rights = LPRS_CLIENT_RIGHTS,
        .transfer_flags = PACHA_IPC_TRANSFER_PRIVATE | PACHA_IPC_TRANSFER_CLOEXEC };
    return 0;
}

static int lprs_unix_session(lprs_process_t *proc, lprs_reply_cap_t *reply, uint64_t *out_session)
{
    /* Both services use the same manager-owned process identity. The private
     * unixd pathname bridge can only act through this filed connection. */
    lprs_reply_cap_t filed_reply = {0};
    uint64_t filed_session = 0;
    int filed_status = lprs_filed_session(proc, &filed_reply, &filed_session);
    if (filed_status) return filed_status;
    if (!proc || !proc->active || proc->exit_ready) return PACHA_STATUS_ESRCH;
    if (proc->process_fd < 16 || proc->control_fd < 16) return PACHA_STATUS_EAGAIN;
    if (proc->unix_fd < 16) {
        if (g_unix_admin_fd < 16) return -PACHA_LINUX_ENOTCONN;
        if (proc->pid > INT32_MAX || proc->credentials.pid != (int32_t)proc->pid ||
            !proc->credentials.generation) return PACHA_STATUS_EINVAL;
        if (g_unix_request == UINT64_MAX) return -PACHA_LINUX_EOVERFLOW;
        const int capacity_status = lprs_channel_admit();
        if (capacity_status != 0) return capacity_status;
        struct unix_control request = { .operation = UNIX_OP_PROCESS_REGISTER,
            .request = ++g_unix_request, .credentials = proc->credentials };
        struct pacha_ipc_fd received = {0};
        unsigned count = 0;
        const int status = unix_client_call(g_unix_admin_fd, &request, NULL, 0, &received, 1, &count);
        if (status != 0) return status;
        struct pacha_fd_info info;
        const uint64_t required = LPRS_UNIX_CLIENT_RIGHTS | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER;
        if (count != 1 || !request.result || received.fd < 16 || received.fd >= PACHA_FD_TABLE_LIMIT ||
            pacha_fd_get_info((int)received.fd, &info) != 0 || info.kind != PACHA_FD_KIND_CHANNEL ||
            (info.rights & required) != required) {
            if (count && received.fd >= 16) (void)pacha_fd_close((int)received.fd);
            return -PACHA_LINUX_EIO;
        }
        /* This retained endpoint keeps the socket owner alive across the
         * CLOEXEC close of the old image's private client. Release on native
         * process exit, not when its parent eventually calls wait4. */
        proc->unix_fd = (int)received.fd;
        proc->unix_session = request.result;
    }
    *out_session = proc->unix_session;
    /* Notification senders can outlive unixd; observe the session itself. */
    *reply = (lprs_reply_cap_t){ .fd = proc->unix_fd, .rights = LPRS_UNIX_CLIENT_RIGHTS,
        .transfer_flags = PACHA_IPC_TRANSFER_PRIVATE | PACHA_IPC_TRANSFER_CLOEXEC };
    return 0;
}

static int lprs_prepare_control(lprs_process_t *proc, lprs_reply_cap_t *reply)
{
    if (!proc || !proc->active || proc->exit_ready) return PACHA_STATUS_ESRCH;
    if (proc->bootstrap_server_fd >= 16 || proc->pending_exec_fd >= 16) return PACHA_STATUS_EAGAIN;
    int status = lprs_channel_admit();
    if (status != 0) return status;
    struct pacha_ipc_channel_pair pair;
    if (pacha_ipc_channel_create(&pair, LPRS_CHANNEL_RIGHTS, 0) != 0) return PACHA_STATUS_ENOMEM;
    proc->bootstrap_server_fd = pair.a;
    proc->bootstrap_client_fd = pair.b;
    *reply = (lprs_reply_cap_t){ .fd = pair.b,
        .rights = LPRS_CLIENT_RIGHTS | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SET_FLAGS };
    return 0;
}

static int lprs_prepare_exec_control(lprs_process_t *proc, lprs_reply_cap_t *reply)
{
    const int status = lprs_prepare_control(proc, reply);
    if (status == 0) {
        /* Self-exec retains the current FD table. No installer needs this
         * capability, and a concurrent fork must not inherit the handoff. */
        reply->rights = LPRS_CLIENT_RIGHTS;
        reply->transfer_flags = PACHA_IPC_TRANSFER_PRIVATE;
    }
    return status;
}

static int lprs_activate_control(lprs_process_t *proc, lprs_reply_cap_t *reply)
{
    if (!proc || !proc->active || proc->bootstrap_server_fd < 16 || proc->exit_ready) return PACHA_STATUS_ESRCH;
    if (proc->process_fd < 16 && proc->pending_exec_fd < 16) return PACHA_STATUS_EAGAIN;
    if (proc->control_fd >= 16 && proc->pending_exec_fd < 16) return PACHA_STATUS_EAGAIN;
    const int status = lprs_channel_admit();
    if (status != 0) return status;
    struct pacha_ipc_channel_pair pair;
    if (pacha_ipc_channel_create(&pair, LPRS_CHANNEL_RIGHTS, 0) != 0) return PACHA_STATUS_ENOMEM;
    if (proc->control_fd >= 16) (void)pacha_fd_close(proc->control_fd);
    (void)pacha_fd_close(proc->bootstrap_server_fd);
    (void)pacha_fd_close(proc->bootstrap_client_fd);
    proc->bootstrap_server_fd = proc->bootstrap_client_fd = -1;
    proc->control_fd = pair.a;
    /* One-shot activation runs before any Linux user thread starts. Failure
     * to receive this reply aborts bootstrap; the handoff cannot be reused
     * to acquire another copy of the running process's authority. */
    *reply = (lprs_reply_cap_t){ .fd = pair.b, .rights = LPRS_CLIENT_RIGHTS,
        .transfer_flags = PACHA_IPC_TRANSFER_MOVE | PACHA_IPC_TRANSFER_PRIVATE |
            PACHA_IPC_TRANSFER_CLOEXEC };
    return 0;
}

static int lprs_authorize(uint64_t actor, int bootstrap, uint32_t op, uint64_t subject)
{
    if (!actor) {
        switch (op) {
        case LPRS_OP_HELLO:
        case LPRS_OP_PROCESS_REGISTER_EXEC:
        case LPRS_OP_PROCESS_REGISTER_FD:
        case LPRS_OP_SIGNAL_DELIVER_TTY:
            return 0;
        case LPRS_OP_PROCESS_EXEC_COMMIT_BEGIN:
        case LPRS_OP_PROCESS_EXEC_COMMIT_CANCEL: {
            const lprs_process_t *proc = lprs_find_by_token(subject);
            return proc && proc->control_fd < 16 ? 0 : -PACHA_LINUX_EPERM;
        }
        default:
            return -PACHA_LINUX_EPERM;
        }
    }
    const lprs_process_t *caller = lprs_find_by_token(actor);
    if (!caller || caller->exit_ready) return PACHA_STATUS_ESRCH;
    if (bootstrap) return caller->bootstrap_server_fd >= 16 &&
        op == LPRS_OP_PROCESS_ACTIVATE && subject == actor ? 0 : -PACHA_LINUX_EPERM;
    if (caller->control_fd < 16) return -PACHA_LINUX_EPERM;
    if (op == LPRS_OP_HELLO) return 0;
    if (op == LPRS_OP_PROCESS_ACTIVATE || op == LPRS_OP_PROCESS_REGISTER_EXEC ||
        op == LPRS_OP_PROCESS_REGISTER_FD || op == LPRS_OP_SIGNAL_DELIVER_TTY)
        return -PACHA_LINUX_EPERM;
    if (op == LPRS_OP_PROCESS_FORK_CANCEL || op == LPRS_OP_PROCESS_FORK_PARENT_REGISTER) {
        const lprs_process_t *child = lprs_find_by_token(subject);
        return child && child->ppid == caller->pid && child->control_fd < 16 ? 0 : -PACHA_LINUX_EPERM;
    }
    return subject == actor ? 0 : -PACHA_LINUX_EPERM;
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

static int lprs_parent_has_unreported_exit(const lprs_process_t *parent)
{
    if (parent == NULL || !parent->active) {
        return 0;
    }
    for (uint64_t i = 0; i < g_process_count; ++i) {
        const lprs_process_t *child = &g_processes[i];
        if (child->active && child->exit_ready && !child->exit_notified &&
            child->ppid == parent->pid)
        {
            return 1;
        }
    }
    return 0;
}

static int lprs_parent_has_children(const lprs_process_t *parent)
{
    if (parent == NULL || !parent->active) {
        return 0;
    }
    for (uint64_t i = 0; i < g_process_count; ++i) {
        const lprs_process_t *child = &g_processes[i];
        if (child->active && !child->waited && child->ppid == parent->pid) {
            return 1;
        }
    }
    return 0;
}

static void lprs_acknowledge_reported_exits(const lprs_process_t *parent)
{
    if (parent == NULL || !parent->active) {
        return;
    }
    for (uint64_t i = 0; i < g_process_count; ++i) {
        lprs_process_t *child = &g_processes[i];
        if (child->active && child->exit_ready && !child->exit_notified &&
            child->ppid == parent->pid)
        {
            child->exit_notified = 1;
        }
    }
}

static void lprs_write_state(const lprs_process_t *proc, lprs_process_state_t *out)
{
    memset(out, 0, sizeof(*out));
    out->token = proc->token;
    out->generation = proc->generation;
    out->child_sequence = proc->child_sequence;
    out->pid = proc->pid;
    out->ppid = proc->ppid;
    out->sid = proc->sid;
    out->pgrp = proc->pgrp;
    out->credentials = (lprs_credentials_t){
        .uid = proc->credentials.uid, .euid = proc->credentials.euid,
        .suid = proc->credentials.suid, .gid = proc->credentials.gid,
        .egid = proc->credentials.egid, .sgid = proc->credentials.sgid,
        .group_count = proc->group_count,
    };
    memcpy(out->credentials.groups, proc->groups, sizeof(proc->groups));
    out->filed_rights = proc->filed_rights;
    out->credential_rights = proc->credential_rights;
    out->foreground_pgrp = proc->foreground_pgrp;
    out->cwd_handle = proc->cwd_handle;
    if (lprs_parent_has_unreported_exit(proc)) {
        out->flags |= LPRS_PROCESS_STATE_SIGCHLD_PENDING;
    }
    if (lprs_parent_has_children(proc)) {
        out->flags |= LPRS_PROCESS_STATE_HAS_CHILDREN;
    }
    lprs_copy_string(out->ctty, sizeof(out->ctty), proc->ctty);
    lprs_copy_string(out->cwd, sizeof(out->cwd), proc->cwd);
}

static int lprs_publish_filed_credentials(lprs_process_t *proc, const struct unix_credentials *c,
    int *may_have_published)
{
    *may_have_published = 0;
    if (!proc->filed_session) return 0;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    int fd = pacha_vmo_create(PACHA_SERVICE_PAGE_BYTES, rights, PACHA_FD_FLAG_PRIVATE);
    if (fd < 16) return -12;
    void *page = pacha_mmap(fd, PACHA_SERVICE_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!page) { (void)pacha_fd_close(fd); return -12; }
    pacha_service_envelope_t *header = page;
    *header = (pacha_service_envelope_t){ .magic = PACHA_SERVICE_REQUEST_MAGIC,
        .abi_version = PACHA_SERVICE_ABI_VERSION, .service_id = FILED_SERVICE_ID,
        .op = FILED_OP_CLIENT_CREDENTIALS, .request_id = proc->token,
        .payload_size = sizeof(filed_identity_update_t) };
    filed_identity_update_t update = { .client = proc->filed_session,
        .identity = { .generation = c->generation, .pid = c->pid,
            .uid = c->uid, .gid = c->gid, .euid = c->euid, .egid = c->egid,
            .suid = c->suid, .sgid = c->sgid, .rights = proc->filed_rights } };
    memcpy((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES, &update, sizeof(update));
    struct pacha_ipc_fd cap = { .fd = (uint64_t)fd, .rights = rights & ~PACHA_FD_RIGHT_TRANSFER };
    struct pacha_ipc_msg message = { .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word3 = proc->token, .fds = &cap, .fd_count = 1 };
    int reply_fd = pacha_ipc_call(g_filed_admin_fd, &message);
    int status = -5;
    if (reply_fd >= 16) {
        *may_have_published = 1;
        struct pacha_ipc_msg response = {0};
        int received = pacha_ipc_recv_wait(reply_fd, &response, PACHA_FD_WAIT_FOREVER);
        (void)pacha_fd_close(reply_fd);
        if (!received && response.word0 == PACHA_SERVICE_REPLY_MAGIC &&
            response.word3 == proc->token && header->magic == PACHA_SERVICE_REPLY_MAGIC &&
            header->request_id == proc->token && header->status <= 0) {
            status = (int)header->status;
            if (status) *may_have_published = 0; /* Valid rejection, no mutation. */
        }
    }
    (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
    (void)pacha_fd_close(fd);
    return status;
}

static int lprs_change_credentials(lprs_process_t *proc, lprs_credential_request_t *req)
{
    if (!proc || !proc->active || proc->exit_ready) return -3;
    lprs_process_state_t state;
    lprs_write_state(proc, &state);
    lprs_credentials_t next;
    int status = lprs_credentials_apply(&state.credentials, proc->credential_rights, req, &next);
    if (status) return status;
    if (!memcmp(&next, &state.credentials, sizeof(next))) {
        req->credentials = next;
        return 0;
    }
    struct unix_credentials identity = proc->credentials;
    identity.uid = next.uid; identity.euid = next.euid; identity.suid = next.suid;
    identity.gid = next.gid; identity.egid = next.egid; identity.sgid = next.sgid;
    int may_have_published = 0;
    status = lprs_publish_filed_credentials(proc, &identity, &may_have_published);
    if (status && !may_have_published) return status;
    if (!status && proc->unix_session) {
        struct unix_control update = { .operation = UNIX_OP_PROCESS_CREDENTIALS,
            .request = ++g_unix_request, .socket = proc->unix_session, .credentials = identity };
        unsigned received = 0;
        status = unix_client_call(g_unix_admin_fd, &update, NULL, 0, NULL, 0, &received);
    }
    if (status) {
        /* Never resume a process with a partially published identity. A lost
         * reply is indistinguishable from an applied update; fail closed. */
        fprintf(stderr, "[lprs] credential publication failed pid=%llu status=%d\n",
            (unsigned long long)proc->pid, status);
        if (proc->process_fd >= 16)
            (void)pacha_syscall2(PACHA_PROCESS_SYSCALL_KILL, (uint64_t)proc->process_fd, 1);
        return status;
    }
    proc->credentials = identity;
    proc->group_count = next.group_count;
    memcpy(proc->groups, next.groups, sizeof(proc->groups));
    req->credentials = next;
    return 0;
}

static int lprs_register_exec(void *page, uint64_t *out_token)
{
    if (page == NULL || out_token == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_register_exec_t *req = (lprs_register_exec_t *)page;
    if (!g_accounts_ready) return -5;
    if (!req->account[0] || !memchr(req->account, 0, sizeof(req->account)) ||
        (req->filed_rights & ~1023u) || (req->credential_rights & ~3u)) return -22;
    lprs_credentials_t initial;
    int status = lprs_resolve_account(&g_accounts, req->account, &initial);
    if (status) return status;
    if (g_next_pid > INT32_MAX) return -PACHA_LINUX_EOVERFLOW;
    lprs_process_t *proc = lprs_alloc_process();
    if (proc == NULL) {
        return PACHA_STATUS_ENOMEM;
    }
    proc->token = g_next_token++;
    proc->generation = g_next_generation++;
    proc->pid = g_next_pid++;
    /* The launch account was resolved from the canonical files before HELLO.
     * Never accept UID/GID or supplementary groups from the request page. */
    proc->credentials = (struct unix_credentials){ .pid = (int32_t)proc->pid,
        .generation = proc->generation,
        .uid = initial.uid, .euid = initial.euid,
        .suid = initial.suid, .gid = initial.gid,
        .egid = initial.egid, .sgid = initial.sgid };
    proc->group_count = initial.group_count;
    memcpy(proc->groups, initial.groups, sizeof(proc->groups));
    proc->filed_rights = req->filed_rights;
    proc->credential_rights = req->credential_rights;
#if defined(LPRS_TEST_CREDENTIALS) && LPRS_TEST_CREDENTIALS
    /* Test image only: exercise nonzero/distinct IDs without introducing an
     * identity-changing API or account-management policy in this step. */
    proc->credentials.uid = 1001; proc->credentials.euid = 1002; proc->credentials.suid = 1003;
    proc->credentials.gid = 2001; proc->credentials.egid = 2002; proc->credentials.sgid = 2003;
#endif
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
    fprintf(stderr, "[lprs] launch account=%s pid=%llu uid=%u gid=%u filed=0x%x credentials=0x%x\n",
        req->account, (unsigned long long)proc->pid, initial.uid, initial.gid,
        proc->filed_rights, proc->credential_rights);
    *out_token = proc->token;
    return 0;
}

/* The page belongs to the process being described; this only maps it read
 * only, so a supervisor bug can never corrupt what a process reports. */
static int lprs_diag_attach(uint64_t token, int diag_fd, int *out_keep_fd)
{
    if (diag_fd < 16 || out_keep_fd == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_process_t *proc = lprs_find_by_token(token);
    if (proc == NULL) {
        return PACHA_STATUS_ESRCH;
    }
    /* exec keeps the token but replaces the address space, so the new image
     * attaches again.  Replacing the mapping is the point: refusing the second
     * attach would leave this reporting the page the old image stopped
     * writing, frozen on the execve it never returned from. */
    lprs_diag_release(proc);
    void *addr = pacha_mmap(
        diag_fd,
        PACHA_SERVICE_PAGE_BYTES,
        PACHA_PROT_READ,
        PACHA_MMAP_SHARED,
        0);
    if (addr == NULL) {
        return PACHA_STATUS_ENOMEM;
    }
    proc->diag_page = (const lprs_diag_slot_t *)addr;
    proc->diag_fd = diag_fd;
    *out_keep_fd = diag_fd;
    return 0;
}

/* Reads one slot without locking.  seq is odd while the owner is writing, so
 * an odd value or a change across the body means the sample was torn and is
 * reported as unavailable rather than as a wrong answer. */
static void lprs_diag_sample(
    const lprs_process_t *proc,
    lprs_process_query_t *out)
{
    out->diag_valid = 0;
    out->syscall_nr = LPRS_DIAG_SYSCALL_NONE;
    if (proc->diag_page == NULL) {
        return;
    }
    const volatile lprs_diag_slot_t *slot = proc->diag_page;
    for (unsigned attempt = 0; attempt < 4u; ++attempt) {
        const uint64_t before = slot->seq;
        if ((before & 1u) != 0) continue;
        const uint64_t syscall_nr = slot->syscall_nr;
        const uint64_t arg0 = slot->arg0;
        const uint64_t arg1 = slot->arg1;
        const uint64_t enter_tick = slot->enter_tick;
        if (slot->seq != before) continue;
        out->syscall_nr = syscall_nr;
        out->syscall_arg0 = arg0;
        out->syscall_arg1 = arg1;
        out->syscall_enter_tick = enter_tick;
        out->diag_valid = 1;
        return;
    }
}

/* The program name follows the token across exec, so the personality reports
 * it just before committing and the record survives into the new image. */
static int lprs_set_comm(uint64_t token, void *page, uint64_t payload_size)
{
    if (page == NULL || payload_size < sizeof(lprs_process_set_comm_t)) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_process_t *proc = lprs_find_by_token(token);
    if (proc == NULL) {
        return PACHA_STATUS_ESRCH;
    }
    const lprs_process_set_comm_t *req = (const lprs_process_set_comm_t *)page;
    char comm[LPRS_PROCESS_COMM_BYTES];
    char cmdline[LPRS_PROCESS_CMDLINE_BYTES];
    snprintf(comm, sizeof(comm), "%.*s",
        (int)(sizeof(req->comm) - 1u), req->comm);
    snprintf(cmdline, sizeof(cmdline), "%.*s",
        (int)(sizeof(req->cmdline) - 1u), req->cmdline);
    lprs_copy_string(proc->comm, sizeof(proc->comm), comm);
    lprs_copy_string(proc->cmdline, sizeof(proc->cmdline), cmdline);
    return 0;
}

static int lprs_query_process(void *page, uint64_t payload_size)
{
    if (page == NULL || payload_size < sizeof(lprs_process_query_t)) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_process_query_t *req = (lprs_process_query_t *)page;
    const lprs_process_t *proc = lprs_find_by_pid(req->pid);
    if (proc == NULL) {
        return PACHA_STATUS_ESRCH;
    }
    req->ppid = proc->ppid;
    req->sid = proc->sid;
    req->pgrp = proc->pgrp;
    req->run_state = proc->exit_ready != 0 ?
        LPRS_PROCESS_RUN_STATE_ZOMBIE : LPRS_PROCESS_RUN_STATE_RUNNING;
    req->exit_status = proc->exit_status;
    req->flags = 0;
    lprs_copy_string(req->comm, sizeof(req->comm), proc->comm);
    lprs_copy_string(req->cmdline, sizeof(req->cmdline), proc->cmdline);
    lprs_copy_string(req->cwd, sizeof(req->cwd), proc->cwd);
    lprs_diag_sample(proc, req);
    return 0;
}

static int lprs_register_process_fd_handle(uint64_t token, int process_fd)
{
    if (process_fd < 16) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_process_t *proc = lprs_find_by_token(token);
    if (proc == NULL) {
        return PACHA_STATUS_ESRCH;
    }
    if (proc->process_fd >= 16) {
        (void)pacha_fd_close(proc->process_fd);
    }
    proc->process_fd = process_fd;
    return 0;
}

static int lprs_exec_commit_begin(uint64_t token, int process_fd)
{
    if (process_fd < 16) return PACHA_STATUS_EINVAL;
    lprs_process_t *proc = lprs_find_by_token(token);
    if (proc == NULL) return PACHA_STATUS_ESRCH;
    if (proc->exit_ready) return PACHA_STATUS_ESRCH;
    if (proc->pending_exec_fd >= 16) return PACHA_STATUS_EAGAIN;
    proc->pending_exec_fd = process_fd;
    return 0;
}

static int lprs_exec_commit_cancel(uint64_t token)
{
    lprs_process_t *proc = lprs_find_by_token(token);
    if (proc == NULL) return PACHA_STATUS_ESRCH;
    const int initial_exec = proc->process_fd < 16;
    lprs_discard_pending_exec(proc);
    if (proc->bootstrap_server_fd >= 16) (void)pacha_fd_close(proc->bootstrap_server_fd);
    if (proc->bootstrap_client_fd >= 16) (void)pacha_fd_close(proc->bootstrap_client_fd);
    proc->bootstrap_server_fd = proc->bootstrap_client_fd = -1;
    if (proc->activation_page_fd >= 16) (void)pacha_fd_close(proc->activation_page_fd);
    if (proc->activation_reply_fd >= 16) (void)pacha_fd_close(proc->activation_reply_fd);
    proc->activation_page_fd = proc->activation_reply_fd = -1;
    if (initial_exec) lprs_process_reap(proc);
    return 0;
}

static int lprs_exec_commit_done(uint64_t token)
{
    lprs_process_t *proc = lprs_find_by_token(token);
    if (proc == NULL) return PACHA_STATUS_ESRCH;
    if (proc->pending_exec_fd < 16) return PACHA_STATUS_EINVAL;
    if (proc->process_fd < 16) {
        proc->process_fd = proc->pending_exec_fd;
    } else {
        (void)pacha_fd_close(proc->pending_exec_fd);
    }
    proc->pending_exec_fd = -1;
    proc->generation = g_next_generation++;
    return 0;
}

static int lprs_get_process_state(uint64_t token, void *page)
{
    if (page == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_process_t *proc = lprs_find_by_token(token);
    if (proc == NULL) {
        return PACHA_STATUS_ESRCH;
    }
    lprs_write_state(proc, (lprs_process_state_t *)page);
    if ((((lprs_process_state_t *)page)->flags &
         LPRS_PROCESS_STATE_SIGCHLD_PENDING) != 0)
    {
        lprs_acknowledge_reported_exits(proc);
    }
    return 0;
}

static int lprs_list_processes(lprs_process_list_t *list)
{
    if (list == NULL || list->token == 0) {
        return PACHA_STATUS_EINVAL;
    }
    if (lprs_find_by_token(list->token) == NULL) {
        return PACHA_STATUS_ESRCH;
    }

    uint64_t capacity = list->capacity;
    if (capacity > LPRS_PROCESS_LIST_CAPACITY) {
        capacity = LPRS_PROCESS_LIST_CAPACITY;
    }
    uint64_t skipped = 0;
    uint64_t count = 0;
    for (uint64_t i = 0; i < g_process_count; ++i) {
        const lprs_process_t *proc = &g_processes[i];
        if (!proc->active) continue;
        if (skipped < list->offset) {
            skipped += 1u;
            continue;
        }
        if (count >= capacity) break;
        list->pids[count++] = proc->pid;
    }
    list->capacity = capacity;
    list->count = count;
    return 0;
}

static int lprs_fork_begin(uint64_t parent_token, void *page)
{
    if (page == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_process_t *parent = lprs_find_by_token(parent_token);
    if (parent == NULL) {
        return PACHA_STATUS_ESRCH;
    }
    const uint64_t parent_pid = parent->pid;
    const uint64_t parent_sid = parent->sid;
    const uint64_t parent_pgrp = parent->pgrp;
    const uint64_t parent_foreground_pgrp = parent->foreground_pgrp;
    const uint64_t parent_cwd_handle = parent->cwd_handle;
    const struct unix_credentials parent_credentials = parent->credentials;
    const uint32_t parent_group_count = parent->group_count;
    const uint32_t parent_filed_rights = parent->filed_rights;
    const uint32_t parent_credential_rights = parent->credential_rights;
    uint32_t parent_groups[LPRS_MAX_GROUPS];
    memcpy(parent_groups, parent->groups, sizeof(parent_groups));
    if (g_next_pid > INT32_MAX) return -PACHA_LINUX_EOVERFLOW;
    char parent_ctty[LPRS_CTTY_BYTES];
    char parent_cwd[LPRS_CWD_BYTES];
    lprs_copy_string(parent_ctty, sizeof(parent_ctty), parent->ctty);
    lprs_copy_string(parent_cwd, sizeof(parent_cwd), parent->cwd);

    lprs_process_t *child = lprs_alloc_process();
    if (child == NULL) {
        return PACHA_STATUS_ENOMEM;
    }
    child->token = g_next_token++;
    child->generation = g_next_generation++;
    child->pid = g_next_pid++;
    child->credentials = parent_credentials;
    child->credentials.pid = (int32_t)child->pid;
    child->credentials.generation = child->generation;
    child->group_count = parent_group_count;
    child->filed_rights = parent_filed_rights;
    child->credential_rights = parent_credential_rights;
    memcpy(child->groups, parent_groups, sizeof(child->groups));
    child->ppid = parent_pid;
    child->sid = parent_sid;
    child->pgrp = parent_pgrp;
    child->foreground_pgrp = parent_foreground_pgrp;
    child->cwd_handle = parent_cwd_handle;
    lprs_copy_string(child->ctty, sizeof(child->ctty), parent_ctty);
    lprs_copy_string(child->cwd, sizeof(child->cwd), parent_cwd);

    lprs_fork_t *fork = (lprs_fork_t *)page;
    memset(fork, 0, sizeof(*fork));
    fork->parent_token = parent_token;
    fork->child_token = child->token;
    fork->child_pid = child->pid;
    fork->child_ppid = child->ppid;
    fork->child_sid = child->sid;
    fork->child_pgrp = child->pgrp;
    return 0;
}

static int lprs_fork_cancel(uint64_t token)
{
    lprs_process_t *proc = lprs_find_by_token(token);
    if (proc == NULL) {
        return PACHA_STATUS_ESRCH;
    }
    if (proc->process_fd >= 16) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_process_reap(proc);
    return 0;
}

static int lprs_signal_process_fd(int process_fd, uint64_t signal)
{
    if (process_fd < 16) {
        return PACHA_STATUS_ESRCH;
    }
    if (signal == 0) {
        return 0;
    }
    if (signal == 9u) {
        return lprs_status_to_errno(pacha_syscall2(
            PACHA_PROCESS_SYSCALL_KILL,
            (uint64_t)(uint32_t)process_fd,
            128u + signal));
    }
    if (signal == LPRS_SIGSTOP) {
        return lprs_status_to_errno(pacha_syscall2(
            PACHA_PROCESS_SYSCALL_STOP,
            (uint64_t)(uint32_t)process_fd,
            signal));
    }
    if (signal == LPRS_SIGCONT) {
        return lprs_status_to_errno(pacha_syscall2(
            PACHA_PROCESS_SYSCALL_CONTINUE,
            (uint64_t)(uint32_t)process_fd,
            signal));
    }
    return lprs_status_to_errno(pacha_syscall2(
        PACHA_PROCESS_SYSCALL_SIGNAL,
        (uint64_t)(uint32_t)process_fd,
        signal));
}

static int lprs_signal_interrupts_wait(uint64_t signal)
{
    return signal != 0 && signal != 9u && signal != LPRS_SIGSTOP;
}

static int lprs_try_wait_process_fd(
    int process_fd,
    uint64_t *out_state,
    uint64_t *out_exit_code)
{
    if (process_fd < 16 || out_state == NULL || out_exit_code == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_process_status_t status;
    memset(&status, 0, sizeof(status));
    const long wait_status = pacha_syscall2(
        PACHA_PROCESS_SYSCALL_WAIT,
        (uint64_t)(uint32_t)process_fd,
        (uint64_t)(uintptr_t)&status);
    if (wait_status == 0) {
        if (status.state != LPRS_NATIVE_PROCESS_EXITED &&
            status.state != LPRS_NATIVE_PROCESS_KILLED)
        {
            return PACHA_STATUS_EAGAIN;
        }
        *out_state = status.state;
        *out_exit_code = status.exit_code & 0xffu;
        return 0;
    }
    return lprs_status_to_errno(wait_status);
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

static int lprs_find_exited_child(
    const lprs_process_t *parent,
    int64_t requested,
    uint64_t *out_selected_index,
    uint64_t *out_exit_state,
    uint64_t *out_exit_code,
    uint64_t *out_match_count)
{
    if (out_selected_index == NULL || out_exit_state == NULL ||
        out_exit_code == NULL || out_match_count == NULL)
    {
        return PACHA_STATUS_EINVAL;
    }
    *out_selected_index = UINT64_MAX;
    *out_exit_state = 0;
    *out_exit_code = 0;
    *out_match_count = 0;
    for (uint64_t i = 0; i < g_process_count; ++i) {
        lprs_process_t *child = &g_processes[i];
        if (!lprs_child_matches(parent, child, requested)) {
            continue;
        }
        *out_match_count += 1;
        if (child->exit_ready) {
            *out_selected_index = i;
            *out_exit_state = child->exit_state;
            *out_exit_code = child->exit_status;
            return 0;
        }
        if (child->process_fd < 16) {
            continue;
        }
        uint64_t exit_state = 0;
        uint64_t exit_code = 0;
        const int status = lprs_try_wait_process_fd(
            child->process_fd, &exit_state, &exit_code);
        if (status == PACHA_STATUS_EAGAIN) {
            continue;
        }
        if (status != 0) {
            return status;
        }
        *out_selected_index = i;
        *out_exit_state = exit_state;
        *out_exit_code = exit_code;
        return 0;
    }
    return 0;
}

static int lprs_find_matching_child(
    uint64_t parent_token,
    int64_t requested,
    uint64_t *out_selected_index,
    uint64_t *out_exit_state,
    uint64_t *out_exit_code)
{
    const lprs_process_t *parent = lprs_find_by_token(parent_token);
    if (parent == NULL) {
        return PACHA_STATUS_ESRCH;
    }
    uint64_t match_count = 0;
    const int status = lprs_find_exited_child(
        parent, requested, out_selected_index, out_exit_state,
        out_exit_code, &match_count);
    if (status != 0 || *out_selected_index != UINT64_MAX) {
        return status;
    }
    return match_count == 0 ? PACHA_STATUS_ECHILD : PACHA_STATUS_EAGAIN;
}

static int lprs_wait4(void *page, uint64_t *out_result)
{
    if (page == NULL || out_result == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    *out_result = 0;
    lprs_wait4_t *req = (lprs_wait4_t *)page;
    lprs_process_t *parent = lprs_find_by_token(req->token);
    if (parent == NULL) {
        return PACHA_STATUS_ESRCH;
    }
    if ((req->options & ~((uint64_t)LPRS_WNOHANG)) != 0) {
        return PACHA_STATUS_EINVAL;
    }
    uint64_t selected_index = UINT64_MAX;
    uint64_t exit_state = 0;
    uint64_t exit_code = 0;
    const int wait_status = lprs_find_matching_child(
        parent->token, req->requested_pid, &selected_index,
        &exit_state, &exit_code);
    if (wait_status == PACHA_STATUS_EAGAIN && (req->options & LPRS_WNOHANG) != 0) {
        req->result_pid = 0;
        req->status = 0;
        req->exit_code = 0;
        *out_result = 0;
        return 0;
    }
    if (wait_status != 0) {
        return wait_status;
    }
    if (selected_index == UINT64_MAX || selected_index >= g_process_count) {
        return PACHA_STATUS_ECHILD;
    }
    lprs_process_t *selected = &g_processes[selected_index];
    const uint64_t selected_pid = selected->pid;
    const int selected_process_fd = selected->process_fd;
    if (!selected->active || selected->pid != selected_pid || selected->process_fd != selected_process_fd) {
        return PACHA_STATUS_ECHILD;
    }
    req->result_pid = (int64_t)selected_pid;
    req->exit_code = exit_code;
    req->status = exit_state == LPRS_NATIVE_PROCESS_KILLED ?
        ((exit_code >= 128u ? exit_code - 128u : exit_code) & 0x7fu) :
        ((exit_code & 0xffu) << 8);
    *out_result = LPRS_WAIT4_RESULT_PACK(selected_pid, req->status);
    lprs_process_reap(selected);
    return 0;
}

static int lprs_setpgid(void *page)
{
    if (page == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_pid_op_t *req = (lprs_pid_op_t *)page;
    lprs_process_t *caller = lprs_find_by_token(req->token);
    if (caller == NULL) {
        return PACHA_STATUS_ESRCH;
    }
    if (req->pid < 0 || req->value < 0) {
        return PACHA_STATUS_EINVAL;
    }
    const uint64_t target_pid = req->pid == 0 ? caller->pid : (uint64_t)req->pid;
    const uint64_t target_pgrp = req->value == 0 ? target_pid : (uint64_t)req->value;
    lprs_process_t *target = lprs_find_by_pid(target_pid);
    if (target == NULL) {
        return PACHA_STATUS_ESRCH;
    }
    if (target != caller && target->ppid != caller->pid) {
        return PACHA_STATUS_EPERM;
    }
    target->pgrp = target_pgrp;
    req->result = target->pgrp;
    return 0;
}

static int lprs_setsid(void *page)
{
    if (page == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_pid_op_t *req = (lprs_pid_op_t *)page;
    lprs_process_t *caller = lprs_find_by_token(req->token);
    if (caller == NULL) {
        return PACHA_STATUS_ESRCH;
    }
    if (caller->pgrp == caller->pid) {
        return PACHA_STATUS_EPERM;
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
        return PACHA_STATUS_EINVAL;
    }
    lprs_pid_op_t *req = (lprs_pid_op_t *)page;
    lprs_process_t *caller = lprs_find_by_token(req->token);
    if (caller == NULL || req->pid < 0) {
        return caller == NULL ? PACHA_STATUS_ESRCH : PACHA_STATUS_EINVAL;
    }
    const uint64_t target_pid = req->pid == 0 ? caller->pid : (uint64_t)req->pid;
    lprs_process_t *target = lprs_find_by_pid(target_pid);
    if (target == NULL) {
        return PACHA_STATUS_ESRCH;
    }
    req->result = want_sid ? target->sid : target->pgrp;
    return 0;
}

static int lprs_set_pdeathsig(void *page)
{
    if (page == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_pdeathsig_t *req = (lprs_pdeathsig_t *)page;
    lprs_process_t *caller = lprs_find_by_token(req->token);
    if (caller == NULL || req->signal > 64u) {
        return caller == NULL ? PACHA_STATUS_ESRCH : PACHA_STATUS_EINVAL;
    }
    caller->pdeath_signal = (uint32_t)req->signal;
    req->result = caller->pdeath_signal;
    return 0;
}

static int lprs_get_pdeathsig(void *page)
{
    if (page == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_pdeathsig_t *req = (lprs_pdeathsig_t *)page;
    lprs_process_t *caller = lprs_find_by_token(req->token);
    if (caller == NULL) {
        return PACHA_STATUS_ESRCH;
    }
    req->result = caller->pdeath_signal;
    return 0;
}

static int lprs_kill(void *page)
{
    if (page == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_kill_t *req = (lprs_kill_t *)page;
    lprs_process_t *caller = lprs_find_by_token(req->token);
    if (caller == NULL || req->signal > 64u) {
        return caller == NULL ? PACHA_STATUS_ESRCH : PACHA_STATUS_EINVAL;
    }
#if LPRS_SIGNAL_DIAG
    printf("[lprs-signal-diag] op=kill caller=%llu caller_pgrp=%llu pid=%lld signal=%llu\n",
        (unsigned long long)caller->pid,
        (unsigned long long)caller->pgrp,
        (long long)req->pid,
        (unsigned long long)req->signal);
#endif
    uint64_t delivered = 0;
    int first_error = 0;
    for (uint64_t i = 0; i < g_process_count; ++i) {
        lprs_process_t *proc = &g_processes[i];
        if (!proc->active || proc->exit_ready || proc->process_fd < 16) {
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
#if LPRS_SIGNAL_DIAG
        printf("[lprs-signal-diag] op=deliver caller=%llu target=%llu target_pgrp=%llu signal=%llu\n",
            (unsigned long long)caller->pid,
            (unsigned long long)proc->pid,
            (unsigned long long)proc->pgrp,
            (unsigned long long)req->signal);
#endif
        const int status = lprs_signal_process_fd(proc->process_fd, req->signal);
        if (status == 0) {
            if (lprs_signal_interrupts_wait(req->signal)) {
                lprs_interrupt_waiters(proc->token);
            }
            delivered++;
        } else if (first_error == 0) {
            first_error = status;
        }
    }
    req->delivered = delivered;
    if (delivered != 0) {
        return 0;
    }
    return first_error != 0 ? first_error : PACHA_STATUS_ESRCH;
}

static int lprs_deliver_tty_signal_fields(uint64_t pgrp, uint64_t signo, uint64_t *out_delivered)
{
    if (pgrp == 0 || signo == 0 || signo > 64u) {
        return PACHA_STATUS_EINVAL;
    }

    uint64_t delivered = 0;
    int first_error = 0;
    for (uint64_t i = 0; i < g_process_count; ++i) {
        lprs_process_t *proc = &g_processes[i];
        if (!proc->active || proc->exit_ready || proc->process_fd < 16 ||
            proc->pgrp != (uint64_t)pgrp)
        {
            continue;
        }
        const int status = lprs_signal_process_fd(proc->process_fd, signo);
        if (status == 0) {
            if (lprs_signal_interrupts_wait(signo)) {
                lprs_interrupt_waiters(proc->token);
            }
            delivered++;
        } else if (first_error == 0) {
            first_error = status;
        }
    }
    if (out_delivered != NULL) {
        *out_delivered = delivered;
    }
    return delivered != 0 ? 0 : (first_error != 0 ? first_error : PACHA_STATUS_ESRCH);
}

static int lprs_cwd_get(uint64_t token, void *page)
{
    if (page == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_process_t *proc = lprs_find_by_token(token);
    if (proc == NULL) {
        return PACHA_STATUS_ESRCH;
    }
    lprs_cwd_t *cwd = (lprs_cwd_t *)page;
    memset(cwd, 0, sizeof(*cwd));
    cwd->token = proc->token;
    cwd->cwd_handle = proc->cwd_handle;
    lprs_copy_string(cwd->cwd, sizeof(cwd->cwd), proc->cwd);
    return 0;
}

static int lprs_cwd_set(void *page)
{
    if (page == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    lprs_cwd_t *cwd = (lprs_cwd_t *)page;
    lprs_process_t *proc = lprs_find_by_token(cwd->token);
    if (proc == NULL) {
        return PACHA_STATUS_ESRCH;
    }
    proc->cwd_handle = cwd->cwd_handle;
    lprs_copy_string(proc->cwd, sizeof(proc->cwd), cwd->cwd);
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

static uint64_t lprs_token_from_payload(const void *payload, uint32_t payload_size)
{
    if (payload == NULL || payload_size < sizeof(lprs_token_request_t)) {
        return 0;
    }
    return ((const lprs_token_request_t *)payload)->token;
}

static int lprs_dispatch(
    struct pacha_ipc_msg *request,
    uint64_t actor,
    int bootstrap,
    uint64_t *out_result,
    int *out_keep_fd,
    lprs_reply_cap_t *out_reply,
    uint64_t *out_error_token,
    uint64_t *out_request_id)
{
    if (request == NULL || out_result == NULL || out_keep_fd == NULL ||
        out_reply == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    *out_result = 0;
    *out_keep_fd = -1;
    *out_reply = (lprs_reply_cap_t){ .fd = -1 };
    if (out_error_token != NULL) {
        *out_error_token = 0;
    }
    if (out_request_id != NULL) {
        *out_request_id = request->word3;
    }

    int page_fd = -1;
    void *mapped_page = lprs_map_request_page(request, &page_fd);
    if (mapped_page == NULL) {
        if (out_error_token != NULL) {
            *out_error_token = lprs_error_token(
                PACHA_STATUS_EFAULT,
                0,
                PACHA_STATUS_STAGE_MAP_PAGE,
                PACHA_STATUS_EFAULT,
                request->word3,
                request->fd_count,
                request->fd_count != 0 ? request->fds[0].fd : 0,
                0,
                "lpr supervisor request page map failed");
        }
        return PACHA_STATUS_EFAULT;
    }

    /* Authorization and execution must see the same request, even when
     * another caller thread rewrites the shared page during dispatch. */
    _Alignas(pacha_service_envelope_t) uint8_t snapshot[PACHA_SERVICE_PAGE_BYTES];
    memcpy(snapshot, mapped_page, sizeof(snapshot));
    void *page = snapshot;
    pacha_service_envelope_t header;
    memcpy(&header, page, sizeof(header));
    if (out_request_id != NULL) {
        *out_request_id = header.request_id;
    }
    int status = PACHA_STATUS_EINVAL;
    uint32_t reply_payload_size = 0;
    if (!pacha_service_request_is_valid(&header, LPRS_SERVICE_ID)) {
        pacha_service_reply_init(
            (pacha_service_envelope_t *)page,
            &header,
            PACHA_STATUS_EINVAL,
            PACHA_SERVICE_ERROR_ABI,
            0,
            0);
        memcpy(mapped_page, page, sizeof(snapshot));
        (void)pacha_munmap(mapped_page, PACHA_SERVICE_PAGE_BYTES);
        return PACHA_STATUS_EINVAL;
    }

    void *payload = (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES;
    const uint64_t token = lprs_token_from_payload(payload, header.payload_size);
    status = lprs_authorize(actor, bootstrap, header.op, token);
    if (status != 0) goto reply;
    switch (header.op) {
    case LPRS_OP_HELLO:
        *out_result = PACHA_SERVICE_ABI_VERSION;
        status = 0;
        break;
    case LPRS_OP_PROCESS_REGISTER_EXEC:
        if (header.payload_size < sizeof(lprs_register_exec_t)) {
            status = PACHA_STATUS_EINVAL;
        } else {
            status = lprs_register_exec(payload, out_result);
            if (status == 0) {
                lprs_process_t *proc = lprs_find_by_token(*out_result);
                status = lprs_prepare_control(proc, out_reply);
                if (status != 0) lprs_process_reap(proc);
            }
            reply_payload_size = sizeof(lprs_register_exec_t);
        }
        break;
    case LPRS_OP_PROCESS_ACTIVATE: {
        lprs_process_t *proc = lprs_find_by_token(actor);
        if (proc->activation_page_fd >= 16) { status = PACHA_STATUS_EAGAIN; break; }
        status = lprs_activate_control(proc, out_reply);
        if (status == PACHA_STATUS_EAGAIN) {
            /* Native fork can schedule the child before the parent has
             * registered its process FD. Hold the activation CALL, rather
             * than making the child poll or exposing user code early. */
            proc->activation_page_fd = page_fd;
            proc->activation_reply_fd = (int)request->fds[request->fd_count - 1u].fd;
            proc->activation_header = header;
            *out_keep_fd = page_fd;
            (void)pacha_munmap(mapped_page, PACHA_SERVICE_PAGE_BYTES);
            return LPRS_DISPATCH_DEFERRED;
        }
        break;
    }
    case LPRS_OP_PROCESS_EXEC_PREPARE:
        status = lprs_prepare_exec_control(lprs_find_by_token(actor), out_reply);
        break;
    case LPRS_OP_PROCESS_UNIX_SESSION:
        status = lprs_unix_session(lprs_find_by_token(actor), out_reply, out_result);
        break;
    case LPRS_OP_PROCESS_FILED_SESSION:
        status = lprs_filed_session(lprs_find_by_token(actor), out_reply, out_result);
        break;
    case LPRS_OP_PROCESS_CREDENTIALS:
        if (header.payload_size != sizeof(lprs_credential_request_t)) status = -22;
        else {
            status = lprs_change_credentials(lprs_find_by_token(actor), payload);
            if (!status) reply_payload_size = sizeof(lprs_credential_request_t);
        }
        break;
    case LPRS_OP_PROCESS_REGISTER_FD:
    case LPRS_OP_PROCESS_FORK_PARENT_REGISTER:
        if (token == 0 || request->fds == NULL || request->fd_count < 2 ||
            request->fds[1].fd < 16)
        {
            status = PACHA_STATUS_EINVAL;
        } else {
            status =
                lprs_register_process_fd_handle(token, (int)(uint32_t)request->fds[1].fd);
            if (status == 0) {
                *out_keep_fd = (int)(uint32_t)request->fds[1].fd;
                lprs_complete_activation(lprs_find_by_token(token));
            }
        }
        break;
    case LPRS_OP_PROCESS_GET_STATE:
        status = token == 0 ? PACHA_STATUS_EINVAL : lprs_get_process_state(token, payload);
        reply_payload_size = status == 0 ? sizeof(lprs_process_state_t) : 0;
        if (status == 0) {
            lprs_process_t *proc = lprs_find_by_token(token);
            if (proc == NULL || proc->process_fd < 16) status = PACHA_STATUS_ESRCH;
            else *out_reply = (lprs_reply_cap_t){ .fd = proc->process_fd,
                .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_TRANSFER |
                    PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE };
        }
        break;
    case LPRS_OP_PROCESS_LIST:
        status = header.payload_size < sizeof(lprs_process_list_t) ?
            PACHA_STATUS_EINVAL : lprs_list_processes(payload);
        reply_payload_size = status == 0 ? sizeof(lprs_process_list_t) : 0;
        break;
    case LPRS_OP_PROCESS_FORK_BEGIN:
        status = token == 0 ? PACHA_STATUS_EINVAL : lprs_fork_begin(token, payload);
        if (status == 0) {
            *out_result = ((lprs_fork_t *)payload)->child_token;
            lprs_process_t *child = lprs_find_by_token(*out_result);
            status = lprs_prepare_control(child, out_reply);
            if (status != 0) lprs_process_reap(child);
            reply_payload_size = sizeof(lprs_fork_t);
        }
        break;
    case LPRS_OP_PROCESS_FORK_CANCEL:
        status = token == 0 ? PACHA_STATUS_EINVAL : lprs_fork_cancel(token);
        break;
    case LPRS_OP_PROCESS_FORK_CHILD_READY:
        status = token == 0 ? PACHA_STATUS_EINVAL :
            (lprs_find_by_token(token) != NULL ? 0 : PACHA_STATUS_ESRCH);
        break;
    case LPRS_OP_PROCESS_EXEC_COMMIT_BEGIN:
        if (token == 0 || request->fds == NULL || request->fd_count < 2 ||
            request->fds[1].fd < 16)
        {
            status = PACHA_STATUS_EINVAL;
        } else {
            status = lprs_exec_commit_begin(
                token, (int)(uint32_t)request->fds[1].fd);
            if (status == 0) {
                *out_keep_fd = (int)(uint32_t)request->fds[1].fd;
                lprs_complete_activation(lprs_find_by_token(token));
            }
        }
        break;
    case LPRS_OP_PROCESS_EXEC_COMMIT_CANCEL:
        status = token == 0 ? PACHA_STATUS_EINVAL :
            lprs_exec_commit_cancel(token);
        break;
    case LPRS_OP_PROCESS_EXEC_COMMIT_DONE:
        status = token == 0 ? PACHA_STATUS_EINVAL :
            lprs_exec_commit_done(token);
        break;
    case LPRS_OP_PROCESS_SET_COMM:
        status = token == 0 ? PACHA_STATUS_EINVAL :
            lprs_set_comm(token, payload, header.payload_size);
        break;
    case LPRS_OP_PROCESS_QUERY:
        status = lprs_query_process(payload, header.payload_size);
        reply_payload_size = status == 0 ? sizeof(lprs_process_query_t) : 0;
        break;
    case LPRS_OP_PROCESS_DIAG_ATTACH:
        if (token == 0 || request->fds == NULL || request->fd_count < 2 ||
            request->fds[1].fd < 16)
        {
            status = PACHA_STATUS_EINVAL;
        } else {
            status = lprs_diag_attach(
                token, (int)(uint32_t)request->fds[1].fd, out_keep_fd);
        }
        break;
    case LPRS_OP_PROCESS_WAIT4:
        if (header.payload_size < sizeof(lprs_wait4_t)) {
            status = PACHA_STATUS_EINVAL;
        } else {
            status = lprs_wait4(payload, out_result);
            reply_payload_size = sizeof(lprs_wait4_t);
            lprs_wait4_t *wait = (lprs_wait4_t *)payload;
            if (status == PACHA_STATUS_EAGAIN &&
                (wait->options & LPRS_WNOHANG) == 0)
            {
                const int reply_fd = request->fd_count != 0 ?
                    (int)(uint32_t)request->fds[request->fd_count - 1u].fd : -1;
                status = lprs_queue_waiter(page_fd, reply_fd, &header, wait);
                if (status == 0) {
                    *out_keep_fd = page_fd;
                    (void)pacha_munmap(mapped_page, PACHA_SERVICE_PAGE_BYTES);
                    return LPRS_DISPATCH_DEFERRED;
                }
            }
        }
        break;
    case LPRS_OP_PROCESS_SETPGID:
        status = header.payload_size < sizeof(lprs_pid_op_t) ?
            PACHA_STATUS_EINVAL :
            lprs_setpgid(payload);
        reply_payload_size = status == 0 ? sizeof(lprs_pid_op_t) : 0;
        break;
    case LPRS_OP_PROCESS_SETSID:
        status = header.payload_size < sizeof(lprs_pid_op_t) ?
            PACHA_STATUS_EINVAL :
            lprs_setsid(payload);
        reply_payload_size = status == 0 ? sizeof(lprs_pid_op_t) : 0;
        break;
    case LPRS_OP_PROCESS_GETPGID:
        status = header.payload_size < sizeof(lprs_pid_op_t) ?
            PACHA_STATUS_EINVAL :
            lprs_getpgid_or_sid(payload, 0);
        reply_payload_size = status == 0 ? sizeof(lprs_pid_op_t) : 0;
        break;
    case LPRS_OP_PROCESS_GETSID:
        status = header.payload_size < sizeof(lprs_pid_op_t) ?
            PACHA_STATUS_EINVAL :
            lprs_getpgid_or_sid(payload, 1);
        reply_payload_size = status == 0 ? sizeof(lprs_pid_op_t) : 0;
        break;
    case LPRS_OP_PROCESS_SET_PDEATHSIG:
        status = header.payload_size < sizeof(lprs_pdeathsig_t) ?
            PACHA_STATUS_EINVAL :
            lprs_set_pdeathsig(payload);
        reply_payload_size = status == 0 ? sizeof(lprs_pdeathsig_t) : 0;
        break;
    case LPRS_OP_PROCESS_GET_PDEATHSIG:
        status = header.payload_size < sizeof(lprs_pdeathsig_t) ?
            PACHA_STATUS_EINVAL :
            lprs_get_pdeathsig(payload);
        reply_payload_size = status == 0 ? sizeof(lprs_pdeathsig_t) : 0;
        break;
    case LPRS_OP_SIGNAL_KILL:
        status = header.payload_size < sizeof(lprs_kill_t) ?
            PACHA_STATUS_EINVAL :
            lprs_kill(payload);
        reply_payload_size = status == 0 ? sizeof(lprs_kill_t) : 0;
        break;
    case LPRS_OP_SIGNAL_DELIVER_TTY:
        if (header.payload_size < sizeof(lprs_tty_signal_t)) {
            status = PACHA_STATUS_EINVAL;
        } else {
            lprs_tty_signal_t *sig = (lprs_tty_signal_t *)payload;
            status = lprs_deliver_tty_signal_fields(sig->pgrp, sig->signal, &sig->delivered);
            reply_payload_size = sizeof(*sig);
        }
        break;
    case LPRS_OP_CWD_GET:
        status = token == 0 ? PACHA_STATUS_EINVAL : lprs_cwd_get(token, payload);
        reply_payload_size = status == 0 ? sizeof(lprs_cwd_t) : 0;
        break;
    case LPRS_OP_CWD_SET:
        status = header.payload_size < sizeof(lprs_cwd_t) ? PACHA_STATUS_EINVAL : lprs_cwd_set(payload);
        break;
    case LPRS_OP_DIAG_ERROR_GET:
        status = PACHA_STATUS_ENOTSUP;
        reply_payload_size = 0;
        break;
    case LPRS_OP_DIAG_DUMP:
    default:
        status = PACHA_STATUS_EINVAL;
        break;
    }

reply:
    if (status < 0 && header.op != LPRS_OP_DIAG_ERROR_GET &&
        out_error_token != NULL && *out_error_token == 0)
    {
        *out_error_token = lprs_error_token(
            status,
            header.op,
            PACHA_STATUS_STAGE_DISPATCH,
            status,
            header.request_id,
            request->fd_count,
            token,
            0,
            "lpr supervisor dispatch failed");
    }
    pacha_service_reply_init(
        (pacha_service_envelope_t *)page,
        &header,
        status,
        status == PACHA_STATUS_EINVAL ? PACHA_SERVICE_ERROR_ABI : PACHA_SERVICE_ERROR_LPR_TRANSLATION,
        *out_result,
        reply_payload_size);
    memcpy(mapped_page, page, sizeof(snapshot));
    (void)pacha_munmap(mapped_page, PACHA_SERVICE_PAGE_BYTES);
    (void)page_fd;
    return status;
}

static int lprs_reply(
    int reply_fd,
    uint64_t request_id,
    int64_t status,
    uint64_t result,
    const lprs_reply_cap_t *cap,
    uint64_t error_token)
{
    struct pacha_ipc_fd transferred = {
        .fd = cap ? (uint64_t)(uint32_t)cap->fd : 0,
        .rights = cap ? cap->rights : 0,
        .transfer_flags = cap ? cap->transfer_flags : 0,
    };
    const struct pacha_ipc_msg reply = {
        .word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = status < 0 ? 0 : result,
        .word3 = request_id,
        .fds = status == 0 && cap && cap->fd >= 16 ? &transferred : NULL,
        .fd_count = status == 0 && cap && cap->fd >= 16 ? 1u : 0u,
    };
    (void)error_token;
    const int reply_status = pacha_ipc_reply(reply_fd, &reply);
    if (cap && cap->fd >= 16 && (cap->transfer_flags & PACHA_IPC_TRANSFER_MOVE))
        (void)pacha_fd_close(cap->fd);
    (void)pacha_fd_close(reply_fd);
    return reply_status;
}

static void lprs_complete_activation(lprs_process_t *proc)
{
    if (!proc || proc->activation_page_fd < 16) return;
    void *page = pacha_mmap(proc->activation_page_fd, PACHA_SERVICE_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    lprs_reply_cap_t cap = { .fd = -1 };
    const int status = page ? lprs_activate_control(proc, &cap) : PACHA_STATUS_EFAULT;
    if (page) {
        pacha_service_reply_init(page, &proc->activation_header, status,
            PACHA_SERVICE_ERROR_LPR_TRANSLATION, 0, 0);
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
    }
    const int page_fd = proc->activation_page_fd;
    const int reply_fd = proc->activation_reply_fd;
    const uint64_t request = proc->activation_header.request_id;
    proc->activation_page_fd = proc->activation_reply_fd = -1;
    (void)pacha_fd_close(page_fd);
    (void)lprs_reply(reply_fd, request, status, 0, &cap, 0);
}

static void lprs_finish_waiter(lprs_waiter_t *waiter, int status, uint64_t result)
{
    if (waiter == NULL || !waiter->active) {
        return;
    }
    void *page = pacha_mmap(
        waiter->page_fd,
        PACHA_SERVICE_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    int reply_status = status;
    if (page == NULL) {
        reply_status = PACHA_STATUS_EFAULT;
    } else {
        memcpy(
            (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES,
            &waiter->request,
            sizeof(waiter->request));
        pacha_service_reply_init(
            (pacha_service_envelope_t *)page,
            &waiter->header,
            reply_status,
            reply_status == PACHA_STATUS_EINVAL ?
                PACHA_SERVICE_ERROR_ABI : PACHA_SERVICE_ERROR_LPR_TRANSLATION,
            result,
            sizeof(waiter->request));
        (void)pacha_munmap(page, PACHA_SERVICE_PAGE_BYTES);
    }

    const int page_fd = waiter->page_fd;
    const int reply_fd = waiter->reply_fd;
    const uint64_t request_id = waiter->header.request_id;
    memset(waiter, 0, sizeof(*waiter));
    waiter->page_fd = -1;
    waiter->reply_fd = -1;
    (void)pacha_fd_close(page_fd);
    (void)lprs_reply(reply_fd, request_id, reply_status, result, NULL, 0);
}

static void lprs_interrupt_waiters(uint64_t token)
{
    for (uint64_t i = 0; i < g_waiter_count; ++i) {
        lprs_waiter_t *waiter = &g_waiters[i];
        if (waiter->active && waiter->request.token == token) {
            lprs_finish_waiter(waiter, -PACHA_LINUX_EINTR, 0);
        }
    }
}

static void lprs_complete_waiters(void)
{
    for (uint64_t i = 0; i < g_waiter_count; ++i) {
        lprs_waiter_t *waiter = &g_waiters[i];
        if (!waiter->active) {
            continue;
        }
        uint64_t result = 0;
        const int status = lprs_wait4(&waiter->request, &result);
        if (status == PACHA_STATUS_EAGAIN) {
            continue;
        }
        lprs_finish_waiter(waiter, status, result);
    }
}

static int lprs_handle_received_request(struct pacha_ipc_msg *request, uint64_t actor, int bootstrap)
{
    if (request == NULL) {
        return PACHA_STATUS_EINVAL;
    }
    const int reply_fd =
        request->fd_count != 0 ? (int)(uint32_t)request->fds[request->fd_count - 1u].fd : -1;
    if (reply_fd < 16) {
        lprs_close_unowned_fds(request, -1, -1);
        return PACHA_STATUS_EINVAL;
    }
    int keep_fd = -1;
    lprs_reply_cap_t transferred = { .fd = -1 };
    uint64_t result = 0;
    uint64_t error_token = 0;
    uint64_t request_id = request->word3;
    const int dispatch_status =
        request->word0 == PACHA_SERVICE_REQUEST_MAGIC ?
            lprs_dispatch(
                request, actor, bootstrap, &result, &keep_fd, &transferred,
                &error_token, &request_id) :
            PACHA_STATUS_EINVAL;
    lprs_close_unowned_fds(request, keep_fd, reply_fd);
    if (dispatch_status == LPRS_DISPATCH_DEFERRED) {
        return 0;
    }
    (void)lprs_reply(
        reply_fd, request_id, dispatch_status, result, &transferred, error_token);
    return 0;
}

static int lprs_service_one_pending_request(int endpoint, uint64_t actor, int bootstrap)
{
    struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS];
    struct pacha_ipc_msg request;
    memset(fds, 0, sizeof(fds));
    memset(&request, 0, sizeof(request));
    request.fds = fds;
    request.fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS;
    const int status = pacha_ipc_recv(endpoint, &request);
    if (status != 0) {
        /* A request the descriptor table cannot hold is left at the head of
         * the queue, so the endpoint stays readable and nothing behind it is
         * ever received.  The service then spins and merely looks idle. */
        if (status == PACHA_ERR_ALLOC) {
            static int reported;
            if (!reported) {
                reported = 1;
                fprintf(stderr,
                    "[lpr-supervisor] recv blocked: descriptor table full, endpoint stalled\n");
                fflush(stderr);
            }
        }
        return status == PACHA_ERR_EMPTY || status == PACHA_ERR_NOT_READY ? PACHA_STATUS_EAGAIN : status;
    }
    return lprs_handle_received_request(&request, actor, bootstrap);
}

static void lprs_notify_exited_child(
    lprs_process_t *child,
    uint64_t exit_state,
    uint64_t exit_code)
{
    if (child == NULL || !child->active || child->exit_ready ||
        child->process_fd < 16)
    {
        return;
    }
    lprs_discard_pending_exec(child);
    child->exit_status = (uint32_t)(exit_code & 0xffu);
    child->exit_state = (uint32_t)exit_state;
    child->exit_ready = 1;
    lprs_unix_release(child);
    lprs_orphan_children(child->pid);
    if (child->ppid == 0) {
        lprs_process_reap(child);
        return;
    }
    lprs_process_t *parent = lprs_find_by_pid(child->ppid);
    if (parent == NULL || parent->exit_ready) {
        lprs_process_reap(child);
        return;
    }
    int notify_status = PACHA_STATUS_ESRCH;
    if (parent->child_sequence != UINT64_MAX)
        parent->child_sequence++;
    if (parent->process_fd >= 16) {
        notify_status = lprs_signal_process_fd(parent->process_fd, LPRS_SIGCHLD);
        if (notify_status == 0) {
            child->exit_notified = 1;
        }
    }
}

static void lprs_refresh_exited_children(void)
{
    for (uint64_t i = 0; i < g_process_count; ++i) {
        lprs_process_t *child = &g_processes[i];
        if (!child->active || child->exit_ready || child->process_fd < 16)
        {
            continue;
        }
        uint64_t exit_state = 0;
        uint64_t exit_code = 0;
        const int status = lprs_try_wait_process_fd(
            child->process_fd, &exit_state, &exit_code);
        if (status == 0) {
            lprs_notify_exited_child(child, exit_state, exit_code);
        }
    }
}

static void lprs_refresh_initial_execs(void)
{
    for (uint64_t i = 0; i < g_process_count; ++i) {
        lprs_process_t *proc = &g_processes[i];
        if (!proc->active || proc->process_fd >= 16 ||
            proc->pending_exec_fd < 16) continue;
        uint64_t exit_state = 0;
        uint64_t exit_code = 0;
        if (lprs_try_wait_process_fd(
                proc->pending_exec_fd, &exit_state, &exit_code) != 0)
        {
            continue;
        }
        proc->process_fd = proc->pending_exec_fd;
        proc->pending_exec_fd = -1;
        if (proc->process_fd >= 16 && proc->exit_ready == 0 &&
            lprs_try_wait_process_fd(
                proc->process_fd, &exit_state, &exit_code) == 0)
        {
            lprs_notify_exited_child(proc, exit_state, exit_code);
        }
    }
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
    (void)pacha_fd_close(bootstrap_fd);
    /* Launch grants install the final client-only, private capability. */
    if (cfg.unix_admin_fd < 16 || cfg.unix_admin_fd >= PACHA_FD_TABLE_LIMIT) return 1;
    g_unix_admin_fd = (int)cfg.unix_admin_fd;
    g_filed_admin_fd = (int)cfg.filed_admin_fd;
    status = lprs_accounts_load(g_filed_admin_fd, &g_accounts);
    if (status) {
        fprintf(stderr, "[lprs] account definitions rejected status=%d\n", status);
        return 1;
    }
    g_accounts_ready = 1;
    fprintf(stderr, "[lprs] account definitions ready\n");
    struct unix_control hello = { .operation = UNIX_OP_HELLO, .request = 1 };
    unsigned received = 0;
    status = unix_client_call(g_unix_admin_fd, &hello, NULL, 0, NULL, 0, &received);
    if (status != 0 || hello.result != UNIX_SERVICE_VERSION) {
        fprintf(stderr, "[lprs] unixd bootstrap failed status=%d\n", status);
        return 1;
    }
    /* Keep the admin capability here. Session issuance will use the
     * authenticated per-process control channels, never a supplied token
     * on the administrative endpoint as proof of process identity. */

    for (;;) {
        struct pacha_pollfd pollfds[LPRS_WAIT_FD_CAPACITY];
        uint64_t process_indices[LPRS_WAIT_FD_CAPACITY];
        uint8_t pending_exec[LPRS_WAIT_FD_CAPACITY];
        memset(pollfds, 0, sizeof(pollfds));
        memset(pending_exec, 0, sizeof(pending_exec));
        for (uint64_t i = 0; i < LPRS_WAIT_FD_CAPACITY; ++i) {
            process_indices[i] = UINT64_MAX;
        }
        uint64_t count = 1;
        uint8_t wait_set_overflow = 0;
        pollfds[0] = (struct pacha_pollfd){
            .fd = g_endpoint_fd,
            .events = PACHA_FD_EVENT_READABLE,
        };
        for (uint64_t i = 0; i < g_process_count; ++i) {
            const lprs_process_t *proc = &g_processes[i];
            if (!proc->active || proc->exit_ready) continue;
            const int controls[] = { proc->control_fd, proc->bootstrap_server_fd };
            for (unsigned which = 0; which < 2; which++) {
                if (controls[which] < 16) continue;
                if (count == LPRS_WAIT_FD_CAPACITY) { wait_set_overflow = 1; continue; }
                pollfds[count] = (struct pacha_pollfd){ .fd = controls[which],
                    .events = PACHA_FD_EVENT_READABLE };
                process_indices[count] = i;
                pending_exec[count] = (uint8_t)(2 + which);
                count++;
            }
        }
        for (uint64_t i = 0; i < g_process_count; ++i) {
            const lprs_process_t *proc = &g_processes[i];
            if (!proc->active || proc->exit_ready || proc->process_fd < 16)
            {
                continue;
            }
            if (count == LPRS_WAIT_FD_CAPACITY) {
                wait_set_overflow = 1;
                continue;
            }
            pollfds[count] = (struct pacha_pollfd){
                .fd = proc->process_fd,
                .events = PACHA_FD_EVENT_READABLE,
            };
            process_indices[count] = i;
            count++;
        }
        for (uint64_t i = 0; i < g_process_count; ++i) {
            const lprs_process_t *proc = &g_processes[i];
            if (!proc->active || proc->process_fd >= 16 ||
                proc->pending_exec_fd < 16) continue;
            if (count == LPRS_WAIT_FD_CAPACITY) {
                wait_set_overflow = 1;
                continue;
            }
            pollfds[count] = (struct pacha_pollfd){
                .fd = proc->pending_exec_fd,
                .events = PACHA_FD_EVENT_READABLE,
            };
            process_indices[count] = i;
            pending_exec[count] = 1;
            count++;
        }

        status = (int)pacha_fd_wait_many(pollfds, count, UINT64_MAX);
        if (status < 0) {
            continue;
        }
        if (wait_set_overflow) {
            lprs_refresh_exited_children();
            lprs_refresh_initial_execs();
        }
        for (uint64_t i = 1; i < count; ++i) {
            const uint64_t process_index = process_indices[i];
            if ((pollfds[i].revents & PACHA_FD_EVENT_READABLE) == 0 ||
                process_index >= g_process_count)
            {
                continue;
            }
            lprs_process_t *proc = &g_processes[process_index];
            if (pending_exec[i] >= 2) {
                const uint64_t actor = proc->token;
                const int bootstrap = pending_exec[i] == 3;
                const int endpoint = (int)pollfds[i].fd;
                for (unsigned n = 0; n < 8; n++) {
                    /* Dispatch can reallocate/reap the process table and
                     * ACTIVATE closes this very bootstrap channel. */
                    proc = lprs_find_by_token(actor);
                    if (!proc || (bootstrap ? proc->bootstrap_server_fd : proc->control_fd) != endpoint ||
                        lprs_service_one_pending_request(endpoint, actor, bootstrap) != 0) break;
                }
                continue;
            }
            if (pending_exec[i]) {
                if (!proc->active || proc->process_fd >= 16 ||
                    proc->pending_exec_fd != pollfds[i].fd)
                {
                    continue;
                }
                uint64_t exit_state = 0;
                uint64_t exit_code = 0;
                if (lprs_try_wait_process_fd(
                        proc->pending_exec_fd, &exit_state, &exit_code) != 0)
                {
                    continue;
                }
                proc->process_fd = proc->pending_exec_fd;
                proc->pending_exec_fd = -1;
                if (proc->exit_ready == 0 &&
                    lprs_try_wait_process_fd(
                        proc->process_fd, &exit_state, &exit_code) == 0)
                {
                    lprs_notify_exited_child(proc, exit_state, exit_code);
                }
                continue;
            }
            if (!proc->active || proc->exit_ready ||
                proc->process_fd != pollfds[i].fd)
            {
                continue;
            }
            uint64_t exit_state = 0;
            uint64_t exit_code = 0;
            if (lprs_try_wait_process_fd(
                    proc->process_fd, &exit_state, &exit_code) == 0)
            {
                lprs_notify_exited_child(proc, exit_state, exit_code);
            }
        }
        lprs_complete_waiters();
        if ((pollfds[0].revents & PACHA_FD_EVENT_READABLE) != 0) {
            for (unsigned n = 0; n < 8 && lprs_service_one_pending_request(g_endpoint_fd, 0, 0) == 0; n++) {}
            lprs_complete_waiters();
        }
    }
}
