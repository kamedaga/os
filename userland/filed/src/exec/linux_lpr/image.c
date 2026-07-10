#include "internal.h"

#include "filed/backend_router.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "filed/page_cache.h"
#include "pacha/trace.h"

#ifndef FILED_LPR_TRACE
#define FILED_LPR_TRACE 0
#endif

enum {
    LPR_EXEC_IMAGE_CACHE_SLOTS = 4,
    LPR_EXEC_IMAGE_CACHE_MAX_BYTES = 2u * 1024u * 1024u,
    LPR_EXEC_META_CACHE_SLOTS = 16,
    LPR_EXEC_META_CACHE_MAX_BYTES = 128u * 1024u,
    LPR_EXEC_RANGE_CACHE_SLOTS = 24,
    LPR_EXEC_RANGE_CACHE_MAX_BYTES = 4u * 1024u * 1024u,
    LPR_EXEC_RANGE_CACHE_MIN_ENTRY_BYTES = 4096u,
    LPR_EXEC_RANGE_CACHE_MAX_ENTRY_BYTES = 2u * 1024u * 1024u,
    LPR_EXEC_BULK_READ_MIN_BYTES = 4096u,
};

typedef struct lpr_exec_image_cache_slot {
    bool valid;
    uint64_t backend_object;
    uint64_t object_generation;
    uint64_t size;
    uint64_t last_used;
    unsigned char *bytes;
} lpr_exec_image_cache_slot_t;

static lpr_exec_image_cache_slot_t lpr_exec_image_cache[LPR_EXEC_IMAGE_CACHE_SLOTS];
static uint64_t lpr_exec_image_cache_clock;
static uint64_t lpr_exec_image_cache_bytes;

typedef struct lpr_exec_range_cache_slot {
    bool valid;
    uint64_t backend_object;
    uint64_t object_generation;
    uint64_t offset;
    uint64_t size;
    uint64_t last_used;
    unsigned char *bytes;
} lpr_exec_range_cache_slot_t;

typedef struct lpr_exec_meta_cache_slot {
    bool valid;
    uint64_t backend_object;
    uint64_t object_generation;
    uint64_t file_size;
    uint64_t phdr_bytes;
    uint64_t last_used;
    lpr_exec_meta_t meta;
} lpr_exec_meta_cache_slot_t;

static lpr_exec_range_cache_slot_t lpr_exec_range_cache[LPR_EXEC_RANGE_CACHE_SLOTS];
static uint64_t lpr_exec_range_cache_bytes;
static lpr_exec_meta_cache_slot_t lpr_exec_meta_cache[LPR_EXEC_META_CACHE_SLOTS];
static uint64_t lpr_exec_meta_cache_bytes;

typedef struct lpr_exec_image_metric_record {
    const char *name;
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t total_bytes;
} lpr_exec_image_metric_record_t;

enum {
    LPR_EXEC_IMAGE_METRIC_SLOTS = 64,
};

static lpr_exec_image_metric_record_t lpr_exec_image_metrics[LPR_EXEC_IMAGE_METRIC_SLOTS];

