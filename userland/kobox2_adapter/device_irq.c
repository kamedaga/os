/* SPDX-License-Identifier: MIT */
#include "device_irq.h"
#include "host.h"
#include <pacha/capsule.h>
#include <errno.h>

static int irq_status(long result) {
    switch (result) {
    case 0: return 0;
    case PACHA_SYSCALL_ERR_INVALID: return -EINVAL;
    case PACHA_SYSCALL_ERR_NOT_READY: return -EBUSY;
    case PACHA_SYSCALL_ERR_ALLOC: return -ENOMEM;
    case PACHA_SYSCALL_ERR_MAP: return -EIO;
    case PACHA_SYSCALL_ERR_CLOSED: return -ENODEV;
    default: return -EPROTO;
    }
}

static uint64_t irq_lock(struct ph_irq *irq) {
    uint64_t previous;
    PH_OK(ph_notifications_save(&previous));
    ph_lock(&irq->lock);
    return previous;
}

static void irq_unlock(struct ph_irq *irq, uint64_t previous, bool notify) {
    uint64_t ready = notify ? ph_irq_queue_ready_cpus(&irq->queue) : 0;
    ph_unlock(&irq->lock);
    /* Never hold the port lock across a notification: its receiver needs this
     * same lock in next/ack. A stale CPU doorbell is revalidated by next. */
    for (uint32_t cpu = 0; cpu < irq->config.cpu_count; ++cpu)
        if (ready & (UINT64_C(1) << cpu))
            PH_OK(ph_task_ops.cpu_notify(cpu, KOBOX_LINUX_TASK_DEVICE_IRQ));
    PH_OK(ph_notifications_restore(previous));
}

static void wake_collector(struct ph_irq *irq) {
    uint64_t one = 1;
    PH_CHECK(irq->wait_epoch != UINT64_MAX);
    ++irq->wait_epoch;
    long bytes = pacha_syscall3(PACHA_FD_SYSCALL_WRITE, irq->control_fd,
        (uintptr_t)&one, sizeof(one));
    PH_CHECK(bytes == sizeof(one));
}

/* The kernel's positive INVALID value equals a successful one-word result.
 * Success must also change the output from the supplied last count. */
static int sample_route(struct ph_irq *irq, unsigned int index, bool discard) {
    struct ph_irq_native_route *native = &irq->native[index];
    uint64_t count = native->count;
    long result = pacha_syscall5(PACHA_CAPSULE_SYSCALL_IRQ_POLL, native->fd,
        native->count, (uintptr_t)&count, 1, 0);
    if (result == 1 && count != native->count) {
        native->count = count;
        if (!discard)
            PH_OK(ph_irq_queue_raise(&irq->queue, index, irq->slots[index].route.cookie));
        return 0;
    }
    if (result == PACHA_SYSCALL_ERR_NOT_READY && count == native->count) return 0;
    return result ? irq_status(result) : -EPROTO;
}

static int retire_route(struct ph_irq *irq, unsigned int index) {
    struct ph_irq_native_route *native = &irq->native[index];
    if (!native->retired) {
        int result = irq_status(pacha_syscall1(PACHA_CAPSULE_SYSCALL_IRQ_RETIRE, native->fd));
        if (result) return result;
        native->retired = 1;
        /* An old WAIT_MANY snapshot may still name this numeric FD. Wake it
         * before close/reuse, and never consume snapshot FDs outside the lock. */
        wake_collector(irq);
    }
    int result = irq_status(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, native->fd));
    if (result) return result;
    *native = (struct ph_irq_native_route){0};
    return 0;
}

