#define _GNU_SOURCE
#include "unixd/transport.h"
#include "unixd/notify_client.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

struct channels { int fd[3]; int cached; struct unix_notify_cache cache; };

static int channel_resolve(void *raw, uint64_t socket, uint32_t id, int *fd)
{
    (void)raw;
    assert(socket == 1 && (id == 1 || id == 2));
    *fd = (int)id + 16;
    return 0;
}

static int channel_send(void *raw, int fd, uint64_t token)
{
    struct channels *channels = raw;
    assert(fd == (int)(uint32_t)token + 16);
    return eventfd_write(channels->fd[fd - 16], 1) == 0 ? 0 : -errno;
}

static int channel_relay(void *raw, uint64_t socket, uint32_t id)
{
    (void)raw; (void)socket; (void)id;
    assert(0); return -EIO;
}

static void channel_close(void *raw, int fd) { (void)raw; (void)fd; }

static void signal_channel(void *context, uint64_t token)
{
    struct channels *channels = context;
    unsigned id = (uint32_t)token;
    assert(id == 1 || id == 2);
    assert(eventfd_write(channels->fd[id], 1) == 0 || errno == EAGAIN);
}

static void notify(const struct unix_tx *tx, const struct unix_rx *rx,
    struct channels *channels)
{
    if (channels->cached) {
        const struct unix_notify_platform platform = { .context = channels,
            .resolve = channel_resolve, .send = channel_send,
            .relay = channel_relay, .close = channel_close };
        assert(unix_notify_direction(&channels->cache, &platform, 1, tx, rx) == 0);
        return;
    }
    unix_notify_scan(&tx->waiters, signal_channel, channels);
    unix_notify_scan(&rx->waiters, signal_channel, channels);
}

static void drain(int fd)
{
    eventfd_t value;
    while (eventfd_read(fd, &value) == 0) {}
    assert(errno == EAGAIN);
}

static int pending(int fd)
{
    struct pollfd item = { .fd = fd, .events = POLLIN };
    return poll(&item, 1, 0) == 1;
}

struct cache_fixture {
    unsigned resolves, sends, relays, closes;
    int allocation_error, send_error, relay_error;
};

static int cache_resolve(void *raw, uint64_t socket, uint32_t id, int *fd)
{
    struct cache_fixture *fixture = raw;
    assert(socket == 71);
    fixture->resolves++;
    if (id > UNIX_WAIT_SLOTS) return -EPERM;
    if (fixture->allocation_error) return fixture->allocation_error;
    *fd = (int)id + 16;
    return 0;
}

static int cache_send(void *raw, int fd, uint64_t token)
{
    struct cache_fixture *fixture = raw;
    assert(fd == (int)(uint32_t)token + 16);
    fixture->sends++;
    return fixture->send_error;
}

static int cache_relay(void *raw, uint64_t socket, uint32_t id)
{
    struct cache_fixture *fixture = raw;
    assert(socket == 71 && id <= UNIX_WAIT_SLOTS);
    fixture->relays++;
    return fixture->relay_error;
}

static void cache_close(void *raw, int fd)
{
    assert(fd >= 16);
    ((struct cache_fixture *)raw)->closes++;
}

