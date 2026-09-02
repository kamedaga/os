#pragma once

#include <stdint.h>

enum {
    LPR_FD_OPS_NONE = 0u,
    LPR_FD_OPS_FILED = 1u,
    LPR_FD_OPS_DEVICE = 2u,
    LPR_FD_OPS_TTY = 3u,
    LPR_FD_OPS_DRM = 4u,
    LPR_FD_OPS_INPUT = 5u,
    LPR_FD_OPS_PIPE = 6u,
    LPR_FD_OPS_EVENT = 7u,
    LPR_FD_OPS_SOCKET = 8u,
    LPR_FD_OPS_EPOLL = 9u,
    LPR_FD_OPS_DMABUF = 10u,
    LPR_FD_OPS_SYNC_FILE = 11u,
    LPR_FD_OPS_COUNT = 12u,

    LPR_FD_ENTRY_CLOEXEC = 1u << 0,

    LPR_OFD_NONBLOCK = 1u << 0,
    LPR_OFD_APPEND = 1u << 1,

    LPR_FD_RIGHT_READ = 1u << 0,
    LPR_FD_RIGHT_WRITE = 1u << 1,
    LPR_FD_RIGHT_IOCTL = 1u << 2,
    LPR_FD_RIGHT_STAT = 1u << 3,
    LPR_FD_RIGHT_MMAP = 1u << 4,
    LPR_FD_RIGHT_DUP = 1u << 5,

    LPR_BACKEND_TRANSFER_LEASE = 1u << 7,

    /* Temporary D-Bus wait-path diagnostic marker. */
    LPR_SOCKET_DIAG_DBUS = 1u << 0,

    /*
     * Keep the name used to open a filed-backed OFD.  Linux exposes this
     * through /proc/self/fd/N; gdk-pixbuf 2.44 uses that link to turn the
     * FILE * passed to an image loader back into a GFile.
     *
     * Backend objects already occupy fixed 256-byte slab slots, so using the
     * otherwise empty tail does not increase the slab footprint.
     */
    LPR_FILED_OPEN_PATH_BYTES = 192u,
};

typedef uint32_t lpr_linux_fd_t;

typedef struct lpr_native_fd {
    int32_t raw;
} lpr_native_fd_t;

typedef struct lpr_filed_backend {
    uint8_t active;
    uint8_t offset_valid;
    uint8_t pread_active;
    uint8_t reserved1;
    uint32_t flags;
    uint64_t handle;
    uint64_t offset;
    lpr_native_fd_t lease_fd;
    uint32_t reserved2;
    uint64_t stat_size;
    uint64_t object_generation;
    char open_path[LPR_FILED_OPEN_PATH_BYTES];
} lpr_filed_backend_t;

typedef struct lpr_pipe_backend {
    uint8_t active;
    uint8_t pipe_id;
    uint8_t readable;
    uint8_t writable;
    uint32_t flags;
    uint64_t last_wait_events;
    uint64_t last_wait_result;
    lpr_native_fd_t native;
    uint32_t reserved0;
} lpr_pipe_backend_t;

typedef struct lpr_event_backend {
    uint8_t active;
    uint8_t subtype;
    uint8_t notify_pending;
    uint8_t reserved1;
    uint32_t flags;
    uint64_t counter;
    uint64_t deadline_ns;
    uint64_t interval_ns;
    int32_t clock_id;
    uint32_t reserved2;
    lpr_native_fd_t wait_fd;
    lpr_native_fd_t notify_fd;
} lpr_event_backend_t;

enum {
    LPR_EVENT_BACKEND_EVENTFD = 0u,
    LPR_EVENT_BACKEND_TIMERFD = 1u,
    LPR_EVENT_BACKEND_SIGNALFD = 2u,
    LPR_EVENT_BACKEND_INOTIFY = 3u,
};

typedef struct lpr_tty_backend {
    uint8_t active;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t flags;
    uint64_t handle;
    lpr_native_fd_t wait_fd;
    lpr_native_fd_t lease_fd;
} lpr_tty_backend_t;

enum {
    LPR_TTY_BACKEND_PTY_MASTER = 1u,
    LPR_TTY_BACKEND_PTY_SLAVE = 2u,
};

typedef struct lpr_device_backend {
    uint8_t active;
    uint8_t major;
    uint8_t minor;
    uint8_t reserved0;
    uint32_t flags;
} lpr_device_backend_t;

