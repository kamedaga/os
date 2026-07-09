static int lpr_pipe_fd_is_active(uint64_t fd);
static int lpr_native_pipe_fd_info(uint64_t fd, struct pacha_fd_info *out);
static int lpr_native_pipe_slot_claimable(uint64_t fd, struct pacha_fd_info *out);
static int lpr_fd_slot_available(uint64_t fd);
static int lpr_fd_slot_alloc_from(uint64_t min_fd);
static int lpr_fd_table_ensure_capacity(uint64_t required_capacity);
static int lpr_fd_table_ensure_fd(uint64_t fd);
static int64_t lpr_pacha_status_to_errno(int64_t status);

static int lpr_create_wire_page(void **out_page);
static void lpr_destroy_wire_page(int page_fd, void *page);
static int lpr_create_tty_wire_page(void **out_page);
static void lpr_destroy_tty_wire_page(int page_fd, void *page);
static void lpr_linux_process_state_init(void);
static void lpr_linux_pump_tty_signals(void);
static void lpr_linux_raise_sigpipe(void);
static uint64_t lpr_linux_unblockable_signal_mask(void);
static void lpr_fill_termd_caller(uint64_t *session_id, uint64_t *process_id, uint64_t *pgrp_id);
static void lpr_fill_termd_signal_state(uint64_t *signal_mask, uint64_t *signal_ignored);
static int64_t lpr_supervisor_call(
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    int transfer_fd,
    uint64_t *out_result);
static int64_t lpr_supervisor_call_token(
    uint32_t op,
    uint64_t token,
    int transfer_fd,
    uint64_t *out_result);
static int64_t lpr_supervisor_kill_pid(int32_t pid, uint32_t sig, uint64_t *out_delivered);
static int lpr_supervisor_get_state(lprs_v2_process_state_t *out_state);
static int lpr_supervisor_fd_table_replace(void);
static int lpr_supervisor_fd_table_restore(uint64_t token);
static int64_t lpr_tty_wait(uint64_t fd, uint32_t events);
static void lpr_pipe_after_fork_child(void);
static void lpr_cwd_init(void);
static int64_t lpr_filed_dup_handle(uint64_t handle, uint64_t fd_flags, uint64_t *out_handle);
static int lpr_exec_local_fd_preserve(uint64_t fd, int *out_preserve);
static int lpr_count_exec_local_fds(uint64_t *out_count);
static void lpr_write_exec_local_fd_desc(filed_v2_exec_lpr_fd_t *desc, uint64_t fd);
static int lpr_prepare_exec_local_fds(
    filed_v2_exec_path_t *exec,
    lpr_exec_local_fd_table_t *local_table);
static void lpr_destroy_exec_local_fd_table(lpr_exec_local_fd_table_t *local_table);
static int lpr_install_bootstrap_local_fds(const lpr_bootstrap_fd_t *descs, uint64_t count);

static void lpr_filed_session_drop(void)
{
    if (lpr_session_page != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)lpr_session_page,
            FILED_V2_SESSION_PAGE_BYTES);
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
static int64_t lpr_filed_call(uint32_t op, int page_fd, uint64_t word2, uint64_t *out_result);
static int64_t lpr_filed_close_handle(uint64_t handle);
static int lpr_create_standalone_wire_page(void **out_page);
static void lpr_destroy_standalone_wire_page(int fd, void *page);

static void *lpr_session_payload_slot(uint64_t slot)
{
    if (lpr_session_page == 0 || slot >= FILED_V2_FAST_PAYLOAD_SLOT_COUNT) {
        return 0;
    }
    return (void *)((uintptr_t)lpr_session_page +
        FILED_V2_FAST_PAYLOAD_OFFSET +
        slot * FILED_V2_PAGE_BYTES);
}

static void lpr_zero_bytes(void *ptr, uint64_t len)
{
    unsigned char *p = (unsigned char *)ptr;
    while (len != 0) {
        *p++ = 0;
        len--;
    }
}

static void lpr_fd_arrays_init(void)
{
    if (lpr_fd_table_capacity != 0) {
        return;
    }
    lpr_fds = lpr_fds_initial;
    lpr_pipe_fds = lpr_pipe_fds_initial;
    lpr_event_fds = lpr_event_fds_initial;
    lpr_tty_fds = lpr_tty_fds_initial;
    lpr_control_slots = lpr_control_slots_initial;
    lpr_control_files = lpr_control_files_initial;
    lpr_fd_table_capacity = LPR_FD_TABLE_INITIAL_SIZE;
    lpr_fd_table_init(
        &lpr_control_fd_table,
        lpr_control_slots,
        LPR_FD_TABLE_INITIAL_SIZE,
        lpr_control_files,
        LPR_FD_TABLE_INITIAL_SIZE);
}

