void filed_page_cache_configure(filed_runtime_t *runtime, uint64_t active_slots)
{
    if (active_slots > FILED_PAGE_CACHE_SLOTS) {
        active_slots = FILED_PAGE_CACHE_SLOTS;
    }
    if (!filed_page_cache.configured ||
        filed_page_cache.active_slots != active_slots)
    {
        for (size_t i = 0; i < FILED_PAGE_CACHE_SLOTS; ++i) {
            memset(&filed_page_cache.slots[i], 0, sizeof(filed_page_cache.slots[i]));
        }
    }
    filed_page_cache.active_slots = active_slots;
    filed_page_cache.configured = true;
}

static void filed_page_cache_ensure_configured(filed_runtime_t *runtime)
{
    if (!filed_page_cache.configured) {
        filed_page_cache_configure(runtime, FILED_PAGE_CACHE_SLOTS);
    }
}

static uint64_t filed_page_cache_next_clock(filed_runtime_t *runtime)
{
    (void)runtime;
    if (filed_page_cache.clock == UINT64_MAX) {
        filed_page_cache.clock = 0;
    }
    ++filed_page_cache.clock;
    if (filed_page_cache.clock == 0) {
        filed_page_cache.clock = 1;
    }
    return filed_page_cache.clock;
}

static filed_page_cache_slot_t *filed_page_cache_find(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t page_index)
{
    (void)runtime;
    filed_page_cache_ensure_configured(runtime);
    if (backend_object == 0) {
        return 0;
    }
    for (size_t i = 0; i < filed_page_cache.active_slots; ++i) {
        filed_page_cache_slot_t *slot = &filed_page_cache.slots[i];
        if (slot->valid &&
            slot->backend_object == backend_object &&
            slot->page_index == page_index)
        {
            return slot;
        }
    }
    return NULL;
}

static bool filed_page_cache_object_dirty(filed_runtime_t *runtime, uint64_t backend_object)
{
    filed_page_cache_ensure_configured(runtime);
    if (filed_backend_object_is_tmpfs(backend_object)) {
        return false;
    }
    if (backend_object == 0 || filed_page_cache.active_slots == 0) {
        return false;
    }
    for (size_t i = 0; i < filed_page_cache.active_slots; ++i) {
        const filed_page_cache_slot_t *slot = &filed_page_cache.slots[i];
        if (slot->valid && slot->dirty && slot->backend_object == backend_object) {
            return true;
        }
    }
    return false;
}

uint64_t filed_page_cache_dirty_count(filed_runtime_t *runtime)
{
    uint64_t count = 0;
    filed_page_cache_ensure_configured(runtime);
    for (size_t i = 0; i < filed_page_cache.active_slots; ++i) {
        const filed_page_cache_slot_t *slot = &filed_page_cache.slots[i];
        if (slot->valid && slot->dirty) {
            count++;
        }
    }
    return count;
}

static int filed_page_cache_flush_slot(
    filed_runtime_t *runtime,
    filed_page_cache_slot_t *slot)
{
    if (slot == NULL || !slot->valid || !slot->dirty) {
        return 0;
    }
    if (runtime == NULL ||
        slot->backend_object == 0 ||
        slot->dirty_start >= slot->dirty_end ||
        slot->dirty_start < slot->valid_start ||
        slot->dirty_end > slot->bytes ||
        slot->dirty_end > FILED_PAGE_CACHE_BYTES ||
        slot->page_index > UINT64_MAX / FILED_PAGE_CACHE_BYTES)
    {
        filed_page_cache.flush_errors++;
        return -22;
    }

    const uint64_t page_start = slot->page_index * FILED_PAGE_CACHE_BYTES;
    const uint64_t offset = page_start + slot->dirty_start;
    const uint64_t length = (uint64_t)slot->dirty_end - slot->dirty_start;
    uint64_t bytes = 0;
    const int status = filed_backend_pwrite(
        runtime,
        slot->backend_object,
        offset,
        slot->data + slot->dirty_start,
        length,
        &bytes);
    if (status != 0 || bytes != length) {
        filed_page_cache.flush_errors++;
        return status != 0 ? status : -5;
    }

    slot->dirty = false;
    slot->dirty_start = 0;
    slot->dirty_end = 0;
    filed_page_cache.flushes++;
    return 0;
}

