#define GPUD_READY_HOST_NO_MAIN
#include "gpud_ready_host.c"
#include "../userland/gpud/ready.c"
/* Exercise the publisher's two visibility windows explicitly, with the actual
 * queue/dequeue code. The worker is paused at the named boundary in each case. */
int main(void) {
    struct gpud_ready *r = gpud_ready_create();
    assert(r);
    struct gpud_ready_source s = {.owner=r,.generation=7,.kind=9};
    struct gpud_ready_event e;
    unsigned long n = atomic_load(&drain_calls);
    for (unsigned i=0;i<10000;i++) assert(!gpud_ready_next(r,&e));
    assert(atomic_load(&drain_calls)==n);
    /* Queue insertion visible; signal not yet issued. Consuming the event must
     * not require the publisher to run again, nor authorize a nonexistent FD. */
    s.queued=1;s.pending=PACHA_FD_EVENT_HANGUP;r->head=r->tail=&s;
    assert(gpud_ready_next(r,&e)==1 && e.source==&s && e.generation==7);
    assert(!gpud_ready_next(r,&e));
    /* Signal visible; completion counter not yet published. It stays readable
     * even if the owner sees an empty queue in this window. */
    assert(!gpud_ready_os_signal(r->notify));
    assert(!gpud_ready_next(r,&e));
    struct pollfd p={.fd=r->notify,.events=POLLIN};
    assert(poll(&p,1,0)==1);
    atomic_fetch_add_explicit(&r->notifications,1,memory_order_release);
    assert(!gpud_ready_next(r,&e));
    assert(poll(&p,1,0)==0);
    n=atomic_load(&drain_calls);
    for(unsigned i=0;i<10000;i++) assert(!gpud_ready_next(r,&e));
    assert(atomic_load(&drain_calls)==n);
    /* A publisher paused after writing but before publishing its count still
     * needs the original drain/recheck, so a stale counter cannot spin wait. */
    atomic_store(&r->notifying,1);
    assert(!gpud_ready_os_signal(r->notify));
    n=atomic_load(&drain_calls);
    assert(!gpud_ready_next(r,&e));
    assert(atomic_load(&drain_calls)==n+1 && poll(&p,1,0)==0);
    atomic_fetch_add(&r->notifications,1);
    atomic_store_explicit(&r->notifying,0,memory_order_release);
    assert(!gpud_ready_next(r,&e));
    /* A newer signal must not be acknowledged by an older drain snapshot. */
    publish(&s,PACHA_FD_EVENT_HANGUP);
    assert(gpud_ready_next(r,&e)==1 && e.events==PACHA_FD_EVENT_HANGUP);
    uint64_t snapshot=atomic_load(&r->notifications);
    publish(&s,PACHA_FD_EVENT_HANGUP);
    assert(!gpud_ready_os_drain(r->notify));
    r->drained_notifications=snapshot;
    assert(gpud_ready_next(r,&e)==1);
    assert(!gpud_ready_next(r,&e));
    assert(r->drained_notifications==atomic_load(&r->notifications));
    assert(!gpud_ready_destroy(r));
    puts("notify quiet fast path / insertion-before-signal / signal-before-count / stale drain snapshot PASS");
}
