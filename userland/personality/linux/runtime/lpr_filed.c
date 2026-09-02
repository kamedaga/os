#include "lpr_filed_internal.h"

__attribute__((visibility("hidden")))
void *memset(void *dst, int c, size_t n)
{
    return lpr_memset(dst, c, n);
}

lpr_state_t lpr_state = {
    .thread_count = 1,
    .filed_rpc = {
        .request_id = 0x4c505246494c4501ull,
        .wire_page_fd = -1,
        .session_fd = -1,
        .session_page_fd = -1,
        .readv_vmo_fd = -1,
        .pread_vmo_page_fd = -1,
    },
    .termd_rpc = {
        .request_id = 0x4c50525445524d01ull,
        .wire_page_fd = -1,
    },
    .netd_rpc = {
        .request_id = 0x4c50524e45544401ull,
        .next_ephemeral_port = 49152u,
    },
};

static _Noreturn void lpr_signal_thread_state_exhausted(void)
{
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, 127u);
    for (;;) {
    }
}

_Static_assert(
    sizeof(lpr_signal_thread_chunk_t) <= 4096u,
    "signal thread chunk must fit in one page");

static uint32_t lpr_signal_thread_tid(void)
{
    const int64_t raw_tid = lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID);
    if (raw_tid <= 0 || raw_tid > UINT32_MAX) {
        lpr_signal_thread_state_exhausted();
    }
    return (uint32_t)raw_tid;
}

static void lpr_signal_thread_consume_start_reservation(void)
{
    uint32_t reservations = __atomic_load_n(
        &lpr_state.signal.start_reservations,
        __ATOMIC_ACQUIRE);
    while (reservations != 0u && !__atomic_compare_exchange_n(
        &lpr_state.signal.start_reservations,
        &reservations,
        reservations - 1u,
        0,
        __ATOMIC_ACQ_REL,
        __ATOMIC_ACQUIRE))
    {
    }
}

static lpr_signal_thread_slot_t *lpr_signal_thread_scan_slots(
    lpr_signal_thread_slot_t *slots,
    uint32_t slot_count,
    uint32_t tid,
    int claim_empty)
{
    const uint32_t first = (tid * 2654435761u) % slot_count;
    for (uint32_t distance = 0; distance < slot_count; ++distance) {
        lpr_signal_thread_slot_t *slot = &slots[(first + distance) % slot_count];
        uint32_t owner = __atomic_load_n(&slot->tid, __ATOMIC_ACQUIRE);
        if (owner == tid) {
            return slot;
        }
        if (claim_empty && owner == 0u && __atomic_compare_exchange_n(
                &slot->tid,
                &owner,
                tid,
                0,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE))
        {
            lpr_signal_thread_consume_start_reservation();
            return slot;
        }
    }
    return 0;
}

static lpr_signal_thread_slot_t *lpr_signal_thread_slot_for_tid(
    uint32_t tid,
    int claim_empty)
{
    lpr_signal_thread_slot_t *slot = lpr_signal_thread_scan_slots(
        lpr_state.signal.threads,
        LPR_SIGNAL_THREAD_SLOT_COUNT,
        tid,
        0);
    if (slot != 0) {
        return slot;
    }
    for (lpr_signal_thread_chunk_t *chunk = __atomic_load_n(
             &lpr_state.signal.overflow_head,
             __ATOMIC_ACQUIRE);
         chunk != 0;
         chunk = __atomic_load_n(&chunk->next, __ATOMIC_ACQUIRE))
    {
        slot = lpr_signal_thread_scan_slots(
            chunk->slots,
            LPR_SIGNAL_THREAD_CHUNK_SLOT_COUNT,
            tid,
            0);
        if (slot != 0) {
            return slot;
        }
    }
    if (!claim_empty) {
        return 0;
    }
    slot = lpr_signal_thread_scan_slots(
        lpr_state.signal.threads,
        LPR_SIGNAL_THREAD_SLOT_COUNT,
        tid,
        1);
    if (slot != 0) {
        return slot;
    }
    for (lpr_signal_thread_chunk_t *chunk = __atomic_load_n(
             &lpr_state.signal.overflow_head,
             __ATOMIC_ACQUIRE);
         chunk != 0;
         chunk = __atomic_load_n(&chunk->next, __ATOMIC_ACQUIRE))
    {
        slot = lpr_signal_thread_scan_slots(
            chunk->slots,
            LPR_SIGNAL_THREAD_CHUNK_SLOT_COUNT,
            tid,
            1);
        if (slot != 0) {
            return slot;
        }
    }
    return 0;
}

