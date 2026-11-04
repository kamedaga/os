#define _POSIX_C_SOURCE 200809L
#include "../userland/gpud/index.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static uint64_t ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000ULL + t.tv_nsec;
}
int main(void) {
    unsigned n[] = {256, 1024, 4096, 16384};
    for (unsigned k = 0; k < 4; k++) {
        size_t cap = 2 * n[k];
        struct gpud_index_entry *t = calloc(cap, sizeof(*t));
        assert(t);
        for (unsigned i = 0; i < n[k]; i++)
            assert(!gpud_index_put(t, cap, (i + 1) * 4096ULL, 17, i + 1));
        volatile uint32_t sum = 0;
        uint64_t b = ns();
        for (unsigned i = 0; i < 100000; i++)
            sum += gpud_index_get(t, cap, ((i % n[k]) + 1) * 4096ULL, 17);
        uint64_t e = ns();
        assert(sum);
        printf("index N=%u lookup_ns=%.2f\n", n[k], (e - b) / 100000.0);
        for (unsigned round = 0; round < 4; round++) {
            for (unsigned i = 0; i < n[k]; i += 2)
                gpud_index_remove(t, cap, (i + 1) * 4096ULL, 17);
            for (unsigned i = 0; i < n[k]; i++)
                assert(gpud_index_get(t, cap, (i + 1) * 4096ULL, 17) == (i % 2 ? i + 1 : 0));
            for (unsigned i = 0; i < n[k]; i += 2)
                assert(!gpud_index_put(t, cap, (i + 1) * 4096ULL, 17, i + 1));
        }
        for (unsigned i = 0; i < n[k]; i++)
            gpud_index_remove(t, cap, (i + 1) * 4096ULL, 17);
        for (size_t i = 0; i < cap; i++)
            assert(!t[i].state);
        free(t);
    }
    /* Deliberately form a wrap-around collision cluster and delete its head. */
    struct gpud_index_entry t[8] = {0};
    uint64_t keys[4];
    unsigned found = 0;
    for (uint64_t i = 1; found < 4; i++)
        if (gpud_index_hash(i, 0) % 8 == 7)
            keys[found++] = i;
    for (unsigned i = 0; i < 4; i++)
        assert(!gpud_index_put(t, 8, keys[i], 0, i + 1));
    gpud_index_remove(t, 8, keys[0], 0);
    for (unsigned i = 1; i < 4; i++)
        assert(gpud_index_get(t, 8, keys[i], 0) == i + 1);
    puts("INDEX churn/wrap/collision/key-validation PASS");
}