static uint64_t lpr_align_up_pow2(uint64_t value, uint64_t align)
{
    const uint64_t mask = align - 1u;
    if (align == 0 || (align & mask) != 0 || value > UINT64_MAX - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

static int lpr_fd_table_segment_bytes(uint64_t capacity, uint64_t element_size, uint64_t *out)
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

static int lpr_fd_table_layout(
    uint64_t capacity,
    uint64_t *filed_offset,
    uint64_t *pipe_offset,
    uint64_t *event_offset,
    uint64_t *tty_offset,
    uint64_t *control_slot_offset,
    uint64_t *control_file_offset,
    uint64_t *total_bytes)
{
    if (capacity < LPR_FD_TABLE_INITIAL_SIZE ||
        capacity > LPR_FD_TABLE_MAX_SIZE ||
        filed_offset == 0 ||
        pipe_offset == 0 ||
        event_offset == 0 ||
        tty_offset == 0 ||
        control_slot_offset == 0 ||
        control_file_offset == 0 ||
        total_bytes == 0)
    {
        return 0;
    }
    uint64_t filed_bytes = 0;
    uint64_t pipe_bytes = 0;
    uint64_t event_bytes = 0;
    uint64_t tty_bytes = 0;
    uint64_t control_slot_bytes = 0;
    uint64_t control_file_bytes = 0;
    if (!lpr_fd_table_segment_bytes(capacity, sizeof(lpr_filed_fd_t), &filed_bytes) ||
        !lpr_fd_table_segment_bytes(capacity, sizeof(lpr_pipe_fd_t), &pipe_bytes) ||
        !lpr_fd_table_segment_bytes(capacity, sizeof(lpr_event_fd_t), &event_bytes) ||
        !lpr_fd_table_segment_bytes(capacity, sizeof(lpr_tty_fd_t), &tty_bytes) ||
        !lpr_fd_table_segment_bytes(capacity, sizeof(lpr_fd_table_slot_t), &control_slot_bytes) ||
        !lpr_fd_table_segment_bytes(capacity, sizeof(lpr_fd_table_file_t), &control_file_bytes))
    {
        return 0;
    }
    if (filed_bytes > UINT64_MAX - pipe_bytes ||
        filed_bytes + pipe_bytes > UINT64_MAX - event_bytes ||
        filed_bytes + pipe_bytes + event_bytes > UINT64_MAX - tty_bytes ||
        filed_bytes + pipe_bytes + event_bytes + tty_bytes > UINT64_MAX - control_slot_bytes ||
        filed_bytes + pipe_bytes + event_bytes + tty_bytes + control_slot_bytes > UINT64_MAX - control_file_bytes)
    {
        return 0;
    }
    *filed_offset = 0;
    *pipe_offset = filed_bytes;
    *event_offset = filed_bytes + pipe_bytes;
    *tty_offset = filed_bytes + pipe_bytes + event_bytes;
    *control_slot_offset = filed_bytes + pipe_bytes + event_bytes + tty_bytes;
    *control_file_offset = filed_bytes + pipe_bytes + event_bytes + tty_bytes + control_slot_bytes;
    *total_bytes = filed_bytes + pipe_bytes + event_bytes + tty_bytes + control_slot_bytes + control_file_bytes;
    return 1;
}

static uint64_t lpr_fd_table_next_capacity(uint64_t required_capacity)
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

static int lpr_fd_table_ensure_capacity(uint64_t required_capacity)
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
    uint64_t filed_offset = 0;
    uint64_t pipe_offset = 0;
    uint64_t event_offset = 0;
    uint64_t tty_offset = 0;
    uint64_t control_slot_offset = 0;
    uint64_t control_file_offset = 0;
    uint64_t total_bytes = 0;
    if (!lpr_fd_table_layout(
            new_capacity,
            &filed_offset,
            &pipe_offset,
            &event_offset,
            &tty_offset,
            &control_slot_offset,
            &control_file_offset,
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
    lpr_filed_fd_t *new_filed = (lpr_filed_fd_t *)(void *)(base + filed_offset);
    lpr_pipe_fd_t *new_pipe = (lpr_pipe_fd_t *)(void *)(base + pipe_offset);
    lpr_event_fd_t *new_event = (lpr_event_fd_t *)(void *)(base + event_offset);
    lpr_tty_fd_t *new_tty = (lpr_tty_fd_t *)(void *)(base + tty_offset);
    lpr_fd_table_slot_t *new_control_slots =
        (lpr_fd_table_slot_t *)(void *)(base + control_slot_offset);
    lpr_fd_table_file_t *new_control_files =
        (lpr_fd_table_file_t *)(void *)(base + control_file_offset);
    lpr_memcpy(new_filed, lpr_fds, (size_t)(lpr_fd_table_capacity * sizeof(*lpr_fds)));
    lpr_memcpy(new_pipe, lpr_pipe_fds, (size_t)(lpr_fd_table_capacity * sizeof(*lpr_pipe_fds)));
    lpr_memcpy(new_event, lpr_event_fds, (size_t)(lpr_fd_table_capacity * sizeof(*lpr_event_fds)));
    lpr_memcpy(new_tty, lpr_tty_fds, (size_t)(lpr_fd_table_capacity * sizeof(*lpr_tty_fds)));
    lpr_memcpy(
        new_control_slots,
        lpr_control_slots,
        (size_t)(lpr_fd_table_capacity * sizeof(*lpr_control_slots)));
    lpr_memcpy(
        new_control_files,
        lpr_control_files,
        (size_t)(lpr_fd_table_capacity * sizeof(*lpr_control_files)));
    if (lpr_fd_table_dynamic_base != 0 && lpr_fd_table_dynamic_bytes != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)lpr_fd_table_dynamic_base,
            lpr_fd_table_dynamic_bytes);
    }
    lpr_fds = new_filed;
    lpr_pipe_fds = new_pipe;
    lpr_event_fds = new_event;
    lpr_tty_fds = new_tty;
    lpr_control_slots = new_control_slots;
    lpr_control_files = new_control_files;
    lpr_control_fd_table.slots = lpr_control_slots;
    lpr_control_fd_table.files = lpr_control_files;
    lpr_control_fd_table.slot_count = (uint32_t)new_capacity;
    lpr_control_fd_table.file_count = (uint32_t)new_capacity;
    lpr_fd_table_capacity = new_capacity;
    lpr_fd_table_dynamic_base = (void *)(uintptr_t)mapped;
    lpr_fd_table_dynamic_bytes = total_bytes;
    return 0;
}

static int lpr_fd_table_ensure_fd(uint64_t fd)
{
    if (fd > LPR_LINUX_FD_MAX) {
        return -LPR_LINUX_EINVAL;
    }
    return lpr_fd_table_ensure_capacity(fd + 1u);
}

static void lpr_trace_clone_args(uint64_t flags, uint64_t child_stack, uint64_t parent_tid, uint64_t child_tid)
{
    pacha_trace4(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_EVENT_LPR_CLONE_ARGS, PACHA_TRACE_CLASS_STATE, flags, child_stack, parent_tid, child_tid);
}

static void lpr_trace_clone_frame(const char *event, const struct lpr_linux_user_frame *frame, int64_t status)
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

static void lpr_trace_process_event(const char *event, uint64_t a, uint64_t b, int64_t status)
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
}

