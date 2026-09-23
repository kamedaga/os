/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#define KOBOX_DRM_FENCE_UNIT_TEST 1
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Model only the Linux primitives used by the actual owned queue. Signaling
 * keeps the fence lock across callbacks, as dma_fence_signal_locked does. */
typedef uint64_t u64;
typedef pthread_mutex_t spinlock_t;
struct task_struct { int identity; };
static struct task_struct owner, stranger;
static _Thread_local struct task_struct *current;
static _Thread_local bool irq_context;
static bool allocation_failure;
static size_t allocations;
static atomic_bool callback_active;
#define GFP_KERNEL 0
#define kobox_host_call(expression) (expression)
#define container_of(p, type, field) ((type *)((char *)(p) - offsetof(type, field)))
struct list_head { struct list_head *next, *prev; };
static void INIT_LIST_HEAD(struct list_head *p) { p->next = p->prev = p; }
static bool list_empty(const struct list_head *p) { return p->next == p; }
static void list_add_tail(struct list_head *p, struct list_head *head)
{
    p->prev = head->prev; p->next = head;
    head->prev->next = p; head->prev = p;
}
static void list_del(struct list_head *p)
{
    p->prev->next = p->next; p->next->prev = p->prev;
}
static void list_del_init(struct list_head *p) { list_del(p); INIT_LIST_HEAD(p); }
#define list_first_entry(head, type, field) container_of((head)->next, type, field)
#define spin_lock_init(lock) assert(!pthread_mutex_init((lock), NULL))
#define spin_lock_irqsave(lock, flags) do { (flags) = 0; assert(!pthread_mutex_lock(lock)); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); assert(!pthread_mutex_unlock(lock)); } while (0)
static bool in_interrupt(void) { return irq_context; }
static bool irqs_disabled(void) { return irq_context; }
static void *kzalloc(size_t bytes, int flags)
{
    (void)flags;
    if (allocation_failure) return NULL;
    void *p = calloc(1, bytes); assert(p); ++allocations; return p;
}
static void kfree(void *p)
{
    assert(!atomic_load(&callback_active));
    assert(p && allocations); --allocations; free(p);
}
struct dma_fence;
struct dma_fence_cb {
    struct list_head node;
    void (*func)(struct dma_fence *, struct dma_fence_cb *);
};
struct dma_fence {
    spinlock_t lock;
    struct list_head callbacks;
    atomic_int refs;
    int status;
};
static void fence_init(struct dma_fence *fence, int status)
{
    spin_lock_init(&fence->lock); INIT_LIST_HEAD(&fence->callbacks);
    atomic_init(&fence->refs, 2); fence->status = status;
}
static int dma_fence_add_callback(struct dma_fence *fence, struct dma_fence_cb *cb,
                                 void (*func)(struct dma_fence *, struct dma_fence_cb *))
{
    assert(!pthread_mutex_lock(&fence->lock));
    int error = fence->status ? -ENOENT : 0;
    INIT_LIST_HEAD(&cb->node);
    if (!error) { cb->func = func; list_add_tail(&cb->node, &fence->callbacks); }
    assert(!pthread_mutex_unlock(&fence->lock)); return error;
}
static bool dma_fence_remove_callback(struct dma_fence *fence, struct dma_fence_cb *cb)
{
    assert(!pthread_mutex_lock(&fence->lock));
    bool queued = !list_empty(&cb->node);
    list_del_init(&cb->node);
    assert(!pthread_mutex_unlock(&fence->lock)); return queued;
}
static int dma_fence_get_status_locked(struct dma_fence *fence) { return fence->status; }
static int dma_fence_get_status(struct dma_fence *fence)
{
    assert(!pthread_mutex_lock(&fence->lock)); int status = fence->status;
    assert(!pthread_mutex_unlock(&fence->lock)); return status;
}
static void dma_fence_put(struct dma_fence *fence) { assert(atomic_fetch_sub(&fence->refs, 1) > 0); }
static void signal_fence(struct dma_fence *fence, int status)
{
    assert(status);
    assert(!pthread_mutex_lock(&fence->lock)); fence->status = status;
    while (!list_empty(&fence->callbacks)) {
        struct dma_fence_cb *cb = list_first_entry(&fence->callbacks, struct dma_fence_cb, node);
        list_del_init(&cb->node); cb->func(fence, cb);
    }
    assert(!pthread_mutex_unlock(&fence->lock));
}

#include "../kobox2/linux-sandbox/kobox/boot/drm_fence.c"

struct notifications { unsigned count; int error; bool block; };
static atomic_bool release_callback, retire_started;
static int notify(void *context)
{
    struct notifications *n = context;
    ++n->count;
    if (n->block) {
        atomic_store(&callback_active, true);
        while (!atomic_load(&release_callback)) sched_yield();
        atomic_store(&callback_active, false);
    }
    return n->error;
}

