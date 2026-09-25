#include "context.h"
#include "../lpr_filed_internal.h"
#include <errno.h>

void lpr_unix_context_destroy(struct lpr_unix_context *context)
{
    if (!context) return;
    /* The receive close removes broker watches. SEND cache and client are
     * private too; their destructors skip copied numbers after fork. */
    lpr_unix_waiter_destroy(&context->waiter);
    lpr_unix_notifier_destroy(&context->notifier);
    lpr_unix_client_close(&context->client);
    *context = (struct lpr_unix_context){0};
}

int lpr_unix_context_next_request(struct lpr_unix_context *context, uint64_t *out)
{
    if (!context || !out) return -EINVAL;
    if (!context->client.process_token || context->client.process_token != lpr_supervisor_token)
        return -ENOTCONN;
    if (context->request == UINT64_MAX) return -EOVERFLOW;
    *out = ++context->request;
    return 0;
}

int lpr_unix_context_current(struct lpr_unix_context **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    lpr_thread_record_t *record = NULL;
    int status = lpr_thread_current_record(&record);
    if (status != 0) return status;
    struct lpr_unix_context *context = &record->unix_context;
    if (context->client.process_token == lpr_supervisor_token &&
        context->client.process_token && context->waiter.owner) {
        *out = context;
        return 0;
    }
    lpr_unix_context_destroy(context);
    status = lpr_unix_client_open(&context->client);
    if (status != 0) return status;
    uint64_t request, owner = 0;
    status = lpr_unix_context_next_request(context, &request);
    if (status == 0) status = lpr_unix_client_register_thread(&context->client, request, &owner);
    if (status != 0) {
        lpr_unix_context_destroy(context);
        return status;
    }
    lpr_unix_notifier_init(&context->notifier, &context->client);
    lpr_unix_waiter_init(&context->waiter, &context->client, owner);
    *out = context;
    return 0;
}

int lpr_unix_context_call(struct lpr_unix_context *context, struct unix_control *request,
    const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received)
{
    if (received) *received = 0;
    if (!request) return -EINVAL;
    const int status = lpr_unix_context_next_request(context, &request->request);
    if (status != 0) return status;
    return lpr_unix_client_call(&context->client, request, send, send_count, receive, capacity, received);
}
