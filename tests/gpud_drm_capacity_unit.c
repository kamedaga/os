#include <assert.h>
#include <stdio.h>
#include "../userland/gpud/drm_service.c"

int main(void)
{
    static struct gpud_drm_service service;
    assert(GPUD_DRM_MAPPINGS_MAX == 256);
    assert(GPUD_DRM_MAPPINGS_MAX == KOBOX_DRM_MAPPING_LIMIT);
    for (size_t i = 0; i < GPUD_DRM_MAPPINGS_MAX; ++i) {
        struct gpud_drm_mapping *slot = empty_mapping(&service);
        assert(slot == &service.mappings[i]);
        slot->id = (i + 1) * 4096;
        slot->handle = i % 6 + 1;
    }
    assert(!empty_mapping(&service));
    /* Closing a GEM owner is not sufficient to drop a live mapping lease. */
    service.mappings[64].owner_closed = 1;
    assert(!empty_mapping(&service));
    memset(&service.mappings[64], 0, sizeof(service.mappings[64]));
    assert(empty_mapping(&service) == &service.mappings[64]);
    for (size_t i = 0; i < GPUD_DRM_MAPPINGS_MAX; ++i)
        if (i != 64) assert(find_mapping(&service, (i + 1) * 4096));
    puts("GPUD 256 mappings, exhaustion and released-slot reuse PASS");
}
