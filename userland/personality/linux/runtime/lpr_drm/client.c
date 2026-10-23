#include "../lpr_filed_internal.h"
#include "dmabuf.h"
#include "../support/unmap_profile.h"
#include "../../../../../kobox2/linux-sandbox/kobox/boot/drm_limits.h"
#include "../../../../../kobox2/protocol/generated/include/kobox2/gpu_drm_amdgpu_layout.h"
#include "../../../../../kobox2/protocol/generated/include/kobox2/gpu_drm_mode_layout.h"
#include "../../../../../kobox2/protocol/include/kobox2/amdgpu_info_bridge.h"
#include "amdgpu_info.h"
#include <gpud/drm_kms_query.h>
#include "../../../../../kobox2/protocol/include/kobox2/amdgpu_cs_shape.h"

static int64_t amdgpu_info_failure(uint32_t query, uint32_t bytes,
    const char *phase, int64_t status, unsigned *wait_not_ready __attribute__((unused)))
{
    char line[192], *out = line;
    const char *prefix = "LPR DRM AMDGPU_INFO failed phase=";
    while (*prefix) *out++ = *prefix++;
    while (*phase) *out++ = *phase++;
    const char *labels[] = {" query=", " bytes=", " errno="};
    uint64_t values[] = {query, bytes, (uint64_t)(status < 0 ? -status : status)};
    for (unsigned i = 0; i < 3; ++i) {
        const char *label = labels[i];
        while (*label) *out++ = *label++;
        char digits[20]; unsigned n = 0;
        do { digits[n++] = '0' + values[i] % 10; values[i] /= 10; } while (values[i]);
        while (n) *out++ = digits[--n];
    }
    *out++ = '\n';
    /* stderr follows the application's RAM log redirection, not kernel log
     * storage. In particular, pre-RPC validation failures remain visible. */
    (void)lpr_linux_write(2, (uint64_t)(uintptr_t)line, (uint64_t)(out - line), wait_not_ready);
    return status;
}

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
static pacha_fd_t lpr_drm_request_channel_fd;
static pacha_fd_t lpr_drm_aux_cache_fd;
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
    pacha_fd_t fd;
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
    pacha_fd_t vmo_fd, pacha_fd_t lease_fd, uint64_t address, uint64_t length,
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
    const int64_t status = lpr_native_ipc_call_wait(
        LPR_GPUD_DRM_ENDPOINT_FD, &request, &reply, NULL);
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

static void lpr_gpud_drm_drop_channel(pacha_fd_t endpoint)
{
    if (endpoint < 16 || endpoint != lpr_drm_request_channel_fd) return;
    lpr_drm_request_channel_fd = 0;
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)endpoint);
}

static pacha_fd_t lpr_gpud_drm_bind_page(pacha_fd_t page_fd, void *page)
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
    {
        status = lpr_native_ipc_call_wait(LPR_GPUD_DRM_ENDPOINT_FD, &request, &reply, NULL);
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
    lpr_drm_request_channel_fd = (pacha_fd_t)pair[0];
    return lpr_drm_request_channel_fd;
}

static int64_t lpr_gpud_drm_call_transfers_many(
    uint32_t op,
    pacha_fd_t page_fd,
    void *page,
    uint32_t payload_size,
    uint64_t *out_result,
    pacha_fd_t *out_received_fds,
    uint32_t received_capacity,
    uint32_t *out_received_count,
    const pacha_fd_t *transfer_fds,
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
    const pacha_fd_t endpoint = lpr_gpud_drm_bind_page(page_fd, page);
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
    header->header_size = sizeof(*header);
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
        const pacha_fd_t transfer_fd = transfer_fds[i];
        const int temporary_vmo = temporary_vmos[i] != 0;
        struct pacha_fd_info info;
        const uint64_t temporary_rights = PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE;
        const uint64_t retained_rights = PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_SET_FLAGS;
        if (!lpr_native_fd_info((uint64_t)transfer_fd, &info) ||
            (temporary_vmo ? info.kind != PACHA_FD_KIND_VMO :
                (info.kind != PACHA_FD_KIND_VMO && info.kind != PACHA_FD_KIND_CHANNEL)) ||
            (info.rights & (temporary_vmo ? temporary_rights : retained_rights)) !=
                (temporary_vmo ? temporary_rights : retained_rights)) {
            return -LPR_LINUX_EBADF;
        }
        fds[i + 1u].fd = (uint64_t)transfer_fd;
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
    const int64_t recv_status = lpr_native_ipc_call_wait(
        (uint64_t)endpoint, &request, &reply, NULL);
    if (recv_status != 0) {
        /* Submission may already have run. Forget the failed connection for
         * a future call, but never replay this request on another endpoint. */
        lpr_gpud_drm_drop_channel(endpoint);
        return lpr_pacha_status_to_errno(recv_status);
    }
    const pacha_service_envelope_t *reply_header = (const pacha_service_envelope_t *)page;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC || reply.word3 != request_id ||
        reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        reply_header->header_size != sizeof(*reply_header) ||
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
        out_received_fds[i] = (pacha_fd_t)reply_fds[i].fd;
    if (out_received_count != 0)
        *out_received_count = (uint32_t)reply.fd_count;
    return 0;
}

static int64_t lpr_gpud_drm_call_transfers(
    uint32_t op,
    pacha_fd_t page_fd,
    void *page,
    uint32_t payload_size,
    uint64_t *out_result,
    pacha_fd_t *out_received_fd,
    const pacha_fd_t *transfer_fds,
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
    pacha_fd_t page_fd,
    void *page,
    uint32_t payload_size,
    uint64_t *out_result,
    pacha_fd_t *out_received_fd,
    pacha_fd_t transfer_fd,
    int temporary_vmo)
{
    const pacha_fd_t transfer_fds[1] = {transfer_fd};
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
    pacha_fd_t page_fd,
    void *page,
    uint32_t payload_size,
    uint64_t *out_result,
    pacha_fd_t *out_received_fd)
{
    return lpr_gpud_drm_call_transfers(
        op, page_fd, page, payload_size, out_result, out_received_fd,
        0, 0, 0);
}

