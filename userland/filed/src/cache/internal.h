#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "filed/payload.h"
#include "filed/runtime.h"
#include "koboxd/storage_protocol.h"

enum {
    FILED_PAGE_CACHE_BYTES = 16384,
    FILED_PAGE_CACHE_SLOTS = 64,
    FILED_DIR_CACHE_SLOTS = 32,
    FILED_NEGATIVE_LOOKUP_CACHE_SLOTS = 64,
    /* Allocation granularity, not a limit on live shared mappings. */
    FILED_FILE_VMO_BANK_ENTRIES = 32,
    /* Keep the GTK/WebKit library working set reusable across processes.
     * A 24-image limit evicts live-mapped libraries and creates physical
     * duplicates on the next process load. The byte budget stays unchanged;
     * shared I/O metadata grows independently. The unrelated unused-vnode LRU
     * stays small to bound retained inactive metadata, independently of the
     * dynamically allocated vnode registry. */
    FILED_FILE_VMO_PINNED_SNAPSHOT_LIMIT = 96,
    FILED_FILE_VMO_MAX_BYTES = 256u * 1024u * 1024u,
    FILED_FILE_VMO_CACHE_TOTAL_BYTES = 512u * 1024u * 1024u,
    FILED_CACHE_OBJECT_SLOTS =
        FILED_PAGE_CACHE_SLOTS +
        FILED_DIR_CACHE_SLOTS +
        FILED_NEGATIVE_LOOKUP_CACHE_SLOTS +
        128, /* Evictable attachment hints, not the VMO registry. */
};

enum {
    FILED_CACHE_ATTACHMENT_PAGE = 1u << 0,
    FILED_CACHE_ATTACHMENT_DIRENT = 1u << 1,
    FILED_CACHE_ATTACHMENT_NEGATIVE = 1u << 2,
    FILED_CACHE_ATTACHMENT_VMO = 1u << 3,
};

typedef struct filed_cache_object_entry {
    bool valid;
    uint32_t attachment_mask;
    uint64_t backend_object;
    uint64_t last_used;
} filed_cache_object_entry_t;

typedef struct filed_page_cache_slot {
    bool valid;
    bool dirty;
    uint64_t backend_object;
    uint64_t page_index;
    uint64_t last_used;
    uint32_t bytes;
    uint32_t valid_start;
    uint32_t dirty_start;
    uint32_t dirty_end;
    uint8_t data[FILED_PAGE_CACHE_BYTES];
} filed_page_cache_slot_t;

typedef struct filed_page_cache {
    filed_page_cache_slot_t slots[FILED_PAGE_CACHE_SLOTS];
    uint64_t clock;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t direct_reads;
    uint64_t dirty_writes;
    uint64_t flushes;
    uint64_t flush_errors;
    uint64_t active_slots;
    bool configured;
} filed_page_cache_t;

typedef struct filed_dir_cache_slot {
    bool valid;
    uint64_t backend_object;
    uint64_t offset;
    uint64_t last_used;
    storage_getdents_request_t entries;
} filed_dir_cache_slot_t;

typedef struct filed_dir_cache {
    filed_dir_cache_slot_t slots[FILED_DIR_CACHE_SLOTS];
    uint64_t clock;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
} filed_dir_cache_t;

typedef struct filed_negative_lookup_cache_slot {
    bool valid;
    uint64_t parent_backend_object;
    filed_generation_t parent_dir_generation;
    uint64_t last_used;
    int64_t status;
    char name[FILED_NAME_BYTES];
} filed_negative_lookup_cache_slot_t;

typedef struct filed_negative_lookup_cache {
    filed_negative_lookup_cache_slot_t slots[FILED_NEGATIVE_LOOKUP_CACHE_SLOTS];
    uint64_t clock;
    uint64_t hits;
    uint64_t misses;
    uint64_t stores;
    uint64_t evictions;
} filed_negative_lookup_cache_t;

typedef struct filed_file_vmo_cache_entry {
    uint8_t active;
    uint8_t shared;
    uint8_t writable_lent;
    uint8_t dirty;
    uint8_t retiring;
    uint8_t zero_on_demand;
    int vmo_fd;
    uint64_t backend_object;
    uint64_t object_generation;
    uint64_t file_offset;
    uint64_t length;
    /* A successful grow can outlive a failed local remap. Charge the backing,
     * not just the still usable prefix mapping, against the cache budget. */
    uint64_t capacity;
    uint64_t logical_size;
    uint64_t clock;
    void *mapped;
    void *retired_mapping;
    uint64_t retired_length;
} filed_file_vmo_cache_entry_t;

