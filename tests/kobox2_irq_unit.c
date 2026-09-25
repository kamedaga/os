/* SPDX-License-Identifier: MIT */
/* Deterministic syscall/collector interleavings; no native hardware or thread. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include "../userland/kobox2_adapter/device_irq.c"

#define DEVICE_FD 224
#define CONTROL_FD 100
#define ROUTE_FD 128

struct mock_route {
    unsigned int live, retired, masked;
    uint64_t count;
};

static struct mock_route native_routes[PH_IRQ_ROUTES];
static struct ph_irq port;
static struct ph_task collector_task;
static struct pacha_capsule_info device;
static uint64_t notification_mask, doorbells, control_value, cookie_sequence;
static unsigned int locked, calls, closes, creates, joins, fail_derive;
static unsigned int fail_route, fail_quiesce, fail_retire, fail_close;
static unsigned int poll_invalid, expect_fatal, wait_case;
static void *(*collector_entry)(void *);
static void *collector_argument;
static struct kobox_linux_irq_route replacement;
static jmp_buf fatal_jump;

_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result) {
    if (!expect_fatal) fprintf(stderr, "%s:%u result=%llu\n", file, line,
        (unsigned long long)result);
    assert(expect_fatal);
    longjmp(fatal_jump, 1);
}

int ph_notifications_save(uint64_t *previous) {
    *previous = notification_mask;
    notification_mask = PH_NOTIFICATION_MASK;
    return 0;
}

int ph_notifications_restore(uint64_t previous) {
    assert(!locked);
    notification_mask = previous;
    return 0;
}

void ph_lock(atomic_uint *lock) {
    assert(lock == &port.lock && !locked && notification_mask == PH_NOTIFICATION_MASK);
    locked = 1;
}

void ph_unlock(atomic_uint *lock) {
    assert(lock == &port.lock && locked && notification_mask == PH_NOTIFICATION_MASK);
    locked = 0;
}

static int notify_cpu(uint32_t cpu, enum kobox_linux_task_notification kind) {
    assert(!locked && cpu < 2 && kind == KOBOX_LINUX_TASK_DEVICE_IRQ);
    doorbells |= UINT64_C(1) << cpu;
    return 0;
}

const struct kobox_linux_task_host_operations ph_task_ops = {.cpu_notify = notify_cpu};

struct ph_task *ph_thread_create(void *(*entry)(void *), void *argument) {
    assert(!locked && !collector_entry);
    collector_entry = entry;
    collector_argument = argument;
    ++creates;
    return &collector_task;
}

int ph_thread_join(void *task) {
    assert(!locked && task == &collector_task && port.stopping);
    assert(!collector_entry(collector_argument));
    collector_entry = NULL;
    ++joins;
    return 0;
}

long pacha_syscall1(uint64_t nr, uint64_t fd) {
    ++calls;
    if (nr == PACHA_FD_SYSCALL_CLOSE && fd == CONTROL_FD) {
        assert(!locked && port.stopping);
        control_value = 0;
        return 0;
    }
    assert(locked && fd >= ROUTE_FD && fd < ROUTE_FD + PH_IRQ_ROUTES);
    struct mock_route *route = &native_routes[fd - ROUTE_FD];
    assert(route->live);
    if (nr == PACHA_FD_SYSCALL_CLOSE) {
        assert(route->retired); /* Never release a live physical lease by close. */
        if (fail_close) return PACHA_SYSCALL_ERR_MAP;
        *route = (struct mock_route){0};
        ++closes;
        return 0;
    }
    if (route->retired) return PACHA_SYSCALL_ERR_CLOSED;
    assert(nr == PACHA_CAPSULE_SYSCALL_IRQ_QUIESCE || nr == PACHA_CAPSULE_SYSCALL_IRQ_RETIRE);
    route->masked = 1;
    if (nr == PACHA_CAPSULE_SYSCALL_IRQ_QUIESCE && fail_quiesce)
        return PACHA_SYSCALL_ERR_NOT_READY;
    if (nr == PACHA_CAPSULE_SYSCALL_IRQ_RETIRE) {
        if (fail_retire) return PACHA_SYSCALL_ERR_NOT_READY;
        route->retired = 1;
    }
    return 0;
}

