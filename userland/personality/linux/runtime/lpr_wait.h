#pragma once

#include <pacha/ipc.h>
#include <stdint.h>

enum {
    LPR_WAIT_GRAPH_MAX_LEAVES = 256u,
};

// Internal-only result: unwind the active syscall before attempting native
// signal delivery.  Linux uses this errno range for the same purpose and it
// must never escape lpr_dispatch_syscall_frame().
#define LPR_WAIT_RESTART_SYSCALL (-512ll)

typedef struct lpr_wait_deadline {
    uint64_t expires_ns;
    uint8_t finite;
    uint8_t reserved[7];
} lpr_wait_deadline_t;

typedef struct lpr_wait_graph {
    struct pacha_pollfd leaves[LPR_WAIT_GRAPH_MAX_LEAVES];
    uint32_t logical_fds[LPR_WAIT_GRAPH_MAX_LEAVES];
    uint8_t drain_modes[LPR_WAIT_GRAPH_MAX_LEAVES];
    uint32_t leaf_count;
    uint32_t reserved0;
    uint64_t relative_deadline_ns;
} lpr_wait_graph_t;

void lpr_wait_graph_init(lpr_wait_graph_t *graph);
int64_t lpr_wait_graph_add_native(
    lpr_wait_graph_t *graph,
    int native_fd,
    uint64_t events);
int64_t lpr_wait_graph_add_native_min(
    lpr_wait_graph_t *graph,
    int native_fd,
    uint64_t events,
    uint64_t min_write_bytes);
int64_t lpr_wait_graph_add_control(
    lpr_wait_graph_t *graph,
    int native_fd);
int64_t lpr_wait_graph_add_fd(
    lpr_wait_graph_t *graph,
    uint64_t fd,
    uint32_t events);
int64_t lpr_wait_deadline_init(
    lpr_wait_deadline_t *deadline,
    int64_t timeout_ms);
int64_t lpr_wait_deadline_init_ns(
    lpr_wait_deadline_t *deadline,
    uint64_t timeout_ns);
int64_t lpr_wait_deadline_expired(
    const lpr_wait_deadline_t *deadline,
    int *out_expired);
int64_t lpr_wait_graph_block(
    lpr_wait_graph_t *graph,
    const lpr_wait_deadline_t *deadline);