static void cached_notifications(struct unix_tx *tx, struct unix_rx *rx)
{
    assert(unix_transport_init(tx, rx, 9, UNIX_TRANSPORT_STREAM) == 0);
    struct unix_notify_cache cache = {0};
    struct cache_fixture fixture = {0};
    const struct unix_notify_platform platform = { .context = &fixture,
        .resolve = cache_resolve, .send = cache_send, .relay = cache_relay, .close = cache_close };
    struct unix_wait_registration a, b;
    assert(unix_wait_arm(&tx->waiters, &tx->changes, &rx->changes, 1, 1, &a) == 0);
    assert(unix_wait_arm(&rx->waiters, &tx->changes, &rx->changes, 2, 1, &b) == 0);
    assert(unix_notify_direction(&cache, &platform, 71, tx, rx) == 0);
    assert(fixture.resolves == 2 && fixture.sends == 2 && !fixture.relays);
    assert(unix_notify_direction(&cache, &platform, 71, tx, rx) == 0);
    assert(fixture.resolves == 2 && fixture.sends == 2 && !fixture.relays); /* same arm: no SEND */
    struct unix_wait_registration spoof;
    assert(unix_wait_arm(&rx->waiters, &tx->changes, &rx->changes, 1, 2, &spoof) == 0);
    assert(unix_notify_direction(&cache, &platform, 71, tx, rx) == 0);
    assert(fixture.sends == 3); /* Peer bank advertises a future local arm. */
    unix_wait_disarm(&rx->waiters, &spoof);
    unix_wait_disarm(&tx->waiters, &a);
    unix_wait_disarm(&rx->waiters, &b);
    assert(unix_wait_arm(&tx->waiters, &tx->changes, &rx->changes, 1, 2, &a) == 0);
    assert(unix_wait_arm(&rx->waiters, &tx->changes, &rx->changes, 2, 2, &b) == 0);
    assert(unix_notify_direction(&cache, &platform, 71, tx, rx) == 0);
    assert(fixture.resolves == 2 && fixture.sends == 5); /* real new arm must wake despite spoof */
    assert(unix_notify_direction(&cache, &platform, 72, tx, rx) == 0);
    assert(fixture.sends == 7 && fixture.resolves == 2); /* Another socket is not deduplicated. */
    assert(unix_notify_direction(&cache, &platform, 71, tx, rx) == 0);
    assert(fixture.sends == 9 && fixture.resolves == 2);
    unix_wait_disarm(&tx->waiters, &a);
    unix_wait_disarm(&rx->waiters, &b);
    assert(unix_wait_arm(&tx->waiters, &tx->changes, &rx->changes, 1, 3, &a) == 0);
    assert(unix_wait_arm(&rx->waiters, &tx->changes, &rx->changes, 2, 3, &b) == 0);
    fixture.send_error = -EAGAIN;
    assert(unix_notify_direction(&cache, &platform, 71, tx, rx) == 0);
    assert(fixture.resolves == 2 && !fixture.relays && !fixture.closes);
    fixture.send_error = -EBADF;
    assert(unix_notify_direction(&cache, &platform, 71, tx, rx) == 0);
    assert(fixture.relays == 2 && fixture.closes == 2);
    fixture.send_error = 0;
    fixture.allocation_error = -EMFILE;
    assert(unix_notify_direction(&cache, &platform, 71, tx, rx) == 0);
    assert(fixture.relays == 4 && fixture.resolves == 4);
    fixture.relay_error = -EIO;
    assert(unix_notify_direction(&cache, &platform, 71, tx, rx) == -EIO);
    assert(fixture.relays == 6);
    fixture.relay_error = fixture.allocation_error = 0;
    unix_wait_disarm(&tx->waiters, &a);
    unix_wait_disarm(&rx->waiters, &b);
    assert(unix_wait_arm(&rx->waiters, &tx->changes, &rx->changes, 999, 1, &b) == 0);
    assert(unix_notify_direction(&cache, &platform, 71, tx, rx) == 0);
    assert(fixture.relays == 6); /* forged ID cannot select an arbitrary native FD */
    unix_wait_disarm(&rx->waiters, &b);
    for (unsigned i = 0; i < UNIX_WAIT_SLOTS; i++)
        assert(unix_wait_arm(&rx->waiters, &tx->changes, &rx->changes, i + 1, 1, &b) == 0);
    const unsigned before = fixture.closes;
    assert(unix_notify_direction(&cache, &platform, 71, tx, rx) == 0);
    assert(fixture.closes - before == UNIX_WAIT_SLOTS - UNIX_NOTIFY_CACHE_ENTRIES);
    unix_notify_cache_clear(&cache, &platform);
    assert(fixture.closes - before == UNIX_WAIT_SLOTS);
    for (unsigned i = 0; i < UNIX_WAIT_SLOTS; i++) unix_wait_remove(&rx->waiters, i + 1);
}