static void lpr_trace_readv_size(uint64_t fd, uint64_t iov_count, uint64_t requested, uint64_t coalesced, uint64_t offset)
{
    pacha_trace5(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_EVENT_LPR_READV_SIZE, PACHA_TRACE_CLASS_DEBUG, fd, iov_count, requested, coalesced, offset);
}

static void lpr_trace_readv_to_vmo_status(uint64_t fd, uint64_t requested, int64_t status)
{
    pacha_trace3(PACHA_TRACE_COMPONENT_LPR, PACHA_TRACE_EVENT_LPR_READV_TO_VMO_STATUS, PACHA_TRACE_CLASS_DEBUG, fd, requested, (uint64_t)status);
}

static uint64_t lpr_readv_cache_total;
static uint64_t lpr_readv_cache_coalesced;
static uint64_t lpr_readv_cache_hit;
static uint64_t lpr_readv_cache_fill;
static uint64_t lpr_readv_cache_fallback;
static uint64_t lpr_readv_cache_cross_page;
static uint64_t lpr_readv_cache_to_vmo;
static uint64_t lpr_readv_cache_bytes;

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

static int64_t lpr_pacha_status_to_errno(int64_t status)
{
    return pacha_kernel_status_to_errno(status);
}

#include "../lpr_fd/control.inc"
#include "../lpr_pipe/io.inc"
