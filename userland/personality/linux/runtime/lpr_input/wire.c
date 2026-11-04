/* SPDX-License-Identifier: MIT */
#include "../lpr_filed_internal.h"
#include "wire.h"
#include "wire_cache.h"

static struct lpr_input_wire_cache cache = { .fd = -1 };
static int create(void **page)
{
    /* CLOEXEC retires the process cache on successful exec. This FD is not
     * PRIVATE: fork inherits a real child-owned FD which child cleanup can
     * close without guessing whether its numeric slot was already reused. */
    const uint64_t rights = PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    int64_t fd = lpr_pacha_syscall3(PACHAOS_SYSCALL_VMO_CREATE,
        PACHA_SERVICE_PAGE_BYTES, rights, PACHA_FD_FLAG_CLOEXEC);
    if (fd < 16) return (int)lpr_pacha_status_to_errno(fd);
    int64_t mapped = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP, (uint64_t)fd,
        0, PACHA_SERVICE_PAGE_BYTES, PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED, 0);
    if (mapped < 4096) {
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)fd);
        return (int)lpr_pacha_status_to_errno(mapped);
    }
    *page = (void *)(uintptr_t)mapped;
    return (int)fd;
}
static void destroy(int fd, void *page)
{
    if (page) (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
        (uint64_t)(uintptr_t)page, PACHA_SERVICE_PAGE_BYTES);
    if (fd >= 16) (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, (uint64_t)(uint32_t)fd);
}
static const struct lpr_input_wire_io io = {
    lpr_state_lock, lpr_state_unlock, create, destroy
};
pacha_fd_t lpr_input_wire_acquire(void **page) { return lpr_input_wire_cache_acquire(&cache, &io, page); }
void lpr_input_wire_release(void) { lpr_input_wire_cache_release(&cache, &io); }
void lpr_input_wire_fork_lock(void) { lpr_state_lock(&cache.lock); }
void lpr_input_wire_fork_unlock(void) { lpr_state_unlock(&cache.lock); }
void lpr_input_wire_after_fork_child(void) { lpr_input_wire_cache_child(&cache, &io); }
