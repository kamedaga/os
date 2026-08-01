#pragma once

#include <stdint.h>

enum {
    DRMD_IOCTL_VIRTGPU_MAP = 0xc0106441u,
    DRMD_IOCTL_VIRTGPU_EXECBUFFER = 0xc0406442u,
    DRMD_IOCTL_VIRTGPU_GETPARAM = 0xc0106443u,
    DRMD_IOCTL_VIRTGPU_RESOURCE_CREATE = 0xc0386444u,
    DRMD_IOCTL_VIRTGPU_RESOURCE_INFO = 0xc0106445u,
    DRMD_IOCTL_VIRTGPU_TRANSFER_FROM_HOST = 0xc02c6446u,
    DRMD_IOCTL_VIRTGPU_TRANSFER_TO_HOST = 0xc02c6447u,
    DRMD_IOCTL_VIRTGPU_WAIT = 0xc0086448u,
    DRMD_IOCTL_VIRTGPU_GET_CAPS = 0xc0186449u,
    DRMD_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB = 0xc030644au,
    DRMD_IOCTL_VIRTGPU_CONTEXT_INIT = 0xc010644bu,

    DRMD_VIRTGPU_PARAM_3D_FEATURES = 1u,
    DRMD_VIRTGPU_PARAM_CAPSET_QUERY_FIX = 2u,
    DRMD_VIRTGPU_PARAM_RESOURCE_BLOB = 3u,
    DRMD_VIRTGPU_PARAM_HOST_VISIBLE = 4u,
    DRMD_VIRTGPU_PARAM_CROSS_DEVICE = 5u,
    DRMD_VIRTGPU_PARAM_CONTEXT_INIT = 6u,
    DRMD_VIRTGPU_PARAM_SUPPORTED_CAPSET_IDS = 7u,
    DRMD_VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME = 8u,

    DRMD_VIRTGPU_EXECBUF_FENCE_FD_IN = 1u << 0,
    DRMD_VIRTGPU_EXECBUF_FENCE_FD_OUT = 1u << 1,
    DRMD_VIRTGPU_EXECBUF_RING_IDX = 1u << 2,
    DRMD_VIRTGPU_WAIT_NOWAIT = 1u,
};

typedef struct drmd_virtgpu_map {
    uint64_t offset;
    uint32_t handle;
    uint32_t pad;
} drmd_virtgpu_map_t;

typedef struct drmd_virtgpu_execbuffer {
    uint32_t flags;
    uint32_t size;
    uint64_t command;
    uint64_t bo_handles;
    uint32_t num_bo_handles;
    int32_t fence_fd;
    uint32_t ring_idx;
    uint32_t syncobj_stride;
    uint32_t num_in_syncobjs;
    uint32_t num_out_syncobjs;
    uint64_t in_syncobjs;
    uint64_t out_syncobjs;
} drmd_virtgpu_execbuffer_t;

typedef struct drmd_virtgpu_getparam {
    uint64_t param;
    uint64_t value;
} drmd_virtgpu_getparam_t;

typedef struct drmd_virtgpu_resource_create {
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t bo_handle;
    uint32_t res_handle;
    uint32_t size;
    uint32_t stride;
} drmd_virtgpu_resource_create_t;

typedef struct drmd_virtgpu_resource_info {
    uint32_t bo_handle;
    uint32_t res_handle;
    uint32_t size;
    uint32_t blob_mem;
} drmd_virtgpu_resource_info_t;

typedef struct drmd_virtgpu_3d_box {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t w;
    uint32_t h;
    uint32_t d;
} drmd_virtgpu_3d_box_t;

typedef struct drmd_virtgpu_3d_transfer {
    uint32_t bo_handle;
    drmd_virtgpu_3d_box_t box;
    uint32_t level;
    uint32_t offset;
    uint32_t stride;
    uint32_t layer_stride;
} drmd_virtgpu_3d_transfer_t;

typedef struct drmd_virtgpu_3d_wait {
    uint32_t handle;
    uint32_t flags;
} drmd_virtgpu_3d_wait_t;

typedef struct drmd_virtgpu_get_caps {
    uint32_t cap_set_id;
    uint32_t cap_set_ver;
    uint64_t addr;
    uint32_t size;
    uint32_t pad;
} drmd_virtgpu_get_caps_t;

typedef struct drmd_virtgpu_resource_create_blob {
    uint32_t blob_mem;
    uint32_t blob_flags;
    uint32_t bo_handle;
    uint32_t res_handle;
    uint64_t size;
    uint32_t pad;
    uint32_t cmd_size;
    uint64_t cmd;
    uint64_t blob_id;
} drmd_virtgpu_resource_create_blob_t;

typedef struct drmd_virtgpu_context_set_param {
    uint64_t param;
    uint64_t value;
} drmd_virtgpu_context_set_param_t;

typedef struct drmd_virtgpu_context_init {
    uint32_t num_params;
    uint32_t pad;
    uint64_t ctx_set_params;
} drmd_virtgpu_context_init_t;

_Static_assert(sizeof(drmd_virtgpu_map_t) == 16, "virtgpu map ABI");
_Static_assert(sizeof(drmd_virtgpu_execbuffer_t) == 64, "virtgpu execbuffer ABI");
_Static_assert(sizeof(drmd_virtgpu_getparam_t) == 16, "virtgpu getparam ABI");
_Static_assert(sizeof(drmd_virtgpu_resource_create_t) == 56, "virtgpu resource create ABI");
_Static_assert(sizeof(drmd_virtgpu_resource_info_t) == 16, "virtgpu resource info ABI");
_Static_assert(sizeof(drmd_virtgpu_3d_transfer_t) == 44, "virtgpu transfer ABI");
_Static_assert(sizeof(drmd_virtgpu_3d_wait_t) == 8, "virtgpu wait ABI");
_Static_assert(sizeof(drmd_virtgpu_get_caps_t) == 24, "virtgpu caps ABI");
_Static_assert(sizeof(drmd_virtgpu_resource_create_blob_t) == 48, "virtgpu blob ABI");
_Static_assert(sizeof(drmd_virtgpu_context_init_t) == 16, "virtgpu context ABI");
