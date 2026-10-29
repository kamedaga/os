/* SPDX-License-Identifier: MIT */
#include "control_cache.h"
#include "../lpr_filed_internal.h"

/* Only request/reply scratch is retained, never a file's data VMO or rights.
 * A lease remains exclusive until a checked synchronous completion. Four
 * idle pages bound retention; larger controls retain their old lifetime. */
static struct lpr_fs_control_lease idle[4];
static struct lpr_fs_control_lease *active;
static volatile uint32_t cache_lock;

void lpr_fs_control_fork_lock(void) { lpr_state_lock(&cache_lock); }
void lpr_fs_control_fork_unlock(void) { lpr_state_unlock(&cache_lock); }

static void destroy(struct lpr_fs_control_lease *lease)
{
    if (lease->address)
        (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, lease->address, lease->bytes);
    if (lease->fd >= 16)
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, lease->fd);
    *lease = (struct lpr_fs_control_lease){0};
}

int64_t lpr_fs_control_acquire(struct lpr_fs_control_lease *lease, uint64_t bytes)
{
    *lease = (struct lpr_fs_control_lease){ .bytes = bytes };
    lpr_fs_control_fork_lock();
    if (bytes == 4096) for (unsigned i = 0; i < 4; i++) {
        if (!idle[i].address) continue;
        *lease = idle[i];
        idle[i] = (struct lpr_fs_control_lease){0};
        break;
    }
    if (!lease->address) {
        const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
        const int64_t fd = lpr_pacha_syscall3(PACHAOS_SYSCALL_VMO_CREATE,
            bytes, rights, PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC);
        if (fd < 16) {
            lpr_fs_control_fork_unlock();
            return fd < 0 ? lpr_pacha_status_to_errno(fd) : -LPR_LINUX_EIO;
        }
        lease->fd = fd;
        const int64_t address = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP,
            fd, 0, bytes, PACHAOS_PROT_READ | PACHAOS_PROT_WRITE, PACHAOS_MMAP_SHARED, 0);
        if (address < 4096) {
            destroy(lease);
            lpr_fs_control_fork_unlock();
            return address < 0 ? lpr_pacha_status_to_errno(address) : -LPR_LINUX_EIO;
        }
        lease->address = address;
    }
    /* Match a fresh VMO's contents, including unused reply and padding bytes. */
    lpr_memset((void *)(uintptr_t)lease->address, 0, bytes);
    lease->next = active;
    active = lease;
    lpr_fs_control_fork_unlock();
    return 0;
}

void lpr_fs_control_release(struct lpr_fs_control_lease *lease, int completed)
{
    lpr_fs_control_fork_lock();
    struct lpr_fs_control_lease **cursor = &active;
    while (*cursor && *cursor != lease) cursor = &(*cursor)->next;
    if (*cursor) *cursor = lease->next;
    /* A failed/uncertain call may still own its transferred capability.
     * Retire the object, so a late writer cannot reach another request. */
    if (completed && lease->bytes == 4096) {
        for (unsigned i = 0; i < 4; i++) if (!idle[i].address) {
            idle[i] = *lease;
            idle[i].next = NULL;
            *lease = (struct lpr_fs_control_lease){0};
            break;
        }
    }
    destroy(lease);
    lpr_fs_control_fork_unlock();
}

static int child_unmap(const struct lpr_fs_control_lease *lease)
{
    return lease->address && lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
        lease->address, lease->bytes) ? -LPR_LINUX_EIO : 0;
}

int lpr_fs_control_fork_child(void)
{
    /* Native fork omits PRIVATE descriptors but copies mappings. Unmap the
     * inherited shared pages without touching their contents or FD numbers. */
    for (struct lpr_fs_control_lease *lease = active; lease; lease = lease->next)
        if (child_unmap(lease)) return -LPR_LINUX_EIO;
    for (unsigned i = 0; i < 4; i++) if (child_unmap(&idle[i])) return -LPR_LINUX_EIO;
    active = NULL;
    for (unsigned i = 0; i < 4; i++) idle[i] = (struct lpr_fs_control_lease){0};
    cache_lock = 0;
    return 0;
}
