#include <assert.h>
#include <stdio.h>
#include "../kobox2/linux-sandbox/kobox/boot/drm_memory_policy.h"

int main(void)
{
    const unsigned long long page = 4096, reserve = 16ull << 20;
    const unsigned long long fhd = 1920ull * 1080 * 4;
    const unsigned long long pages = (fhd + page - 1) / page;
    const unsigned long long needed = reserve + pages * (page + 64) + 65536;
    assert(kobox_drm_memory_admit(needed, fhd, 0));
    assert(!kobox_drm_memory_admit(needed - 1, fhd, 0));
    assert(!kobox_drm_memory_admit(needed, fhd, 1));
    assert(kobox_drm_memory_admit(needed + reserve, fhd, 1));
    assert(!kobox_drm_memory_admit(0, 0, 0));
    assert(!kobox_drm_memory_admit(reserve, 0, 0));
    assert(kobox_drm_memory_admit(reserve + page + 64 + 65536, 0, 0));
    assert(!kobox_drm_memory_admit(~0ull, ~0ull, 0));
    assert(!kobox_drm_memory_admit(256ull << 20, 256ull << 20, 0));
    puts("DRM memory admission: reserve, FHD boundary, zero and overflow PASS");
}
