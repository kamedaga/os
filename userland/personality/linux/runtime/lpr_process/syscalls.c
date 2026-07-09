#include "../lpr_filed_internal.h"

enum {
    LPR_CLONE_VM = 0x00000100ull,
    LPR_CLONE_FS = 0x00000200ull,
    LPR_CLONE_FILES = 0x00000400ull,
    LPR_CLONE_SIGHAND = 0x00000800ull,
    LPR_CLONE_VFORK = 0x00004000ull,
    LPR_CLONE_THREAD = 0x00010000ull,
    LPR_CLONE_SYSVSEM = 0x00040000ull,
    LPR_CLONE_SETTLS = 0x00080000ull,
    LPR_CLONE_PARENT_SETTID = 0x00100000ull,
    LPR_CLONE_CHILD_CLEARTID = 0x00200000ull,
    LPR_CLONE_DETACHED = 0x00400000ull,
    LPR_CLONE_CHILD_SETTID = 0x01000000ull,
};

_Static_assert(sizeof(lpr_thread_record_t) == 64u, "thread launch record size");

static void lpr_thread_record_add(lpr_thread_record_t *record)
{
    lpr_state_lock(&lpr_state.threads.lock_word);
    record->next = lpr_state.threads.head;
    lpr_state.threads.head = record;
    lpr_state_unlock(&lpr_state.threads.lock_word);
}

static int lpr_thread_record_remove(lpr_thread_record_t *record)
{
    lpr_thread_record_t **cursor = &lpr_state.threads.head;
    while (*cursor != 0 && *cursor != record) {
        cursor = &(*cursor)->next;
    }
    if (*cursor == record) {
        *cursor = record->next;
        record->next = 0;
    }
    return lpr_state.threads.head == 0;
}

static lpr_thread_record_t *lpr_thread_record_find(uint32_t tid)
{
    for (lpr_thread_record_t *record = lpr_state.threads.head;
         record != 0;
         record = record->next)
    {
        if (record->tid == tid) {
            return record;
        }
    }
    return 0;
}

static int lpr_thread_ensure_current_record(void)
{
    const int64_t raw_tid = lpr_linux_gettid();
    if (raw_tid < 0 || raw_tid > UINT32_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    const uint32_t tid = (uint32_t)raw_tid;
    lpr_state_lock(&lpr_state.threads.lock_word);
    if (lpr_thread_record_find(tid) == 0) {
        lpr_thread_record_t *record = &lpr_state.threads.main_thread;
        if (record->started != 0u && record->tid != tid) {
            lpr_state_unlock(&lpr_state.threads.lock_word);
            return -LPR_LINUX_EINVAL;
        }
        lpr_memset(record, 0, sizeof(*record));
        record->tid = tid;
        record->started = 1;
        record->parent_ready = 1;
        record->next = lpr_state.threads.head;
        lpr_state.threads.head = record;
    }
    lpr_state_unlock(&lpr_state.threads.lock_word);
    return 0;
}

static void lpr_thread_count_start(void)
{
    (void)__atomic_add_fetch(&lpr_state.thread_count, 1u, __ATOMIC_ACQ_REL);
}

static void lpr_thread_count_start_failed(void)
{
    (void)__atomic_sub_fetch(&lpr_state.thread_count, 1u, __ATOMIC_ACQ_REL);
}

static void lpr_thread_after_fork_child(void)
{
    lpr_memset(&lpr_state.threads, 0, sizeof(lpr_state.threads));
    __atomic_store_n(&lpr_state.thread_count, 1u, __ATOMIC_RELEASE);
}

int64_t lpr_linux_gettid(void)
{
    return lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID);
}

