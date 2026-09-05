#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../userland/personality/linux/runtime/lpr_epoll.c"

lpr_state_t lpr_state;
static lpr_fd_entry_t entries[6];
static lpr_ofd_t objects[2];
static lpr_backend_record_t backends[2];
static lpr_epoll_backend_t epoll_backend;
static union { max_align_t align; unsigned char bytes[4096]; } storage;
static uint32_t ready_events;
static unsigned notifications, failures, locked;

void *lpr_memset(void *dst, int value, size_t bytes) { return memset(dst, value, bytes); }
void lpr_fd_table_lock(lpr_fd_table_t *table) { (void)table; assert(!locked); locked = 1; }
void lpr_fd_table_unlock(lpr_fd_table_t *table) { (void)table; assert(locked); locked = 0; }
uint8_t lpr_ofd_ops_id(const lpr_ofd_t *object) { return backends[object->backend.index].ops_id; }
void *lpr_backend_state_from_ofd(const lpr_ofd_t *object) { return backends[object->backend.index].state; }
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t fd, uint64_t message)
{
    (void)message;
    assert(nr == PACHAOS_SYSCALL_IPC_SEND && fd == 60 && !locked);
    notifications++;
    return 0;
}
int64_t lpr_linux_poll_current(uint64_t raw, uint64_t count)
{
    lpr_linux_pollfd_t *fds = (void *)(uintptr_t)raw;
    assert(!locked);
    int64_t ready = 0;
    for (uint64_t i = 0; i < count; ++i) {
        fds[i].revents = (int16_t)(ready_events & (uint16_t)fds[i].events);
        ready += fds[i].revents != 0;
    }
    return ready;
}
int64_t lpr_linux_poll_cached(uint64_t raw, uint64_t count)
{
    return lpr_linux_poll_current(raw, count);
}

static void expect(int condition, const char *name)
{
    if (!condition) { fprintf(stderr, "FAIL: %s\n", name); failures++; }
}
static void setup(uint32_t events)
{
    memset(&lpr_state, 0, sizeof(lpr_state));
    memset(entries, 0, sizeof(entries));
    memset(objects, 0, sizeof(objects));
    memset(backends, 0, sizeof(backends));
    memset(&epoll_backend, 0, sizeof(epoll_backend));
    memset(&storage, 0, sizeof(storage));
    lpr_control_fd_table.entries = entries;
    lpr_control_fd_table.entry_count = 6;
    lpr_control_fd_table.ofds = objects;
    lpr_control_fd_table.ofd_count = 2;
    lpr_control_fd_table.backends = backends;
    lpr_control_fd_table.backend_count = 2;
    entries[3] = (lpr_fd_entry_t){.active=1, .ofd_index=0, .ofd_generation=7};
    entries[5] = entries[3]; /* dup alias of the registered descriptor */
    entries[4] = (lpr_fd_entry_t){.active=1, .ofd_index=1, .ofd_generation=9};
    objects[0] = (lpr_ofd_t){.active=1, .generation=7, .backend={.index=0}};
    objects[1] = (lpr_ofd_t){.active=1, .generation=9, .backend={.index=1}};
    backends[0].ops_id = LPR_FD_OPS_SOCKET;
    backends[1].ops_id = LPR_FD_OPS_EPOLL;
    backends[1].state = &epoll_backend;
    epoll_backend.instance = (uint64_t)(uintptr_t)&storage;
    epoll_backend.map_bytes = LPR_EPOLL_INSTANCE_BYTES;
    epoll_backend.notify_fd.raw = 60;
    lpr_epoll_instance_t *instance = (void *)&storage;
    instance->magic = LPR_EPOLL_INSTANCE_MAGIC;
    instance->capacity = LPR_EPOLL_MAX_INTERESTS;
    instance->count = 1;
    instance->interests[0] = (lpr_epoll_interest_t){
        .target_fd=3, .target_ofd_index=0, .target_generation=7,
        .events=events, .data=1234};
    ready_events = 0;
    notifications = 0;
}
static uint32_t take_event(void)
{
    lpr_epoll_snapshot_t snapshot[LPR_EPOLL_MAX_INTERESTS];
    lpr_linux_epoll_event_t event = {0};
    uint32_t count;
    assert(lpr_epoll_snapshot(4, snapshot, &count) == 0);
    const int64_t result = lpr_epoll_scan(4, snapshot, count, &event, 1, 1, 1);
    assert(result == 0 || result == 1);
    assert(!result || event.data == 1234);
    return result ? event.events : 0;
}
static void note_io(uint64_t fd, uint32_t direction, int64_t result)
{
    lpr_epoll_note_fd_state(fd, direction, result);
}
int main(void)
{
    const uint32_t both = LPR_EPOLLIN | LPR_EPOLLOUT;
    for (unsigned direction = LPR_EPOLLIN; direction <= LPR_EPOLLOUT; direction <<= 2) {
        setup(both | LPR_EPOLLET);
        ready_events = both;
        expect(take_event() == both, "initial readiness");
        expect(take_event() == 0, "ET does not repeat unchanged readiness");
        /* I/O returned EAGAIN; the peer becomes ready before the state query.
         * The mock deliberately exposes only the later, refilled state.
         */
        note_io(5, direction, -LPR_LINUX_EAGAIN);
        expect(notifications == 1, "refill after EAGAIN wakes the epoll waiter through a dup alias");
        expect(take_event() == direction, "refill reports only the direction that became empty");
        expect(take_event() == 0, "refill edge is consumed once");
    }
    setup(LPR_EPOLLIN | LPR_EPOLLET);
    ready_events = LPR_EPOLLIN;
    expect(take_event() == LPR_EPOLLIN, "partial-read initial readiness");
    note_io(3, LPR_EPOLLIN, 1);
    expect(take_event() == 0 && notifications == 0, "partial successful read does not synthesize an edge");
    note_io(3, LPR_EPOLLIN, -LPR_LINUX_EINTR);
    expect(take_event() == 0, "unrelated I/O error does not rearm");

    setup(LPR_EPOLLIN | LPR_EPOLLET);
    ready_events = LPR_EPOLLIN;
    note_io(3, LPR_EPOLLIN, 1); /* Queue an event without consuming it. */
    ready_events = 0;
    note_io(3, LPR_EPOLLIN, -LPR_LINUX_EAGAIN);
    expect(take_event() == LPR_EPOLLIN, "EAGAIN preserves an already queued edge");

    setup(LPR_EPOLLIN | LPR_EPOLLET | LPR_EPOLLONESHOT);
    ready_events = LPR_EPOLLIN;
    expect(take_event() == LPR_EPOLLIN, "one-shot initial readiness");
    note_io(3, LPR_EPOLLIN, -LPR_LINUX_EAGAIN);
    expect(take_event() == 0 && notifications == 0, "EAGAIN does not rearm a disabled one-shot");
    if (failures) return 1;
    puts("LPR_EPOLL_IO_RACE_UNIT=OK");
    return 0;
}