static int allocate_routes(void *context, enum kobox_linux_irq_mode mode,
    uint32_t index, uint32_t count, uint32_t cpu, struct kobox_linux_irq_route *routes) {
    struct ph_irq *irq = context;
    unsigned int kind;
    if (mode == KOBOX_IRQ_INTX) return -EOPNOTSUPP;
    if (mode == KOBOX_IRQ_MSI) kind = PACHA_CAPSULE_IRQ_MSI;
    else if (mode == KOBOX_IRQ_MSIX) kind = PACHA_CAPSULE_IRQ_MSIX;
    else return -EINVAL;
    if (!routes || !count || index >= PH_IRQ_ROUTES || count > PH_IRQ_ROUTES - index ||
        (mode == KOBOX_IRQ_MSI && ((count & (count - 1)) || index % count))) return -EINVAL;

    uint64_t previous = irq_lock(irq);
    int result = irq->admitted ? ph_irq_queue_reserve(&irq->queue, index, count, cpu) : -ENODEV;
    if (result) goto out;
    for (uint32_t i = 0; i < count; ++i) {
        struct ph_irq_slot *slot = &irq->slots[index + i];
        struct pacha_capsule_irq_route message = {0};
        long fd = pacha_syscall4(PACHA_CAPSULE_SYSCALL_DERIVE_IRQ,
            irq->config.device_fd, kind, index + i, 0);
        if (fd < 16 || fd >= PACHA_FD_TABLE_LIMIT) {
            result = fd ? irq_status(fd) : -EPROTO;
            goto rollback;
        }
        irq->native[index + i].fd = (int)fd;
        result = irq_status(pacha_syscall1(PACHA_CAPSULE_SYSCALL_IRQ_QUIESCE, fd));
        if (result) goto rollback;
        result = sample_route(irq, index + i, true);
        if (result) goto rollback;
        long words = pacha_syscall3(PACHA_CAPSULE_SYSCALL_IRQ_ROUTE,
            fd, (uintptr_t)&message, 3);
        if (words != 3 || !message.message_address || message.message_data > UINT32_MAX) {
            result = words == 3 || !words ? -EPROTO : irq_status(words);
            goto rollback;
        }
        slot->route.address = message.message_address;
        slot->route.data = (uint32_t)message.message_data;
        if (mode == KOBOX_IRQ_MSI && (irq->slots[index].route.data % count ||
            slot->route.address != irq->slots[index].route.address ||
            (uint64_t)slot->route.data != (uint64_t)irq->slots[index].route.data + i)) {
            result = -EPROTO;
            goto rollback;
        }
    }
    for (uint32_t i = 0; i < count; ++i) routes[i] = irq->slots[index + i].route;
    wake_collector(irq);
    goto out;
rollback:
    /* Returning partial ownership would let Linux reuse a still-live source.
     * Failed rollback is terminal, not an ordinary allocation failure. */
    for (uint32_t i = 0; i < count; ++i) {
        if (irq->native[index + i].fd) PH_OK(retire_route(irq, index + i));
        PH_OK(ph_irq_queue_release(&irq->queue, index + i, irq->slots[index + i].route.cookie));
    }
out:
    irq_unlock(irq, previous, false);
    return result;
}

static int release_route(void *context, uint64_t hwirq, uint64_t cookie) {
    struct ph_irq *irq = context;
    uint64_t previous = irq_lock(irq);
    struct ph_irq_slot *slot = ph_irq_queue_lookup(&irq->queue, hwirq, cookie);
    int result = -ESTALE;
    if (slot) {
        result = -EBUSY;
        if (!slot->servicing) {
            slot->masked = 1;
            result = retire_route(irq, (unsigned int)hwirq);
            if (!result) result = ph_irq_queue_release(&irq->queue, hwirq, cookie);
        }
    }
    irq_unlock(irq, previous, false);
    return result;
}

static int mask_route(void *context, uint64_t hwirq, uint64_t cookie, unsigned int masked) {
    struct ph_irq *irq = context;
    uint64_t previous = irq_lock(irq);
    int result;
    if (!ph_irq_queue_lookup(&irq->queue, hwirq, cookie)) result = -ESTALE;
    else if ((!irq->admitted || irq->native[hwirq].retired) && !masked) result = -ENODEV;
    else result = ph_irq_queue_mask(&irq->queue, hwirq, cookie, masked);
    irq_unlock(irq, previous, !result);
    return result;
}

static int ack_route(void *context, uint64_t hwirq, uint64_t cookie) {
    struct ph_irq *irq = context;
    uint64_t previous = irq_lock(irq);
    int result = ph_irq_queue_ack(&irq->queue, hwirq, cookie);
    irq_unlock(irq, previous, !result);
    return result;
}

