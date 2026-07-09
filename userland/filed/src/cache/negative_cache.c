static uint64_t filed_negative_lookup_cache_next_clock(filed_runtime_t *runtime)
{
    if (filed_negative_lookup_cache.clock == UINT64_MAX) {
        filed_negative_lookup_cache.clock = 0;
    }
    ++filed_negative_lookup_cache.clock;
    if (filed_negative_lookup_cache.clock == 0) {
        filed_negative_lookup_cache.clock = 1;
    }
    return filed_negative_lookup_cache.clock;
}

static size_t filed_lookup_name_len(const char *name)
{
    if (name == NULL) {
        return 0;
    }
    return strnlen(name, FILED_V2_NAME_BYTES);
}

static filed_negative_lookup_cache_slot_t *filed_negative_lookup_cache_find(
    filed_runtime_t *runtime,
    uint64_t parent_backend_object,
    filed_generation_t parent_dir_generation,
    const char *name,
    size_t name_len)
{
    if (parent_backend_object == 0 ||
        name == NULL ||
        name_len == 0 ||
        name_len >= FILED_V2_NAME_BYTES)
    {
        return NULL;
    }
    for (size_t i = 0; i < FILED_NEGATIVE_LOOKUP_CACHE_SLOTS; ++i) {
        filed_negative_lookup_cache_slot_t *slot = &filed_negative_lookup_cache.slots[i];
        if (slot->valid &&
            slot->parent_backend_object == parent_backend_object &&
            slot->parent_dir_generation == parent_dir_generation &&
            strncmp(slot->name, name, FILED_V2_NAME_BYTES) == 0)
        {
            return slot;
        }
    }
    return NULL;
}

static filed_negative_lookup_cache_slot_t *filed_negative_lookup_cache_choose_slot(filed_runtime_t *runtime)
{
    filed_negative_lookup_cache_slot_t *oldest = NULL;
    for (size_t i = 0; i < FILED_NEGATIVE_LOOKUP_CACHE_SLOTS; ++i) {
        filed_negative_lookup_cache_slot_t *slot = &filed_negative_lookup_cache.slots[i];
        if (!slot->valid) {
            return slot;
        }
        if (oldest == NULL || slot->last_used < oldest->last_used) {
            oldest = slot;
        }
    }
    if (oldest != NULL) {
        filed_negative_lookup_cache.evictions++;
    }
    return oldest;
}

static bool filed_negative_lookup_cache_get(
    filed_runtime_t *runtime,
    uint64_t parent_backend_object,
    filed_generation_t parent_dir_generation,
    const char *name,
    int64_t *out_status)
{
    const size_t name_len = filed_lookup_name_len(name);
    filed_negative_lookup_cache_slot_t *slot = filed_negative_lookup_cache_find(
        runtime,
        parent_backend_object,
        parent_dir_generation,
        name,
        name_len);
    if (slot == NULL) {
        filed_negative_lookup_cache.misses++;
        return false;
    }
    slot->last_used = filed_negative_lookup_cache_next_clock(runtime);
    if (out_status != NULL) {
        *out_status = slot->status;
    }
    filed_negative_lookup_cache.hits++;
    return true;
}

static void filed_negative_lookup_cache_store(
    filed_runtime_t *runtime,
    uint64_t parent_backend_object,
    filed_generation_t parent_dir_generation,
    const char *name,
    int64_t status)
{
    const size_t name_len = filed_lookup_name_len(name);
    filed_negative_lookup_cache_slot_t *slot;
    if (parent_backend_object == 0 ||
        name == NULL ||
        name_len == 0 ||
        name_len >= FILED_V2_NAME_BYTES ||
        status == 0)
    {
        return;
    }
    slot = filed_negative_lookup_cache_find(
        runtime,
        parent_backend_object,
        parent_dir_generation,
        name,
        name_len);
    if (slot == NULL) {
        slot = filed_negative_lookup_cache_choose_slot(runtime);
    }
    if (slot == NULL) {
        return;
    }

    memset(slot, 0, sizeof(*slot));
    slot->valid = true;
    slot->parent_backend_object = parent_backend_object;
    slot->parent_dir_generation = parent_dir_generation;
    slot->last_used = filed_negative_lookup_cache_next_clock(runtime);
    slot->status = status;
    memcpy(slot->name, name, name_len);
    slot->name[name_len] = '\0';
    filed_negative_lookup_cache.stores++;
}
