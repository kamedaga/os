#pragma once
#include "socket.h"
#include "../lpr_wait.h"

int64_t lpr_unix_socket_poll(uint64_t fd, uint32_t events);
int64_t lpr_unix_socket_poll_sequence(uint64_t fd, uint32_t events,
    struct unix_poll_sequence *sequence);
int64_t lpr_unix_poll_block(lpr_wait_graph_t *graph,
    const lpr_wait_deadline_t *deadline,
    int64_t (*block_native)(lpr_wait_graph_t *, const lpr_wait_deadline_t *));