int64_t lpr_linux_set_tid_address(uint64_t tid_address)
{
    const int64_t raw_tid = lpr_linux_gettid();
    if (raw_tid < 0 || raw_tid > UINT32_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    const uint32_t tid = (uint32_t)raw_tid;
    const int ensure_status = lpr_thread_ensure_current_record();
    if (ensure_status != 0) {
        return ensure_status;
    }
    lpr_state_lock(&lpr_state.threads.lock_word);
    lpr_thread_record_t *record = lpr_thread_record_find(tid);
    if (record == 0) {
        lpr_state_unlock(&lpr_state.threads.lock_word);
        return -LPR_LINUX_EINVAL;
    }
    record->child_tid = (volatile uint32_t *)(uintptr_t)tid_address;
    lpr_state_unlock(&lpr_state.threads.lock_word);
    return raw_tid;
}

void lpr_linux_exit_thread(uint64_t code)
{
    const int64_t raw_tid = lpr_linux_gettid();
    volatile uint32_t *clear_tid = 0;
    int last_thread = 0;
    lpr_state_lock(&lpr_state.threads.lock_word);
    lpr_thread_record_t *record = raw_tid >= 0 && raw_tid <= UINT32_MAX ?
        lpr_thread_record_find((uint32_t)raw_tid) : 0;
    if (record != 0) {
        clear_tid = record->child_tid;
        last_thread = lpr_thread_record_remove(record);
    } else {
        last_thread = lpr_state.threads.head == 0;
    }
    lpr_state_unlock(&lpr_state.threads.lock_word);

    if (last_thread) {
        lpr_linux_prepare_process_exit(code);
    }
    (void)lpr_pacha_syscall3(
        PACHAOS_SYSCALL_THREAD_EXIT,
        code,
        (uint64_t)(uintptr_t)clear_tid,
        clear_tid != 0 ? PACHAOS_THREAD_EXIT_CLEAR_TID : 0u);
    for (;;) {
    }
}

void lpr_linux_exit_group(uint64_t code)
{
    lpr_linux_prepare_process_exit(code);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, code);
    for (;;) {
    }
}

void lpr_clone_thread_bootstrap(lpr_thread_record_t *record)
{
    const int64_t raw_tid = lpr_linux_gettid();
    if (record == 0 || raw_tid < 0 || raw_tid > UINT32_MAX) {
        lpr_linux_exit_thread(127u);
    }
    const uint32_t tid = (uint32_t)raw_tid;
    record->tid = tid;
    if ((record->clone_flags & LPR_CLONE_PARENT_SETTID) != 0) {
        __atomic_store_n(record->parent_tid, tid, __ATOMIC_RELEASE);
    }
    if ((record->clone_flags & LPR_CLONE_CHILD_SETTID) != 0) {
        __atomic_store_n(record->child_tid, tid, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&record->started, 1u, __ATOMIC_RELEASE);
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_FUTEX_WAKE,
        (uint64_t)(uintptr_t)&record->started,
        1u);
    while (__atomic_load_n(&record->parent_ready, __ATOMIC_ACQUIRE) == 0u) {
        (void)lpr_pacha_syscall3(
            PACHAOS_SYSCALL_FUTEX_WAIT,
            (uint64_t)(uintptr_t)&record->parent_ready,
            0u,
            0u);
    }
    int (*start_function)(void *) =
        (int (*)(void *))(uintptr_t)record->start_function;
    const int result = start_function((void *)(uintptr_t)record->start_argument);
    lpr_linux_exit_thread((uint64_t)(uint32_t)result);
}

static int64_t lpr_linux_clone_thread(
    const struct lpr_linux_user_frame *user_frame,
    uint64_t flags,
    uint64_t child_stack,
    uint64_t parent_tid,
    uint64_t child_tid,
    uint64_t tls)
{
    const uint64_t required_flags =
        LPR_CLONE_VM | LPR_CLONE_FS | LPR_CLONE_FILES |
        LPR_CLONE_SIGHAND | LPR_CLONE_THREAD | LPR_CLONE_SETTLS;
    const uint64_t allowed_flags =
        required_flags | LPR_CLONE_SYSVSEM | LPR_CLONE_PARENT_SETTID |
        LPR_CLONE_CHILD_SETTID | LPR_CLONE_CHILD_CLEARTID | LPR_CLONE_DETACHED;
    if (user_frame == 0 || (flags & 0xffu) != 0 ||
        (flags & required_flags) != required_flags ||
        (flags & ~allowed_flags) != 0 || child_stack < sizeof(lpr_thread_record_t) ||
        user_frame->r9 == 0 || tls == 0 ||
        (((flags & LPR_CLONE_PARENT_SETTID) != 0) && parent_tid == 0) ||
        (((flags & (LPR_CLONE_CHILD_SETTID | LPR_CLONE_CHILD_CLEARTID)) != 0) && child_tid == 0))
    {
        return -LPR_LINUX_EINVAL;
    }
    const int ensure_status = lpr_thread_ensure_current_record();
    if (ensure_status != 0) {
        return ensure_status;
    }

    const uint64_t launch_stack =
        (child_stack - sizeof(lpr_thread_record_t)) & ~0xfull;
    lpr_thread_record_t *record =
        (lpr_thread_record_t *)(uintptr_t)launch_stack;
    lpr_memset(record, 0, sizeof(*record));
    record->start_function = user_frame->r9;
    record->start_argument = *(const uint64_t *)(uintptr_t)child_stack;
    record->clone_flags = flags;
    record->parent_tid = (volatile uint32_t *)(uintptr_t)parent_tid;
    if ((flags & (LPR_CLONE_CHILD_SETTID | LPR_CLONE_CHILD_CLEARTID)) != 0) {
        record->child_tid = (volatile uint32_t *)(uintptr_t)child_tid;
    }

    const uint64_t thread_rights =
        PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_KILL | PACHA_FD_RIGHT_START;
    const int64_t thread_fd = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_THREAD_CREATE,
        PACHAOS_PROCESS_SELF_FD,
        (uint64_t)(uintptr_t)lpr_clone_thread_entry,
        launch_stack,
        0,
        tls,
        thread_rights);
    if (thread_fd < 16) {
        return lpr_pacha_status_to_errno(thread_fd);
    }

    lpr_thread_record_add(record);
    lpr_thread_count_start();
    const int64_t start_status = lpr_pacha_syscall1(
        PACHAOS_SYSCALL_THREAD_START,
        (uint64_t)(uint32_t)thread_fd);
    if (start_status != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_THREAD_KILL,
            (uint64_t)(uint32_t)thread_fd,
            1u);
        lpr_state_lock(&lpr_state.threads.lock_word);
        (void)lpr_thread_record_remove(record);
        lpr_state_unlock(&lpr_state.threads.lock_word);
        lpr_thread_count_start_failed();
        (void)lpr_pacha_syscall1(
            PACHAOS_SYSCALL_FD_CLOSE,
            (uint64_t)(uint32_t)thread_fd);
        return lpr_pacha_status_to_errno(start_status);
    }
    (void)lpr_pacha_syscall1(
        PACHAOS_SYSCALL_FD_CLOSE,
        (uint64_t)(uint32_t)thread_fd);

    while (__atomic_load_n(&record->started, __ATOMIC_ACQUIRE) == 0u) {
        (void)lpr_pacha_syscall3(
            PACHAOS_SYSCALL_FUTEX_WAIT,
            (uint64_t)(uintptr_t)&record->started,
            0u,
            0u);
    }
    const uint32_t tid = record->tid;
    __atomic_store_n(&record->parent_ready, 1u, __ATOMIC_RELEASE);
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_FUTEX_WAKE,
        (uint64_t)(uintptr_t)&record->parent_ready,
        1u);
    return tid;
}

