#include "../lpr_filed_internal.h"
#include "dmabuf.h"
#include "../support/unmap_profile.h"
#include "../../../../../kobox2/linux-sandbox/kobox/boot/drm_limits.h"

enum {
    LPR_DRM_IOCTL_VERSION = 0xc0406400u,
    LPR_UDMABUF_CREATE = 0x40187542u,
    LPR_DRM_IOCTL_SYNCOBJ_WAIT = 0xc02864c3u,
    LPR_DRM_IOCTL_SYNCOBJ_RESET = 0xc01064c4u,
    LPR_DRM_IOCTL_SYNCOBJ_SIGNAL = 0xc01064c5u,
    LPR_DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT = 0xc03864cau,
    LPR_DRM_IOCTL_SYNCOBJ_QUERY = 0xc01864cbu,
    LPR_DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL = 0xc01864cdu,
};

typedef struct lpr_udmabuf_create {
    uint32_t memfd;
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
} lpr_udmabuf_create_t;

typedef struct lpr_dma_buf_sync_file {
    uint32_t flags;
    int32_t fd;
} lpr_dma_buf_sync_file_t;

_Static_assert(sizeof(lpr_dma_buf_sync_file_t) == 8,
    "Linux dma-buf sync-file ioctl layout");

typedef struct lpr_drm_version {
    int32_t major;
    int32_t minor;
    int32_t patchlevel;
    uint32_t reserved0;
    uint64_t name_len;
    uint64_t name;
    uint64_t date_len;
    uint64_t date;
    uint64_t desc_len;
    uint64_t desc;
} lpr_drm_version_t;

typedef struct lpr_drm_mode_atomic {
    uint32_t flags;
    uint32_t count_objs;
    uint64_t objs_ptr;
    uint64_t count_props_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint64_t reserved;
    uint64_t user_data;
} lpr_drm_mode_atomic_t;

typedef struct lpr_drm_mode_create_blob {
    uint64_t data;
    uint32_t length;
    uint32_t blob_id;
} lpr_drm_mode_create_blob_t;

typedef struct lpr_drm_clip_rect {
    uint16_t x1;
    uint16_t y1;
    uint16_t x2;
    uint16_t y2;
} lpr_drm_clip_rect_t;

_Static_assert(sizeof(lpr_drm_mode_atomic_t) == 56,
    "Linux DRM atomic ioctl layout");
_Static_assert(sizeof(lpr_drm_mode_create_blob_t) == 16,
    "Linux DRM create-blob ioctl layout");
_Static_assert(sizeof(lpr_drm_clip_rect_t) == 8,
    "Linux DRM clip rectangle layout");

enum { LPR_DRM_EVENT_COOKIE_CAPACITY = 32u };

typedef struct lpr_drm_event_cookie {
    uint64_t handle;
    uint64_t token;
    uint64_t cookie;
} lpr_drm_event_cookie_t;

static volatile uint32_t lpr_drm_event_cookie_lock;
static volatile uint64_t lpr_drm_event_token_counter;
/* Owned with the cached tty/DRM wire page under termd_rpc.lock_word. */
static int lpr_drm_request_channel_fd;
static int lpr_drm_aux_cache_fd;
static void *lpr_drm_aux_cache;
static int lpr_drm_aux_cache_busy;
static lpr_drm_event_cookie_t
    lpr_drm_event_cookies[LPR_DRM_EVENT_COOKIE_CAPACITY];

enum {
    LPR_DRM_MAPPING_LEASE_CAPACITY =
        KOBOX_DRM_MAPPING_LIMIT + KOBOX_DRM_PRIME_LIMIT,
    LPR_DRM_MAPPING_FRAGMENT_CAPACITY = 2 * LPR_DRM_MAPPING_LEASE_CAPACITY,
};

typedef struct lpr_drm_mapping_lease {
    int fd;
    uint32_t fragments;
    uint32_t conservative;
} lpr_drm_mapping_lease_t;

typedef struct lpr_drm_mapping_fragment {
    uint64_t address;
    uint64_t length;
    uint32_t lease;
} lpr_drm_mapping_fragment_t;

static volatile uint32_t lpr_drm_mapping_lock;
static lpr_drm_mapping_lease_t
    lpr_drm_mapping_leases[LPR_DRM_MAPPING_LEASE_CAPACITY];
static lpr_drm_mapping_fragment_t
    lpr_drm_mapping_fragments[LPR_DRM_MAPPING_FRAGMENT_CAPACITY];

static uint64_t lpr_drm_page_span(uint64_t length)
{
    return length && length <= UINT64_MAX - 4095u ?
        (length + 4095u) & ~UINT64_C(4095) : 0;
}

static int lpr_drm_ranges_overlap(
    uint64_t first, uint64_t first_length,
    uint64_t second, uint64_t second_length)
{
    return first_length && second_length && first <= UINT64_MAX - first_length &&
        second <= UINT64_MAX - second_length &&
        first < second + second_length && second < first + first_length;
}

static void lpr_drm_mapping_make_conservative(uint32_t lease_index)
{
    lpr_drm_mapping_lease_t *lease = &lpr_drm_mapping_leases[lease_index];
    lease->conservative = 1;
    lease->fragments = 0;
    for (uint32_t i = 0; i < LPR_DRM_MAPPING_FRAGMENT_CAPACITY; ++i) {
        if (lpr_drm_mapping_fragments[i].lease == lease_index + 1u)
            lpr_memset(&lpr_drm_mapping_fragments[i], 0,
                sizeof(lpr_drm_mapping_fragments[i]));
    }
}

static void lpr_drm_mapping_drop_locked(uint64_t address, uint64_t length)
{
    const uint64_t end = address + length;
    for (uint32_t i = 0; i < LPR_DRM_MAPPING_FRAGMENT_CAPACITY; ++i) {
        lpr_drm_mapping_fragment_t *fragment = &lpr_drm_mapping_fragments[i];
        if (!fragment->lease || !lpr_drm_ranges_overlap(
                fragment->address, fragment->length, address, length))
            continue;
        uint32_t lease_index = fragment->lease - 1u;
        lpr_drm_mapping_lease_t *lease = &lpr_drm_mapping_leases[lease_index];
        const uint64_t fragment_end = fragment->address + fragment->length;
        if (address <= fragment->address && end >= fragment_end) {
            lpr_memset(fragment, 0, sizeof(*fragment));
            --lease->fragments;
        } else if (address <= fragment->address) {
            fragment->address = end;
            fragment->length = fragment_end - end;
        } else if (end >= fragment_end) {
            fragment->length = address - fragment->address;
        } else {
            uint32_t spare = LPR_DRM_MAPPING_FRAGMENT_CAPACITY;
            for (uint32_t j = 0; j < LPR_DRM_MAPPING_FRAGMENT_CAPACITY; ++j)
                if (!lpr_drm_mapping_fragments[j].lease) {
                    spare = j;
                    break;
                }
            if (spare == LPR_DRM_MAPPING_FRAGMENT_CAPACITY) {
                lpr_drm_mapping_make_conservative(lease_index);
                continue;
            }
            lpr_drm_mapping_fragments[spare] =
                (lpr_drm_mapping_fragment_t){
                    .address = end,
                    .length = fragment_end - end,
                    .lease = fragment->lease,
                };
            fragment->length = address - fragment->address;
            ++lease->fragments;
        }
    }
    for (uint32_t i = 0; i < LPR_DRM_MAPPING_LEASE_CAPACITY; ++i) {
        lpr_drm_mapping_lease_t *lease = &lpr_drm_mapping_leases[i];
        if (lease->fd >= 16 && !lease->fragments && !lease->conservative) {
            (void)lpr_pacha_syscall1(
                PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)lease->fd);
            lpr_memset(lease, 0, sizeof(*lease));
        }
    }
}

void lpr_drm_mapping_unmapped(uint64_t address, uint64_t length)
{
    length = lpr_drm_page_span(length);
    if (!length || address > UINT64_MAX - length)
        return;
    lpr_state_lock(&lpr_drm_mapping_lock);
    lpr_drm_mapping_drop_locked(address, length);
    lpr_state_unlock(&lpr_drm_mapping_lock);
}

static void lpr_drm_mapping_remapped_locked(
    uint64_t old_address, uint64_t old_length,
    uint64_t new_address, uint64_t new_length)
{
    old_length = lpr_drm_page_span(old_length);
    new_length = lpr_drm_page_span(new_length);
    if (!old_length || !new_length ||
        old_address > UINT64_MAX - old_length ||
        new_address > UINT64_MAX - new_length)
        return;
    uint32_t source = LPR_DRM_MAPPING_FRAGMENT_CAPACITY;
    for (uint32_t i = 0; i < LPR_DRM_MAPPING_FRAGMENT_CAPACITY; ++i) {
        if (lpr_drm_mapping_fragments[i].lease &&
            lpr_drm_mapping_fragments[i].address == old_address &&
            lpr_drm_mapping_fragments[i].length == old_length) {
            source = i;
            break;
        }
    }
    if (source != LPR_DRM_MAPPING_FRAGMENT_CAPACITY) {
        const uint32_t lease_id = lpr_drm_mapping_fragments[source].lease;
        lpr_drm_mapping_lease_t *lease =
            &lpr_drm_mapping_leases[lease_id - 1u];
        lpr_drm_mapping_fragments[source] =
            (lpr_drm_mapping_fragment_t){.lease = UINT32_MAX};
        lpr_drm_mapping_drop_locked(new_address, new_length);
        if (lease->conservative) {
            lpr_memset(&lpr_drm_mapping_fragments[source], 0,
                sizeof(lpr_drm_mapping_fragments[source]));
        } else {
            lpr_drm_mapping_fragments[source] = (lpr_drm_mapping_fragment_t){
                .address = new_address, .length = new_length, .lease = lease_id};
        }
    } else {
        for (uint32_t i = 0; i < LPR_DRM_MAPPING_FRAGMENT_CAPACITY; ++i) {
            if (lpr_drm_mapping_fragments[i].lease &&
                lpr_drm_ranges_overlap(lpr_drm_mapping_fragments[i].address,
                    lpr_drm_mapping_fragments[i].length,
                    old_address, old_length))
                lpr_drm_mapping_make_conservative(
                    lpr_drm_mapping_fragments[i].lease - 1u);
        }
        lpr_drm_mapping_drop_locked(new_address, new_length);
    }
}

void lpr_drm_mapping_remapped(uint64_t old_address, uint64_t old_length,
    uint64_t new_address, uint64_t new_length)
{
    lpr_state_lock(&lpr_drm_mapping_lock);
    lpr_drm_mapping_remapped_locked(old_address, old_length, new_address, new_length);
    lpr_state_unlock(&lpr_drm_mapping_lock);
}

/* Serialize native address reuse with lease accounting. A notification after
 * munmap is too late: another thread can already have mapped a new GPU view
 * at the same address, and the old notification would close its lease. */
int64_t lpr_drm_native_munmap(uint64_t address, uint64_t length)
{
#if defined(LPR_UNMAP_PROFILE) && LPR_UNMAP_PROFILE
    uint64_t stamp[4] = { lpr_unmap_profile_clock() };
    lpr_unmap_profile_snapshot_t snapshot;
#endif
    lpr_state_lock(&lpr_drm_mapping_lock);
#if defined(LPR_UNMAP_PROFILE) && LPR_UNMAP_PROFILE
    stamp[1] = lpr_unmap_profile_clock();
#endif
    int64_t result = lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, address, length);
#if defined(LPR_UNMAP_PROFILE) && LPR_UNMAP_PROFILE
    stamp[2] = lpr_unmap_profile_clock();
#endif
    uint64_t span = lpr_drm_page_span(length);
    if (!result && span && address <= UINT64_MAX - span)
        lpr_drm_mapping_drop_locked(address, span);
#if defined(LPR_UNMAP_PROFILE) && LPR_UNMAP_PROFILE
    stamp[3] = lpr_unmap_profile_clock();
    int emit = lpr_unmap_profile_record(length, result, stamp, &snapshot);
#endif
    lpr_state_unlock(&lpr_drm_mapping_lock);
#if defined(LPR_UNMAP_PROFILE) && LPR_UNMAP_PROFILE
    if (emit) lpr_unmap_profile_emit(&snapshot);
#endif
    return result;
}

int64_t lpr_drm_native_mmap(uint64_t fd, uint64_t address, uint64_t length,
    uint64_t prot, uint64_t flags, uint64_t offset)
{
    lpr_state_lock(&lpr_drm_mapping_lock);
    int64_t result = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP,
        fd, address, length, prot, flags, offset);
    uint64_t span = lpr_drm_page_span(length);
    if (result >= 4096 && span && (flags & PACHAOS_MMAP_FIXED))
        lpr_drm_mapping_drop_locked((uint64_t)result, span);
    lpr_state_unlock(&lpr_drm_mapping_lock);
    return result;
}

int64_t lpr_drm_native_mremap(uint64_t address, uint64_t length,
    uint64_t new_length, uint64_t flags, uint64_t target)
{
    lpr_state_lock(&lpr_drm_mapping_lock);
    int64_t result = lpr_pacha_syscall5(PACHA_VM_SYSCALL_MREMAP,
        address, length, new_length, flags, target);
    if (result >= 4096)
        lpr_drm_mapping_remapped_locked(address, length, (uint64_t)result, new_length);
    lpr_state_unlock(&lpr_drm_mapping_lock);
    return result;
}

void lpr_drm_mapping_fork_lock(void) { lpr_state_lock(&lpr_drm_mapping_lock); }
void lpr_drm_mapping_fork_unlock(void) { lpr_state_unlock(&lpr_drm_mapping_lock); }
void lpr_drm_mapping_fork_child(void)
{
    lpr_drm_mapping_lock = 0;
#if defined(LPR_UNMAP_PROFILE) && LPR_UNMAP_PROFILE
    lpr_unmap_profile_reset();
#endif
}

