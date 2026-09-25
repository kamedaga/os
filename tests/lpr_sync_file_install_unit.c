#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"
#include "../userland/personality/linux/runtime/lpr_drm/dmabuf.h"

lpr_state_t lpr_state;
static lpr_fd_entry_t entries[64];
static lpr_ofd_t ofds[64];
static lpr_backend_record_t backends[64];
static lpr_sync_file_backend_t allocated;
static lpr_event_backend_t allocated_event;
static lpr_dmabuf_backend_t allocated_dmabuf;
static lpr_drm_backend_t allocated_drm;
static int compete, fail_alloc, bad_kind, closes, frees;
static uint32_t poll_revents;
int64_t lpr_pacha_syscall2(uint64_t number, uint64_t a0, uint64_t a1)
{
    assert(number == PACHAOS_SYSCALL_FD_POLL && a1 == 1);
    struct pacha_pollfd *fd = (void *)(uintptr_t)a0;
    assert(fd->fd == 80 && (fd->events & PACHA_FD_EVENT_HANGUP));
    fd->revents = poll_revents;
    return poll_revents ? 1 : 0;
}
static lpr_device_backend_t device;
static const lpr_fd_install_t competitor = { .ops_id = LPR_FD_OPS_DEVICE,
    .backend_state = &device, .backend_state_bytes = sizeof(device) };

void *lpr_memset(void *p, int c, size_t n) { return memset(p, c, n); }
void lpr_fd_arrays_init(void) {}
int lpr_native_fd_info(uint64_t fd, struct pacha_fd_info *info)
{
    assert(fd == 80);
    info->kind = bad_kind ? PACHA_FD_KIND_VMO : PACHA_FD_KIND_CHANNEL;
    info->rights = PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE;
    return 1;
}
int64_t lpr_close_native_fd_if_open(uint64_t fd) { assert(fd == 80 || fd == 81); ++closes; return 0; }
int64_t lpr_pacha_syscall3(uint64_t nr, uint64_t address, uint64_t rights, uint64_t flags)
{
    assert(nr == PACHAOS_SYSCALL_IPC_CHANNEL_CREATE);
    (void)rights; (void)flags;
    uint64_t *pair = (void *)(uintptr_t)address;
    pair[0] = 80; pair[1] = 81;
    return 0;
}
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{ assert(nr == PACHAOS_SYSCALL_FD_CLOSE); return lpr_close_native_fd_if_open(fd); }
int64_t pacha_kernel_status_to_errno(int64_t status) { assert(status == 0); return 0; }
void *lpr_backend_state_alloc(uint64_t n)
{
    assert(n == sizeof(allocated) || n == sizeof(allocated_event) ||
        n == sizeof(allocated_dmabuf) || n == sizeof(allocated_drm));
    /* A backend slab allocation can yield. Another thread publishes the
     * number observed free before allocation by the old implementation. */
    if (compete) assert(lpr_fd_table_install_at(&lpr_control_fd_table, 3, &competitor) == 0);
    return fail_alloc ? NULL : n == sizeof(allocated) ? (void *)&allocated :
        n == sizeof(allocated_event) ? (void *)&allocated_event :
        n == sizeof(allocated_drm) ? (void *)&allocated_drm : (void *)&allocated_dmabuf;
}
int64_t lpr_backend_state_free(void *p, uint64_t n)
{ assert((p == &allocated && n == sizeof(allocated)) ||
    (p == &allocated_event && n == sizeof(allocated_event)) ||
    (p == &allocated_dmabuf && n == sizeof(allocated_dmabuf)) ||
    (p == &allocated_drm && n == sizeof(allocated_drm))); ++frees; return 0; }
int lpr_fd_table_ensure_capacity(uint64_t n) { (void)n; return -1; }

/* Keep the historical select-then-install path linkable for the negative
 * control: running this test with the old source must fail the race case. */
int lpr_fd_slot_alloc(void) { return 3; }
void lpr_control_close_fd(uint64_t fd) { (void)fd; }
int lpr_control_install_fd(uint64_t fd, uint8_t ops, uint64_t flags, uint64_t id, uint64_t offset)
{
    (void)id; (void)offset;
    void *state = lpr_backend_state_alloc(sizeof(allocated));
    if (!state) return -LPR_LINUX_ENOMEM;
    const lpr_fd_install_t install = { .ops_id = ops, .backend_state = state,
        .backend_state_bytes = sizeof(allocated), .fd_flags = flags ? LPR_FD_ENTRY_CLOEXEC : 0 };
    return lpr_fd_table_install_at(&lpr_control_fd_table, fd, &install) ? -LPR_LINUX_EMFILE : 0;
}
lpr_sync_file_backend_t *lpr_sync_file_backend(uint64_t fd)
{ (void)fd; return &allocated; }
lpr_event_backend_t *lpr_event_backend(uint64_t fd)
{ (void)fd; return &allocated_event; }

