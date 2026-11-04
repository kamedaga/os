#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../userland/personality/linux/runtime/lpr_epoll.c"

lpr_state_t lpr_state;
static lpr_fd_entry_t entries[8];
static lpr_ofd_t objects[4];
static lpr_backend_record_t backends[4];
static lpr_epoll_backend_t epoll_backend;
static union { max_align_t align; unsigned char bytes[LPR_EPOLL_INSTANCE_BYTES]; } storage;
static struct unix_poll_sequence unix_sequence;
static uint32_t ready_events;
static lpr_socket_backend_t mock_socket;
static lpr_event_backend_t mock_event;
static uint64_t event_counter_state[3];
static unsigned locked;
int lpr_fd_table_pin(lpr_fd_table_t *table, lpr_linux_fd_t fd, lpr_fd_pin_t *pin)
{
    assert(!locked);
    if (fd >= table->entry_count || !table->entries[fd].active) return -1;
    lpr_ofd_t *object = &table->ofds[table->entries[fd].ofd_index];
    *pin = (lpr_fd_pin_t){.ops_id=lpr_ofd_ops_id(object),
        .state=lpr_backend_state_from_ofd(object)};
    return 0;
}
void lpr_fd_unpin(const lpr_fd_pin_t *pin) { (void)pin; assert(!locked); }
static struct lpr_unix_socket mock_unix = {.mapped=1};
static int delayed_fd=-1, sequence_error_fd=-1, unix_ready_override=-1;
static unsigned delayed_queries;
static unsigned notifications, failures;

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
int64_t lpr_linux_poll_current(uint64_t raw, uint64_t count, unsigned *wait_not_ready __attribute__((unused)))
{
    lpr_linux_pollfd_t *fds = (void *)(uintptr_t)raw;
    assert(!locked);
    int64_t ready = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (fds[i].fd == delayed_fd) { delayed_queries++; fds[i].revents=0; }
        else fds[i].revents = (int16_t)(ready_events & (uint16_t)fds[i].events);
        ready += fds[i].revents != 0;
    }
    return ready;
}
int64_t lpr_linux_poll_cached(uint64_t raw, uint64_t count, unsigned *wait_not_ready __attribute__((unused)))
{
    return lpr_linux_poll_current(raw, count, 0);
}

int64_t lpr_unix_socket_poll_sequence(uint64_t fd, uint32_t events,
    struct unix_poll_sequence *sequence)
{
    (void)fd;
    assert(!locked);
    if ((int)fd == sequence_error_fd) return -LPR_LINUX_EBADF;
    *sequence = unix_sequence;
    return (unix_ready_override < 0 ? ready_events : (uint32_t)unix_ready_override) & events;
}

lpr_socket_backend_t *lpr_socket_backend(uint64_t fd) { (void)fd; return NULL; }
int64_t lpr_socket_poll_sequence(uint64_t fd, uint32_t events, struct unix_poll_sequence *sequence)
{ return lpr_unix_socket_poll_sequence(fd, events, sequence); }