static int64_t lpr_drm_map_received(
    int vmo_fd, int lease_fd, uint64_t address, uint64_t length,
    uint64_t prot, uint64_t flags, uint64_t offset)
{
    const uint64_t span = lpr_drm_page_span(length);
    if (!span)
        return -LPR_LINUX_EINVAL;
    lpr_state_lock(&lpr_drm_mapping_lock);
    uint32_t lease_index = LPR_DRM_MAPPING_LEASE_CAPACITY;
    for (uint32_t i = 0; i < LPR_DRM_MAPPING_LEASE_CAPACITY; ++i)
        if (lpr_drm_mapping_leases[i].fd < 16) {
            lease_index = i;
            break;
        }
    if (lease_index == LPR_DRM_MAPPING_LEASE_CAPACITY) {
        lpr_state_unlock(&lpr_drm_mapping_lock);
        return -LPR_LINUX_EMFILE;
    }
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP, (uint64_t)(uint32_t)vmo_fd,
        address, length, prot, flags, offset);
    if (mapped >= 4096) {
        lpr_drm_mapping_drop_locked((uint64_t)mapped, span);
        uint32_t fragment_index = LPR_DRM_MAPPING_FRAGMENT_CAPACITY;
        for (uint32_t i = 0; i < LPR_DRM_MAPPING_FRAGMENT_CAPACITY; ++i)
            if (!lpr_drm_mapping_fragments[i].lease) {
                fragment_index = i;
                break;
            }
        lpr_drm_mapping_leases[lease_index] =
            (lpr_drm_mapping_lease_t){
                .fd = lease_fd,
                .fragments = fragment_index != LPR_DRM_MAPPING_FRAGMENT_CAPACITY,
                .conservative = fragment_index == LPR_DRM_MAPPING_FRAGMENT_CAPACITY,
            };
        if (fragment_index != LPR_DRM_MAPPING_FRAGMENT_CAPACITY)
            lpr_drm_mapping_fragments[fragment_index] =
                (lpr_drm_mapping_fragment_t){
                    .address = (uint64_t)mapped,
                    .length = span,
                    .lease = lease_index + 1u,
                };
    }
    lpr_state_unlock(&lpr_drm_mapping_lock);
    return mapped < 4096 ? lpr_pacha_status_to_errno(mapped) : mapped;
}

#if defined(LPR_DRM_STARTUP_PROFILE) && LPR_DRM_STARTUP_PROFILE
enum { LPR_DRM_PROFILE_COMMAND_SLOTS = 32u };

typedef struct lpr_drm_profile_command {
    uint32_t command;
    uint32_t count;
    uint64_t total_cycles;
    uint64_t max_cycles;
    uint64_t errors;
} lpr_drm_profile_command_t;

static lpr_drm_profile_command_t
    lpr_drm_profile_commands[LPR_DRM_PROFILE_COMMAND_SLOTS];
static uint64_t lpr_drm_profile_page_cycles;
static uint64_t lpr_drm_profile_prepare_cycles;
static uint64_t lpr_drm_profile_ipc_cycles;
static uint64_t lpr_drm_profile_cleanup_cycles;
static uint64_t lpr_drm_profile_calls;
static volatile uint32_t lpr_drm_profile_lock;

static void lpr_drm_profile_record(
    uint32_t command,
    int64_t status,
    uint64_t start,
    uint64_t page_end,
    uint64_t ipc_begin,
    uint64_t ipc_end,
    uint64_t end)
{
    while (__atomic_exchange_n(
        &lpr_drm_profile_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        __asm__ volatile("pause");
    }
    lpr_drm_profile_command_t *slot = 0;
    for (uint32_t i = 0; i < LPR_DRM_PROFILE_COMMAND_SLOTS; i++) {
        lpr_drm_profile_command_t *candidate = &lpr_drm_profile_commands[i];
        if (candidate->count != 0 && candidate->command == command) {
            slot = candidate;
            break;
        }
        if (slot == 0 && candidate->count == 0) slot = candidate;
    }
    if (slot != 0) {
        const uint64_t total = end >= start ? end - start : 0;
        slot->command = command;
        slot->count++;
        slot->total_cycles += total;
        if (total > slot->max_cycles) slot->max_cycles = total;
        if (status < 0) slot->errors++;
    }
    lpr_drm_profile_calls++;
    if (page_end >= start) lpr_drm_profile_page_cycles += page_end - start;
    if (ipc_begin >= page_end) {
        lpr_drm_profile_prepare_cycles += ipc_begin - page_end;
    }
    if (ipc_end >= ipc_begin) lpr_drm_profile_ipc_cycles += ipc_end - ipc_begin;
    if (end >= ipc_end) lpr_drm_profile_cleanup_cycles += end - ipc_end;
    __atomic_store_n(&lpr_drm_profile_lock, 0u, __ATOMIC_RELEASE);
}

void lpr_drm_startup_profile_dump(void)
{
    const uint64_t pid =
        (uint64_t)lpr_pacha_syscall0(PACHAOS_SYSCALL_GETPID);
    const uint64_t command_marker = pacha_trace_name_id("lpr.drm.ioctl");
    const uint64_t stage_marker = pacha_trace_name_id("lpr.drm.stages");
    for (uint32_t i = 0; i < LPR_DRM_PROFILE_COMMAND_SLOTS; i++) {
        const lpr_drm_profile_command_t *slot = &lpr_drm_profile_commands[i];
        if (slot->count == 0) continue;
        pacha_trace6(
            PACHA_TRACE_COMPONENT_LPR,
            PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
            PACHA_TRACE_CLASS_METRIC,
            command_marker,
            pid,
            slot->command,
            slot->count,
            slot->total_cycles,
            slot->max_cycles);
    }
    pacha_trace6(
        PACHA_TRACE_COMPONENT_LPR,
        PACHA_TRACE_EVENT_METRIC_TIMING_EXTRA,
        PACHA_TRACE_CLASS_METRIC,
        stage_marker,
        lpr_drm_profile_calls,
        lpr_drm_profile_page_cycles,
        lpr_drm_profile_prepare_cycles,
        lpr_drm_profile_ipc_cycles,
        lpr_drm_profile_cleanup_cycles);
    pacha_trace_dump_ring();
}
#else
void lpr_drm_startup_profile_dump(void)
{
}
#endif

static int lpr_drm_event_token_active(uint64_t token)
{
    for (uint32_t i = 0; i < LPR_DRM_EVENT_COOKIE_CAPACITY; i++) {
        if (lpr_drm_event_cookies[i].token == token) return 1;
    }
    return 0;
}

static int lpr_drm_event_cookie_register(
    uint64_t handle,
    uint64_t cookie,
    uint64_t *out_token)
{
    if (handle == 0 || out_token == 0) return -LPR_LINUX_EINVAL;
    lpr_state_lock(&lpr_drm_event_cookie_lock);
    uint32_t free_slot = LPR_DRM_EVENT_COOKIE_CAPACITY;
    for (uint32_t i = 0; i < LPR_DRM_EVENT_COOKIE_CAPACITY; i++) {
        if (lpr_drm_event_cookies[i].token == 0) {
            free_slot = i;
            break;
        }
    }
    if (free_slot == LPR_DRM_EVENT_COOKIE_CAPACITY) {
        lpr_state_unlock(&lpr_drm_event_cookie_lock);
        return -LPR_LINUX_EMFILE;
    }
    uint64_t token;
    do {
        token = UINT64_C(0x4c50524400000000) |
            (lpr_next_request_id(&lpr_drm_event_token_counter) &
                UINT64_C(0xffffffff));
    } while (token == UINT64_C(0x4c50524400000000) ||
        lpr_drm_event_token_active(token));
    lpr_drm_event_cookies[free_slot].handle = handle;
    lpr_drm_event_cookies[free_slot].token = token;
    lpr_drm_event_cookies[free_slot].cookie = cookie;
    lpr_state_unlock(&lpr_drm_event_cookie_lock);
    *out_token = token;
    return 0;
}

static int lpr_drm_event_cookie_take(
    uint64_t handle,
    uint64_t token,
    uint64_t *out_cookie)
{
    if (handle == 0 || token == 0 || out_cookie == 0) return 0;
    int found = 0;
    lpr_state_lock(&lpr_drm_event_cookie_lock);
    for (uint32_t i = 0; i < LPR_DRM_EVENT_COOKIE_CAPACITY; i++) {
        lpr_drm_event_cookie_t *entry = &lpr_drm_event_cookies[i];
        if (entry->handle != handle || entry->token != token) continue;
        *out_cookie = entry->cookie;
        lpr_memset(entry, 0, sizeof(*entry));
        found = 1;
        break;
    }
    lpr_state_unlock(&lpr_drm_event_cookie_lock);
    return found;
}

static void lpr_drm_event_cookie_cancel(uint64_t handle, uint64_t token)
{
    uint64_t ignored = 0;
    (void)lpr_drm_event_cookie_take(handle, token, &ignored);
}

static void lpr_drm_event_cookie_cancel_handle(uint64_t handle)
{
    if (handle == 0) return;
    lpr_state_lock(&lpr_drm_event_cookie_lock);
    for (uint32_t i = 0; i < LPR_DRM_EVENT_COOKIE_CAPACITY; i++) {
        if (lpr_drm_event_cookies[i].handle == handle) {
            lpr_memset(
                &lpr_drm_event_cookies[i], 0,
                sizeof(lpr_drm_event_cookies[i]));
        }
    }
    lpr_state_unlock(&lpr_drm_event_cookie_lock);
}

void lpr_drm_after_fork_child(void)
{
    /* PRIVATE was not inherited: the copied number can already name an
     * unrelated child FD. The child also obtains its own cached wire page. */
    lpr_drm_request_channel_fd = 0;
    /* The PRIVATE descriptor was not inherited, but fork copied its VMA.
     * Remove only this child's alias; never close the copied FD number. */
    if (lpr_drm_aux_cache)
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
            (uintptr_t)lpr_drm_aux_cache, GPUD_DRM_AUX_REUSE_BYTES);
    lpr_drm_aux_cache_fd = 0;
    lpr_drm_aux_cache = 0;
    lpr_drm_aux_cache_busy = 0;
    __atomic_store_n(&lpr_drm_event_cookie_lock, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&lpr_drm_mapping_lock, 0u, __ATOMIC_RELEASE);
}

static void *lpr_gpud_drm_payload(void *page)
{
    return page == 0 ? 0 : (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES;
}

static int64_t lpr_gpud_drm_ioctl_inline(gpud_drm_ioctl_request_t *ioctl)
{
    struct pacha_ipc_msg request = {
        .word0 = GPUD_DRM_INLINE_IOCTL_REQUEST_MAGIC,
        .word1 = ioctl->handle,
        .word2 = ioctl->request,
    };
    lpr_memcpy(&request.word3, ioctl->data, ioctl->data_size);
    struct pacha_ipc_fd fds[PACHA_IPC_MAX_TRANSFER_FDS] = {0};
    struct pacha_ipc_msg reply = {
        .fds = fds, .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS,
    };
    const int64_t reply_fd = lpr_pacha_syscall2(PACHAOS_SYSCALL_IPC_CALL,
        LPR_GPUD_DRM_ENDPOINT_FD, (uint64_t)(uintptr_t)&request);
    if (reply_fd < 16)
        return lpr_pacha_status_to_errno(reply_fd);
    const int64_t status = lpr_native_ipc_recv_wait((uint64_t)reply_fd, &reply);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)reply_fd);
    if (status != 0)
        return lpr_pacha_status_to_errno(status);
    /* Never leak unexpected capabilities, including a malformed error reply. */
    for (uint32_t i = 0; i < reply.fd_count; ++i)
        if (fds[i].fd >= 16)
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, fds[i].fd);
    if (reply.word0 != GPUD_DRM_INLINE_IOCTL_REPLY_MAGIC ||
        reply.word2 != ioctl->request || reply.fd_count || reply.flags)
        return -LPR_LINUX_EIO;
    if (reply.word1 != 0)
        return (int64_t)reply.word1;
    lpr_memcpy(ioctl->data, &reply.word3, ioctl->data_size);
    return 0;
}

static void lpr_gpud_drm_drop_channel(int endpoint)
{
    if (endpoint < 16 || endpoint != lpr_drm_request_channel_fd) return;
    lpr_drm_request_channel_fd = 0;
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)endpoint);
}

static int lpr_gpud_drm_bind_page(int page_fd, void *page)
{
    if (page_fd != lpr_tty_wire_page_fd || page != lpr_tty_wire_page)
        return LPR_GPUD_DRM_ENDPOINT_FD;
    if (lpr_drm_request_channel_fd >= 16) return lpr_drm_request_channel_fd;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_CALL | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL;
    uint64_t pair[2] = {0, 0};
    int64_t status = lpr_pacha_syscall3(PACHAOS_SYSCALL_IPC_CHANNEL_CREATE,
        (uintptr_t)pair, rights, PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC);
    if (status) return (int)lpr_pacha_status_to_errno(status);
    const int auxiliary = lpr_drm_aux_cache_fd >= 16 && lpr_drm_aux_cache != 0;
    struct pacha_ipc_fd fds[3] = {
        {.fd = (uint64_t)(uint32_t)page_fd,
         .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE},
        {.fd = pair[1], .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_RECV |
            PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL},
        {.fd = (uint64_t)(uint32_t)lpr_drm_aux_cache_fd,
         .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE},
    };
    const struct pacha_ipc_msg request = {.word0 = GPUD_DRM_BIND_PAGE_REQUEST_MAGIC,
        .word1 = auxiliary ? GPUD_DRM_AUX_REUSE_BYTES : 0,
        .fds = fds, .fd_count = 2u + auxiliary};
    struct pacha_ipc_fd reply_fds[PACHA_IPC_MAX_TRANSFER_FDS];
    struct pacha_ipc_msg reply = {.fds = reply_fds, .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS};
    int64_t reply_fd = lpr_pacha_syscall2(PACHAOS_SYSCALL_IPC_CALL,
        LPR_GPUD_DRM_ENDPOINT_FD, (uintptr_t)&request);
    if (reply_fd < 16) status = lpr_pacha_status_to_errno(reply_fd);
    else {
        status = lpr_native_ipc_recv_wait((uint64_t)reply_fd, &reply);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)reply_fd);
        if (status) status = lpr_pacha_status_to_errno(status);
        else {
            for (uint32_t i = 0; i < reply.fd_count; ++i)
                if (reply_fds[i].fd >= 16)
                    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, reply_fds[i].fd);
            status = reply.word0 != GPUD_DRM_BIND_PAGE_REPLY_MAGIC ||
                reply.word2 || reply.word3 || reply.flags || reply.fd_count ?
                -LPR_LINUX_EIO : (int64_t)reply.word1;
        }
    }
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[1]);
    if (status) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, pair[0]);
        return (int)status;
    }
    lpr_drm_request_channel_fd = (int)pair[0];
    return lpr_drm_request_channel_fd;
}

