#pragma once

#include <stdint.h>

enum {
    LPR_FD_TABLE_KIND_EMPTY = 0u,
    LPR_FD_TABLE_KIND_FILED = 1u,
    LPR_FD_TABLE_KIND_TTY = 2u,
    LPR_FD_TABLE_KIND_PIPE = 3u,
    LPR_FD_TABLE_KIND_EVENT = 4u,
    LPR_FD_TABLE_KIND_SOCKET = 5u,

    LPR_FD_TABLE_FD_CLOEXEC = 1u << 0,

    LPR_FD_TABLE_STATUS_NONBLOCK = 1u << 0,
    LPR_FD_TABLE_STATUS_APPEND = 1u << 1,
};

typedef struct lpr_filed_fd {
    uint8_t active;
    uint8_t offset_valid;
    uint8_t pread_active;
    uint8_t reserved1;
    uint32_t flags;
    uint64_t handle;
    uint64_t offset;
} lpr_filed_fd_t;

typedef struct lpr_pipe_fd {
    uint8_t active;
    uint8_t pipe_id;
    uint8_t readable;
    uint8_t writable;
    uint32_t flags;
    uint64_t last_wait_events;
    uint64_t last_wait_result;
} lpr_pipe_fd_t;

typedef struct lpr_event_fd {
    uint8_t active;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t flags;
    uint64_t counter;
} lpr_event_fd_t;

typedef struct lpr_tty_fd {
    uint8_t active;
    uint8_t reserved0;
    uint16_t reserved1;
    uint32_t flags;
    uint64_t handle;
} lpr_tty_fd_t;

typedef struct lpr_socket_fd {
    uint8_t active;
    uint8_t type;
    uint8_t cloexec;
    uint8_t connected;
    uint8_t connecting;
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
} lpr_socket_fd_t;

typedef enum lpr_fd_kind {
    LPR_FD_NONE = LPR_FD_TABLE_KIND_EMPTY,
    LPR_FD_FILED = LPR_FD_TABLE_KIND_FILED,
    LPR_FD_TTY = LPR_FD_TABLE_KIND_TTY,
    LPR_FD_PIPE = LPR_FD_TABLE_KIND_PIPE,
    LPR_FD_EVENTFD = LPR_FD_TABLE_KIND_EVENT,
    LPR_FD_SOCKET = LPR_FD_TABLE_KIND_SOCKET,
} lpr_fd_kind_t;

typedef union lpr_fd_payload {
    lpr_filed_fd_t filed;
    lpr_pipe_fd_t pipe;
    lpr_event_fd_t eventfd;
    lpr_tty_fd_t tty;
    lpr_socket_fd_t socket;
} lpr_fd_payload_t;

typedef struct lpr_fd_table_entry {
    uint8_t active;
    uint8_t reserved0;
    uint16_t fd_flags;
    uint32_t file_index;
} lpr_fd_entry_t;

typedef struct lpr_fd_table_object {
    uint8_t active;
    uint8_t kind;
    uint16_t reserved0;
    uint32_t refcount;
    uint32_t status_flags;
    uint32_t rights;
    uint64_t backend_id;
    uint64_t offset;
    uint64_t generation;
    lpr_fd_payload_t payload;
} lpr_fd_object_t;

typedef lpr_fd_entry_t lpr_fd_table_slot_t;
typedef lpr_fd_object_t lpr_fd_table_file_t;

typedef void (*lpr_futex_wait_fn)(volatile uint32_t *word, uint32_t expected);
typedef void (*lpr_futex_wake_fn)(volatile uint32_t *word, uint32_t count);

typedef struct lpr_lock {
    volatile uint32_t word;
    const volatile uint32_t *thread_count;
    lpr_futex_wait_fn futex_wait;
    lpr_futex_wake_fn futex_wake;
} lpr_lock_t;

typedef struct lpr_fd_table {
    lpr_fd_entry_t *slots;
    uint32_t slot_count;
    lpr_fd_object_t *files;
    uint32_t file_count;
    uint64_t generation;
    lpr_lock_t lock;
} lpr_fd_table_t;

typedef struct lpr_fd_table_install {
    uint8_t kind;
    uint16_t fd_flags;
    uint32_t status_flags;
    uint32_t rights;
    uint64_t backend_id;
    uint64_t offset;
} lpr_fd_table_install_t;

void lpr_fd_table_init(
    lpr_fd_table_t *table,
    lpr_fd_table_slot_t *slots,
    uint32_t slot_count,
    lpr_fd_table_file_t *files,
    uint32_t file_count);

void lpr_fd_table_configure_lock(
    lpr_fd_table_t *table,
    const volatile uint32_t *thread_count,
    lpr_futex_wait_fn futex_wait,
    lpr_futex_wake_fn futex_wake);
void lpr_fd_table_lock(lpr_fd_table_t *table);
void lpr_fd_table_unlock(lpr_fd_table_t *table);

int lpr_fd_table_install_at(
    lpr_fd_table_t *table,
    uint32_t fd,
    const lpr_fd_table_install_t *install);

int lpr_fd_table_alloc(
    lpr_fd_table_t *table,
    uint32_t min_fd,
    const lpr_fd_table_install_t *install,
    uint32_t *out_fd);

int lpr_fd_table_close(lpr_fd_table_t *table, uint32_t fd);

int lpr_fd_table_dup(
    lpr_fd_table_t *table,
    uint32_t old_fd,
    uint32_t min_fd,
    uint16_t new_fd_flags,
    uint32_t *out_fd);

int lpr_fd_table_dup2(
    lpr_fd_table_t *table,
    uint32_t old_fd,
    uint32_t new_fd,
    uint16_t new_fd_flags);

int lpr_fd_table_close_range(
    lpr_fd_table_t *table,
    uint32_t first,
    uint32_t last,
    uint32_t cloexec_only);

int lpr_fd_table_get_fd_flags(const lpr_fd_table_t *table, uint32_t fd, uint16_t *out_flags);
int lpr_fd_table_set_fd_flags(lpr_fd_table_t *table, uint32_t fd, uint16_t flags);
int lpr_fd_table_get_status_flags(const lpr_fd_table_t *table, uint32_t fd, uint32_t *out_flags);
int lpr_fd_table_set_status_flags(lpr_fd_table_t *table, uint32_t fd, uint32_t flags);
int lpr_fd_table_get_offset(const lpr_fd_table_t *table, uint32_t fd, uint64_t *out_offset);
int lpr_fd_table_set_offset(lpr_fd_table_t *table, uint32_t fd, uint64_t offset);

lpr_fd_object_t *lpr_fd_table_object_for_fd(lpr_fd_table_t *table, uint32_t fd);
const lpr_fd_object_t *lpr_fd_table_object_for_fd_const(const lpr_fd_table_t *table, uint32_t fd);
int lpr_fd_table_get_kind(const lpr_fd_table_t *table, uint32_t fd, uint8_t *out_kind);
int lpr_fd_table_get_refcount(const lpr_fd_table_t *table, uint32_t fd, uint32_t *out_refcount);
uint32_t lpr_fd_table_open_count(const lpr_fd_table_t *table);
uint32_t lpr_fd_table_live_file_count(const lpr_fd_table_t *table);
