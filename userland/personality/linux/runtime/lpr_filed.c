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
        .page_fd = -1,
        .next_ephemeral_port = 49152u,
    },
};

static _Noreturn void lpr_signal_thread_state_exhausted(void)
{
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT, 127u);
    for (;;) {
    }
}

static lpr_signal_thread_slot_t *lpr_signal_thread_slot_current(void)
{
    const int64_t raw_tid = lpr_pacha_syscall0(PACHAOS_SYSCALL_GETTID);
    if (raw_tid <= 0 || raw_tid > UINT32_MAX) {
        lpr_signal_thread_state_exhausted();
    }
    const uint32_t tid = (uint32_t)raw_tid;
    const uint32_t first = (tid * 2654435761u) % LPR_SIGNAL_THREAD_SLOT_COUNT;
    for (uint32_t distance = 0; distance < LPR_SIGNAL_THREAD_SLOT_COUNT; ++distance) {
        lpr_signal_thread_slot_t *slot =
            &lpr_state.signal.threads[(first + distance) % LPR_SIGNAL_THREAD_SLOT_COUNT];
        uint32_t owner = __atomic_load_n(&slot->tid, __ATOMIC_ACQUIRE);
        if (owner == tid) {
            return slot;
        }
        if (owner == 0u && __atomic_compare_exchange_n(
                &slot->tid,
                &owner,
                tid,
                0,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE))
        {
            return slot;
        }
    }
    lpr_signal_thread_state_exhausted();
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
    lpr_memset(lpr_state.signal.threads, 0, sizeof(lpr_state.signal.threads));
    lpr_signal_thread_state_t *current = &lpr_signal_thread_slot_current()->state;
    lpr_memcpy(current, &inherited, sizeof(*current));
    current->pending_mask = 0;
    current->wait_restore_mask = 0;
    current->wait_restore_mask_active = 0;
    current->dispatching = 0;
    const struct lpr_linux_user_frame *frame = lpr_current_linux_user_frame();
    if (frame != 0) {
        ((struct lpr_linux_user_frame *)frame)->runtime_signal_state =
            (uint64_t)(uintptr_t)current;
    }
}
