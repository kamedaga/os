/* SPDX-License-Identifier: MIT */
#include "../kobox2/linux-sandbox/kobox/boot/cpu_features.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    const unsigned int clflush = 1u << 19;
    assert(kobox_cpuid_clflush_size(clflush, 8u << 8, 4096) == 64);
    assert(kobox_cpuid_clflush_size(clflush, 16u << 8, 4096) == 128);
    assert(kobox_cpuid_clflush_size(clflush, (8u << 8) | 0xabcd00ff, 4096) == 64);
    assert(!kobox_cpuid_clflush_size(0, 8u << 8, 4096));
    assert(!kobox_cpuid_clflush_size(1u << 20, 8u << 8, 4096));
    assert(!kobox_cpuid_clflush_size(clflush, 0, 4096));
    assert(!kobox_cpuid_clflush_size(clflush, 3u << 8, 4096));
    assert(!kobox_cpuid_clflush_size(clflush, 255u << 8, 4096));
    assert(!kobox_cpuid_clflush_size(clflush, 8u << 8, 32));
    puts("Kobox CLFLUSH CPUID: supported, absent, invalid size and unrelated bits PASS");
    return 0;
}