int64_t lpr_linux_clone(uint64_t flags, uint64_t child_stack, uint64_t parent_tid, uint64_t child_tid, uint64_t tls)
{
    return lpr_linux_clone_frame(
        lpr_current_linux_user_frame(),
        flags,
        child_stack,
        parent_tid,
        child_tid,
        tls);
}

int64_t lpr_linux_clone_frame(const struct lpr_linux_user_frame *user_frame, uint64_t flags, uint64_t child_stack, uint64_t parent_tid, uint64_t child_tid, uint64_t tls)
{
    lpr_trace_clone_args(flags, child_stack, parent_tid, child_tid);
    const uint64_t signal = flags & 0xffu;
    const uint64_t known_process_flags = LPR_CLONE_VM | LPR_CLONE_VFORK | LPR_CLONE_PARENT_SETTID | LPR_CLONE_CHILD_SETTID | LPR_CLONE_CHILD_CLEARTID;
    if ((flags & LPR_CLONE_THREAD) != 0) {
        return lpr_linux_clone_thread(user_frame, flags, child_stack, parent_tid, child_tid, tls);
    }
    if (signal != 0 && signal != 17u) {
        return -LPR_LINUX_EINVAL;
    }
    if ((flags & ~(known_process_flags | 0xffull)) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (user_frame == 0) {
        return -LPR_LINUX_ENOSYS;
    }
    struct lpr_linux_user_frame child_frame;
    lpr_memcpy(&child_frame, user_frame, sizeof(child_frame));
    lpr_linux_process_state_init();
    int32_t child_pid = 0;
    uint64_t child_token = 0;
    if (lpr_supervisor_enabled) {
        void *page = 0;
        const int page_fd = lpr_create_standalone_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
        lprs_v2_fork_t *fork_req = (lprs_v2_fork_t *)lpr_supervisor_payload(page);
        fork_req->parent_token = lpr_supervisor_token;
        const int64_t fork_status = lpr_supervisor_call(
            LPRS_V2_OP_PROCESS_FORK_BEGIN,
            page_fd,
            page,
            sizeof(lprs_v2_token_request_t),
            -1,
            0);
        if (fork_status == 0 &&
            fork_req->child_pid != 0 &&
            fork_req->child_pid <= INT32_MAX &&
            fork_req->child_token != 0)
        {
            child_pid = (int32_t)fork_req->child_pid;
            child_token = fork_req->child_token;
        }
        lpr_destroy_standalone_wire_page(page_fd, page);
        if (fork_status != 0) {
            return fork_status;
        }
        if (child_pid <= 0 || child_token == 0) {
            return -LPR_LINUX_EIO;
        }
    } else {
        child_pid = lpr_linux_alloc_child_pid();
        if (child_pid <= 0) {
            return -LPR_LINUX_EAGAIN;
        }
    }
    lpr_linux_pending_child_pid = child_pid;
    lpr_linux_pending_child_ppid = lpr_linux_current_pid;
    lpr_linux_pending_child_sid = lpr_linux_current_sid;
    lpr_linux_pending_child_pgrp = lpr_linux_current_pgrp;
    lpr_supervisor_pending_child_token = child_token;
    lpr_trace_clone_frame("before_drop", &child_frame, 0);
    lpr_filed_session_drop();
    lpr_trace_clone_frame("before_syscall", &child_frame, 0);
    uint64_t child_process_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_KILL;
    if (lpr_supervisor_enabled) {
        child_process_rights |= PACHA_FD_RIGHT_TRANSFER;
    }
    const int64_t ret = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_PROCESS_CLONE,
        child_process_rights,
        PACHA_PROCESS_CLONE_CURRENT_THREAD | PACHA_PROCESS_CLONE_USER_FRAME,
        (uint64_t)(uintptr_t)&child_frame);
    lpr_trace_clone_frame("after_syscall", &child_frame, ret);
    if (ret == 0) {
        lpr_trace_process_event("clone_child", flags, child_stack, 0);
        lpr_linux_process_state_checked = 1;
        lpr_linux_current_pid = lpr_linux_pending_child_pid;
        lpr_linux_current_ppid = lpr_linux_pending_child_ppid;
        lpr_linux_current_sid = lpr_linux_pending_child_sid;
        lpr_linux_current_pgrp = lpr_linux_pending_child_pgrp;
        if (lpr_supervisor_pending_child_token != 0) {
            lpr_supervisor_token = lpr_supervisor_pending_child_token;
            lpr_supervisor_enabled = 1;
            (void)lpr_supervisor_call_token(
                LPRS_V2_OP_PROCESS_FORK_CHILD_READY,
                lpr_supervisor_token,
                -1,
                0);
        }
        lpr_linux_pending_child_pid = 0;
        lpr_linux_pending_child_ppid = 0;
        lpr_linux_pending_child_sid = 0;
        lpr_linux_pending_child_pgrp = 0;
        lpr_supervisor_pending_child_token = 0;
        lpr_linux_process_clear_children();
        lpr_thread_after_fork_child();
        lpr_pipe_after_fork_child();
        return 0;
    }
    if (ret >= 16) {
        lpr_trace_process_event("clone_parent", flags, child_stack, ret);
        int reg_status = 0;
        if (lpr_supervisor_enabled && child_token != 0) {
            const int64_t supervisor_status = lpr_supervisor_call_token(
                LPRS_V2_OP_PROCESS_FORK_PARENT_REGISTER,
                child_token,
                (int)(uint32_t)ret,
                0);
            reg_status = supervisor_status == 0 ? 0 : (int)supervisor_status;
        } else {
            reg_status = lpr_linux_process_register(
                child_pid,
                lpr_linux_current_pid,
                lpr_linux_current_sid,
                lpr_linux_current_pgrp,
                (int)(uint32_t)ret);
        }
        lpr_linux_pending_child_pid = 0;
        lpr_linux_pending_child_ppid = 0;
        lpr_linux_pending_child_sid = 0;
        lpr_linux_pending_child_pgrp = 0;
        lpr_supervisor_pending_child_token = 0;
        if (reg_status != 0) {
            (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_PROCESS_KILL, (uint64_t)(uint32_t)ret, 1);
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)ret);
            return reg_status;
        }
        if (lpr_supervisor_enabled && child_token != 0) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)ret);
        }
        return (int64_t)child_pid;
    }
    lpr_trace_process_event("clone_error", flags, child_stack, ret);
    lpr_linux_pending_child_pid = 0;
    lpr_linux_pending_child_ppid = 0;
    lpr_linux_pending_child_sid = 0;
    lpr_linux_pending_child_pgrp = 0;
    lpr_supervisor_pending_child_token = 0;
    return lpr_pacha_status_to_errno(ret);
}