static uint32_t lpr_signal_thread_free_slot_count(void)
{
    uint32_t count = 0;
    for (uint32_t index = 0; index < LPR_SIGNAL_THREAD_SLOT_COUNT; ++index) {
        if (__atomic_load_n(
                &lpr_state.signal.threads[index].tid,
                __ATOMIC_ACQUIRE) == 0u)
        {
            ++count;
        }
    }
    for (lpr_signal_thread_chunk_t *chunk = __atomic_load_n(
             &lpr_state.signal.overflow_head,
             __ATOMIC_ACQUIRE);
         chunk != 0;
         chunk = __atomic_load_n(&chunk->next, __ATOMIC_ACQUIRE))
    {
        for (uint32_t index = 0;
             index < LPR_SIGNAL_THREAD_CHUNK_SLOT_COUNT;
             ++index)
        {
            if (__atomic_load_n(&chunk->slots[index].tid, __ATOMIC_ACQUIRE) == 0u) {
                ++count;
            }
        }
    }
    return count;
}

static int lpr_signal_thread_grow_locked(void)
{
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        0,
        0,
        4096u,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_PRIVATE | PACHAOS_MMAP_ANONYMOUS,
        0);
    if (mapped < 4096) {
        return (int)lpr_pacha_status_to_errno(mapped);
    }
    lpr_signal_thread_chunk_t *chunk =
        (lpr_signal_thread_chunk_t *)(uintptr_t)mapped;
    lpr_memset(chunk, 0, 4096u);
    /* Frames cache state pointers, so published chunks never move or unmap. */
    chunk->next = __atomic_load_n(
        &lpr_state.signal.overflow_head,
        __ATOMIC_ACQUIRE);
    __atomic_store_n(
        &lpr_state.signal.overflow_head,
        chunk,
        __ATOMIC_RELEASE);
    return 0;
}

int lpr_signal_thread_reserve_start(void)
{
    /* A new native thread may receive an async signal before its bootstrap
     * runs.  Reserve aggregate free capacity in the parent so that path only
     * needs a lock-free slot claim and never allocates from a signal frame. */
    lpr_state_lock(&lpr_state.signal.grow_lock_word);
    uint32_t reservations = __atomic_load_n(
        &lpr_state.signal.start_reservations,
        __ATOMIC_ACQUIRE);
    while (lpr_signal_thread_free_slot_count() <= reservations) {
        const int status = lpr_signal_thread_grow_locked();
        if (status != 0) {
            lpr_state_unlock(&lpr_state.signal.grow_lock_word);
            return status;
        }
        reservations = __atomic_load_n(
            &lpr_state.signal.start_reservations,
            __ATOMIC_ACQUIRE);
    }
    (void)__atomic_add_fetch(
        &lpr_state.signal.start_reservations,
        1u,
        __ATOMIC_RELEASE);
    lpr_state_unlock(&lpr_state.signal.grow_lock_word);
    return 0;
}

void lpr_signal_thread_release_start_reservation(void)
{
    lpr_signal_thread_consume_start_reservation();
}

