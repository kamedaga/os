#pragma once

#include <stdint.h>

void *lpr_process_client_payload(void *page);

int64_t lpr_process_client_call(
    uint64_t *request_counter,
    int64_t (*status_to_errno)(int64_t status),
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    int transfer_fd,
    uint64_t *out_result);

int64_t lpr_process_client_call_with_reply_fd(
    uint64_t *request_counter,
    int64_t (*status_to_errno)(int64_t status),
    uint32_t op,
    int page_fd,
    void *page,
    uint32_t payload_size,
    int transfer_fd,
    uint64_t *out_result,
    int *out_reply_fd);

int64_t lpr_process_client_call_token(
    uint64_t *request_counter,
    int64_t (*status_to_errno)(int64_t status),
    int (*create_page)(void **out_page),
    void (*destroy_page)(int fd, void *page),
    uint32_t op,
    uint64_t token,
    int transfer_fd,
    uint64_t *out_result);
