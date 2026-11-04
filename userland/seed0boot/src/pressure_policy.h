/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <pacha/pressure.h>
struct pressure_policy {
 uint64_t low, high, cooldown_ns, next_request_ns;
 unsigned max_pages;
 int pressured;
};
static inline unsigned pressure_poll_ms(struct pressure_policy *p, uint64_t free) {
 if (free < p->low) p->pressured = 1;
 else if (free >= p->high) p->pressured = 0;
 return p->pressured ? 50 : 250;
}
static inline uint64_t pressure_target(struct pressure_policy *p, uint64_t free, uint64_t now) {
 if (!p->pressured || now < p->next_request_ns || free >= p->high) return 0;
 uint64_t pages = (p->high - free) / 4096 + !!((p->high - free) % 4096);
 return pages < p->max_pages ? pages : p->max_pages;
}
struct pressure_pending {
 uint64_t sequence, requested, deadline;
 int active;
};
static inline int pressure_ack_matches(const struct pressure_pending *p,
 uint64_t tag, uint64_t requested, uint64_t now) {
 return p->active && now < p->deadline && requested == p->requested &&
  tag == pacha_pressure_tag(p->sequence, PACHA_PRESSURE_ACK);
}