int64_t lpr_linux_fork(void)
{
    return lpr_linux_clone(17u, 0, 0, 0, 0);
}

int64_t lpr_linux_vfork(void)
{
    return lpr_linux_clone(0x4000ull | 0x100ull | 17u, 0, 0, 0, 0);
}

int64_t lpr_linux_getpid(void)
{
    lpr_linux_process_state_init();
    return lpr_linux_current_pid;
}

int64_t lpr_linux_getppid(void)
{
    lpr_linux_process_state_init();
    return lpr_linux_current_ppid;
}

int64_t lpr_linux_getpgrp(void)
{
    lpr_linux_process_state_init();
    return lpr_linux_current_pgrp;
}

int64_t lpr_linux_getpgid(uint64_t pid_raw)
{
    lpr_linux_process_state_init();
    const int32_t pid = (int32_t)(int64_t)pid_raw;
    if (pid < 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_supervisor_enabled) {
        void *page = 0;
        const int page_fd = lpr_create_standalone_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
        lprs_v2_pid_op_t *op = (lprs_v2_pid_op_t *)lpr_supervisor_payload(page);
        op->token = lpr_supervisor_token;
        op->pid = pid;
        const int64_t status = lpr_supervisor_call(
            LPRS_V2_OP_PROCESS_GETPGID,
            page_fd,
            page,
            sizeof(*op),
            -1,
            0);
        const int64_t result = status == 0 ? (int64_t)op->result : status;
        lpr_destroy_standalone_wire_page(page_fd, page);
        return result;
    }
    if (pid == 0 || pid == lpr_linux_current_pid) {
        return lpr_linux_current_pgrp;
    }
    lpr_linux_process_entry_t *entry = lpr_linux_process_find(pid);
    return entry != 0 ? entry->linux_pgrp : -LPR_LINUX_ESRCH;
}

