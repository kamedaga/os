#ifndef PERSONALITY_LINUX_LPR_H
#define PERSONALITY_LINUX_LPR_H

#include <stdint.h>
#include "personality_abi.h"
#include "runtime_page.h"
#include "zpoline.h"

#define LPR_LINUX_SYS_WRITE 1ull
#define LPR_LINUX_SYS_GETPID 39ull
#define LPR_LINUX_SYS_EXIT 60ull
#define LPR_LINUX_SYS_GETTID 186ull
#define LPR_LINUX_SYS_EXIT_GROUP 231ull

#define LPR_LINUX_ENOSYS 38

int64_t lpr_start(struct lpr_runtime_page *runtime);
int64_t lpr_patch_mapping(const struct lpr_patch_mapping_request *request,
                          struct lpr_patch_mapping_result *result);
int64_t lpr_init_zpoline_page(uint8_t *page);
int64_t lpr_build_zpoline_page(uint8_t *page, uint64_t handler_va);
uint64_t lpr_zpoline_common_offset(void);

#endif