static int lpr_drm_aux_create(
    uint64_t size,
    int reusable,
    pacha_fd_t *out_fd,
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
    *out_fd = (pacha_fd_t)fd;
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

static void lpr_drm_aux_destroy(pacha_fd_t fd, void *mapping, uint64_t map_size)
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
    pacha_fd_t *aux_fd,
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
    pacha_fd_t *out_input_wait_fd)
{
    if (arg == 0 || wire == 0 || out_input_wait_fd == 0) {
        return -LPR_LINUX_EFAULT;
    }
    *out_input_wait_fd = -1;
    const lpr_drm_mode_atomic_t *atomic =
        (const lpr_drm_mode_atomic_t *)(uintptr_t)arg;
    if (atomic->reserved != 0 ||
        (atomic->flags & ~GPUD_DRM_MODE_ATOMIC_FLAGS) != 0 ||
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

    /* Property IDs belong to the probed device, not the virtio KMS fixture.
     * The sandbox checks pointer/FD property names before Linux sees values. */
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
    const pacha_fd_t page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        return page_fd;
    }
    gpud_drm_open_request_t *open = (gpud_drm_open_request_t *)lpr_gpud_drm_payload(page);
    lpr_memset(open, 0, sizeof(*open));
    open->device_minor = render ? 128u : 0u;
    open->flags = flags;
    pacha_fd_t native_wait_fd = -1;
    pacha_fd_t remote_wait_fd = -1;
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
        (uint64_t)remote_wait_fd);
    remote_wait_fd = -1;
    lpr_destroy_tty_wire_page(page_fd, page);
    if (status != 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)native_wait_fd);
        return status;
    }
    const int fd = lpr_drm_install(handle, flags, native_wait_fd,
        udmabuf ? LPR_DRM_NODE_UDMABUF : render ? LPR_DRM_NODE_RENDER : 0);
    if (fd < 0) {
        (void)lpr_drm_close_handle(handle);
        (void)lpr_close_native_fd_if_open((uint64_t)native_wait_fd);
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
    const pacha_fd_t page_fd = lpr_create_tty_wire_page(&page);
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
    const pacha_fd_t page_fd = lpr_create_tty_wire_page(&page);
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
    pacha_fd_t lease_fd,
    uint64_t *out_handle)
{
    if (out_handle == 0 || lease_fd < 16) return -LPR_LINUX_EINVAL;
    *out_handle = 0;
    void *page = 0;
    const pacha_fd_t page_fd = lpr_create_tty_wire_page(&page);
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
        (void)lpr_close_native_fd_if_open((uint64_t)lease_fd);
        return 0;
    }
    return status != 0 ? status : -LPR_LINUX_EIO;
}

int64_t lpr_drm_prime_ref(uint32_t op, uint64_t token)
{
    void *page = 0;
    const pacha_fd_t page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    gpud_drm_prime_token_request_t *request = (gpud_drm_prime_token_request_t *)lpr_gpud_drm_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->token = token;
    const int64_t status = lpr_gpud_drm_call(op, page_fd, page, sizeof(*request), 0, 0);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status;
}

int64_t lpr_drm_prime_transfer_acquire(uint64_t token, pacha_fd_t lease_fd)
{
    if (token == 0 || lease_fd < 16) return -LPR_LINUX_EINVAL;
    void *page = 0;
    const pacha_fd_t page_fd = lpr_create_tty_wire_page(&page);
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
        (void)lpr_close_native_fd_if_open((uint64_t)lease_fd);
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
    pacha_fd_t lease_fd = -1;
    if (dmabuf->token == 0) {
        struct pacha_fd_info info;
        lpr_memset(&info, 0, sizeof(info));
        if (!lpr_native_fd_info(
                (uint64_t)dmabuf->native.raw, &info) ||
            info.kind != PACHA_FD_KIND_VMO ||
            (info.rights & PACHA_FD_RIGHT_DUP) == 0)
            return -LPR_LINUX_EBADF;
        const int64_t duplicate = lpr_pacha_syscall4(
            PACHA_FD_SYSCALL_DUP,
            (uint64_t)dmabuf->native.raw,
            16,
            PACHA_FD_RIGHT_CLOSE,
            PACHA_FD_FLAG_CLOEXEC | PACHA_FD_FLAG_INHERIT);
        if (duplicate < 16)
            return lpr_pacha_status_to_errno(duplicate);
        lease_fd = (pacha_fd_t)duplicate;
        const int64_t mapped = lpr_drm_map_received(
            dmabuf->native.raw, lease_fd,
            address, length, prot, flags, offset);
        if (mapped < 0)
            (void)lpr_close_native_fd_if_open(
                (uint64_t)lease_fd);
        return mapped;
    }
    pacha_fd_t remote_lease_fd = -1;
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
                (uint64_t)lease_fd);
        if (remote_lease_fd >= 16)
            (void)lpr_close_native_fd_if_open(
                (uint64_t)remote_lease_fd);
        return status;
    }
    const int64_t mapped = lpr_drm_map_received(dmabuf->native.raw,
        lease_fd, address, length, prot, flags, offset);
    if (mapped < 0)
        (void)lpr_close_native_fd_if_open((uint64_t)lease_fd);
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

    const pacha_fd_t wait_fd = lpr_sync_file_duplicate_wait(
        (uint64_t)(uint32_t)sync->fd);
    if (wait_fd < 0) return wait_fd;
    void *page = 0;
    const pacha_fd_t page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)wait_fd);
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
    (void)lpr_close_native_fd_if_open((uint64_t)wait_fd);
    return status;
}

static int64_t lpr_drm_prime_export(uint64_t drm_handle, uint32_t gem_handle, uint32_t flags)
{
    void *page = 0;
    const pacha_fd_t page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    gpud_drm_prime_export_request_t *request = (gpud_drm_prime_export_request_t *)lpr_gpud_drm_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->handle = drm_handle;
    request->gem_handle = gem_handle;
    request->flags = flags;
    uint64_t token = 0;
    pacha_fd_t received[2] = {-1, -1};
    uint32_t received_count = 0;
    int64_t status = lpr_gpud_drm_call_transfers_many(
        GPUD_DRM_OP_PRIME_EXPORT, page_fd, page, sizeof(*request),
        &token, received, 2, &received_count, 0, 0, 0);
    lpr_destroy_tty_wire_page(page_fd, page);
    if (status != 0) return status;
    struct pacha_fd_info info;
    struct pacha_fd_info lease;
    const pacha_fd_t native_fd = received[0];
    const uint64_t view_rights = PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    if (received_count != 2 || native_fd < 16 || token == 0 || !lpr_native_fd_info((uint64_t)native_fd, &info) ||
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
        (void)lpr_close_native_fd_if_open((uint64_t)native_fd);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)received[1]);
    }
    return linux_fd;
}

static int64_t lpr_drm_prime_import(uint64_t drm_handle, uint64_t prime_fd, uint32_t flags, unsigned *wait_not_ready __attribute__((unused)))
{
    lpr_dmabuf_backend_t *dmabuf = lpr_dmabuf_backend(prime_fd);
    if (flags != 0) return -LPR_LINUX_EINVAL;
    pacha_fd_t import_vmo_fd = -1;
    uint64_t import_size = 0;
    if (dmabuf == 0) {
        if (!lpr_linux_filed_fd_active(prime_fd)) return -LPR_LINUX_EBADF;
        lpr_linux_stat_t st;
        lpr_memset(&st, 0, sizeof(st));
        const int64_t stat_status = lpr_linux_fstat(
            prime_fd, (uint64_t)(uintptr_t)&st, wait_not_ready);
        if (stat_status != 0) return stat_status;
        if (st.st_size <= 0 || (uint64_t)st.st_size > 256u * 1024u * 1024u) {
            return -LPR_LINUX_EINVAL;
        }
        import_size = ((uint64_t)st.st_size + 4095u) & ~4095ull;
        uint64_t file_size = 0;
        const int64_t shared_vmo_fd = lpr_linux_shared_file_vmo(
            prime_fd, 0, import_size, 1, 0, PACHAOS_PROT_READ | PACHAOS_PROT_WRITE, &file_size);
        if (shared_vmo_fd < 16) return shared_vmo_fd;
        if (file_size < (uint64_t)st.st_size) {
            (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)shared_vmo_fd);
            return -LPR_LINUX_EIO;
        }
        import_vmo_fd = (pacha_fd_t)shared_vmo_fd;
    }
    void *page = 0;
    const pacha_fd_t page_fd = lpr_create_tty_wire_page(&page);
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