static int64_t lpr_gpud_drm_call_transfers_many(
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    uint64_t *out_result,
    int *out_received_fds,
    uint32_t received_capacity,
    uint32_t *out_received_count,
    const int *transfer_fds,
    const uint8_t *temporary_vmos,
    uint32_t transfer_count)
{
    if (LPR_GPUD_DRM_ENDPOINT_FD < 16 || page_fd < 16 || page == 0 ||
        received_capacity > PACHA_IPC_MAX_TRANSFER_FDS ||
        (received_capacity != 0 && out_received_fds == 0) ||
        transfer_count > 3u ||
        (transfer_count != 0 &&
            (transfer_fds == 0 || temporary_vmos == 0))) {
        return -LPR_LINUX_ENODEV;
    }
    const int endpoint = lpr_gpud_drm_bind_page(page_fd, page);
    if (endpoint < 16) return endpoint;
    const int bound = endpoint != LPR_GPUD_DRM_ENDPOINT_FD;
    const int bound_aux = bound && transfer_count && temporary_vmos[0] &&
        transfer_fds[0] == lpr_drm_aux_cache_fd && lpr_drm_aux_cache_busy;
    struct pacha_ipc_fd fds[4];
    struct pacha_ipc_msg request;
    struct pacha_ipc_msg reply;
    struct pacha_ipc_fd reply_fds[PACHA_IPC_MAX_TRANSFER_FDS];
    lpr_memset(fds, 0, sizeof(fds));
    lpr_memset(&request, 0, sizeof(request));
    lpr_memset(&reply, 0, sizeof(reply));
    lpr_memset(reply_fds, 0, sizeof(reply_fds));
    for (uint32_t i = 0; i < received_capacity; ++i)
        out_received_fds[i] = -1;
    if (out_received_count != 0)
        *out_received_count = 0;
    reply.fds = reply_fds;
    reply.fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS;
    const uint64_t request_id = lpr_next_request_id(&lpr_termd_request_id);
    pacha_service_envelope_t *header = (pacha_service_envelope_t *)page;
    lpr_memset(header, 0, sizeof(*header));
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = GPUD_DRM_SERVICE_ID;
    header->op = op;
    header->flags = payload_size != 0 ? PACHA_SERVICE_FLAG_PAGE_PAYLOAD : 0;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = payload_size;
    fds[0].fd = (uint64_t)(uint32_t)page_fd;
    fds[0].rights = PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    for (uint32_t i = 0; i < transfer_count; i++) {
        const int transfer_fd = transfer_fds[i];
        const int temporary_vmo = temporary_vmos[i] != 0;
        struct pacha_fd_info info;
        const uint64_t temporary_rights = PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE;
        const uint64_t retained_rights = PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_SET_FLAGS;
        if (!lpr_native_fd_info((uint64_t)(uint32_t)transfer_fd, &info) ||
            (temporary_vmo ? info.kind != PACHA_FD_KIND_VMO :
                (info.kind != PACHA_FD_KIND_VMO && info.kind != PACHA_FD_KIND_CHANNEL)) ||
            (info.rights & (temporary_vmo ? temporary_rights : retained_rights)) !=
                (temporary_vmo ? temporary_rights : retained_rights)) {
            return -LPR_LINUX_EBADF;
        }
        fds[i + 1u].fd = (uint64_t)(uint32_t)transfer_fd;
        fds[i + 1u].rights = temporary_vmo ?
            PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE :
            info.kind == PACHA_FD_KIND_VMO ?
            PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP |
                PACHA_FD_RIGHT_SET_FLAGS | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE :
            PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL;
        /* Keep ownership local until IPC_CALL returns. This makes cleanup
         * unambiguous even if the request was queued but wakeup reports an
         * error after a MOVE would already have consumed the numeric FD. */
        fds[i + 1u].transfer_flags = 0;
    }
    request.word0 = PACHA_SERVICE_REQUEST_MAGIC;
    request.word1 = bound_aux ? GPUD_DRM_REQUEST_BOUND_AUX : 0;
    request.word3 = request_id;
    request.fds = fds + bound + bound_aux;
    request.fd_count = 1u + transfer_count - bound - bound_aux;
    const int64_t reply_fd = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_CALL,
        (uint64_t)(uint32_t)endpoint,
        (uint64_t)(uintptr_t)&request);
    if (reply_fd < 16) {
        lpr_gpud_drm_drop_channel(endpoint);
        return lpr_pacha_status_to_errno(reply_fd);
    }
    const int64_t recv_status = lpr_native_ipc_recv_wait(
        (uint64_t)(uint32_t)reply_fd,
        &reply);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)reply_fd);
    if (recv_status != 0) {
        /* Submission may already have run. Forget the failed connection for
         * a future call, but never replay this request on another endpoint. */
        lpr_gpud_drm_drop_channel(endpoint);
        return lpr_pacha_status_to_errno(recv_status);
    }
    const pacha_service_envelope_t *reply_header = (const pacha_service_envelope_t *)page;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC || reply.word3 != request_id ||
        reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        reply_header->service_id != GPUD_DRM_SERVICE_ID ||
        reply_header->op != op || reply_header->request_id != request_id) {
        lpr_gpud_drm_drop_channel(endpoint);
        for (uint32_t i = 0; i < reply.fd_count; ++i)
            if (reply_fds[i].fd >= 16)
                (void)lpr_pacha_syscall1(
                    PACHAOS_SYSCALL_FD_CLOSE, reply_fds[i].fd);
        return -LPR_LINUX_EIO;
    }
    if (out_result != 0) {
        *out_result = reply_header->result;
    }
    if (reply_header->status != 0 || reply.fd_count > received_capacity) {
        for (uint32_t i = 0; i < reply.fd_count; ++i)
            if (reply_fds[i].fd >= 16)
                (void)lpr_pacha_syscall1(
                    PACHAOS_SYSCALL_FD_CLOSE, reply_fds[i].fd);
        return reply_header->status != 0 ? reply_header->status : -LPR_LINUX_EIO;
    }
    for (uint32_t i = 0; i < reply.fd_count; ++i)
        if (reply_fds[i].fd < 16) {
            for (uint32_t j = 0; j < reply.fd_count; ++j)
                if (reply_fds[j].fd >= 16)
                    (void)lpr_pacha_syscall1(
                        PACHAOS_SYSCALL_FD_CLOSE, reply_fds[j].fd);
            return -LPR_LINUX_EIO;
        }
    for (uint32_t i = 0; i < reply.fd_count; ++i)
        out_received_fds[i] = (int)(uint32_t)reply_fds[i].fd;
    if (out_received_count != 0)
        *out_received_count = (uint32_t)reply.fd_count;
    return 0;
}

static int64_t lpr_gpud_drm_call_transfers(
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    uint64_t *out_result,
    int *out_received_fd,
    const int *transfer_fds,
    const uint8_t *temporary_vmos,
    uint32_t transfer_count)
{
    return lpr_gpud_drm_call_transfers_many(
        op, page_fd, page, payload_size, out_result, out_received_fd,
        out_received_fd != 0 ? 1u : 0u, 0,
        transfer_fds, temporary_vmos, transfer_count);
}

static int64_t lpr_gpud_drm_call_transfer(
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    uint64_t *out_result,
    int *out_received_fd,
    int transfer_fd,
    int temporary_vmo)
{
    const int transfer_fds[1] = {transfer_fd};
    const uint8_t temporary_vmos[1] = {temporary_vmo != 0};
    const uint32_t transfer_count = transfer_fd >= 16 ? 1u : 0u;
    return lpr_gpud_drm_call_transfers(
        op,
        page_fd,
        page,
        payload_size,
        out_result,
        out_received_fd,
        transfer_fds,
        temporary_vmos,
        transfer_count);
}

static int64_t lpr_gpud_drm_call(
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    uint64_t *out_result,
    int *out_received_fd)
{
    return lpr_gpud_drm_call_transfers(
        op, page_fd, page, payload_size, out_result, out_received_fd,
        0, 0, 0);
}

static int lpr_drm_aux_create(
    uint64_t size,
    int reusable,
    int *out_fd,
    void **out_mapping,
    uint64_t *out_map_size)
{
    if (size == 0 || size > GPUD_DRM_IOCTL_AUX_MAX_BYTES || out_fd == 0 ||
        out_mapping == 0 || out_map_size == 0) {
        return -LPR_LINUX_EINVAL;
    }
    /* All callers hold termd_rpc.lock_word through the synchronous reply.
     * A cached buffer must not be reused while that request owns it. */
    if (reusable && size <= GPUD_DRM_AUX_REUSE_BYTES && lpr_drm_aux_cache && !lpr_drm_aux_cache_busy) {
        lpr_drm_aux_cache_busy = 1;
        lpr_memset(lpr_drm_aux_cache, 0, size);
        *out_fd = lpr_drm_aux_cache_fd;
        *out_mapping = lpr_drm_aux_cache;
        *out_map_size = GPUD_DRM_AUX_REUSE_BYTES;
        return 0;
    }
    const int cacheable = reusable && size <= GPUD_DRM_AUX_REUSE_BYTES && !lpr_drm_aux_cache;
    const uint64_t map_size = cacheable ? GPUD_DRM_AUX_REUSE_BYTES :
        (size + 4095u) & ~UINT64_C(4095);
    if (map_size < size) return -LPR_LINUX_EINVAL;
    const uint64_t rights = PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t fd = lpr_pacha_syscall3(
        PACHAOS_SYSCALL_VMO_CREATE, map_size, rights,
        cacheable ? PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC : 0);
    if (fd < 16) return (int)lpr_pacha_status_to_errno(fd);
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        (uint64_t)(uint32_t)fd,
        0,
        map_size,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED,
        0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
        return (int)lpr_pacha_status_to_errno(mapped);
    }
    lpr_memset((void *)(uintptr_t)mapped, 0, map_size);
    *out_fd = (int)(uint32_t)fd;
    *out_mapping = (void *)(uintptr_t)mapped;
    *out_map_size = map_size;
    if (cacheable) {
        lpr_drm_aux_cache_fd = *out_fd;
        lpr_drm_aux_cache = *out_mapping;
        lpr_drm_aux_cache_busy = 1;
        /* The next binding transfers both pages. The old peer retains its
         * old mapping until channel hangup; no in-flight call is replayed. */
        lpr_gpud_drm_drop_channel(lpr_drm_request_channel_fd);
    }
    return 0;
}

static void lpr_drm_aux_destroy(int fd, void *mapping, uint64_t map_size)
{
    if (fd == lpr_drm_aux_cache_fd && mapping == lpr_drm_aux_cache &&
        map_size == GPUD_DRM_AUX_REUSE_BYTES) {
        lpr_drm_aux_cache_busy = 0;
        return;
    }
    if (mapping != 0 && map_size != 0) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP, (uint64_t)(uintptr_t)mapping, map_size);
    }
    if (fd >= 16) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
    }
}

static int lpr_drm_prepare_virtgpu_context(
    uint64_t argument,
    int reusable_aux,
    gpud_drm_ioctl_request_t *request,
    int *aux_fd,
    void **aux_mapping,
    uint64_t *aux_map_size)
{
    gpud_drm_virtgpu_context_init_t user;
    gpud_drm_virtgpu_context_wire_t wire;

    if (!argument || !request || !aux_fd || !aux_mapping || !aux_map_size)
        return -LPR_LINUX_EFAULT;
    lpr_memset(&wire, 0, sizeof(wire));
    lpr_memcpy(&user, (const void *)(uintptr_t)argument, sizeof(user));
    if (user.pad || !user.num_params || user.num_params > 4 ||
        !user.ctx_set_params)
        return -LPR_LINUX_EINVAL;

    const gpud_drm_virtgpu_context_set_param_t *params =
        (const void *)(uintptr_t)user.ctx_set_params;
    const char *debug_name = 0;
    for (uint32_t i = 0; i < user.num_params; ++i) {
        uint32_t bit;

        switch (params[i].param) {
        case GPUD_DRM_VIRTGPU_CONTEXT_PARAM_CAPSET_ID:
            bit = GPUD_DRM_VIRTGPU_CONTEXT_HAS_CAPSET_ID;
            if (params[i].value > 63) return -LPR_LINUX_EINVAL;
            wire.capset_id = (uint32_t)params[i].value;
            break;
        case GPUD_DRM_VIRTGPU_CONTEXT_PARAM_NUM_RINGS:
            bit = GPUD_DRM_VIRTGPU_CONTEXT_HAS_NUM_RINGS;
            if (params[i].value > 64) return -LPR_LINUX_EINVAL;
            wire.ring_count = (uint32_t)params[i].value;
            break;
        case GPUD_DRM_VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK:
            bit = GPUD_DRM_VIRTGPU_CONTEXT_HAS_POLL_RINGS_MASK;
            wire.poll_ring_mask = params[i].value;
            break;
        case GPUD_DRM_VIRTGPU_CONTEXT_PARAM_DEBUG_NAME:
            bit = GPUD_DRM_VIRTGPU_CONTEXT_HAS_DEBUG_NAME;
            if (!params[i].value) return -LPR_LINUX_EINVAL;
            debug_name = (const void *)(uintptr_t)params[i].value;
            break;
        default:
            return -LPR_LINUX_EINVAL;
        }
        if (wire.parameter_mask & bit) return -LPR_LINUX_EINVAL;
        wire.parameter_mask |= bit;
    }
    if ((wire.parameter_mask & GPUD_DRM_VIRTGPU_CONTEXT_HAS_POLL_RINGS_MASK) &&
        (!(wire.parameter_mask & GPUD_DRM_VIRTGPU_CONTEXT_HAS_NUM_RINGS) ||
         !wire.ring_count ||
         (wire.ring_count < 64 &&
          (wire.poll_ring_mask >> wire.ring_count))))
        return -LPR_LINUX_EINVAL;

    if (debug_name) {
        uint32_t bytes = 0;

        do {
            ++bytes;
        } while (bytes < GPUD_DRM_VIRTGPU_CONTEXT_DEBUG_NAME_MAX_BYTES &&
                 debug_name[bytes - 1] != '\0');
        int result = lpr_drm_aux_create(
            bytes, reusable_aux, aux_fd, aux_mapping, aux_map_size);
        if (result)
            return result;
        lpr_memcpy(*aux_mapping, debug_name, bytes);
        wire.debug_name_bytes = bytes;
        request->aux_size = bytes;
    }
    lpr_memcpy(request->data, &wire, sizeof(wire));
    request->arg_size = sizeof(user);
    request->data_size = sizeof(wire);
    return 0;
}

