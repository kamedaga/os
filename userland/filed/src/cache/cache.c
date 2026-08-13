#include "../dispatch/common.h"
#include "internal.h"

static void filed_file_vmo_cache_invalidate_object(filed_runtime_t *runtime, uint64_t backend_object);
static void filed_file_vmo_cache_release_object(filed_runtime_t *runtime, uint64_t backend_object);

static uint64_t filed_cache_next_object_clock(filed_runtime_t *runtime)
{
    if (filed_cache.object_clock == UINT64_MAX) {
        filed_cache.object_clock = 0;
    }
    ++filed_cache.object_clock;
    if (filed_cache.object_clock == 0) {
        filed_cache.object_clock = 1;
    }
    return filed_cache.object_clock;
}

void filed_cache_note_attachment(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint32_t attachment)
{
    filed_cache_object_entry_t *oldest = NULL;
    if (runtime == NULL || backend_object == 0 || attachment == 0) {
        return;
    }
    for (size_t i = 0; i < FILED_CACHE_OBJECT_SLOTS; ++i) {
        filed_cache_object_entry_t *entry = &filed_cache.objects[i];
        if (entry->valid && entry->backend_object == backend_object) {
            entry->attachment_mask |= attachment;
            entry->last_used = filed_cache_next_object_clock(runtime);
            return;
        }
        if (!entry->valid) {
            memset(entry, 0, sizeof(*entry));
            entry->valid = true;
            entry->backend_object = backend_object;
            entry->attachment_mask = attachment;
            entry->last_used = filed_cache_next_object_clock(runtime);
            return;
        }
        if (oldest == NULL || entry->last_used < oldest->last_used) {
            oldest = entry;
        }
    }
    if (oldest != NULL) {
        memset(oldest, 0, sizeof(*oldest));
        oldest->valid = true;
        oldest->backend_object = backend_object;
        oldest->attachment_mask = attachment;
        oldest->last_used = filed_cache_next_object_clock(runtime);
    }
}

void filed_cache_configure(filed_runtime_t *runtime, uint64_t active_slots)
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
        filed_cache_configure(runtime, FILED_PAGE_CACHE_SLOTS);
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

