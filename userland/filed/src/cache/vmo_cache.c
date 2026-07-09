static void filed_file_vmo_cache_clear_entry(filed_file_vmo_cache_entry_t *entry)
{
    if (entry == NULL || !entry->active) {
        return;
    }
    if (entry->vmo_fd >= 16) {
        (void)pacha_fd_close(entry->vmo_fd);
    }
    memset(entry, 0, sizeof(*entry));
    entry->vmo_fd = -1;
}

static void filed_file_vmo_cache_invalidate_object(filed_runtime_t *runtime, uint64_t backend_object)
{
    if (runtime == NULL || backend_object == 0) {
        return;
    }
    for (uint64_t i = 0; i < FILED_RUNTIME_FILE_VMO_CACHE_SLOTS; ++i) {
        filed_file_vmo_cache_entry_t *entry = &runtime->dispatch_state->file_vmo_cache[i];
        if (entry->active && entry->backend_object == backend_object) {
            filed_file_vmo_cache_clear_entry(entry);
        }
    }
}

static filed_file_vmo_cache_entry_t *filed_file_vmo_cache_lookup(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t object_generation,
    uint64_t file_offset,
    uint64_t length)
{
    if (runtime == NULL || backend_object == 0 || length == 0) {
        return NULL;
    }
    for (uint64_t i = 0; i < FILED_RUNTIME_FILE_VMO_CACHE_SLOTS; ++i) {
        filed_file_vmo_cache_entry_t *entry = &runtime->dispatch_state->file_vmo_cache[i];
        if (entry->active &&
            entry->backend_object == backend_object &&
            entry->object_generation == object_generation &&
            entry->file_offset == file_offset &&
            entry->length == length)
        {
            entry->clock = ++runtime->dispatch_state->file_vmo_cache_clock;
            filed_file_vmo_cache_hits++;
            return entry;
        }
    }
    filed_file_vmo_cache_misses++;
    return NULL;
}

static filed_file_vmo_cache_entry_t *filed_file_vmo_cache_slot(filed_runtime_t *runtime)
{
    uint64_t slot = 0;
    uint64_t oldest = UINT64_MAX;
    for (uint64_t i = 0; i < FILED_RUNTIME_FILE_VMO_CACHE_SLOTS; ++i) {
        filed_file_vmo_cache_entry_t *entry = &runtime->dispatch_state->file_vmo_cache[i];
        if (!entry->active) {
            return entry;
        }
        if (entry->clock < oldest) {
            oldest = entry->clock;
            slot = i;
        }
    }
    filed_file_vmo_cache_evictions++;
    filed_file_vmo_cache_clear_entry(&runtime->dispatch_state->file_vmo_cache[slot]);
    return &runtime->dispatch_state->file_vmo_cache[slot];
}