int filed_page_cache_flush_object(filed_runtime_t *runtime, uint64_t backend_object)
{
    filed_page_cache_ensure_configured(runtime);
    if (filed_backend_object_is_tmpfs(backend_object)) {
        return 0;
    }
    if (filed_page_cache.active_slots == 0) {
        return 0;
    }
    for (size_t i = 0; i < filed_page_cache.active_slots; ++i) {
        filed_page_cache_slot_t *slot = &filed_page_cache.slots[i];
        if (slot->valid && slot->dirty &&
            (backend_object == 0 || slot->backend_object == backend_object))
        {
            const int status = filed_page_cache_flush_slot(runtime, slot);
            if (status != 0) {
                return status;
            }
        }
    }
    return 0;
}

static filed_page_cache_slot_t *filed_page_cache_choose_slot(filed_runtime_t *runtime)
{
    filed_page_cache_slot_t *oldest = NULL;
    (void)runtime;
    filed_page_cache_ensure_configured(runtime);
    if (filed_page_cache.active_slots == 0) {
        return NULL;
    }
    for (size_t i = 0; i < filed_page_cache.active_slots; ++i) {
        filed_page_cache_slot_t *slot = &filed_page_cache.slots[i];
        if (!slot->valid) {
            return slot;
        }
        if (slot->dirty) {
            continue;
        }
        if (oldest == NULL || slot->last_used < oldest->last_used) {
            oldest = slot;
        }
    }
    if (oldest != NULL) {
        filed_page_cache.evictions++;
    }
    return oldest;
}

static void filed_page_cache_store(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t page_index,
    const void *data,
    uint64_t bytes)
{
    if (backend_object == 0 || data == NULL || bytes == 0) {
        return;
    }
    filed_page_cache_ensure_configured(runtime);
    if (filed_page_cache.active_slots == 0) {
        return;
    }
    if (bytes > FILED_PAGE_CACHE_BYTES) {
        bytes = FILED_PAGE_CACHE_BYTES;
    }

    filed_page_cache_slot_t *slot = filed_page_cache_find(runtime, backend_object, page_index);
    if (slot != NULL && slot->dirty) {
        return;
    }
    if (slot == NULL) {
        slot = filed_page_cache_choose_slot(runtime);
    }
    if (slot == NULL) {
        return;
    }

    memcpy(slot->data, data, (size_t)bytes);
    slot->valid = true;
    slot->dirty = false;
    slot->backend_object = backend_object;
    slot->page_index = page_index;
    slot->bytes = (uint32_t)bytes;
    slot->valid_start = 0;
    slot->dirty_start = 0;
    slot->dirty_end = 0;
    slot->last_used = filed_page_cache_next_clock(runtime);
}

void filed_page_cache_invalidate_object(filed_runtime_t *runtime, uint64_t backend_object)
{
    (void)runtime;
    filed_page_cache_ensure_configured(runtime);
    if (filed_backend_object_is_tmpfs(backend_object)) {
        return;
    }
    for (size_t i = 0; i < filed_page_cache.active_slots; ++i) {
        filed_page_cache_slot_t *slot = &filed_page_cache.slots[i];
        if (slot->valid && (backend_object == 0 || slot->backend_object == backend_object)) {
            memset(slot, 0, sizeof(*slot));
        }
    }
}

static void filed_page_cache_invalidate_page(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t page_index)
{
    filed_page_cache_slot_t *slot = filed_page_cache_find(runtime, backend_object, page_index);
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
}

