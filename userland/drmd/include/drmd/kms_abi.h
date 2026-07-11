#pragma once

#include <stdint.h>

enum {
    DRMD_IOCTL_GET_CAP = 0xc010640cu,
    DRMD_IOCTL_GEM_CLOSE = 0x40086409u,
    DRMD_IOCTL_PRIME_HANDLE_TO_FD = 0xc00c642du,
    DRMD_IOCTL_PRIME_FD_TO_HANDLE = 0xc00c642eu,
    DRMD_IOCTL_SET_MASTER = 0x641eu,
    DRMD_IOCTL_DROP_MASTER = 0x641fu,
    DRMD_IOCTL_MODE_GETRESOURCES = 0xc04064a0u,
    DRMD_IOCTL_MODE_GETCRTC = 0xc06864a1u,
    DRMD_IOCTL_MODE_SETCRTC = 0xc06864a2u,
    DRMD_IOCTL_MODE_GETENCODER = 0xc01464a6u,
    DRMD_IOCTL_MODE_GETCONNECTOR = 0xc05064a7u,
    DRMD_IOCTL_MODE_ADDFB = 0xc01c64aeu,
    DRMD_IOCTL_MODE_RMFB = 0xc00464afu,
    DRMD_IOCTL_MODE_PAGE_FLIP = 0xc01864b0u,
    DRMD_IOCTL_MODE_CREATE_DUMB = 0xc02064b2u,
    DRMD_IOCTL_MODE_MAP_DUMB = 0xc01064b3u,
    DRMD_IOCTL_MODE_DESTROY_DUMB = 0xc00464b4u,
    DRMD_IOCTL_MODE_GETPLANERESOURCES = 0xc01064b5u,
    DRMD_IOCTL_MODE_GETPLANE = 0xc02064b6u,
    DRMD_IOCTL_MODE_ADDFB2 = 0xc06864b8u,

    DRMD_MODE_TYPE_PREFERRED = 1u << 3,
    DRMD_MODE_TYPE_DRIVER = 1u << 6,
    DRMD_MODE_CONNECTOR_VIRTUAL = 15u,
    DRMD_MODE_ENCODER_VIRTUAL = 5u,
    DRMD_MODE_CONNECTED = 1u,
    DRMD_MODE_SUBPIXEL_UNKNOWN = 1u,
    DRMD_FORMAT_XRGB8888 = 0x34325258u,
    DRMD_MODE_PAGE_FLIP_EVENT = 1u << 0,
    DRMD_MODE_FB_MODIFIERS = 1u << 1,
    DRMD_FORMAT_MOD_LINEAR = 0u,

    DRMD_CAP_DUMB_BUFFER = 0x1u,
    DRMD_CAP_PRIME = 0x5u,
    DRMD_CAP_ADDFB2_MODIFIERS = 0x10u,
    DRMD_PRIME_CAP_IMPORT = 1u << 0,
    DRMD_PRIME_CAP_EXPORT = 1u << 1,
    DRMD_CLOEXEC = 0x80000u,
    DRMD_RDWR = 0x2u,

    DRMD_EVENT_FLIP_COMPLETE = 0x02u,

    DRMD_KMS_FB_CAPACITY = 16u,
    DRMD_KMS_CRTC_CAPACITY = 4u,
    DRMD_KMS_CONNECTOR_CAPACITY = 4u,
    DRMD_KMS_ENCODER_CAPACITY = 4u,
    DRMD_KMS_MODE_CAPACITY = 8u,
    DRMD_KMS_PLANE_CAPACITY = 4u,
    DRMD_KMS_FORMAT_CAPACITY = 4u,
};

typedef struct drmd_modeinfo {
    uint32_t clock;
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
} drmd_modeinfo_t;

typedef struct drmd_get_cap {
    uint64_t capability;
    uint64_t value;
} drmd_get_cap_t;

typedef struct drmd_prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
} drmd_prime_handle_t;

