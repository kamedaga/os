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

typedef struct lpr_fd_table_slot {
    uint8_t active;
    uint8_t reserved0;
    uint16_t fd_flags;
    uint32_t file_index;
} lpr_fd_table_slot_t;

typedef struct lpr_fd_table_file {
    uint8_t active;
    uint8_t kind;
    uint16_t reserved0;
    uint32_t refcount;
    uint32_t status_flags;
    uint32_t rights;
    uint64_t backend_id;
    uint64_t offset;
} lpr_fd_table_file_t;

typedef struct lpr_fd_table {
    lpr_fd_table_slot_t *slots;
    uint32_t slot_count;
    lpr_fd_table_file_t *files;
    uint32_t file_count;
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

uint32_t lpr_fd_table_open_count(const lpr_fd_table_t *table);
uint32_t lpr_fd_table_live_file_count(const lpr_fd_table_t *table);

