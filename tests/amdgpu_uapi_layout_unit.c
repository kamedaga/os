/* SPDX-License-Identifier: MIT */
#include <stddef.h>


#ifndef __user
#define __user
#define AMDGPU_LAYOUT_UNDEF_USER
#endif
#include "../kobox2/linux-sandbox/include/uapi/drm/amdgpu_drm.h"
#ifdef AMDGPU_LAYOUT_UNDEF_USER
#undef __user
#endif
#include "../kobox2/protocol/generated/include/kobox2/gpu_drm_amdgpu_layout.h"
#include "../kobox2/protocol/include/kobox2/amdgpu_info_bridge.h"
#include "../userland/gpud/include/gpud/drm_amdgpu_abi.h"

_Static_assert(sizeof(struct drm_amdgpu_info_device) ==
    sizeof(gpud_drm_amdgpu_device_native_t), "device UAPI size");
_Static_assert(sizeof(struct drm_amdgpu_info_hw_ip) ==
    sizeof(gpud_drm_amdgpu_hw_ip_native_t), "HW IP UAPI size");
_Static_assert(DRM_IOCTL_AMDGPU_INFO == GPUD_DRM_IOCTL_AMDGPU_INFO,
    "INFO ioctl number");
_Static_assert(DRM_IOCTL_AMDGPU_GEM_CREATE == GPUD_DRM_IOCTL_AMDGPU_GEM_CREATE,
    "GEM_CREATE ioctl number");
_Static_assert(DRM_IOCTL_AMDGPU_GEM_MMAP == GPUD_DRM_IOCTL_AMDGPU_GEM_MMAP,
    "GEM_MMAP ioctl number");
_Static_assert(DRM_IOCTL_AMDGPU_CTX == GPUD_DRM_IOCTL_AMDGPU_CTX,
    "CTX ioctl number");
_Static_assert(DRM_IOCTL_AMDGPU_CS == GPUD_DRM_IOCTL_AMDGPU_CS,
    "CS ioctl number");
_Static_assert(DRM_IOCTL_AMDGPU_GEM_VA == GPUD_DRM_IOCTL_AMDGPU_GEM_VA,
    "GEM_VA ioctl number");
_Static_assert(DRM_IOCTL_AMDGPU_WAIT_CS == GPUD_DRM_IOCTL_AMDGPU_WAIT_CS,
    "WAIT_CS ioctl number");
_Static_assert(DRM_IOCTL_AMDGPU_VM == GPUD_DRM_IOCTL_AMDGPU_VM,
    "VM ioctl number");

#define CHECK_DEVICE(member, wire) \
    _Static_assert(offsetof(struct drm_amdgpu_info_device, member) == \
        offsetof(gpud_drm_amdgpu_device_native_t, member), \
        "device field offset: " #member); \
    _Static_assert(sizeof(((struct drm_amdgpu_info_device *)0)->member) == \
        sizeof(((gpud_drm_amdgpu_device_native_t *)0)->member), \
        "device field size: " #member); \
    _Static_assert(KB2_GPU_DRM_AMDGPU_RECORD_INFO_DEVICE_##wire##_OFFSET + \
        sizeof(((gpud_drm_amdgpu_device_native_t *)0)->member) <= \
        KB2_GPU_DRM_AMDGPU_RECORD_INFO_DEVICE_SIZE, \
        "device record extent: " #member);
KB2_AMDGPU_DEVICE_FIELDS(CHECK_DEVICE)
#undef CHECK_DEVICE

#define CHECK_HW_IP(member, wire) \
    _Static_assert(offsetof(struct drm_amdgpu_info_hw_ip, member) == \
        offsetof(gpud_drm_amdgpu_hw_ip_native_t, member), \
        "HW-IP field offset: " #member); \
    _Static_assert(sizeof(((struct drm_amdgpu_info_hw_ip *)0)->member) == \
        sizeof(((gpud_drm_amdgpu_hw_ip_native_t *)0)->member), \
        "HW-IP field size: " #member); \
    _Static_assert(KB2_GPU_DRM_AMDGPU_RECORD_INFO_HW_IP_##wire##_OFFSET + \
        sizeof(((gpud_drm_amdgpu_hw_ip_native_t *)0)->member) <= \
        KB2_GPU_DRM_AMDGPU_RECORD_INFO_HW_IP_SIZE, \
        "HW-IP record extent: " #member);
KB2_AMDGPU_HW_IP_FIELDS(CHECK_HW_IP)
#undef CHECK_HW_IP

_Static_assert(DRM_IOCTL_AMDGPU_GEM_WAIT_IDLE == GPUD_DRM_IOCTL_AMDGPU_GEM_WAIT_IDLE, "WAIT_IDLE ioctl");
_Static_assert(DRM_IOCTL_AMDGPU_GEM_METADATA == GPUD_DRM_IOCTL_AMDGPU_GEM_METADATA, "METADATA ioctl");
_Static_assert(sizeof(struct drm_amdgpu_gem_metadata) == 288, "metadata native size");
_Static_assert(offsetof(struct drm_amdgpu_gem_metadata, data.data) == 28, "metadata native data");
_Static_assert(sizeof(struct drm_amdgpu_info_firmware) == 8, "firmware size");
_Static_assert(offsetof(struct drm_amdgpu_info_firmware, feature) == 4, "firmware feature");
_Static_assert(sizeof(struct drm_amdgpu_memory_info) == 96, "memory size");
_Static_assert(offsetof(struct drm_amdgpu_memory_info, cpu_accessible_vram) == 32, "memory heap2");
_Static_assert(offsetof(struct drm_amdgpu_memory_info, gtt) == 64, "memory heap3");
_Static_assert(sizeof(struct drm_amdgpu_heap_info) == 32, "heap size");
_Static_assert(offsetof(struct drm_amdgpu_heap_info, max_allocation) == 24, "heap max allocation");
_Static_assert(sizeof(struct drm_amdgpu_info_vram_gtt) == 24, "vram size");
_Static_assert(sizeof(struct drm_amdgpu_info_video_caps) == 192, "video size");
_Static_assert(offsetof(struct drm_amdgpu_info_video_codec_info, max_level) == 16, "codec level");
_Static_assert(sizeof(struct drm_amdgpu_cs_chunk_sem) == 4, "binary syncobj chunk");
_Static_assert(sizeof(struct drm_amdgpu_cs_chunk_syncobj) == 16, "timeline chunk");
_Static_assert(offsetof(struct drm_amdgpu_cs_chunk_syncobj, point) == 8, "timeline point");
_Static_assert(sizeof(struct drm_amdgpu_cs_chunk_cp_gfx_shadow) == 32, "GFX shadow chunk");
_Static_assert(offsetof(struct drm_amdgpu_cs_chunk_cp_gfx_shadow, flags) == 24, "shadow flags");
_Static_assert(AMDGPU_HW_IP_NUM == 10, "MAX_IBS array extent");

int main(void) { return 0; }