int64_t lpr_linux_setpgid(uint64_t pid_raw, uint64_t pgid_raw)
{
    lpr_linux_process_state_init();
    const int32_t pid = (int32_t)(int64_t)pid_raw;
    int32_t pgid = (int32_t)(int64_t)pgid_raw;
    if (pid < 0 || pgid < 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_supervisor_enabled) {
        void *page = 0;
        const int page_fd = lpr_create_standalone_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
        lprs_v2_pid_op_t *op = (lprs_v2_pid_op_t *)lpr_supervisor_payload(page);
        op->token = lpr_supervisor_token;
        op->pid = pid;
        op->value = pgid;
        const int64_t status = lpr_supervisor_call(
            LPRS_V2_OP_PROCESS_SETPGID,
            page_fd,
            page,
            sizeof(*op),
            -1,
            0);
        if (status == 0 && (pid == 0 || pid == lpr_linux_current_pid)) {
            lpr_linux_current_pgrp = (int32_t)op->result;
        }
        lpr_destroy_standalone_wire_page(page_fd, page);
        return status;
    }
    const int32_t target_pid = pid == 0 ? lpr_linux_current_pid : pid;
    if (pgid == 0) {
        pgid = target_pid;
    }
    if (target_pid == lpr_linux_current_pid) {
        lpr_linux_current_pgrp = pgid;
        return 0;
    }
    lpr_linux_process_entry_t *entry = lpr_linux_process_find(target_pid);
    if (entry == 0) {
        return -LPR_LINUX_ESRCH;
    }
    entry->linux_pgrp = pgid;
    return 0;
}

int64_t lpr_linux_setsid(void)
{
    lpr_linux_process_state_init();
    if (lpr_supervisor_enabled) {
        void *page = 0;
        const int page_fd = lpr_create_standalone_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
        lprs_v2_pid_op_t *op = (lprs_v2_pid_op_t *)lpr_supervisor_payload(page);
        op->token = lpr_supervisor_token;
        const int64_t status = lpr_supervisor_call(
            LPRS_V2_OP_PROCESS_SETSID,
            page_fd,
            page,
            sizeof(*op),
            -1,
            0);
        if (status == 0) {
            lpr_linux_current_sid = (int32_t)op->result;
            lpr_linux_current_pgrp = lpr_linux_current_pid;
        }
        lpr_destroy_standalone_wire_page(page_fd, page);
        return status == 0 ? lpr_linux_current_sid : status;
    }
    if (lpr_linux_current_pgrp == lpr_linux_current_pid) {
        return -LPR_LINUX_EPERM;
    }
    lpr_linux_current_sid = lpr_linux_current_pid;
    lpr_linux_current_pgrp = lpr_linux_current_pid;
    return lpr_linux_current_sid;
}