static inline uint64_t filed_file_vmo_cache_bytes(const filed_file_vmo_cache_entry_t *entry)
{
    return entry->capacity > entry->length ? entry->capacity : entry->length;
}

typedef struct filed_file_vmo_bank {
    struct filed_file_vmo_bank *next;
    filed_file_vmo_cache_entry_t entries[FILED_FILE_VMO_BANK_ENTRIES];
} filed_file_vmo_bank_t;

typedef struct filed_file_vmo_cache {
    filed_file_vmo_bank_t *banks;
    uint64_t hits;
    uint64_t misses;
    uint64_t stores;
    uint64_t evictions;
    /* Aggregate evictions include invalidation; track successful byte-budget
     * victims separately before attributing browser reload cost to pressure. */
    uint64_t byte_budget_evictions;
    uint64_t clock;
} filed_file_vmo_cache_t;

/* Stable entry addresses, including while another bank is allocated. */
typedef struct filed_file_vmo_iterator {
    filed_file_vmo_bank_t *bank;
    unsigned index;
    filed_file_vmo_cache_entry_t *entry;
} filed_file_vmo_iterator_t;

static inline filed_file_vmo_iterator_t filed_file_vmo_cache_iterate(
    filed_file_vmo_cache_t *cache)
{
    return (filed_file_vmo_iterator_t){
        .bank = cache->banks,
        .entry = cache->banks ? &cache->banks->entries[0] : NULL,
    };
}

static inline void filed_file_vmo_cache_next(filed_file_vmo_iterator_t *it)
{
    if (++it->index == FILED_FILE_VMO_BANK_ENTRIES) {
        it->bank = it->bank->next;
        it->index = 0;
    }
    it->entry = it->bank ? &it->bank->entries[it->index] : NULL;
}

/* Only between operations: no caller may retain an inactive entry pointer. */
void filed_file_vmo_cache_trim_empty_banks(filed_runtime_t *runtime);
uint32_t filed_file_vmo_cache_reclaim_idle_shared(filed_runtime_t *runtime);

typedef struct filed_cache {
    filed_cache_object_entry_t objects[FILED_CACHE_OBJECT_SLOTS];
    uint64_t object_clock;
    filed_page_cache_t page;
    filed_dir_cache_t dir;
    filed_negative_lookup_cache_t negative;
    filed_file_vmo_cache_t file_vmo;
} filed_cache_t;

void filed_cache_note_attachment(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint32_t attachment);

int filed_cached_pwrite_ex(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    const void *buffer,
    uint64_t length,
    uint64_t *out_bytes,
    bool allow_extend_slot);

int filed_dir_cache_get(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    storage_getdents_request_t *out_entries);
void filed_dir_cache_store(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    const storage_getdents_request_t *entries);

bool filed_negative_lookup_cache_get(
    filed_runtime_t *runtime,
    uint64_t parent_backend_object,
    filed_generation_t parent_dir_generation,
    const char *name,
    int64_t *out_status);
void filed_negative_lookup_cache_store(
    filed_runtime_t *runtime,
    uint64_t parent_backend_object,
    filed_generation_t parent_dir_generation,
    const char *name,
    int64_t status);

filed_file_vmo_cache_entry_t *filed_file_vmo_cache_lookup(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t object_generation,
    uint64_t file_offset,
    uint64_t length);
filed_file_vmo_cache_entry_t *filed_file_vmo_cache_slot(filed_runtime_t *runtime);
filed_file_vmo_cache_entry_t *filed_file_vmo_cache_slot_for_length(
    filed_runtime_t *runtime,
    uint64_t length);
filed_file_vmo_cache_entry_t *filed_file_vmo_cache_pinned_slot_for_length(
    filed_runtime_t *runtime,
    uint64_t length);
uint32_t filed_file_vmo_cache_reclaim_snapshots(
    filed_runtime_t *runtime,
    uint64_t *out_bytes);
filed_file_vmo_cache_entry_t *filed_file_vmo_cache_shared_lookup(
    filed_runtime_t *runtime,
    uint64_t backend_object);
int filed_cache_create_shared_vmo(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t object_generation,
    uint64_t logical_size,
    uint64_t required_end,
    filed_file_vmo_cache_entry_t **out_entry);
