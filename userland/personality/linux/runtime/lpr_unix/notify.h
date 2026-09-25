#pragma once
#include "client.h"
#include <unixd/notify_client.h>

/* One updater thread owns this cache. The borrowed client must outlive it.
 * Do not share between threads or copy it as part of an OFD transfer. */
struct lpr_unix_notifier {
    const struct lpr_unix_client *client;
    uint64_t process_token;
    uint64_t request;
    struct unix_notify_cache cache;
};

void lpr_unix_notifier_init(struct lpr_unix_notifier *notifier, const struct lpr_unix_client *client);
void lpr_unix_notifier_destroy(struct lpr_unix_notifier *notifier);
/* After commit/cancel/PEEK/shutdown, scan both sides of the changed direction.
 * An error does not roll back already committed payload. */
int lpr_unix_notifier_signal(struct lpr_unix_notifier *notifier, uint64_t socket,
    const struct unix_tx *tx, const struct unix_rx *rx);
