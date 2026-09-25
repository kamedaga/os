#include "cache.h"
#include "../lpr_filed_internal.h"
#include <errno.h>

/* Four idle pages + four idle routes per process. Concurrent calls beyond
 * these bounds use ordinary live leases, not an unbounded retained cache. */
static struct lpr_unix_cache_lease idle[8];
static struct lpr_unix_cache_lease *active;
static volatile uint32_t cache_lock;
static unsigned victim[2];

void lpr_unix_cache_fork_lock(void) { lpr_state_lock(&cache_lock); }
void lpr_unix_cache_fork_unlock(void) { lpr_state_unlock(&cache_lock); }

static void destroy(struct lpr_unix_cache_lease *lease)
{
    if (lease->buffer.page) lease->io->page_destroy(lease->buffer.fd, lease->buffer.page);
    lpr_unix_route_destroy(&lease->mapping);
    *lease = (struct lpr_unix_cache_lease){0};
}

int lpr_unix_cache_begin(struct lpr_unix_cache_lease *lease,
    const struct lpr_unix_client *owner, uint64_t socket, unsigned writing,
    const struct unix_client_io *io)
{
    const unsigned kind = io ? LPR_UNIX_CACHE_PAGE : LPR_UNIX_CACHE_ROUTE;
    *lease = (struct lpr_unix_cache_lease){ .owner = owner, .socket = socket,
        .writing = writing, .io = io, .kind = kind };
    lpr_unix_cache_fork_lock();
    unsigned base = io ? 0 : 4;
    for (unsigned i = base; i < base + 4; i++) {
        if (!idle[i].kind || (!io &&
            (idle[i].socket != socket || idle[i].writing != writing))) continue;
        lease->buffer = idle[i].buffer;
        lease->mapping = idle[i].mapping;
        lease->generation = idle[i].generation;
        idle[i] = (struct lpr_unix_cache_lease){0};
        break;
    }
    if (io && !lease->buffer.page) {
        lease->buffer.fd = io->page_create(&lease->buffer.page);
        if (lease->buffer.fd < 16) {
            /* Drop only idle resources before retrying allocation. No RPC
             * has happened, and no active operation loses its mapping. */
            for (unsigned i = 0; i < 8; i++) destroy(&idle[i]);
            lease->buffer.fd = io->page_create(&lease->buffer.page);
        }
        if (lease->buffer.fd < 16) {
            int error = lease->buffer.fd < 0 ? lease->buffer.fd : -EIO;
            *lease = (struct lpr_unix_cache_lease){0};
            lpr_unix_cache_fork_unlock();
            return error;
        }
    }
    lease->next = active;
    active = lease;
    lpr_unix_cache_fork_unlock();
    return 0;
}

int lpr_unix_cache_import(struct lpr_unix_cache_lease *lease,
    const struct pacha_ipc_fd *caps, unsigned count, uint64_t generation)
{
    if (!count) return generation && lease->generation == generation &&
        lease->mapping.tx && lease->mapping.rx ? 0 : -EPROTO;
    lpr_unix_cache_fork_lock();
    lpr_unix_route_destroy(&lease->mapping);
    lease->generation = 0;
    int status = lpr_unix_route_import(caps, count, generation, lease->writing, &lease->mapping);
    if (!status) lease->generation = generation;
    lpr_unix_cache_fork_unlock();
    return status;
}

void lpr_unix_cache_end(struct lpr_unix_cache_lease *lease, int keep)
{
    lpr_unix_cache_fork_lock();
    struct lpr_unix_cache_lease **cursor = &active;
    while (*cursor && *cursor != lease) cursor = &(*cursor)->next;
    if (*cursor) *cursor = lease->next;
    if (keep && (lease->buffer.page || lease->generation)) {
        unsigned base = lease->kind == LPR_UNIX_CACHE_PAGE ? 0 : 4;
        unsigned slot = base;
        while (slot < base + 4 && idle[slot].kind) slot++;
        if (slot == base + 4) slot = base + victim[base / 4]++ % 4;
        destroy(&idle[slot]);
        idle[slot] = *lease;
        idle[slot].next = NULL;
        *lease = (struct lpr_unix_cache_lease){0};
    } else destroy(lease);
    lpr_unix_cache_fork_unlock();
}

void lpr_unix_cache_forget(const struct lpr_unix_client *owner)
{
    lpr_unix_cache_fork_lock();
    for (unsigned i = 0; i < 8; i++) if (idle[i].owner == owner) destroy(&idle[i]);
    lpr_unix_cache_fork_unlock();
}

void lpr_unix_cache_forget_socket(uint64_t socket)
{
    lpr_unix_cache_fork_lock();
    for (unsigned i = 4; i < 8; i++) if (idle[i].socket == socket) destroy(&idle[i]);
    lpr_unix_cache_fork_unlock();
}

static int child_unmap(struct lpr_unix_cache_lease *lease)
{
    const void *pages[] = { lease->buffer.page, lease->mapping.tx, lease->mapping.rx };
    const uint64_t sizes[] = { UNIX_CONTROL_BYTES, sizeof(struct unix_tx), sizeof(struct unix_rx) };
    for (unsigned i = 0; i < 3; i++)
        if (pages[i] && lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)pages[i], sizes[i]) != 0) return -EIO;
    /* All cache FDs are PRIVATE and absent. Never close copied numbers. */
    return 0;
}

int lpr_unix_cache_fork_child(void)
{
    /* Called first in bootstrap, with the fork snapshot holding cache_lock.
     * Only metadata in the child's private pages changes; never reset the
     * shared payload/control pages or another process's transport locks. */
    for (struct lpr_unix_cache_lease *lease = active; lease; lease = lease->next)
        if (child_unmap(lease)) return -EIO;
    for (unsigned i = 0; i < 8; i++) if (child_unmap(&idle[i])) return -EIO;
    active = NULL;
    for (unsigned i = 0; i < 8; i++) idle[i] = (struct lpr_unix_cache_lease){0};
    cache_lock = 0;
    return 0;
}
