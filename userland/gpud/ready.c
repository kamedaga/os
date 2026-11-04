/* SPDX-License-Identifier: MIT */
#include "ready.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
enum { SLOTS = 255 };
struct gpud_ready_source {
    struct gpud_ready *owner;
    struct gpud_ready_worker *worker;
    unsigned slot, kind;
    pacha_fd_t fd;
    uint64_t events, generation;
    void *context;
    struct gpud_ready_source *next, *prev;
    unsigned queued;
    uint64_t pending;
};
struct gpud_ready_worker {
    struct gpud_ready_worker *next;
    struct gpud_ready *owner;
    pacha_fd_t control, ack;
    void *thread;
    /* Owner serializes mailbox edits and holds a source through the ACK. The
     * worker installs/excludes its FD under the kernel arm/recheck before ACK. */
    _Atomic uint64_t revision, completed;
    _Atomic unsigned finished;
    unsigned command, slot;
    struct gpud_ready_source *input;
    struct gpud_ready_source *sources[SLOTS];
    unsigned char armed[SLOTS];
    unsigned occupied;
    unsigned char allocated[SLOTS];
};
struct gpud_ready {
    pacha_fd_t notify;
    struct gpud_ready_worker *workers;
    atomic_flag lock;
    struct gpud_ready_source *head, *tail;
    _Atomic int error;
    uint64_t sequence;
    /* Completed notification writes, not queue occupancy. A publisher can be
     * between queue insertion and signal; its eventual signal must remain a
     * wake for the owner. Only the owner updates drained_notifications. */
    _Atomic uint64_t notifications;
    _Atomic unsigned notifying;
    uint64_t drained_notifications;
};
static void lock(struct gpud_ready *r) {
    while (atomic_flag_test_and_set_explicit(&r->lock, memory_order_acquire))
        __asm__ volatile("pause");
}
static void unlock(struct gpud_ready *r) {
    atomic_flag_clear_explicit(&r->lock, memory_order_release);
}
static void fail(struct gpud_ready *r) {
    atomic_store_explicit(&r->error, -EIO, memory_order_release);
    (void)gpud_ready_os_signal(r->notify);
}
static void publish(struct gpud_ready_source *s, uint64_t events) {
    struct gpud_ready *r = s->owner;
    atomic_fetch_add_explicit(&r->notifying, 1, memory_order_acq_rel);
    lock(r);
    s->pending |= events;
    if (!s->queued) {
        s->queued = 1;
        s->next = NULL;
        s->prev = r->tail;
        if (r->tail)
            r->tail->next = s;
        else
            r->head = s;
        r->tail = s;
    }
    unlock(r);
    if (gpud_ready_os_signal(r->notify))
        fail(r);
    else
        atomic_fetch_add_explicit(&r->notifications, 1, memory_order_release);
    atomic_fetch_sub_explicit(&r->notifying, 1, memory_order_release);
}
static void finished(struct gpud_ready_worker *w) {
    atomic_store_explicit(&w->finished, 1, memory_order_release);
    (void)gpud_ready_os_signal(w->ack);
}
void gpud_ready_worker_run(struct gpud_ready_worker *w) {
    struct pacha_fd_wait_retained retained = {0};
    struct pacha_pollfd fds[SLOTS + 1];
    unsigned indices[SLOTS + 1];
    uint64_t seen = 0;
    for (;;) {
        uint64_t revision = atomic_load_explicit(&w->revision, memory_order_acquire);
        int changed = revision != seen;
        if (changed) {
            if (w->command == 3) {
                /* Clear subscriptions before the owner may close source FDs or reclaim
                 * this worker's storage. join additionally waits for terminal thread. */
                struct pacha_pollfd ctrl = {.fd = w->control, .events = PACHA_FD_EVENT_READABLE};
                struct pacha_fd_wait_retained empty = {0};
                (void)gpud_ready_os_wait(&empty, &ctrl, 1, 0);
                atomic_store_explicit(&w->completed, revision, memory_order_release);
                (void)gpud_ready_os_signal(w->ack);
                finished(w);
                return;
            }
            unsigned i = w->slot;
            if (w->command == 1) {
                w->sources[i] = w->input;
                w->armed[i] = 1;
            } else if (w->command == 2) {
                w->sources[i] = NULL;
                w->armed[i] = 0;
            }
            seen = revision;
        }
        size_t count = 1;
        fds[0] = (struct pacha_pollfd){.fd = w->control, .events = PACHA_FD_EVENT_READABLE};
        for (unsigned i = 0; i < SLOTS; i++)
            if (w->sources[i] && w->armed[i]) {
                struct gpud_ready_source *s = w->sources[i];
                indices[count] = i;
                fds[count++] = (struct pacha_pollfd){.fd = s->fd, .events = s->events};
            }
        /* An update uses timeout=0 to install + recheck before ACK. A subsequent
         * retained wait observes a late arrival, even if it races the ACK. */
        int rc = gpud_ready_os_wait(&retained, fds, count, changed ? 0 : UINT64_MAX);
        if (rc < 0) {
            fail(w->owner);
            atomic_store_explicit(&w->completed, seen, memory_order_release);
            (void)gpud_ready_os_signal(w->ack);
            finished(w);
            return;
        }
        if (fds[0].revents && gpud_ready_os_drain(w->control)) {
            fail(w->owner);
            finished(w);
            return;
        }
        for (size_t j = 1; j < count; j++)
            if (fds[j].revents) {
                unsigned i = indices[j];
                w->armed[i] = 0;
                publish(w->sources[i], fds[j].revents);
            }
        if (changed) {
            atomic_store_explicit(&w->completed, seen, memory_order_release);
            if (gpud_ready_os_signal(w->ack)) {
                fail(w->owner);
                finished(w);
                return;
            }
        }
    }
}
static int command(struct gpud_ready_worker *w, unsigned op, unsigned slot,
                   struct gpud_ready_source *s) {
    if (atomic_load_explicit(&w->finished, memory_order_acquire))
        return op == 3 ? 0 : -EIO;
    if (op != 3 && gpud_ready_error(w->owner))
        return -EIO;
    w->command = op;
    w->slot = slot;
    w->input = s;
    uint64_t rev = atomic_load_explicit(&w->revision, memory_order_relaxed) + 1;
    if (!rev)
        return -EOVERFLOW;
    atomic_store_explicit(&w->revision, rev, memory_order_release);
    if (gpud_ready_os_signal(w->control))
        return -EIO;
    while (atomic_load_explicit(&w->completed, memory_order_acquire) != rev) {
        if (atomic_load_explicit(&w->finished, memory_order_acquire))
            return op == 3 ? 0 : -EIO;
        if (gpud_ready_os_ack(w->thread, w->ack) < 0 || gpud_ready_os_drain(w->ack) ||
            (op != 3 && gpud_ready_error(w->owner)))
            return -EIO;
    }
    /* ACK remains readable if observed without parking; drain prevents an old
     * acknowledgement satisfying a later mailbox revision. */
    return gpud_ready_os_drain(w->ack);
}
struct gpud_ready *gpud_ready_create(void) {
    struct gpud_ready *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    atomic_flag_clear(&r->lock);
    r->notify = -1;
    if (gpud_ready_os_event(&r->notify)) {
        free(r);
        return NULL;
    }
    return r;
}
pacha_fd_t gpud_ready_fd(struct gpud_ready *r) { return r ? r->notify : -1; }
int gpud_ready_error(struct gpud_ready *r) {
    return r ? atomic_load_explicit(&r->error, memory_order_acquire) : -EINVAL;
}
int gpud_ready_add(struct gpud_ready *r, pacha_fd_t fd, uint64_t events, unsigned kind, void *ctx,
                   struct gpud_ready_source **out) {
    if (!r || !out || *out || fd < 16 || !events || gpud_ready_error(r))
        return -EINVAL;
    struct gpud_ready_worker *w = r->workers;
    while (w && w->occupied == SLOTS)
        w = w->next;
    if (!w) {
        w = calloc(1, sizeof(*w));
        if (!w)
            return -ENOMEM;
        w->owner = r;
        w->control = w->ack = -1;
        if (gpud_ready_os_event(&w->control) || gpud_ready_os_event(&w->ack) ||
            gpud_ready_os_start(w, gpud_ready_worker_run, &w->thread)) {
            if (w->control >= 16)
                (void)gpud_ready_os_close(w->control);
            if (w->ack >= 16)
                (void)gpud_ready_os_close(w->ack);
            free(w);
            return -ENOMEM;
        }
        w->next = r->workers;
        r->workers = w;
    }
    unsigned i = 0;
    while (w->allocated[i])
        i++;
    struct gpud_ready_source *s = calloc(1, sizeof(*s));
    if (!s)
        return -ENOMEM;
    if (r->sequence == UINT64_MAX) {
        free(s);
        return -EOVERFLOW;
    }
    *s = (struct gpud_ready_source){.owner = r,
                                    .worker = w,
                                    .slot = i,
                                    .kind = kind,
                                    .fd = fd,
                                    .events = events,
                                    .context = ctx,
                                    .generation = ++r->sequence};
    w->allocated[i] = 1;
    w->occupied++;
    *out = s;
    int error = command(w, 1, i, s);
    /* On a failed command the worker may own s. Retain it for terminal teardown,
     * never free it or reuse the FD while an observer may still access it. */
    return error;
}
int gpud_ready_rearm(struct gpud_ready_source *s) {
    return s ? command(s->worker, 1, s->slot, s) : -EINVAL;
}
int gpud_ready_remove(struct gpud_ready_source **ptr) {
    if (!ptr || !*ptr)
        return 0;
    struct gpud_ready_source *s = *ptr;
    struct gpud_ready *r = s->owner;
    int error = command(s->worker, 2, s->slot, NULL);
    if (error)
        return error;
    lock(r);
    if (s->queued) {
        if (s->prev)
            s->prev->next = s->next;
        else
            r->head = s->next;
        if (s->next)
            s->next->prev = s->prev;
        else
            r->tail = s->prev;
        s->queued = 0;
        s->next = s->prev = NULL;
    }
    unlock(r);
    s->worker->allocated[s->slot] = 0;
    s->worker->occupied--;
    free(s);
    *ptr = NULL;
    return 0;
}
int gpud_ready_next(struct gpud_ready *r, struct gpud_ready_event *out) {
    if (!r || !out)
        return -EINVAL;
    if (gpud_ready_error(r))
        return -EIO;
    lock(r);
    struct gpud_ready_source *s = r->head;
    if (s) {
        r->head = s->next;
        if (!r->head)
            r->tail = NULL;
        else
            r->head->prev = NULL;
        s->next = s->prev = NULL;
        s->queued = 0;
        *out = (struct gpud_ready_event){s, s->context, s->pending, s->generation, s->kind};
        s->pending = 0;
    }
    unlock(r);
    if (!s) {
        /* Read in this order: seeing a completed publisher via notifying
         * must also expose its completed notification count. While a writer is
         * between insertion and signal, retain the original drain/recheck. */
        unsigned notifying = atomic_load_explicit(&r->notifying, memory_order_acquire);
        uint64_t notifications = atomic_load_explicit(&r->notifications, memory_order_acquire);
        if (!notifying && notifications == r->drained_notifications)
            return 0;
        if (gpud_ready_os_drain(r->notify))
            return -EIO; /* arm/recheck: publisher after this drain leaves queue nonempty and
                            counter readable. */
        /* A signal racing with drain but not in this snapshot is deliberately
         * not acknowledged. A later turn drains it again; never lose a wake. */
        r->drained_notifications = notifications;
        lock(r);
        int again = r->head != NULL;
        unlock(r);
        if (again)
            return gpud_ready_next(r, out);
    }
    return s ? 1 : 0;
}
int gpud_ready_destroy(struct gpud_ready *r) {
    if (!r)
        return 0;
    /* Normally the owner removed every source. Terminal teardown must also join
     * errored workers before reclaiming any source/stack they may have touched. */
    for (struct gpud_ready_worker *w = r->workers; w; w = w->next) {
        if (command(w, 3, 0, NULL) && !atomic_load_explicit(&w->finished, memory_order_acquire))
            return -EIO;
        if (gpud_ready_os_join(w->thread))
            return -EIO;
    }
    struct gpud_ready_worker *w = r->workers;
    while (w) {
        struct gpud_ready_worker *next = w->next;
        for (unsigned i = 0; i < SLOTS; i++)
            if (w->sources[i])
                free(w->sources[i]);
        (void)gpud_ready_os_close(w->control);
        (void)gpud_ready_os_close(w->ack);
        free(w);
        w = next;
    }
    int error = gpud_ready_os_close(r->notify);
    free(r);
    return error;
}