int64_t lpr_linux_getsid(uint64_t pid_raw)
{
    lpr_linux_process_state_init();
    const int32_t pid = (int32_t)(int64_t)pid_raw;
    if (pid < 0) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_supervisor_enabled) {
        void *page = 0;
        const int page_fd = lpr_create_standalone_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
        lprs_v2_pid_op_t *op = (lprs_v2_pid_op_t *)lpr_supervisor_payload(page);
        op->token = lpr_supervisor_token;
        op->pid = pid;
        const int64_t status = lpr_supervisor_call(
            LPRS_V2_OP_PROCESS_GETSID,
            page_fd,
            page,
            sizeof(*op),
            -1,
            0);
        const int64_t result = status == 0 ? (int64_t)op->result : status;
        lpr_destroy_standalone_wire_page(page_fd, page);
        return result;
    }
    if (pid == 0 || pid == lpr_linux_current_pid) {
        return lpr_linux_current_sid;
    }
    lpr_linux_process_entry_t *entry = lpr_linux_process_find(pid);
    return entry != 0 ? entry->linux_sid : -LPR_LINUX_ESRCH;
}

int64_t lpr_linux_kill(uint64_t pid_raw, uint64_t sig_raw)
{
    lpr_linux_process_state_init();
    const int32_t pid = (int32_t)(int64_t)pid_raw;
    const uint32_t sig = (uint32_t)sig_raw;
    if (sig > LPR_LINUX_SIGNAL_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    if (lpr_supervisor_enabled) {
        const int64_t status = lpr_supervisor_kill_pid(pid, sig, 0);
        const int targets_current =
            pid == -1 ||
            pid == 0 ||
            pid == lpr_linux_current_pid ||
            (pid < -1 && lpr_linux_current_pgrp == -pid);
        if (status == 0 && targets_current && sig != 0) {
            const int64_t signal_status = lpr_linux_dispatch_pending_signals();
            return signal_status != 0 ? signal_status : 0;
        }
        return status;
    }
    if (pid == 0) {
        const int64_t status = lpr_linux_signal_pgrp(lpr_linux_current_pgrp, sig);
        if (status == 0) {
            const int64_t signal_status = lpr_linux_dispatch_pending_signals();
            return signal_status != 0 ? signal_status : 0;
        }
        return status;
    }
    if (pid < -1) {
        const int64_t status = lpr_linux_signal_pgrp(-pid, sig);
        if (status == 0) {
            const int64_t signal_status = lpr_linux_dispatch_pending_signals();
            return signal_status != 0 ? signal_status : 0;
        }
        return status;
    }
    if (pid == -1) {
        int delivered = 0;
        if (sig != 0) {
            lpr_linux_queue_signal(sig);
            (void)lpr_linux_dispatch_pending_signals();
        }
        delivered = 1;
        for (uint64_t i = 0; i < LPR_LINUX_PROCESS_TABLE_SIZE; i++) {
            lpr_linux_process_entry_t *entry = &lpr_linux_processes[i];
            if (!entry->active || entry->process_fd < 16) {
                continue;
            }
            if (sig != 0) {
                (void)lpr_linux_signal_process_fd(entry->process_fd, sig);
            }
            delivered = 1;
        }
        return delivered ? 0 : -LPR_LINUX_ESRCH;
    }
    if (pid == lpr_linux_current_pid) {
        if (sig != 0) {
            lpr_linux_queue_signal(sig);
            return lpr_linux_dispatch_pending_signals();
        }
        return 0;
    }
    lpr_linux_process_entry_t *entry = lpr_linux_process_find(pid);
    if (entry == 0) {
        return -LPR_LINUX_ESRCH;
    }
    return sig == 0 ? 0 : lpr_linux_signal_process_fd(entry->process_fd, sig);
}

int64_t lpr_linux_rt_sigaction(uint64_t sig_raw, uint64_t act_raw, uint64_t oldact_raw, uint64_t sigsetsize)
{
    const uint64_t sig = sig_raw;
    if (sig == 0 || sig > LPR_LINUX_SIGNAL_MAX || sigsetsize != 8u) {
        return -LPR_LINUX_EINVAL;
    }
    lpr_linux_sigaction_record_t *slot = &lpr_linux_sigactions[sig];
    if (oldact_raw != 0) {
        lpr_memcpy((void *)(uintptr_t)oldact_raw, slot, sizeof(*slot));
    }
    if (act_raw != 0) {
        lpr_memcpy(slot, (const void *)(uintptr_t)act_raw, sizeof(*slot));
    }
    return 0;
}

int64_t lpr_linux_rt_sigprocmask(uint64_t how, uint64_t set_raw, uint64_t oldset_raw, uint64_t sigsetsize)
{
    if (sigsetsize != 8u) {
        return -LPR_LINUX_EINVAL;
    }
    if (oldset_raw != 0) {
        *(uint64_t *)(uintptr_t)oldset_raw = lpr_linux_signal_mask;
    }
    if (set_raw == 0) {
        return 0;
    }
    const uint64_t set = *(const uint64_t *)(uintptr_t)set_raw;
    switch (how) {
    case LPR_LINUX_SIG_BLOCK:
        lpr_linux_signal_mask |= set;
        lpr_linux_signal_mask &= ~lpr_linux_unblockable_signal_mask();
        return 0;
    case LPR_LINUX_SIG_UNBLOCK:
        lpr_linux_signal_mask &= ~set;
        lpr_linux_signal_mask &= ~lpr_linux_unblockable_signal_mask();
        return lpr_linux_dispatch_pending_signals();
    case LPR_LINUX_SIG_SETMASK:
        lpr_linux_signal_mask = set & ~lpr_linux_unblockable_signal_mask();
        return lpr_linux_dispatch_pending_signals();
    default:
        return -LPR_LINUX_EINVAL;
    }
}

int64_t lpr_linux_wait4(uint64_t pid, uint64_t status_raw, uint64_t options, uint64_t rusage)
{
    (void)rusage;
    if ((options & ~(LPR_LINUX_WNOHANG | LPR_LINUX_WUNTRACED | LPR_LINUX_WCONTINUED)) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    lpr_linux_process_state_init();
    const int32_t requested = (int32_t)(int64_t)pid;
    lpr_trace_process_event("wait4_request", (uint64_t)(uint32_t)requested, options, 0);
    if (lpr_supervisor_enabled) {
        void *page = 0;
        const int page_fd = lpr_create_standalone_wire_page(&page);
        if (page_fd < 0) {
            return page_fd;
        }
        lpr_memset(page, 0, PACHA_SERVICE_PAGE_BYTES);
        lprs_v2_wait4_t *wait_req = (lprs_v2_wait4_t *)lpr_supervisor_payload(page);
        wait_req->token = lpr_supervisor_token;
        wait_req->requested_pid = requested;
        wait_req->options = options & LPR_LINUX_WNOHANG;
        uint64_t packed_result = 0;
        const int64_t wait_status = lpr_supervisor_call(
            LPRS_V2_OP_PROCESS_WAIT4,
            page_fd,
            page,
            sizeof(*wait_req),
            -1,
            &packed_result);
        int64_t result = 0;
        if (wait_status == 0) {
            const uint32_t result_pid = LPRS_V2_WAIT4_RESULT_PID(packed_result);
            const uint32_t wait_result_status = LPRS_V2_WAIT4_RESULT_STATUS(packed_result);
            if (result_pid == 0) {
                result = 0;
            } else {
                if (status_raw != 0) {
                    *(int *)(uintptr_t)status_raw = (int)wait_result_status;
                }
                result = (int64_t)result_pid;
            }
        } else {
            result = wait_status;
        }
        lpr_destroy_standalone_wire_page(page_fd, page);
        return result;
    }
    lpr_linux_process_entry_t *selected = 0;
    for (uint64_t i = 0; i < LPR_LINUX_PROCESS_TABLE_SIZE; i++) {
        lpr_linux_process_entry_t *entry = &lpr_linux_processes[i];
        if (!entry->active || entry->process_fd < 16) {
            continue;
        }
        if (requested > 0 && entry->linux_pid != requested) {
            continue;
        }
        if (requested == 0 && entry->linux_pgrp != lpr_linux_current_pgrp) {
            continue;
        }
        if (requested < -1 && entry->linux_pgrp != -requested) {
            continue;
        }
        selected = entry;
        break;
    }
    if (selected == 0) {
        lpr_trace_process_event("wait4_nochild", (uint64_t)(uint32_t)requested, options, -LPR_LINUX_ECHILD);
        return -LPR_LINUX_ECHILD;
    }

    uint64_t exit_code = 0;
    int64_t status = 0;
    lpr_trace_process_event(
        "wait4_selected",
        (uint64_t)(uint32_t)selected->linux_pid,
        (uint64_t)(uint32_t)selected->process_fd,
        0);
    if ((options & LPR_LINUX_WNOHANG) != 0) {
        lpr_linux_pump_tty_signals();
        status = lpr_linux_try_wait_process_fd((uint64_t)(uint32_t)selected->process_fd, &exit_code);
        if (status == -LPR_LINUX_EAGAIN) {
            return 0;
        }
    } else {
        status = lpr_linux_wait_process_fd((uint64_t)(uint32_t)selected->process_fd, &exit_code);
    }
    if (status != 0) {
        return status;
    }
    if (status_raw != 0) {
        int *out_status = (int *)(uintptr_t)status_raw;
        *out_status = (int)((exit_code & 0xffu) << 8);
    }
    const int32_t linux_pid = selected->linux_pid;
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)selected->process_fd);
    lpr_memset(selected, 0, sizeof(*selected));
    lpr_trace_process_event("wait4_reaped", (uint64_t)(uint32_t)linux_pid, exit_code & 0xffu, 0);
    return (int64_t)linux_pid;
}