static int64_t lpr_udmabuf_create(uint64_t drm_handle, uint64_t arg, unsigned *wait_not_ready __attribute__((unused)))
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
        create->memfd, (uint64_t)(uintptr_t)&st, wait_not_ready);
    if (stat_status != 0) return stat_status;
    if (st.st_size <= 0 || create->size > (uint64_t)st.st_size) return -LPR_LINUX_EINVAL;
    uint64_t file_size = 0;
    const int64_t native_fd = lpr_linux_shared_file_vmo(
        create->memfd, create->offset, create->size, 1, 0, PACHAOS_PROT_READ | PACHAOS_PROT_WRITE, &file_size);
    if (native_fd < 16) return native_fd;
    if (file_size < create->size) {
        (void)lpr_close_native_fd_if_open((uint64_t)native_fd);
        return -LPR_LINUX_EIO;
    }
    const uint64_t linux_flags = LPR_LINUX_O_RDWR |
        ((create->flags & 1u) != 0 ? LPR_LINUX_O_CLOEXEC : 0);
    const int linux_fd = lpr_dmabuf_install((pacha_fd_t)native_fd, -1, 0, create->size, linux_flags);
    if (linux_fd < 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)native_fd);
    }
    return linux_fd;
}

static char *lpr_drm_diag_text(char *out, const char *end, const char *value)
{
    while (*value && out < end) *out++ = *value++;
    return out;
}

static char *lpr_drm_diag_uint(char *out, const char *end, uint64_t value,
    unsigned base)
{
    char digits[17];
    unsigned length = 0;
    do {
        const unsigned digit = (unsigned)(value % base);
        digits[length++] = "0123456789abcdef"[digit];
        value /= base;
    } while (value && length < sizeof(digits));
    while (length && out < end) *out++ = digits[--length];
    return out;
}

static int lpr_drm_cs_reject(const char *reason,
    const gpud_drm_amdgpu_cs_t *user, uint32_t chunk_id,
    uint32_t length_dw, int error, unsigned *wait_not_ready __attribute__((unused)))
{
    char line[240];
    char *out = line;
    const char *end = line + sizeof(line) - 1;
    out = lpr_drm_diag_text(out, end,
        "LPR DRM ioctl rejected phase=prepare-amdgpu-cs request=0x");
    out = lpr_drm_diag_uint(out, end, GPUD_DRM_IOCTL_AMDGPU_CS, 16);
    out = lpr_drm_diag_text(out, end, " chunks=");
    out = lpr_drm_diag_uint(out, end, user ? user->num_chunks : 0, 10);
    out = lpr_drm_diag_text(out, end, " ctx=");
    out = lpr_drm_diag_uint(out, end, user ? user->ctx_id : 0, 10);
    out = lpr_drm_diag_text(out, end, " chunk_id=");
    out = lpr_drm_diag_uint(out, end, chunk_id, 10);
    out = lpr_drm_diag_text(out, end, " length_dw=");
    out = lpr_drm_diag_uint(out, end, length_dw, 10);
    out = lpr_drm_diag_text(out, end, " reason=");
    out = lpr_drm_diag_text(out, end, reason);
    out = lpr_drm_diag_text(out, end, " status=-");
    out = lpr_drm_diag_uint(out, end, (unsigned)-error, 10);
    if (out < end) *out++ = '\n';
    /* The test client's stderr is redirected into a RAM transcript. Early
     * LPR rejection cannot reach gpud, so it needs its own userland record. */
    (void)lpr_linux_write(2, (uintptr_t)line, (uint64_t)(out - line), wait_not_ready);
    return error;
}

