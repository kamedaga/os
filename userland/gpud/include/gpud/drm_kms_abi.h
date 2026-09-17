#pragma once

#include <stdint.h>

enum {
    GPUD_DRM_IOCTL_GET_CAP = 0xc010640cu,
    GPUD_DRM_IOCTL_GET_MAGIC = 0x80046402u,
    GPUD_DRM_IOCTL_AUTH_MAGIC = 0x40046411u,
    GPUD_DRM_IOCTL_SET_CLIENT_CAP = 0x4010640du,
    GPUD_DRM_IOCTL_GEM_CLOSE = 0x40086409u,
    GPUD_DRM_IOCTL_PRIME_HANDLE_TO_FD = 0xc00c642du,
    GPUD_DRM_IOCTL_PRIME_FD_TO_HANDLE = 0xc00c642eu,
    GPUD_DRM_IOCTL_SET_MASTER = 0x641eu,
    GPUD_DRM_IOCTL_DROP_MASTER = 0x641fu,
    GPUD_DRM_IOCTL_MODE_GETRESOURCES = 0xc04064a0u,
    GPUD_DRM_IOCTL_MODE_GETCRTC = 0xc06864a1u,
    GPUD_DRM_IOCTL_MODE_SETCRTC = 0xc06864a2u,
    GPUD_DRM_IOCTL_MODE_CURSOR = 0xc01c64a3u,
    GPUD_DRM_IOCTL_MODE_GETENCODER = 0xc01464a6u,
    GPUD_DRM_IOCTL_MODE_GETCONNECTOR = 0xc05064a7u,
    GPUD_DRM_IOCTL_MODE_GETPROPERTY = 0xc04064aau,
    GPUD_DRM_IOCTL_MODE_SETPROPERTY = 0xc01064abu,
    GPUD_DRM_IOCTL_MODE_GETPROPBLOB = 0xc01064acu,
    GPUD_DRM_IOCTL_MODE_ADDFB = 0xc01c64aeu,
    GPUD_DRM_IOCTL_MODE_RMFB = 0xc00464afu,
    GPUD_DRM_IOCTL_MODE_PAGE_FLIP = 0xc01864b0u,
    GPUD_DRM_IOCTL_MODE_DIRTYFB = 0xc01864b1u,
    GPUD_DRM_IOCTL_MODE_CREATE_DUMB = 0xc02064b2u,
    GPUD_DRM_IOCTL_MODE_MAP_DUMB = 0xc01064b3u,
    GPUD_DRM_IOCTL_MODE_DESTROY_DUMB = 0xc00464b4u,
    GPUD_DRM_IOCTL_MODE_GETPLANERESOURCES = 0xc01064b5u,
    GPUD_DRM_IOCTL_MODE_GETPLANE = 0xc02064b6u,
    GPUD_DRM_IOCTL_MODE_ADDFB2 = 0xc06864b8u,
    GPUD_DRM_IOCTL_MODE_OBJ_GETPROPERTIES = 0xc02064b9u,
    GPUD_DRM_IOCTL_MODE_ATOMIC = 0xc03864bcu,
    GPUD_DRM_IOCTL_MODE_CREATEPROPBLOB = 0xc01064bdu,
    GPUD_DRM_IOCTL_MODE_DESTROYPROPBLOB = 0xc00464beu,
    GPUD_DRM_IOCTL_SYNCOBJ_CREATE = 0xc00864bfu,
    GPUD_DRM_IOCTL_SYNCOBJ_DESTROY = 0xc00864c0u,
    GPUD_DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD = 0xc01064c1u,
    GPUD_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE = 0xc01064c2u,
    GPUD_DRM_IOCTL_MODE_CREATE_LEASE = 0xc01864c6u,
    GPUD_DRM_IOCTL_SYNCOBJ_TRANSFER = 0xc02064ccu,
    GPUD_DRM_IOCTL_MODE_CLOSEFB = 0xc00864d0u,
    GPUD_DRM_IOCTL_SYNCOBJ_EVENTFD = 0xc01864cfu,
    GPUD_DRM_IOCTL_WAIT_VBLANK = 0xc018643au,
    GPUD_DRM_IOCTL_CRTC_GET_SEQUENCE = 0xc018643bu,
    GPUD_DRM_IOCTL_CRTC_QUEUE_SEQUENCE = 0xc018643cu,

