#pragma once

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

void filed_page_cache_configure(uint64_t active_slots);
int filed_page_cache_flush_object(struct filed_runtime *runtime, uint64_t backend_object);
void filed_page_cache_invalidate_object(struct filed_runtime *runtime, uint64_t backend_object);
void filed_dump_cache_metrics(const struct filed_runtime *runtime);