static int lpr_drm_prepare_amdgpu_cs(uint64_t arg, int reusable_aux,
    gpud_drm_ioctl_request_t *request, pacha_fd_t *aux_fd,
    void **aux_mapping, uint64_t *aux_map_size, unsigned *wait_not_ready __attribute__((unused)))
{
    gpud_drm_amdgpu_cs_t user = {0};
    gpud_drm_amdgpu_cs_wire_t wire = {0};
    uint64_t sources[KB2_AMDGPU_CS_PARTS] = {0};
    uint32_t total = 0, ibs = 0, bos = 0;
    const uint32_t records[] = {0,
        KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_IB,
        KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_USER_FENCE,
        KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_DEPENDENCY,
        KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_SYNCOBJ_IN,
        KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_SYNCOBJ_OUT,
        KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_BO_HANDLE,
        KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_SCHEDULED_DEPENDENCY,
        KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_TIMELINE_WAIT,
        KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_TIMELINE_SIGNAL,
        KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_GFX_SHADOW};
    if (!arg || !request || !aux_fd || !aux_mapping || !aux_map_size)
        return lpr_drm_cs_reject("null-argument", NULL, 0, 0, -LPR_LINUX_EFAULT, wait_not_ready);
    lpr_memcpy(&user, (const void *)(uintptr_t)arg, sizeof(user));
    if (!user.ctx_id || !user.chunks || user.flags || !user.num_chunks ||
        user.num_chunks > KB2_AMDGPU_CS_PARTS ||
        user.chunks > UINT64_MAX - user.num_chunks * sizeof(uint64_t))
        return lpr_drm_cs_reject("header-shape", &user, 0, 0, -LPR_LINUX_EINVAL, wait_not_ready);
    for (uint32_t i = 0; i < user.num_chunks; ++i) {
        uint64_t pointer;
        gpud_drm_amdgpu_cs_chunk_t chunk;
        lpr_memcpy(&pointer, (const void *)(uintptr_t)(user.chunks + i * 8), 8);
        if (!pointer || pointer > UINT64_MAX - sizeof(chunk))
            return lpr_drm_cs_reject("chunk-pointer", &user, 0, 0, -LPR_LINUX_EFAULT, wait_not_ready);
        lpr_memcpy(&chunk, (const void *)(uintptr_t)pointer, sizeof(chunk));
        if (!chunk.chunk_id || chunk.chunk_id >= sizeof(records)/sizeof(records[0]))
            return lpr_drm_cs_reject("unsupported-chunk", &user, chunk.chunk_id,
                chunk.length_dw, -LPR_LINUX_EOPNOTSUPP, wait_not_ready);
        if (!chunk.chunk_data || !chunk.length_dw ||
            chunk.length_dw > KB2_AMDGPU_CS_BYTES / 4 ||
            chunk.chunk_data > UINT64_MAX - (uint64_t)chunk.length_dw * 4)
            return lpr_drm_cs_reject("chunk-range", &user, chunk.chunk_id,
                chunk.length_dw, -LPR_LINUX_EINVAL, wait_not_ready);
        gpud_drm_amdgpu_cs_part_t *p = &wire.parts[i];
        p->record = records[chunk.chunk_id];
        p->offset = total;
        p->bytes = chunk.length_dw * 4;
        sources[i] = chunk.chunk_data;
        if (chunk.chunk_id == 6) {
            gpud_drm_amdgpu_bo_list_in_t list;
            if (p->bytes != sizeof(list) || ++bos > 1 || user.bo_list_handle)
                return lpr_drm_cs_reject("bo-list-shape", &user, 6, chunk.length_dw, -LPR_LINUX_EINVAL, wait_not_ready);
            lpr_memcpy(&list, (const void *)(uintptr_t)chunk.chunk_data, sizeof(list));
            /* CS BO_HANDLES ignores operation/list_handle, unlike BO_LIST.
             * Mesa intentionally poisons those unused fields with ~0. */
            if (!list.bo_number || list.bo_number > 65536 ||
                list.bo_info_size != sizeof(gpud_drm_amdgpu_bo_entry_t) ||
                !list.bo_info_ptr || list.bo_info_ptr >
                UINT64_MAX - (uint64_t)list.bo_number * list.bo_info_size)
                return lpr_drm_cs_reject("bo-list-range", &user, 6, chunk.length_dw, -LPR_LINUX_EINVAL, wait_not_ready);
            p->bytes = list.bo_number * list.bo_info_size;
            sources[i] = list.bo_info_ptr;
        }
        uint32_t size = kb2_amdgpu_cs_element_size(p->record);
        if (!size || p->bytes % size)
            return lpr_drm_cs_reject("chunk-element-size", &user, chunk.chunk_id,
                chunk.length_dw, -LPR_LINUX_EINVAL, wait_not_ready);
        p->count = p->bytes / size;
        struct kb2_amdgpu_cs_part shape = {p->record, p->count, p->offset, p->bytes};
        if (!kb2_amdgpu_cs_part_valid(&shape))
            return lpr_drm_cs_reject("chunk-capacity", &user, chunk.chunk_id,
                chunk.length_dw, -LPR_LINUX_EINVAL, wait_not_ready);
        total += p->bytes;
        ibs += chunk.chunk_id == 1;
    }
    if (!ibs) return lpr_drm_cs_reject("missing-ib", &user, 0, 0, -LPR_LINUX_EINVAL, wait_not_ready);
    int result = lpr_drm_aux_create(total, reusable_aux, aux_fd, aux_mapping, aux_map_size);
    if (result) return lpr_drm_cs_reject("aux-allocation", &user, 0, 0, result, wait_not_ready);
    for (uint32_t i = 0; i < user.num_chunks; ++i) {
        const gpud_drm_amdgpu_cs_part_t *p = &wire.parts[i];
        uint8_t *dst = (uint8_t *)*aux_mapping + p->offset;
        if (p->record == KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_IB) {
            gpud_drm_amdgpu_cs_ib_t ib;
            lpr_memcpy(&ib, (const void *)(uintptr_t)sources[i], sizeof(ib));
            if (ib.pad || !ib.va_start || !ib.ib_bytes) {
                lpr_drm_aux_destroy(*aux_fd, *aux_mapping, *aux_map_size);
                *aux_fd = -1; *aux_mapping = 0; *aux_map_size = 0;
                return lpr_drm_cs_reject("ib-shape", &user, 1, 8, -LPR_LINUX_EINVAL, wait_not_ready);
            }
            lpr_memset(dst, 0, p->bytes);
            lpr_memcpy(dst, &ib.flags, 4); lpr_memcpy(dst + 4, &ib.ip_type, 4);
            lpr_memcpy(dst + 8, &ib.ip_instance, 4); lpr_memcpy(dst + 12, &ib.ring, 4);
            lpr_memcpy(dst + 16, &ib.va_start, 8); lpr_memcpy(dst + 24, &ib.ib_bytes, 4);
        } else {
            lpr_memcpy(dst, (const void *)(uintptr_t)sources[i], p->bytes);
        }
    }
    wire.ctx_id = user.ctx_id; wire.bo_list_handle = user.bo_list_handle;
    wire.chunk_count = user.num_chunks;
    lpr_memcpy(request->data, &wire, sizeof(wire));
    request->arg_size = sizeof(user); request->data_size = sizeof(wire);
    request->aux_size = total;
    return 0;
}

static int lpr_kms_query_prepare(gpud_drm_ioctl_request_t *request,
    const struct gpud_kms_query_shape *shape, const unsigned char *snapshot,
    int reusable_aux, pacha_fd_t *aux_fd, void **aux_mapping, uint64_t *aux_map_size)
{
    uint64_t bytes = 0;
    int large = 0;
    for (unsigned int i = 0; i < shape->vectors; ++i) {
        const struct gpud_kms_vector *v = &shape->vector[i];
        uint32_t count;
        lpr_memcpy(&count, snapshot + v->count, 4);
        if (count > 65536) return -LPR_LINUX_EINVAL;
        large |= count > v->limit;
        bytes += (uint64_t)count * v->stride;
    }
    if (large) {
        int result = lpr_drm_aux_create(bytes, reusable_aux, aux_fd,
            aux_mapping, aux_map_size);
        if (result) return result;
    }
    lpr_memcpy(request->data, snapshot, shape->scalar_size);
    for (unsigned int i = 0; i < shape->vectors; ++i) {
        const struct gpud_kms_vector *v = &shape->vector[i];
        lpr_memset(request->data + v->pointer, 0, 8);
        if (v->second_pointer) lpr_memset(request->data + v->second_pointer, 0, 8);
    }
    request->arg_size = shape->scalar_size;
    request->data_size = large ? shape->scalar_size : shape->inline_size;
    request->aux_size = large ? bytes : 0;
    return 0;
}