long pacha_syscall3(uint64_t nr, uint64_t fd, uint64_t address, uint64_t length) {
    ++calls;
    if (nr == PACHA_CAPSULE_SYSCALL_QUERY) {
        assert(!locked && fd == DEVICE_FD && length == 11);
        *(struct pacha_capsule_info *)(uintptr_t)address = device;
        return 11;
    }
    if (nr == PACHA_FD_SYSCALL_EVENTFD_CREATE) {
        assert(!locked && !fd && !length);
        return CONTROL_FD;
    }
    assert(locked);
    if (nr == PACHA_CAPSULE_SYSCALL_IRQ_ROUTE) {
        assert(fd >= ROUTE_FD && fd < ROUTE_FD + PH_IRQ_ROUTES && length == 3);
        assert(native_routes[fd - ROUTE_FD].masked);
        if (fail_route) return PACHA_SYSCALL_ERR_MAP;
        *(struct pacha_capsule_irq_route *)(uintptr_t)address =
            (struct pacha_capsule_irq_route){.message_address = 0xfee00000,
                .message_data = 64 + fd - ROUTE_FD, .hwirq = 64 + fd - ROUTE_FD};
        return 3;
    }
    assert(fd == CONTROL_FD && length == 8);
    if (nr == PACHA_FD_SYSCALL_WRITE) control_value += *(uint64_t *)(uintptr_t)address;
    else {
        assert(nr == PACHA_FD_SYSCALL_READ && control_value);
        *(uint64_t *)(uintptr_t)address = control_value;
        control_value = 0;
    }
    return 8;
}

long pacha_syscall5(uint64_t nr, uint64_t fd, uint64_t last, uint64_t address,
    uint64_t words, uint64_t flags) {
    ++calls;
    assert(locked && nr == PACHA_CAPSULE_SYSCALL_IRQ_POLL && words == 1 && !flags);
    assert(fd >= ROUTE_FD && fd < ROUTE_FD + PH_IRQ_ROUTES);
    struct mock_route *route = &native_routes[fd - ROUTE_FD];
    assert(route->live && !route->retired);
    if (poll_invalid) return PACHA_SYSCALL_ERR_INVALID;
    if (last == route->count) return PACHA_SYSCALL_ERR_NOT_READY;
    *(uint64_t *)(uintptr_t)address = route->count;
    return 1;
}

long pacha_syscall4(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    ++calls;
    if (nr == PACHA_CAPSULE_SYSCALL_DERIVE_IRQ) {
        assert(locked && a == DEVICE_FD && (b == PACHA_CAPSULE_IRQ_MSI ||
            b == PACHA_CAPSULE_IRQ_MSIX) && c < PH_IRQ_ROUTES && !d);
        if (fail_derive == c + 1) return PACHA_SYSCALL_ERR_ALLOC;
        assert(!native_routes[c].live);
        native_routes[c] = (struct mock_route){.live = 1};
        return ROUTE_FD + (long)c;
    }
    assert(!locked && nr == PACHA_FD_SYSCALL_WAIT_MANY && b == 2 &&
        c == PACHA_FD_WAIT_FOREVER && !d);
    struct pacha_pollfd *fds = (void *)(uintptr_t)a;
    assert(fds[0].fd == CONTROL_FD && fds[1].fd == ROUTE_FD);
    if (wait_case == 3) return PACHA_SYSCALL_ERR_INVALID; /* Unchanged epoch: fatal. */
    if (wait_case) {
        /* Recycle the numeric FD between snapshot and completion. No stale
         * snapshot count or HUP is allowed to become the new route's edge. */
        struct kobox_linux_irq_route old = port.slots[0].route;
        assert(!port.host.release(&port, old.hwirq, old.cookie));
        assert(!port.host.allocate(&port, KOBOX_IRQ_MSIX, 0, 1, 1, &replacement));
        assert(replacement.cookie != old.cookie);
        assert(!port.host.mask(&port, replacement.hwirq, replacement.cookie, 0));
        port.stopping = 1; /* Bound this deterministic collector iteration. */
        if (wait_case == 1) return PACHA_SYSCALL_ERR_INVALID;
        fds[1].revents = PACHA_FD_EVENT_HANGUP;
        return 1;
    }
    ++native_routes[0].count;
    fds[0].revents = PACHA_FD_EVENT_READABLE;
    fds[1].revents = PACHA_FD_EVENT_READABLE;
    port.stopping = 1;
    return 2; /* Also the native NOT_READY value: count/revents disambiguate. */
}