static int filed_page_cache_try_dirty_write(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    const void *data,
    uint64_t length,
    bool allow_extend_slot)
{
    uint64_t total = 0;

    if (runtime == NULL || backend_object == 0 || data == NULL || length == 0) {
        return 0;
    }
    filed_page_cache_ensure_configured(runtime);
    if (filed_page_cache.active_slots == 0) {
        return 0;
    }

    while (total < length) {
        const uint64_t absolute_offset = offset + total;
        if (absolute_offset < offset) {
            return 0;
        }
        const uint64_t page_index = absolute_offset / FILED_PAGE_CACHE_BYTES;
        const uint64_t page_offset = absolute_offset % FILED_PAGE_CACHE_BYTES;
        uint64_t chunk = length - total;
        if (chunk > FILED_PAGE_CACHE_BYTES - page_offset) {
            chunk = FILED_PAGE_CACHE_BYTES - page_offset;
        }

        filed_page_cache_slot_t *slot =
            filed_page_cache_find(runtime, backend_object, page_index);
        if (slot == NULL) {
            if (!allow_extend_slot || page_offset + chunk > FILED_PAGE_CACHE_BYTES) {
                return 0;
            }
        } else if (page_offset < slot->valid_start ||
            page_offset > slot->bytes ||
            page_offset + chunk > FILED_PAGE_CACHE_BYTES)
        {
            return 0;
        }
        total += chunk;
    }

    total = 0;
    while (total < length) {
        const uint64_t absolute_offset = offset + total;
        const uint64_t page_index = absolute_offset / FILED_PAGE_CACHE_BYTES;
        const uint64_t page_offset = absolute_offset % FILED_PAGE_CACHE_BYTES;
        uint64_t chunk = length - total;
        if (chunk > FILED_PAGE_CACHE_BYTES - page_offset) {
            chunk = FILED_PAGE_CACHE_BYTES - page_offset;
        }

        filed_page_cache_slot_t *slot =
            filed_page_cache_find(runtime, backend_object, page_index);
        if (slot == NULL) {
            slot = filed_page_cache_choose_slot(runtime);
            if (slot == NULL) {
                return 0;
            }
            memset(slot, 0, sizeof(*slot));
            slot->valid = true;
            slot->backend_object = backend_object;
            slot->page_index = page_index;
            slot->valid_start = (uint32_t)page_offset;
            slot->bytes = (uint32_t)page_offset;
        }
        if (page_offset < slot->valid_start ||
            page_offset > slot->bytes ||
            page_offset + chunk > FILED_PAGE_CACHE_BYTES)
        {
            return 0;
        }
        memcpy(slot->data + page_offset, (const uint8_t *)data + total, (size_t)chunk);
        if (page_offset + chunk > slot->bytes) {
            slot->bytes = (uint32_t)(page_offset + chunk);
        }
        slot->dirty = true;
        if (slot->dirty_start == slot->dirty_end) {
            slot->dirty_start = (uint32_t)page_offset;
            slot->dirty_end = (uint32_t)(page_offset + chunk);
        } else {
            if (page_offset < slot->dirty_start) {
                slot->dirty_start = (uint32_t)page_offset;
            }
            if (page_offset + chunk > slot->dirty_end) {
                slot->dirty_end = (uint32_t)(page_offset + chunk);
            }
        }
        slot->last_used = filed_page_cache_next_clock(runtime);
        total += chunk;
    }

    filed_page_cache.dirty_writes++;
    return 1;
}

static void filed_page_cache_note_write(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    const void *data,
    uint64_t length)
{
    uint64_t total = 0;

    if (runtime == NULL || backend_object == 0 || data == NULL || length == 0) {
        return;
    }
    filed_page_cache_ensure_configured(runtime);
    if (filed_page_cache.active_slots == 0) {
        return;
    }

    while (total < length) {
        const uint64_t absolute_offset = offset + total;
        if (absolute_offset < offset) {
            return;
        }
        const uint64_t page_index = absolute_offset / FILED_PAGE_CACHE_BYTES;
        const uint64_t page_offset = absolute_offset % FILED_PAGE_CACHE_BYTES;
        filed_page_cache_slot_t *slot =
            filed_page_cache_find(runtime, backend_object, page_index);
        uint64_t chunk = length - total;
        if (chunk > FILED_PAGE_CACHE_BYTES - page_offset) {
            chunk = FILED_PAGE_CACHE_BYTES - page_offset;
        }

        if (slot != NULL && !slot->dirty) {
            if (page_offset >= slot->valid_start &&
                page_offset <= slot->bytes &&
                chunk <= FILED_PAGE_CACHE_BYTES - page_offset)
            {
                memcpy(slot->data + page_offset, (const uint8_t *)data + total, (size_t)chunk);
                if (page_offset + chunk > slot->bytes) {
                    slot->bytes = (uint32_t)(page_offset + chunk);
                }
                slot->last_used = filed_page_cache_next_clock(runtime);
            } else {
                filed_page_cache_invalidate_page(runtime, backend_object, page_index);
            }
        }
        total += chunk;
    }
}

