/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../kobox2/linux-sandbox/kobox/boot/drm_time.h"

int main(void)
{
    unsigned long long translated = 0;
    const unsigned long long origin = 181000000000ull;
    assert(kobox_drm_time_translate(5000000000000ull,
        5000010000000ull, 5000010000000ull + origin, &translated));
    assert(translated == 5000000000000ull + origin);
    /* Queued events retain their age, not the time at which they are read. */
    assert(kobox_drm_time_translate(4999980000000ull,
        5000010000000ull, 5000010000000ull + origin, &translated));
    assert(translated == 4999980000000ull + origin);
    assert(kobox_drm_time_translate(70, 100, 80, &translated) && translated == 50);
    translated = 123;
    assert(!kobox_drm_time_translate(19, 100, 80, &translated));
    assert(translated == 123);
    assert(!kobox_drm_time_translate(UINT64_MAX, 0, 1, &translated));
    assert(translated == 123);
    assert(!kobox_drm_time_translate(0, 0, 0, NULL));
    assert(kobox_drm_time_translate(UINT64_MAX, 77, 77, &translated));
    assert(translated == UINT64_MAX);
    assert(kobox_drm_deadline_translate(5000020000000ull + origin,
        5000010000000ull + origin, 5000010000000ull) == 5000020000000ull);
    assert(!kobox_drm_deadline_translate(1, origin, 0));
    assert(!kobox_drm_deadline_translate(0, origin, 0));
    assert(kobox_drm_deadline_translate(UINT64_MAX, origin, 0) == UINT64_MAX);
    assert(kobox_drm_deadline_translate(INT64_MAX, origin, 0) == INT64_MAX);
    assert(kobox_drm_deadline_translate(INT64_MAX - 1, 0, 100) == INT64_MAX);
    puts("DRM clock epochs: PASS event age, both offset signs and bounds");
    return 0;
}
