#pragma once
#include <unixd/client.h>

/* Process-local control capability, never an OFD transfer record. */
struct lpr_unix_client {
    int fd;
    uint64_t session;
    uint64_t process_token;
};

/* out must be fresh storage or a previously closed client. */
int lpr_unix_client_open(struct lpr_unix_client *out);
void lpr_unix_client_close(struct lpr_unix_client *client);
/* Register the executing native thread for transport owner-death recovery.
 * Cache the returned owner per thread/session, not across fork or exec. */
int lpr_unix_client_register_thread(const struct lpr_unix_client *client,
    uint64_t request_id, uint64_t *out_owner);
int lpr_unix_client_call(const struct lpr_unix_client *client, struct unix_control *request,
    const struct pacha_ipc_fd *send, unsigned send_count,
    struct pacha_ipc_fd *receive, unsigned capacity, unsigned *received);