static void completion_cases(void)
{
    struct notifications n = {0};
    struct kobox_drm_fences queue = {0};
    struct kobox_drm_fence_entry *a = NULL, *b = NULL;
    struct kobox_drm_fence_result result;
    struct dma_fence first, second;
    assert(!kobox_drm_fences_init(&queue, notify, &n));
    assert(!kobox_drm_fence_reserve(&queue, 7, 10, &a));
    assert(!kobox_drm_fence_reserve(&queue, 9, 11, &b));
    fence_init(&first, 0); fence_init(&second, 0);
    assert(!kobox_drm_fence_arm(a, &first));
    assert(!kobox_drm_fence_arm(b, &second));
    assert(!kobox_drm_fences_take(&queue, &result));
    signal_fence(&second, -EIO); signal_fence(&first, 1);
    assert(n.count == 1); /* Coalesce only while an unread completion exists. */
    assert(kobox_drm_fences_take(&queue, &result) == 1);
    assert(result.session == 9 && result.correlation == 11 && result.status == -EIO);
    assert(kobox_drm_fences_take(&queue, &result) == 1);
    assert(result.session == 7 && result.correlation == 10 && result.status == 1);
    assert(!kobox_drm_fences_take(&queue, &result));
    assert(atomic_load(&first.refs) == 1 && atomic_load(&second.refs) == 1);
    assert(!allocations);
    a = NULL;
    assert(!kobox_drm_fence_reserve(&queue, 7, 12, &a));
    struct dma_fence already;
    fence_init(&already, 1);
    assert(!kobox_drm_fence_arm(a, &already));
    assert(n.count == 2 && kobox_drm_fences_take(&queue, &result) == 1);
    assert(result.correlation == 12 && result.status == 1);
    assert(atomic_load(&already.refs) == 1 && !allocations);
    assert(!kobox_drm_fences_quiesce(&queue));
}

static void failure_and_retirement_cases(void)
{
    struct notifications n = {0};
    struct kobox_drm_fences queue = {0};
    struct kobox_drm_fence_entry *a = NULL, *b = NULL;
    struct kobox_drm_fence_result result;
    struct dma_fence pending;
    assert(!kobox_drm_fences_init(&queue, notify, &n));
    current = &stranger;
    assert(kobox_drm_fence_reserve(&queue, 1, 1, &a) == -EPERM);
    current = &owner;
    allocation_failure = true;
    assert(kobox_drm_fence_reserve(&queue, 1, 1, &a) == -ENOMEM && !a);
    allocation_failure = false;
    assert(!kobox_drm_fence_reserve(&queue, 1, 1, &a));
    assert(kobox_drm_fence_reserve(&queue, 1, 1, &b) == -EINVAL);
    assert(!kobox_drm_fence_cancel(&a) && !a);
    assert(!kobox_drm_fence_reserve(&queue, 1, 2, &a));
    assert(!kobox_drm_fence_reserve(&queue, 1, 3, &b));
    fence_init(&pending, 0);
    assert(!kobox_drm_fence_arm(a, &pending));
    assert(kobox_drm_fence_arm(a, &pending) == -EINVAL);
    assert(!kobox_drm_fences_quiesce(&queue));
    signal_fence(&pending, 1);
    assert(!n.count && !allocations && atomic_load(&pending.refs) == 1);
    a = NULL;
    assert(kobox_drm_fence_reserve(&queue, 1, 4, &a) == -ESHUTDOWN);
    assert(!kobox_drm_fences_take(&queue, &result));
    assert(!kobox_drm_fences_quiesce(&queue));

    struct kobox_drm_fences failed = {0};
    struct dma_fence done;
    n.error = -EPIPE;
    assert(!kobox_drm_fences_init(&failed, notify, &n));
    assert(!kobox_drm_fence_reserve(&failed, 1, 1, &a));
    fence_init(&done, -ECANCELED);
    assert(!kobox_drm_fence_arm(a, &done));
    assert(kobox_drm_fences_take(&failed, &result) == -EPIPE);
    assert(!kobox_drm_fences_quiesce(&failed) && !allocations);
}

static void *signal_thread(void *fence) { signal_fence(fence, 1); return NULL; }
static void *release_thread(void *ignored)
{
    (void)ignored;
    while (!atomic_load(&retire_started)) sched_yield();
    assert(atomic_load(&callback_active));
    atomic_store(&release_callback, true);
    return NULL;
}
static void callback_retirement_race(bool quiesce)
{
    struct notifications n = {.block = true};
    struct kobox_drm_fences queue = {0};
    struct kobox_drm_fence_entry *entry = NULL;
    struct kobox_drm_fence_result result;
    struct dma_fence fence;
    pthread_t signaler, releaser;
    atomic_store(&release_callback, false);
    atomic_store(&retire_started, false);
    assert(!kobox_drm_fences_init(&queue, notify, &n));
    assert(!kobox_drm_fence_reserve(&queue, 1, 1, &entry));
    fence_init(&fence, 0);
    assert(!kobox_drm_fence_arm(entry, &fence));
    assert(!pthread_create(&signaler, NULL, signal_thread, &fence));
    while (!atomic_load(&callback_active)) sched_yield();
    assert(!pthread_create(&releaser, NULL, release_thread, NULL));
    atomic_store(&retire_started, true);
    if (quiesce)
        assert(!kobox_drm_fences_quiesce(&queue));
    else {
        assert(kobox_drm_fences_take(&queue, &result) == 1);
        assert(result.status == 1);
    }
    assert(!pthread_join(signaler, NULL) && !pthread_join(releaser, NULL));
    assert(!allocations && atomic_load(&fence.refs) == 1);
    assert(!kobox_drm_fences_quiesce(&queue));
}

int main(void)
{
    current = &owner;
    completion_cases();
    failure_and_retirement_cases();
    callback_retirement_race(false);
    callback_retirement_race(true);
    puts("DRM fence queue: deferred/error/already-signaled, ownership, allocation, quiesce, callback race PASS");
}
