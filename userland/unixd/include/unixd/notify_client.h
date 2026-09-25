#pragma once
#include "unixd/transport.h"

/* Per-updater cache, not shared between threads. It may be discarded at any
 * point; cache capacity does not limit the number of registered waiters. */
#define UNIX_NOTIFY_CACHE_ENTRIES 16u
struct unix_notify_cache {
    struct {
        uint32_t notification;
        int fd;
        uint64_t sent_token, sent_socket;
        const struct unix_wait_bank *sent_bank;
    } entries[UNIX_NOTIFY_CACHE_ENTRIES];
    uint32_t next;
};

struct unix_notify_platform {
    void *context;
    int (*resolve)(void *, uint64_t socket, uint32_t notification, int *fd);
    int (*send)(void *, int fd, uint64_t token); /* -EAGAIN = already pending */
    int (*relay)(void *, uint64_t socket, uint32_t notification);
    void (*close)(void *, int fd);
};

/* Call after every operation that released a direction lock, including
 * cancel/PEEK, and after shutdown. Warm cached notifications need no broker
 * RPC. Failed capability allocation uses WAIT_NOTIFY without extra FDs.
 * An error does NOT undo already committed payload; the caller must handle
 * control-path failure separately (wait also observes broker HANGUP). */
int unix_notify_direction(struct unix_notify_cache *cache,
    const struct unix_notify_platform *platform, uint64_t socket,
    const struct unix_tx *tx, const struct unix_rx *rx);
void unix_notify_cache_clear(struct unix_notify_cache *cache,
    const struct unix_notify_platform *platform);