static int lpr_kms_query_reply(const gpud_drm_ioctl_request_t *request,
    const struct gpud_kms_query_shape *shape, const unsigned char *snapshot,
    const unsigned char *aux, uint64_t arg)
{
    const int large = request->data_size == shape->scalar_size;
    if (request->arg_size != shape->scalar_size ||
        (!large && request->data_size != shape->inline_size) ||
        (large && !aux)) return -LPR_LINUX_EIO;
    uint32_t copied[4] = {0}, offsets[4] = {0};
    uint64_t pointers[4] = {0}, second[4] = {0};
    unsigned char value[sizeof(gpud_drm_mode_card_res_t)];
    lpr_memcpy(value, request->data, shape->scalar_size);
    uint64_t expected_bytes = 0;
    int expected_large = 0;
    for (unsigned int i = 0; i < shape->vectors; ++i) {
        uint32_t capacity;
        lpr_memcpy(&capacity, snapshot + shape->vector[i].count, 4);
        if (capacity > 65536) return -LPR_LINUX_EIO;
        expected_large |= capacity > shape->vector[i].limit;
        expected_bytes += (uint64_t)capacity * shape->vector[i].stride;
    }
    if (large != expected_large || request->aux_size != (large ? expected_bytes : 0))
        return -LPR_LINUX_EIO;
    uint32_t offset = 0;
    /* Validate all split property records before exposing any result. */
    for (unsigned int i = 0; i < shape->vectors; ++i) {
        const struct gpud_kms_vector *v = &shape->vector[i];
        uint32_t capacity, required;
        lpr_memcpy(&capacity, snapshot + v->count, 4);
        lpr_memcpy(&required, value + v->count, 4);
        copied[i] = required <= capacity ? required : v->partial ? capacity : 0;
        offsets[i] = offset;
        offset += capacity * v->stride;
        lpr_memcpy(&pointers[i], snapshot + v->pointer, 8);
        lpr_memcpy(value + v->pointer, &pointers[i], 8);
        if (v->second_pointer) {
            lpr_memcpy(&second[i], snapshot + v->second_pointer, 8);
            lpr_memcpy(value + v->second_pointer, &second[i], 8);
        }
        if (copied[i] && (!pointers[i] || (v->second_pointer && !second[i])))
            return -LPR_LINUX_EFAULT;
        if (large && v->second_pointer)
            for (uint32_t n = 0; n < copied[i]; ++n) {
                uint32_t reserved;
                lpr_memcpy(&reserved, aux + offsets[i] + n * 16 + 4, 4);
                if (reserved) return -LPR_LINUX_EIO;
            }
    }
    for (unsigned int i = 0; i < shape->vectors; ++i) {
        const struct gpud_kms_vector *v = &shape->vector[i];
        if (!copied[i]) continue;
        if (large && v->second_pointer) {
            for (uint32_t n = 0; n < copied[i]; ++n) {
                lpr_memcpy((void *)(uintptr_t)(pointers[i] + n * 4),
                    aux + offsets[i] + n * 16, 4);
                lpr_memcpy((void *)(uintptr_t)(second[i] + n * 8),
                    aux + offsets[i] + n * 16 + 8, 8);
            }
        } else {
            lpr_memcpy((void *)(uintptr_t)pointers[i],
                large ? aux + offsets[i] : request->data + v->inline_array,
                copied[i] * (v->second_pointer ? 4 : v->stride));
            if (v->second_pointer)
                lpr_memcpy((void *)(uintptr_t)second[i],
                    request->data + v->second_array, copied[i] * 8);
        }
    }
    lpr_memcpy((void *)(uintptr_t)arg, value, shape->scalar_size);
    return 0;
}

static int lpr_drm_prepare_amdgpu_gem_op(uint32_t command, uint64_t arg,
    int reusable_aux, gpud_drm_ioctl_request_t *request, uint64_t *output_pointer,
    pacha_fd_t *aux_fd, void **aux_mapping, uint64_t *aux_map_size)
{
    gpud_drm_amdgpu_gem_op_t wire = {0};
    const uint32_t native_size = command == GPUD_DRM_IOCTL_AMDGPU_GEM_OP ? 16 : 24;
    lpr_memcpy(&wire, (const void *)(uintptr_t)arg, native_size);
    if (!wire.handle || wire.padding || wire.num_entries)
        return -LPR_LINUX_EINVAL;
    if (wire.op > 1) return -LPR_LINUX_EOPNOTSUPP;
    if (wire.op == 0) {
        if (!wire.value) return -LPR_LINUX_EFAULT;
        const int error = lpr_drm_aux_create(32, reusable_aux,
            aux_fd, aux_mapping, aux_map_size);
        if (error) return error;
        /* Only the caller's LPR sees the output pointer. SET_PLACEMENT's
         * value is instead a scalar domain mask, not an address. */
        *output_pointer = wire.value;
        wire.value = 0;
        request->aux_size = 32;
    } else if (wire.value & ~UINT64_C(7)) {
        return -LPR_LINUX_EINVAL;
    }
    lpr_memcpy(request->data, &wire, sizeof(wire));
    request->arg_size = native_size;
    request->data_size = sizeof(wire);
    return 0;
}