static void registrations(struct channels *channels)
{
    struct unix_wait_bank bank = {0};
    uint64_t first = 0, second = 0;
    struct unix_wait_registration a, b, old;
    assert(unix_wait_arm(&bank, &first, &second, 1, 1, &a) == 0);
    assert(unix_wait_arm(&bank, &first, &second, 2, 1, &b) == 0);
    assert(!unix_wait_changed(&first, &second, &a));
    __atomic_fetch_add(&first, 1, __ATOMIC_SEQ_CST);
    unix_notify_scan(&bank, signal_channel, channels);
    assert(unix_wait_changed(&first, &second, &a));
    assert(pending(channels->fd[1]) && pending(channels->fd[2]));
    drain(channels->fd[1]);
    assert(!pending(channels->fd[1]) && pending(channels->fd[2]));
    drain(channels->fd[2]);
    old = a;
    unix_wait_disarm(&bank, &a);
    assert(unix_wait_arm(&bank, &first, &second, 1, 2, &a) == 0);
    assert(a.slot == old.slot);
    unix_wait_disarm(&bank, &old);
    assert(bank.armed[a.slot] == a.token); /* stale disarm cannot clear reuse */
    unix_wait_remove(&bank, 1);
    unix_notify_scan(&bank, signal_channel, channels);
    assert(!pending(channels->fd[1]) && pending(channels->fd[2]));
    drain(channels->fd[2]);
    unix_wait_disarm(&bank, &b);
    for (unsigned i = 0; i < UNIX_WAIT_SLOTS; i++)
        assert(unix_wait_arm(&bank, &first, &second, i + 1, 1, &a) == 0);
    assert(unix_wait_arm(&bank, &first, &second, 99, 1, &a) == -EAGAIN);
    assert(unix_wait_changed(&first, &second, &a));
    for (unsigned i = 0; i < UNIX_WAIT_SLOTS; i++) unix_wait_remove(&bank, i + 1);
    assert(unix_wait_arm(&bank, &first, &second, 1, 3, &a) == 0);
    assert(eventfd_write(channels->fd[1], UINT64_MAX - 1) == 0);
    unix_notify_scan(&bank, signal_channel, channels); /* already full = awake */
    assert(pending(channels->fd[1]));
    drain(channels->fd[1]);
    unix_wait_disarm(&bank, &a);
}

static void boundaries(struct unix_tx *tx, struct unix_rx *rx, struct channels *channels)
{
    for (unsigned arm_first = 0; arm_first < 2; arm_first++) {
        assert(unix_transport_init(tx, rx, 9, UNIX_TRANSPORT_STREAM) == 0);
        struct unix_wait_registration registration;
        if (arm_first)
            assert(unix_wait_arm(&rx->waiters, &tx->changes, &rx->changes,
                1, arm_first + 1, &registration) == 0);
        struct unix_write write;
        assert(unix_transport_write_begin(tx, rx, 9, 4, 1, 0, 0, &write) == 0);
        *(char *)write.spans[0].base = 'x';
        assert(unix_transport_write_commit(tx, &write) == 0);
        if (!arm_first) {
            notify(tx, rx, channels); /* no registered waiter, then arm */
            assert(unix_wait_arm(&rx->waiters, &tx->changes, &rx->changes,
                1, 1, &registration) == 0);
        }
        int readable, eof;
        assert(unix_transport_readable(tx, rx, 9, &readable, &eof) == 0 && readable);
        if (arm_first) {
            assert(unix_wait_changed(&tx->changes, &rx->changes, &registration));
            /* Commit finished but notification was skipped by the dead owner.
             * The broker must notify even when recovery finds no held lock. */
            assert(unix_transport_recover_tx(tx, 4) == 0);
            notify(tx, rx, channels);
            assert(pending(channels->fd[1]));
            drain(channels->fd[1]);
        }
        unix_wait_disarm(&rx->waiters, &registration);
    }
}