static int lpr_drm_prepare_atomic(
    uint64_t arg,
    gpud_drm_mode_atomic_wire_t *wire,
    int *out_input_wait_fd)
{
    if (arg == 0 || wire == 0 || out_input_wait_fd == 0) {
        return -LPR_LINUX_EFAULT;
    }
    *out_input_wait_fd = -1;
    const lpr_drm_mode_atomic_t *atomic =
        (const lpr_drm_mode_atomic_t *)(uintptr_t)arg;
    if (atomic->reserved != 0 ||
        atomic->count_objs > GPUD_DRM_ATOMIC_OBJECT_CAPACITY ||
        (atomic->count_objs != 0 &&
            (atomic->objs_ptr == 0 || atomic->count_props_ptr == 0))) {
        return -LPR_LINUX_EINVAL;
    }

    wire->flags = atomic->flags;
    wire->count_objs = atomic->count_objs;
    /* drmModeAtomicCommit user_data is a process-local opaque cookie and is
     * commonly a heap pointer. It is tokenized before crossing into gpud. */
    wire->user_data = 0;
    if (atomic->count_objs != 0) {
        lpr_memcpy(
            wire->objects,
            (const void *)(uintptr_t)atomic->objs_ptr,
            (uint64_t)atomic->count_objs * sizeof(wire->objects[0]));
        lpr_memcpy(
            wire->object_prop_counts,
            (const void *)(uintptr_t)atomic->count_props_ptr,
            (uint64_t)atomic->count_objs *
                sizeof(wire->object_prop_counts[0]));
    }

    uint32_t total_props = 0;
    for (uint32_t i = 0; i < atomic->count_objs; i++) {
        if (wire->object_prop_counts[i] >
            GPUD_DRM_ATOMIC_PROPERTY_CAPACITY - total_props) {
            return -LPR_LINUX_EINVAL;
        }
        total_props += wire->object_prop_counts[i];
    }
    if (total_props != 0 &&
        (atomic->props_ptr == 0 || atomic->prop_values_ptr == 0)) {
        return -LPR_LINUX_EINVAL;
    }
    wire->total_props = total_props;
    if (total_props != 0) {
        lpr_memcpy(
            wire->props,
            (const void *)(uintptr_t)atomic->props_ptr,
            (uint64_t)total_props * sizeof(wire->props[0]));
        lpr_memcpy(
            wire->prop_values,
            (const void *)(uintptr_t)atomic->prop_values_ptr,
            (uint64_t)total_props * sizeof(wire->prop_values[0]));
    }

    int saw_in_fence = 0;
    for (uint32_t i = 0; i < total_props; i++) {
        if (wire->props[i] != GPUD_DRM_KMS_PLANE_IN_FENCE_FD_PROP_ID) continue;
        if (saw_in_fence) {
            if (*out_input_wait_fd >= 16) {
                (void)lpr_close_native_fd_if_open(
                    (uint64_t)(uint32_t)*out_input_wait_fd);
            }
            *out_input_wait_fd = -1;
            return -LPR_LINUX_EINVAL;
        }
        saw_in_fence = 1;
        if (wire->prop_values[i] == UINT64_MAX) continue;
        if (wire->prop_values[i] > INT32_MAX ||
            !lpr_linux_sync_file_fd_active(wire->prop_values[i])) {
            return wire->prop_values[i] > INT32_MAX ?
                -LPR_LINUX_EINVAL : -LPR_LINUX_EBADF;
        }
        const int wait_fd = lpr_sync_file_duplicate_wait(wire->prop_values[i]);
        if (wait_fd < 0) return wait_fd;
        *out_input_wait_fd = wait_fd;
        /* Native capabilities are transported out-of-band. */
        wire->prop_values[i] = 0;
    }
    return 0;
}

int64_t lpr_drm_open_path(const char *path, uint64_t flags)
{
    const int udmabuf = path != 0 && lpr_strcmp(path, "/dev/udmabuf") == 0;
    const int render = path != 0 && lpr_strcmp(path, "/dev/dri/renderD128") == 0;
    if (path == 0 || (!udmabuf && !render && lpr_strcmp(path, "/dev/dri/card0") != 0)) {
        return -LPR_LINUX_ENOENT;
    }
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    gpud_drm_open_request_t *open = (gpud_drm_open_request_t *)lpr_gpud_drm_payload(page);
    lpr_memset(open, 0, sizeof(*open));
    open->device_minor = render ? 128u : 0u;
    open->flags = flags;
    int native_wait_fd = -1;
    int remote_wait_fd = -1;
    const int pair_status = lpr_native_wait_pair(&native_wait_fd, &remote_wait_fd);
    if (pair_status != 0) {
        lpr_destroy_tty_wire_page(page_fd, page);
        return pair_status;
    }
    uint64_t handle = 0;
    const int64_t status = lpr_gpud_drm_call_transfer(
        GPUD_DRM_OP_OPEN_NODE, page_fd, page, sizeof(*open), &handle, 0,
        remote_wait_fd, 0);
    (void)lpr_close_native_fd_if_open(
        (uint64_t)(uint32_t)remote_wait_fd);
    remote_wait_fd = -1;
    lpr_destroy_tty_wire_page(page_fd, page);
    if (status != 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)native_wait_fd);
        return status;
    }
    const int fd = lpr_drm_install(handle, flags, native_wait_fd,
        udmabuf ? LPR_DRM_NODE_UDMABUF : render ? LPR_DRM_NODE_RENDER : 0);
    if (fd < 0) {
        (void)lpr_drm_close_handle(handle);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)native_wait_fd);
    }
    return fd;
}

int64_t lpr_drm_stat_path(const char *path, uint64_t statbuf)
{
    uint32_t major = 0, minor = 0;
    if (path != 0 && lpr_strcmp(path, "/dev/dri/card0") == 0) {
        major = 226;
    } else if (path != 0 && lpr_strcmp(path, "/dev/dri/renderD128") == 0) {
        major = 226;
        minor = 128;
    } else if (path != 0 && lpr_strcmp(path, "/dev/udmabuf") == 0) {
        major = 10;
        minor = 60;
    } else {
        return -LPR_LINUX_ENOENT;
    }
    if (statbuf == 0) return -LPR_LINUX_EFAULT;
    lpr_linux_stat_t *st = (lpr_linux_stat_t *)(uintptr_t)statbuf;
    lpr_memset(st, 0, sizeof(*st));
    st->st_ino = UINT64_C(0x64726900) + minor;
    st->st_nlink = 1;
    st->st_mode = LPR_LINUX_S_IFCHR | 0660u;
    st->st_rdev = ((uint64_t)major << 8u) | minor;
    st->st_blksize = 4096;
    return 0;
}

int64_t lpr_drm_close_handle(uint64_t handle)
{
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    gpud_drm_handle_request_t *close = (gpud_drm_handle_request_t *)lpr_gpud_drm_payload(page);
    lpr_memset(close, 0, sizeof(*close));
    close->handle = handle;
    const int64_t status = lpr_gpud_drm_call(GPUD_DRM_OP_HANDLE_CLOSE, page_fd, page, sizeof(*close), 0, 0);
    lpr_destroy_tty_wire_page(page_fd, page);
    lpr_drm_event_cookie_cancel_handle(handle);
    return status;
}

int64_t lpr_drm_dup_handle(uint64_t handle)
{
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    gpud_drm_handle_request_t *dup = (gpud_drm_handle_request_t *)lpr_gpud_drm_payload(page);
    lpr_memset(dup, 0, sizeof(*dup));
    dup->handle = handle;
    uint64_t result = 0;
    const int64_t status = lpr_gpud_drm_call(GPUD_DRM_OP_HANDLE_DUP, page_fd, page, sizeof(*dup), &result, 0);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status == 0 && result == handle ? 0 : (status != 0 ? status : -LPR_LINUX_EIO);
}

int64_t lpr_drm_transfer_dup_handle(
    uint64_t handle,
    int lease_fd,
    uint64_t *out_handle)
{
    if (out_handle == 0 || lease_fd < 16) return -LPR_LINUX_EINVAL;
    *out_handle = 0;
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    gpud_drm_handle_request_t *dup = (gpud_drm_handle_request_t *)lpr_gpud_drm_payload(page);
    lpr_memset(dup, 0, sizeof(*dup));
    dup->handle = handle;
    const int64_t status = lpr_gpud_drm_call_transfer(
        GPUD_DRM_OP_HANDLE_DUP,
        page_fd,
        page,
        sizeof(*dup),
        out_handle,
        0,
        lease_fd,
        0);
    lpr_destroy_tty_wire_page(page_fd, page);
    if (status == 0 && *out_handle == handle) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)lease_fd);
        return 0;
    }
    return status != 0 ? status : -LPR_LINUX_EIO;
}

int64_t lpr_drm_prime_ref(uint32_t op, uint64_t token)
{
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    gpud_drm_prime_token_request_t *request = (gpud_drm_prime_token_request_t *)lpr_gpud_drm_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->token = token;
    const int64_t status = lpr_gpud_drm_call(op, page_fd, page, sizeof(*request), 0, 0);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status;
}

int64_t lpr_drm_prime_transfer_acquire(uint64_t token, int lease_fd)
{
    if (token == 0 || lease_fd < 16) return -LPR_LINUX_EINVAL;
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    gpud_drm_prime_token_request_t *request =
        (gpud_drm_prime_token_request_t *)lpr_gpud_drm_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->token = token;
    const int64_t status = lpr_gpud_drm_call_transfer(
        GPUD_DRM_OP_PRIME_ACQUIRE,
        page_fd,
        page,
        sizeof(*request),
        0,
        0,
        lease_fd,
        0);
    lpr_destroy_tty_wire_page(page_fd, page);
    if (status == 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)lease_fd);
    }
    return status;
}

int64_t lpr_dmabuf_mmap(uint64_t fd, uint64_t address, uint64_t length,
    uint64_t prot, uint64_t flags, uint64_t offset)
{
    lpr_dmabuf_backend_t *dmabuf = lpr_dmabuf_backend(fd);
    if (dmabuf == 0)
        return -LPR_LINUX_EBADF;
    if (dmabuf->native.raw < 16 || (offset & 4095ull) != 0 ||
        offset > dmabuf->size || length > dmabuf->size - offset ||
        ((prot & PACHAOS_PROT_WRITE) != 0 && !dmabuf->writable))
        return -LPR_LINUX_EINVAL;
    int lease_fd = -1;
    if (dmabuf->token == 0) {
        struct pacha_fd_info info;
        lpr_memset(&info, 0, sizeof(info));
        if (!lpr_native_fd_info(
                (uint64_t)(uint32_t)dmabuf->native.raw, &info) ||
            info.kind != PACHA_FD_KIND_VMO ||
            (info.rights & PACHA_FD_RIGHT_DUP) == 0)
            return -LPR_LINUX_EBADF;
        const int64_t duplicate = lpr_pacha_syscall4(
            PACHA_FD_SYSCALL_DUP,
            (uint64_t)(uint32_t)dmabuf->native.raw,
            16,
            PACHA_FD_RIGHT_CLOSE,
            PACHA_FD_FLAG_CLOEXEC | PACHA_FD_FLAG_INHERIT);
        if (duplicate < 16)
            return lpr_pacha_status_to_errno(duplicate);
        lease_fd = (int)duplicate;
        const int64_t mapped = lpr_drm_map_received(
            dmabuf->native.raw, lease_fd,
            address, length, prot, flags, offset);
        if (mapped < 0)
            (void)lpr_close_native_fd_if_open(
                (uint64_t)(uint32_t)lease_fd);
        return mapped;
    }
    int remote_lease_fd = -1;
    /* fork retains the mapping, exec replaces it. The new LPR image cannot
     * recover this private mapping registry, so its lease must close on exec. */
    int status = lpr_native_wait_pair_flags(&lease_fd, &remote_lease_fd,
        PACHA_FD_FLAG_CLOEXEC | PACHA_FD_FLAG_INHERIT);
    if (!status)
        status = (int)lpr_drm_prime_transfer_acquire(
            dmabuf->token, remote_lease_fd);
    if (status) {
        if (lease_fd >= 16)
            (void)lpr_close_native_fd_if_open(
                (uint64_t)(uint32_t)lease_fd);
        if (remote_lease_fd >= 16)
            (void)lpr_close_native_fd_if_open(
                (uint64_t)(uint32_t)remote_lease_fd);
        return status;
    }
    const int64_t mapped = lpr_drm_map_received(dmabuf->native.raw,
        lease_fd, address, length, prot, flags, offset);
    if (mapped < 0)
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)lease_fd);
    return mapped;
}

static int lpr_dma_buf_sync_flags_valid(uint32_t flags)
{
    return (flags & LPR_LINUX_DMA_BUF_SYNC_RW) != 0 &&
        (flags & ~LPR_LINUX_DMA_BUF_SYNC_RW) == 0;
}

int64_t lpr_dmabuf_ioctl(uint64_t fd, uint64_t request, uint64_t arg)
{
    lpr_dmabuf_backend_t *dmabuf = lpr_dmabuf_backend(fd);
    const uint32_t command = (uint32_t)request;
    if (dmabuf == 0) return -LPR_LINUX_EBADF;
    if (arg == 0) return -LPR_LINUX_EFAULT;
    lpr_dma_buf_sync_file_t *sync =
        (lpr_dma_buf_sync_file_t *)(uintptr_t)arg;
    if (!lpr_dma_buf_sync_flags_valid(sync->flags))
        return -LPR_LINUX_EINVAL;

    if (command == (uint32_t)LPR_LINUX_DMA_BUF_IOCTL_EXPORT_SYNC_FILE) {
        const int64_t sync_fd = lpr_sync_file_create_signaled();
        if (sync_fd < 0) return sync_fd;
        sync->fd = (int32_t)sync_fd;
        return 0;
    }
    if (command != (uint32_t)LPR_LINUX_DMA_BUF_IOCTL_IMPORT_SYNC_FILE)
        return -LPR_LINUX_ENOTTY;
    if (sync->fd < 0 ||
        !lpr_linux_sync_file_fd_active((uint64_t)(uint32_t)sync->fd))
        return -LPR_LINUX_EBADF;

    const int wait_fd = lpr_sync_file_duplicate_wait(
        (uint64_t)(uint32_t)sync->fd);
    if (wait_fd < 0) return wait_fd;
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
        return page_fd;
    }
    gpud_drm_prime_token_request_t *import =
        (gpud_drm_prime_token_request_t *)lpr_gpud_drm_payload(page);
    lpr_memset(import, 0, sizeof(*import));
    import->token = dmabuf->token;
    const int64_t status = lpr_gpud_drm_call_transfer(
        GPUD_DRM_OP_PRIME_IMPORT_SYNC_FILE,
        page_fd,
        page,
        sizeof(*import),
        0,
        0,
        wait_fd,
        0);
    lpr_destroy_tty_wire_page(page_fd, page);
    (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)wait_fd);
    return status;
}