typedef struct drmd_gem_close {
    uint32_t handle;
    uint32_t pad;
} drmd_gem_close_t;

typedef struct drmd_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width;
    uint32_t max_width;
    uint32_t min_height;
    uint32_t max_height;
} drmd_mode_card_res_t;

typedef struct drmd_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    drmd_modeinfo_t mode;
} drmd_mode_crtc_t;

typedef struct drmd_mode_get_encoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
} drmd_mode_get_encoder_t;

typedef struct drmd_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width;
    uint32_t mm_height;
    uint32_t subpixel;
    uint32_t pad;
} drmd_mode_get_connector_t;

typedef struct drmd_mode_fb_cmd {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
} drmd_mode_fb_cmd_t;

typedef struct drmd_mode_fb_cmd2 {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
} drmd_mode_fb_cmd2_t;

typedef struct drmd_mode_crtc_page_flip {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t reserved;
    uint64_t user_data;
} drmd_mode_crtc_page_flip_t;

typedef struct drmd_event_vblank {
    uint32_t type;
    uint32_t length;
    uint64_t user_data;
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t sequence;
    uint32_t crtc_id;
} drmd_event_vblank_t;

typedef struct drmd_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
} drmd_mode_create_dumb_t;

typedef struct drmd_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
} drmd_mode_map_dumb_t;

typedef struct drmd_mode_destroy_dumb { uint32_t handle; } drmd_mode_destroy_dumb_t;

typedef struct drmd_mode_get_plane_res {
    uint64_t plane_id_ptr;
    uint32_t count_planes;
    uint32_t pad;
} drmd_mode_get_plane_res_t;

typedef struct drmd_mode_get_plane {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t possible_crtcs;
    uint32_t gamma_size;
    uint32_t count_format_types;
    uint64_t format_type_ptr;
} drmd_mode_get_plane_t;

typedef struct drmd_kms_resources_wire {
    drmd_mode_card_res_t value;
    uint32_t fbs[DRMD_KMS_FB_CAPACITY];
    uint32_t crtcs[DRMD_KMS_CRTC_CAPACITY];
    uint32_t connectors[DRMD_KMS_CONNECTOR_CAPACITY];
    uint32_t encoders[DRMD_KMS_ENCODER_CAPACITY];
} drmd_kms_resources_wire_t;

typedef struct drmd_kms_connector_wire {
    drmd_mode_get_connector_t value;
    uint32_t encoders[DRMD_KMS_ENCODER_CAPACITY];
    drmd_modeinfo_t modes[DRMD_KMS_MODE_CAPACITY];
} drmd_kms_connector_wire_t;

typedef struct drmd_kms_crtc_wire {
    drmd_mode_crtc_t value;
    uint32_t connectors[DRMD_KMS_CONNECTOR_CAPACITY];
} drmd_kms_crtc_wire_t;

typedef struct drmd_kms_plane_res_wire {
    drmd_mode_get_plane_res_t value;
    uint32_t planes[DRMD_KMS_PLANE_CAPACITY];
} drmd_kms_plane_res_wire_t;

typedef struct drmd_kms_plane_wire {
    drmd_mode_get_plane_t value;
    uint32_t formats[DRMD_KMS_FORMAT_CAPACITY];
} drmd_kms_plane_wire_t;

_Static_assert(sizeof(drmd_modeinfo_t) == 68, "drm modeinfo ABI");
_Static_assert(sizeof(drmd_mode_card_res_t) == 64, "drm resources ABI");
_Static_assert(sizeof(drmd_mode_crtc_t) == 104, "drm crtc ABI");
_Static_assert(sizeof(drmd_mode_get_connector_t) == 80, "drm connector ABI");
_Static_assert(sizeof(drmd_mode_fb_cmd2_t) == 104, "drm fb2 ABI");
_Static_assert(sizeof(drmd_event_vblank_t) == 32, "drm event vblank ABI");
