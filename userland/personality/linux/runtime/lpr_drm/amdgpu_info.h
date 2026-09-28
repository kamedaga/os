/* SPDX-License-Identifier: MIT */
#ifndef LPR_AMDGPU_INFO_H
#define LPR_AMDGPU_INFO_H

#include "../../../../../kobox2/protocol/include/kobox2/amdgpu_info_shape.h"

/* Keep the Linux caller's capacity separate from the canonical wire record.
 * Linux copies min(return_size, native_size), including older UAPI prefixes. */
static uint32_t lpr_amdgpu_info_copy_size(uint32_t capacity, uint32_t native)
{
    return capacity < native ? capacity : native;
}

static int lpr_amdgpu_info_prepare(gpud_drm_amdgpu_info_t *info)
{
    if (!info->return_pointer || !info->return_size || info->return_size > 1048576u)
        return -LPR_LINUX_EINVAL;
    struct kb2_amdgpu_info_shape shape;
    if (!kb2_amdgpu_info_shape(info->query, info->selector, &shape))
        return -LPR_LINUX_EINVAL;
    info->return_size = shape.bytes;
    return 0;
}
#endif