static int quiesce_route(void *context, uint64_t hwirq, uint64_t cookie) {
    struct ph_irq *irq = context;
    uint64_t previous = irq_lock(irq);
    struct ph_irq_slot *slot = ph_irq_queue_lookup(&irq->queue, hwirq, cookie);
    int result = -ESTALE;
    if (slot) {
        result = -EBUSY;
        if (slot->masked && !slot->servicing) {
            result = irq_status(pacha_syscall1(PACHA_CAPSULE_SYSCALL_IRQ_QUIESCE,
                irq->native[hwirq].fd));
            if (!result) result = sample_route(irq, (unsigned int)hwirq, true);
            if (!result) result = ph_irq_queue_quiesce(&irq->queue, hwirq, cookie);
        }
    }
    irq_unlock(irq, previous, false);
    return result;
}

static int active_route(void *context, uint64_t hwirq, uint64_t cookie, unsigned int *active) {
    struct ph_irq *irq = context;
    uint64_t previous = irq_lock(irq);
    int result = ph_irq_queue_active(&irq->queue, hwirq, cookie, active);
    /* An accepted native edge may precede the collector's next wake. Include
     * it in the pending-state query, not only already queued doorbells. */
    if (!result && !irq->native[hwirq].retired) {
        result = sample_route(irq, (unsigned int)hwirq, false);
        if (!result) result = ph_irq_queue_active(&irq->queue, hwirq, cookie, active);
    }
    irq_unlock(irq, previous, !result);
    return result;
}

static int affinity_route(void *context, uint64_t hwirq, uint64_t cookie, uint32_t cpu) {
    struct ph_irq *irq = context;
    uint64_t previous = irq_lock(irq);
    int result = ph_irq_queue_affinity(&irq->queue, hwirq, cookie, cpu);
    irq_unlock(irq, previous, !result);
    return result;
}

static int retrigger_route(void *context, uint64_t hwirq, uint64_t cookie) {
    struct ph_irq *irq = context;
    uint64_t previous = irq_lock(irq);
    struct ph_irq_slot *slot = ph_irq_queue_lookup(&irq->queue, hwirq, cookie);
    int result = -ESTALE;
    if (slot) result = irq->admitted && !irq->native[hwirq].retired ?
        ph_irq_queue_raise(&irq->queue, hwirq, cookie) : -ENODEV;
    irq_unlock(irq, previous, !result);
    return result;
}

static int next_event(void *context, uint32_t cpu, struct kobox_linux_irq_event *event) {
    struct ph_irq *irq = context;
    uint64_t previous = irq_lock(irq);
    int result = ph_irq_queue_next(&irq->queue, cpu, event);
    irq_unlock(irq, previous, false);
    return result;
}

static void *collect_irqs(void *context) {
    struct ph_irq *irq = context;
    for (;;) {
        struct pacha_pollfd fds[1 + PH_IRQ_ROUTES] = {0};
        size_t count = 1;
        uint64_t previous = irq_lock(irq);
        if (irq->stopping) {
            irq_unlock(irq, previous, false);
            return NULL;
        }
        fds[0] = (struct pacha_pollfd){.fd = irq->control_fd, .events = PACHA_FD_EVENT_READABLE};
        for (unsigned int i = 0; i < PH_IRQ_ROUTES; ++i)
            if (irq->native[i].fd && !irq->native[i].retired)
                fds[count++] = (struct pacha_pollfd){
                    .fd = irq->native[i].fd, .events = PACHA_FD_EVENT_READABLE,
                };
        uint64_t epoch = irq->wait_epoch;
        irq_unlock(irq, previous, false);

        long result = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
            (uintptr_t)fds, count, PACHA_FD_WAIT_FOREVER, 0);
        previous = irq_lock(irq);
        size_t ready = 0;
        for (size_t i = 0; i < count; ++i) ready += fds[i].revents != 0;
        /* Close/reallocate may invalidate the snapshot or attach its numeric
         * FD to a replacement. It is only a wake hint, never route identity.
         * With an unchanged epoch, an unexpected native failure is fatal. */
        PH_CHECK(epoch != irq->wait_epoch || (result > 0 && (size_t)result == ready) ||
            (result == PACHA_SYSCALL_ERR_NOT_READY && !ready));
        if (fds[0].revents & PACHA_FD_EVENT_READABLE) {
            uint64_t value;
            PH_CHECK(pacha_syscall3(PACHA_FD_SYSCALL_READ, irq->control_fd,
                (uintptr_t)&value, sizeof(value)) == sizeof(value));
        }
        for (unsigned int i = 0; i < PH_IRQ_ROUTES; ++i)
            if (irq->native[i].fd && !irq->native[i].retired)
                PH_OK(sample_route(irq, i, false));
        irq_unlock(irq, previous, true);
    }
}