    GPUD_DRM_MODE_TYPE_PREFERRED = 1u << 3,
    GPUD_DRM_MODE_TYPE_DRIVER = 1u << 6,
    GPUD_DRM_MODE_CONNECTOR_VIRTUAL = 15u,
    GPUD_DRM_MODE_ENCODER_VIRTUAL = 5u,
    GPUD_DRM_MODE_CONNECTED = 1u,
    GPUD_DRM_MODE_SUBPIXEL_UNKNOWN = 1u,
    GPUD_DRM_FORMAT_XRGB8888 = 0x34325258u,
    GPUD_DRM_MODE_PAGE_FLIP_EVENT = 1u << 0,
    GPUD_DRM_MODE_PAGE_FLIP_ASYNC = 1u << 1,
    GPUD_DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE = 1u << 2,
    GPUD_DRM_MODE_PAGE_FLIP_TARGET_RELATIVE = 1u << 3,
    GPUD_DRM_MODE_PAGE_FLIP_FLAGS = GPUD_DRM_MODE_PAGE_FLIP_EVENT |
        GPUD_DRM_MODE_PAGE_FLIP_ASYNC |
        GPUD_DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE |
        GPUD_DRM_MODE_PAGE_FLIP_TARGET_RELATIVE,
    GPUD_DRM_MODE_DIRTY_ANNOTATE_COPY = 1u << 0,
    GPUD_DRM_MODE_DIRTY_ANNOTATE_FILL = 1u << 1,
    GPUD_DRM_MODE_DIRTY_FLAGS = GPUD_DRM_MODE_DIRTY_ANNOTATE_COPY |
        GPUD_DRM_MODE_DIRTY_ANNOTATE_FILL,
    GPUD_DRM_MODE_DIRTY_MAX_CLIPS = 256u,
    GPUD_DRM_MODE_CURSOR_BO = 1u << 0,
    GPUD_DRM_MODE_CURSOR_MOVE = 1u << 1,
    GPUD_DRM_MODE_FB_MODIFIERS = 1u << 1,
    GPUD_DRM_FORMAT_MOD_LINEAR = 0u,
    GPUD_DRM_MODE_ATOMIC_TEST_ONLY = 0x0100u,
    GPUD_DRM_MODE_ATOMIC_NONBLOCK = 0x0200u,
    GPUD_DRM_MODE_ATOMIC_ALLOW_MODESET = 0x0400u,
    GPUD_DRM_MODE_ATOMIC_FLAGS = GPUD_DRM_MODE_PAGE_FLIP_EVENT |
        GPUD_DRM_MODE_ATOMIC_TEST_ONLY | GPUD_DRM_MODE_ATOMIC_NONBLOCK |
        GPUD_DRM_MODE_ATOMIC_ALLOW_MODESET,

    GPUD_DRM_MODE_OBJECT_CRTC = 0xccccccccu,
    GPUD_DRM_MODE_OBJECT_CONNECTOR = 0xc0c0c0c0u,
    GPUD_DRM_MODE_OBJECT_FB = 0xfbfbfbfbu,
    GPUD_DRM_MODE_OBJECT_PLANE = 0xeeeeeeeeu,
    GPUD_DRM_MODE_OBJECT_ANY = 0u,
    GPUD_DRM_MODE_PROP_RANGE = 1u << 1,
    GPUD_DRM_MODE_PROP_IMMUTABLE = 1u << 2,
    GPUD_DRM_MODE_PROP_ENUM = 1u << 3,
    GPUD_DRM_MODE_PROP_BLOB = 1u << 4,
    GPUD_DRM_MODE_PROP_OBJECT = 1u << 6,
    GPUD_DRM_MODE_PROP_SIGNED_RANGE = 2u << 6,
    GPUD_DRM_MODE_DPMS_ON = 0u,
    GPUD_DRM_MODE_DPMS_STANDBY = 1u,
    GPUD_DRM_MODE_DPMS_SUSPEND = 2u,
    GPUD_DRM_MODE_DPMS_OFF = 3u,
    GPUD_DRM_MODE_PLANE_TYPE_OVERLAY = 0u,
    GPUD_DRM_MODE_PLANE_TYPE_PRIMARY = 1u,
    GPUD_DRM_MODE_PLANE_TYPE_CURSOR = 2u,

