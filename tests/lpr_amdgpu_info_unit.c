/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <amdgpu_drm.h>
#include "../userland/gpud/include/gpud/drm_amdgpu_abi.h"
#include "../kobox2/protocol/generated/include/kobox2/gpu_drm_amdgpu_layout.h"
#define LPR_LINUX_EINVAL EINVAL
#include "../userland/personality/linux/runtime/lpr_drm/amdgpu_info.h"

int main(void)
{
    /* Use the actual libdrm development headers, not the newer Linux tree
     * used by our driver-side layout assertions. */
    assert(sizeof(struct drm_amdgpu_info_device) == 440);
    assert(sizeof(struct drm_amdgpu_info_hw_ip) == 32);
    const uint32_t capacities[] = {1, 31, 32, 40, 439, 440, 448, 449, 1024};
    const uint32_t queries[] = {AMDGPU_INFO_DEV_INFO, AMDGPU_INFO_HW_IP_INFO};
    for (size_t q = 0; q < 2; ++q) for (size_t c = 0; c < sizeof(capacities)/sizeof(*capacities); ++c) {
        gpud_drm_amdgpu_info_t info = {.return_pointer=1,
            .return_size=capacities[c], .query=queries[q]};
        assert(!lpr_amdgpu_info_prepare(&info));
        size_t native = q ? sizeof(gpud_drm_amdgpu_hw_ip_native_t) : sizeof(gpud_drm_amdgpu_device_native_t);
        unsigned char src[448], dst[1026];
        memset(src, 0x5a, sizeof(src)); memset(dst, 0xa5, sizeof(dst));
        uint32_t n = lpr_amdgpu_info_copy_size(capacities[c], native);
        assert(n == (capacities[c] < native ? capacities[c] : native));
        memcpy(dst + 1, src, n);
        assert(dst[0] == 0xa5 && dst[n + 1] == 0xa5 && dst[capacities[c] + 1] == 0xa5);
    }
    gpud_drm_amdgpu_info_t mmr = {.return_pointer=1,.return_size=4,
        .query=AMDGPU_INFO_READ_MMR_REG,.selector={0x263e,1,UINT32_MAX,0}};
    assert(!lpr_amdgpu_info_prepare(&mmr) && mmr.return_size == 4);
    mmr.selector[1] = 128;
    assert(!lpr_amdgpu_info_prepare(&mmr) && mmr.return_size == 512);
    mmr.selector[1] = 129; assert(lpr_amdgpu_info_prepare(&mmr) == -EINVAL);
    mmr.selector[1] = 0; assert(lpr_amdgpu_info_prepare(&mmr) == -EINVAL);
    mmr.selector[1] = 2; mmr.selector[0] = UINT32_MAX;
    assert(lpr_amdgpu_info_prepare(&mmr) == -EINVAL);
    mmr.return_pointer = 0; assert(lpr_amdgpu_info_prepare(&mmr) == -EINVAL);
    gpud_drm_amdgpu_info_t scalar={.return_pointer=1,.return_size=1,
        .query=AMDGPU_INFO_ACCEL_WORKING};
    assert(!lpr_amdgpu_info_prepare(&scalar) && scalar.return_size==4);
    scalar.query=AMDGPU_INFO_TIMESTAMP; scalar.return_size=1024;
    assert(!lpr_amdgpu_info_prepare(&scalar) && scalar.return_size==8);

    const uint32_t extra_queries[] = {AMDGPU_INFO_FW_VERSION, AMDGPU_INFO_VRAM_GTT,
        AMDGPU_INFO_MEMORY, AMDGPU_INFO_VIDEO_CAPS, AMDGPU_INFO_MAX_IBS};
    const uint32_t extra_bytes[] = {8, 24, 96, 192, 40};
    assert(sizeof(struct drm_amdgpu_info_firmware) == 8);
    assert(sizeof(struct drm_amdgpu_memory_info) == 96);
    assert(sizeof(struct drm_amdgpu_info_video_caps) == 192);
    for (unsigned i=0; i<5; ++i) {
        gpud_drm_amdgpu_info_t info={.return_pointer=1,.return_size=1,
            .query=extra_queries[i]};
        if (!i) { info.selector[0]=AMDGPU_INFO_FW_GFX_ME; info.selector[2]=2; }
        assert(!lpr_amdgpu_info_prepare(&info) && info.return_size==extra_bytes[i]);
        info.selector[3]=1;
        assert(lpr_amdgpu_info_prepare(&info)==-EINVAL);
    }
    puts("LPR AMDGPU INFO: libdrm 440/32-byte prefixes, bounded copies, MMR bounds PASS");
}
