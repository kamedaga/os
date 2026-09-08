#pragma once

#include <stddef.h>
#include <stdint.h>
#include "unixd/notify.h"

/* This is a userland wire layout, shared by unixd and LPR. The two regions
 * are separate VMOs: a peer never receives MAP_WRITE to our cursor/data. */
#define UNIX_TRANSPORT_MAGIC UINT64_C(0x3152474e49525855)
#define UNIX_TRANSPORT_BYTES 65536u
#define UNIX_TRANSPORT_PAGE 4096u
#define UNIX_TRANSPORT_CLOSED (UINT64_C(1) << 63)
#define UNIX_TRANSPORT_RECOVERING UINT64_MAX

enum {
    UNIX_TRANSPORT_STREAM = 1,
    UNIX_TRANSPORT_DGRAM = 2,
    UNIX_TRANSPORT_SEQPACKET = 5,
};

struct unix_record {
    uint32_t length;
    uint32_t reserved;
    uint64_t ticket;
    uint64_t operation;
    uint64_t reserved1;
};

struct unix_tx {
    uint64_t magic;
    uint64_t generation;
    uint32_t type;
    uint32_t capacity;
    /* low 32 bits: published end (wrapping byte position); bit 63: EOF. */
    uint64_t published;
    uint64_t owner;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t changes;
    uint64_t progress; /* successful publication, shutdown, or dead-owner recovery */
    struct unix_wait_bank waiters;
    uint8_t reserved[UNIX_TRANSPORT_PAGE - 64 - sizeof(struct unix_wait_bank)];
    uint8_t data[UNIX_TRANSPORT_BYTES];
};

struct unix_rx {
    uint64_t magic;
    uint64_t generation;
    uint64_t options; /* receiver-owned UNIX_SOCKET_* flags, peer maps RO */
    /* low 32 bits: record start; bits 32..62: consumed payload in record. */
    uint64_t consumed;
    uint64_t owner;
    uint64_t changes;
    uint64_t progress; /* successful consumption, shutdown, or dead-owner recovery */
    struct unix_wait_bank waiters;
    uint8_t reserved[UNIX_TRANSPORT_PAGE - 56 - sizeof(struct unix_wait_bank)];
};

/* One writer per endpoint: our outgoing payload and our consumption of the
 * peer's payload. The peer maps this entire VMO read-only. */
struct unix_endpoint {
    struct unix_tx tx;
    struct unix_rx rx;
};

struct unix_span {
    void *base;
    size_t length;
};

struct unix_const_span {
    const void *base;
    size_t length;
};

struct unix_write {
    struct unix_span spans[2];
    uint64_t owner;
    uint64_t before;
    uint64_t after;
    uint32_t length;
    uint32_t reserved;
};

struct unix_read {
    struct unix_const_span spans[2];
    uint64_t owner;
    uint64_t before;
    uint64_t after;
    uint64_t ticket;
    uint64_t operation;
    uint32_t length;
    uint32_t message_length;
    uint32_t eof;
    uint32_t truncated;
};

/* None of these functions allocate, wait, copy payload, or perform IPC.
 * begin succeeds with the corresponding side locked. The caller copies the
 * spans, then commits/cancels. A successful zero-length stream write has no
 * lock (write.owner == 0); a zero-length packet DOES occupy a record.
 * -EBUSY means another operation owns this side; wait on its notification,
 * not a process-local futex. Recovery is only legal after that owner died. */
int unix_transport_init(struct unix_tx *tx, struct unix_rx *rx,
    uint64_t generation, uint32_t type);
int unix_transport_write_begin(struct unix_tx *tx, const struct unix_rx *rx,
    uint64_t generation, uint64_t owner, size_t length,
    uint64_t ticket, uint64_t operation, struct unix_write *out);
int unix_transport_write_commit(struct unix_tx *tx, struct unix_write *write);
void unix_transport_write_cancel(struct unix_tx *tx, struct unix_write *write);
int unix_transport_read_begin(const struct unix_tx *tx, struct unix_rx *rx,
    uint64_t generation, uint64_t owner, size_t capacity, struct unix_read *out);
/* Datagram metadata comes from unixd's delivery queue, not the writable
 * sender header. A corrupt sender must not block other senders' datagrams. */
int unix_transport_packet_begin(const struct unix_tx *tx, struct unix_rx *rx,
    uint64_t generation, uint64_t owner, uint32_t position, uint32_t length,
    uint64_t ticket, uint64_t operation, size_t capacity, struct unix_read *out);
int unix_transport_read_commit(struct unix_rx *rx, struct unix_read *read);
void unix_transport_read_cancel(struct unix_rx *rx, struct unix_read *read);
int unix_transport_readable(const struct unix_tx *tx, const struct unix_rx *rx,
    uint64_t generation, int *readable, int *eof);
int unix_transport_pending(const struct unix_tx *tx, const struct unix_rx *rx,
    uint64_t generation, uint32_t *bytes);
int unix_transport_writable(const struct unix_tx *tx, const struct unix_rx *rx,
    uint64_t generation, int *writable);
int unix_transport_writable_for(const struct unix_tx *tx, const struct unix_rx *rx,
    uint64_t generation, size_t length, int *writable);
void unix_transport_shutdown_tx(struct unix_tx *tx);
void unix_transport_shutdown_rx(struct unix_rx *rx);
int unix_transport_recover_tx(struct unix_tx *tx, uint64_t dead_owner);
int unix_transport_recover_rx(struct unix_rx *rx, uint64_t dead_owner);

_Static_assert(sizeof(struct unix_record) == 32, "unix record wire size");
_Static_assert(offsetof(struct unix_tx, data) == UNIX_TRANSPORT_PAGE,
    "unix data must start on its own page");
_Static_assert(sizeof(struct unix_rx) == UNIX_TRANSPORT_PAGE, "unix rx page");
_Static_assert(sizeof(struct unix_tx) == UNIX_TRANSPORT_PAGE + UNIX_TRANSPORT_BYTES,
    "unix tx VMO size");