    GPUD_DRM_CAP_DUMB_BUFFER = 0x1u,
    GPUD_DRM_CAP_PRIME = 0x5u,
    GPUD_DRM_CAP_TIMESTAMP_MONOTONIC = 0x6u,
    GPUD_DRM_CAP_ADDFB2_MODIFIERS = 0x10u,
    GPUD_DRM_CAP_CRTC_IN_VBLANK_EVENT = 0x12u,
    GPUD_DRM_CAP_SYNCOBJ = 0x13u,
    GPUD_DRM_CAP_SYNCOBJ_TIMELINE = 0x14u,
    GPUD_DRM_CLIENT_CAP_UNIVERSAL_PLANES = 2u,
    GPUD_DRM_CLIENT_CAP_ATOMIC = 3u,
    GPUD_DRM_PRIME_CAP_IMPORT = 1u << 0,
    GPUD_DRM_PRIME_CAP_EXPORT = 1u << 1,
    GPUD_DRM_CLOEXEC = 0x80000u,
    GPUD_DRM_RDWR = 0x2u,
    GPUD_DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE = 1u << 0,
    GPUD_DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE = 1u << 0,

    GPUD_DRM_EVENT_FLIP_COMPLETE = 0x02u,
    GPUD_DRM_EVENT_VBLANK = 0x01u,
    GPUD_DRM_EVENT_CRTC_SEQUENCE = 0x03u,
    GPUD_DRM_VBLANK_RELATIVE = 0x00000001u,
    GPUD_DRM_VBLANK_HIGH_CRTC_MASK = 0x0000003eu,
    GPUD_DRM_VBLANK_EVENT = 0x04000000u,
    GPUD_DRM_VBLANK_NEXT_ON_MISS = 0x10000000u,
    GPUD_DRM_VBLANK_SECONDARY = 0x20000000u,
    GPUD_DRM_VBLANK_SIGNAL = 0x40000000u,
    GPUD_DRM_CRTC_SEQUENCE_RELATIVE = 0x00000001u,
    GPUD_DRM_CRTC_SEQUENCE_NEXT_ON_MISS = 0x00000002u,

    GPUD_DRM_KMS_FB_CAPACITY = 16u,
    GPUD_DRM_KMS_CRTC_CAPACITY = 4u,
    GPUD_DRM_KMS_CONNECTOR_CAPACITY = 4u,
    GPUD_DRM_KMS_ENCODER_CAPACITY = 4u,
    GPUD_DRM_KMS_MODE_CAPACITY = 32u,
    GPUD_DRM_KMS_PLANE_CAPACITY = 4u,
    GPUD_DRM_KMS_FORMAT_CAPACITY = 4u,
    GPUD_DRM_KMS_PROPERTY_CAPACITY = 16u,
    GPUD_DRM_KMS_PROPERTY_VALUE_CAPACITY = 8u,
    GPUD_DRM_KMS_PROPERTY_ENUM_CAPACITY = 8u,
    GPUD_DRM_KMS_PROPERTY_BLOB_BYTES = 128u,
    GPUD_DRM_PROP_NAME_BYTES = 32u,
};

enum {
    GPUD_DRM_KMS_CONNECTOR_ID = 31u,
    GPUD_DRM_KMS_ENCODER_ID = 41u,
    GPUD_DRM_KMS_CRTC_ID = 51u,
    GPUD_DRM_KMS_PLANE_ID = 61u,
    GPUD_DRM_KMS_DPMS_PROP_ID = 101u,
    GPUD_DRM_KMS_PLANE_TYPE_PROP_ID = 102u,
    GPUD_DRM_KMS_IN_FORMATS_PROP_ID = 103u,
    GPUD_DRM_KMS_CONNECTOR_CRTC_ID_PROP_ID = 104u,
    GPUD_DRM_KMS_CRTC_ACTIVE_PROP_ID = 105u,
    GPUD_DRM_KMS_CRTC_MODE_ID_PROP_ID = 106u,
    GPUD_DRM_KMS_PLANE_SRC_X_PROP_ID = 107u,
    GPUD_DRM_KMS_PLANE_SRC_Y_PROP_ID = 108u,
    GPUD_DRM_KMS_PLANE_SRC_W_PROP_ID = 109u,
    GPUD_DRM_KMS_PLANE_SRC_H_PROP_ID = 110u,
    GPUD_DRM_KMS_PLANE_CRTC_X_PROP_ID = 111u,
    GPUD_DRM_KMS_PLANE_CRTC_Y_PROP_ID = 112u,
    GPUD_DRM_KMS_PLANE_CRTC_W_PROP_ID = 113u,
    GPUD_DRM_KMS_PLANE_CRTC_H_PROP_ID = 114u,
    GPUD_DRM_KMS_PLANE_FB_ID_PROP_ID = 115u,
    GPUD_DRM_KMS_PLANE_CRTC_ID_PROP_ID = 116u,
    GPUD_DRM_KMS_PLANE_IN_FENCE_FD_PROP_ID = 117u,
    GPUD_DRM_KMS_IN_FORMATS_BLOB_ID = 201u,

