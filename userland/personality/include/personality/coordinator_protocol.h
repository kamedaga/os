#ifndef PERSONALITY_COORDINATOR_PROTOCOL_H
#define PERSONALITY_COORDINATOR_PROTOCOL_H

#include <stdint.h>
#include "personality_abi.h"

#define LPR_COORDINATOR_MAGIC 0x44524f4f434c5052ull
#define LPR_COORDINATOR_VERSION 1u

enum lpr_coordinator_op {
    LPR_COORD_OP_NONE = 0,
    LPR_COORD_OP_REGISTER_PROCESS = 1,
    LPR_COORD_OP_UNREGISTER_PROCESS = 2,
    LPR_COORD_OP_ALLOC_TID = 3,
    LPR_COORD_OP_EXIT = 4,
    LPR_COORD_OP_WAIT = 5,
    LPR_COORD_OP_SIGNAL = 6,
    LPR_COORD_OP_FORK_FD_TABLE = 7,
    LPR_COORD_OP_SHARE_FD_TABLE = 8,
};

enum lpr_coordinator_status {
    LPR_COORD_STATUS_OK = 0,
    LPR_COORD_STATUS_INVALID = 1,
    LPR_COORD_STATUS_NOT_FOUND = 2,
    LPR_COORD_STATUS_AGAIN = 3,
    LPR_COORD_STATUS_NO_MEMORY = 4,
    LPR_COORD_STATUS_UNSUPPORTED = 5,
};

struct lpr_coordinator_request {
    uint64_t magic;
    uint32_t version;
    uint32_t op;
    uint64_t pid;
    uint64_t tid;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
};

struct lpr_coordinator_response {
    uint64_t magic;
    uint32_t version;
    uint32_t status;
    uint64_t value0;
    uint64_t value1;
    uint64_t value2;
    uint64_t value3;
};

_Static_assert(sizeof(struct lpr_coordinator_request) == 64, "lpr_coordinator_request size");
_Static_assert(sizeof(struct lpr_coordinator_response) == 48, "lpr_coordinator_response size");

#endif
