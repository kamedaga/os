#include "drm_property_metadata.h"
#include "drmd/kms_abi.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_object(uint32_t property, const char *name, uint32_t type)
{
    drmd_property_metadata_t metadata;
    assert(drmd_atomic_property_metadata(property, &metadata) == 0);
    assert(metadata.flags == DRMD_MODE_PROP_OBJECT);
    assert(metadata.count_values == 1);
    assert(metadata.values[0] == type);
    assert(strcmp(metadata.name, name) == 0);
}

int main(void)
{
    expect_object(
        DRMD_KMS_CONNECTOR_CRTC_ID_PROP_ID,
        "CRTC_ID",
        DRMD_MODE_OBJECT_CRTC);
    expect_object(
        DRMD_KMS_PLANE_CRTC_ID_PROP_ID,
        "CRTC_ID",
        DRMD_MODE_OBJECT_CRTC);
    expect_object(
        DRMD_KMS_PLANE_FB_ID_PROP_ID,
        "FB_ID",
        DRMD_MODE_OBJECT_FB);

    drmd_property_metadata_t metadata;
    assert(drmd_atomic_property_metadata(
        DRMD_KMS_CRTC_ACTIVE_PROP_ID, &metadata) == 0);
    assert(metadata.flags == DRMD_MODE_PROP_RANGE);
    assert(metadata.count_values == 2);
    assert(metadata.values[0] == 0 && metadata.values[1] == 1);
    assert(drmd_atomic_property_metadata(9999, &metadata) == -2);
    puts("drmd property metadata unit: ok");
    return 0;
}
