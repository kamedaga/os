/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <kobox2/gpu_drm_i915_layout.h>

#define GPUD_DRM_IOCTL_I915_GEM_CREATE UINT32_C(0xc010645b)
#define GPUD_DRM_IOCTL_I915_GEM_WAIT UINT32_C(0xc010646c)
#define GPUD_DRM_IOCTL_I915_GETPARAM UINT32_C(0xc0106446)
#define GPUD_DRM_IOCTL_I915_GET_APERTURE UINT32_C(0x80106463)
#define GPUD_DRM_IOCTL_I915_CONTEXT_GETPARAM UINT32_C(0xc0186474)

/* Only scalar context parameters cross this command. Pointer-bearing SSEU
 * and ENGINES parameters require separate typed marshaling. */
static inline int gpud_i915_context_scalar_parameter(uint64_t parameter)
{ return parameter == 3 || parameter == 9; }

#define GPUD_DRM_IOCTL_I915_QUERY UINT32_C(0xc0106479)

typedef struct gpud_drm_i915_query {
    uint32_t count, flags;
    uint64_t items_pointer;
} gpud_drm_i915_query_t;

typedef struct gpud_drm_i915_query_item {
    uint64_t query_id;
    int32_t length;
    uint32_t flags;
    uint64_t data_pointer;
} gpud_drm_i915_query_item_t;

typedef struct gpud_drm_i915_query_wire {
    uint32_t count, flags;
    uint64_t blob_bytes;
} gpud_drm_i915_query_wire_t;

typedef struct gpud_drm_i915_query_item_wire {
    uint64_t query_id;
    int32_t length;
    uint32_t flags;
    uint64_t data_offset;
} gpud_drm_i915_query_item_wire_t;

_Static_assert(sizeof(gpud_drm_i915_query_t) == 16, "i915 query UAPI");
_Static_assert(sizeof(gpud_drm_i915_query_wire_t) == 16, "i915 query wire");
_Static_assert(sizeof(gpud_drm_i915_query_item_t) == 24, "i915 query item UAPI");
_Static_assert(sizeof(gpud_drm_i915_query_item_wire_t) == 24, "i915 query item wire");

/* Linux UAPI on x86-64. The pointer remains in LPR and is never sent to gpud. */
typedef struct gpud_drm_i915_getparam {
    int32_t parameter;
    uint32_t padding;
    uint64_t value_pointer;
} gpud_drm_i915_getparam_t;

/* Pointer-free native request/reply. LPR initializes value to zero. */
typedef struct gpud_drm_i915_getparam_wire {
    int32_t parameter;
    int32_t value;
} gpud_drm_i915_getparam_wire_t;

_Static_assert(sizeof(gpud_drm_i915_getparam_t) == 16, "i915 getparam UAPI");
_Static_assert(sizeof(gpud_drm_i915_getparam_wire_t) == 8, "i915 getparam wire");

#define GPUD_DRM_IOCTL_I915_CONTEXT_CREATE UINT32_C(0xc008646d)
#define GPUD_DRM_IOCTL_I915_CONTEXT_CREATE_EXT UINT32_C(0xc010646d)
#define GPUD_DRM_IOCTL_I915_CONTEXT_SETPARAM UINT32_C(0xc0186475)
#define GPUD_DRM_IOCTL_I915_CONTEXT_DESTROY UINT32_C(0x4008646e)

typedef struct gpud_i915_user_extension {
    uint64_t next;
    uint32_t name, flags, reserved[4];
} gpud_i915_user_extension_t;
typedef struct gpud_i915_context_param {
    uint32_t context_id, size;
    uint64_t parameter, value;
} gpud_i915_context_param_t;
typedef struct gpud_i915_context_setparam_ext {
    gpud_i915_user_extension_t base;
    gpud_i915_context_param_t param;
} gpud_i915_context_setparam_ext_t;
typedef struct gpud_i915_context_create {
    uint32_t context_id, flags;
    uint64_t extensions;
} gpud_i915_context_create_t;
typedef struct gpud_i915_context_param_wire {
    uint64_t parameter, value;
    uint32_t size, engine_first, engine_count, reserved;
} gpud_i915_context_param_wire_t;
_Static_assert(sizeof(gpud_i915_user_extension_t)==32, "i915 extension UAPI");
_Static_assert(sizeof(gpud_i915_context_param_t)==24, "i915 context param UAPI");
_Static_assert(sizeof(gpud_i915_context_setparam_ext_t)==56, "i915 setparam extension UAPI");
_Static_assert(sizeof(gpud_i915_context_create_t)==16, "i915 context create UAPI");
_Static_assert(sizeof(gpud_i915_context_param_wire_t)==32, "i915 pointer-free context param");

#define GPUD_DRM_IOCTL_I915_EXECBUFFER UINT32_C(0x40406469)
#define GPUD_DRM_IOCTL_I915_EXECBUFFER_WR UINT32_C(0xc0406469)
typedef struct gpud_i915_execbuffer {
    uint64_t objects_pointer;
    uint32_t object_count,batch_start,batch_length,dr1,dr4,fence_count;
    uint64_t fences_pointer,flags,context_id,sync_file_fds;
} gpud_i915_execbuffer_t;
typedef struct gpud_i915_exec_object {
    uint32_t handle,relocation_count;
    uint64_t relocations_pointer,alignment,offset,flags,pad_to_size,reserved;
} gpud_i915_exec_object_t;
typedef struct gpud_i915_exec_object_wire {
    uint32_t handle,reserved;
    uint64_t alignment,offset,flags,pad_to_size;
} gpud_i915_exec_object_wire_t;
_Static_assert(sizeof(gpud_i915_execbuffer_t)==64,"i915 execbuffer UAPI");
_Static_assert(sizeof(gpud_i915_exec_object_t)==56,"i915 exec object UAPI");
_Static_assert(sizeof(gpud_i915_exec_object_wire_t)==40,"i915 pointer-free exec object");

#define GPUD_DRM_IOCTL_I915_GEM_MMAP_OFFSET UINT32_C(0xc0206464)
typedef struct gpud_drm_i915_mmap_offset {
    uint32_t handle, pad;
    uint64_t offset, flags, extensions;
} gpud_drm_i915_mmap_offset_t;
_Static_assert(sizeof(gpud_drm_i915_mmap_offset_t)==32,"i915 mmap offset UAPI");

#define GPUD_DRM_IOCTL_I915_GEM_CREATE_EXT UINT32_C(0xc018647c)
typedef struct gpud_i915_create_ext { uint64_t size; uint32_t handle,flags; uint64_t extensions; } gpud_i915_create_ext_t;