bool filed_cache_object_dirty(filed_runtime_t *runtime, uint64_t backend_object)
{
    filed_page_cache_ensure_configured(runtime);
    if (backend_object == 0) {
        return false;
    }
    filed_file_vmo_cache_entry_t *shared =
        filed_file_vmo_cache_shared_lookup(runtime, backend_object);
    if (shared != NULL && (shared->dirty || shared->writable_lent)) {
        return true;
    }
    if (filed_backend_object_is_tmpfs(backend_object) || filed_page_cache.active_slots == 0) {
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

bool filed_cache_object_evictable(filed_runtime_t *runtime, uint64_t backend_object)
{
    if (runtime == NULL || backend_object == 0) {
        return false;
    }
    for (uint64_t i = 0; i < FILED_RUNTIME_FILE_VMO_CACHE_SLOTS; ++i) {
        const filed_file_vmo_cache_entry_t *entry = &filed_file_vmo_cache.entries[i];
        /*
         * A non-shared FILE_VMO is an immutable snapshot.  Once its contents
         * have been copied into the VMO, the cache entry no longer needs the
         * backend object or its vnode to remain resident.  Reclaiming that
         * vnode closes the cache's VMO owner through
         * filed_cache_release_object(); processes keep their transferred VMO
         * reference.
         *
         * A shared VMO is different: it remains an I/O source and may still
         * require writeback/revocation, so it must continue to pin the object.
         */
        if (entry->active && entry->shared &&
            entry->backend_object == backend_object)
        {
            return false;
        }
    }
    return true;
}

uint64_t filed_cache_dirty_count(filed_runtime_t *runtime)
{
    uint64_t count = 0;
    filed_page_cache_ensure_configured(runtime);
    for (size_t i = 0; i < filed_page_cache.active_slots; ++i) {
        const filed_page_cache_slot_t *slot = &filed_page_cache.slots[i];
        if (slot->valid && slot->dirty) {
            count++;
        }
    }
    for (size_t i = 0; i < FILED_RUNTIME_FILE_VMO_CACHE_SLOTS; ++i) {
        const filed_file_vmo_cache_entry_t *entry = &filed_file_vmo_cache.entries[i];
        if (entry->active && entry->shared && (entry->dirty || entry->writable_lent)) {
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
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=page_cache_validate object=%llu "
            "page=%llu bytes=%u valid_start=%u dirty_start=%u dirty_end=%u\n",
            (unsigned long long)(slot != NULL ? slot->backend_object : 0),
            (unsigned long long)(slot != NULL ? slot->page_index : 0),
            slot != NULL ? slot->bytes : 0,
            slot != NULL ? slot->valid_start : 0,
            slot != NULL ? slot->dirty_start : 0,
            slot != NULL ? slot->dirty_end : 0);
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
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=page_cache_pwrite status=%d "
            "object=%llu page=%llu offset=%llu length=%llu bytes=%llu\n",
            status,
            (unsigned long long)slot->backend_object,
            (unsigned long long)slot->page_index,
            (unsigned long long)offset,
            (unsigned long long)length,
            (unsigned long long)bytes);
        return status != 0 ? status : -5;
    }

    slot->dirty = false;
    slot->dirty_start = 0;
    slot->dirty_end = 0;
    filed_page_cache.flushes++;
    return 0;
}

static int filed_shared_vmo_flush_entry(
    filed_runtime_t *runtime,
    filed_file_vmo_cache_entry_t *entry)
{
    if (entry == NULL || !entry->active || !entry->shared || entry->mapped == NULL ||
        (!entry->dirty && !entry->writable_lent))
    {
        return 0;
    }
    uint64_t offset = 0;
    while (offset < entry->logical_size) {
        uint64_t chunk = entry->logical_size - offset;
        if (chunk > STORAGE_IO_BYTES) {
            chunk = STORAGE_IO_BYTES;
        }
        uint64_t bytes = 0;
        const int status = filed_backend_pwrite(
            runtime,
            entry->backend_object,
            offset,
            (const uint8_t *)entry->mapped + offset,
            chunk,
            &bytes);
        if (status != 0 || bytes != chunk) {
            filed_page_cache.flush_errors++;
            return status != 0 ? status : -5;
        }
        offset += bytes;
    }
    entry->dirty = 0;
    filed_page_cache.flushes++;
    return 0;
}

int filed_cache_flush_object(filed_runtime_t *runtime, uint64_t backend_object)
{
    filed_page_cache_ensure_configured(runtime);
    for (size_t i = 0; i < FILED_RUNTIME_FILE_VMO_CACHE_SLOTS; ++i) {
        filed_file_vmo_cache_entry_t *entry = &filed_file_vmo_cache.entries[i];
        if (entry->active && entry->shared &&
            (backend_object == 0 || entry->backend_object == backend_object))
        {
            const int status = filed_shared_vmo_flush_entry(runtime, entry);
            if (status != 0) {
                return status;
            }
        }
    }
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
    filed_cache_note_attachment(runtime, backend_object, FILED_CACHE_ATTACHMENT_PAGE);
}

static void filed_page_cache_invalidate_object(filed_runtime_t *runtime, uint64_t backend_object)
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
            filed_cache_note_attachment(runtime, backend_object, FILED_CACHE_ATTACHMENT_PAGE);
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
    filed_file_vmo_cache_entry_t *shared =
        filed_file_vmo_cache_shared_lookup(runtime, backend_object);
    if (shared != NULL && shared->mapped != NULL) {
        if (offset >= shared->logical_size) {
            return 0;
        }
        uint64_t available = shared->logical_size - offset;
        if (length > available) {
            length = available;
        }
        memcpy(buffer, (const uint8_t *)shared->mapped + offset, (size_t)length);
        shared->clock = ++filed_file_vmo_cache.clock;
        *out_bytes = length;
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
        const int flush_status = filed_cache_flush_object(runtime, backend_object);
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

int filed_cached_pwrite_ex(
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
    filed_file_vmo_cache_entry_t *shared =
        filed_file_vmo_cache_shared_lookup(runtime, backend_object);
    if (shared != NULL && shared->mapped != NULL) {
        if (offset + length <= shared->length) {
            memcpy((uint8_t *)shared->mapped + offset, buffer, (size_t)length);
            if (offset + length > shared->logical_size) {
                shared->logical_size = offset + length;
            }
            shared->dirty = 1;
            shared->clock = ++filed_file_vmo_cache.clock;
            *out_bytes = length;
            return 0;
        }
        int status = filed_shared_vmo_flush_entry(runtime, shared);
        if (status != 0) {
            return status;
        }
        filed_file_vmo_cache_invalidate_object(runtime, backend_object);
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
        filed_file_vmo_cache_invalidate_object(runtime, backend_object);
        *out_bytes = length;
        return 0;
    }

    int status = filed_cache_flush_object(runtime, backend_object);
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
        filed_file_vmo_cache_invalidate_object(runtime, backend_object);
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

int filed_dir_cache_get(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    storage_getdents_request_t *out_entries)
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

void filed_dir_cache_store(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t offset,
    const storage_getdents_request_t *entries)
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
    filed_cache_note_attachment(runtime, backend_object, FILED_CACHE_ATTACHMENT_DIRENT);
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
    size_t len = 0;
    if (name == NULL) {
        return 0;
    }
    while (len < FILED_NAME_BYTES && name[len] != '\0') {
        ++len;
    }
    return len;
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
        name_len >= FILED_NAME_BYTES)
    {
        return NULL;
    }
    for (size_t i = 0; i < FILED_NEGATIVE_LOOKUP_CACHE_SLOTS; ++i) {
        filed_negative_lookup_cache_slot_t *slot = &filed_negative_lookup_cache.slots[i];
        if (slot->valid &&
            slot->parent_backend_object == parent_backend_object &&
            slot->parent_dir_generation == parent_dir_generation &&
            strncmp(slot->name, name, FILED_NAME_BYTES) == 0)
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

bool filed_negative_lookup_cache_get(
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

void filed_negative_lookup_cache_store(
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
        name_len >= FILED_NAME_BYTES ||
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
    filed_cache_note_attachment(runtime, parent_backend_object, FILED_CACHE_ATTACHMENT_NEGATIVE);
}

static void filed_file_vmo_cache_clear_entry(
    filed_file_vmo_cache_entry_t *entry,
    bool revoke_shared)
{
    if (entry == NULL || !entry->active) {
        return;
    }
    if (entry->shared && revoke_shared && entry->vmo_fd >= 16) {
        if (pacha_vmo_revoke(entry->vmo_fd) != 0) {
            if (entry->mapped != NULL) {
                (void)pacha_munmap(entry->mapped, entry->length);
            }
            (void)pacha_fd_close(entry->vmo_fd);
        }
    } else {
        if (entry->mapped != NULL && entry->vmo_fd >= 16) {
            (void)pacha_munmap(entry->mapped, entry->length);
        }
        if (entry->vmo_fd >= 16) {
            (void)pacha_fd_close(entry->vmo_fd);
        }
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
        filed_file_vmo_cache_entry_t *entry = &filed_file_vmo_cache.entries[i];
        if (entry->active && entry->backend_object == backend_object) {
            filed_file_vmo_cache_clear_entry(entry, entry->shared != 0);
        }
    }
}

static void filed_file_vmo_cache_release_object(filed_runtime_t *runtime, uint64_t backend_object)
{
    if (runtime == NULL || backend_object == 0) {
        return;
    }
    for (uint64_t i = 0; i < FILED_RUNTIME_FILE_VMO_CACHE_SLOTS; ++i) {
        filed_file_vmo_cache_entry_t *entry = &filed_file_vmo_cache.entries[i];
        if (entry->active && entry->backend_object == backend_object) {
            filed_file_vmo_cache_clear_entry(entry, false);
        }
    }
}

filed_file_vmo_cache_entry_t *filed_file_vmo_cache_lookup(
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
        filed_file_vmo_cache_entry_t *entry = &filed_file_vmo_cache.entries[i];
        if (entry->active && !entry->shared &&
            entry->backend_object == backend_object &&
            entry->object_generation == object_generation &&
            entry->file_offset == file_offset &&
            entry->length == length)
        {
            entry->clock = ++filed_file_vmo_cache.clock;
            filed_file_vmo_cache_hits++;
            return entry;
        }
    }
    filed_file_vmo_cache_misses++;
    return NULL;
}

filed_file_vmo_cache_entry_t *filed_file_vmo_cache_slot(filed_runtime_t *runtime)
{
    return filed_file_vmo_cache_slot_for_length(runtime, 0);
}

filed_file_vmo_cache_entry_t *filed_file_vmo_cache_slot_for_length(
    filed_runtime_t *runtime,
    uint64_t length)
{
    filed_file_vmo_cache_entry_t *oldest_entry = NULL;
    uint64_t oldest = UINT64_MAX;
    if (runtime == NULL || length > FILED_FILE_VMO_CACHE_TOTAL_BYTES) {
        return NULL;
    }
    if (length != 0) {
        uint64_t active_bytes = 0;
        for (uint64_t i = 0; i < FILED_RUNTIME_FILE_VMO_CACHE_SLOTS; ++i) {
            const filed_file_vmo_cache_entry_t *entry = &filed_file_vmo_cache.entries[i];
            if (entry->active) {
                if (entry->length > FILED_FILE_VMO_CACHE_TOTAL_BYTES - active_bytes) {
                    active_bytes = FILED_FILE_VMO_CACHE_TOTAL_BYTES;
                    break;
                }
                active_bytes += entry->length;
            }
        }
        while (active_bytes > FILED_FILE_VMO_CACHE_TOTAL_BYTES - length) {
            oldest_entry = NULL;
            oldest = UINT64_MAX;
            for (uint64_t i = 0; i < FILED_RUNTIME_FILE_VMO_CACHE_SLOTS; ++i) {
                filed_file_vmo_cache_entry_t *entry = &filed_file_vmo_cache.entries[i];
                if (entry->active && !entry->shared && entry->clock < oldest) {
                    oldest = entry->clock;
                    oldest_entry = entry;
                }
            }
            if (oldest_entry == NULL) {
                return NULL;
            }
            active_bytes = oldest_entry->length > active_bytes ?
                0 : active_bytes - oldest_entry->length;
            filed_file_vmo_cache_evictions++;
            filed_file_vmo_cache_clear_entry(oldest_entry, false);
        }
    }
    oldest_entry = NULL;
    oldest = UINT64_MAX;
    for (uint64_t i = 0; i < FILED_RUNTIME_FILE_VMO_CACHE_SLOTS; ++i) {
        filed_file_vmo_cache_entry_t *entry = &filed_file_vmo_cache.entries[i];
        if (!entry->active) {
            return entry;
        }
        if (!entry->shared && entry->clock < oldest) {
            oldest = entry->clock;
            oldest_entry = entry;
        }
    }
    if (oldest_entry == NULL) {
        return NULL;
    }
    filed_file_vmo_cache_evictions++;
    filed_file_vmo_cache_clear_entry(oldest_entry, false);
    return oldest_entry;
}

uint32_t filed_file_vmo_cache_reclaim_snapshots(
    filed_runtime_t *runtime,
    uint64_t *out_bytes)
{
    uint32_t reclaimed_entries = 0;
    uint64_t reclaimed_bytes = 0;
    if (out_bytes != NULL) {
        *out_bytes = 0;
    }
    if (runtime == NULL) {
        return 0;
    }
    for (uint64_t i = 0; i < FILED_RUNTIME_FILE_VMO_CACHE_SLOTS; ++i) {
        filed_file_vmo_cache_entry_t *entry = &filed_file_vmo_cache.entries[i];
        if (!entry->active || entry->shared) {
            continue;
        }
        reclaimed_bytes = entry->length > UINT64_MAX - reclaimed_bytes ?
            UINT64_MAX : reclaimed_bytes + entry->length;
        filed_file_vmo_cache_evictions++;
        filed_file_vmo_cache_clear_entry(entry, false);
        reclaimed_entries++;
    }
    if (out_bytes != NULL) {
        *out_bytes = reclaimed_bytes;
    }
    return reclaimed_entries;
}

filed_file_vmo_cache_entry_t *filed_file_vmo_cache_shared_lookup(
    filed_runtime_t *runtime,
    uint64_t backend_object)
{
    if (runtime == NULL || backend_object == 0) {
        return NULL;
    }
    for (uint64_t i = 0; i < FILED_RUNTIME_FILE_VMO_CACHE_SLOTS; ++i) {
        filed_file_vmo_cache_entry_t *entry = &filed_file_vmo_cache.entries[i];
        if (entry->active && entry->shared && entry->backend_object == backend_object) {
            entry->clock = ++filed_file_vmo_cache.clock;
            return entry;
        }
    }
    return NULL;
}

int filed_cache_create_shared_vmo(
    filed_runtime_t *runtime,
    uint64_t backend_object,
    uint64_t object_generation,
    uint64_t logical_size,
    uint64_t required_end,
    filed_file_vmo_cache_entry_t **out_entry)
{
    if (runtime == NULL || backend_object == 0 || out_entry == NULL || required_end == 0)
    {
        return -22;
    }
    *out_entry = NULL;
    uint64_t capacity = logical_size > required_end ? logical_size : required_end;
    if (capacity > UINT64_MAX - 4095u) {
        return -75;
    }
    capacity = (capacity + 4095u) & ~4095ull;
    if (capacity == 0) {
        return -27;
    }

    filed_file_vmo_cache_entry_t *existing =
        filed_file_vmo_cache_shared_lookup(runtime, backend_object);
    if (existing != NULL) {
        if (existing->length >= capacity) {
            if (logical_size > existing->logical_size) {
                existing->logical_size = logical_size;
            }
            *out_entry = existing;
            return 0;
        }
        const int flush_status = filed_shared_vmo_flush_entry(runtime, existing);
        if (flush_status != 0) {
            return flush_status;
        }
        filed_file_vmo_cache_clear_entry(existing, true);
    }

    int status = filed_cache_flush_object(runtime, backend_object);
    if (status != 0) {
        return status;
    }
    const uint64_t rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_MAP_EXEC |
        PACHA_FD_RIGHT_REVOKE;
    /* Enforce the cache byte budget before allocating the replacement.  This
     * keeps resize/create from temporarily requiring old + new VMO capacity. */
    filed_file_vmo_cache_entry_t *entry =
        filed_file_vmo_cache_slot_for_length(runtime, capacity);
    if (entry == NULL) {
        return -28;
    }
    int vmo_fd = pacha_vmo_create(capacity, rights, 0);
    if (vmo_fd < 16) {
        uint64_t reclaimed_bytes = 0;
        const uint32_t reclaimed_entries =
            filed_file_vmo_cache_reclaim_snapshots(runtime, &reclaimed_bytes);
        if (reclaimed_entries != 0) {
            fprintf(stderr,
                "[filed] file_vmo_allocation_retry kind=shared length=%llu "
                "reclaimed_entries=%u reclaimed_bytes=%llu\n",
                (unsigned long long)capacity,
                reclaimed_entries,
                (unsigned long long)reclaimed_bytes);
            vmo_fd = pacha_vmo_create(capacity, rights, 0);
        }
    }
    if (vmo_fd < 16) {
        return -12;
    }
    void *mapped = pacha_mmap(
        vmo_fd,
        capacity,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (mapped == NULL) {
        (void)pacha_fd_close(vmo_fd);
        return -12;
    }

    uint64_t loaded = 0;
    while (loaded < logical_size) {
        uint64_t chunk = logical_size - loaded;
        if (chunk > STORAGE_IO_BYTES) {
            chunk = STORAGE_IO_BYTES;
        }
        uint64_t bytes = 0;
        status = filed_backend_pread(
            runtime,
            backend_object,
            loaded,
            (uint8_t *)mapped + loaded,
            chunk,
            &bytes);
        if (status != 0) {
            (void)pacha_munmap(mapped, capacity);
            (void)pacha_fd_close(vmo_fd);
            return status;
        }
        if (bytes == 0) {
            break;
        }
        loaded += bytes;
    }

    filed_page_cache_invalidate_object(runtime, backend_object);
    memset(entry, 0, sizeof(*entry));
    entry->active = 1;
    entry->shared = 1;
    entry->vmo_fd = vmo_fd;
    entry->backend_object = backend_object;
    entry->object_generation = object_generation;
    entry->length = capacity;
    entry->logical_size = logical_size;
    entry->clock = ++filed_file_vmo_cache.clock;
    entry->mapped = mapped;
    filed_cache_note_attachment(runtime, backend_object, FILED_CACHE_ATTACHMENT_VMO);
    filed_file_vmo_cache_stores++;
    *out_entry = entry;
    return 0;
}

void filed_cache_invalidate(filed_runtime_t *runtime, uint64_t backend_object)
{
    if (runtime == NULL || backend_object == 0) {
        return;
    }
    filed_page_cache_invalidate_object(runtime, backend_object);
    filed_dir_cache_invalidate_dir(runtime, backend_object);
    for (size_t i = 0; i < FILED_NEGATIVE_LOOKUP_CACHE_SLOTS; ++i) {
        filed_negative_lookup_cache_slot_t *slot = &filed_negative_lookup_cache.slots[i];
        if (slot->valid && slot->parent_backend_object == backend_object) {
            memset(slot, 0, sizeof(*slot));
        }
    }
    filed_file_vmo_cache_invalidate_object(runtime, backend_object);
    for (size_t i = 0; i < FILED_CACHE_OBJECT_SLOTS; ++i) {
        filed_cache_object_entry_t *entry = &filed_cache.objects[i];
        if (entry->valid && entry->backend_object == backend_object) {
            memset(entry, 0, sizeof(*entry));
            return;
        }
    }
}

void filed_cache_release_object(filed_runtime_t *runtime, uint64_t backend_object)
{
    if (runtime == NULL || backend_object == 0) {
        return;
    }
    filed_page_cache_invalidate_object(runtime, backend_object);
    filed_dir_cache_invalidate_dir(runtime, backend_object);
    for (size_t i = 0; i < FILED_NEGATIVE_LOOKUP_CACHE_SLOTS; ++i) {
        filed_negative_lookup_cache_slot_t *slot = &filed_negative_lookup_cache.slots[i];
        if (slot->valid && slot->parent_backend_object == backend_object) {
            memset(slot, 0, sizeof(*slot));
        }
    }
    filed_file_vmo_cache_release_object(runtime, backend_object);
    for (size_t i = 0; i < FILED_CACHE_OBJECT_SLOTS; ++i) {
        filed_cache_object_entry_t *entry = &filed_cache.objects[i];
        if (entry->valid && entry->backend_object == backend_object) {
            memset(entry, 0, sizeof(*entry));
            return;
        }
    }
}
