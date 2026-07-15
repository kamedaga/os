#include "../lpr_filed_internal.h"


int lpr_create_wire_page(void **out_page);
void lpr_destroy_wire_page(int page_fd, void *page);
int lpr_create_tty_wire_page(void **out_page);
void lpr_destroy_tty_wire_page(int page_fd, void *page);
void lpr_linux_process_state_init(void);
int lpr_linux_pump_tty_signals(void);
void lpr_linux_raise_sigpipe(void);
uint64_t lpr_linux_unblockable_signal_mask(void);
void lpr_fill_termd_caller(uint64_t *session_id, uint64_t *process_id, uint64_t *pgrp_id);
void lpr_fill_termd_signal_state(uint64_t *signal_mask, uint64_t *signal_ignored);
int64_t lpr_supervisor_call(
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    int transfer_fd,
    uint64_t *out_result);
int64_t lpr_supervisor_call_token(
    uint32_t op,
    uint64_t token,
    int transfer_fd,
    uint64_t *out_result);
int64_t lpr_supervisor_kill_pid(int32_t pid, uint32_t sig, uint64_t *out_delivered);
int lpr_supervisor_get_state(lprs_process_state_t *out_state);
int64_t lpr_tty_wait(uint64_t fd, uint32_t events);
void lpr_pipe_after_fork_child(void);
void lpr_cwd_init(void);
int64_t lpr_filed_dup_handle(uint64_t handle, uint64_t fd_flags, uint64_t *out_handle);
int lpr_exec_local_fd_preserve(uint64_t fd, int *out_preserve);
int lpr_count_exec_local_fds(uint64_t *out_count);

void lpr_filed_session_drop(void)
{
    if (lpr_session_page != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)lpr_session_page,
            FILED_SESSION_PAGE_BYTES);
    }
    if (lpr_session_page_fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)lpr_session_page_fd);
    }
    if (lpr_session_fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)lpr_session_fd);
    }
    lpr_session_fd = -1;
    lpr_session_page_fd = -1;
    lpr_session_page = 0;
    lpr_session_checked = 0;
    lpr_session_payload_busy = 0;
}
void *lpr_session_payload_slot(uint64_t slot)
{
    if (lpr_session_page == 0 || slot >= FILED_FAST_PAYLOAD_SLOT_COUNT) {
        return 0;
    }
    return (void *)((uintptr_t)lpr_session_page +
        FILED_FAST_PAYLOAD_OFFSET +
        slot * FILED_PAGE_BYTES);
}

void lpr_zero_bytes(void *ptr, uint64_t len)
{
    unsigned char *p = (unsigned char *)ptr;
    while (len != 0) {
        *p++ = 0;
        len--;
    }
}

static void lpr_fd_lock_futex_wait(volatile uint32_t *word, uint32_t expected)
{
    (void)lpr_pacha_syscall3(
        PACHA_RUNTIME_SYSCALL_FUTEX_WAIT,
        (uint64_t)(uintptr_t)word,
        expected,
        0);
}

static void lpr_fd_lock_futex_wake(volatile uint32_t *word, uint32_t count)
{
    (void)lpr_pacha_syscall2(
        PACHA_RUNTIME_SYSCALL_FUTEX_WAKE,
        (uint64_t)(uintptr_t)word,
        count);
}

void lpr_state_lock(volatile uint32_t *word)
{
    if (word == 0 || __atomic_load_n(&lpr_state.thread_count, __ATOMIC_ACQUIRE) <= 1u) {
        return;
    }
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(
                word,
                &expected,
                1u,
                0,
                __ATOMIC_ACQUIRE,
                __ATOMIC_RELAXED))
        {
            return;
        }
        lpr_fd_lock_futex_wait(word, 1u);
    }
}

void lpr_state_unlock(volatile uint32_t *word)
{
    if (word == 0 || __atomic_load_n(&lpr_state.thread_count, __ATOMIC_ACQUIRE) <= 1u) {
        return;
    }
    __atomic_store_n(word, 0u, __ATOMIC_RELEASE);
    lpr_fd_lock_futex_wake(word, 1u);
}

uint64_t lpr_next_request_id(volatile uint64_t *counter)
{
    return __atomic_add_fetch(counter, 1u, __ATOMIC_RELAXED);
}

void lpr_fd_arrays_init(void)
{
    if (lpr_fd_table_capacity != 0) {
        return;
    }
    lpr_control_entries = lpr_control_entries_initial;
    lpr_control_ofds = lpr_control_ofds_initial;
    lpr_control_backends = lpr_control_backends_initial;
    lpr_fd_table_capacity = LPR_FD_TABLE_INITIAL_SIZE;
    lpr_fd_table_init(
        &lpr_control_fd_table,
        lpr_control_entries,
        LPR_FD_TABLE_INITIAL_SIZE,
        lpr_control_ofds,
        LPR_FD_TABLE_INITIAL_SIZE,
        lpr_control_backends,
        LPR_FD_TABLE_INITIAL_SIZE);
    lpr_fd_table_configure_lock(
        &lpr_control_fd_table,
        &lpr_state.thread_count,
        lpr_fd_lock_futex_wait,
        lpr_fd_lock_futex_wake);
}

