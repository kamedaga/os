/* SPDX-License-Identifier: MIT */
#ifndef PACHA_GPUD_DRM_AMDGPU_ABI_H
#define PACHA_GPUD_DRM_AMDGPU_ABI_H


#define GPUD_DRM_IOCTL_AMDGPU_GEM_METADATA UINT32_C(0xc1206446)
#include <stdint.h>
enum { GPUD_DRM_AMDGPU_CS_PARTS = 64 };
typedef struct gpud_drm_amdgpu_cs_part {
    uint32_t record, count, offset, bytes;
} gpud_drm_amdgpu_cs_part_t;

/* Linux x86-64 DRM UAPI, kept separate from the Kobox2 command schema.
 * These structures describe only the LPR-facing ioctl argument. Pointers
 * must be replaced by bounded auxiliary spans before reaching gpud. */
#define GPUD_DRM_IOCTL_AMDGPU_INFO UINT32_C(0x40206445)
#define GPUD_DRM_IOCTL_AMDGPU_GEM_CREATE UINT32_C(0xc0206440)
#define GPUD_DRM_IOCTL_AMDGPU_GEM_MMAP UINT32_C(0xc0086441)
#define GPUD_DRM_IOCTL_AMDGPU_CTX UINT32_C(0xc0106442)
#define GPUD_DRM_IOCTL_AMDGPU_BO_LIST UINT32_C(0xc0186443)
#define GPUD_DRM_IOCTL_AMDGPU_CS UINT32_C(0xc0186444)

#define GPUD_DRM_IOCTL_AMDGPU_GEM_WAIT_IDLE UINT32_C(0xc0106447)
#define GPUD_DRM_IOCTL_AMDGPU_GEM_VA UINT32_C(0x40406448)
/* libdrm uses drmCommandWriteRead for GEM_VA; Linux accepts that direction
 * variant while retaining the driver's input-only argument semantics. */
#define GPUD_DRM_IOCTL_AMDGPU_GEM_VA_RDWR UINT32_C(0xc0406448)
#define GPUD_DRM_IOCTL_AMDGPU_WAIT_CS UINT32_C(0xc0206449)
#define GPUD_DRM_IOCTL_AMDGPU_VM UINT32_C(0xc0086453)

typedef struct gpud_drm_amdgpu_info {
    uint64_t return_pointer;
    uint32_t return_size;
    uint32_t query;
    uint32_t selector[4];
} gpud_drm_amdgpu_info_t;

typedef struct gpud_drm_amdgpu_cs {
    uint32_t ctx_id;
    uint32_t bo_list_handle;
    uint32_t num_chunks;
    uint32_t flags;
    uint64_t chunks;
} gpud_drm_amdgpu_cs_t;

typedef struct gpud_drm_amdgpu_cs_chunk {
    uint32_t chunk_id;
    uint32_t length_dw;
    uint64_t chunk_data;
} gpud_drm_amdgpu_cs_chunk_t;

typedef struct gpud_drm_amdgpu_cs_ib {
    uint32_t pad;
    uint32_t flags;
    uint64_t va_start;
    uint32_t ib_bytes;
    uint32_t ip_type;
    uint32_t ip_instance;
    uint32_t ring;
} gpud_drm_amdgpu_cs_ib_t;

typedef struct gpud_drm_amdgpu_bo_list_in {
    uint32_t operation;
    uint32_t list_handle;
    uint32_t bo_number;
    uint32_t bo_info_size;
    uint64_t bo_info_ptr;
} gpud_drm_amdgpu_bo_list_in_t;

typedef struct gpud_drm_amdgpu_bo_entry {
    uint32_t handle;
    uint32_t priority;
} gpud_drm_amdgpu_bo_entry_t;

/* Private LPR -> gpud plan; every chunk names a bounded canonical span.
 * User pointers and the ignored native BO-list fields never cross IPC. */
typedef struct gpud_drm_amdgpu_cs_wire {
    uint32_t ctx_id, bo_list_handle, chunk_count, flags;
    gpud_drm_amdgpu_cs_part_t parts[GPUD_DRM_AMDGPU_CS_PARTS];
} gpud_drm_amdgpu_cs_wire_t;

