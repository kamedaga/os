#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../userland/personality/linux/runtime/lpr_inotify.c"
#include "../userland/personality/linux/runtime/lpr_fd/table.c"

lpr_state_t lpr_state;
static lpr_fd_entry_t entries[8];
static lpr_ofd_t objects[2];
static lpr_backend_record_t backends[2];
static lpr_event_backend_t event, replacement;
static unsigned drops, close_on_drain, close_on_stat, close_on_lock;

static void finish_drop(lpr_fd_drop_t *drop)
{
    if (!drop->ready) return;
    assert(drop->state == &event);
    free((void *)(uintptr_t)event.counter);
    event.counter = 0;
    drops++;
}
void lpr_fd_unpin(const lpr_fd_pin_t *pin)
{
    lpr_fd_drop_t drop;
    assert(lpr_fd_table_unpin(&lpr_control_fd_table, pin, &drop) == 0);
    finish_drop(&drop);
}
static void close_and_reuse(void)
{
    lpr_fd_drop_t drop;
    assert(lpr_fd_table_close(&lpr_control_fd_table, 3, &drop) == 0);
    finish_drop(&drop);
    const lpr_fd_install_t install = {
        .ops_id = LPR_FD_OPS_EVENT, .rights = LPR_FD_RIGHT_READ,
        .backend_state = &replacement, .backend_state_bytes = sizeof(replacement)};
    assert(lpr_fd_table_install_at(&lpr_control_fd_table, 3, &install) == 0);
}
void *lpr_memset(void *p, int c, size_t n) { return memset(p, c, n); }
void *lpr_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
int lpr_strcmp(const char *a, const char *b) { return strcmp(a, b); }
void lpr_state_lock(volatile uint32_t *word)
{
    if (close_on_lock) { close_on_lock = 0; close_and_reuse(); }
    assert(*word == 0); *word = 1;
}
void lpr_state_unlock(volatile uint32_t *word) { assert(*word == 1); *word = 0; }
int64_t lpr_cwd_normalize(const char *path, char *out, uint64_t bytes)
{
    assert(strlen(path) < bytes); strcpy(out, path); return 0;
}
int64_t lpr_linux_newfstatat(uint64_t dir, uint64_t path, uint64_t raw, uint64_t flags)
{
    (void)dir; (void)path; (void)flags;
    if (close_on_stat) { close_on_stat = 0; close_and_reuse(); }
    lpr_linux_stat_t *st = (void *)(uintptr_t)raw;
    memset(st, 0, sizeof(*st)); st->st_size = 1;
    return 0;
}
int64_t lpr_pacha_syscall3(uint64_t nr, uint64_t fd, uint64_t buf, uint64_t bytes)
{
    (void)buf; (void)bytes;
    assert(nr == PACHAOS_SYSCALL_FD_READ && fd == 60);
    if (close_on_drain) { close_on_drain = 0; close_and_reuse(); }
    return 0;
}
void lpr_wait_graph_init(lpr_wait_graph_t *g) { memset(g, 0, sizeof(*g)); }
int64_t lpr_wait_graph_add_native(lpr_wait_graph_t *g, int fd, uint64_t events)
{
    (void)g; assert(fd == 60 && events == 1 && drops == 0); return 0;
}
int64_t lpr_wait_deadline_init(lpr_wait_deadline_t *d, int64_t timeout)
{ (void)d; assert(timeout == -1); return 0; }
int64_t lpr_wait_graph_block(lpr_wait_graph_t *g, const lpr_wait_deadline_t *d)
{ (void)g; (void)d; return -LPR_LINUX_EINTR; }

static lpr_inotify_instance_t *setup(void)
{
    memset(&event, 0, sizeof(event));
    memset(&replacement, 0, sizeof(replacement));
    replacement.wait_fd.raw = 99;
    lpr_fd_table_init(&lpr_control_fd_table, entries, 8, objects, 2, backends, 2);
    lpr_inotify_instance_t *instance = calloc(1, LPR_INOTIFY_INSTANCE_BYTES);
    assert(instance);
    instance->magic = LPR_INOTIFY_INSTANCE_MAGIC;
    instance->next_wd = 1;
    event.active = 1; event.subtype = LPR_EVENT_BACKEND_INOTIFY;
    event.counter = (uintptr_t)instance; event.deadline_ns = LPR_INOTIFY_INSTANCE_BYTES;
    event.wait_fd.raw = 60; event.flags = LPR_LINUX_O_NONBLOCK;
    const lpr_fd_install_t install = {
        .ops_id = LPR_FD_OPS_EVENT, .rights = LPR_FD_RIGHT_READ,
        .backend_state = &event, .backend_state_bytes = sizeof(event)};
    assert(lpr_fd_table_install_at(&lpr_control_fd_table, 3, &install) == 0);
    drops = close_on_drain = close_on_stat = close_on_lock = 0;
    return instance;
}
int main(void)
{
    setup(); close_on_drain = 1;
    assert(lpr_linux_inotify_poll_events(3, 1) == 0 && drops == 1);

    lpr_inotify_instance_t *instance = setup();
    instance->watches[0] = (lpr_inotify_watch_t){.active=1, .wd=7, .mask=LPR_IN_MODIFY};
    close_on_stat = 1;
    lpr_linux_inotify_event_t out;
    assert(lpr_linux_inotify_read(3, (uintptr_t)&out, sizeof(out)) == sizeof(out));
    assert(out.wd == 7 && out.mask == LPR_IN_MODIFY && drops == 1);

    setup(); event.flags = 0; close_on_drain = 1;
    assert(lpr_linux_inotify_read(3, (uintptr_t)&out, sizeof(out)) == -LPR_LINUX_EINTR);
    assert(drops == 1); /* Wait kept the original timer, despite FD reuse. */

    setup(); close_on_stat = 1;
    assert(lpr_linux_inotify_add_watch(3, (uintptr_t)"/file", LPR_IN_MODIFY) == 1);
    assert(drops == 1);

    instance = setup(); instance->watches[0].active = 1; instance->watches[0].wd = 7;
    close_on_lock = 1;
    assert(lpr_linux_inotify_rm_watch(3, 7) == 0 && drops == 1);

    setup();
    assert(lpr_linux_inotify_read(3, 0, sizeof(out)) == -LPR_LINUX_EFAULT);
    assert(lpr_linux_inotify_add_watch(3, 0, LPR_IN_MODIFY) == -LPR_LINUX_EFAULT);
    assert(lpr_linux_inotify_rm_watch(3, UINT64_MAX) == -LPR_LINUX_EINVAL);
    assert(objects[entries[3].ofd_index].pin_count == 0);
    assert(!lpr_linux_inotify_active(UINT64_MAX));
    close_and_reuse(); assert(drops == 1);
    puts("LPR_INOTIFY_CLOSE_RACE_UNIT=OK");
    return 0;
}