int64_t lpr_linux_execve(uint64_t path_raw, uint64_t argv_raw, uint64_t envp_raw)
{
    const char *path = (const char *)(uintptr_t)path_raw;
    if (path == 0) {
        return -LPR_LINUX_EFAULT;
    }
    const uint64_t path_len = (uint64_t)lpr_strnlen(path, FILED_V2_PATH_BYTES);
    if (path_len == 0 || path_len >= FILED_V2_PATH_BYTES) {
        return -LPR_LINUX_ENAMETOOLONG;
    }

    filed_v2_exec_path_t exec;
    lpr_memset(&exec, 0, sizeof(exec));
    exec.flags = FILED_V2_EXEC_LINUX_LPR | FILED_V2_EXEC_LINUX_BOOTSTRAP | FILED_V2_EXEC_SELF;
    lpr_memcpy(exec.path, path, (size_t)path_len + 1u);
    int status = (int)lpr_prepare_exec_cwd(&exec);
    if (status != 0) {
        return status;
    }
    lpr_linux_process_state_init();
    exec.linux_pid = (uint64_t)(uint32_t)lpr_linux_current_pid;
    exec.linux_ppid = (uint64_t)(uint32_t)lpr_linux_current_ppid;
    exec.linux_sid = (uint64_t)(uint32_t)lpr_linux_current_sid;
    exec.linux_pgrp = (uint64_t)(uint32_t)lpr_linux_current_pgrp;
    exec.linux_next_pid = (uint64_t)(uint32_t)lpr_linux_next_pid;
    if (lpr_supervisor_enabled) {
        lpr_exec_set_supervisor_tokens(&exec, lpr_supervisor_token);
    }
    lpr_trace_process_event("execve_begin", path_len, 0, 0);

    status = lpr_exec_copy_string_vector(
        &exec,
        exec.argv,
        FILED_V2_EXEC_MAX_ARGS,
        argv_raw,
        &exec.argc);
    if (status != 0) {
        lpr_discard_exec_cwd(&exec);
        return status;
    }
    if (exec.argc == 0) {
        status = lpr_exec_add_string(&exec, &exec.argv[0], path);
        if (status != 0) {
            lpr_discard_exec_cwd(&exec);
            return status;
        }
        exec.argc = 1;
    }
    status = lpr_exec_copy_string_vector(
        &exec,
        exec.envp,
        FILED_V2_EXEC_MAX_ENVS,
        envp_raw,
        &exec.envc);
    if (status != 0) {
        lpr_discard_exec_cwd(&exec);
        return status;
    }

    lpr_exec_local_fd_table_t local_table;
    local_table.fd = -1;
    local_table.map_bytes = 0;
    local_table.table = 0;
    status = lpr_prepare_exec_local_fds(&exec, &local_table);
    if (status != 0) {
        lpr_discard_exec_cwd(&exec);
        return status;
    }

    int process_fd = -1;
    int thread_fd = -1;
    int bootstrap_fd = -1;
    const int64_t exec_status =
        lpr_filed_exec_self(&exec, &local_table, &process_fd, &thread_fd, &bootstrap_fd);
    lpr_destroy_exec_local_fd_table(&local_table);
    if (exec_status != 0) {
        lpr_discard_exec_cwd(&exec);
        lpr_trace_process_event("execve_error", path_len, 0, exec_status);
        return exec_status;
    }
    status = lpr_install_exec_bootstrap_fd(bootstrap_fd);
    if (status != 0) {
        lpr_discard_exec_cwd(&exec);
        if (bootstrap_fd >= 16) {
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)bootstrap_fd);
        }
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_PROCESS_KILL, (uint64_t)(uint32_t)process_fd, 1);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)thread_fd);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)process_fd);
        return status;
    }
    lpr_trace_process_event("execve_commit", (uint64_t)(uint32_t)process_fd, (uint64_t)(uint32_t)thread_fd, 0);
    if (lpr_supervisor_enabled) {
        (void)lpr_supervisor_call_token(
            LPRS_V2_OP_PROCESS_EXEC_COMMIT_BEGIN,
            lpr_supervisor_token,
            -1,
            0);
    }
    lpr_close_local_state_before_self_exec();
    const int64_t commit_status = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_PROCESS_EXEC_FROM,
        (uint64_t)(uint32_t)process_fd,
        (uint64_t)(uint32_t)thread_fd,
        0);
    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_PROCESS_KILL, (uint64_t)(uint32_t)process_fd, 1);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)thread_fd);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)process_fd);
    if (commit_status != 0) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, LPR_BOOTSTRAP_FD);
        return lpr_pacha_status_to_errno(commit_status);
    }
    for (;;) {
    }
}
