/* SPDX-License-Identifier: MIT */
#ifndef GPUD_INDEX_H
#define GPUD_INDEX_H
#include <stdint.h>
#include <stddef.h>
struct gpud_index_entry {
    uint64_t a, b;
    uint32_t value;
    unsigned state;
};
static inline uint64_t gpud_index_hash(uint64_t a, uint64_t b) {
    a ^= b + UINT64_C(0x9e3779b97f4a7c15) + (a << 6) + (a >> 2);
    a ^= a >> 30;
    a *= UINT64_C(0xbf58476d1ce4e5b9);
    a ^= a >> 27;
    a *= UINT64_C(0x94d049bb133111eb);
    return a ^ (a >> 31);
}
/* Owner-only bounded open addressing. Tombstones are reused; lookup always
 * verifies both keys. No index result grants FD or object authority. */
static inline uint32_t gpud_index_get(const struct gpud_index_entry *t, size_t n, uint64_t a,
                                      uint64_t b) {
    size_t h = gpud_index_hash(a, b) % n;
    for (size_t i = 0; i < n; i++) {
        const struct gpud_index_entry *e = t + (h + i) % n;
        if (!e->state)
            return 0;
        if (e->state == 1 && e->a == a && e->b == b)
            return e->value;
    }
    return 0;
}
static inline int gpud_index_put(struct gpud_index_entry *t, size_t n, uint64_t a, uint64_t b,
                                 uint32_t value) {
    size_t h = gpud_index_hash(a, b) % n, free = n;
    for (size_t i = 0; i < n; i++) {
        size_t j = (h + i) % n;
        struct gpud_index_entry *e = t + j;
        if (e->state == 1 && e->a == a && e->b == b) {
            e->value = value;
            return 0;
        }
        if (e->state != 1 && free == n)
            free = j;
        if (!e->state)
            break;
    }
    if (free == n)
        return -1;
    t[free] = (struct gpud_index_entry){a, b, value, 1};
    return 0;
}
static inline void gpud_index_remove(struct gpud_index_entry *t, size_t n, uint64_t a, uint64_t b) {
    size_t h = gpud_index_hash(a, b) % n, hole = n;
    for (size_t i = 0; i < n; i++) {
        size_t j = (h + i) % n;
        if (!t[j].state)
            return;
        if (t[j].a == a && t[j].b == b) {
            hole = j;
            break;
        }
    }
    if (hole == n)
        return;
    /* Back-shift deletion prevents tombstones from turning repeated mapping
     * lifetimes into a capacity-wide miss scan. One owner excludes lookups. */
    size_t j = (hole + 1) % n;
    while (t[j].state) {
        size_t home = gpud_index_hash(t[j].a, t[j].b) % n;
        if ((hole + n - home) % n < (j + n - home) % n) {
            t[hole] = t[j];
            hole = j;
        }
        j = (j + 1) % n;
    }
    t[hole] = (struct gpud_index_entry){0};
}
#endif
