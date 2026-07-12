#ifndef INPUTD_IPC_PROTOCOL_H
#define INPUTD_IPC_PROTOCOL_H

#include <stdint.h>

#include <pacha/service_abi.h>

enum {
    INPUTD_SERVICE_ID = 0x494e5055u,
    INPUTD_OP_OPEN = 0u,
    INPUTD_OP_CLOSE = 1u,
    INPUTD_OP_DUP = 2u,
    INPUTD_OP_READ = 3u,
    INPUTD_OP_IOCTL = 4u,
    INPUTD_OP_POLL = 5u,
    INPUTD_EVENT_CAPACITY = 64u,
    INPUTD_IOCTL_DATA_BYTES = 1024u,
    INPUTD_POLLIN = 0x0001u,
};

typedef struct inputd_open_request {
    uint32_t event_index;
    uint32_t flags;
} inputd_open_request_t;

typedef struct inputd_handle_request {
    uint64_t handle;
} inputd_handle_request_t;

typedef struct inputd_input_event {
    int64_t seconds;
    int64_t microseconds;
    uint16_t type;
    uint16_t code;
    int32_t value;
} inputd_input_event_t;

typedef struct inputd_read_request {
    uint64_t handle;
    uint32_t event_capacity;
    uint32_t event_count;
    inputd_input_event_t events[INPUTD_EVENT_CAPACITY];
} inputd_read_request_t;

typedef struct inputd_ioctl_request {
    uint64_t handle;
    uint64_t request;
    uint32_t data_size;
    uint32_t result_size;
    uint8_t data[INPUTD_IOCTL_DATA_BYTES];
} inputd_ioctl_request_t;

typedef struct inputd_poll_request {
    uint64_t handle;
    uint32_t events;
    uint32_t revents;
} inputd_poll_request_t;

_Static_assert(sizeof(inputd_input_event_t) == 24, "inputd input_event size");
_Static_assert(sizeof(inputd_read_request_t) <= PACHA_SERVICE_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES,
    "inputd read payload fits service page");
_Static_assert(sizeof(inputd_ioctl_request_t) <= PACHA_SERVICE_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES,
    "inputd ioctl payload fits service page");

#endif