    GPUD_DRM_ATOMIC_OBJECT_CAPACITY = 4u,
    GPUD_DRM_ATOMIC_PROPERTY_CAPACITY = 32u,
};

typedef struct gpud_drm_modeinfo {
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
} gpud_drm_modeinfo_t;

typedef struct gpud_drm_get_cap {
    uint64_t capability;
    uint64_t value;
} gpud_drm_get_cap_t;

typedef struct gpud_drm_set_client_cap {
    uint64_t capability;
    uint64_t value;
} gpud_drm_set_client_cap_t;

typedef struct gpud_drm_mode_property_enum {
    uint64_t value;
    char name[GPUD_DRM_PROP_NAME_BYTES];
} gpud_drm_mode_property_enum_t;

typedef struct gpud_drm_mode_get_property {
    uint64_t values_ptr;
    uint64_t enum_blob_ptr;
    uint32_t prop_id;
    uint32_t flags;
    char name[GPUD_DRM_PROP_NAME_BYTES];
    uint32_t count_values;
    uint32_t count_enum_blobs;
} gpud_drm_mode_get_property_t;

typedef struct gpud_drm_mode_connector_set_property {
    uint64_t value;
    uint32_t prop_id;
    uint32_t connector_id;
} gpud_drm_mode_connector_set_property_t;

typedef struct gpud_drm_mode_obj_get_properties {
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_props;
    uint32_t obj_id;
    uint32_t obj_type;
    uint32_t pad;
} gpud_drm_mode_obj_get_properties_t;

typedef struct gpud_drm_mode_get_blob {
    uint32_t blob_id;
    uint32_t length;
    uint64_t data;
} gpud_drm_mode_get_blob_t;

typedef struct gpud_drm_mode_create_blob_wire {
    uint32_t length;
    uint32_t blob_id;
    uint8_t data[GPUD_DRM_KMS_PROPERTY_BLOB_BYTES];
} gpud_drm_mode_create_blob_wire_t;

typedef struct gpud_drm_mode_atomic_wire {
    uint32_t flags;
    uint32_t count_objs;
    uint32_t total_props;
    uint32_t reserved0;
    uint64_t user_data;
    uint32_t objects[GPUD_DRM_ATOMIC_OBJECT_CAPACITY];
    uint32_t object_prop_counts[GPUD_DRM_ATOMIC_OBJECT_CAPACITY];
    uint32_t props[GPUD_DRM_ATOMIC_PROPERTY_CAPACITY];
    uint64_t prop_values[GPUD_DRM_ATOMIC_PROPERTY_CAPACITY];
} gpud_drm_mode_atomic_wire_t;

typedef struct gpud_drm_syncobj_create {
    uint32_t handle;
    uint32_t flags;
} gpud_drm_syncobj_create_t;

typedef struct gpud_drm_syncobj_destroy {
    uint32_t handle;
    uint32_t pad;
} gpud_drm_syncobj_destroy_t;

typedef struct gpud_drm_syncobj_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
    uint32_t pad;
} gpud_drm_syncobj_handle_t;

typedef struct gpud_drm_syncobj_transfer {
    uint32_t src_handle;
    uint32_t dst_handle;
    uint64_t src_point;
    uint64_t dst_point;
    uint32_t flags;
    uint32_t pad;
} gpud_drm_syncobj_transfer_t;

typedef struct gpud_drm_syncobj_wait {
    uint64_t handles;
    int64_t timeout_nsec;
    uint32_t count_handles;
    uint32_t flags;
    uint32_t first_signaled;
    uint32_t pad;
    uint64_t deadline_nsec;
} gpud_drm_syncobj_wait_t;

typedef struct gpud_drm_syncobj_array {
    uint64_t handles;
    uint32_t count_handles;
    uint32_t pad;
} gpud_drm_syncobj_array_t;

typedef struct gpud_drm_prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
} gpud_drm_prime_handle_t;

_Static_assert(sizeof(gpud_drm_syncobj_wait_t) == 40, "syncobj wait ABI");
_Static_assert(sizeof(gpud_drm_syncobj_array_t) == 16, "syncobj array ABI");

typedef struct gpud_drm_gem_close {
    uint32_t handle;
    uint32_t pad;
} gpud_drm_gem_close_t;