uint64_t lpr_align_up_pow2(uint64_t value, uint64_t align)
{
    const uint64_t mask = align - 1u;
    if (align == 0 || (align & mask) != 0 || value > UINT64_MAX - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

int lpr_fd_table_segment_bytes(uint64_t capacity, uint64_t element_size, uint64_t *out)
{
    if (out == 0 || capacity > UINT64_MAX / element_size) {
        return 0;
    }
    const uint64_t bytes = lpr_align_up_pow2(capacity * element_size, 4096ull);
    if (bytes == 0) {
        return 0;
    }
    *out = bytes;
    return 1;
}

int lpr_fd_table_layout(
    uint64_t capacity,
    uint64_t *entry_offset,
    uint64_t *ofd_offset,
    uint64_t *backend_offset,
    uint64_t *total_bytes)
{
    if (capacity < LPR_FD_TABLE_INITIAL_SIZE ||
        capacity > LPR_FD_TABLE_MAX_SIZE ||
        entry_offset == 0 ||
        ofd_offset == 0 ||
        backend_offset == 0 ||
        total_bytes == 0)
    {
        return 0;
    }
    uint64_t entry_bytes = 0;
    uint64_t ofd_bytes = 0;
    uint64_t backend_bytes = 0;
    if (!lpr_fd_table_segment_bytes(capacity, sizeof(lpr_fd_entry_t), &entry_bytes) ||
        !lpr_fd_table_segment_bytes(capacity, sizeof(lpr_ofd_t), &ofd_bytes) ||
        !lpr_fd_table_segment_bytes(capacity, sizeof(lpr_backend_record_t), &backend_bytes))
    {
        return 0;
    }
    if (entry_bytes > UINT64_MAX - ofd_bytes ||
        entry_bytes + ofd_bytes > UINT64_MAX - backend_bytes)
    {
        return 0;
    }
    *entry_offset = 0;
    *ofd_offset = entry_bytes;
    *backend_offset = entry_bytes + ofd_bytes;
    *total_bytes = entry_bytes + ofd_bytes + backend_bytes;
    return 1;
}

uint64_t lpr_fd_table_next_capacity(uint64_t required_capacity)
{
    lpr_fd_arrays_init();
    uint64_t capacity = lpr_fd_table_capacity;
    if (capacity < LPR_FD_TABLE_INITIAL_SIZE) {
        capacity = LPR_FD_TABLE_INITIAL_SIZE;
    }
    while (capacity < required_capacity) {
        if (capacity >= LPR_FD_TABLE_MAX_SIZE) {
            return 0;
        }
        if (capacity > LPR_FD_TABLE_MAX_SIZE / 2u) {
            capacity = LPR_FD_TABLE_MAX_SIZE;
            continue;
        }
        capacity *= 2u;
    }
    return capacity;
}

int lpr_fd_table_ensure_capacity(uint64_t required_capacity)
{
    lpr_fd_arrays_init();
    if (required_capacity <= lpr_fd_table_capacity) {
        return 0;
    }
    if (required_capacity > LPR_FD_TABLE_MAX_SIZE) {
        return -LPR_LINUX_EMFILE;
    }
    const uint64_t new_capacity = lpr_fd_table_next_capacity(required_capacity);
    if (new_capacity == 0) {
        return -LPR_LINUX_EMFILE;
    }
    uint64_t entry_offset = 0;
    uint64_t ofd_offset = 0;
    uint64_t backend_offset = 0;
    uint64_t total_bytes = 0;
    if (!lpr_fd_table_layout(
            new_capacity,
            &entry_offset,
            &ofd_offset,
            &backend_offset,
            &total_bytes))
    {
        return -LPR_LINUX_ENOMEM;
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        0,
        0,
        total_bytes,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_PRIVATE | PACHAOS_MMAP_ANONYMOUS,
        0);
    if (mapped < 4096) {
        return (int)lpr_pacha_status_to_errno(mapped);
    }
    unsigned char *base = (unsigned char *)(uintptr_t)mapped;
    lpr_fd_entry_t *new_entries =
        (lpr_fd_entry_t *)(void *)(base + entry_offset);
    lpr_ofd_t *new_ofds =
        (lpr_ofd_t *)(void *)(base + ofd_offset);
    lpr_backend_record_t *new_backends =
        (lpr_backend_record_t *)(void *)(base + backend_offset);
    lpr_fd_table_lock(&lpr_control_fd_table);
    if (required_capacity <= lpr_fd_table_capacity) {
        lpr_fd_table_unlock(&lpr_control_fd_table);
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)mapped,
            total_bytes);
        return 0;
    }
    lpr_memcpy(
        new_entries,
        lpr_control_entries,
        (size_t)(lpr_fd_table_capacity * sizeof(*lpr_control_entries)));
    lpr_memcpy(
        new_ofds,
        lpr_control_ofds,
        (size_t)(lpr_fd_table_capacity * sizeof(*lpr_control_ofds)));
    lpr_memcpy(
        new_backends,
        lpr_control_backends,
        (size_t)(lpr_fd_table_capacity * sizeof(*lpr_control_backends)));
    if (lpr_fd_table_dynamic_base != 0 && lpr_fd_table_dynamic_bytes != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)lpr_fd_table_dynamic_base,
            lpr_fd_table_dynamic_bytes);
    }
    lpr_control_entries = new_entries;
    lpr_control_ofds = new_ofds;
    lpr_control_backends = new_backends;
    lpr_control_fd_table.entries = lpr_control_entries;
    lpr_control_fd_table.ofds = lpr_control_ofds;
    lpr_control_fd_table.backends = lpr_control_backends;
    lpr_control_fd_table.entry_count = (uint32_t)new_capacity;
    lpr_control_fd_table.ofd_count = (uint32_t)new_capacity;
    lpr_control_fd_table.backend_count = (uint32_t)new_capacity;
    lpr_control_fd_table.generation++;
    lpr_fd_table_capacity = new_capacity;
    lpr_fd_table_dynamic_base = (void *)(uintptr_t)mapped;
    lpr_fd_table_dynamic_bytes = total_bytes;
    lpr_fd_table_unlock(&lpr_control_fd_table);
    return 0;
}