int64_t lpr_input_poll_sequence(uint64_t fd, uint32_t events,
    struct unix_poll_sequence *sequence)
{
    return lpr_unix_socket_poll_sequence(fd, events, sequence);
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
    backends[0].ops_id = LPR_FD_OPS_SOCKET; backends[0].state=&mock_socket;
    backends[0].state = &mock_unix;
    delayed_fd=-1; sequence_error_fd=-1; unix_ready_override=-1; delayed_queries=0;
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
    assert(lpr_epoll_snapshot(4, snapshot, &count, NULL) == 0);
    const int64_t result = lpr_epoll_scan(4, snapshot, count, &event, 1, 1, 1, 0);
    assert(result == 0 || result == 1);
    assert(!result || event.data == 1234);
    return result ? event.events : 0;
}
static void note_io(uint64_t fd, uint32_t direction, int64_t result)
{
    lpr_epoll_note_fd_state(fd, direction, result, 0);
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

    setup(both | LPR_EPOLLET);
    backends[0].ops_id = LPR_FD_OPS_UNIX;
    unix_sequence = (struct unix_poll_sequence){ .source = 9, .read = 1, .write = 1 };
    ready_events = both;
    expect(take_event() == both && take_event() == 0, "UNIX initial stamp consumed once");
    unix_sequence.read++;
    expect(take_event() == LPR_EPOLLIN && take_event() == 0,
        "remote receive/refill reports IN without spuriously repeating OUT");
    unix_sequence.write++;
    expect(take_event() == LPR_EPOLLOUT && take_event() == 0,
        "remote full/drain reports OUT without spuriously repeating IN");
    setup(LPR_EPOLLIN | LPR_EPOLLET | LPR_EPOLLONESHOT);
    backends[0].ops_id = LPR_FD_OPS_UNIX;
    ready_events = LPR_EPOLLIN;
    expect(take_event() == LPR_EPOLLIN, "UNIX one-shot initial event");
    unix_sequence.read++;
    expect(take_event() == 0, "remote progress does not rearm ONESHOT");

    setup(LPR_EPOLLIN | LPR_EPOLLET);
    backends[0].ops_id = LPR_FD_OPS_INPUT;
    unix_sequence = (struct unix_poll_sequence){.source=7,.read=3};
    ready_events = LPR_EPOLLIN;
    expect(take_event() == LPR_EPOLLIN && take_event() == 0,
        "input report stamp consumed once");
    unix_sequence.read += 3;
    expect(take_event() == LPR_EPOLLIN && take_event() == 0,
        "new input report triggers ET with unread old records");
    setup(LPR_EPOLLIN | LPR_EPOLLET | LPR_EPOLLONESHOT);
    backends[0].ops_id = LPR_FD_OPS_INPUT;
    ready_events = LPR_EPOLLIN;
    expect(take_event() == LPR_EPOLLIN, "input one-shot initial event");
    unix_sequence.read += 3;
    expect(take_event() == 0, "input progress does not rearm disabled ONESHOT");

    setup(LPR_EPOLLIN);
    lpr_epoll_snapshot_t old[LPR_EPOLL_MAX_INTERESTS];
    uint32_t count, report;
    assert(lpr_epoll_snapshot(4, old, &count, NULL) == 0 && count == 1);
    /* Reuse the descriptor after the unlocked readiness query. The watched
     * OFD survives through fd 5, so only that new snapshot may report it. */
    entries[3] = entries[4];
    assert(lpr_epoll_apply_observation(4, old, LPR_EPOLLIN, 1, &report, NULL) == 0);
    expect(report == 0, "reassigned query descriptor cannot publish readiness");
    ready_events = LPR_EPOLLIN;
    expect(take_event() == LPR_EPOLLIN, "surviving dup resolves on next snapshot");

    setup(LPR_EPOLLIN | LPR_EPOLLONESHOT);
    assert(lpr_epoll_snapshot(4, old, &count, NULL) == 0 && count == 1);
    lpr_epoll_instance_t *instance = (void *)&storage;
    instance->generation++; /* CTL publication, even with the same OFD/slot. */
    assert(lpr_epoll_apply_observation(4, old, LPR_EPOLLIN, 1, &report, NULL) == 0);
    expect(report == 0 && !(instance->interests[0].state & LPR_EPOLL_STATE_ONESHOT_DISABLED),
        "obsolete snapshot cannot consume a rearmed one-shot");
    ready_events = LPR_EPOLLIN;
    expect(take_event() == LPR_EPOLLIN, "fresh snapshot consumes the new interest");
    setup(LPR_EPOLLIN);
    lpr_epoll_snapshot_epoch_t epoch;
    assert(lpr_epoll_snapshot(4, old, &count, &epoch) == 0);
    expect(!lpr_epoll_wait_should_retry(4, &epoch, -LPR_LINUX_EINVAL),
        "unchanged graph preserves native EINVAL");
    expect(!lpr_epoll_wait_should_retry(4, &epoch, -LPR_LINUX_EBADF),
        "unchanged graph preserves EBADF");
    ((lpr_epoll_instance_t *)&storage)->generation++;
    expect(lpr_epoll_wait_should_retry(4, &epoch, -LPR_LINUX_EINVAL),
        "concurrent interest retirement rebuilds native invalid graph");
    expect(lpr_epoll_wait_should_retry(4, &epoch, -LPR_LINUX_EBADF),
        "concurrent interest retirement rebuilds closed target graph");
    expect(!lpr_epoll_wait_should_retry(4, &epoch, -LPR_LINUX_EFAULT),
        "interest mutation does not hide unrelated failure");
    expect(!lpr_epoll_wait_should_retry(4, &epoch, LPR_WAIT_RESTART_SYSCALL),
        "signal restart must unwind and deliver rather than retry here");
    entries[4].active = 0;
    expect(!lpr_epoll_wait_should_retry(4, &epoch, -LPR_LINUX_EBADF),
        "closed root does not retry");
    entries[4].active = 1;
    entries[4].ofd_generation++;
    expect(!lpr_epoll_wait_should_retry(4, &epoch, -LPR_LINUX_EBADF),
        "reused root descriptor does not retry");
    setup(LPR_EPOLLIN);
    backends[0].ops_id = LPR_FD_OPS_UNIX;
    lpr_control_fd_table.entry_count=8;
    lpr_control_fd_table.ofd_count=3;
    lpr_control_fd_table.backend_count=3;
    entries[6]=(lpr_fd_entry_t){.active=1,.ofd_index=2,.ofd_generation=11};
    objects[2]=(lpr_ofd_t){.active=1,.generation=11,.backend={.index=2}};
    backends[2].ops_id=LPR_FD_OPS_SOCKET; backends[2].state=&mock_socket;
    instance=(void *)&storage;
    instance->count=2;
    instance->interests[1]=(lpr_epoll_interest_t){.target_fd=6,.target_ofd_index=2,
        .target_generation=11,.events=LPR_EPOLLIN,.data=5678};
    ready_events=LPR_EPOLLIN;delayed_fd=6;
    expect(take_event()==LPR_EPOLLIN && delayed_queries==0,
        "observed local readiness is not delayed by an unrelated broker query");
    expect(take_event()==LPR_EPOLLIN && delayed_queries==1,
        "deferred broker interest gets first service next call");
    delayed_fd=-1;
    lpr_epoll_snapshot_t fair[LPR_EPOLL_MAX_INTERESTS];
    lpr_linux_epoll_event_t returned;
    uint64_t first_data=0;
    for(unsigned i=0;i<4;i++) {
        assert(!lpr_epoll_snapshot(4,fair,&count,NULL));
        assert(lpr_epoll_scan(4,fair,count,&returned,1,1,1,0)==1);
        if(!i)first_data=returned.data;
        expect(returned.data==(i%2? (first_data==1234?5678:1234):first_data),
            "maxevents=1 rotates continuously ready interests");
    }
    instance->scan_cursor=0;
    backends[2].ops_id=LPR_FD_OPS_UNIX;backends[2].state=&mock_unix;
    instance->interests[0].events=LPR_EPOLLIN | LPR_EPOLLET;
    instance->interests[0].state=0;
    sequence_error_fd=6;
    lpr_linux_epoll_event_t batch[2];
    assert(!lpr_epoll_snapshot(4,fair,&count,NULL));
    expect(lpr_epoll_scan(4,fair,count,batch,2,1,1,0)==1 && batch[0].data==1234,
        "later query failure does not discard an already consumed ET event");
    sequence_error_fd=-1;unix_ready_override=0;instance->scan_cursor=0;
    backends[2].ops_id=LPR_FD_OPS_SOCKET; backends[2].state=&mock_socket;
    assert(!lpr_epoll_snapshot(4,fair,&count,NULL));
    expect(lpr_epoll_scan(4,fair,count,batch,2,1,1,0)==1 && batch[0].data==5678,
        "without local readiness the remaining broker sources are queried");

    union { max_align_t align; unsigned char bytes[LPR_EPOLL_INSTANCE_BYTES]; } child_storage;
    memset(&child_storage,0,sizeof child_storage);
    lpr_epoll_backend_t child_backend={.instance=(uintptr_t)&child_storage,
        .map_bytes=LPR_EPOLL_INSTANCE_BYTES};
    lpr_epoll_instance_t *child_instance=(void *)&child_storage;
    child_instance->magic=LPR_EPOLL_INSTANCE_MAGIC;child_instance->generation=1;
    child_instance->capacity=LPR_EPOLL_MAX_INTERESTS;child_instance->count=1;
    child_instance->interests[0]=(lpr_epoll_interest_t){.target_fd=3,.target_ofd_index=0,
        .target_generation=7,.events=LPR_EPOLLIN|LPR_EPOLLET,.data=1234};
    lpr_control_fd_table.ofd_count=4;lpr_control_fd_table.backend_count=4;
    entries[6]=(lpr_fd_entry_t){.active=1,.ofd_index=3,.ofd_generation=13};
    objects[3]=(lpr_ofd_t){.active=1,.generation=13,.backend={.index=3}};
    backends[3].ops_id=LPR_FD_OPS_EPOLL;backends[3].state=&child_backend;
    instance->interests[0].state=LPR_EPOLL_STATE_ONESHOT_DISABLED;
    instance->interests[1]=(lpr_epoll_interest_t){.target_fd=6,.target_ofd_index=3,
        .target_generation=13,.events=LPR_EPOLLIN,.data=5678};
    unix_ready_override=-1;instance->scan_cursor=0;
    assert(!lpr_epoll_snapshot(4,fair,&count,NULL));
    expect(lpr_epoll_scan(4,fair,count,batch,2,1,1,0)==1 && batch[0].data==5678 &&
        (child_instance->interests[0].state & LPR_EPOLL_STATE_PENDING_MASK),
        "nested epoll reports child readiness without consuming its ET event");
    expect(lpr_epoll_scan_fd(6,batch,1,1,1,0)==1 && batch[0].data==1234,
        "child ET event remains available after parent observation");
    setup(LPR_EPOLLIN | LPR_EPOLLET);
    backends[0].ops_id=LPR_FD_OPS_EVENT;backends[0].state=&mock_event;
    memset(event_counter_state,0,sizeof(event_counter_state));
    mock_event=(lpr_event_backend_t){.active=1,.subtype=LPR_EVENT_BACKEND_EVENTFD,
        .counter_map=(uint64_t)(uintptr_t)event_counter_state};
    event_counter_state[0]=1;lpr_event_counter_publish(&mock_event,1);
    expect(take_event()==LPR_EPOLLIN && take_event()==0,"eventfd initial edge consumed once");
    event_counter_state[0]++;lpr_event_counter_publish(&mock_event,1);
    expect(take_event()==LPR_EPOLLIN && take_event()==0,
        "eventfd write repeats edge without clearing readable counter");
    instance=(void *)&storage;instance->interests[0].events |= LPR_EPOLLONESHOT;
    lpr_event_counter_publish(&mock_event,1);
    expect(take_event()==LPR_EPOLLIN,"eventfd one-shot consumes progress");
    lpr_event_counter_publish(&mock_event,1);
    expect(take_event()==0,"eventfd progress cannot rearm disabled one-shot");
    instance->interests[0].events=LPR_EPOLLOUT | LPR_EPOLLET;
    instance->interests[0].state=0;
    expect(take_event()==LPR_EPOLLOUT && take_event()==0,"eventfd initial writable edge");
    event_counter_state[0]=0;lpr_event_counter_publish(&mock_event,4);
    expect(take_event()==LPR_EPOLLOUT && take_event()==0,"eventfd read repeats writable edge");
    if (failures) return 1;
    puts("LPR_EPOLL_IO_RACE_UNIT=OK");
    return 0;
}

#include "lpr_ipc_mock.h"
int64_t lpr_ipc_message(uint64_t nr, uint64_t fd, uint64_t send, uint64_t receive,
    uint64_t timeout, uint64_t flags, int *committed) {
    return test_lpr_ipc_noncall(nr, fd, send, receive, timeout, flags, committed);
}

int64_t lpr_pacha_syscall4(uint64_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d) {
    (void)nr;(void)a;(void)b;(void)c;(void)d;abort();
}
