#pragma once
#include "mapping.h"

enum { LPR_UNIX_CACHE_PAGE = 1, LPR_UNIX_CACHE_ROUTE = 2 };
/* Active leases live on the executing thread's stack. Their registry and
 * mapping changes are serialized with native fork, never with broker RPC. */
struct lpr_unix_cache_lease {
    struct lpr_unix_cache_lease *next;
    const struct lpr_unix_client *owner;
    const struct unix_client_io *io;
    uint64_t socket, generation;
    unsigned kind, writing;
    struct unix_client_buffer buffer;
    struct lpr_unix_route_mapping mapping;
};
int lpr_unix_cache_begin(struct lpr_unix_cache_lease *lease,
    const struct lpr_unix_client *owner, uint64_t socket, unsigned writing,
    const struct unix_client_io *io);
int lpr_unix_cache_import(struct lpr_unix_cache_lease *lease,
    const struct pacha_ipc_fd *caps, unsigned count, uint64_t generation);
void lpr_unix_cache_end(struct lpr_unix_cache_lease *lease, int keep);
void lpr_unix_cache_forget(const struct lpr_unix_client *owner);
void lpr_unix_cache_forget_socket(uint64_t socket);
void lpr_unix_cache_fork_lock(void);
void lpr_unix_cache_fork_unlock(void);
int lpr_unix_cache_fork_child(void);