typedef struct lpr_drm_backend {
    uint8_t active;
    uint8_t node_kind;
    uint16_t reserved1;
    uint32_t flags;
    uint64_t handle;
    lpr_native_fd_t wait_fd;
    lpr_native_fd_t lease_fd;
} lpr_drm_backend_t;

enum {
    LPR_DRM_NODE_PRIMARY = 0u,
    LPR_DRM_NODE_UDMABUF = 1u,
    LPR_DRM_NODE_RENDER = 2u,
};

typedef struct lpr_input_backend {
    uint8_t active;
    uint8_t event_index;
    uint16_t reserved1;
    uint32_t flags;
    uint64_t handle;
    lpr_native_fd_t wait_fd;
    lpr_native_fd_t lease_fd;
} lpr_input_backend_t;

typedef struct lpr_dmabuf_backend {
    uint8_t active;
    uint8_t writable;
    uint16_t reserved0;
    uint32_t flags;
    uint64_t token;
    uint64_t size;
    lpr_native_fd_t native;
    lpr_native_fd_t lease_fd;
} lpr_dmabuf_backend_t;

typedef struct lpr_sync_file_backend {
    uint8_t active;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t flags;
    lpr_native_fd_t wait_fd;
    uint32_t reserved2;
} lpr_sync_file_backend_t;

typedef struct lpr_socket_backend {
    uint8_t active;
    uint8_t type;
    uint8_t readable_hint;
    uint8_t write_blocked;
    uint8_t connected;
    uint8_t connecting;
    uint8_t domain;
    uint8_t notify_ack;
    uint16_t protocol;
    uint32_t flags;
    uint32_t sndbuf;
    uint32_t rcvbuf;
    int32_t reuseaddr;
    int32_t keepalive;
    int32_t tcp_nodelay;
    int32_t sndtimeo_ms;
    int32_t rcvtimeo_ms;
    uint64_t handle;
    int32_t last_error;
    uint32_t local_addr_be;
    uint16_t local_port_be;
    uint16_t reserved0;
    uint32_t peer_addr_be;
    uint16_t peer_port_be;
    uint16_t reserved1;
    int32_t peer_pid;
    uint32_t peer_uid;
    uint32_t peer_gid;
    lpr_native_fd_t wait_fd;
    lpr_native_fd_t lease_fd;
} lpr_socket_backend_t;

typedef struct lpr_epoll_backend {
    uint8_t active;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t flags;
    uint64_t instance;
    uint64_t map_bytes;
    lpr_native_fd_t wait_fd;
    lpr_native_fd_t notify_fd;
} lpr_epoll_backend_t;

typedef struct lpr_backend_ref {
    uint32_t index;
    uint32_t generation;
} lpr_backend_ref_t;

typedef struct lpr_linux_fd_entry {
    uint8_t active;
    uint8_t reserved0;
    uint16_t fd_flags;
    uint32_t ofd_index;
    uint32_t ofd_generation;
    uint32_t effective_rights;
} lpr_fd_entry_t;

typedef struct lpr_ofd {
    uint8_t active;
    uint8_t closing;
    uint16_t access_mode;
    uint32_t refcount;
    uint32_t pin_count;
    uint32_t status_flags;
    uint32_t rights_ceiling;
    uint32_t generation;
    uint32_t reserved0;
    uint64_t offset;
    lpr_backend_ref_t backend;
} lpr_ofd_t;

typedef struct lpr_backend_record {
    uint8_t active;
    uint8_t ops_id;
    uint16_t reserved0;
    uint32_t generation;
    uint64_t state_bytes;
    void *state;
} lpr_backend_record_t;

typedef void (*lpr_futex_wait_fn)(volatile uint32_t *word, uint32_t expected);
typedef void (*lpr_futex_wake_fn)(volatile uint32_t *word, uint32_t count);

typedef struct lpr_lock {
    volatile uint32_t word;
    const volatile uint32_t *thread_count;
    lpr_futex_wait_fn futex_wait;
    lpr_futex_wake_fn futex_wake;
} lpr_lock_t;

typedef struct lpr_fd_table {
    lpr_fd_entry_t *entries;
    uint32_t entry_count;
    lpr_ofd_t *ofds;
    uint32_t ofd_count;
    lpr_backend_record_t *backends;
    uint32_t backend_count;
    uint32_t generation;
    lpr_lock_t lock;
} lpr_fd_table_t;

typedef struct lpr_fd_install {
    uint8_t ops_id;
    uint16_t fd_flags;
    uint16_t access_mode;
    uint32_t status_flags;
    uint32_t rights;
    uint64_t offset;
    void *backend_state;
    uint64_t backend_state_bytes;
} lpr_fd_install_t;

