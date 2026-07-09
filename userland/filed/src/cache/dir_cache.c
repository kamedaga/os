static uint64_t filed_dir_cache_next_clock(filed_runtime_t *runtime)
{
    if (filed_dir_cache.clock == UINT64_MAX) {
        filed_dir_cache.clock = 0;
    }
    ++filed_dir_cache.clock;
    if (filed_dir_cache.clock == 0) {
        filed_dir_cache.clock = 1;
    }
    return filed_dir_cache.clock;
}

static filed_dir_cache_slot_t *filed_dir_cache_find(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset)
{
    if (backend_object == 0) {
        return NULL;
    }
    for (size_t i = 0; i < FILED_DIR_CACHE_SLOTS; ++i) {
        filed_dir_cache_slot_t *slot = &filed_dir_cache.slots[i];
        if (slot->valid &&
            slot->backend_object == backend_object &&
            slot->offset == offset)
        {
            return slot;
        }
    }
    return NULL;
}

static filed_dir_cache_slot_t *filed_dir_cache_choose_slot(filed_runtime_t *runtime)
{
    filed_dir_cache_slot_t *oldest = NULL;
    for (size_t i = 0; i < FILED_DIR_CACHE_SLOTS; ++i) {
        filed_dir_cache_slot_t *slot = &filed_dir_cache.slots[i];
        if (!slot->valid) {
            return slot;
        }
        if (oldest == NULL || slot->last_used < oldest->last_used) {
            oldest = slot;
        }
    }
    if (oldest != NULL) {
        filed_dir_cache.evictions++;
    }
    return oldest;
}

static int filed_dir_cache_get(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    storage_v2_getdents_request_t *out_entries)
{
    filed_dir_cache_slot_t *slot;
    if (out_entries == NULL) {
        return 0;
    }
    slot = filed_dir_cache_find(runtime, backend_object, offset);
    if (slot == NULL) {
        filed_dir_cache.misses++;
        return 0;
    }
    if (offset == 0 && slot->entries.count == 0) {
        memset(slot, 0, sizeof(*slot));
        filed_dir_cache.misses++;
        return 0;
    }
    *out_entries = slot->entries;
    slot->last_used = filed_dir_cache_next_clock(runtime);
    filed_dir_cache.hits++;
    return 1;
}

static void filed_dir_cache_store(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    const storage_v2_getdents_request_t *entries)
{
    filed_dir_cache_slot_t *slot;
    if (backend_object == 0 || entries == NULL) {
        return;
    }
    if (offset == 0 && entries->count == 0) {
        return;
    }
    slot = filed_dir_cache_find(runtime, backend_object, offset);
    if (slot == NULL) {
        slot = filed_dir_cache_choose_slot(runtime);
    }
    if (slot == NULL) {
        return;
    }

    memset(slot, 0, sizeof(*slot));
    slot->valid = true;
    slot->backend_object = backend_object;
    slot->offset = offset;
    slot->last_used = filed_dir_cache_next_clock(runtime);
    slot->entries = *entries;
}

static void filed_dir_cache_invalidate_dir(filed_runtime_t *runtime, uint64_t backend_object)
{
    if (backend_object == 0) {
        return;
    }
    for (size_t i = 0; i < FILED_DIR_CACHE_SLOTS; ++i) {
        filed_dir_cache_slot_t *slot = &filed_dir_cache.slots[i];
        if (slot->valid && slot->backend_object == backend_object) {
            memset(slot, 0, sizeof(*slot));
        }
    }
}