void lpr_signal_thread_state_prepare_current(void)
{
    const uint32_t tid = lpr_signal_thread_tid();
    if (lpr_signal_thread_slot_for_tid(tid, 1) != 0) {
        return;
    }
    lpr_state_lock(&lpr_state.signal.grow_lock_word);
    while (lpr_signal_thread_slot_for_tid(tid, 1) == 0) {
        if (lpr_signal_thread_grow_locked() != 0) {
            lpr_state_unlock(&lpr_state.signal.grow_lock_word);
            lpr_signal_thread_state_exhausted();
        }
    }
    lpr_state_unlock(&lpr_state.signal.grow_lock_word);
}

static lpr_signal_thread_slot_t *lpr_signal_thread_slot_current(void)
{
    /* This is also used by lpr_async_signal_entry: scan and claim only.  All
     * growth happens before signal registration or before THREAD_START. */
    const uint32_t tid = lpr_signal_thread_tid();
    lpr_signal_thread_slot_t *slot =
        lpr_signal_thread_slot_for_tid(tid, 1);
    if (slot == 0) {
        lpr_signal_thread_state_exhausted();
    }
    return slot;
}

lpr_signal_thread_state_t *lpr_signal_thread_state_current(void)
{
    const struct lpr_linux_user_frame *frame = lpr_current_linux_user_frame();
    if (frame != 0 && frame->runtime_signal_state != 0) {
        return (lpr_signal_thread_state_t *)(uintptr_t)frame->runtime_signal_state;
    }
    lpr_signal_thread_state_t *state = &lpr_signal_thread_slot_current()->state;
    if (frame != 0) {
        ((struct lpr_linux_user_frame *)frame)->runtime_signal_state =
            (uint64_t)(uintptr_t)state;
    }
    return state;
}

void lpr_signal_thread_state_release_current(void)
{
    /* Prevent a reusable slot from being observed by another signal handler
     * while the old owner is between this point and THREAD_EXIT. */
    (void)lpr_pacha_syscall2(
        PACHAOS_SYSCALL_PROCESS_SIGNAL_CTL,
        PACHAOS_PROCESS_SIGNAL_CTL_SET_MASK,
        UINT64_MAX);
    lpr_signal_thread_slot_t *slot = lpr_signal_thread_slot_current();
    lpr_memset(&slot->state, 0, sizeof(slot->state));
    __atomic_store_n(&slot->tid, 0u, __ATOMIC_RELEASE);
}

void lpr_signal_thread_state_after_fork_child(void)
{
    lpr_signal_thread_state_t inherited;
    lpr_memcpy(
        &inherited,
        lpr_signal_thread_state_current(),
        sizeof(inherited));
    lpr_state.signal.grow_lock_word = 0;
    __atomic_store_n(
        &lpr_state.signal.signalfd_pending_mask,
        0u,
        __ATOMIC_RELEASE);
    __atomic_store_n(
        &lpr_state.signal.start_reservations,
        0u,
        __ATOMIC_RELEASE);
    lpr_memset(lpr_state.signal.threads, 0, sizeof(lpr_state.signal.threads));
    for (lpr_signal_thread_chunk_t *chunk = __atomic_load_n(
             &lpr_state.signal.overflow_head,
             __ATOMIC_ACQUIRE);
         chunk != 0;
         chunk = __atomic_load_n(&chunk->next, __ATOMIC_ACQUIRE))
    {
        lpr_memset(chunk->slots, 0, sizeof(chunk->slots));
    }
    lpr_signal_thread_state_t *current = &lpr_signal_thread_slot_current()->state;
    lpr_memcpy(current, &inherited, sizeof(*current));
    current->pending_mask = 0;
    current->wait_restore_mask = 0;
    current->wait_restore_mask_active = 0;
    current->sigwait_active = 0;
    current->sigwait_set = 0;
    current->sigwait_info = 0;
    current->sigwait_deadline_ns = 0;
    current->sigwait_deadline_finite = 0;
    current->dispatching = 0;
    const struct lpr_linux_user_frame *frame = lpr_current_linux_user_frame();
    if (frame != 0) {
        ((struct lpr_linux_user_frame *)frame)->runtime_signal_state =
            (uint64_t)(uintptr_t)current;
    }
}