typedef struct lpr_fd_pin {
    lpr_linux_fd_t fd;
    uint16_t fd_flags;
    uint16_t access_mode;
    uint32_t effective_rights;
    uint32_t status_flags;
    uint32_t ofd_index;
    uint32_t ofd_generation;
    uint32_t backend_index;
    uint32_t backend_generation;
    uint8_t ops_id;
    uint8_t reserved0[7];
    uint64_t offset;
    void *state;
} lpr_fd_pin_t;

typedef struct lpr_fd_drop {
    uint8_t ready;
    uint8_t ops_id;
    uint16_t reserved0;
    uint32_t backend_generation;
    uint64_t state_bytes;
    void *state;
} lpr_fd_drop_t;

void lpr_fd_table_init(
    lpr_fd_table_t *table,
    lpr_fd_entry_t *entries,
    uint32_t entry_count,
    lpr_ofd_t *ofds,
    uint32_t ofd_count,
    lpr_backend_record_t *backends,
    uint32_t backend_count);
void lpr_fd_table_configure_lock(
    lpr_fd_table_t *table,
    const volatile uint32_t *thread_count,
    lpr_futex_wait_fn futex_wait,
    lpr_futex_wake_fn futex_wake);
void lpr_fd_table_lock(lpr_fd_table_t *table);
void lpr_fd_table_unlock(lpr_fd_table_t *table);

int lpr_fd_table_install_at(
    lpr_fd_table_t *table,
    lpr_linux_fd_t fd,
    const lpr_fd_install_t *install);
int lpr_fd_table_alloc(
    lpr_fd_table_t *table,
    lpr_linux_fd_t min_fd,
    const lpr_fd_install_t *install,
    lpr_linux_fd_t *out_fd);
int lpr_fd_table_alloc_batch(
    lpr_fd_table_t *table,
    lpr_linux_fd_t min_fd,
    const lpr_fd_install_t *installs,
    uint32_t install_count,
    const lpr_linux_fd_t *excluded_fds,
    uint32_t excluded_count,
    lpr_linux_fd_t *out_fds);
int lpr_fd_table_close(
    lpr_fd_table_t *table,
    lpr_linux_fd_t fd,
    lpr_fd_drop_t *out_drop);
int lpr_fd_table_dup(
    lpr_fd_table_t *table,
    lpr_linux_fd_t old_fd,
    lpr_linux_fd_t min_fd,
    uint16_t new_fd_flags,
    lpr_linux_fd_t *out_fd);
int lpr_fd_table_dup_at(
    lpr_fd_table_t *table,
    lpr_linux_fd_t old_fd,
    lpr_linux_fd_t new_fd,
    uint16_t new_fd_flags);
int lpr_fd_table_pin(
    lpr_fd_table_t *table,
    lpr_linux_fd_t fd,
    lpr_fd_pin_t *out_pin);
int lpr_fd_table_unpin(
    lpr_fd_table_t *table,
    const lpr_fd_pin_t *pin,
    lpr_fd_drop_t *out_drop);
int lpr_fd_table_seek_pinned(
    lpr_fd_table_t *table,
    const lpr_fd_pin_t *pin,
    int64_t delta,
    uint32_t whence,
    uint64_t *out_offset);

int lpr_fd_table_get_fd_flags(const lpr_fd_table_t *table, lpr_linux_fd_t fd, uint16_t *out_flags);
int lpr_fd_table_set_fd_flags(lpr_fd_table_t *table, lpr_linux_fd_t fd, uint16_t flags);
int lpr_fd_table_get_status_flags(const lpr_fd_table_t *table, lpr_linux_fd_t fd, uint32_t *out_flags);
int lpr_fd_table_set_status_flags(lpr_fd_table_t *table, lpr_linux_fd_t fd, uint32_t flags);
int lpr_fd_table_get_offset(const lpr_fd_table_t *table, lpr_linux_fd_t fd, uint64_t *out_offset);
int lpr_fd_table_set_offset(lpr_fd_table_t *table, lpr_linux_fd_t fd, uint64_t offset);
int lpr_fd_table_get_refcount(const lpr_fd_table_t *table, lpr_linux_fd_t fd, uint32_t *out_refcount);
uint32_t lpr_fd_table_open_count(const lpr_fd_table_t *table);
uint32_t lpr_fd_table_live_ofd_count(const lpr_fd_table_t *table);
