#include "unixd/notify_client.h"

struct send_context {
    struct unix_notify_cache *cache;
    const struct unix_notify_platform *platform;
    uint64_t socket;
    const struct unix_wait_bank *bank;
    int error;
};

static void send_one(void *raw, uint64_t token)
{
    struct send_context *context = raw;
    struct unix_notify_cache *cache = context->cache;
    const struct unix_notify_platform *platform = context->platform;
    const uint32_t id = (uint32_t)token;
    unsigned entry;
    for (entry = 0; entry < UNIX_NOTIFY_CACHE_ENTRIES; entry++)
        if (cache->entries[entry].notification == id && cache->entries[entry].fd >= 16) break;
    int status = 0;
    if (entry == UNIX_NOTIFY_CACHE_ENTRIES) {
        entry = cache->next++ % UNIX_NOTIFY_CACHE_ENTRIES;
        if (cache->entries[entry].fd >= 16)
            platform->close(platform->context, cache->entries[entry].fd);
        cache->entries[entry].notification = 0;
        cache->entries[entry].fd = 0;
        cache->entries[entry].sent_token = 0;
        int fd = -1;
        status = platform->resolve(platform->context, context->socket, id, &fd);
        if (status == 0 && fd >= 16) {
            cache->entries[entry].notification = id;
            cache->entries[entry].fd = fd;
        } else if (status == 0) status = -5;
    }
    if (status == 0) {
        /* One successful send suffices for this exact arm attempt. The
         * receiver drains BEFORE arming and disarms BEFORE draining again;
         * attempts are never reused by a notification registration. This
         * is local deduplication only: never clear a peer's armed slot. */
        /* A peer may forge tokens in its own writable bank. Scope the
         * suppression to its origin so this cannot suppress a real arm in
         * our bank or on another socket sharing this notification channel. */
        if (cache->entries[entry].sent_token == token &&
            cache->entries[entry].sent_socket == context->socket &&
            cache->entries[entry].sent_bank == context->bank) return;
        status = platform->send(platform->context, cache->entries[entry].fd, token);
        if (status == 0) {
            cache->entries[entry].sent_token = token;
            cache->entries[entry].sent_socket = context->socket;
            cache->entries[entry].sent_bank = context->bank;
        }
        if (status == -11) return; /* notification already queued */
        if (status != 0) {
            platform->close(platform->context, cache->entries[entry].fd);
            cache->entries[entry].notification = 0;
            cache->entries[entry].fd = 0;
        }
    }
    /* A disappeared registration or an unauthorised peer-supplied ID is not
     * a failure of this socket's committed I/O. Do not relay forged IDs. */
    if (status == 0 || status == -2 || status == -1) return;
    status = platform->relay(platform->context, context->socket, id);
    if (status != 0 && status != -2 && status != -1 && !context->error)
        context->error = status;
}

int unix_notify_direction(struct unix_notify_cache *cache,
    const struct unix_notify_platform *platform, uint64_t socket,
    const struct unix_tx *tx, const struct unix_rx *rx)
{
    if (!cache || !platform || !platform->resolve || !platform->send ||
        !platform->relay || !platform->close || !socket || !tx || !rx) return -22;
    struct send_context context = { .cache = cache, .platform = platform, .socket = socket };
    context.bank = &tx->waiters;
    unix_notify_scan(&tx->waiters, send_one, &context);
    context.bank = &rx->waiters;
    unix_notify_scan(&rx->waiters, send_one, &context);
    return context.error;
}

void unix_notify_cache_clear(struct unix_notify_cache *cache,
    const struct unix_notify_platform *platform)
{
    if (!cache || !platform || !platform->close) return;
    for (unsigned i = 0; i < UNIX_NOTIFY_CACHE_ENTRIES; i++) {
        if (cache->entries[i].fd >= 16) platform->close(platform->context, cache->entries[i].fd);
        cache->entries[i].fd = 0;
        cache->entries[i].notification = 0;
        cache->entries[i].sent_token = 0;
    }
    cache->next = 0;
}
