#define _GNU_SOURCE
#include "../userland/personality/linux/runtime/lpr_unix/cache.c"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned maps, creates, unmaps, closes;
static int next_fd = 16, fail_unmap;
void lpr_state_lock(volatile uint32_t *word) { assert(!*word); *word = 1; }
void lpr_state_unlock(volatile uint32_t *word) { assert(*word == 1); *word = 0; }
static void *map_page(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(p != MAP_FAILED); maps++; creates++; return p;
}
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t p, uint64_t bytes)
{
    assert(nr == PACHAOS_SYSCALL_MUNMAP);
    if (fail_unmap) return 1;
    assert(munmap((void *)(uintptr_t)p, bytes) == 0 && maps);
    maps--; unmaps++; return 0;
}
static int page_create(struct unix_control **page)
{
    *page = map_page(UNIX_CONTROL_BYTES); return next_fd++;
}
static void page_destroy(int fd, struct unix_control *page)
{
    assert(fd >= 16); closes++;
    assert(!lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uintptr_t)page, UNIX_CONTROL_BYTES));
}
void lpr_unix_route_destroy(struct lpr_unix_route_mapping *mapping)
{
    if (mapping->tx) assert(!lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uintptr_t)mapping->tx, sizeof(struct unix_tx)));
    if (mapping->rx) assert(!lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, (uintptr_t)mapping->rx, sizeof(struct unix_rx)));
    for (unsigned i = 0; i < 2; i++) if (mapping->fds[i] >= 16) closes++;
    *mapping = (struct lpr_unix_route_mapping){0};
}
int lpr_unix_route_import(const struct pacha_ipc_fd *caps, unsigned count,
    uint64_t generation, int writing, struct lpr_unix_route_mapping *mapping)
{
    (void)writing;
    assert(count == 2 && generation);
    *mapping = (struct lpr_unix_route_mapping){ .tx = map_page(sizeof(struct unix_tx)),
        .rx = map_page(sizeof(struct unix_rx)), .fds = {(int)caps[0].fd, (int)caps[1].fd} };
    return 0;
}
static const struct unix_client_io io = { .page_create = page_create, .page_destroy = page_destroy };
static const struct lpr_unix_client owner = { .fd = 100 };
static const struct pacha_ipc_fd caps[2] = {{ .fd = 40 }, { .fd = 41 }};

int main(void)
{
    struct lpr_unix_cache_lease lease, blocked;
    for (unsigned i = 0; i < 100; i++) {
        assert(!lpr_unix_cache_begin(&lease, &owner, 0, 0, &io));
        lpr_unix_cache_end(&lease, 1);
    }
    assert(creates == 1 && maps == 1); /* 100 RPCs retain one control page. */
    for (unsigned i = 0; i < 100; i++) {
        assert(!lpr_unix_cache_begin(&lease, &owner, 7, 1, NULL));
        assert(!lpr_unix_cache_import(&lease, caps, i ? 0 : 2, 13));
        lpr_unix_cache_end(&lease, 1);
    }
    assert(creates == 3 && maps == 3); /* Same route: no map/unmap churn. */
    assert(!lpr_unix_cache_begin(&lease, &owner, 7, 1, NULL));
    assert(lpr_unix_cache_import(&lease, NULL, 0, 14) == -EPROTO);
    assert(!lpr_unix_cache_import(&lease, caps, 2, 14));
    lpr_unix_cache_end(&lease, 1);
    for (unsigned i = 10; i < 40; i++) {
        assert(!lpr_unix_cache_begin(&lease, &owner, i, 0, NULL));
        assert(!lpr_unix_cache_import(&lease, caps, 2, i));
        lpr_unix_cache_end(&lease, 1);
        assert(maps <= 9); /* Four retained routes, one retained page. */
    }
    assert(!lpr_unix_cache_begin(&blocked, &owner, 39, 0, NULL));
    assert(blocked.generation == 39);
    assert(!lpr_unix_cache_begin(&lease, &owner, 0, 0, &io));
    unsigned before = maps, old_closes = closes;
    lpr_unix_cache_fork_lock();
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        assert(!lpr_unix_cache_fork_child());
        assert(!maps && !active && !cache_lock && closes == old_closes);
        /* Copied PRIVATE FD numbers must never be closed, even for active
         * sibling leases. New child use starts with a fresh control page. */
        assert(!lpr_unix_cache_begin(&lease, &owner, 0, 0, &io));
        lpr_unix_cache_end(&lease, 0);
        _exit(0);
    }
    lpr_unix_cache_fork_unlock();
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(maps == before && closes == old_closes && blocked.mapping.tx);
    lpr_unix_cache_end(&lease, 0); /* Failed RPC must discard its page. */
    lpr_unix_cache_end(&blocked, 1);
    lpr_unix_cache_forget_socket(39);
    lpr_unix_cache_forget(&owner);
    assert(!maps && !active);
    assert(!lpr_unix_cache_begin(&lease, &owner, 0, 0, &io));
    fail_unmap = 1;
    assert(lpr_unix_cache_fork_child() == -EIO && maps == 1 && active);
    fail_unmap = 0;
    lpr_unix_cache_end(&lease, 0);
    assert(!maps);
    puts("unix cache: warm page/route reuse, bounded eviction, generation mismatch, active+idle fork cleanup, no private-FD close, parent intact, failed-RPC discard passed");
}