int lpr_fd_table_ensure_fd(uint64_t fd)
{
    if (fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    return lpr_fd_table_ensure_capacity(fd + 1u);
}

void lpr_trace_clone_args(uint64_t flags, uint64_t child_stack, uint64_t parent_tid, uint64_t child_tid)
{
    pacha_trace4(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_EVENT_LPR_CLONE_ARGS, PACHA_TRACE_CLASS_STATE, flags, child_stack, parent_tid, child_tid);
}

void lpr_trace_clone_frame(const char *event, const struct lpr_linux_user_frame *frame, int64_t status)
{
    if (frame == 0) {
        return;
    }
    pacha_trace6(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_CLONE_FRAME,
        PACHA_TRACE_CLASS_STATE,
        pacha_trace_name_id(event),
        (uint64_t)(uintptr_t)frame,
        frame->rip,
        frame->rsp,
        frame->rcx,
        (uint64_t)status);
    pacha_trace3(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_EVENT_LPR_CLONE_FRAME, PACHA_TRACE_CLASS_STATE, frame->rax, frame->rdx, frame->rflags);
}

void lpr_trace_process_event(const char *event, uint64_t a, uint64_t b, int64_t status)
{
    pacha_trace5(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_PROCESS,
        PACHA_TRACE_CLASS_STATE,
        pacha_trace_name_id(event),
        (uint64_t)lpr_linux_current_pid,
        a,
        b,
        (uint64_t)status);
    if (event != 0 && lpr_strcmp(event, "execve_begin") == 0) {
        lpr_state_dump(event);
    }
}

void lpr_trace_readv_size(uint64_t fd, uint64_t iov_count, uint64_t requested, uint64_t coalesced, uint64_t offset)
{
    pacha_trace5(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_EVENT_LPR_READV_SIZE, PACHA_TRACE_CLASS_DEBUG, fd, iov_count, requested, coalesced, offset);
}

void lpr_trace_readv_to_vmo_status(uint64_t fd, uint64_t requested, int64_t status)
{
    pacha_trace3(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_EVENT_LPR_READV_TO_VMO_STATUS, PACHA_TRACE_CLASS_DEBUG, fd, requested, (uint64_t)status);
}

void lpr_linux_readv_cache_trace_dump(void)
{
    pacha_trace6(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_READV_CACHE_METRIC,
        PACHA_TRACE_CLASS_METRIC,
        lpr_readv_cache_total,
        lpr_readv_cache_coalesced,
        lpr_readv_cache_hit,
        lpr_readv_cache_fill,
        lpr_readv_cache_fallback,
        lpr_readv_cache_bytes);
    pacha_trace2(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_LPR_READV_CACHE_METRIC,
        PACHA_TRACE_CLASS_METRIC,
        lpr_readv_cache_cross_page,
        lpr_readv_cache_to_vmo);
}

int64_t lpr_pacha_status_to_errno(int64_t status)
{
    return pacha_kernel_status_to_errno(status);
}
