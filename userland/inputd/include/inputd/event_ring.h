#ifndef INPUTD_EVENT_RING_H
#define INPUTD_EVENT_RING_H
#include <inputd/ipc_protocol.h>
#include <stddef.h>
#include <string.h>
/* No pointers, locks or ABI version. One producer per open file description.
 * dup/fork/SCM_RIGHTS readers share head. Speculative atomic copies are
 * committed by CAS: overflow/another reader invalidates the entire copy.
 * A client dying halfway through read cannot strand a producer-owned lock. */
#define INPUTD_RING_BYTES 65536u
#define INPUTD_SHARED_EVENTS 2048u
struct inputd_event_ring {
    uint64_t handle; /* OPEN acknowledgement, not an ABI version */
    uint64_t head, tail;
    int32_t error;
    uint32_t armed; /* prepare-wait handshake; producer exchanges after tail */
    uint64_t overflows;
    uint64_t cells[INPUTD_SHARED_EVENTS][3];
};
_Static_assert(sizeof(struct inputd_event_ring) <= INPUTD_RING_BYTES, "input ring VMO");
_Static_assert(__atomic_always_lock_free(8, 0), "input ring needs atomic u64");
static inline uint64_t inputd_ring_head(struct inputd_event_ring *r) { return __atomic_load_n(&r->head,__ATOMIC_SEQ_CST); }
static inline uint64_t inputd_ring_tail(struct inputd_event_ring *r) { return __atomic_load_n(&r->tail,__ATOMIC_SEQ_CST); }
static inline int inputd_ring_error(struct inputd_event_ring *r) { return __atomic_load_n(&r->error,__ATOMIC_ACQUIRE); }
static inline int inputd_ring_ready(struct inputd_event_ring *r) { return inputd_ring_error(r) || inputd_ring_head(r)!=inputd_ring_tail(r); }
static inline void inputd_ring_load(struct inputd_event_ring *r,uint64_t seq,inputd_input_event_t *e) {
    uint64_t v[3]; for(unsigned j=0;j<3;j++)v[j]=__atomic_load_n(&r->cells[seq%INPUTD_SHARED_EVENTS][j],__ATOMIC_SEQ_CST);
    memcpy(e,v,sizeof(v));
}
static inline void inputd_ring_store(struct inputd_event_ring *r,uint64_t seq,const inputd_input_event_t *e) {
    uint64_t v[3];memcpy(v,e,sizeof(v));for(unsigned j=0;j<3;j++)__atomic_store_n(&r->cells[seq%INPUTD_SHARED_EVENTS][j],v[j],__ATOMIC_SEQ_CST);
}
/* Returns event count, or negative Linux errno. Short, non-multiple buffers
 * are handled by the caller: only whole input_event records are consumed. */
static inline int inputd_ring_read(struct inputd_event_ring *r,void *out,size_t capacity) {
    if(!capacity)return -22;
    for(;;){
        int error=inputd_ring_error(r);if(error)return error;
        uint64_t h=inputd_ring_head(r),t=inputd_ring_tail(r),n=t-h;
        if(!n)return -11;
        if(n>INPUTD_SHARED_EVENTS)return -5;
        if(!out)return -14;
        if(n>capacity)n=capacity;
        for(uint64_t i=0;i<n;i++){inputd_input_event_t e;inputd_ring_load(r,h+i,&e);memcpy((char*)out+i*sizeof(e),&e,sizeof(e));}
        if(__atomic_compare_exchange_n(&r->head,&h,h+n,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST))return (int)n;
    }
}
/* Producer keeps its trusted tail privately; never indexes using client data.
 * Overflow discards unread records, then publishes SYN_DROPPED
 * before a complete new frame. Advancing head invalidates concurrent copies. */
static inline int inputd_ring_publish(struct inputd_event_ring *r,uint64_t *producer_tail,const inputd_input_event_t *e,size_t n) {
    if(!n||n>INPUTD_SHARED_EVENTS-1)return 0;
    uint64_t t=*producer_tail,h=inputd_ring_head(r);int empty=h==t;
    if(t-h>INPUTD_SHARED_EVENTS-n){
        __atomic_store_n(&r->head,t,__ATOMIC_SEQ_CST);
        inputd_input_event_t syn=e[0];syn.type=0;syn.code=3;syn.value=0;inputd_ring_store(r,t++,&syn);
        __atomic_fetch_add(&r->overflows,1,__ATOMIC_RELAXED);empty=1;
    }
    for(size_t i=0;i<n;i++)inputd_ring_store(r,t+i,&e[i]);
    *producer_tail=t+n;__atomic_store_n(&r->tail,t+n,__ATOMIC_SEQ_CST);
    return __atomic_exchange_n(&r->armed,0,__ATOMIC_SEQ_CST) || empty;
}
#endif
