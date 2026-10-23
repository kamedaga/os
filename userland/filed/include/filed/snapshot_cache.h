/* SPDX-License-Identifier: MIT */
#pragma once
#include "filed/storage.h"
#include <pacha/abi.h>

/* Owned by filed's single dispatcher. No backend callback may reenter it.
 * A hit returns an independently owned, attenuated FD; dropping an entry
 * never consumes a caller's FD or a live VMA reference. */
struct filed_snapshot_entry;
struct filed_snapshot_inode;
struct filed_pager_record;
enum { FILED_SNAPSHOT_BUCKETS = 1024, FILED_SNAPSHOT_MAX_ENTRIES = 4096 };
typedef struct filed_snapshot_cache {
    struct filed_snapshot_entry *first, *last;
    struct filed_snapshot_inode *buckets[FILED_SNAPSHOT_BUCKETS];
    uint64_t bytes, limit, generation, hits, misses, bypasses, evictions;
    size_t count;
} filed_snapshot_cache_t;
void filed_snapshot_cache_clear(filed_snapshot_cache_t *);
void filed_snapshot_cache_reserve_fds(filed_snapshot_cache_t *);
void filed_snapshot_cache_observe(filed_snapshot_cache_t *, uint64_t,
    const filed_storage_stat_t *);
/* Caller supplies rights authorized for this request, including its current
 * EXEC/noexec check. Content identity never confers execution authority. */
int filed_snapshot_cache_get(filed_snapshot_cache_t *, const filed_storage_stat_t *,
    uint64_t offset, uint64_t length, uint64_t rights, pacha_fd_t *, uint64_t *);
int filed_snapshot_cache_get_variant(filed_snapshot_cache_t *,
    const filed_storage_stat_t *,uint64_t offset,uint64_t length,uint64_t rights,
    int rx,pacha_fd_t *,uint64_t *,int *pager);
void filed_snapshot_cache_put(filed_snapshot_cache_t *, const filed_storage_stat_t *,
    uint64_t offset, uint64_t length, pacha_fd_t);
void filed_snapshot_cache_put_pager(filed_snapshot_cache_t *,
    const filed_storage_stat_t *,uint64_t offset,uint64_t length,pacha_fd_t,
    struct filed_pager_record *,int rx);
void filed_snapshot_cache_put_variant(filed_snapshot_cache_t *,
    const filed_storage_stat_t *,uint64_t offset,uint64_t length,pacha_fd_t,
    int rx);
uint64_t filed_snapshot_cache_resident_bytes(filed_snapshot_cache_t *);
