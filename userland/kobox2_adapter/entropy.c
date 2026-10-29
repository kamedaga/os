/* SPDX-License-Identifier: MIT */
#include "entropy.h"
#include "boot.h"
#include "host.h"
#include <errno.h>

static int hardware_seed_word(uint64_t *word) {
    for (unsigned attempt = 0; attempt < 1024; attempt++) {
        unsigned char ready;
        __asm__ volatile("rdseed %0; setc %1" : "=r"(*word), "=qm"(ready) : : "cc");
        if (ready) return 0;
        __asm__ volatile("pause" ::: "memory");
    }
    return -EAGAIN;
}

int ph_seed_linux_entropy(void) {
    unsigned char bytes[64];
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    if (a < 7) return -EOPNOTSUPP;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    if (!(b & (1u << 18))) return -EOPNOTSUPP;
    int result = 0;
    /* Native getrandom currently derives ASLR words from timing, not a
     * cryptographic pool. Never credit that output as Linux entropy. RDSEED
     * is unprivileged; absent/exhausted hardware has no weak fallback. */
    for (size_t used = 0; used < sizeof(bytes); used += sizeof(uint64_t)) {
        uint64_t word = 0;
        result = hardware_seed_word(&word);
        if (!result) memcpy(bytes + used, &word, sizeof(word));
        *(volatile uint64_t *)&word = 0;
        if (result) break;
    }
    if (!result) {
        int (*seed)(const void *, size_t);
        void *symbol = ph_image_lookup(&ph_core, "kobox_linux_entropy_seed");
        if (!symbol) result = -ENOENT;
        else {
            memcpy(&seed, &symbol, sizeof(seed));
            result = seed(bytes, sizeof(bytes));
        }
    }
    for (size_t i = 0; i < sizeof(bytes); i++)
        ((volatile unsigned char *)bytes)[i] = 0;
    return result;
}
