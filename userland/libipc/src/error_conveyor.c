#include "pacha/error_conveyor.h"
#include "pacha/ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PACHA_ERRCONV_CHUNK_FRAMES = 16,
};

struct pacha_errconv_chunk {
    pacha_errconv_chunk_t *next;
    uint64_t count;
    pacha_errconv_frame_t frames[PACHA_ERRCONV_CHUNK_FRAMES];
};

struct pacha_errconv_chain {
    pacha_errconv_chain_t *next;
    uint64_t token;
    uint64_t total_frames;
    uint64_t lost_frames;
    uint8_t loss_frame_added;
    int64_t root_status;
    pacha_errconv_chunk_t *first;
    pacha_errconv_chunk_t *last;
};

static pacha_errconv_chain_t *pacha_errconv_find_chain(
    const pacha_errconv_store_t *store,
    uint64_t token)
{
    if (store == NULL || token == 0) {
        return NULL;
    }
    for (pacha_errconv_chain_t *chain = store->chains; chain != NULL; chain = chain->next) {
        if (chain->token == token) {
            return chain;
        }
    }
    return NULL;
}

static uint64_t pacha_errconv_next_token(pacha_errconv_store_t *store)
{
    uint64_t token = store->next_token++;
    if (token == 0) {
        token = store->next_token++;
    }
    return token;
}

