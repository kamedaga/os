#include "../lpr_filed_internal.h"

void lpr_readlink_cache_clear(void)
{
    lpr_memset(lpr_readlink_cache, 0, sizeof(lpr_readlink_cache));
    lpr_readlink_cache_clock = 0;
}

int lpr_readlink_cache_lookup(const char *path, uint64_t length, int64_t *out_status)
{
    if (path == 0 || out_status == 0 || length == 0 || length >= FILED_PATH_BYTES) {
        return 0;
    }
    for (uint64_t i = 0; i < LPR_READLINK_CACHE_ENTRIES; i += 1) {
        const lpr_readlink_cache_entry_t *entry = &lpr_readlink_cache[i];
        if (entry->active &&
            entry->length == length &&
            lpr_memcmp(entry->path, path, (size_t)length) == 0)
        {
            *out_status = entry->status;
            return 1;
        }
    }
    return 0;
}

void lpr_readlink_cache_store(const char *path, uint64_t length, int64_t status)
{
    if (path == 0 || length == 0 || length >= FILED_PATH_BYTES || status >= 0) {
        return;
    }
    const uint64_t slot = lpr_readlink_cache_clock++ % LPR_READLINK_CACHE_ENTRIES;
    lpr_readlink_cache_entry_t *entry = &lpr_readlink_cache[slot];
    lpr_memset(entry, 0, sizeof(*entry));
    entry->active = 1;
    entry->length = (uint16_t)length;
    entry->status = status;
    lpr_memcpy(entry->path, path, (size_t)length);
    entry->path[length] = '\0';
}

void lpr_page_cache_clear(void)
{
    lpr_memset(lpr_page_cache, 0, sizeof(lpr_page_cache));
    lpr_page_cache_clock = 0;
}

void lpr_page_cache_invalidate_handle(uint64_t handle)
{
    if (handle == 0) {
        return;
    }
    for (uint64_t i = 0; i < LPR_FILED_PAGE_CACHE_ENTRIES; i += 1) {
        if (lpr_page_cache[i].active && lpr_page_cache[i].handle == handle) {
            lpr_memset(&lpr_page_cache[i], 0, offsetof(lpr_filed_page_cache_entry_t, data));
        }
    }
}