int filed_cached_pread(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    uint64_t total = 0;
    uint8_t page_buffer[FILED_PAGE_CACHE_BYTES];

    if (runtime == NULL || backend_object == 0 || buffer == NULL || out_bytes == NULL) {
        return -1;
    }
    *out_bytes = 0;
    if (length == 0) {
        return 0;
    }
    if (filed_backend_object_is_tmpfs(backend_object)) {
        filed_page_cache.direct_reads++;
        return filed_backend_pread(
            runtime,
            backend_object,
            offset,
            buffer,
            length,
            out_bytes);
    }
    filed_page_cache_ensure_configured(runtime);
    if (length >= 65536u) {
        const int flush_status = filed_page_cache_flush_object(runtime, backend_object);
        if (flush_status != 0) {
            return flush_status;
        }
        filed_page_cache.direct_reads++;
        return filed_backend_pread(
            runtime,
            backend_object,
            offset,
            buffer,
            length,
            out_bytes);
    }
    if (filed_page_cache.active_slots == 0) {
        filed_page_cache.direct_reads++;
        return filed_backend_pread(
            runtime,
            backend_object,
            offset,
            buffer,
            length,
            out_bytes);
    }
    while (total < length) {
        const uint64_t absolute_offset = offset + total;
        if (absolute_offset < offset) {
            break;
        }
        const uint64_t page_index = absolute_offset / FILED_PAGE_CACHE_BYTES;
        const uint64_t page_offset = absolute_offset % FILED_PAGE_CACHE_BYTES;
        filed_page_cache_slot_t *slot =
            filed_page_cache_find(runtime, backend_object, page_index);

        if (slot != NULL && page_offset < slot->valid_start) {
            const int flush_status = filed_page_cache_flush_slot(runtime, slot);
            if (flush_status != 0) {
                return flush_status;
            }
            memset(slot, 0, sizeof(*slot));
            slot = NULL;
        }

        if (slot != NULL) {
            filed_page_cache.hits++;
            slot->last_used = filed_page_cache_next_clock(runtime);
        } else {
            uint64_t read_bytes = 0;
            const uint64_t page_start = absolute_offset - page_offset;
            filed_page_cache.misses++;
            memset(page_buffer, 0, sizeof(page_buffer));
            const int status = filed_backend_pread(
                runtime,
                backend_object,
                page_start,
                page_buffer,
                FILED_PAGE_CACHE_BYTES,
                &read_bytes);
            if (status != 0) {
                return status;
            }
            if (read_bytes == 0) {
                break;
            }
            filed_page_cache_store(runtime, backend_object, page_index, page_buffer, read_bytes);
            slot = filed_page_cache_find(runtime, backend_object, page_index);
            if (slot == NULL) {
                uint64_t available = read_bytes > page_offset ? read_bytes - page_offset : 0;
                uint64_t wanted = length - total;
                if (available == 0) {
                    break;
                }
                if (wanted > available) {
                    wanted = available;
                }
                memcpy(
                    (uint8_t *)buffer + total,
                    page_buffer + page_offset,
                    (size_t)wanted);
                total += wanted;
                if (read_bytes < FILED_PAGE_CACHE_BYTES) {
                    break;
                }
                continue;
            }
        }

        if (page_offset < slot->valid_start || page_offset >= slot->bytes) {
            break;
        }
        uint64_t available = (uint64_t)slot->bytes - page_offset;
        uint64_t wanted = length - total;
        if (wanted > available) {
            wanted = available;
        }
        memcpy((uint8_t *)buffer + total, slot->data + page_offset, (size_t)wanted);
        total += wanted;

        if (slot->bytes < FILED_PAGE_CACHE_BYTES) {
            break;
        }
    }

    *out_bytes = total;
    return 0;
}

static int filed_cached_pwrite_ex(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    const void *buffer,
    uint64_t length,
    uint64_t *out_bytes,
    bool allow_extend_slot)
{
    if (runtime == NULL || backend_object == 0 || buffer == NULL || out_bytes == NULL) {
        return -1;
    }
    *out_bytes = 0;
    if (length == 0) {
        return 0;
    }
    if (offset + length < offset) {
        return -75;
    }
    if (filed_backend_object_is_tmpfs(backend_object)) {
        return filed_backend_pwrite(
            runtime,
            backend_object,
            offset,
            buffer,
            length,
            out_bytes);
    }
    if (filed_page_cache_try_dirty_write(
            runtime,
            backend_object,
            offset,
            buffer,
            length,
            allow_extend_slot))
    {
        *out_bytes = length;
        return 0;
    }

    int status = filed_page_cache_flush_object(runtime, backend_object);
    if (status != 0) {
        return status;
    }

    status = filed_backend_pwrite(
        runtime,
        backend_object,
        offset,
        buffer,
        length,
        out_bytes);
    if (status == 0) {
        filed_page_cache_note_write(runtime, backend_object, offset, buffer, *out_bytes);
    }
    return status;
}

int filed_cached_pwrite(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    const void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    return filed_cached_pwrite_ex(
        runtime,
        backend_object,
        offset,
        buffer,
        length,
        out_bytes,
        false);
}