static int64_t lpr_drm_prime_export(uint64_t drm_handle, uint32_t gem_handle, uint32_t flags)
{
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    gpud_drm_prime_export_request_t *request = (gpud_drm_prime_export_request_t *)lpr_gpud_drm_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->handle = drm_handle;
    request->gem_handle = gem_handle;
    request->flags = flags;
    uint64_t token = 0;
    int received[2] = {-1, -1};
    uint32_t received_count = 0;
    int64_t status = lpr_gpud_drm_call_transfers_many(
        GPUD_DRM_OP_PRIME_EXPORT, page_fd, page, sizeof(*request),
        &token, received, 2, &received_count, 0, 0, 0);
    lpr_destroy_tty_wire_page(page_fd, page);
    if (status != 0) return status;
    struct pacha_fd_info info;
    struct pacha_fd_info lease;
    const int native_fd = received[0];
    const uint64_t view_rights = PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    if (received_count != 2 || native_fd < 16 || token == 0 || !lpr_native_fd_info((uint64_t)(uint32_t)native_fd, &info) ||
        info.kind != PACHA_FD_KIND_VMO || !info.size || (info.size & 4095u) ||
        info.rights != view_rights || info.flags != PACHA_FD_FLAG_CLOEXEC ||
        !lpr_native_fd_info((uint64_t)(uint32_t)received[1], &lease) ||
        lease.kind != PACHA_FD_KIND_CHANNEL || lease.size ||
        lease.rights != PACHA_FD_RIGHT_CLOSE ||
        lease.flags != (PACHA_FD_FLAG_CLOEXEC | PACHA_FD_FLAG_INHERIT)) {
        for (uint32_t i = 0; i < received_count; ++i)
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)received[i]);
        return -LPR_LINUX_EIO;
    }
    const uint64_t linux_flags = LPR_LINUX_O_RDWR |
        ((flags & GPUD_DRM_CLOEXEC) != 0 ? LPR_LINUX_O_CLOEXEC : 0);
    const int linux_fd = lpr_dmabuf_install(native_fd, received[1], token, info.size, linux_flags);
    if (linux_fd < 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)native_fd);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)received[1]);
    }
    return linux_fd;
}

static int64_t lpr_drm_prime_import(uint64_t drm_handle, uint64_t prime_fd, uint32_t flags)
{
    lpr_dmabuf_backend_t *dmabuf = lpr_dmabuf_backend(prime_fd);
    if (flags != 0) return -LPR_LINUX_EINVAL;
    int import_vmo_fd = -1;
    uint64_t import_size = 0;
    if (dmabuf == 0) {
        if (!lpr_linux_filed_fd_active(prime_fd)) return -LPR_LINUX_EBADF;
        lpr_linux_stat_t st;
        lpr_memset(&st, 0, sizeof(st));
        const int64_t stat_status = lpr_linux_fstat(
            prime_fd, (uint64_t)(uintptr_t)&st);
        if (stat_status != 0) return stat_status;
        if (st.st_size <= 0 || (uint64_t)st.st_size > 256u * 1024u * 1024u) {
            return -LPR_LINUX_EINVAL;
        }
        import_size = ((uint64_t)st.st_size + 4095u) & ~4095ull;
        uint64_t file_size = 0;
        const int64_t shared_vmo_fd = lpr_linux_shared_file_vmo(
            prime_fd, 0, import_size, 1, 0, &file_size);
        if (shared_vmo_fd < 16) return shared_vmo_fd;
        if (file_size < (uint64_t)st.st_size) {
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)shared_vmo_fd);
            return -LPR_LINUX_EIO;
        }
        import_vmo_fd = (int)(uint32_t)shared_vmo_fd;
    }
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        if (import_vmo_fd >= 16) (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)import_vmo_fd);
        return page_fd;
    }
    gpud_drm_prime_import_request_t *request = (gpud_drm_prime_import_request_t *)lpr_gpud_drm_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->handle = drm_handle;
    request->token = dmabuf != 0 ? dmabuf->token : 0;
    request->size = import_size;
    uint64_t gem_handle = 0;
    const int64_t status = lpr_gpud_drm_call_transfer(
        GPUD_DRM_OP_PRIME_IMPORT, page_fd, page, sizeof(*request), &gem_handle, 0,
        import_vmo_fd, 0);
    lpr_destroy_tty_wire_page(page_fd, page);
    if (import_vmo_fd >= 16) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)import_vmo_fd);
    }
    return status == 0 && gem_handle <= UINT32_MAX ? (int64_t)gem_handle :
        (status != 0 ? status : -LPR_LINUX_EIO);
}

static int64_t lpr_udmabuf_create(uint64_t drm_handle, uint64_t arg)
{
    (void)drm_handle;
    if (arg == 0) return -LPR_LINUX_EFAULT;
    const lpr_udmabuf_create_t *create = (const void *)(uintptr_t)arg;
    if ((create->flags & ~1u) != 0 || create->offset != 0 || create->size == 0 ||
        (create->size & 4095u) != 0 || !lpr_linux_filed_fd_active(create->memfd)) {
        return -LPR_LINUX_EINVAL;
    }
    lpr_linux_stat_t st;
    lpr_memset(&st, 0, sizeof(st));
    const int64_t stat_status = lpr_linux_fstat(
        create->memfd, (uint64_t)(uintptr_t)&st);
    if (stat_status != 0) return stat_status;
    if (st.st_size <= 0 || create->size > (uint64_t)st.st_size) return -LPR_LINUX_EINVAL;
    uint64_t file_size = 0;
    const int64_t native_fd = lpr_linux_shared_file_vmo(
        create->memfd, create->offset, create->size, 1, 0, &file_size);
    if (native_fd < 16) return native_fd;
    if (file_size < create->size) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)native_fd);
        return -LPR_LINUX_EIO;
    }
    const uint64_t linux_flags = LPR_LINUX_O_RDWR |
        ((create->flags & 1u) != 0 ? LPR_LINUX_O_CLOEXEC : 0);
    const int linux_fd = lpr_dmabuf_install((int)native_fd, -1, 0, create->size, linux_flags);
    if (linux_fd < 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)native_fd);
    }
    return linux_fd;
}

