#pragma once
#include <stdint.h>

#define PACHA_READINESS_BYTES 4096u
#define PACHA_READINESS_MASK UINT64_C(0xffff)
#define PACHA_READINESS_EXPORT UINT64_C(1)
#define PACHA_READINESS_ARM UINT64_C(2)
/* One service writer; all OFD owners map read-only. Each atomic cell couples
 * its level with its own progress stamp. Consuming one of several queued
 * messages must not manufacture another read edge; adding a message must.
 * Independent directions need no seqlock, retry loop, or time-based cache. */
struct pacha_readiness {
    uint64_t identity;
    uint64_t sample;
    uint64_t names; /* NETLINK: pid | groups<<32; listener: unused */
    uint64_t read_sample, write_sample, error_sample;
};
struct pacha_ready_observation { uint64_t read, write, error; uint32_t ready; };
static inline void pacha_readiness_cell(uint64_t *cell, uint32_t ready, uint32_t events)
{
    uint64_t previous = __atomic_load_n(cell, __ATOMIC_RELAXED);
    uint64_t generation = previous >> 16;
    if (generation == (UINT64_MAX >> 16)) {
        /* Exhaustion is a sticky error, never an aliased old edge. */
        __atomic_store_n(cell,(UINT64_MAX & ~PACHA_READINESS_MASK)|8u,__ATOMIC_RELEASE);
        return;
    }
    if (!generation || events || (ready & ~(uint32_t)previous)) generation++;
    __atomic_store_n(cell,(generation<<16)|ready,__ATOMIC_RELEASE);
}
static inline void pacha_readiness_publish_events(struct pacha_readiness *page,
    uint32_t ready, uint32_t events)
{
    pacha_readiness_cell(&page->read_sample,ready & (1u|0x2000u),events & (1u|0x2000u));
    pacha_readiness_cell(&page->write_sample,ready & 4u,events & 4u);
    pacha_readiness_cell(&page->error_sample,ready & (8u|16u),events & (8u|16u));
    /* Service bookkeeping/testing only; readers pair each direction's bits
     * with the progress loaded from that same atomic cell below. */
    __atomic_store_n(&page->sample,(UINT64_C(1)<<16)|ready,__ATOMIC_RELEASE);
}
static inline void pacha_readiness_publish(struct pacha_readiness *page, uint32_t ready)
{
    pacha_readiness_publish_events(page,ready,0);
}
static inline uint64_t pacha_readiness_load(const struct pacha_readiness *page)
{
    return __atomic_load_n(&page->sample,__ATOMIC_ACQUIRE);
}
static inline struct pacha_ready_observation pacha_readiness_observe(const struct pacha_readiness *page)
{
    uint64_t read=__atomic_load_n(&page->read_sample,__ATOMIC_ACQUIRE);
    uint64_t write=__atomic_load_n(&page->write_sample,__ATOMIC_ACQUIRE);
    uint64_t error=__atomic_load_n(&page->error_sample,__ATOMIC_ACQUIRE);
    return (struct pacha_ready_observation){.read=read>>16,.write=write>>16,
        .error=error>>16,.ready=(uint32_t)((read|write|error)&PACHA_READINESS_MASK)};
}
