#pragma once
#include <stdint.h>

#define UNIX_WAIT_SLOTS 64u

/* Written only by this socket side. The peer receives a read-only mapping.
 * Low 32 bits identify a broker-issued notification capability; high 32 bits
 * identify this wait attempt. Never wrap/reuse an attempt generation. */
struct unix_wait_bank { uint64_t armed[UNIX_WAIT_SLOTS]; };
struct unix_wait_registration {
    uint64_t token;
    uint64_t changes[2];
    uint32_t slot;
    uint32_t reserved;
};

typedef void (*unix_notify_fn)(void *context, uint64_t token);

/* Caller: drain its OWN channel, check state, arm, recheck state and changed,
 * then sleep only if both still say to wait. Afterwards always disarm.
 * Changes belong to the two direction owners; the waiter only reads them.
 * A full bank is EAGAIN, never permission to sleep without registration. */
int unix_wait_arm(struct unix_wait_bank *bank, const uint64_t *first_changes,
    const uint64_t *second_changes, uint32_t notification, uint32_t attempt,
    struct unix_wait_registration *out);
int unix_wait_changed(const uint64_t *first_changes, const uint64_t *second_changes,
    const struct unix_wait_registration *registration);
void unix_wait_disarm(struct unix_wait_bank *bank,
    const struct unix_wait_registration *registration);
void unix_wait_remove(struct unix_wait_bank *bank, uint32_t notification);
/* Updater: publish state/changes with SC atomics, then scan BOTH sides' banks.
 * send must be nonblocking; full means a wake is already pending. Capability
 * lookup on cache miss uses the broker, not a peer-supplied native FD number. */
void unix_notify_scan(const struct unix_wait_bank *bank, unix_notify_fn send,
    void *context);