typedef struct gpud_drm_mode_card_res {
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
} gpud_drm_mode_card_res_t;

typedef struct gpud_drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    gpud_drm_modeinfo_t mode;
} gpud_drm_mode_crtc_t;

typedef struct gpud_drm_mode_get_encoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
} gpud_drm_mode_get_encoder_t;

typedef struct gpud_drm_mode_get_connector {
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
} gpud_drm_mode_get_connector_t;

typedef struct gpud_drm_mode_fb_cmd {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
} gpud_drm_mode_fb_cmd_t;

typedef struct gpud_drm_mode_fb_cmd2 {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
} gpud_drm_mode_fb_cmd2_t;

typedef struct gpud_drm_mode_crtc_page_flip {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t reserved;
    uint64_t user_data;
} gpud_drm_mode_crtc_page_flip_t;

typedef struct gpud_drm_mode_fb_dirty {
    uint32_t fb_id;
    uint32_t flags;
    uint32_t color;
    uint32_t num_clips;
    uint64_t clips_ptr;
} gpud_drm_mode_fb_dirty_t;

/* Private LPR/gpud auxiliary wire format. Linux drm_clip_rect uses u16;
 * canonical kobox2 rectangle records use one u32 for each coordinate. */
typedef struct gpud_drm_mode_rectangle {
    uint32_t x1;
    uint32_t y1;
    uint32_t x2;
    uint32_t y2;
} gpud_drm_mode_rectangle_t;

typedef struct gpud_drm_mode_cursor {
    uint32_t flags;
    uint32_t crtc_id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t handle;
} gpud_drm_mode_cursor_t;

typedef struct gpud_drm_event_vblank {
    uint32_t type;
    uint32_t length;
    uint64_t user_data;
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t sequence;
    uint32_t crtc_id;
} gpud_drm_event_vblank_t;

typedef union gpud_drm_wait_vblank {
    struct {
        uint32_t type;
        uint32_t sequence;
        uint64_t signal;
    } request;
    struct {
        uint32_t type;
        uint32_t sequence;
        int64_t tv_sec;
        int64_t tv_usec;
    } reply;
} gpud_drm_wait_vblank_t;

typedef struct gpud_drm_crtc_get_sequence {
    uint32_t crtc_id;
    uint32_t active;
    uint64_t sequence;
    int64_t sequence_ns;
} gpud_drm_crtc_get_sequence_t;

typedef struct gpud_drm_crtc_queue_sequence {
    uint32_t crtc_id;
    uint32_t flags;
    uint64_t sequence;
    uint64_t user_data;
} gpud_drm_crtc_queue_sequence_t;

typedef struct gpud_drm_event_crtc_sequence {
    uint32_t type;
    uint32_t length;
    uint64_t user_data;
    int64_t time_ns;
    uint64_t sequence;
} gpud_drm_event_crtc_sequence_t;

typedef struct gpud_drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
} gpud_drm_mode_create_dumb_t;

typedef struct gpud_drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
} gpud_drm_mode_map_dumb_t;

typedef struct gpud_drm_mode_destroy_dumb { uint32_t handle; } gpud_drm_mode_destroy_dumb_t;

typedef struct gpud_drm_mode_get_plane_res {
    uint64_t plane_id_ptr;
    uint32_t count_planes;
    uint32_t pad;
} gpud_drm_mode_get_plane_res_t;

typedef struct gpud_drm_mode_get_plane {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t possible_crtcs;
    uint32_t gamma_size;
    uint32_t count_format_types;
    uint64_t format_type_ptr;
} gpud_drm_mode_get_plane_t;

typedef struct gpud_drm_kms_resources_wire {
    gpud_drm_mode_card_res_t value;
    uint32_t fbs[GPUD_DRM_KMS_FB_CAPACITY];
    uint32_t crtcs[GPUD_DRM_KMS_CRTC_CAPACITY];
    uint32_t connectors[GPUD_DRM_KMS_CONNECTOR_CAPACITY];
    uint32_t encoders[GPUD_DRM_KMS_ENCODER_CAPACITY];
} gpud_drm_kms_resources_wire_t;

typedef struct gpud_drm_kms_connector_wire {
    gpud_drm_mode_get_connector_t value;
    uint32_t encoders[GPUD_DRM_KMS_ENCODER_CAPACITY];
    gpud_drm_modeinfo_t modes[GPUD_DRM_KMS_MODE_CAPACITY];
    uint32_t props[GPUD_DRM_KMS_PROPERTY_CAPACITY];
    uint64_t prop_values[GPUD_DRM_KMS_PROPERTY_CAPACITY];
} gpud_drm_kms_connector_wire_t;

