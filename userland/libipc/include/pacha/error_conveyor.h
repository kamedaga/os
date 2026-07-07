#ifndef PACHA_ERROR_CONVEYOR_H
#define PACHA_ERROR_CONVEYOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PACHA_ERRCONV_VERSION = 1,
    PACHA_ERRCONV_TEXT_BYTES = 64,
    PACHA_ERRCONV_DEFAULT_MAX_FRAMES = 1024,
};

enum pacha_errconv_domain {
    PACHA_ERRCONV_DOMAIN_NONE = 0,
    PACHA_ERRCONV_DOMAIN_KERNEL_STATUS = 1,
    PACHA_ERRCONV_DOMAIN_PACHA_ERRNO = 2,
    PACHA_ERRCONV_DOMAIN_FILED_STATUS = 3,
    PACHA_ERRCONV_DOMAIN_TERMD_STATUS = 4,
    PACHA_ERRCONV_DOMAIN_LPRS_STATUS = 5,
    PACHA_ERRCONV_DOMAIN_LINUX_ERRNO = 6,
    PACHA_ERRCONV_DOMAIN_IPC_STATUS = 7,
    PACHA_ERRCONV_DOMAIN_ERRCONV = 8,
};

enum pacha_errconv_component {
    PACHA_ERRCONV_COMPONENT_NONE = 0,
    PACHA_ERRCONV_COMPONENT_FILED = 1,
    PACHA_ERRCONV_COMPONENT_TERMD = 2,
    PACHA_ERRCONV_COMPONENT_LPR_SUPERVISOR = 3,
    PACHA_ERRCONV_COMPONENT_SEED0ROOT = 4,
    PACHA_ERRCONV_COMPONENT_LPR_RUNTIME = 5,
};

enum pacha_errconv_stage {
    PACHA_ERRCONV_STAGE_NONE = 0,
    PACHA_ERRCONV_STAGE_DISPATCH_ENTRY = 1,
    PACHA_ERRCONV_STAGE_VALIDATION = 2,
    PACHA_ERRCONV_STAGE_MAP_PAGE = 3,
    PACHA_ERRCONV_STAGE_FD_TRANSFER = 4,
    PACHA_ERRCONV_STAGE_CHILD_RPC_CALL = 5,
    PACHA_ERRCONV_STAGE_CHILD_RPC_RECV = 6,
    PACHA_ERRCONV_STAGE_REPLY_MAGIC = 7,
    PACHA_ERRCONV_STAGE_CHILD_STATUS = 8,
    PACHA_ERRCONV_STAGE_STATUS_MAP = 9,
    PACHA_ERRCONV_STAGE_KERNEL_SYSCALL = 10,
    PACHA_ERRCONV_STAGE_ERROR_GET = 11,
    PACHA_ERRCONV_STAGE_STORE = 12,
    PACHA_ERRCONV_STAGE_LINUX_ERRNO = 13,
};

typedef struct pacha_errconv_frame {
    uint64_t domain;
    uint64_t component;
    uint64_t op;
    uint64_t stage;
    int64_t status;
    int64_t raw_status;
    uint64_t request_id;
    uint64_t fd_count;
    uint64_t subject;
    uint64_t child_token;
    char text[PACHA_ERRCONV_TEXT_BYTES];
} pacha_errconv_frame_t;

typedef struct pacha_errconv_page {
    uint64_t version;
    uint64_t token;
    uint64_t total_frames;
    uint64_t returned_frames;
    uint64_t lost_frames;
    int64_t root_status;
    uint64_t reserved[2];
    pacha_errconv_frame_t frames[];
} pacha_errconv_page_t;

typedef struct pacha_errconv_chunk pacha_errconv_chunk_t;
typedef struct pacha_errconv_chain pacha_errconv_chain_t;

typedef struct pacha_errconv_store {
    uint64_t component;
    uint64_t next_token;
    uint64_t max_frames_per_chain;
    pacha_errconv_chain_t *chains;
} pacha_errconv_store_t;

void pacha_errconv_store_init(pacha_errconv_store_t *store, uint64_t component);
uint64_t pacha_errconv_page_capacity(uint64_t page_bytes);
uint64_t pacha_errconv_begin(
    pacha_errconv_store_t *store,
    int64_t root_status,
    const pacha_errconv_frame_t *root_frame);
int pacha_errconv_append(
    pacha_errconv_store_t *store,
    uint64_t token,
    const pacha_errconv_frame_t *frame);
int pacha_errconv_export(
    const pacha_errconv_store_t *store,
    uint64_t token,
    void *page,
    uint64_t page_bytes);
uint64_t pacha_errconv_import_page(
    pacha_errconv_store_t *store,
    int64_t root_status,
    const pacha_errconv_frame_t *parent_frame,
    const void *child_page,
    uint64_t child_page_bytes);
uint64_t pacha_errconv_error_token(
    pacha_errconv_store_t *store,
    int64_t status,
    uint64_t domain,
    uint64_t op,
    uint64_t stage,
    int64_t raw_status,
    uint64_t request_id,
    uint64_t fd_count,
    uint64_t subject,
    uint64_t child_token,
    const char *text);
void pacha_errconv_frame_text(pacha_errconv_frame_t *frame, const char *text);
int pacha_errconv_dump_page_to_fd(int fd, const char *prefix, const void *page, uint64_t page_bytes);

const char *pacha_errconv_domain_name(uint64_t domain);
const char *pacha_errconv_component_name(uint64_t component);
const char *pacha_errconv_stage_name(uint64_t stage);

#ifdef __cplusplus
}
#endif

#endif
