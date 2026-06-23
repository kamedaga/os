#pragma once

#include <stdint.h>

#include "pacha/ipc.h"

enum {
    FILED_IPC_WAIT_SPINS = 262144u,
};

typedef struct filed_wire_page {
    int fd;
    void *addr;
    uint64_t size;
} filed_wire_page_t;

int filed_ipc_recv_wait(int fd, struct pacha_ipc_msg *msg);
int filed_ipc_send_wait(int fd, const struct pacha_ipc_msg *msg);

int filed_ipc_create_wire_page(uint64_t size, filed_wire_page_t *out_page);
void filed_ipc_destroy_wire_page(filed_wire_page_t *page);

int filed_ipc_create_client_endpoint(void);