typedef struct gpud_drm_kms_crtc_wire {
    gpud_drm_mode_crtc_t value;
    uint32_t connectors[GPUD_DRM_KMS_CONNECTOR_CAPACITY];
} gpud_drm_kms_crtc_wire_t;

typedef struct gpud_drm_kms_plane_res_wire {
    gpud_drm_mode_get_plane_res_t value;
    uint32_t planes[GPUD_DRM_KMS_PLANE_CAPACITY];
} gpud_drm_kms_plane_res_wire_t;

typedef struct gpud_drm_kms_plane_wire {
    gpud_drm_mode_get_plane_t value;
    uint32_t formats[GPUD_DRM_KMS_FORMAT_CAPACITY];
} gpud_drm_kms_plane_wire_t;

typedef struct gpud_drm_kms_object_properties_wire {
    gpud_drm_mode_obj_get_properties_t value;
    uint32_t props[GPUD_DRM_KMS_PROPERTY_CAPACITY];
    uint64_t prop_values[GPUD_DRM_KMS_PROPERTY_CAPACITY];
} gpud_drm_kms_object_properties_wire_t;

typedef struct gpud_drm_kms_property_wire {
    gpud_drm_mode_get_property_t value;
    uint64_t values[GPUD_DRM_KMS_PROPERTY_VALUE_CAPACITY];
    gpud_drm_mode_property_enum_t enums[GPUD_DRM_KMS_PROPERTY_ENUM_CAPACITY];
} gpud_drm_kms_property_wire_t;

typedef struct gpud_drm_kms_property_blob_wire {
    gpud_drm_mode_get_blob_t value;
    uint8_t data[GPUD_DRM_KMS_PROPERTY_BLOB_BYTES];
} gpud_drm_kms_property_blob_wire_t;

_Static_assert(sizeof(gpud_drm_modeinfo_t) == 68, "drm modeinfo ABI");
_Static_assert(sizeof(gpud_drm_mode_card_res_t) == 64, "drm resources ABI");
_Static_assert(sizeof(gpud_drm_mode_crtc_t) == 104, "drm crtc ABI");
_Static_assert(sizeof(gpud_drm_mode_get_connector_t) == 80, "drm connector ABI");
_Static_assert(sizeof(gpud_drm_mode_fb_cmd2_t) == 104, "drm fb2 ABI");
_Static_assert(sizeof(gpud_drm_mode_fb_dirty_t) == 24, "drm dirtyfb ABI");
_Static_assert(sizeof(gpud_drm_mode_rectangle_t) == 16,
    "DRM dirty rectangle wire ABI");
_Static_assert(sizeof(gpud_drm_event_vblank_t) == 32, "drm event vblank ABI");
_Static_assert(sizeof(gpud_drm_wait_vblank_t) == 24, "drm wait vblank ABI");
_Static_assert(sizeof(gpud_drm_crtc_get_sequence_t) == 24, "drm get sequence ABI");
_Static_assert(sizeof(gpud_drm_crtc_queue_sequence_t) == 24, "drm queue sequence ABI");
_Static_assert(sizeof(gpud_drm_event_crtc_sequence_t) == 32, "drm crtc sequence event ABI");
_Static_assert(sizeof(gpud_drm_mode_get_property_t) == 64, "drm property ABI");
_Static_assert(sizeof(gpud_drm_mode_obj_get_properties_t) == 32, "drm object properties ABI");
_Static_assert(sizeof(gpud_drm_mode_get_blob_t) == 16, "drm property blob ABI");
_Static_assert(sizeof(gpud_drm_mode_cursor_t) == 28, "drm cursor ABI");
_Static_assert(sizeof(gpud_drm_mode_create_blob_wire_t) == 136, "drm create blob wire ABI");
_Static_assert(sizeof(gpud_drm_mode_atomic_wire_t) == 440, "drm atomic wire ABI");
_Static_assert(sizeof(gpud_drm_syncobj_create_t) == 8, "drm syncobj create ABI");
_Static_assert(sizeof(gpud_drm_syncobj_destroy_t) == 8, "drm syncobj destroy ABI");
_Static_assert(sizeof(gpud_drm_syncobj_handle_t) == 16, "drm syncobj handle ABI");
_Static_assert(sizeof(gpud_drm_syncobj_transfer_t) == 32, "drm syncobj transfer ABI");
