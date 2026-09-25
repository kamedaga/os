#pragma once
#include "context.h"

struct lpr_unix_ancillary {
    uint64_t control, capacity, used;
    uint64_t ticket, operation, route;
    struct unix_address address;
    struct unix_credentials credentials;
    unsigned count;
    unsigned automatic_credentials, passcred;
    int fds[UNIX_RIGHTS_MAX];
};

int lpr_unix_rights_parse(struct lpr_unix_ancillary *state);
void lpr_unix_credentials_output(struct lpr_unix_ancillary *,
    const struct unix_credentials *, uint32_t *flags);
int lpr_unix_rights_prepare(struct lpr_unix_context *context, uint64_t socket,
    struct lpr_unix_ancillary *state);
int lpr_unix_rights_commit(struct lpr_unix_context *context, uint64_t socket,
    const struct lpr_unix_ancillary *state, const struct unix_write *write);
void lpr_unix_rights_cancel(struct lpr_unix_context *context,
    const struct lpr_unix_ancillary *state);
int lpr_unix_rights_receive(struct lpr_unix_context *context, uint64_t socket,
    const struct unix_read *read, struct lpr_unix_ancillary *state,
    uint64_t flags, uint32_t *message_flags);