static void reset(void)
{
    memset(&lpr_state, 0, sizeof(lpr_state));
    memset(&allocated, 0, sizeof(allocated));
    memset(&allocated_event, 0, sizeof(allocated_event));
    lpr_fd_table_init(&lpr_control_fd_table, entries, 64, ofds, 64, backends, 64);
    lpr_fd_table_capacity = 64;
    compete = fail_alloc = bad_kind = closes = frees = 0;
}
int main(void)
{
    reset();
    compete = 1;
    assert(lpr_drm_install(17, LPR_LINUX_O_RDWR | LPR_LINUX_O_CLOEXEC, 80, LPR_DRM_NODE_RENDER) == 4);
    assert(allocated_drm.active && allocated_drm.handle == 17);
    assert(allocated_drm.wait_fd.raw == 80 && allocated_drm.lease_fd.raw == -1);
    assert(allocated_drm.node_kind == LPR_DRM_NODE_RENDER);
    assert(entries[4].fd_flags & LPR_FD_ENTRY_CLOEXEC);
    assert(!closes && !frees);
    reset();
    fail_alloc = 1;
    assert(lpr_drm_install(17, LPR_LINUX_O_RDWR, 80, 0) == -LPR_LINUX_ENOMEM);
    assert(!closes && !frees);
    reset();
    for (unsigned i = 3; i < 64; ++i)
        assert(lpr_fd_table_install_at(&lpr_control_fd_table, i, &competitor) == 0);
    assert(lpr_drm_install(17, LPR_LINUX_O_RDWR, 80, 0) == -LPR_LINUX_EMFILE);
    assert(!closes && frees == 1);
    reset();
    compete = 1;
    assert(lpr_dmabuf_install(80, 81, 19, 8192, LPR_LINUX_O_RDWR | LPR_LINUX_O_CLOEXEC) == 4);
    assert(allocated_dmabuf.native.raw == 80 && allocated_dmabuf.lease_fd.raw == 81);
    assert(allocated_dmabuf.token == 19 && allocated_dmabuf.size == 8192);
    assert(allocated_dmabuf.active && allocated_dmabuf.writable);
    assert(entries[4].fd_flags & LPR_FD_ENTRY_CLOEXEC);
    assert(!closes && !frees);
    reset();
    fail_alloc = 1;
    assert(lpr_dmabuf_install(80, 81, 19, 4096, LPR_LINUX_O_RDWR) == -LPR_LINUX_ENOMEM);
    assert(!closes && !frees);
    reset();
    for (unsigned i = 3; i < 64; ++i)
        assert(lpr_fd_table_install_at(&lpr_control_fd_table, i, &competitor) == 0);
    assert(lpr_dmabuf_install(80, 81, 19, 4096, LPR_LINUX_O_RDWR) == -LPR_LINUX_EMFILE);
    assert(!closes && frees == 1);
    reset();
    compete = 1;
    assert(lpr_sync_file_install_wait(80) == 4);
    assert(entries[3].active && entries[4].active);
    assert(allocated.active && allocated.wait_fd.raw == 80);
    assert(allocated.flags == (LPR_LINUX_O_RDONLY | LPR_LINUX_O_CLOEXEC));
    assert(entries[4].fd_flags & LPR_FD_ENTRY_CLOEXEC);
    assert(!closes && !frees);
    reset();
    fail_alloc = 1;
    assert(lpr_sync_file_install_wait(80) == -LPR_LINUX_ENOMEM);
    assert(closes == 1 && frees == 0);
    reset();
    for (unsigned i = 3; i < 64; ++i)
        assert(lpr_fd_table_install_at(&lpr_control_fd_table, i, &competitor) == 0);
    assert(lpr_sync_file_install_wait(80) == -LPR_LINUX_EMFILE);
    assert(closes == 1 && frees == 1);
    reset();
    bad_kind = 1;
    assert(lpr_sync_file_install_wait(80) == -LPR_LINUX_EBADF);
    assert(closes == 1 && frees == 0);
    reset();
    compete = 1;
    assert(lpr_linux_eventfd2(7, LPR_LINUX_O_CLOEXEC | LPR_LINUX_O_NONBLOCK | LPR_LINUX_EFD_SEMAPHORE) == 4);
    assert(allocated_event.counter == 7 && allocated_event.wait_fd.raw == 80 && allocated_event.notify_fd.raw == 81);
    assert(allocated_event.reserved1 == LPR_LINUX_EFD_SEMAPHORE);
    assert(allocated_event.flags == (LPR_LINUX_O_CLOEXEC | LPR_LINUX_O_NONBLOCK));
    assert(entries[4].fd_flags & LPR_FD_ENTRY_CLOEXEC);
    assert(!closes && !frees);
    reset();
    fail_alloc = 1;
    assert(lpr_linux_eventfd2(0, 0) == -LPR_LINUX_ENOMEM);
    assert(closes == 2 && !frees);
    reset();
    for (unsigned i = 3; i < 64; ++i)
        assert(lpr_fd_table_install_at(&lpr_control_fd_table, i, &competitor) == 0);
    assert(lpr_linux_eventfd2(0, 0) == -LPR_LINUX_EMFILE);
    assert(closes == 2 && frees == 1);
    reset();
    assert(lpr_sync_file_install_wait(80) == 3);
    poll_revents = 0;
    assert(!lpr_sync_file_poll_events(3, 1));
    poll_revents = PACHA_FD_EVENT_HANGUP;
    assert(lpr_sync_file_poll_events(3, 1) == 8);
    assert(lpr_sync_file_poll_events(3, 0) == 8);
    poll_revents |= PACHA_FD_EVENT_READABLE;
    assert(lpr_sync_file_poll_events(3, 1) == 1);
    assert(!lpr_sync_file_poll_events(3, 0));
    puts("sync file atomic install and completion/hangup poll: PASS");
}