static uint64_t lpr_exec_image_now_ns(void)
{
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static lpr_exec_image_metric_record_t *lpr_exec_image_metric_slot(const char *label)
{
    if (label == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < LPR_EXEC_IMAGE_METRIC_SLOTS; ++i) {
        if (lpr_exec_image_metrics[i].name == label ||
            (lpr_exec_image_metrics[i].name != NULL && strcmp(lpr_exec_image_metrics[i].name, label) == 0))
        {
            return &lpr_exec_image_metrics[i];
        }
    }
    for (size_t i = 0; i < LPR_EXEC_IMAGE_METRIC_SLOTS; ++i) {
        if (lpr_exec_image_metrics[i].name == NULL) {
            lpr_exec_image_metrics[i].name = label;
            return &lpr_exec_image_metrics[i];
        }
    }
    return NULL;
}

static void lpr_exec_image_metric(const char *label, uint64_t start_ns, uint64_t end_ns, uint64_t bytes)
{
    if (label == NULL || start_ns == 0 || end_ns < start_ns) {
        return;
    }
    const uint64_t elapsed_ns = end_ns - start_ns;
    lpr_exec_image_metric_record_t *metric = lpr_exec_image_metric_slot(label);
    if (metric == NULL) {
        return;
    }
    metric->count++;
    metric->total_ns += elapsed_ns;
    if (elapsed_ns > metric->max_ns) {
        metric->max_ns = elapsed_ns;
    }
    metric->total_bytes += bytes;
}

static void lpr_exec_image_metric_count(const char *label, uint64_t bytes)
{
    if (label == NULL) {
        return;
    }
    lpr_exec_image_metric_record_t *metric = lpr_exec_image_metric_slot(label);
    if (metric == NULL) {
        return;
    }
    metric->count++;
    metric->total_bytes += bytes;
}

void lpr_exec_image_dump_metrics(void)
{
    for (size_t i = 0; i < LPR_EXEC_IMAGE_METRIC_SLOTS; ++i) {
        const lpr_exec_image_metric_record_t *metric = &lpr_exec_image_metrics[i];
        if (metric->name == NULL || metric->count == 0) {
            continue;
        }
        if (strcmp(metric->name, "meta_cache_miss") != 0 &&
            strcmp(metric->name, "meta_cache_hit") != 0 &&
            strcmp(metric->name, "range_cache_miss") != 0 &&
            strcmp(metric->name, "range_cache_hit") != 0 &&
            strcmp(metric->name, "image_cache_miss") != 0 &&
            strcmp(metric->name, "image_cache_hit") != 0 &&
            strcmp(metric->name, "bulk_backend_pread") != 0 &&
            strcmp(metric->name, "cached_pread") != 0 &&
            strcmp(metric->name, "read_range_total") != 0)
        {
            continue;
        }
        pacha_trace5(
            PACHA_TRACE_COMPONENT_FILED,
            PACHA_TRACE_EVENT_FILED_EXEC_METRIC,
            PACHA_TRACE_CLASS_METRIC,
            pacha_trace_name_id(metric->name),
            metric->count,
            metric->total_ns / metric->count,
            metric->max_ns,
            metric->total_bytes);
    }
}

static uint64_t lpr_exec_image_cache_next_clock(void)
{
    if (lpr_exec_image_cache_clock == UINT64_MAX) {
        lpr_exec_image_cache_clock = 0;
    }
    ++lpr_exec_image_cache_clock;
    if (lpr_exec_image_cache_clock == 0) {
        lpr_exec_image_cache_clock = 1;
    }
    return lpr_exec_image_cache_clock;
}

static void lpr_exec_image_cache_clear_slot(lpr_exec_image_cache_slot_t *slot)
{
    if (slot == NULL || !slot->valid) {
        return;
    }
    if (lpr_exec_image_cache_bytes >= slot->size) {
        lpr_exec_image_cache_bytes -= slot->size;
    } else {
        lpr_exec_image_cache_bytes = 0;
    }
    free(slot->bytes);
    memset(slot, 0, sizeof(*slot));
}

static void lpr_exec_range_cache_clear_slot(lpr_exec_range_cache_slot_t *slot)
{
    if (slot == NULL || !slot->valid) {
        return;
    }
    if (lpr_exec_range_cache_bytes >= slot->size) {
        lpr_exec_range_cache_bytes -= slot->size;
    } else {
        lpr_exec_range_cache_bytes = 0;
    }
    free(slot->bytes);
    memset(slot, 0, sizeof(*slot));
}

static void lpr_exec_meta_cache_clear_slot(lpr_exec_meta_cache_slot_t *slot)
{
    if (slot == NULL || !slot->valid) {
        return;
    }
    if (lpr_exec_meta_cache_bytes >= slot->phdr_bytes) {
        lpr_exec_meta_cache_bytes -= slot->phdr_bytes;
    } else {
        lpr_exec_meta_cache_bytes = 0;
    }
    lpr_exec_free_meta(&slot->meta);
    memset(slot, 0, sizeof(*slot));
}

static lpr_exec_image_cache_slot_t *lpr_exec_image_cache_find(uint64_t backend_object, uint64_t object_generation)
{
    if (backend_object == 0) {
        return NULL;
    }
    for (uint64_t i = 0; i < LPR_EXEC_IMAGE_CACHE_SLOTS; ++i) {
        lpr_exec_image_cache_slot_t *slot = &lpr_exec_image_cache[i];
        if (slot->valid &&
            slot->backend_object == backend_object &&
            slot->object_generation == object_generation)
        {
            return slot;
        }
    }
    return NULL;
}

static lpr_exec_meta_cache_slot_t *lpr_exec_meta_cache_find(
    uint64_t backend_object,
    uint64_t object_generation,
    uint64_t file_size)
{
    if (backend_object == 0) {
        return NULL;
    }
    for (uint64_t i = 0; i < LPR_EXEC_META_CACHE_SLOTS; ++i) {
        lpr_exec_meta_cache_slot_t *slot = &lpr_exec_meta_cache[i];
        if (slot->valid &&
            slot->backend_object == backend_object &&
            slot->object_generation == object_generation &&
            slot->file_size == file_size)
        {
            return slot;
        }
    }
    return NULL;
}

static lpr_exec_range_cache_slot_t *lpr_exec_range_cache_find(
    uint64_t backend_object,
    uint64_t object_generation,
    uint64_t offset,
    uint64_t size)
{
    if (backend_object == 0 || size == 0) {
        return NULL;
    }
    for (uint64_t i = 0; i < LPR_EXEC_RANGE_CACHE_SLOTS; ++i) {
        lpr_exec_range_cache_slot_t *slot = &lpr_exec_range_cache[i];
        if (slot->valid &&
            slot->backend_object == backend_object &&
            slot->object_generation == object_generation &&
            slot->offset == offset &&
            slot->size == size)
        {
            return slot;
        }
    }
    return NULL;
}

static lpr_exec_image_cache_slot_t *lpr_exec_image_cache_choose_slot(void)
{
    lpr_exec_image_cache_slot_t *oldest = NULL;
    for (uint64_t i = 0; i < LPR_EXEC_IMAGE_CACHE_SLOTS; ++i) {
        lpr_exec_image_cache_slot_t *slot = &lpr_exec_image_cache[i];
        if (!slot->valid) {
            return slot;
        }
        if (oldest == NULL || slot->last_used < oldest->last_used) {
            oldest = slot;
        }
    }
    return oldest;
}

static lpr_exec_range_cache_slot_t *lpr_exec_range_cache_choose_slot(void)
{
    lpr_exec_range_cache_slot_t *oldest = NULL;
    for (uint64_t i = 0; i < LPR_EXEC_RANGE_CACHE_SLOTS; ++i) {
        lpr_exec_range_cache_slot_t *slot = &lpr_exec_range_cache[i];
        if (!slot->valid) {
            return slot;
        }
        if (oldest == NULL || slot->last_used < oldest->last_used) {
            oldest = slot;
        }
    }
    return oldest;
}

static lpr_exec_meta_cache_slot_t *lpr_exec_meta_cache_choose_slot(void)
{
    lpr_exec_meta_cache_slot_t *oldest = NULL;
    for (uint64_t i = 0; i < LPR_EXEC_META_CACHE_SLOTS; ++i) {
        lpr_exec_meta_cache_slot_t *slot = &lpr_exec_meta_cache[i];
        if (!slot->valid) {
            return slot;
        }
        if (oldest == NULL || slot->last_used < oldest->last_used) {
            oldest = slot;
        }
    }
    return oldest;
}

static bool lpr_exec_image_cache_clone(
    uint64_t backend_object,
    uint64_t object_generation,
    uint64_t size,
    lpr_exec_image_t *out_image)
{
    if (out_image == NULL || size == 0 || size > SIZE_MAX) {
        return false;
    }
    lpr_exec_image_cache_slot_t *slot = lpr_exec_image_cache_find(backend_object, object_generation);
    if (slot == NULL || slot->size != size || slot->bytes == NULL) {
        lpr_exec_image_metric_count("image_cache_miss", size);
        return false;
    }
    unsigned char *copy = malloc((size_t)size);
    if (copy == NULL) {
        lpr_exec_image_metric_count("image_cache_oom", size);
        return false;
    }
    memcpy(copy, slot->bytes, (size_t)size);
    slot->last_used = lpr_exec_image_cache_next_clock();
    out_image->bytes = copy;
    out_image->backend_object = backend_object;
    out_image->object_generation = object_generation;
    out_image->size = size;
    lpr_exec_image_metric_count("image_cache_hit", size);
    return true;
}

static bool lpr_exec_meta_cache_clone(
    const lpr_exec_file_t *file,
    lpr_exec_meta_t *out_meta)
{
    if (file == NULL || out_meta == NULL || file->size == 0 || file->backend_object == 0) {
        return false;
    }
    lpr_exec_meta_cache_slot_t *slot = lpr_exec_meta_cache_find(
        file->backend_object,
        file->object_generation,
        file->size);
    if (slot == NULL || slot->meta.phdrs == NULL || slot->phdr_bytes == 0) {
        lpr_exec_image_metric_count("meta_cache_miss", 0);
        return false;
    }

    unsigned char *phdrs = malloc((size_t)slot->phdr_bytes);
    if (phdrs == NULL) {
        lpr_exec_image_metric_count("meta_cache_oom", slot->phdr_bytes);
        return false;
    }
    memcpy(phdrs, slot->meta.phdrs, (size_t)slot->phdr_bytes);
    *out_meta = slot->meta;
    out_meta->phdrs = phdrs;
    slot->last_used = lpr_exec_image_cache_next_clock();
    lpr_exec_image_metric_count("meta_cache_hit", slot->phdr_bytes);
    return true;
}

static bool lpr_exec_range_cache_copy(
    uint64_t backend_object,
    uint64_t object_generation,
    uint64_t offset,
    uint64_t size,
    unsigned char *out)
{
    if (out == NULL ||
        size < LPR_EXEC_RANGE_CACHE_MIN_ENTRY_BYTES ||
        size > SIZE_MAX)
    {
        return false;
    }
    lpr_exec_range_cache_slot_t *slot = lpr_exec_range_cache_find(backend_object, object_generation, offset, size);
    if (slot == NULL || slot->bytes == NULL) {
        lpr_exec_image_metric_count("range_cache_miss", size);
        return false;
    }
    memcpy(out, slot->bytes, (size_t)size);
    slot->last_used = lpr_exec_image_cache_next_clock();
    lpr_exec_image_metric_count("range_cache_hit", size);
    return true;
}

static void lpr_exec_image_cache_store(
    uint64_t backend_object,
    uint64_t object_generation,
    const unsigned char *bytes,
    uint64_t size)
{
    if (backend_object == 0 ||
        bytes == NULL ||
        size == 0 ||
        size > SIZE_MAX ||
        size > LPR_EXEC_IMAGE_CACHE_MAX_BYTES)
    {
        return;
    }

    lpr_exec_image_cache_slot_t *slot = lpr_exec_image_cache_find(backend_object, object_generation);
    if (slot != NULL) {
        lpr_exec_image_cache_clear_slot(slot);
    }

    while (lpr_exec_image_cache_bytes + size > LPR_EXEC_IMAGE_CACHE_MAX_BYTES) {
        lpr_exec_image_cache_slot_t *victim = lpr_exec_image_cache_choose_slot();
        if (victim == NULL || !victim->valid) {
            break;
        }
        lpr_exec_image_metric_count("image_cache_evict", victim->size);
        lpr_exec_image_cache_clear_slot(victim);
    }

    slot = lpr_exec_image_cache_choose_slot();
    if (slot == NULL) {
        return;
    }
    if (slot->valid) {
        lpr_exec_image_metric_count("image_cache_evict", slot->size);
        lpr_exec_image_cache_clear_slot(slot);
    }

    unsigned char *copy = malloc((size_t)size);
    if (copy == NULL) {
        lpr_exec_image_metric_count("image_cache_store_oom", size);
        return;
    }
    memcpy(copy, bytes, (size_t)size);
    slot->valid = true;
    slot->backend_object = backend_object;
    slot->object_generation = object_generation;
    slot->size = size;
    slot->last_used = lpr_exec_image_cache_next_clock();
    slot->bytes = copy;
    lpr_exec_image_cache_bytes += size;
    lpr_exec_image_metric_count("image_cache_store", size);
}

static void lpr_exec_meta_cache_store(
    const lpr_exec_file_t *file,
    const lpr_exec_meta_t *meta)
{
    if (file == NULL ||
        meta == NULL ||
        meta->phdrs == NULL ||
        file->backend_object == 0 ||
        meta->phdr_bytes == 0 ||
        meta->phdr_bytes > SIZE_MAX ||
        meta->phdr_bytes > LPR_EXEC_META_CACHE_MAX_BYTES)
    {
        return;
    }

    lpr_exec_meta_cache_slot_t *slot = lpr_exec_meta_cache_find(
        file->backend_object,
        file->object_generation,
        file->size);
    if (slot != NULL) {
        lpr_exec_meta_cache_clear_slot(slot);
    }

    while (lpr_exec_meta_cache_bytes + meta->phdr_bytes > LPR_EXEC_META_CACHE_MAX_BYTES) {
        lpr_exec_meta_cache_slot_t *victim = lpr_exec_meta_cache_choose_slot();
        if (victim == NULL || !victim->valid) {
            break;
        }
        lpr_exec_image_metric_count("meta_cache_evict", victim->phdr_bytes);
        lpr_exec_meta_cache_clear_slot(victim);
    }

    slot = lpr_exec_meta_cache_choose_slot();
    if (slot == NULL) {
        return;
    }
    if (slot->valid) {
        lpr_exec_image_metric_count("meta_cache_evict", slot->phdr_bytes);
        lpr_exec_meta_cache_clear_slot(slot);
    }

    unsigned char *phdrs = malloc((size_t)meta->phdr_bytes);
    if (phdrs == NULL) {
        lpr_exec_image_metric_count("meta_cache_store_oom", meta->phdr_bytes);
        return;
    }
    memcpy(phdrs, meta->phdrs, (size_t)meta->phdr_bytes);

    slot->valid = true;
    slot->backend_object = file->backend_object;
    slot->object_generation = file->object_generation;
    slot->file_size = file->size;
    slot->phdr_bytes = meta->phdr_bytes;
    slot->last_used = lpr_exec_image_cache_next_clock();
    slot->meta = *meta;
    slot->meta.phdrs = phdrs;
    lpr_exec_meta_cache_bytes += meta->phdr_bytes;
    lpr_exec_image_metric_count("meta_cache_store", meta->phdr_bytes);
}

static void lpr_exec_range_cache_store(
    uint64_t backend_object,
    uint64_t object_generation,
    uint64_t offset,
    const unsigned char *bytes,
    uint64_t size)
{
    if (backend_object == 0 ||
        bytes == NULL ||
        size < LPR_EXEC_RANGE_CACHE_MIN_ENTRY_BYTES ||
        size > SIZE_MAX ||
        size > LPR_EXEC_RANGE_CACHE_MAX_ENTRY_BYTES)
    {
        return;
    }

    lpr_exec_range_cache_slot_t *slot = lpr_exec_range_cache_find(backend_object, object_generation, offset, size);
    if (slot != NULL) {
        lpr_exec_range_cache_clear_slot(slot);
    }

    while (lpr_exec_range_cache_bytes + size > LPR_EXEC_RANGE_CACHE_MAX_BYTES) {
        lpr_exec_range_cache_slot_t *victim = lpr_exec_range_cache_choose_slot();
        if (victim == NULL || !victim->valid) {
            break;
        }
        lpr_exec_image_metric_count("range_cache_evict", victim->size);
        lpr_exec_range_cache_clear_slot(victim);
    }

    slot = lpr_exec_range_cache_choose_slot();
    if (slot == NULL) {
        return;
    }
    if (slot->valid) {
        lpr_exec_image_metric_count("range_cache_evict", slot->size);
        lpr_exec_range_cache_clear_slot(slot);
    }

    unsigned char *copy = malloc((size_t)size);
    if (copy == NULL) {
        lpr_exec_image_metric_count("range_cache_store_oom", size);
        return;
    }
    memcpy(copy, bytes, (size_t)size);
    slot->valid = true;
    slot->backend_object = backend_object;
    slot->object_generation = object_generation;
    slot->offset = offset;
    slot->size = size;
    slot->last_used = lpr_exec_image_cache_next_clock();
    slot->bytes = copy;
    lpr_exec_range_cache_bytes += size;
    lpr_exec_image_metric_count("range_cache_store", size);
}

static bool lpr_exec_range_cache_store_take(
    uint64_t backend_object,
    uint64_t object_generation,
    uint64_t offset,
    unsigned char *bytes,
    uint64_t size)
{
    if (backend_object == 0 ||
        bytes == NULL ||
        size < LPR_EXEC_RANGE_CACHE_MIN_ENTRY_BYTES ||
        size > SIZE_MAX ||
        size > LPR_EXEC_RANGE_CACHE_MAX_ENTRY_BYTES)
    {
        return false;
    }

    lpr_exec_range_cache_slot_t *slot = lpr_exec_range_cache_find(backend_object, object_generation, offset, size);
    if (slot != NULL) {
        lpr_exec_range_cache_clear_slot(slot);
    }

    while (lpr_exec_range_cache_bytes + size > LPR_EXEC_RANGE_CACHE_MAX_BYTES) {
        lpr_exec_range_cache_slot_t *victim = lpr_exec_range_cache_choose_slot();
        if (victim == NULL || !victim->valid) {
            break;
        }
        lpr_exec_image_metric_count("range_cache_evict", victim->size);
        lpr_exec_range_cache_clear_slot(victim);
    }

    slot = lpr_exec_range_cache_choose_slot();
    if (slot == NULL) {
        return false;
    }
    if (slot->valid) {
        lpr_exec_image_metric_count("range_cache_evict", slot->size);
        lpr_exec_range_cache_clear_slot(slot);
    }

    slot->valid = true;
    slot->backend_object = backend_object;
    slot->object_generation = object_generation;
    slot->offset = offset;
    slot->size = size;
    slot->last_used = lpr_exec_image_cache_next_clock();
    slot->bytes = bytes;
    lpr_exec_range_cache_bytes += size;
    lpr_exec_image_metric_count("range_cache_store_take", size);
    return true;
}

static int lpr_exec_meta_read_text_section(
    filed_runtime_t *runtime,
    const lpr_exec_file_t *file,
    const unsigned char *prefix,
    uint64_t prefix_size,
    lpr_exec_meta_t *meta)
{
    if (runtime == NULL || file == NULL || prefix == NULL || meta == NULL) {
        return -22;
    }
    meta->text_offset = 0;
    meta->text_size = 0;

    const unsigned char *ehdr = meta->ehdr;
    const uint64_t e_shoff = lpr_exec_rd64(ehdr + 40);
    const uint16_t e_shentsize = lpr_exec_rd16(ehdr + 58);
    const uint16_t e_shnum = lpr_exec_rd16(ehdr + 60);
    const uint16_t e_shstrndx = lpr_exec_rd16(ehdr + 62);
    if (e_shoff == 0 || e_shnum == 0) {
        return 0;
    }
    if (e_shentsize < LPR_EXEC_SHDR_BYTES || e_shstrndx >= e_shnum || e_shoff > file->size) {
        return -8;
    }
    const uint64_t shdr_bytes = (uint64_t)e_shentsize * (uint64_t)e_shnum;
    if (shdr_bytes > file->size - e_shoff || shdr_bytes > SIZE_MAX) {
        return -8;
    }

    unsigned char *shdrs = malloc((size_t)shdr_bytes);
    if (shdrs == NULL) {
        return -12;
    }
    int status = 0;
    if (e_shoff <= prefix_size && shdr_bytes <= prefix_size - e_shoff) {
        memcpy(shdrs, prefix + e_shoff, (size_t)shdr_bytes);
    } else {
        status = lpr_exec_read_file_range(runtime, file, e_shoff, shdrs, shdr_bytes);
        if (status != 0) {
            free(shdrs);
            return status;
        }
    }

    const unsigned char *shstr = shdrs + (uint64_t)e_shstrndx * e_shentsize;
    const uint64_t shstr_offset = lpr_exec_rd64(shstr + 24);
    const uint64_t shstr_size = lpr_exec_rd64(shstr + 32);
    if (shstr_offset > file->size || shstr_size > file->size - shstr_offset || shstr_size > SIZE_MAX) {
        free(shdrs);
        return -8;
    }

    unsigned char *names = malloc((size_t)shstr_size);
    if (names == NULL) {
        free(shdrs);
        return -12;
    }
    if (shstr_offset <= prefix_size && shstr_size <= prefix_size - shstr_offset) {
        memcpy(names, prefix + shstr_offset, (size_t)shstr_size);
    } else {
        status = lpr_exec_read_file_range(runtime, file, shstr_offset, names, shstr_size);
        if (status != 0) {
            free(names);
            free(shdrs);
            return status;
        }
    }

    for (uint16_t i = 0; i < e_shnum; ++i) {
        const unsigned char *sh = shdrs + (uint64_t)i * e_shentsize;
        const uint32_t sh_name = lpr_exec_rd32(sh);
        const uint64_t sh_offset = lpr_exec_rd64(sh + 24);
        const uint64_t sh_size = lpr_exec_rd64(sh + 32);
        if (sh_name >= shstr_size || strcmp((const char *)(const void *)(names + sh_name), ".text") != 0) {
            continue;
        }
        if (sh_offset > file->size || sh_size > file->size - sh_offset) {
            free(names);
            free(shdrs);
            return -8;
        }
        meta->text_offset = sh_offset;
        meta->text_size = sh_size;
        break;
    }

    free(names);
    free(shdrs);
    return 0;
}

void filed_exec_linux_lpr_invalidate_backend_object(filed_runtime_t *runtime, uint64_t backend_object)
{
    if (backend_object == 0) {
        return;
    }
    lpr_exec_invalidate_runtime_image_cache(backend_object);
    lpr_exec_invalidate_interpreter_cache(runtime, backend_object);
    lpr_exec_invalidate_segment_vmo_cache(backend_object);
    for (uint64_t i = 0; i < LPR_EXEC_IMAGE_CACHE_SLOTS; ++i) {
        lpr_exec_image_cache_slot_t *slot = &lpr_exec_image_cache[i];
        if (slot->valid && slot->backend_object == backend_object) {
            lpr_exec_image_metric_count("image_cache_invalidate", slot->size);
            lpr_exec_image_cache_clear_slot(slot);
        }
    }
    for (uint64_t i = 0; i < LPR_EXEC_META_CACHE_SLOTS; ++i) {
        lpr_exec_meta_cache_slot_t *meta_slot = &lpr_exec_meta_cache[i];
        if (meta_slot->valid && meta_slot->backend_object == backend_object) {
            lpr_exec_image_metric_count("meta_cache_invalidate", meta_slot->phdr_bytes);
            lpr_exec_meta_cache_clear_slot(meta_slot);
        }
    }
    for (uint64_t i = 0; i < LPR_EXEC_RANGE_CACHE_SLOTS; ++i) {
        lpr_exec_range_cache_slot_t *range_slot = &lpr_exec_range_cache[i];
        if (range_slot->valid && range_slot->backend_object == backend_object) {
            lpr_exec_image_metric_count("range_cache_invalidate", range_slot->size);
            lpr_exec_range_cache_clear_slot(range_slot);
        }
    }
}

static const char *skip_slashes(const char *path)
{
    while (path != NULL && *path == '/') {
        ++path;
    }
    return path;
}

static filed_vnode_kind_t kind_from_unix_type(uint64_t kind)
{
    switch (kind & 0170000u) {
    case 0040000u:
        return FILED_VNODE_DIRECTORY;
    case 0120000u:
        return FILED_VNODE_SYMLINK;
    case 0010000u:
        return FILED_VNODE_FIFO;
    case 0100000u:
    default:
        return FILED_VNODE_REGULAR;
    }
}

static void close_walk_handle(filed_runtime_t *runtime, filed_handle_id_t handle_id, int owned)
{
    if (runtime != NULL && owned && handle_id != 0 && handle_id != runtime->root_handle_id) {
        (void)filed_vfs_close_handle(&runtime->vfs, handle_id);
    }
}

static int lookup_and_open_component(
    filed_runtime_t *runtime,
    filed_handle_id_t parent_handle,
    const char *name,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open)
{
    filed_vfs_io_decision_t parent_decision;
    uint64_t object_id = 0;
    storage_v2_statx_reply_t backend_stat;

    if (runtime == NULL || name == NULL || out_open == NULL) {
        return -22;
    }
    filed_status_t status = filed_vfs_open_cached_child(
        &runtime->vfs,
        parent_handle,
        name,
        rights,
        open_flags,
        out_open);
    if (status == FILED_OK) {
        return 0;
    }
    if (status != FILED_ERR_NOT_FOUND) {
        return lpr_exec_status_to_errno(status);
    }

    status = filed_vfs_lookup_prepare(&runtime->vfs, parent_handle, &parent_decision);
    if (status != FILED_OK) {
        return lpr_exec_status_to_errno(status);
    }
    int result = filed_runtime_backend_lookup(runtime, parent_decision.backend_object, name, &object_id);
    if (result != 0) {
        return result;
    }
    memset(&backend_stat, 0, sizeof(backend_stat));
    result = filed_runtime_backend_statx(runtime, object_id, &backend_stat);
    if (result != 0) {
        (void)filed_runtime_backend_release_object(runtime, object_id);
        return result;
    }
    status = filed_vfs_open_backend_child(
        &runtime->vfs,
        parent_handle,
        object_id,
        kind_from_unix_type(backend_stat.kind),
        name,
        rights,
        open_flags,
        out_open);
    if (status != FILED_OK) {
        (void)filed_runtime_backend_release_object(runtime, object_id);
    }
    return lpr_exec_status_to_errno(status);
}

static int open_absolute_path(
    filed_runtime_t *runtime,
    const char *path,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open)
{
    filed_handle_id_t current_handle;
    int current_owned = 0;

    if (runtime == NULL || path == NULL || path[0] != '/' || out_open == NULL) {
        return -22;
    }
    path = skip_slashes(path);
    if (*path == '\0') {
        return -22;
    }
    memset(out_open, 0, sizeof(*out_open));
    current_handle = runtime->root_handle_id;

    for (;;) {
        char component[FILED_V2_NAME_BYTES];
        const char *component_start;
        const char *after_slashes;
        size_t component_len;
        int has_more;
        int require_directory;
        int final_component;

        path = skip_slashes(path);
        if (*path == '\0') {
            close_walk_handle(runtime, current_handle, current_owned);
            return -22;
        }
        component_start = path;
        while (*path != '\0' && *path != '/') {
            ++path;
        }
        component_len = (size_t)(path - component_start);
        if (component_len == 0 || component_len >= sizeof(component)) {
            close_walk_handle(runtime, current_handle, current_owned);
            return -22;
        }
        after_slashes = skip_slashes(path);
        has_more = *after_slashes != '\0';
        require_directory = (*path == '/');
        final_component = !has_more;
        memset(component, 0, sizeof(component));
        memcpy(component, component_start, component_len);

        if (component_len == 1 && component[0] == '.') {
            if (final_component) {
                const filed_status_t status = filed_vfs_open_existing(
                    &runtime->vfs,
                    current_handle,
                    rights,
                    open_flags | (require_directory ? FILED_OPEN_DIRECTORY : 0),
                    out_open);
                close_walk_handle(runtime, current_handle, current_owned);
                return lpr_exec_status_to_errno(status);
            }
            path = after_slashes;
            continue;
        }

        if (component_len == 2 && component[0] == '.' && component[1] == '.') {
            filed_vfs_open_result_t parent_open;
            const uint32_t next_rights = final_component ? rights : LPR_EXEC_WALK_RIGHTS;
            const uint32_t next_flags =
                final_component ? (open_flags | (require_directory ? FILED_OPEN_DIRECTORY : 0)) : FILED_OPEN_DIRECTORY;
            memset(&parent_open, 0, sizeof(parent_open));
            const filed_status_t status = filed_vfs_open_parent(
                &runtime->vfs,
                current_handle,
                next_rights,
                next_flags,
                &parent_open);
            close_walk_handle(runtime, current_handle, current_owned);
            if (status != FILED_OK) {
                return lpr_exec_status_to_errno(status);
            }
            if (final_component) {
                *out_open = parent_open;
                return 0;
            }
            current_handle = parent_open.handle_id;
            current_owned = 1;
            path = after_slashes;
            continue;
        }

        if (final_component) {
            const int result = lookup_and_open_component(
                runtime,
                current_handle,
                component,
                rights,
                open_flags | (require_directory ? FILED_OPEN_DIRECTORY : 0),
                out_open);
            close_walk_handle(runtime, current_handle, current_owned);
            return result;
        }

        filed_vfs_open_result_t next_open;
        const int result = lookup_and_open_component(
            runtime,
            current_handle,
            component,
            LPR_EXEC_WALK_RIGHTS,
            FILED_OPEN_DIRECTORY,
            &next_open);
        close_walk_handle(runtime, current_handle, current_owned);
        if (result != 0) {
            return result;
        }
        current_handle = next_open.handle_id;
        current_owned = 1;
        path = after_slashes;
    }
}

static int lpr_exec_read_file_range_ex(
    filed_runtime_t *runtime,
    const lpr_exec_file_t *file,
    uint64_t offset,
    unsigned char *buffer,
    uint64_t length,
    bool allow_bulk,
    bool allow_range_cache)
{
    filed_vfs_io_decision_t read_decision;
    uint64_t got = 0;

    if (runtime == NULL ||
        file == NULL ||
        file->handle_id == 0 ||
        file->backend_object == 0 ||
        buffer == NULL ||
        offset > file->size ||
        length > file->size - offset)
    {
        return -22;
    }
    if (length == 0) {
        return 0;
    }
    uint64_t stage_start = lpr_exec_image_now_ns();
    const filed_status_t status = filed_vfs_pread_prepare(&runtime->vfs, file->handle_id, offset, length, &read_decision);
    lpr_exec_image_metric("pread_prepare", stage_start, lpr_exec_image_now_ns(), length);
    if (status != FILED_OK) {
        return -13;
    }
    if (read_decision.backend_object != file->backend_object) {
        return -13;
    }
    if (read_decision.offset == offset &&
        read_decision.length == length &&
        allow_range_cache &&
        lpr_exec_range_cache_copy(file->backend_object, file->object_generation, offset, length, buffer))
    {
        return 0;
    }
    if (read_decision.offset == offset &&
        read_decision.length == length &&
        allow_bulk &&
        length > LPR_EXEC_BULK_READ_MIN_BYTES)
    {
        uint64_t direct_got = 0;
        stage_start = lpr_exec_image_now_ns();
        const int flush_status = filed_cache_flush_object(runtime, file->backend_object);
        lpr_exec_image_metric("bulk_flush", stage_start, lpr_exec_image_now_ns(), length);
        if (flush_status != 0) {
            return flush_status;
        }
        stage_start = lpr_exec_image_now_ns();
        const int direct_status = filed_runtime_backend_pread(
            runtime,
            file->backend_object,
            offset,
            buffer,
            length,
            &direct_got);
        lpr_exec_image_metric("bulk_backend_pread", stage_start, lpr_exec_image_now_ns(), length);
        if (direct_status != 0 || direct_got != length) {
            return direct_status != 0 ? direct_status : -5;
        }
        if (allow_range_cache) {
            lpr_exec_range_cache_store(file->backend_object, file->object_generation, offset, buffer, length);
        }
        return 0;
    }
    stage_start = lpr_exec_image_now_ns();
    const int result = filed_cached_pread(runtime, file->backend_object, read_decision.offset, buffer, read_decision.length, &got);
    lpr_exec_image_metric("cached_pread", stage_start, lpr_exec_image_now_ns(), read_decision.length);
    if (result != 0 || got != length) {
        return result != 0 ? result : -5;
    }
    if (read_decision.offset == offset && read_decision.length == length) {
        if (allow_range_cache) {
            lpr_exec_range_cache_store(file->backend_object, file->object_generation, offset, buffer, length);
        }
    }
    return 0;
}

int lpr_exec_read_file_range(
    filed_runtime_t *runtime,
    const lpr_exec_file_t *file,
    uint64_t offset,
    unsigned char *buffer,
    uint64_t length)
{
    return lpr_exec_read_file_range_ex(runtime, file, offset, buffer, length, true, true);
}

int lpr_exec_read_file_range_for_load(
    filed_runtime_t *runtime,
    const lpr_exec_file_t *file,
    uint64_t offset,
    uint64_t length,
    unsigned char **out_buffer,
    bool *out_owned)
{
    filed_vfs_io_decision_t read_decision;
    uint64_t got = 0;

    if (out_buffer != NULL) {
        *out_buffer = NULL;
    }
    if (out_owned != NULL) {
        *out_owned = true;
    }
    if (runtime == NULL ||
        file == NULL ||
        file->handle_id == 0 ||
        file->backend_object == 0 ||
        out_buffer == NULL ||
        out_owned == NULL ||
        offset > file->size ||
        length == 0 ||
        length > file->size - offset ||
        length > SIZE_MAX)
    {
        return -22;
    }

    unsigned char *buffer = malloc((size_t)length);
    if (buffer == NULL) {
        return -12;
    }

    uint64_t stage_start = lpr_exec_image_now_ns();
    const filed_status_t status = filed_vfs_pread_prepare(&runtime->vfs, file->handle_id, offset, length, &read_decision);
    lpr_exec_image_metric("pread_prepare", stage_start, lpr_exec_image_now_ns(), length);
    if (status != FILED_OK) {
        free(buffer);
        return -13;
    }
    if (read_decision.backend_object != file->backend_object) {
        free(buffer);
        return -13;
    }
    if (read_decision.offset == offset &&
        read_decision.length == length &&
        lpr_exec_range_cache_copy(file->backend_object, file->object_generation, offset, length, buffer))
    {
        *out_buffer = buffer;
        *out_owned = true;
        return 0;
    }
    if (read_decision.offset == offset &&
        read_decision.length == length &&
        length > LPR_EXEC_BULK_READ_MIN_BYTES)
    {
        uint64_t direct_got = 0;
        stage_start = lpr_exec_image_now_ns();
        const int flush_status = filed_cache_flush_object(runtime, file->backend_object);
        lpr_exec_image_metric("bulk_flush", stage_start, lpr_exec_image_now_ns(), length);
        if (flush_status != 0) {
            free(buffer);
            return flush_status;
        }
        stage_start = lpr_exec_image_now_ns();
        const int direct_status = filed_runtime_backend_pread(
            runtime,
            file->backend_object,
            offset,
            buffer,
            length,
            &direct_got);
        lpr_exec_image_metric("bulk_backend_pread", stage_start, lpr_exec_image_now_ns(), length);
        if (direct_status != 0 || direct_got != length) {
            free(buffer);
            return direct_status != 0 ? direct_status : -5;
        }
        *out_buffer = buffer;
        if (lpr_exec_range_cache_store_take(file->backend_object, file->object_generation, offset, buffer, length)) {
            *out_owned = false;
        } else {
            *out_owned = true;
        }
        return 0;
    }

    stage_start = lpr_exec_image_now_ns();
    const int result = filed_cached_pread(runtime, file->backend_object, read_decision.offset, buffer, read_decision.length, &got);
    lpr_exec_image_metric("cached_pread", stage_start, lpr_exec_image_now_ns(), read_decision.length);
    if (result != 0 || got != length) {
        free(buffer);
        return result != 0 ? result : -5;
    }
    *out_buffer = buffer;
    if (read_decision.offset == offset &&
        read_decision.length == length &&
        lpr_exec_range_cache_store_take(file->backend_object, file->object_generation, offset, buffer, length))
    {
        *out_owned = false;
    } else {
        *out_owned = true;
    }
    return 0;
}

int lpr_exec_init_file_from_handle(filed_runtime_t *runtime, filed_handle_id_t handle_id, lpr_exec_file_t *out_file)
{
    filed_vfs_io_decision_t stat_decision;
    filed_vfs_stat_snapshot_t snapshot;
    storage_v2_statx_reply_t stat;

    if (runtime == NULL || out_file == NULL || handle_id == 0) {
        return -22;
    }
    memset(out_file, 0, sizeof(*out_file));
    uint64_t stage_start = lpr_exec_image_now_ns();
    filed_status_t vfs_status = filed_vfs_stat_prepare(&runtime->vfs, handle_id, &stat_decision);
    lpr_exec_image_metric("stat_prepare", stage_start, lpr_exec_image_now_ns(), 0);
    if (vfs_status != FILED_OK) {
        return -13;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    stage_start = lpr_exec_image_now_ns();
    vfs_status = filed_vfs_get_stat_snapshot(&runtime->vfs, handle_id, &snapshot);
    lpr_exec_image_metric("stat_snapshot", stage_start, lpr_exec_image_now_ns(), 0);
    if (vfs_status != FILED_OK) {
        return -13;
    }
    memset(&stat, 0, sizeof(stat));
    if (snapshot.valid) {
        stat.mode = snapshot.mode;
        stat.size = snapshot.size;
        stat.blocks = snapshot.blocks;
        stat.nlink = snapshot.nlink;
        stat.kind = snapshot.kind;
    }
    if (!snapshot.valid ||
        (stat.kind & 0170000u) == 0 ||
        stat.size < LPR_EXEC_EHDR_BYTES)
    {
        stage_start = lpr_exec_image_now_ns();
        const int status = filed_runtime_backend_statx(runtime, stat_decision.backend_object, &stat);
        lpr_exec_image_metric("backend_statx", stage_start, lpr_exec_image_now_ns(), 0);
        if (status != 0) {
            return status;
        }
        snapshot.valid = true;
        snapshot.mode = stat.mode;
        snapshot.size = stat.size;
        snapshot.blocks = stat.blocks;
        snapshot.nlink = stat.nlink;
        snapshot.kind = stat.kind;
        (void)filed_vfs_update_stat_snapshot(&runtime->vfs, stat_decision.backend_object, &snapshot);
    }
    if ((stat.kind & 0170000u) != 0100000u ||
        stat.size < LPR_EXEC_EHDR_BYTES ||
        stat.size > LPR_EXEC_MAX_IMAGE_BYTES)
    {
        fprintf(stderr,
            "[filed] linux-lpr: exec stat invalid handle=%u backend=0x%llx kind=0%llo mode=0%llo size=%llu snapshot_valid=%u\n",
            (unsigned)handle_id,
            (unsigned long long)stat_decision.backend_object,
            (unsigned long long)stat.kind,
            (unsigned long long)stat.mode,
            (unsigned long long)stat.size,
            snapshot.valid ? 1u : 0u);
        return -8;
    }
    out_file->handle_id = handle_id;
    out_file->backend_object = stat_decision.backend_object;
    out_file->object_generation = stat_decision.object_generation;
    out_file->size = stat.size;
    return 0;
}

int lpr_exec_read_full_image(filed_runtime_t *runtime, filed_handle_id_t handle_id, lpr_exec_image_t *out_image)
{
    lpr_exec_file_t file;

    if (runtime == NULL || out_image == NULL) {
        return -22;
    }
    memset(out_image, 0, sizeof(*out_image));
    memset(&file, 0, sizeof(file));
    int status = lpr_exec_init_file_from_handle(runtime, handle_id, &file);
    if (status != 0) {
        return status;
    }
    return lpr_exec_read_full_file_image(runtime, &file, out_image);
}

int lpr_exec_read_full_file_image(
    filed_runtime_t *runtime,
    const lpr_exec_file_t *file,
    lpr_exec_image_t *out_image)
{
    if (runtime == NULL ||
        file == NULL ||
        out_image == NULL ||
        file->handle_id == 0 ||
        file->backend_object == 0 ||
        file->size < LPR_EXEC_EHDR_BYTES ||
        file->size > LPR_EXEC_MAX_IMAGE_BYTES ||
        file->size > SIZE_MAX)
    {
        return -22;
    }
    memset(out_image, 0, sizeof(*out_image));
    if (lpr_exec_image_cache_clone(file->backend_object, file->object_generation, file->size, out_image)) {
        return 0;
    }
    uint64_t stage_start = lpr_exec_image_now_ns();
    unsigned char *image = malloc((size_t)file->size);
    lpr_exec_image_metric("malloc_image", stage_start, lpr_exec_image_now_ns(), file->size);
    if (image == NULL) {
        return -12;
    }
    stage_start = lpr_exec_image_now_ns();
    const int status = lpr_exec_read_file_range_ex(runtime, file, 0, image, file->size, true, false);
    lpr_exec_image_metric("read_range_total", stage_start, lpr_exec_image_now_ns(), file->size);
    if (status != 0) {
        free(image);
        return status;
    }
    out_image->bytes = image;
    out_image->backend_object = file->backend_object;
    out_image->object_generation = file->object_generation;
    out_image->size = file->size;
    lpr_exec_image_cache_store(file->backend_object, file->object_generation, image, file->size);
    return 0;
}

int lpr_exec_read_meta(filed_runtime_t *runtime, const lpr_exec_file_t *file, lpr_exec_meta_t *out_meta)
{
    unsigned char prefix[LPR_IMAGE_PAGE_SIZE];
    uint64_t prefix_size = 0;

    if (runtime == NULL || file == NULL || out_meta == NULL) {
        return -22;
    }
    memset(out_meta, 0, sizeof(*out_meta));
    if (file->size < LPR_EXEC_EHDR_BYTES) {
        return -8;
    }
    if (lpr_exec_meta_cache_clone(file, out_meta)) {
        return 0;
    }
    memset(prefix, 0, sizeof(prefix));
    prefix_size = file->size < sizeof(prefix) ? file->size : sizeof(prefix);
    int status = lpr_exec_read_file_range(runtime, file, 0, prefix, prefix_size);
    if (status != 0) {
        return status;
    }
    memcpy(out_meta->ehdr, prefix, LPR_EXEC_EHDR_BYTES);
    const unsigned char *ehdr = out_meta->ehdr;
    if (ehdr[0] != 0x7f || ehdr[1] != 'E' || ehdr[2] != 'L' || ehdr[3] != 'F') {
        return -8;
    }
    if (ehdr[4] != LPR_EXEC_ELF_CLASS_64 ||
        ehdr[5] != LPR_EXEC_ELF_DATA_LSB ||
        ehdr[6] != LPR_EXEC_ELF_VERSION_CURRENT)
    {
        return -8;
    }
    out_meta->type = lpr_exec_rd16(ehdr + 16);
    const uint16_t e_machine = lpr_exec_rd16(ehdr + 18);
    const uint32_t e_version = lpr_exec_rd32(ehdr + 20);
    out_meta->entry = lpr_exec_rd64(ehdr + 24);
    out_meta->phoff = lpr_exec_rd64(ehdr + 32);
    out_meta->phent = lpr_exec_rd16(ehdr + 54);
    out_meta->phnum = lpr_exec_rd16(ehdr + 56);
    if ((out_meta->type != LPR_EXEC_ELF_TYPE_EXEC && out_meta->type != LPR_EXEC_ELF_TYPE_DYN) ||
        e_machine != LPR_EXEC_ELF_MACHINE_X86_64 ||
        e_version != LPR_EXEC_ELF_VERSION_CURRENT ||
        out_meta->phent < LPR_EXEC_PHDR_BYTES ||
        out_meta->phnum == 0)
    {
        return -8;
    }
    if (out_meta->phoff > file->size) {
        return -8;
    }
    out_meta->phdr_bytes = (uint64_t)out_meta->phent * (uint64_t)out_meta->phnum;
    if (out_meta->phdr_bytes > file->size - out_meta->phoff ||
        out_meta->phdr_bytes > SIZE_MAX)
    {
        return -8;
    }
    out_meta->phdrs = malloc((size_t)out_meta->phdr_bytes);
    if (out_meta->phdrs == NULL) {
        return -12;
    }
    if (out_meta->phoff <= prefix_size &&
        out_meta->phdr_bytes <= prefix_size - out_meta->phoff)
    {
        memcpy(out_meta->phdrs, prefix + out_meta->phoff, (size_t)out_meta->phdr_bytes);
    } else {
        status = lpr_exec_read_file_range(runtime, file, out_meta->phoff, out_meta->phdrs, out_meta->phdr_bytes);
        if (status != 0) {
            lpr_exec_free_meta(out_meta);
            return status;
        }
    }
    status = lpr_exec_meta_read_text_section(runtime, file, prefix, prefix_size, out_meta);
    if (status != 0) {
        lpr_exec_free_meta(out_meta);
        return status;
    }
    for (uint16_t i = 0; i < out_meta->phnum; ++i) {
        const unsigned char *ph = out_meta->phdrs + (uint64_t)i * out_meta->phent;
        if (lpr_exec_rd32(ph) != LPR_EXEC_PT_INTERP) {
            continue;
        }
        const uint64_t p_offset = lpr_exec_rd64(ph + 8);
        const uint64_t p_filesz = lpr_exec_rd64(ph + 32);
        if (out_meta->interp_path[0] != '\0' ||
            p_filesz == 0 ||
            p_filesz >= sizeof(out_meta->interp_path) ||
            p_offset > file->size ||
            p_filesz > file->size - p_offset)
        {
            lpr_exec_free_meta(out_meta);
            return -8;
        }
        if (p_offset <= prefix_size && p_filesz <= prefix_size - p_offset) {
            if (prefix[p_offset + p_filesz - 1u] != '\0' || prefix[p_offset] != '/') {
                lpr_exec_free_meta(out_meta);
                return -8;
            }
            memcpy(out_meta->interp_path, prefix + p_offset, (size_t)p_filesz);
        } else {
            status = lpr_exec_read_file_range(runtime, file, p_offset, (unsigned char *)out_meta->interp_path, p_filesz);
            if (status != 0) {
                lpr_exec_free_meta(out_meta);
                return status;
            }
            if (out_meta->interp_path[p_filesz - 1u] != '\0' || out_meta->interp_path[0] != '/') {
                lpr_exec_free_meta(out_meta);
                return -8;
            }
        }
    }
    lpr_exec_meta_cache_store(file, out_meta);
    return 0;
}

void lpr_exec_free_meta(lpr_exec_meta_t *meta)
{
    if (meta == NULL) {
        return;
    }
    free(meta->phdrs);
    memset(meta, 0, sizeof(*meta));
}

int lpr_exec_read_absolute_image(filed_runtime_t *runtime, const char *path, lpr_exec_image_t *out_image)
{
    filed_vfs_open_result_t open_result;
    if (runtime == NULL || path == NULL || out_image == NULL) {
        return -22;
    }
    memset(&open_result, 0, sizeof(open_result));
    int status = open_absolute_path(
        runtime,
        path,
        FILED_RIGHT_READ | FILED_RIGHT_EXEC | FILED_RIGHT_STAT,
        0,
        &open_result);
    if (status != 0) {
        return status;
    }
    status = lpr_exec_read_full_image(runtime, open_result.handle_id, out_image);
    (void)filed_vfs_close_handle(&runtime->vfs, open_result.handle_id);
    return status;
}

int lpr_exec_open_absolute_file(filed_runtime_t *runtime, const char *path, lpr_exec_file_t *out_file)
{
    filed_vfs_open_result_t open_result;
    if (runtime == NULL || path == NULL || out_file == NULL) {
        return -22;
    }
    memset(out_file, 0, sizeof(*out_file));
    memset(&open_result, 0, sizeof(open_result));
    int status = open_absolute_path(
        runtime,
        path,
        FILED_RIGHT_READ | FILED_RIGHT_EXEC | FILED_RIGHT_STAT,
        0,
        &open_result);
    if (status != 0) {
        return status;
    }
    status = lpr_exec_init_file_from_handle(runtime, open_result.handle_id, out_file);
    if (status != 0) {
        (void)filed_vfs_close_handle(&runtime->vfs, open_result.handle_id);
        memset(out_file, 0, sizeof(*out_file));
    }
    return status;
}

void lpr_exec_close_file(filed_runtime_t *runtime, lpr_exec_file_t *file)
{
    if (runtime == NULL || file == NULL || file->handle_id == 0) {
        return;
    }
    (void)filed_vfs_close_handle(&runtime->vfs, file->handle_id);
    memset(file, 0, sizeof(*file));
}
