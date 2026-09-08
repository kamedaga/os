#pragma once
#include "notify.h"
#include "wait.h"

/* Embedded in the native thread's launch record: no allocation or global
 * per-TID cache, and no private capability survives the fork-child reset. */
struct lpr_unix_context {
    struct lpr_unix_client client;
    struct lpr_unix_notifier notifier;
    struct lpr_unix_waiter waiter;
    uint64_t request;
};

/* Only the executing thread may access its context. */
int lpr_unix_context_current(struct lpr_unix_context **out);
int lpr_unix_context_next_request(struct lpr_unix_context *context, uint64_t *out);
int lpr_unix_context_call(struct lpr_unix_context *context, struct unix_control *request,
    const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received);
void lpr_unix_context_destroy(struct lpr_unix_context *context);