int64_t lpr_drm_ioctl(uint64_t fd, uint64_t request, uint64_t arg, unsigned *wait_not_ready __attribute__((unused)))
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
            lpr_udmabuf_create(drm->handle, arg, wait_not_ready) : -LPR_LINUX_ENOTTY;
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
            drm->handle, (uint64_t)(uint32_t)prime->fd, prime->flags, wait_not_ready);
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
    const pacha_fd_t page_fd = lpr_create_tty_wire_page(&page);
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
    pacha_fd_t aux_fd = -1;
    void *aux_mapping = 0;
    uint64_t aux_map_size = 0;
    pacha_fd_t input_wait_fd = -1;
    pacha_fd_t output_wait_fd = -1;
    pacha_fd_t output_notify_fd = -1;
    int output_sync_fd = -1;
    uint64_t event_token = 0;
    uint64_t gamma_pointer[3] = {0};
    uint32_t gamma_count = 0;
    uint64_t blob_pointer = 0;
    uint32_t blob_capacity = 0;
    gpud_drm_mode_get_connector_t connector_input = {0};
    struct gpud_kms_query_shape kms_shape;
    const int kms_query = gpud_kms_query_shape(command, &kms_shape);
    unsigned char kms_snapshot[sizeof(gpud_drm_mode_card_res_t)] = {0};
    uint64_t amdgpu_info_pointer = 0;
    uint32_t amdgpu_info_query = 0;
    uint32_t amdgpu_info_capacity = 0;
    uint64_t amdgpu_gem_op_pointer = 0;
    enum {
        LPR_DRM_WIRE_GENERIC,
        LPR_DRM_WIRE_VERSION,
        LPR_DRM_WIRE_CONNECTOR,
        LPR_DRM_WIRE_CONNECTOR_AUX,
        LPR_DRM_WIRE_CRTC,
        LPR_DRM_WIRE_DIRTY_FB,
        LPR_DRM_WIRE_PROPERTY_BLOB,
        LPR_DRM_WIRE_PROPERTY_BLOB_AUX,
        LPR_DRM_WIRE_CREATE_PROPERTY_BLOB,
        LPR_DRM_WIRE_ATOMIC,
        LPR_DRM_WIRE_CRTC_QUEUE_SEQUENCE,
        LPR_DRM_WIRE_GAMMA,
        LPR_DRM_WIRE_SYNCOBJ_FD_TO_HANDLE,
        LPR_DRM_WIRE_SYNCOBJ_HANDLE_TO_FD,
        LPR_DRM_WIRE_SYNCOBJ_WAIT,
        LPR_DRM_WIRE_SYNCOBJ_ARRAY,
        LPR_DRM_WIRE_NO_ARGUMENT,
        LPR_DRM_WIRE_VIRTGPU_GETPARAM,
        LPR_DRM_WIRE_VIRTGPU_GET_CAPS,
        LPR_DRM_WIRE_VIRTGPU_EXECBUFFER,
        LPR_DRM_WIRE_VIRTGPU_CONTEXT_INIT,
        LPR_DRM_WIRE_AMDGPU_INFO,
        LPR_DRM_WIRE_AMDGPU_GEM_OP,
        LPR_DRM_WIRE_AMDGPU_CS,
        LPR_DRM_WIRE_KMS_QUERY,
    } wire_kind = LPR_DRM_WIRE_GENERIC;
    if (kms_query) {
        lpr_memcpy(kms_snapshot, (const void *)(uintptr_t)arg, kms_shape.scalar_size);
        int result = lpr_kms_query_prepare(ioctl, &kms_shape, kms_snapshot,
            reusable_aux, &aux_fd, &aux_mapping, &aux_map_size);
        if (result) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return result;
        }
        wire_kind = LPR_DRM_WIRE_KMS_QUERY;
    } else if (command == LPR_DRM_IOCTL_SYNCOBJ_WAIT) {
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
    } else if (command == GPUD_DRM_IOCTL_MODE_GETGAMMA ||
        command == GPUD_DRM_IOCTL_MODE_SETGAMMA) {
        gpud_drm_mode_crtc_lut_t wire;
        lpr_memcpy(&wire, (const void *)(uintptr_t)arg, sizeof(wire));
        const uint64_t bytes = (uint64_t)wire.gamma_size * sizeof(uint16_t);
        if (!wire.crtc_id || !wire.gamma_size || wire.gamma_size > 65536 ||
            !wire.red || !wire.green || !wire.blue ||
            lpr_drm_aux_create(bytes * 3, reusable_aux, &aux_fd,
                &aux_mapping, &aux_map_size) != 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EINVAL;
        }
        gamma_pointer[0] = wire.red;
        gamma_pointer[1] = wire.green;
        gamma_pointer[2] = wire.blue;
        gamma_count = wire.gamma_size;
        if (command == GPUD_DRM_IOCTL_MODE_SETGAMMA)
            for (unsigned int i = 0; i < 3; ++i)
                lpr_memcpy((uint8_t *)aux_mapping + i * bytes,
                    (const void *)(uintptr_t)gamma_pointer[i], bytes);
        wire.red = wire.green = wire.blue = 0;
        lpr_memcpy(ioctl->data, &wire, sizeof(wire));
        ioctl->arg_size = sizeof(wire);
        ioctl->data_size = sizeof(wire);
        ioctl->aux_size = bytes * 3;
        wire_kind = LPR_DRM_WIRE_GAMMA;
    } else if (command == GPUD_DRM_IOCTL_MODE_GETCONNECTOR) {
        lpr_memcpy(&connector_input, (const void *)(uintptr_t)arg,
            sizeof(connector_input));
        const int large = connector_input.count_modes > GPUD_DRM_KMS_MODE_CAPACITY ||
            connector_input.count_props > GPUD_DRM_KMS_PROPERTY_CAPACITY ||
            connector_input.count_encoders > GPUD_DRM_KMS_ENCODER_CAPACITY;
        if (connector_input.count_modes > 65536 ||
            connector_input.count_props > 65536 ||
            connector_input.count_encoders > 65536) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EINVAL;
        }
        gpud_drm_mode_get_connector_t value = connector_input;
        value.encoders_ptr = value.modes_ptr = value.props_ptr =
            value.prop_values_ptr = 0;
        ioctl->arg_size = sizeof(value);
        if (large) {
            const uint64_t bytes = (uint64_t)value.count_modes * 68 +
                (uint64_t)value.count_props * 16 +
                (uint64_t)value.count_encoders * 4;
            if (lpr_drm_aux_create(bytes, reusable_aux, &aux_fd,
                    &aux_mapping, &aux_map_size) != 0) {
                lpr_destroy_tty_wire_page(page_fd, page);
                return -LPR_LINUX_EINVAL;
            }
            lpr_memcpy(ioctl->data, &value, sizeof(value));
            ioctl->data_size = sizeof(value);
            ioctl->aux_size = bytes;
            wire_kind = LPR_DRM_WIRE_CONNECTOR_AUX;
        } else {
            gpud_drm_kms_connector_wire_t *wire =
                (gpud_drm_kms_connector_wire_t *)ioctl->data;
            wire->value = value;
            ioctl->data_size = sizeof(*wire);
            wire_kind = LPR_DRM_WIRE_CONNECTOR;
        }
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
    } else if (command == GPUD_DRM_IOCTL_WAIT_VBLANK) {
        gpud_drm_wait_vblank_t wire;
        lpr_memcpy(&wire, (const void *)(uintptr_t)arg, sizeof(wire));
        if (wire.request.type & ~GPUD_DRM_VBLANK_FLAGS) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EINVAL;
        }
        if (wire.request.type & GPUD_DRM_VBLANK_EVENT) {
            const int cookie_status = lpr_drm_event_cookie_register(
                drm->handle, wire.request.signal, &event_token);
            if (cookie_status != 0) {
                lpr_destroy_tty_wire_page(page_fd, page);
                return cookie_status;
            }
            wire.request.signal = event_token;
        } else {
            wire.request.signal = 0;
        }
        lpr_memcpy(ioctl->data, &wire, sizeof(wire));
        ioctl->arg_size = sizeof(wire);
        ioctl->data_size = sizeof(wire);
    } else if (command == GPUD_DRM_IOCTL_CRTC_QUEUE_SEQUENCE) {
        gpud_drm_crtc_queue_sequence_t wire;
        lpr_memcpy(&wire, (const void *)(uintptr_t)arg, sizeof(wire));
        if (!wire.crtc_id || (wire.flags &
            ~(GPUD_DRM_CRTC_SEQUENCE_RELATIVE |
              GPUD_DRM_CRTC_SEQUENCE_NEXT_ON_MISS))) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return -LPR_LINUX_EINVAL;
        }
        const int cookie_status = lpr_drm_event_cookie_register(
            drm->handle, wire.user_data, &event_token);
        if (cookie_status != 0) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return cookie_status;
        }
        wire.user_data = event_token;
        lpr_memcpy(ioctl->data, &wire, sizeof(wire));
        ioctl->arg_size = sizeof(wire);
        ioctl->data_size = sizeof(wire);
        wire_kind = LPR_DRM_WIRE_CRTC_QUEUE_SEQUENCE;
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
    } else if (command == GPUD_DRM_IOCTL_MODE_GETPROPBLOB) {
        gpud_drm_mode_get_blob_t value;
        lpr_memcpy(&value, (const void *)(uintptr_t)arg, sizeof(value));
        if (value.length > GPUD_DRM_KMS_PROPERTY_BLOB_BYTES) {
            if (!value.data || value.length > KB2_GPU_DRM_MODE_MAX_BLOB_BYTES ||
                lpr_drm_aux_create(value.length, reusable_aux, &aux_fd,
                    &aux_mapping, &aux_map_size) != 0) {
                lpr_destroy_tty_wire_page(page_fd, page);
                return -LPR_LINUX_EINVAL;
            }
            blob_pointer = value.data;
            blob_capacity = value.length;
            value.data = 0;
            lpr_memcpy(ioctl->data, &value, sizeof(value));
            ioctl->arg_size = ioctl->data_size = sizeof(value);
            ioctl->aux_size = value.length;
            wire_kind = LPR_DRM_WIRE_PROPERTY_BLOB_AUX;
        } else {
            gpud_drm_kms_property_blob_wire_t *wire =
                (gpud_drm_kms_property_blob_wire_t *)ioctl->data;
            wire->value = value;
            wire->value.data = 0;
            ioctl->arg_size = sizeof(wire->value);
            ioctl->data_size = sizeof(*wire);
            wire_kind = LPR_DRM_WIRE_PROPERTY_BLOB;
        }
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
                        (uint64_t)input_wait_fd);
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
                (uint64_t)output_notify_fd);
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
    } else if (command == GPUD_DRM_IOCTL_AMDGPU_CS) {
        const int cs_status = lpr_drm_prepare_amdgpu_cs(arg,
            reusable_aux, ioctl, &aux_fd, &aux_mapping, &aux_map_size, wait_not_ready);
        if (cs_status) {
            lpr_drm_aux_destroy(aux_fd, aux_mapping, aux_map_size);
            lpr_destroy_tty_wire_page(page_fd, page);
            return cs_status;
        }
        wire_kind = LPR_DRM_WIRE_AMDGPU_CS;
    } else if (command == GPUD_DRM_IOCTL_AMDGPU_GEM_OP ||
        command == GPUD_DRM_IOCTL_AMDGPU_GEM_OP_EXT) {
        const int error = lpr_drm_prepare_amdgpu_gem_op(command, arg,
            reusable_aux, ioctl, &amdgpu_gem_op_pointer,
            &aux_fd, &aux_mapping, &aux_map_size);
        if (error) {
            lpr_drm_aux_destroy(aux_fd, aux_mapping, aux_map_size);
            lpr_destroy_tty_wire_page(page_fd, page);
            return error;
        }
        wire_kind = LPR_DRM_WIRE_AMDGPU_GEM_OP;
    } else if (command == GPUD_DRM_IOCTL_AMDGPU_INFO) {
        gpud_drm_amdgpu_info_t wire;
        lpr_memcpy(&wire, (const void *)(uintptr_t)arg, sizeof(wire));
        amdgpu_info_query = wire.query;
        amdgpu_info_capacity = wire.return_size;
        int prepared = lpr_amdgpu_info_prepare(&wire);
        if (prepared) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return amdgpu_info_failure(wire.query, amdgpu_info_capacity, "validate", prepared, wait_not_ready);
        }
        const int aux_status = lpr_drm_aux_create(wire.return_size,
            reusable_aux, &aux_fd, &aux_mapping, &aux_map_size);
        if (aux_status) {
            lpr_destroy_tty_wire_page(page_fd, page);
            return amdgpu_info_failure(wire.query, amdgpu_info_capacity, "aux-map", aux_status, wait_not_ready);
        }
        amdgpu_info_pointer = wire.return_pointer;
        wire.return_pointer = 0;
        lpr_memcpy(ioctl->data, &wire, sizeof(wire));
        ioctl->arg_size = sizeof(wire);
        ioctl->data_size = sizeof(wire);
        ioctl->aux_size = wire.return_size;
        wire_kind = LPR_DRM_WIRE_AMDGPU_INFO;
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
                        (uint64_t)input_wait_fd);
                lpr_drm_aux_destroy(aux_fd, aux_mapping, aux_map_size);
                lpr_destroy_tty_wire_page(page_fd, page);
                return pair_status;
            }
            const int64_t installed =
                lpr_sync_file_install_wait(output_wait_fd);
            output_wait_fd = -1;
            if (installed < 0) {
                (void)lpr_close_native_fd_if_open(
                    (uint64_t)output_notify_fd);
                output_notify_fd = -1;
                if (input_wait_fd >= 16)
                    (void)lpr_close_native_fd_if_open(
                        (uint64_t)input_wait_fd);
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
    pacha_fd_t transfer_fds[3];
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
        (void)lpr_close_native_fd_if_open((uint64_t)input_wait_fd);
        input_wait_fd = -1;
    }
    if (output_notify_fd >= 16) {
        (void)lpr_close_native_fd_if_open((uint64_t)output_notify_fd);
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
    if (status != 0 && wire_kind == LPR_DRM_WIRE_AMDGPU_INFO)
        (void)amdgpu_info_failure(amdgpu_info_query, amdgpu_info_capacity, "rpc", status, wait_not_ready);
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
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_KMS_QUERY) {
        status = lpr_kms_query_reply(ioctl, &kms_shape, kms_snapshot,
            aux_mapping, arg);
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_CONNECTOR) {
        gpud_drm_kms_connector_wire_t *wire = (gpud_drm_kms_connector_wire_t *)ioctl->data;
        if (connector_input.count_modes >= wire->value.count_modes &&
            connector_input.modes_ptr)
            lpr_memcpy((void *)(uintptr_t)connector_input.modes_ptr,
                wire->modes, wire->value.count_modes * sizeof(wire->modes[0]));
        if (connector_input.count_encoders >= wire->value.count_encoders &&
            connector_input.encoders_ptr)
            lpr_memcpy((void *)(uintptr_t)connector_input.encoders_ptr,
                wire->encoders, wire->value.count_encoders * sizeof(wire->encoders[0]));
        if (connector_input.count_props >= wire->value.count_props) {
            if (connector_input.props_ptr)
                lpr_memcpy((void *)(uintptr_t)connector_input.props_ptr,
                    wire->props, wire->value.count_props * sizeof(wire->props[0]));
            if (connector_input.prop_values_ptr)
                lpr_memcpy((void *)(uintptr_t)connector_input.prop_values_ptr,
                    wire->prop_values,
                    wire->value.count_props * sizeof(wire->prop_values[0]));
        }
        wire->value.encoders_ptr = connector_input.encoders_ptr;
        wire->value.modes_ptr = connector_input.modes_ptr;
        wire->value.props_ptr = connector_input.props_ptr;
        wire->value.prop_values_ptr = connector_input.prop_values_ptr;
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value,
            sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_CONNECTOR_AUX) {
        gpud_drm_mode_get_connector_t *value =
            (gpud_drm_mode_get_connector_t *)ioctl->data;
        const uint8_t *data = (const uint8_t *)aux_mapping;
        const uint64_t prop_offset = (uint64_t)connector_input.count_modes * 68;
        const uint64_t encoder_offset = prop_offset +
            (uint64_t)connector_input.count_props * 16;
        if (connector_input.count_props >= value->count_props)
            for (uint32_t i = 0; i < value->count_props; ++i) {
                uint32_t reserved;
                lpr_memcpy(&reserved, data + prop_offset + i * 16 + 4, 4);
                if (reserved != 0) {
                    status = -LPR_LINUX_EIO;
                    break;
                }
            }
        if (status == 0 && connector_input.count_modes >= value->count_modes &&
            connector_input.modes_ptr)
            lpr_memcpy((void *)(uintptr_t)connector_input.modes_ptr,
                data, value->count_modes * 68);
        if (status == 0 && connector_input.count_props >= value->count_props)
            for (uint32_t i = 0; i < value->count_props; ++i) {
                uint32_t property;
                uint64_t property_value;
                lpr_memcpy(&property, data + prop_offset + i * 16, 4);
                lpr_memcpy(&property_value, data + prop_offset + i * 16 + 8, 8);
                if (connector_input.props_ptr)
                    lpr_memcpy((void *)(uintptr_t)(connector_input.props_ptr +
                        (uint64_t)i * 4), &property, 4);
                if (connector_input.prop_values_ptr)
                    lpr_memcpy((void *)(uintptr_t)(connector_input.prop_values_ptr +
                        (uint64_t)i * 8), &property_value, 8);
            }
        if (status == 0 && connector_input.count_encoders >= value->count_encoders &&
            connector_input.encoders_ptr)
            lpr_memcpy((void *)(uintptr_t)connector_input.encoders_ptr,
                data + encoder_offset, value->count_encoders * 4);
        if (status == 0) {
            value->encoders_ptr = connector_input.encoders_ptr;
            value->modes_ptr = connector_input.modes_ptr;
            value->props_ptr = connector_input.props_ptr;
            value->prop_values_ptr = connector_input.prop_values_ptr;
            lpr_memcpy((void *)(uintptr_t)arg, value, sizeof(*value));
        }
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_CRTC) {
        gpud_drm_kms_crtc_wire_t *wire = (gpud_drm_kms_crtc_wire_t *)ioctl->data;
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_DIRTY_FB) {
        /* DIRTYFB has no output fields. Preserve the caller's clips pointer. */
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_PROPERTY_BLOB) {
        gpud_drm_kms_property_blob_wire_t *wire =
            (gpud_drm_kms_property_blob_wire_t *)ioctl->data;
        const gpud_drm_mode_get_blob_t *user = (const gpud_drm_mode_get_blob_t *)(uintptr_t)arg;
        const uint64_t user_data = user->data;
        if (user->length == wire->value.length && user->data != 0)
            lpr_memcpy((void *)(uintptr_t)user->data, wire->data, user->length);
        lpr_memcpy((void *)(uintptr_t)arg, &wire->value, sizeof(wire->value));
        ((gpud_drm_mode_get_blob_t *)(uintptr_t)arg)->data = user_data;
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_PROPERTY_BLOB_AUX) {
        const gpud_drm_mode_get_blob_t *wire =
            (const gpud_drm_mode_get_blob_t *)ioctl->data;
        if (wire->length == blob_capacity)
            lpr_memcpy((void *)(uintptr_t)blob_pointer,
                aux_mapping, blob_capacity);
        gpud_drm_mode_get_blob_t *user =
            (gpud_drm_mode_get_blob_t *)(uintptr_t)arg;
        user->length = wire->length;
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
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_AMDGPU_GEM_OP) {
        if (amdgpu_gem_op_pointer) {
            if (!aux_mapping || ioctl->aux_size != 32)
                status = -LPR_LINUX_EIO;
            else
                lpr_memcpy((void *)(uintptr_t)amdgpu_gem_op_pointer, aux_mapping, 32);
        }
        /* Neither operation changes native argument fields. In particular,
         * never copy the pointer-free 24-byte plan over a 16-byte argument. */
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_AMDGPU_INFO) {
        const gpud_drm_amdgpu_info_t *wire =
            (const gpud_drm_amdgpu_info_t *)ioctl->data;
        if (amdgpu_info_query == KB2_GPU_DRM_AMDGPU_INFO_QUERY_DEVICE_INFO) {
            gpud_drm_amdgpu_device_native_t device = {0};
#define UNPACK_DEVICE(member, field) \
            lpr_memcpy(&device.member, (const unsigned char *)aux_mapping + \
                KB2_GPU_DRM_AMDGPU_RECORD_INFO_DEVICE_##field##_OFFSET, \
                sizeof(device.member));
            KB2_AMDGPU_DEVICE_FIELDS(UNPACK_DEVICE)
#undef UNPACK_DEVICE
            lpr_memcpy((void *)(uintptr_t)amdgpu_info_pointer,
                &device, lpr_amdgpu_info_copy_size(amdgpu_info_capacity, sizeof(device)));
        } else if (amdgpu_info_query == KB2_GPU_DRM_AMDGPU_INFO_QUERY_HW_IP_INFO) {
            gpud_drm_amdgpu_hw_ip_native_t hw_ip = {0};
#define UNPACK_HW_IP(member, field) \
            lpr_memcpy(&hw_ip.member, (const unsigned char *)aux_mapping + \
                KB2_GPU_DRM_AMDGPU_RECORD_INFO_HW_IP_##field##_OFFSET, \
                sizeof(hw_ip.member));
            KB2_AMDGPU_HW_IP_FIELDS(UNPACK_HW_IP)
#undef UNPACK_HW_IP
            lpr_memcpy((void *)(uintptr_t)amdgpu_info_pointer,
                &hw_ip, lpr_amdgpu_info_copy_size(amdgpu_info_capacity, sizeof(hw_ip)));
        } else {
            lpr_memcpy((void *)(uintptr_t)amdgpu_info_pointer,
                aux_mapping, lpr_amdgpu_info_copy_size(amdgpu_info_capacity, wire->return_size));
        }
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_AMDGPU_CS) {
        lpr_memcpy((void *)(uintptr_t)arg, ioctl->data, sizeof(uint64_t));
    } else if (status == 0 && wire_kind == LPR_DRM_WIRE_GAMMA) {
        if (command == GPUD_DRM_IOCTL_MODE_GETGAMMA) {
            const uint64_t bytes = (uint64_t)gamma_count * sizeof(uint16_t);
            for (unsigned int i = 0; i < 3; ++i)
                lpr_memcpy((void *)(uintptr_t)gamma_pointer[i],
                    (const uint8_t *)aux_mapping + i * bytes, bytes);
        }
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
    } else if (status == 0 &&
        wire_kind == LPR_DRM_WIRE_CRTC_QUEUE_SEQUENCE) {
        gpud_drm_crtc_queue_sequence_t *user =
            (gpud_drm_crtc_queue_sequence_t *)(uintptr_t)arg;
        const gpud_drm_crtc_queue_sequence_t *wire =
            (const gpud_drm_crtc_queue_sequence_t *)ioctl->data;
        user->sequence = wire->sequence;
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
    const pacha_fd_t page_fd = lpr_create_tty_wire_page(&page);
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
    pacha_fd_t received[2] = {-1, -1};
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
    const int64_t status = lpr_ipc_message(PACHAOS_SYSCALL_IPC_RECV, (uint64_t)drm->wait_fd.raw, 0, (uint64_t)(uintptr_t)((uint64_t)(uintptr_t)&message), 0, 0, 0);
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

int64_t lpr_drm_read_events(uint64_t fd, uint64_t buf, uint64_t count, unsigned *wait_not_ready __attribute__((unused)))
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
        const pacha_fd_t page_fd = lpr_create_tty_wire_page(&page);
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
            if ((header[0] == GPUD_DRM_EVENT_FLIP_COMPLETE ||
                 header[0] == GPUD_DRM_EVENT_VBLANK ||
                 header[0] == GPUD_DRM_EVENT_CRTC_SEQUENCE) &&
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
            wait_status = lpr_wait_graph_block(&graph, &deadline, wait_not_ready);
        if (wait_status != 0) return wait_status;
    }
}

pacha_fd_t lpr_drm_native_wait_fd(uint64_t fd)
{
    lpr_drm_backend_t *drm = lpr_drm_backend(fd);
    return drm != 0 ? drm->wait_fd.raw : -1;
}