static void pacha_errconv_copy_text(char dst[PACHA_ERRCONV_TEXT_BYTES], const char *src)
{
    if (dst == NULL) {
        return;
    }
    memset(dst, 0, PACHA_ERRCONV_TEXT_BYTES);
    if (src == NULL) {
        return;
    }
    for (size_t i = 0; i + 1u < PACHA_ERRCONV_TEXT_BYTES && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
}

void pacha_errconv_store_init(pacha_errconv_store_t *store, uint64_t component)
{
    if (store == NULL) {
        return;
    }
    memset(store, 0, sizeof(*store));
    store->component = component;
    store->next_token = (component << 48) | 1u;
    if (store->next_token == 0) {
        store->next_token = 1;
    }
    store->max_frames_per_chain = PACHA_ERRCONV_DEFAULT_MAX_FRAMES;
}

uint64_t pacha_errconv_page_capacity(uint64_t page_bytes)
{
    if (page_bytes <= sizeof(pacha_errconv_page_t)) {
        return 0;
    }
    return (page_bytes - sizeof(pacha_errconv_page_t)) / sizeof(pacha_errconv_frame_t);
}

void pacha_errconv_frame_text(pacha_errconv_frame_t *frame, const char *text)
{
    if (frame == NULL) {
        return;
    }
    pacha_errconv_copy_text(frame->text, text);
}

static int pacha_errconv_append_loss_frame(pacha_errconv_chain_t *chain, const char *text)
{
    if (chain == NULL || chain->loss_frame_added) {
        return -1;
    }
    chain->loss_frame_added = 1;
    pacha_errconv_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.domain = PACHA_ERRCONV_DOMAIN_ERRCONV;
    frame.component = PACHA_ERRCONV_COMPONENT_NONE;
    frame.stage = PACHA_ERRCONV_STAGE_STORE;
    frame.status = -12;
    frame.raw_status = -12;
    pacha_errconv_frame_text(&frame, text);

    pacha_errconv_chunk_t *chunk = chain->last;
    if (chunk == NULL || chunk->count >= PACHA_ERRCONV_CHUNK_FRAMES) {
        chunk = (pacha_errconv_chunk_t *)calloc(1, sizeof(*chunk));
        if (chunk == NULL) {
            return -1;
        }
        if (chain->last != NULL) {
            chain->last->next = chunk;
        } else {
            chain->first = chunk;
        }
        chain->last = chunk;
    }
    chunk->frames[chunk->count++] = frame;
    chain->total_frames++;
    return 0;
}

static int pacha_errconv_chain_append(
    pacha_errconv_store_t *store,
    pacha_errconv_chain_t *chain,
    const pacha_errconv_frame_t *frame)
{
    if (store == NULL || chain == NULL || frame == NULL) {
        return -1;
    }
    const uint64_t limit =
        store->max_frames_per_chain == 0 ? PACHA_ERRCONV_DEFAULT_MAX_FRAMES : store->max_frames_per_chain;
    if (chain->total_frames >= limit) {
        chain->lost_frames++;
        (void)pacha_errconv_append_loss_frame(chain, "error chain frame limit reached");
        return -1;
    }
    pacha_errconv_chunk_t *chunk = chain->last;
    if (chunk == NULL || chunk->count >= PACHA_ERRCONV_CHUNK_FRAMES) {
        chunk = (pacha_errconv_chunk_t *)calloc(1, sizeof(*chunk));
        if (chunk == NULL) {
            chain->lost_frames++;
            (void)pacha_errconv_append_loss_frame(chain, "error chain allocation failed");
            return -1;
        }
        if (chain->last != NULL) {
            chain->last->next = chunk;
        } else {
            chain->first = chunk;
        }
        chain->last = chunk;
    }
    chunk->frames[chunk->count++] = *frame;
    chain->total_frames++;
    return 0;
}

uint64_t pacha_errconv_begin(
    pacha_errconv_store_t *store,
    int64_t root_status,
    const pacha_errconv_frame_t *root_frame)
{
    if (store == NULL) {
        return 0;
    }
    if (store->next_token == 0) {
        pacha_errconv_store_init(store, PACHA_ERRCONV_COMPONENT_NONE);
    }
    pacha_errconv_chain_t *chain = (pacha_errconv_chain_t *)calloc(1, sizeof(*chain));
    if (chain == NULL) {
        return 0;
    }
    chain->token = pacha_errconv_next_token(store);
    chain->root_status = root_status;
    chain->next = store->chains;
    store->chains = chain;
    if (root_frame != NULL) {
        (void)pacha_errconv_chain_append(store, chain, root_frame);
    }
    return chain->token;
}

int pacha_errconv_append(
    pacha_errconv_store_t *store,
    uint64_t token,
    const pacha_errconv_frame_t *frame)
{
    pacha_errconv_chain_t *chain = pacha_errconv_find_chain(store, token);
    if (chain == NULL) {
        return -1;
    }
    return pacha_errconv_chain_append(store, chain, frame);
}

int pacha_errconv_export(
    const pacha_errconv_store_t *store,
    uint64_t token,
    void *page,
    uint64_t page_bytes)
{
    if (page == NULL || page_bytes < sizeof(pacha_errconv_page_t)) {
        return -22;
    }
    pacha_errconv_page_t *out = (pacha_errconv_page_t *)page;
    memset(out, 0, (size_t)page_bytes);
    const pacha_errconv_chain_t *chain = pacha_errconv_find_chain(store, token);
    if (chain == NULL) {
        return -3;
    }
    out->version = PACHA_ERRCONV_VERSION;
    out->token = chain->token;
    out->total_frames = chain->total_frames;
    out->lost_frames = chain->lost_frames;
    out->root_status = chain->root_status;

    const uint64_t capacity = pacha_errconv_page_capacity(page_bytes);
    uint64_t copied = 0;
    for (const pacha_errconv_chunk_t *chunk = chain->first;
         chunk != NULL && copied < capacity;
         chunk = chunk->next)
    {
        for (uint64_t i = 0; i < chunk->count && copied < capacity; ++i) {
            out->frames[copied++] = chunk->frames[i];
        }
    }
    out->returned_frames = copied;
    return 0;
}

uint64_t pacha_errconv_import_page(
    pacha_errconv_store_t *store,
    int64_t root_status,
    const pacha_errconv_frame_t *parent_frame,
    const void *child_page,
    uint64_t child_page_bytes)
{
    if (store == NULL || parent_frame == NULL) {
        return 0;
    }
    uint64_t token = pacha_errconv_begin(store, root_status, parent_frame);
    if (token == 0) {
        return 0;
    }
    if (child_page == NULL || child_page_bytes < sizeof(pacha_errconv_page_t)) {
        return token;
    }
    const pacha_errconv_page_t *page = (const pacha_errconv_page_t *)child_page;
    if (page->version != PACHA_ERRCONV_VERSION) {
        pacha_errconv_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.domain = PACHA_ERRCONV_DOMAIN_ERRCONV;
        frame.component = store->component;
        frame.stage = PACHA_ERRCONV_STAGE_ERROR_GET;
        frame.status = -22;
        frame.raw_status = (int64_t)page->version;
        frame.child_token = parent_frame->child_token;
        pacha_errconv_frame_text(&frame, "child error page version mismatch");
        (void)pacha_errconv_append(store, token, &frame);
        return token;
    }
    const uint64_t capacity = pacha_errconv_page_capacity(child_page_bytes);
    const uint64_t count =
        page->returned_frames < capacity ? page->returned_frames : capacity;
    for (uint64_t i = 0; i < count; ++i) {
        (void)pacha_errconv_append(store, token, &page->frames[i]);
    }
    if (page->total_frames > page->returned_frames || page->lost_frames != 0) {
        pacha_errconv_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.domain = PACHA_ERRCONV_DOMAIN_ERRCONV;
        frame.component = store->component;
        frame.stage = PACHA_ERRCONV_STAGE_ERROR_GET;
        frame.status = page->root_status;
        frame.raw_status = (int64_t)page->lost_frames;
        frame.child_token = page->token;
        frame.subject = page->total_frames - page->returned_frames;
        pacha_errconv_frame_text(&frame, "child error chain truncated or lost frames");
        (void)pacha_errconv_append(store, token, &frame);
    }
    return token;
}

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
    const char *text)
{
    if (status >= 0) {
        return 0;
    }
    pacha_errconv_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.domain = domain;
    frame.component = store != NULL ? store->component : PACHA_ERRCONV_COMPONENT_NONE;
    frame.op = op;
    frame.stage = stage;
    frame.status = status;
    frame.raw_status = raw_status;
    frame.request_id = request_id;
    frame.fd_count = fd_count;
    frame.subject = subject;
    frame.child_token = child_token;
    pacha_errconv_frame_text(&frame, text);
    return pacha_errconv_begin(store, status, &frame);
}

