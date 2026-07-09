#pragma once

#include <stdbool.h>
#include <stdint.h>

struct filed_runtime;

int filed_cached_pread(
    struct filed_runtime *runtime,
    uint64_t backend_object,
    uint64_t offset,
    void *buffer,
    uint64_t length,
    uint64_t *out_bytes);

int filed_cached_pwrite(
    struct filed_runtime *runtime,
    uint64_t backend_object,
    uint64_t offset,
    const void *buffer,
    uint64_t length,
    uint64_t *out_bytes);

void filed_cache_configure(struct filed_runtime *runtime, uint64_t active_page_slots);
uint64_t filed_cache_dirty_count(struct filed_runtime *runtime);
bool filed_cache_object_dirty(struct filed_runtime *runtime, uint64_t backend_object);
int filed_cache_flush_object(struct filed_runtime *runtime, uint64_t backend_object);
void filed_cache_invalidate(struct filed_runtime *runtime, uint64_t backend_object);
void filed_dump_cache_metrics(const struct filed_runtime *runtime);