static int ready(const struct unix_tx *tx, const struct unix_rx *rx, int reading)
{
    int result, eof;
    const int status = reading ? unix_transport_readable(tx, rx, 9, &result, &eof) :
        unix_transport_writable(tx, rx, 9, &result);
    assert(status == 0 || status == -EBUSY);
    return status == -EBUSY || result;
}

static void await_change(const struct unix_tx *tx, const struct unix_rx *rx,
    struct unix_wait_bank *bank, struct channels *channels, int reading, uint32_t *attempt)
{
    const unsigned id = reading ? 1 : 2;
    drain(channels->fd[id]);
    if (ready(tx, rx, reading)) return;
    struct unix_wait_registration registration;
    assert(unix_wait_arm(bank, &tx->changes, &rx->changes, id, ++*attempt, &registration) == 0);
    if (!ready(tx, rx, reading) &&
        !unix_wait_changed(&tx->changes, &rx->changes, &registration)) {
        struct pollfd item = { .fd = channels->fd[id], .events = POLLIN };
        assert(poll(&item, 1, 5000) == 1); /* Fail on a lost wake, never hang. */
    }
    unix_wait_disarm(bank, &registration);
}

static void process_exchange(struct unix_tx *tx, struct unix_rx *rx, struct channels *channels)
{
    assert(unix_transport_init(tx, rx, 9, UNIX_TRANSPORT_STREAM) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        assert(mprotect(rx, sizeof(*rx), PROT_READ) == 0);
        uint32_t attempt = 0;
        for (uint32_t i = 0; i < 50000; i++) {
            struct unix_write write;
            int status;
            while ((status = unix_transport_write_begin(tx, rx, 9, 4,
                sizeof(i), 0, 0, &write)) != 0) {
                assert(status == -EAGAIN || status == -EBUSY);
                notify(tx, rx, channels);
                await_change(tx, rx, &tx->waiters, channels, 0, &attempt);
            }
            memcpy(write.spans[0].base, &i, write.spans[0].length);
            memcpy(write.spans[1].base, (const char *)&i + write.spans[0].length,
                write.spans[1].length);
            assert(unix_transport_write_commit(tx, &write) == 0);
            notify(tx, rx, channels);
        }
        _exit(0);
    }
    assert(mprotect(tx, sizeof(*tx), PROT_READ) == 0);
    uint32_t attempt = 0;
    for (uint32_t i = 0; i < 50000; i++) {
        struct unix_read read;
        int status;
        while ((status = unix_transport_read_begin(tx, rx, 9, 5, sizeof(i), &read)) != 0) {
            assert(status == -EAGAIN || status == -EBUSY);
            notify(tx, rx, channels);
            await_change(tx, rx, &rx->waiters, channels, 1, &attempt);
        }
        uint32_t received;
        memcpy(&received, read.spans[0].base, read.spans[0].length);
        memcpy((char *)&received + read.spans[0].length, read.spans[1].base,
            read.spans[1].length);
        assert(read.length == sizeof(received) && received == i);
        assert(unix_transport_read_commit(rx, &read) == 0);
        notify(tx, rx, channels);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
    struct channels channels = { .fd = {-1,
        eventfd(0, EFD_NONBLOCK), eventfd(0, EFD_NONBLOCK)} };
    assert(channels.fd[1] >= 0 && channels.fd[2] >= 0);
    registrations(&channels);
    struct unix_tx *tx = mmap(NULL, sizeof(*tx), PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    struct unix_rx *rx = mmap(NULL, sizeof(*rx), PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(tx != MAP_FAILED && rx != MAP_FAILED);
    cached_notifications(tx, rx);
    boundaries(tx, rx, &channels);
    channels.cached = 1;
    process_exchange(tx, rx, &channels);
    assert(munmap(tx, sizeof(*tx)) == 0 && munmap(rx, sizeof(*rx)) == 0);
    close(channels.fd[1]); close(channels.fd[2]);
    puts("unix notify: cached caps/relay, independent waiters, stale disarm, full queue, arm/commit/death boundaries, RO peers, 50000 messages passed");
}