/* Native x86-64 Linux INFO structures. The Kobox2 record deliberately
 * excludes C alignment holes, so LPR unpacks field-by-field. */
typedef struct gpud_drm_amdgpu_hw_ip_native {
    uint32_t hw_ip_version_major, hw_ip_version_minor;
    uint64_t capabilities_flags;
    uint32_t ib_start_alignment, ib_size_alignment, available_rings;
    uint32_t ip_discovery_version, userq_num_slots;
} gpud_drm_amdgpu_hw_ip_native_t;

typedef struct gpud_drm_amdgpu_device_native {
    uint32_t device_id, chip_rev, external_rev, pci_rev, family;
    uint32_t num_shader_engines, num_shader_arrays_per_engine;
    uint32_t gpu_counter_freq;
    uint64_t max_engine_clock, max_memory_clock;
    uint32_t cu_active_number, cu_ao_mask, cu_bitmap[4][4];
    uint32_t enabled_rb_pipes_mask, num_rb_pipes, num_hw_gfx_contexts;
    uint32_t pcie_gen;
    uint64_t ids_flags, virtual_address_offset, virtual_address_max;
    uint32_t virtual_address_alignment, pte_fragment_size, gart_page_size;
    uint32_t ce_ram_size, vram_type, vram_bit_width, vce_harvest_config;
    uint32_t gc_double_offchip_lds_buf;
    uint64_t prim_buf_gpu_addr, pos_buf_gpu_addr, cntl_sb_buf_gpu_addr;
    uint64_t param_buf_gpu_addr;
    uint32_t prim_buf_size, pos_buf_size, cntl_sb_buf_size, param_buf_size;
    uint32_t wave_front_size, num_shader_visible_vgprs, num_cu_per_sh;
    uint32_t num_tcc_blocks, gs_vgt_table_depth, gs_prim_buffer_depth;
    uint32_t max_gs_waves_per_vgt, pcie_num_lanes, cu_ao_bitmap[4][4];
    uint64_t high_va_offset, high_va_max;
    uint32_t pa_sc_tile_steering_override;
    uint64_t tcc_disabled_mask, min_engine_clock, min_memory_clock;
    uint32_t tcp_cache_size, num_sqc_per_wgp, sqc_data_cache_size;
    uint32_t sqc_inst_cache_size, gl1c_cache_size, gl2c_cache_size;
    uint64_t mall_size;
    uint32_t enabled_rb_pipes_mask_hi, shadow_size, shadow_alignment;
    uint32_t csa_size, csa_alignment, userq_ip_mask, pad;
} gpud_drm_amdgpu_device_native_t;

_Static_assert(sizeof(gpud_drm_amdgpu_info_t) == 32,
    "AMDGPU_INFO ioctl layout");
_Static_assert(sizeof(gpud_drm_amdgpu_cs_t) == 24,
    "AMDGPU_CS ioctl layout");
_Static_assert(sizeof(gpud_drm_amdgpu_cs_chunk_t) == 16,
    "AMDGPU_CS chunk layout");
_Static_assert(sizeof(gpud_drm_amdgpu_cs_ib_t) == 32,
    "AMDGPU_CS IB layout");
_Static_assert(sizeof(gpud_drm_amdgpu_bo_list_in_t) == 24,
    "AMDGPU BO list layout");
_Static_assert(sizeof(gpud_drm_amdgpu_bo_entry_t) == 8,
    "AMDGPU BO entry layout");
_Static_assert(sizeof(gpud_drm_amdgpu_cs_wire_t) == 16 + 16 * GPUD_DRM_AMDGPU_CS_PARTS,
    "AMDGPU CS wire layout");
_Static_assert(sizeof(gpud_drm_amdgpu_hw_ip_native_t) == 40,
    "AMDGPU HW-IP native layout");
_Static_assert(sizeof(gpud_drm_amdgpu_device_native_t) == 448,
    "AMDGPU device-info native layout");

#endif