int64_t lpr_drm_ioctl(uint64_t fd, uint64_t request, uint64_t arg)
{
#if defined(LPR_DRM_STARTUP_PROFILE) && LPR_DRM_STARTUP_PROFILE
    const uint64_t profile_start = pacha_trace_read_tsc();
#endif
    lpr_drm_backend_t *drm = lpr_drm_backend(fd);
    const uint32_t command = (uint32_t)request;
    const int no_argument = command == GPUD_DRM_IOCTL_SET_MASTER || command == GPUD_DRM_IOCTL_DROP_MASTER;
    if (drm == 0 || (arg == 0 && !no_argument)) {
        return drm == 0 ? -LPR_LINUX_EBADF : -LPR_LINUX_EFAULT;
    }
    if (drm->node_kind == LPR_DRM_NODE_UDMABUF) {
        return command == LPR_UDMABUF_CREATE ?
            lpr_udmabuf_create(drm->handle, arg) : -LPR_LINUX_ENOTTY;
    }
    if (command == GPUD_DRM_IOCTL_PRIME_HANDLE_TO_FD) {
        gpud_drm_prime_handle_t *prime = (gpud_drm_prime_handle_t *)(uintptr_t)arg;
        const int64_t prime_fd = lpr_drm_prime_export(drm->handle, prime->handle, prime->flags);
        if (prime_fd >= 0) prime->fd = (int32_t)prime_fd;
        return prime_fd >= 0 ? 0 : prime_fd;
    }
    if (command == GPUD_DRM_IOCTL_PRIME_FD_TO_HANDLE) {
        gpud_drm_prime_handle_t *prime = (gpud_drm_prime_handle_t *)(uintptr_t)arg;
        if (prime->fd < 0) return -LPR_LINUX_EBADF;
        const int64_t gem_handle = lpr_drm_prime_import(
            drm->handle, (uint64_t)(uint32_t)prime->fd, prime->flags);
        if (gem_handle >= 0) prime->handle = (uint32_t)gem_handle;
        return gem_handle >= 0 ? 0 : gem_handle;
    }
    if (command == GPUD_DRM_IOCTL_SYNCOBJ_EVENTFD ||
        command == LPR_DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT ||
        command == LPR_DRM_IOCTL_SYNCOBJ_QUERY ||
        command == LPR_DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL) {
        /* Client-facing timeline synchronization is intentionally not advertised. */
        return -LPR_LINUX_EOPNOTSUPP;
    }
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
#if defined(LPR_DRM_STARTUP_PROFILE) && LPR_DRM_STARTUP_PROFILE
    const uint64_t profile_page_end = pacha_trace_read_tsc();
#endif
    gpud_drm_ioctl_request_t *ioctl = (gpud_drm_ioctl_request_t *)lpr_gpud_drm_payload(page);
    /* Temporary control pages cannot borrow a connection-owned auxiliary
     * mapping: their receiver still validates an exact-size temporary VMO. */
    const int reusable_aux = page_fd == lpr_tty_wire_page_fd && page == lpr_tty_wire_page;
    lpr_memset(ioctl, 0, sizeof(*ioctl));
    ioctl->handle = drm->handle;
    /* Linux ioctl commands are unsigned 32-bit values.  musl's int
     * parameter may arrive sign-extended in the syscall register. */
    ioctl->request = command;
    int aux_fd = -1;
    void *aux_mapping = 0;
    uint64_t aux_map_size = 0;
    int input_wait_fd = -1;
    int output_wait_fd = -1;
    int output_notify_fd = -1;
    int output_sync_fd = -1;
    uint64_t event_token = 0;
    enum {
        LPR_DRM_WIRE_GENERIC,
        LPR_DRM_WIRE_VERSION,
        LPR_DRM_WIRE_RESOURCES,
        LPR_DRM_WIRE_CONNECTOR,
        LPR_DRM_WIRE_CRTC,
        LPR_DRM_WIRE_DIRTY_FB,
        LPR_DRM_WIRE_PLANE_RES,
        LPR_DRM_WIRE_PLANE,
        LPR_DRM_WIRE_OBJECT_PROPERTIES,
        LPR_DRM_WIRE_PROPERTY,
        LPR_DRM_WIRE_PROPERTY_BLOB,
        LPR_DRM_WIRE_CREATE_PROPERTY_BLOB,
        LPR_DRM_WIRE_ATOMIC,
        LPR_DRM_WIRE_SYNCOBJ_FD_TO_HANDLE,
        LPR_DRM_WIRE_SYNCOBJ_HANDLE_TO_FD,
        LPR_DRM_WIRE_SYNCOBJ_WAIT,
        LPR_DRM_WIRE_SYNCOBJ_ARRAY,
        LPR_DRM_WIRE_NO_ARGUMENT,
        LPR_DRM_WIRE_VIRTGPU_GETPARAM,
        LPR_DRM_WIRE_VIRTGPU_GET_CAPS,
        LPR_DRM_WIRE_VIRTGPU_EXECBUFFER,
        LPR_DRM_WIRE_VIRTGPU_CONTEXT_INIT,
    } wire_kind = LPR_DRM_WIRE_GENERIC;
    if (command == LPR_DRM_IOCTL_SYNCOBJ_WAIT) {
        gpud_drm_syncobj_wait_t wire;
        lpr_memcpy(&wire, (const void *)(uintptr_t)arg, sizeof(wire));
        const uint64_t handles_bytes =
            (uint64_t)wire.count_handles * sizeof(uint32_t);
        if (!wire.handles || !wire.count_handles ||
            wire.count_handles > GPUD_DRM_VIRTGPU_EXEC_HANDLE_MAX_COUNT ||
            handles_bytes > GPUD_DRM_IOCTL_AUX_MAX_BYTES || wire.pad ||
            (wire.flags & ~15u) || (!(wire.flags & 8u) && wire.deadline_nsec) ||
            lpr_drm_aux_create(handles_bytes, reusable_aux, &aux_fd, &aux_mapping,
                &aux_map_size) != 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EINVAL;
        }
        lpr_memcpy(aux_mapping, (const void *)(uintptr_t)wire.handles,
            handles_bytes);
        wire.handles = 0;
        wire.first_signaled = 0;
        lpr_memcpy(ioctl->data, &wire, sizeof(wire));
        ioctl->arg_size = sizeof(wire);
        ioctl->data_size = sizeof(wire);
        ioctl->aux_size = handles_bytes;
        wire_kind = LPR_DRM_WIRE_SYNCOBJ_WAIT;
    } else if (command == LPR_DRM_IOCTL_SYNCOBJ_RESET ||
        command == LPR_DRM_IOCTL_SYNCOBJ_SIGNAL) {
        gpud_drm_syncobj_array_t wire;
        lpr_memcpy(&wire, (const void *)(uintptr_t)arg, sizeof(wire));
        const uint64_t handles_bytes =
            (uint64_t)wire.count_handles * sizeof(uint32_t);
        if (!wire.handles || !wire.count_handles ||
            wire.count_handles > GPUD_DRM_VIRTGPU_EXEC_HANDLE_MAX_COUNT ||
            handles_bytes > GPUD_DRM_IOCTL_AUX_MAX_BYTES || wire.pad ||
            lpr_drm_aux_create(handles_bytes, reusable_aux, &aux_fd, &aux_mapping,
                &aux_map_size) != 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EINVAL;
        }
        lpr_memcpy(aux_mapping, (const void *)(uintptr_t)wire.handles,
            handles_bytes);
        wire.handles = 0;
        lpr_memcpy(ioctl->data, &wire, sizeof(wire));
        ioctl->arg_size = sizeof(wire);
        ioctl->data_size = sizeof(wire);
        ioctl->aux_size = handles_bytes;
        wire_kind = LPR_DRM_WIRE_SYNCOBJ_ARRAY;
    } else if (command == LPR_DRM_IOCTL_VERSION) {
        lpr_drm_version_t *version = (lpr_drm_version_t *)(uintptr_t)arg;
        gpud_drm_version_wire_t *wire = (gpud_drm_version_wire_t *)ioctl->data;
        wire->name_capacity = version->name_len;
        wire->date_capacity = version->date_len;
        wire->desc_capacity = version->desc_len;
        ioctl->arg_size = sizeof(*version);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_VERSION;
    } else if (command == GPUD_DRM_IOCTL_MODE_GETRESOURCES) {
        gpud_drm_kms_resources_wire_t *wire = (gpud_drm_kms_resources_wire_t *)ioctl->data;
        lpr_memcpy(&wire->value, (const void *)(uintptr_t)arg, sizeof(wire->value));
        ioctl->arg_size = sizeof(wire->value);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_RESOURCES;
    } else if (command == GPUD_DRM_IOCTL_MODE_GETCONNECTOR) {
        gpud_drm_kms_connector_wire_t *wire = (gpud_drm_kms_connector_wire_t *)ioctl->data;
        lpr_memcpy(&wire->value, (const void *)(uintptr_t)arg, sizeof(wire->value));
        ioctl->arg_size = sizeof(wire->value);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_CONNECTOR;
    } else if (command == GPUD_DRM_IOCTL_MODE_GETCRTC || command == GPUD_DRM_IOCTL_MODE_SETCRTC) {
        gpud_drm_kms_crtc_wire_t *wire = (gpud_drm_kms_crtc_wire_t *)ioctl->data;
        lpr_memcpy(&wire->value, (const void *)(uintptr_t)arg, sizeof(wire->value));
        if (command == GPUD_DRM_IOCTL_MODE_SETCRTC && wire->value.set_connectors_ptr != 0) {
            uint32_t count = wire->value.count_connectors;
            if (count > GPUD_DRM_KMS_CONNECTOR_CAPACITY) count = GPUD_DRM_KMS_CONNECTOR_CAPACITY;
            lpr_memcpy(wire->connectors, (const void *)(uintptr_t)wire->value.set_connectors_ptr, count * sizeof(uint32_t));
        }
        ioctl->arg_size = sizeof(wire->value);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_CRTC;
    } else if (command == GPUD_DRM_IOCTL_MODE_PAGE_FLIP) {
        gpud_drm_mode_crtc_page_flip_t wire;
        lpr_memcpy(&wire, (const void *)(uintptr_t)arg, sizeof(wire));
        if (!wire.crtc_id || !wire.fb_id ||
            (wire.flags & ~GPUD_DRM_MODE_PAGE_FLIP_FLAGS) ||
            (wire.flags & GPUD_DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE &&
             wire.flags & GPUD_DRM_MODE_PAGE_FLIP_TARGET_RELATIVE) ||
            (!(wire.flags & (GPUD_DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE |
                             GPUD_DRM_MODE_PAGE_FLIP_TARGET_RELATIVE)) &&
             wire.reserved)) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EINVAL;
        }
        if (wire.flags & GPUD_DRM_MODE_PAGE_FLIP_EVENT) {
            const int cookie_status = lpr_drm_event_cookie_register(
                drm->handle, wire.user_data, &event_token);
            if (cookie_status != 0) {
                lpr_destroy_tty_wire_page(page_fd, page);
                return cookie_status;
            }
            wire.user_data = event_token;
        } else {
            wire.user_data = 0;
        }
        lpr_memcpy(ioctl->data, &wire, sizeof(wire));
        ioctl->arg_size = sizeof(wire);
        ioctl->data_size = sizeof(wire);
    } else if (command == GPUD_DRM_IOCTL_MODE_DIRTYFB) {
        gpud_drm_mode_fb_dirty_t wire;
        lpr_memcpy(&wire, (const void *)(uintptr_t)arg, sizeof(wire));
        if ((wire.flags & ~GPUD_DRM_MODE_DIRTY_FLAGS) ||
            wire.num_clips > GPUD_DRM_MODE_DIRTY_MAX_CLIPS ||
            (!!wire.num_clips != !!wire.clips_ptr) ||
            ((wire.flags & GPUD_DRM_MODE_DIRTY_ANNOTATE_COPY) &&
                (wire.num_clips & 1u))) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EINVAL;
        }
        if (wire.num_clips) {
            const uint64_t rectangle_bytes =
                (uint64_t)wire.num_clips * sizeof(gpud_drm_mode_rectangle_t);
            const int aux_status = lpr_drm_aux_create(
                rectangle_bytes, reusable_aux, &aux_fd, &aux_mapping, &aux_map_size);
            if (aux_status != 0) {
                lpr_destroy_tty_wire_page(page_fd, page);
                return aux_status;
            }
            const lpr_drm_clip_rect_t *source =
                (const void *)(uintptr_t)wire.clips_ptr;
            gpud_drm_mode_rectangle_t *rectangles = aux_mapping;
            for (uint32_t i = 0; i < wire.num_clips; ++i) {
                lpr_drm_clip_rect_t rectangle;
                lpr_memcpy(&rectangle, source + i, sizeof(rectangle));
                rectangles[i] = (gpud_drm_mode_rectangle_t){
                    .x1 = rectangle.x1,
                    .y1 = rectangle.y1,
                    .x2 = rectangle.x2,
                    .y2 = rectangle.y2,
                };
            }
        }
        wire.clips_ptr = 0;
        lpr_memcpy(ioctl->data, &wire, sizeof(wire));
        ioctl->arg_size = sizeof(wire);
        ioctl->data_size = sizeof(wire);
        ioctl->aux_size =
            (uint64_t)wire.num_clips * sizeof(gpud_drm_mode_rectangle_t);
        wire_kind = LPR_DRM_WIRE_DIRTY_FB;
    } else if (command == GPUD_DRM_IOCTL_MODE_GETPLANERESOURCES) {
        gpud_drm_kms_plane_res_wire_t *wire = (gpud_drm_kms_plane_res_wire_t *)ioctl->data;
        lpr_memcpy(&wire->value, (const void *)(uintptr_t)arg, sizeof(wire->value));
        ioctl->arg_size = sizeof(wire->value);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_PLANE_RES;
    } else if (command == GPUD_DRM_IOCTL_MODE_GETPLANE) {
        gpud_drm_kms_plane_wire_t *wire = (gpud_drm_kms_plane_wire_t *)ioctl->data;
        lpr_memcpy(&wire->value, (const void *)(uintptr_t)arg, sizeof(wire->value));
        ioctl->arg_size = sizeof(wire->value);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_PLANE;
    } else if (command == GPUD_DRM_IOCTL_MODE_OBJ_GETPROPERTIES) {
        gpud_drm_kms_object_properties_wire_t *wire =
            (gpud_drm_kms_object_properties_wire_t *)ioctl->data;
        lpr_memcpy(&wire->value, (const void *)(uintptr_t)arg, sizeof(wire->value));
        ioctl->arg_size = sizeof(wire->value);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_OBJECT_PROPERTIES;
    } else if (command == GPUD_DRM_IOCTL_MODE_GETPROPERTY) {
        gpud_drm_kms_property_wire_t *wire = (gpud_drm_kms_property_wire_t *)ioctl->data;
        lpr_memcpy(&wire->value, (const void *)(uintptr_t)arg, sizeof(wire->value));
        ioctl->arg_size = sizeof(wire->value);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_PROPERTY;
    } else if (command == GPUD_DRM_IOCTL_MODE_GETPROPBLOB) {
        gpud_drm_kms_property_blob_wire_t *wire =
            (gpud_drm_kms_property_blob_wire_t *)ioctl->data;
        lpr_memcpy(&wire->value, (const void *)(uintptr_t)arg, sizeof(wire->value));
        ioctl->arg_size = sizeof(wire->value);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_PROPERTY_BLOB;
    } else if (command == GPUD_DRM_IOCTL_MODE_CREATEPROPBLOB) {
        const lpr_drm_mode_create_blob_t *create =
            (const lpr_drm_mode_create_blob_t *)(uintptr_t)arg;
        if (create->length == 0 ||
            create->length > GPUD_DRM_KMS_PROPERTY_BLOB_BYTES ||
            create->data == 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EINVAL;
        }
        gpud_drm_mode_create_blob_wire_t *wire =
            (gpud_drm_mode_create_blob_wire_t *)ioctl->data;
        wire->length = create->length;
        lpr_memcpy(
            wire->data,
            (const void *)(uintptr_t)create->data,
            create->length);
        ioctl->arg_size = sizeof(*create);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_CREATE_PROPERTY_BLOB;
    } else if (command == GPUD_DRM_IOCTL_MODE_ATOMIC) {
        gpud_drm_mode_atomic_wire_t *wire =
            (gpud_drm_mode_atomic_wire_t *)ioctl->data;
        const int prepare_status =
            lpr_drm_prepare_atomic(arg, wire, &input_wait_fd);
        if (prepare_status != 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return prepare_status;
        }
        const lpr_drm_mode_atomic_t *atomic =
            (const lpr_drm_mode_atomic_t *)(uintptr_t)arg;
        if ((wire->flags & GPUD_DRM_MODE_PAGE_FLIP_EVENT) != 0 &&
            (wire->flags & GPUD_DRM_MODE_ATOMIC_TEST_ONLY) == 0) {
            const int cookie_status = lpr_drm_event_cookie_register(
                drm->handle, atomic->user_data, &event_token);
            if (cookie_status != 0) {
                if (input_wait_fd >= 16) {
                    (void)lpr_close_native_fd_if_open(
                        (uint64_t)(uint32_t)input_wait_fd);
                }
                lpr_destroy_tty_wire_page(page_fd, page);
                return cookie_status;
            }
            wire->user_data = event_token;
        }
        if (input_wait_fd >= 16) {
            ioctl->fd_flags |= GPUD_DRM_IOCTL_FD_INPUT_WAIT;
        }
        ioctl->arg_size = sizeof(lpr_drm_mode_atomic_t);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_ATOMIC;
    } else if (command == GPUD_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE) {
        const gpud_drm_syncobj_handle_t *user =
            (const gpud_drm_syncobj_handle_t *)(uintptr_t)arg;
        if (user->flags !=
            GPUD_DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return user->flags == 0 ?
                -LPR_LINUX_EOPNOTSUPP : -LPR_LINUX_EINVAL;
        }
        if (user->fd < 0 ||
            !lpr_linux_sync_file_fd_active((uint64_t)(uint32_t)user->fd)) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EBADF;
        }
        input_wait_fd = lpr_sync_file_duplicate_wait(
            (uint64_t)(uint32_t)user->fd);
        if (input_wait_fd < 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return input_wait_fd;
        }
        gpud_drm_syncobj_handle_t *wire =
            (gpud_drm_syncobj_handle_t *)ioctl->data;
        lpr_memcpy(wire, user, sizeof(*wire));
        wire->fd = -1;
        ioctl->fd_flags = GPUD_DRM_IOCTL_FD_INPUT_WAIT;
        ioctl->arg_size = sizeof(*user);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_SYNCOBJ_FD_TO_HANDLE;
    } else if (command == GPUD_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD) {
        const gpud_drm_syncobj_handle_t *user =
            (const gpud_drm_syncobj_handle_t *)(uintptr_t)arg;
        if (user->flags !=
            GPUD_DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return user->flags == 0 ?
                -LPR_LINUX_EOPNOTSUPP : -LPR_LINUX_EINVAL;
        }
        const int pair_status = lpr_native_wait_pair(
            &output_wait_fd, &output_notify_fd);
        if (pair_status != 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return pair_status;
        }
        const int64_t installed = lpr_sync_file_install_wait(output_wait_fd);
        output_wait_fd = -1;
        if (installed < 0) {
            (void)lpr_close_native_fd_if_open(
                (uint64_t)(uint32_t)output_notify_fd);
            lpr_destroy_tty_wire_page(page_fd, page);
            return installed;
        }
        output_sync_fd = (int)installed;
        gpud_drm_syncobj_handle_t *wire =
            (gpud_drm_syncobj_handle_t *)ioctl->data;
        lpr_memcpy(wire, user, sizeof(*wire));
        wire->fd = -1;
        ioctl->fd_flags = GPUD_DRM_IOCTL_FD_OUTPUT_NOTIFY;
        ioctl->arg_size = sizeof(*user);
        ioctl->data_size = sizeof(*wire);
        wire_kind = LPR_DRM_WIRE_SYNCOBJ_HANDLE_TO_FD;
    } else if (command == GPUD_DRM_IOCTL_VIRTGPU_GETPARAM) {
        gpud_drm_virtgpu_getparam_t wire;
        lpr_memcpy(&wire, (const void *)(uintptr_t)arg, sizeof(wire));
        if (wire.value == 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EINVAL;
        }
        wire.value = 0;
        lpr_memcpy(ioctl->data, &wire, sizeof(wire));
        ioctl->arg_size = sizeof(wire);
        ioctl->data_size = sizeof(wire);
        wire_kind = LPR_DRM_WIRE_VIRTGPU_GETPARAM;
    } else if (command == GPUD_DRM_IOCTL_VIRTGPU_GET_CAPS) {
        gpud_drm_virtgpu_get_caps_t wire;
        lpr_memcpy(&wire, (const void *)(uintptr_t)arg, sizeof(wire));
        if (wire.addr == 0 || wire.size == 0 ||
            wire.size > GPUD_DRM_VIRTGPU_CAPSET_MAX_BYTES || wire.pad != 0 ||
            lpr_drm_aux_create(wire.size, reusable_aux, &aux_fd, &aux_mapping,
                &aux_map_size) != 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EINVAL;
        }
        wire.addr = 0;
        lpr_memcpy(ioctl->data, &wire, sizeof(wire));
        ioctl->arg_size = sizeof(wire);
        ioctl->data_size = sizeof(wire);
        ioctl->aux_size = wire.size;
        wire_kind = LPR_DRM_WIRE_VIRTGPU_GET_CAPS;
    } else if (command == GPUD_DRM_IOCTL_VIRTGPU_CONTEXT_INIT) {
        const int context_status = lpr_drm_prepare_virtgpu_context(
            arg, reusable_aux, ioctl, &aux_fd, &aux_mapping, &aux_map_size);
        if (context_status) {
            lpr_drm_aux_destroy(aux_fd, aux_mapping, aux_map_size);
            lpr_destroy_tty_wire_page(page_fd, page);
            return context_status;
        }
        wire_kind = LPR_DRM_WIRE_VIRTGPU_CONTEXT_INIT;
    } else if (command == GPUD_DRM_IOCTL_VIRTGPU_EXECBUFFER) {
        gpud_drm_virtgpu_execbuffer_t wire;
        lpr_memcpy(&wire, (const void *)(uintptr_t)arg, sizeof(wire));
        const uint32_t known_flags = GPUD_DRM_VIRTGPU_EXECBUF_FENCE_FD_IN |
            GPUD_DRM_VIRTGPU_EXECBUF_FENCE_FD_OUT |
            GPUD_DRM_VIRTGPU_EXECBUF_RING_IDX;
        if (wire.flags & ~known_flags) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EINVAL;
        }
        if ((wire.flags & GPUD_DRM_VIRTGPU_EXECBUF_FENCE_FD_IN) ?
                (wire.fence_fd < 0 || !lpr_linux_sync_file_fd_active(
                    (uint64_t)(uint32_t)wire.fence_fd)) :
                wire.fence_fd != -1) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return wire.flags & GPUD_DRM_VIRTGPU_EXECBUF_FENCE_FD_IN ?
                -LPR_LINUX_EBADF : -LPR_LINUX_EINVAL;
        }
        const uint32_t syncobj_count = wire.num_in_syncobjs +
            wire.num_out_syncobjs;
        if (syncobj_count < wire.num_in_syncobjs || wire.size == 0 ||
            wire.command == 0 ||
            (!(wire.flags & GPUD_DRM_VIRTGPU_EXECBUF_RING_IDX) && wire.ring_idx) ||
            (syncobj_count && wire.syncobj_stride !=
                sizeof(gpud_drm_virtgpu_execbuffer_syncobj_t)) ||
            (!syncobj_count && wire.syncobj_stride != 0 &&
                wire.syncobj_stride != sizeof(gpud_drm_virtgpu_execbuffer_syncobj_t)) ||
            (!!wire.num_in_syncobjs != !!wire.in_syncobjs) ||
            (!!wire.num_out_syncobjs != !!wire.out_syncobjs) ||
            (wire.num_bo_handles != 0 && wire.bo_handles == 0)) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EINVAL;
        }
        const uint64_t command_bytes = ((uint64_t)wire.size + 7u) & ~UINT64_C(7);
        const uint64_t handle_bytes = (uint64_t)wire.num_bo_handles * sizeof(uint32_t);
        const uint64_t syncobj_offset = (command_bytes + handle_bytes + 7u) &
            ~UINT64_C(7);
        const uint64_t input_bytes = (uint64_t)wire.num_in_syncobjs *
            sizeof(gpud_drm_virtgpu_execbuffer_syncobj_t);
        const uint64_t output_offset = syncobj_offset + input_bytes;
        const uint64_t output_bytes = (uint64_t)wire.num_out_syncobjs *
            sizeof(gpud_drm_virtgpu_execbuffer_syncobj_t);
        const uint64_t total_bytes = syncobj_count ?
            output_offset + output_bytes : command_bytes + handle_bytes;
        if (wire.size > GPUD_DRM_VIRTGPU_EXEC_COMMAND_MAX_BYTES ||
            wire.num_bo_handles > GPUD_DRM_VIRTGPU_EXEC_HANDLE_MAX_COUNT ||
            wire.num_in_syncobjs > GPUD_DRM_VIRTGPU_EXEC_HANDLE_MAX_COUNT ||
            wire.num_out_syncobjs > GPUD_DRM_VIRTGPU_EXEC_HANDLE_MAX_COUNT ||
            command_bytes < wire.size || handle_bytes / sizeof(uint32_t) !=
                wire.num_bo_handles ||
            command_bytes > GPUD_DRM_VIRTGPU_EXEC_COMMAND_MAX_BYTES ||
            syncobj_offset < command_bytes + handle_bytes ||
            output_offset < syncobj_offset ||
            (syncobj_count && total_bytes < output_offset) ||
            total_bytes > GPUD_DRM_IOCTL_AUX_MAX_BYTES ||
            lpr_drm_aux_create(total_bytes, reusable_aux, &aux_fd,
                &aux_mapping, &aux_map_size) != 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EINVAL;
        }
        lpr_memcpy(aux_mapping, (const void *)(uintptr_t)wire.command, wire.size);
        if (handle_bytes != 0) {
            lpr_memcpy((uint8_t *)aux_mapping + command_bytes,
                (const void *)(uintptr_t)wire.bo_handles, handle_bytes);
        }
        if (input_bytes != 0) {
            lpr_memcpy((uint8_t *)aux_mapping + syncobj_offset,
                (const void *)(uintptr_t)wire.in_syncobjs, input_bytes);
        }
        if (output_bytes != 0) {
            lpr_memcpy((uint8_t *)aux_mapping + output_offset,
                (const void *)(uintptr_t)wire.out_syncobjs, output_bytes);
        }
        if (wire.flags & GPUD_DRM_VIRTGPU_EXECBUF_FENCE_FD_IN) {
            input_wait_fd = lpr_sync_file_duplicate_wait(
                (uint64_t)(uint32_t)wire.fence_fd);
            if (input_wait_fd < 0) {
                lpr_drm_aux_destroy(aux_fd, aux_mapping, aux_map_size);
                lpr_destroy_tty_wire_page(page_fd, page);
                return input_wait_fd;
            }
            ioctl->fd_flags |= GPUD_DRM_IOCTL_FD_INPUT_WAIT;
        }
        if (wire.flags & GPUD_DRM_VIRTGPU_EXECBUF_FENCE_FD_OUT) {
            const int pair_status = lpr_native_wait_pair(
                &output_wait_fd, &output_notify_fd);
            if (pair_status != 0) {
                if (input_wait_fd >= 16)
                    (void)lpr_close_native_fd_if_open(
                        (uint64_t)(uint32_t)input_wait_fd);
                lpr_drm_aux_destroy(aux_fd, aux_mapping, aux_map_size);
                lpr_destroy_tty_wire_page(page_fd, page);
                return pair_status;
            }
            const int64_t installed =
                lpr_sync_file_install_wait(output_wait_fd);
            output_wait_fd = -1;
            if (installed < 0) {
                (void)lpr_close_native_fd_if_open(
                    (uint64_t)(uint32_t)output_notify_fd);
                output_notify_fd = -1;
                if (input_wait_fd >= 16)
                    (void)lpr_close_native_fd_if_open(
                        (uint64_t)(uint32_t)input_wait_fd);
                lpr_drm_aux_destroy(aux_fd, aux_mapping, aux_map_size);
                lpr_destroy_tty_wire_page(page_fd, page);
                return installed;
            }
            output_sync_fd = (int)installed;
            ioctl->fd_flags |= GPUD_DRM_IOCTL_FD_OUTPUT_NOTIFY;
        }
        wire.command = 0;
        wire.bo_handles = command_bytes;
        wire.fence_fd = -1;
        wire.in_syncobjs = wire.num_in_syncobjs ? syncobj_offset : 0;
        wire.out_syncobjs = wire.num_out_syncobjs ? output_offset : 0;
        lpr_memcpy(ioctl->data, &wire, sizeof(wire));
        ioctl->arg_size = sizeof(wire);
        ioctl->data_size = sizeof(wire);
        ioctl->aux_size = total_bytes;
        wire_kind = LPR_DRM_WIRE_VIRTGPU_EXECBUFFER;
    } else if (no_argument) {
        ioctl->arg_size = 0;
        ioctl->data_size = 0;
        wire_kind = LPR_DRM_WIRE_NO_ARGUMENT;
    } else {
        const uint64_t size = (request >> 16u) & 0x3fffu;
        if (size == 0 || size > GPUD_DRM_IOCTL_DATA_BYTES) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EINVAL;
        }
        ioctl->arg_size = size;
        ioctl->data_size = size;
        lpr_memcpy(ioctl->data, (const void *)(uintptr_t)arg, size);
    }
    int transfer_fds[3];
    uint8_t temporary_vmos[3];
    uint32_t transfer_count = 0;
    if (aux_fd >= 16) {
        transfer_fds[transfer_count] = aux_fd;
        temporary_vmos[transfer_count++] = 1;
    }
    if (input_wait_fd >= 16) {
        transfer_fds[transfer_count] = input_wait_fd;
        temporary_vmos[transfer_count++] = 0;
    }
    if (output_notify_fd >= 16) {
        transfer_fds[transfer_count] = output_notify_fd;
        temporary_vmos[transfer_count++] = 0;
    }
