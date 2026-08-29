#ifndef LPR_PROCESS_CAPABILITY_H
#define LPR_PROCESS_CAPABILITY_H

#include <stdint.h>

enum {
    LPR_LINUX_CAPABILITY_VERSION_1 = 0x19980330u,
    LPR_LINUX_CAPABILITY_VERSION_2 = 0x20071026u,
    LPR_LINUX_CAPABILITY_VERSION_3 = 0x20080522u,
};

typedef struct lpr_linux_capability_header {
    uint32_t version;
    int32_t pid;
} lpr_linux_capability_header_t;

typedef struct lpr_linux_capability_data {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
} lpr_linux_capability_data_t;

int64_t lpr_linux_capget(uint64_t header_raw,
                         uint64_t data_raw,
                         int64_t current_pid);
int64_t lpr_linux_capset(uint64_t header_raw,
                         uint64_t data_raw,
                         int64_t current_pid);

#endif
