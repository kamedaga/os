#include "../userland/personality/linux/runtime/lpr_process/exec.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

lpr_state_t lpr_state;
static unsigned closed[64], freed;
void *lpr_memset(void *p, int c, size_t n) { return memset(p, c, n); }
int64_t lpr_close_native_fd_if_open(uint64_t fd)
{
    assert(fd >= 16 && fd < 64);
    closed[fd]++;
    return 0;
}
int64_t lpr_backend_state_free(void *state, uint64_t size)
{
    assert(state && size == sizeof(lpr_filed_backend_t));
    freed++;
    return 0;
}
void lpr_unix_mapping_destroy(struct lpr_unix_mapping *mapping)
{
    (void)mapping;
    assert(!"unexpected Unix backend");
}

int main(void)
{
    lpr_dmabuf_backend_t prime = {.token = 19,
        .native.raw = 30, .lease_fd.raw = 31};
    assert(lpr_exec_manifest_capability_count(LPR_FD_OPS_DMABUF, &prime) == 2);
    prime.reserved0 = LPR_BACKEND_TRANSFER_LEASE;
    assert(lpr_exec_manifest_capability_count(LPR_FD_OPS_DMABUF, &prime) == 2);
    prime.token = 0; prime.reserved0 = 0; prime.lease_fd.raw = -1;
    assert(lpr_exec_manifest_capability_count(LPR_FD_OPS_DMABUF, &prime) == 1);
    prime.token = 19;
    assert(lpr_exec_manifest_capability_count(LPR_FD_OPS_DMABUF, &prime) == 2);
    lpr_fd_entry_t entries[16] = {0};
    lpr_ofd_t ofds[3] = {0};
    lpr_backend_record_t backends[3] = {0};
    lpr_filed_backend_t states[3] = {0};
    lpr_control_fd_table = (lpr_fd_table_t){
        .entries = entries, .entry_count = 16,
        .ofds = ofds, .ofd_count = 3,
        .backends = backends, .backend_count = 3,
    };
    for (unsigned i = 0; i < 3; ++i) {
        states[i].lease_fd.raw = i ? 39 + i : 37;
        backends[i] = (lpr_backend_record_t){
            .active = 1, .ops_id = LPR_FD_OPS_FILED, .generation = i + 1,
            .state = &states[i], .state_bytes = sizeof(states[i]),
        };
        ofds[i] = (lpr_ofd_t){.active = 1, .generation = i + 1,
            .backend = {.index = i, .generation = i + 1},
            .refcount = i == 1, .pin_count = 5, .closing = i != 1};
    }
    /* After snapshot: parent closes both old aliases, reuses FD 3, publishes
     * FD 11, and leaves a staged import and a sibling's orphaned pin. */
    entries[3] = entries[11] = (lpr_fd_entry_t){.active = 1, .ofd_index = 1};
    entries[12] = (lpr_fd_entry_t){.active = 2, .ofd_index = 2};
    struct {
        lpr_manifest_t header;
        lpr_manifest_entry_t entries[2];
    } snapshot = {.header = {.entry_offset = sizeof(lpr_manifest_t), .entry_count = 2},
        .entries = {{.fd = 3, .ofd_index = 0, .effective_rights = 7},
                    {.fd = 8, .ofd_index = 0, .fd_flags = LPR_FD_ENTRY_CLOEXEC,
                     .effective_rights = 3}}};
    lpr_fd_pin_t pin = {.ofd_index = 0, .ofd_generation = 1,
        .backend_index = 0, .backend_generation = 1};
    lpr_exec_transaction_t tx = {.manifest = &snapshot.header, .pins = &pin, .pin_count = 1};
    snapshot.entries[1].fd = 16;
    assert(lpr_fork_restore_fd_snapshot(&tx) == -LPR_LINUX_EIO);
    assert(!freed && entries[3].ofd_index == 1);
    snapshot.entries[1].fd = 8;
    assert(lpr_fork_restore_fd_snapshot(&tx) == 0);
    assert(entries[3].active == 1 && entries[3].ofd_index == 0 && entries[3].effective_rights == 7);
    assert(entries[8].active == 1 && entries[8].ofd_index == 0 && entries[8].fd_flags == LPR_FD_ENTRY_CLOEXEC);
    assert(!entries[11].active && !entries[12].active);
    assert(ofds[0].refcount == 2 && ofds[0].pin_count == 1 && !ofds[0].closing);
    assert(!ofds[1].active && !ofds[2].active);
    assert(backends[0].active && !backends[1].active && !backends[2].active);
    assert(freed == 2 && closed[40] == 1 && closed[41] == 1 && !closed[37]);
    puts("Fork FD snapshot: reused slots, restored aliases, orphan pins and child-only close PASS");
}
