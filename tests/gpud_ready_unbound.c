/* SPDX-License-Identifier: MIT */
/* Existing owner/codec fixtures deliberately exercise an unbound generation.
 * Fail if a test accidentally claims it covered native observer startup. */
#include "../userland/gpud/ready.h"
#include <assert.h>
struct gpud_ready *gpud_ready_create(void) {
    assert(0);
    return 0;
}
int gpud_ready_destroy(struct gpud_ready *r) {
    (void)r;
    assert(0);
    return -1;
}
pacha_fd_t gpud_ready_fd(struct gpud_ready *r) {
    (void)r;
    assert(0);
    return -1;
}
int gpud_ready_add(struct gpud_ready *r, pacha_fd_t f, uint64_t e, unsigned k, void *c,
                   struct gpud_ready_source **s) {
    (void)r;
    (void)f;
    (void)e;
    (void)k;
    (void)c;
    (void)s;
    assert(0);
    return -1;
}
int gpud_ready_remove(struct gpud_ready_source **s) {
    assert(s && !*s);
    return 0;
}
int gpud_ready_rearm(struct gpud_ready_source *s) {
    (void)s;
    assert(0);
    return -1;
}
int gpud_ready_next(struct gpud_ready *r, struct gpud_ready_event *e) {
    (void)r;
    (void)e;
    assert(0);
    return -1;
}
int gpud_ready_error(struct gpud_ready *r) {
    (void)r;
    assert(0);
    return -1;
}