#if defined(LPR_DRM_STARTUP_PROFILE) && LPR_DRM_STARTUP_PROFILE
    const uint64_t profile_ipc_begin = pacha_trace_read_tsc();
#endif
    const int inline_ioctl = !transfer_count &&
        (wire_kind == LPR_DRM_WIRE_GENERIC || wire_kind == LPR_DRM_WIRE_NO_ARGUMENT) &&
        gpud_drm_ioctl_can_inline(ioctl);
    int64_t status = inline_ioctl ? lpr_gpud_drm_ioctl_inline(ioctl) :
        lpr_gpud_drm_call_transfers(
        GPUD_DRM_OP_HANDLE_IOCTL,
        page_fd,
        page,
        sizeof(*ioctl),
        0,
        0,
        transfer_fds,
        temporary_vmos,
        transfer_count);
#if defined(LPR_DRM_STARTUP_PROFILE) && LPR_DRM_STARTUP_PROFILE
    const uint64_t profile_ipc_end = pacha_trace_read_tsc();
#endif
    if (status != 0 && event_token != 0) {
        lpr_drm_event_cookie_cancel(drm->handle, event_token);
        event_token = 0;
    }
    if (input_wait_fd >= 16) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)input_wait_fd);
        input_wait_fd = -1;
    }
    if (output_notify_fd >= 16) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)output_notify_fd);
        output_notify_fd = -1;
    }
    if (wire_kind == LPR_DRM_WIRE_SYNCOBJ_HANDLE_TO_FD) {
        if (status == 0) {
            ((gpud_drm_syncobj_handle_t *)(uintptr_t)arg)->fd = output_sync_fd;
            output_sync_fd = -1;
        } else if (output_sync_fd >= 0) {
            (void)lpr_linux_close((uint64_t)(uint32_t)output_sync_fd);
            output_sync_fd = -1;
        }
    } else if (wire_kind == LPR_DRM_WIRE_VIRTGPU_EXECBUFFER &&
               output_sync_fd >= 0) {
        if (status == 0) {
            ((gpud_drm_virtgpu_execbuffer_t *)(uintptr_t)arg)->fence_fd =
                output_sync_fd;
            output_sync_fd = -1;
        } else {
            (void)lpr_linux_close((uint64_t)(uint32_t)output_sync_fd);
            output_sync_fd = -1;
        }
    }
    if (status == 0 && wire_kind == LPR_DRM_WIRE_VERSION) {
        lpr_drm_version_t *version = (lpr_drm_version_t *)(uintptr_t)arg;
        const gpud_drm_version_wire_t *wire = (const gpud_drm_version_wire_t *)ioctl->data;
        version->major = wire->major;
        version->minor = wire->minor;
        version->patchlevel = wire->patchlevel;
        version->name_len = wire->name_length;
        version->date_len = wire->date_length;
        version->desc_len = wire->desc_length;
        if (version->name != 0 && wire->name_capacity != 0) {
            uint64_t length = wire->name_length < wire->name_capacity ? wire->name_length : wire->name_capacity;
            if (length > sizeof(wire->name)) length = sizeof(wire->name);
            lpr_memcpy((void *)(uintptr_t)version->name, wire->name, length);
        }
        if (version->date != 0 && wire->date_capacity != 0) {
            uint64_t length = wire->date_length < wire->date_capacity ? wire->date_length : wire->date_capacity;
            if (length > sizeof(wire->date)) length = sizeof(wire->date);
            lpr_memcpy((void *)(uintptr_t)version->date, wire->date, length);
        }
        if (version->desc != 0 && wire->desc_capacity != 0) {
            uint64_t length = wire->desc_length < wire->desc_capacity ? wire->desc_length : wire->desc_capacity;
            if (length > sizeof(wire->desc)) length = sizeof(wire->desc);
            lpr_memcpy((void *)(uintptr_t)version->desc, wire->desc, length);
        }
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_RESOURCES) {
        gpud_drm_kms_resources_wire_t *wire = (gpud_drm_kms_resources_wire_t *)ioctl->data;
        const gpud_drm_mode_card_res_t *user = (const gpud_drm_mode_card_res_t *)(uintptr_t)arg;
        const uint32_t fb_capacity = user->count_fbs < GPUD_DRM_KMS_FB_CAPACITY ? user->count_fbs : GPUD_DRM_KMS_FB_CAPACITY;
        const uint32_t crtc_capacity = user->count_crtcs < GPUD_DRM_KMS_CRTC_CAPACITY ? user->count_crtcs : GPUD_DRM_KMS_CRTC_CAPACITY;
        const uint32_t connector_capacity = user->count_connectors < GPUD_DRM_KMS_CONNECTOR_CAPACITY ? user->count_connectors : GPUD_DRM_KMS_CONNECTOR_CAPACITY;
        const uint32_t encoder_capacity = user->count_encoders < GPUD_DRM_KMS_ENCODER_CAPACITY ? user->count_encoders : GPUD_DRM_KMS_ENCODER_CAPACITY;
        if (user->fb_id_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->fb_id_ptr, wire->fbs, fb_capacity * sizeof(uint32_t));
        if (user->crtc_id_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->crtc_id_ptr, wire->crtcs, crtc_capacity * sizeof(uint32_t));
        if (user->connector_id_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->connector_id_ptr, wire->connectors, connector_capacity * sizeof(uint32_t));
        if (user->encoder_id_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->encoder_id_ptr, wire->encoders, encoder_capacity * sizeof(uint32_t));
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_CONNECTOR) {
        gpud_drm_kms_connector_wire_t *wire = (gpud_drm_kms_connector_wire_t *)ioctl->data;
        const gpud_drm_mode_get_connector_t *user = (const gpud_drm_mode_get_connector_t *)(uintptr_t)arg;
        const uint32_t mode_capacity = user->count_modes < GPUD_DRM_KMS_MODE_CAPACITY ? user->count_modes : GPUD_DRM_KMS_MODE_CAPACITY;
        const uint32_t encoder_capacity = user->count_encoders < GPUD_DRM_KMS_ENCODER_CAPACITY ? user->count_encoders : GPUD_DRM_KMS_ENCODER_CAPACITY;
        const uint32_t prop_capacity = user->count_props < GPUD_DRM_KMS_PROPERTY_CAPACITY ?
            user->count_props : GPUD_DRM_KMS_PROPERTY_CAPACITY;
        if (user->modes_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->modes_ptr, wire->modes, mode_capacity * sizeof(gpud_drm_modeinfo_t));
        if (user->encoders_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->encoders_ptr, wire->encoders, encoder_capacity * sizeof(uint32_t));
        if (user->props_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->props_ptr, wire->props, prop_capacity * sizeof(uint32_t));
        if (user->prop_values_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->prop_values_ptr, wire->prop_values, prop_capacity * sizeof(uint64_t));
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_CRTC) {
        gpud_drm_kms_crtc_wire_t *wire = (gpud_drm_kms_crtc_wire_t *)ioctl->data;
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_DIRTY_FB) {
        /* DIRTYFB has no output fields. Preserve the caller's clips pointer. */
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_PLANE_RES) {
        gpud_drm_kms_plane_res_wire_t *wire = (gpud_drm_kms_plane_res_wire_t *)ioctl->data;
        const gpud_drm_mode_get_plane_res_t *user = (const gpud_drm_mode_get_plane_res_t *)(uintptr_t)arg;
        const uint32_t capacity = user->count_planes < GPUD_DRM_KMS_PLANE_CAPACITY ? user->count_planes : GPUD_DRM_KMS_PLANE_CAPACITY;
        if (user->plane_id_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->plane_id_ptr, wire->planes, capacity * sizeof(uint32_t));
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_PLANE) {
        gpud_drm_kms_plane_wire_t *wire = (gpud_drm_kms_plane_wire_t *)ioctl->data;
        const gpud_drm_mode_get_plane_t *user = (const gpud_drm_mode_get_plane_t *)(uintptr_t)arg;
        const uint32_t capacity = user->count_format_types < GPUD_DRM_KMS_FORMAT_CAPACITY ? user->count_format_types : GPUD_DRM_KMS_FORMAT_CAPACITY;
        if (user->format_type_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->format_type_ptr, wire->formats, capacity * sizeof(uint32_t));
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_OBJECT_PROPERTIES) {
        gpud_drm_kms_object_properties_wire_t *wire =
            (gpud_drm_kms_object_properties_wire_t *)ioctl->data;
        const gpud_drm_mode_obj_get_properties_t *user =
            (const gpud_drm_mode_obj_get_properties_t *)(uintptr_t)arg;
        const uint32_t capacity = user->count_props < GPUD_DRM_KMS_PROPERTY_CAPACITY ?
            user->count_props : GPUD_DRM_KMS_PROPERTY_CAPACITY;
        if (user->props_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->props_ptr,
            wire->props, capacity * sizeof(uint32_t));
        if (user->prop_values_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->prop_values_ptr,
            wire->prop_values, capacity * sizeof(uint64_t));
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_PROPERTY) {
        gpud_drm_kms_property_wire_t *wire = (gpud_drm_kms_property_wire_t *)ioctl->data;
        const gpud_drm_mode_get_property_t *user =
            (const gpud_drm_mode_get_property_t *)(uintptr_t)arg;
        const uint32_t value_capacity = user->count_values < GPUD_DRM_KMS_PROPERTY_VALUE_CAPACITY ?
            user->count_values : GPUD_DRM_KMS_PROPERTY_VALUE_CAPACITY;
        const uint32_t enum_capacity = user->count_enum_blobs < GPUD_DRM_KMS_PROPERTY_ENUM_CAPACITY ?
            user->count_enum_blobs : GPUD_DRM_KMS_PROPERTY_ENUM_CAPACITY;
        if (user->values_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->values_ptr,
            wire->values, value_capacity * sizeof(uint64_t));
        if (user->enum_blob_ptr != 0) lpr_memcpy((void *)(uintptr_t)user->enum_blob_ptr,
            wire->enums, enum_capacity * sizeof(gpud_drm_mode_property_enum_t));
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_PROPERTY_BLOB) {
        gpud_drm_kms_property_blob_wire_t *wire =
            (gpud_drm_kms_property_blob_wire_t *)ioctl->data;
        const gpud_drm_mode_get_blob_t *user = (const gpud_drm_mode_get_blob_t *)(uintptr_t)arg;
        uint32_t length = user->length < wire->value.length ? user->length : wire->value.length;
        if (length > GPUD_DRM_KMS_PROPERTY_BLOB_BYTES) length = GPUD_DRM_KMS_PROPERTY_BLOB_BYTES;
        if (user->data != 0) lpr_memcpy((void *)(uintptr_t)user->data, wire->data, length);
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 &&
        wire_kind == LPR_DRM_WIRE_CREATE_PROPERTY_BLOB) {
        lpr_drm_mode_create_blob_t *user =
            (lpr_drm_mode_create_blob_t *)(uintptr_t)arg;
        const gpud_drm_mode_create_blob_wire_t *wire =
            (const gpud_drm_mode_create_blob_wire_t *)ioctl->data;
        user->blob_id = wire->blob_id;
    } else if (status == 0 &&
        wire_kind == LPR_DRM_WIRE_SYNCOBJ_FD_TO_HANDLE) {
        gpud_drm_syncobj_handle_t *user =
            (gpud_drm_syncobj_handle_t *)(uintptr_t)arg;
        const gpud_drm_syncobj_handle_t *wire =
            (const gpud_drm_syncobj_handle_t *)ioctl->data;
        user->handle = wire->handle;
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_SYNCOBJ_WAIT) {
        gpud_drm_syncobj_wait_t *user =
            (gpud_drm_syncobj_wait_t *)(uintptr_t)arg;
        const gpud_drm_syncobj_wait_t *wire =
            (const gpud_drm_syncobj_wait_t *)ioctl->data;
        user->first_signaled = wire->first_signaled;
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_SYNCOBJ_ARRAY) {
        /* RESET and SIGNAL have no output fields. */
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_VIRTGPU_GETPARAM) {
        const gpud_drm_virtgpu_getparam_t *user =
            (const gpud_drm_virtgpu_getparam_t *)(uintptr_t)arg;
        const gpud_drm_virtgpu_getparam_t *wire =
            (const gpud_drm_virtgpu_getparam_t *)ioctl->data;
        const uint32_t value = (uint32_t)wire->value;
        lpr_memcpy((void *)(uintptr_t)user->value, &value, sizeof(value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_VIRTGPU_GET_CAPS) {
        const gpud_drm_virtgpu_get_caps_t *user =
            (const gpud_drm_virtgpu_get_caps_t *)(uintptr_t)arg;
        lpr_memcpy((void *)(uintptr_t)user->addr, aux_mapping, user->size);
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_GENERIC) {
        lpr_memcpy((void *)(uintptr_t)arg, ioctl->data, ioctl->data_size);
    }
    lpr_drm_aux_destroy(aux_fd, aux_mapping, aux_map_size);
    lpr_destroy_tty_wire_page(page_fd, page);
#if defined(LPR_DRM_STARTUP_PROFILE) && LPR_DRM_STARTUP_PROFILE
    const uint64_t profile_end = pacha_trace_read_tsc();
    lpr_drm_profile_record(
        command,
        status,
        profile_start,
        profile_page_end,
        profile_ipc_begin,
        profile_ipc_end,
        profile_end);
#endif
    return status;
}

int64_t lpr_drm_mmap(
    uint64_t fd,
    uint64_t address,
    uint64_t length,
    uint64_t pacha_prot,
    uint64_t pacha_flags,
    uint64_t offset)
{
    lpr_drm_backend_t *drm = lpr_drm_backend(fd);
    if (drm == 0) {
        return -LPR_LINUX_EBADF;
    }
    void *page = 0;
    const int page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    gpud_drm_mmap_request_t *mmap = (gpud_drm_mmap_request_t *)lpr_gpud_drm_payload(page);
    lpr_memset(mmap, 0, sizeof(*mmap));
    mmap->handle = drm->handle;
    mmap->length = length;
    mmap->prot = pacha_prot;
    mmap->flags = pacha_flags;
    mmap->offset = offset;
    int received[2] = {-1, -1};
    uint32_t received_count = 0;
    uint64_t mapping_length = 0;
    const int64_t status = lpr_gpud_drm_call_transfers_many(
        GPUD_DRM_OP_HANDLE_MMAP, page_fd, page, sizeof(*mmap),
        &mapping_length, received, 2, &received_count, 0, 0, 0);
    lpr_destroy_tty_wire_page(page_fd, page);
    if (status != 0)
        return status;
    struct pacha_fd_info view;
    struct pacha_fd_info lease;
    const uint64_t view_rights = PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    const uint32_t lease_flags = PACHA_FD_FLAG_CLOEXEC | PACHA_FD_FLAG_INHERIT;
    if (received_count != 2 || mapping_length < length ||
        !lpr_native_fd_info((uint64_t)(uint32_t)received[0], &view) ||
        view.kind != PACHA_FD_KIND_VMO || view.size != mapping_length ||
        view.rights != view_rights || view.flags != PACHA_FD_FLAG_CLOEXEC ||
        !lpr_native_fd_info((uint64_t)(uint32_t)received[1], &lease) ||
        lease.kind != PACHA_FD_KIND_CHANNEL || lease.size ||
        lease.rights != PACHA_FD_RIGHT_CLOSE || lease.flags != lease_flags) {
        for (uint32_t i = 0; i < received_count; ++i)
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)received[i]);
        return -LPR_LINUX_EIO;
    }
    const int64_t mapped = lpr_drm_map_received(
        received[0], received[1], address, length,
        pacha_prot, pacha_flags, 0);
    (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)received[0]);
    if (mapped < 0)
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)received[1]);
    return mapped;
}