const char *pacha_errconv_domain_name(uint64_t domain)
{
    switch (domain) {
    case PACHA_ERRCONV_DOMAIN_KERNEL_STATUS: return "kernel";
    case PACHA_ERRCONV_DOMAIN_PACHA_ERRNO: return "pacha_errno";
    case PACHA_ERRCONV_DOMAIN_FILED_STATUS: return "filed";
    case PACHA_ERRCONV_DOMAIN_TERMD_STATUS: return "termd";
    case PACHA_ERRCONV_DOMAIN_LPRS_STATUS: return "lpr_supervisor";
    case PACHA_ERRCONV_DOMAIN_LINUX_ERRNO: return "linux_errno";
    case PACHA_ERRCONV_DOMAIN_IPC_STATUS: return "ipc";
    case PACHA_ERRCONV_DOMAIN_ERRCONV: return "errconv";
    default: return "none";
    }
}

const char *pacha_errconv_component_name(uint64_t component)
{
    switch (component) {
    case PACHA_ERRCONV_COMPONENT_FILED: return "filed";
    case PACHA_ERRCONV_COMPONENT_TERMD: return "termd";
    case PACHA_ERRCONV_COMPONENT_LPR_SUPERVISOR: return "lpr_supervisor";
    case PACHA_ERRCONV_COMPONENT_SEED0ROOT: return "seed0root";
    case PACHA_ERRCONV_COMPONENT_LPR_RUNTIME: return "lpr_runtime";
    default: return "none";
    }
}

const char *pacha_errconv_stage_name(uint64_t stage)
{
    switch (stage) {
    case PACHA_ERRCONV_STAGE_DISPATCH_ENTRY: return "dispatch";
    case PACHA_ERRCONV_STAGE_VALIDATION: return "validation";
    case PACHA_ERRCONV_STAGE_MAP_PAGE: return "map_page";
    case PACHA_ERRCONV_STAGE_FD_TRANSFER: return "fd_transfer";
    case PACHA_ERRCONV_STAGE_CHILD_RPC_CALL: return "child_rpc_call";
    case PACHA_ERRCONV_STAGE_CHILD_RPC_RECV: return "child_rpc_recv";
    case PACHA_ERRCONV_STAGE_REPLY_MAGIC: return "reply_magic";
    case PACHA_ERRCONV_STAGE_CHILD_STATUS: return "child_status";
    case PACHA_ERRCONV_STAGE_STATUS_MAP: return "status_map";
    case PACHA_ERRCONV_STAGE_KERNEL_SYSCALL: return "kernel_syscall";
    case PACHA_ERRCONV_STAGE_ERROR_GET: return "error_get";
    case PACHA_ERRCONV_STAGE_STORE: return "store";
    case PACHA_ERRCONV_STAGE_LINUX_ERRNO: return "linux_errno";
    default: return "none";
    }
}

static void pacha_errconv_write_line(int fd, const char *line)
{
    if (line == NULL) {
        return;
    }
    (void)pacha_fd_write(fd, line, (uint64_t)strlen(line));
}

int pacha_errconv_dump_page_to_fd(int fd, const char *prefix, const void *page, uint64_t page_bytes)
{
    if (fd < 0 || page == NULL || page_bytes < sizeof(pacha_errconv_page_t)) {
        return -22;
    }
    const pacha_errconv_page_t *err = (const pacha_errconv_page_t *)page;
    if (err->version != PACHA_ERRCONV_VERSION) {
        return -22;
    }
    const char *p = prefix != NULL ? prefix : "[errconv]";
    char line[384];
    snprintf(
        line,
        sizeof(line),
        "%s token=0x%llx root=%lld frames=%llu returned=%llu lost=%llu\n",
        p,
        (unsigned long long)err->token,
        (long long)err->root_status,
        (unsigned long long)err->total_frames,
        (unsigned long long)err->returned_frames,
        (unsigned long long)err->lost_frames);
    pacha_errconv_write_line(fd, line);

    const uint64_t capacity = pacha_errconv_page_capacity(page_bytes);
    const uint64_t count =
        err->returned_frames < capacity ? err->returned_frames : capacity;
    for (uint64_t i = 0; i < count; ++i) {
        const pacha_errconv_frame_t *frame = &err->frames[i];
        snprintf(
            line,
            sizeof(line),
            "%s #%llu comp=%s domain=%s op=%llu stage=%s status=%lld raw=%lld req=0x%llx fds=%llu subject=%llu child=0x%llx text=%s\n",
            p,
            (unsigned long long)i,
            pacha_errconv_component_name(frame->component),
            pacha_errconv_domain_name(frame->domain),
            (unsigned long long)frame->op,
            pacha_errconv_stage_name(frame->stage),
            (long long)frame->status,
            (long long)frame->raw_status,
            (unsigned long long)frame->request_id,
            (unsigned long long)frame->fd_count,
            (unsigned long long)frame->subject,
            (unsigned long long)frame->child_token,
            frame->text);
        pacha_errconv_write_line(fd, line);
    }
    return 0;
}
