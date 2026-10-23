/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pacha/abi.h>
#include "filed/storage.h"

struct filed_runtime;
struct filed_pager_record;

int filed_pager_start(struct filed_runtime *);
int filed_pager_create(struct filed_runtime *, uint64_t handle,
    uint64_t file_offset, uint64_t mapped_length,uint64_t rx_length,int rx,
    const filed_storage_stat_t *, uint64_t rights,
    pacha_fd_t *vmo_fd, struct filed_pager_record **record);
uint64_t filed_pager_resident_bytes(const struct filed_pager_record *);
