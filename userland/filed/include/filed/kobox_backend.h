#pragma once

#include <stddef.h>
#include <stdint.h>

#include "koboxd/ipc_protocol.h"

typedef struct filed_kobox_backend {
    int fs_fd;
    uint64_t root_object_id;
    uint64_t ext4_magic;
    uint64_t calls;
    uint64_t bytes_read;
} filed_kobox_backend_t;

void filed_kobox_backend_init(filed_kobox_backend_t *backend, int fs_fd);
int filed_kobox_backend_mount_root(filed_kobox_backend_t *backend);
int filed_kobox_backend_lookup(
    filed_kobox_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint64_t *out_object_id);
int filed_kobox_backend_statx(
    filed_kobox_backend_t *backend,
    uint64_t object_id,
    koboxd_wire_fs_statx_t *out_stat);
int filed_kobox_backend_pread(
    filed_kobox_backend_t *backend,
    uint64_t object_id,
    uint64_t offset,
    void *buffer,
    uint64_t length,
    uint64_t *out_bytes);
int filed_kobox_backend_getdents(
    filed_kobox_backend_t *backend,
    uint64_t dir_object_id,
    uint64_t offset,
    koboxd_wire_fs_getdents_t *out_entries);