static void init_port(void) {
    struct ph_irq_config config = {.device_fd = DEVICE_FD, .native_device = 42,
        .generation = 7, .cpu_count = 2, .cookie_sequence = &cookie_sequence};
    device = (struct pacha_capsule_info){.kind = PACHA_CAPSULE_KIND_DEVICE, .device = 42,
        .rights = PACHA_FD_RIGHT_QUERY | PACHA_FD_RIGHT_DERIVE_IRQ |
            PACHA_FD_RIGHT_IRQ_WAIT | PACHA_FD_RIGHT_IRQ_ACK | PACHA_FD_RIGHT_CLOSE};
    assert(!ph_irq_init(&port, &config));
    assert(ph_irq_init(&port, &config) == -EINVAL);
    assert(port.host.size == sizeof(port.host));
}

int main(void) {
    struct kobox_linux_irq_route routes[4];
    struct kobox_linux_irq_event event;
    unsigned int active, before;
    init_port();
    const struct kobox_linux_irq_host *host = &port.host;
    before = calls;
    assert(host->allocate(&port, KOBOX_IRQ_INTX, 0, 1, 0, routes) == -EOPNOTSUPP);
    assert(host->allocate(&port, KOBOX_IRQ_MSI, 1, 2, 0, routes) == -EINVAL);
    assert(host->allocate(&port, KOBOX_IRQ_MSI, 0, 3, 0, routes) == -EINVAL);
    assert(host->allocate(&port, KOBOX_IRQ_MSIX, 15, 2, 0, routes) == -EINVAL);
    assert(host->allocate(&port, KOBOX_IRQ_MSIX, 0, 1, 2, routes) == -EINVAL);
    assert(calls == before);

    fail_derive = 3;
    memset(routes, 0xa5, sizeof(routes));
    assert(host->allocate(&port, KOBOX_IRQ_MSI, 0, 4, 0, routes) == -ENOMEM);
    assert(closes == 2 && cookie_sequence == 4 && routes[0].cookie == UINT64_C(0xa5a5a5a5a5a5a5a5));
    for (unsigned int i = 0; i < 4; ++i) assert(!port.slots[i].route.cookie && !native_routes[i].live);
    fail_derive = 0;
    fail_route = 1;
    assert(host->allocate(&port, KOBOX_IRQ_MSIX, 0, 1, 0, routes) == -EIO);
    fail_route = 0;
    fail_quiesce = 1;
    assert(host->allocate(&port, KOBOX_IRQ_MSIX, 0, 1, 0, routes) == -EBUSY);
    fail_quiesce = 0;
    assert(!host->allocate(&port, KOBOX_IRQ_MSI, 0, 4, 0, routes));
    for (unsigned int i = 0; i < 4; ++i) {
        assert(routes[i].data == 64 + i && routes[i].cookie > 4);
        assert(native_routes[i].masked && port.slots[i].masked);
    }
    assert(ph_irq_destroy(&port) == -EBUSY);
    assert(host->allocate(&port, KOBOX_IRQ_MSIX, 0, 1, 0, &replacement) == -EBUSY);
    for (unsigned int i = 1; i < 4; ++i) assert(!host->release(&port, i, routes[i].cookie));
    uint64_t cookie = routes[0].cookie;
    ++native_routes[0].count;
    assert(!host->active(&port, 0, cookie, &active) && !active && port.slots[0].pending);
    assert(!host->mask(&port, 0, cookie, 0) && doorbells == 1);
    doorbells = 0;
    assert(!host->affinity(&port, 0, cookie, 1) && doorbells == 2);
    assert(host->next(&port, 0, &event) == -EAGAIN);
    assert(!host->next(&port, 1, &event) && event.cookie == cookie);
    assert(host->release(&port, 0, cookie) == -EBUSY);
    assert(!host->mask(&port, 0, cookie, 1));
    assert(host->quiesce(&port, 0, cookie) == -EBUSY);
    assert(!host->retrigger(&port, 0, cookie));
    assert(!host->ack(&port, 0, cookie));
    fail_quiesce = 1;
    assert(host->quiesce(&port, 0, cookie) == -EBUSY && port.slots[0].pending);
    fail_quiesce = 0;
    ++native_routes[0].count;
    assert(!host->quiesce(&port, 0, cookie) && !port.slots[0].pending);
    assert(port.native[0].count == native_routes[0].count);
    poll_invalid = 1;
    assert(host->active(&port, 0, cookie, &active) == -EINVAL);
    poll_invalid = 0;
    fail_retire = 1;
    assert(host->release(&port, 0, cookie) == -EBUSY && native_routes[0].live);
    fail_retire = 0;
    fail_close = 1;
    assert(host->release(&port, 0, cookie) == -EIO && port.native[0].retired);
    assert(host->mask(&port, 0, cookie, 0) == -ENODEV);
    assert(host->retrigger(&port, 0, cookie) == -ENODEV);
    fail_close = 0;
    assert(!host->release(&port, 0, cookie));
    before = calls;
    assert(host->release(&port, 0, cookie) == -ESTALE);
    assert(host->quiesce(&port, 0, cookie) == -ESTALE);
    assert(host->retrigger(&port, 0, cookie) == -ESTALE);
    assert(calls == before);

    for (wait_case = 0; wait_case < 3; ++wait_case) {
        assert(!host->allocate(&port, KOBOX_IRQ_MSIX, 0, 1, 0, routes));
        assert(!host->mask(&port, 0, routes[0].cookie, 0));
        doorbells = 0;
        assert(!collector_entry(collector_argument));
        port.stopping = 0;
        if (!wait_case) {
            assert(doorbells == 1 && !host->next(&port, 0, &event));
            assert(event.cookie == routes[0].cookie);
            assert(!host->ack(&port, event.hwirq, event.cookie));
        } else {
            assert(!doorbells && host->next(&port, 1, &event) == -EAGAIN);
            assert(host->mask(&port, 0, routes[0].cookie, 0) == -ESTALE);
            assert(!host->retrigger(&port, 0, replacement.cookie));
            assert(doorbells == 2 && !host->next(&port, 1, &event));
            assert(event.cookie == replacement.cookie);
            assert(!host->ack(&port, 0, event.cookie));
        }
        assert(!host->release(&port, 0, port.slots[0].route.cookie));
    }
    assert(!host->allocate(&port, KOBOX_IRQ_MSIX, 0, 1, 0, routes));
    assert(ph_irq_revoke(&port, 6) == -ESTALE && port.admitted);
    assert(!ph_irq_revoke(&port, 7));
    assert(host->allocate(&port, KOBOX_IRQ_MSIX, 1, 1, 0, &replacement) == -ENODEV);
    assert(host->mask(&port, 0, routes[0].cookie, 0) == -ENODEV);
    assert(!host->release(&port, 0, routes[0].cookie));
    assert(!ph_irq_destroy(&port) && creates == joins);
    uint64_t old_sequence = cookie_sequence;
    init_port();
    assert(!host->allocate(&port, KOBOX_IRQ_MSIX, 0, 1, 0, routes));
    assert(routes[0].cookie > old_sequence);
    assert(!host->release(&port, 0, routes[0].cookie));
    /* Rollback may not return an ordinary allocation error while a physical
     * lease remains live. longjmp is only the test's fatal-path observer. */
    fail_route = fail_retire = expect_fatal = 1;
    before = closes;
    if (!setjmp(fatal_jump)) {
        host->allocate(&port, KOBOX_IRQ_MSIX, 0, 1, 0, routes);
        abort();
    }
    assert(native_routes[0].live && !native_routes[0].retired && closes == before);
    assert(port.slots[0].route.cookie && port.native[0].fd == ROUTE_FD);
    locked = 0; notification_mask = 0; /* Unwind the mock lock after the fatal observer. */
    fail_route = fail_retire = expect_fatal = 0;
    assert(!host->release(&port, 0, port.slots[0].route.cookie));
    assert(!host->allocate(&port, KOBOX_IRQ_MSIX, 0, 1, 0, routes));
    wait_case = 3;
    expect_fatal = 1;
    if (!setjmp(fatal_jump)) {
        collector_entry(collector_argument);
        abort();
    }
    assert(native_routes[0].live && port.slots[0].route.cookie == routes[0].cookie);
    locked = 0; notification_mask = 0;
    expect_fatal = 0;
    assert(!host->release(&port, 0, routes[0].cookie));
    cookie_sequence = UINT64_MAX;
    before = calls;
    assert(host->allocate(&port, KOBOX_IRQ_MSIX, 0, 1, 0, routes) == -EOVERFLOW);
    assert(calls == before && !ph_irq_destroy(&port));
    assert(!locked && !notification_mask && creates == joins);
    puts("kobox2 IRQ native-wrapper/collector unit: PASS");
    return 0;
}
