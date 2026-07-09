#ifndef PERSONALITY_COORDINATOR_PROTOCOL_V2_H
#define PERSONALITY_COORDINATOR_PROTOCOL_V2_H

#include <stdint.h>
#include "pacha/service_abi.h"
#include "personality_abi.h"

#define LPR_COORDINATOR_V2_MAGIC 0x3244524f4f434c50ull
#define LPR_COORDINATOR_V2_VERSION PACHA_SERVICE_ABI_VERSION

enum lpr_coordinator_op {
    LPR_COORD_OP_NONE = 0,
    LPR_COORD_OP_REGISTER_PROCESS = 0x0101u,
    LPR_COORD_OP_UNREGISTER_PROCESS = 0x0102u,
    LPR_COORD_OP_ALLOC_TID = 0x0103u,
    LPR_COORD_OP_EXIT = 0x0104u,
    LPR_COORD_OP_WAIT = 0x0201u,
    LPR_COORD_OP_SIGNAL = 0x0202u,
    LPR_COORD_OP_FORK_FD_TABLE = 0x0301u,
    LPR_COORD_OP_SHARE_FD_TABLE = 0x0302u,
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
