/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <kobox2/filesystem.h>

enum { FILED_RX_GUARD_BYTES=320, FILED_RX_HASH_BYTES=16 };
typedef struct filed_rx_patch {
    uint64_t offset;
    uint8_t value;
} filed_rx_patch_t;
typedef struct filed_rx_manifest {
    uint64_t file_offset,length,page_count,patch_count;
    uint8_t root_uuid[16],scanner_hash[32];
    uint8_t identity[KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE];
    uint8_t key[32];
    uint8_t *page_hashes;
    filed_rx_patch_t *patches;
} filed_rx_manifest_t;
struct filed_runtime;

void filed_rx_manifest_free(filed_rx_manifest_t *);
void filed_rx_manifest_key(const uint8_t root_uuid[16],
    const uint8_t identity[KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE],
    uint64_t file_offset,uint64_t length,const uint8_t scanner_hash[32],
    uint8_t key[32]);
int filed_rx_manifest_build(const uint8_t *raw,size_t length,
    uint64_t file_offset,const uint8_t root_uuid[16],
    const uint8_t identity[KB2_FILESYSTEM_SNAPSHOT_STAT_SIZE],
    const uint8_t scanner_hash[32],filed_rx_manifest_t *);
int filed_rx_manifest_apply(const filed_rx_manifest_t *,
    uint64_t target_offset,uint8_t *page_bytes,size_t page_length,
    uint64_t guarded_offset,const uint8_t *guarded,size_t guarded_length);
int filed_rx_manifest_encode(const filed_rx_manifest_t *,uint8_t **,size_t *);
int filed_rx_manifest_decode(const uint8_t *,size_t,
    const uint8_t expected_key[32],const uint8_t scanner_hash[32],
    filed_rx_manifest_t *);
int filed_manifest_storage_load(struct filed_runtime *,const uint8_t key[32],
    uint8_t **,size_t *);
int filed_manifest_storage_save(struct filed_runtime *,const uint8_t key[32],
    const uint8_t *,size_t);
void filed_manifest_storage_remove(struct filed_runtime *,const uint8_t key[32]);