int ph_irq_init(struct ph_irq *irq, const struct ph_irq_config *config) {
    const uint64_t required = PACHA_FD_RIGHT_QUERY | PACHA_FD_RIGHT_DERIVE_IRQ |
        PACHA_FD_RIGHT_IRQ_WAIT | PACHA_FD_RIGHT_IRQ_ACK | PACHA_FD_RIGHT_CLOSE;
    const uint64_t event_rights = PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WRITE |
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE;
    struct pacha_capsule_info info = {0};
    if (!irq || !config || irq->config.generation || config->device_fd < 0 ||
        config->device_fd >= PACHA_FD_TABLE_LIMIT || !config->native_device ||
        !config->generation || !config->cpu_count || config->cpu_count > PH_CPU_COUNT ||
        !config->cookie_sequence) return -EINVAL;
    long words = pacha_syscall3(PACHA_CAPSULE_SYSCALL_QUERY, config->device_fd,
        (uintptr_t)&info, 11);
    if (words != 11) return words ? irq_status(words) : -EPROTO;
    if (info.kind != PACHA_CAPSULE_KIND_DEVICE || info.device != config->native_device)
        return -ENODEV;
    if ((info.rights & required) != required) return -EACCES;
    long control = pacha_syscall3(PACHA_FD_SYSCALL_EVENTFD_CREATE, 0, event_rights, 0);
    if (control < 16 || control >= PACHA_FD_TABLE_LIMIT)
        return control ? irq_status(control) : -EPROTO;
    irq->config = *config;
    irq->control_fd = (int)control;
    PH_OK(ph_irq_queue_init(&irq->queue, irq->slots, PH_IRQ_ROUTES,
        config->cpu_count, config->cookie_sequence));
    irq->host = (struct kobox_linux_irq_host){
        .size = sizeof(irq->host), .context = irq, .allocate = allocate_routes,
        .release = release_route, .mask = mask_route, .ack = ack_route,
        .quiesce = quiesce_route, .active = active_route, .affinity = affinity_route,
        .retrigger = retrigger_route, .next = next_event,
    };
    irq->admitted = 1;
    irq->collector = ph_thread_create(collect_irqs, irq);
    return 0;
}

int ph_irq_revoke(struct ph_irq *irq, uint64_t generation) {
    if (!irq || !irq->config.generation || irq->config.generation != generation) return -ESTALE;
    uint64_t previous = irq_lock(irq);
    irq->admitted = 0;
    for (unsigned int i = 0; i < PH_IRQ_ROUTES; ++i) irq->slots[i].masked = 1;
    irq_unlock(irq, previous, false);
    return 0;
}

int ph_irq_destroy(struct ph_irq *irq) {
    if (!irq || !irq->config.generation) return -EINVAL;
    uint64_t previous = irq_lock(irq);
    for (unsigned int i = 0; i < PH_IRQ_ROUTES; ++i) {
        if (irq->slots[i].route.cookie) {
            irq_unlock(irq, previous, false);
            return -EBUSY;
        }
    }
    irq->admitted = 0;
    irq->stopping = 1;
    wake_collector(irq);
    irq_unlock(irq, previous, false);
    PH_OK(ph_thread_join(irq->collector));
    PH_OK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, irq->control_fd));
    *irq = (struct ph_irq){0};
    return 0;
}