int64_t lpr_drm_poll_events(uint64_t fd, uint32_t events)
{
    lpr_drm_backend_t *drm = lpr_drm_backend(fd);
    if (drm == 0) return -LPR_LINUX_EBADF;
    if (drm->wait_fd.raw < 16) return -LPR_LINUX_EBADF;
    struct pacha_pollfd pollfd = {
        .fd = drm->wait_fd.raw,
        .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP,
    };
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_FD_POLL, (uint64_t)(uintptr_t)&pollfd, 1);
    if (status < 0) return lpr_pacha_status_to_errno(status);
    uint32_t revents = 0;
    if ((pollfd.revents & PACHA_FD_EVENT_READABLE) != 0)
        revents |= events & (0x0001u | 0x0040u);
    if ((pollfd.revents & PACHA_FD_EVENT_HANGUP) != 0)
        revents |= 0x0010u;
    return revents;
}

static int64_t lpr_drm_drain_one_event_hint(const lpr_drm_backend_t *drm)
{
    struct pacha_ipc_msg message;
    lpr_memset(&message, 0, sizeof(message));
    const int64_t status = lpr_pacha_syscall2(
        PACHAOS_SYSCALL_IPC_RECV,
        (uint64_t)(uint32_t)drm->wait_fd.raw,
        (uint64_t)(uintptr_t)&message);
    if (status == 0)
        return message.word0 || message.word1 || message.word2 ||
            message.word3 || message.fd_count ? -LPR_LINUX_EIO : 0;
    if (status == PACHA_SYSCALL_ERR_EMPTY ||
        status == PACHA_SYSCALL_ERR_NOT_READY ||
        status == -PACHA_SYSCALL_ERR_EMPTY ||
        status == -PACHA_SYSCALL_ERR_NOT_READY)
        return 0;
    return lpr_pacha_status_to_errno(status);
}

int64_t lpr_drm_read_events(uint64_t fd, uint64_t buf, uint64_t count)
{
    lpr_drm_backend_t *drm = lpr_drm_backend(fd);
    if (drm == 0) return -LPR_LINUX_EBADF;
    if (count == 0) return 0;
    if (buf == 0) return -LPR_LINUX_EFAULT;
    if (count < 4u * sizeof(uint64_t)) return -LPR_LINUX_EINVAL;
    for (;;) {
        /* Drain before the service-side recheck. If an event races with this
         * drain, gpud either includes it in this read or leaves data buffered
         * and publishes a fresh hint. Draining after the read loses that wake. */
        const int64_t hint_status = lpr_drm_drain_one_event_hint(drm);
        if (hint_status != 0) return hint_status;
        void *page = 0;
        const int page_fd = lpr_create_tty_wire_page(&page);
        if (page_fd < 0) return page_fd;
        gpud_drm_read_request_t *read =
            (gpud_drm_read_request_t *)lpr_gpud_drm_payload(page);
        lpr_memset(read, 0, sizeof(*read));
        read->handle = drm->handle;
        read->capacity = count < GPUD_DRM_EVENT_READ_BYTES ?
            count : GPUD_DRM_EVENT_READ_BYTES;
        const int64_t status = lpr_gpud_drm_call(
            GPUD_DRM_OP_HANDLE_READ, page_fd, page, sizeof(*read), 0, 0);
        if (status != 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return status;
        }
        if (read->data_size > read->capacity || read->data_size > count) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EIO;
        }
        uint64_t offset = 0;
        while (offset < read->data_size) {
            uint32_t header[2];
            if (read->data_size - offset < sizeof(header)) {
                lpr_destroy_tty_wire_page(page_fd, page);
                return -LPR_LINUX_EIO;
            }
            lpr_memcpy(header, read->data + offset, sizeof(header));
            if (header[1] < sizeof(header) || header[1] > read->data_size - offset) {
                lpr_destroy_tty_wire_page(page_fd, page);
                return -LPR_LINUX_EIO;
            }
            offset += header[1];
        }
        for (offset = 0; offset < read->data_size;) {
            gpud_drm_event_vblank_t event;
            uint32_t header[2];
            lpr_memcpy(header, read->data + offset, sizeof(header));
            if (header[0] == GPUD_DRM_EVENT_FLIP_COMPLETE &&
                header[1] == sizeof(event)) {
                uint64_t cookie = 0;
                lpr_memcpy(&event, read->data + offset, sizeof(event));
                if (lpr_drm_event_cookie_take(
                        drm->handle, event.user_data, &cookie))
                    event.user_data = cookie;
                lpr_memcpy(read->data + offset, &event, sizeof(event));
            }
            offset += header[1];
        }
        if (read->data_size) {
            const uint64_t bytes = read->data_size;
            lpr_memcpy((void *)(uintptr_t)buf, read->data, bytes);
            lpr_destroy_tty_wire_page(page_fd, page);
            return (int64_t)bytes;
        }
        lpr_destroy_tty_wire_page(page_fd, page);
        if ((drm->flags & LPR_LINUX_O_NONBLOCK) != 0) return -LPR_LINUX_EAGAIN;
        lpr_wait_graph_t graph;
        lpr_wait_deadline_t deadline;
        lpr_wait_graph_init(&graph);
        int64_t wait_status = lpr_wait_graph_add_fd(
            &graph, fd, 0x0001u);
        if (wait_status == 0)
            wait_status = lpr_wait_deadline_init(&deadline, -1);
        if (wait_status == 0)
            wait_status = lpr_wait_graph_block(&graph, &deadline);
        if (wait_status != 0) return wait_status;
    }
}

int lpr_drm_native_wait_fd(uint64_t fd)
{
    lpr_drm_backend_t *drm = lpr_drm_backend(fd);
    return drm != 0 ? drm->wait_fd.raw : -1;
}
