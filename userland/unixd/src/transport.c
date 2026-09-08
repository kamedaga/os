#include "unixd/transport.h"

enum { UX_EAGAIN = 11, UX_EBUSY = 16, UX_EINVAL = 22, UX_EPIPE = 32,
    UX_EPROTO = 71, UX_EMSGSIZE = 90, UX_ESTALE = 116 };

static uint64_t acquire(const uint64_t *word)
{
    return __atomic_load_n(word, __ATOMIC_SEQ_CST);
}

static int valid_owner(uint64_t owner)
{
    return owner != 0 && owner != UNIX_TRANSPORT_RECOVERING;
}

static int lock(uint64_t *word, uint64_t owner)
{
    uint64_t empty = 0;
    return __atomic_compare_exchange_n(word, &empty, owner, 0,
        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : -UX_EBUSY;
}

static void unlock(uint64_t *word, uint64_t owner, uint64_t *changes)
{
    if (__atomic_compare_exchange_n(word, &owner, 0, 0,
        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        __atomic_fetch_add(changes, 1, __ATOMIC_SEQ_CST);
}

static int valid(const struct unix_tx *tx, const struct unix_rx *rx,
    uint64_t generation)
{
    if (tx == NULL || rx == NULL || generation == 0) return -UX_EINVAL;
    if (tx->magic != UNIX_TRANSPORT_MAGIC || rx->magic != UNIX_TRANSPORT_MAGIC ||
        tx->capacity != UNIX_TRANSPORT_BYTES ||
        (tx->type != UNIX_TRANSPORT_STREAM && tx->type != UNIX_TRANSPORT_DGRAM &&
         tx->type != UNIX_TRANSPORT_SEQPACKET)) return -UX_EPROTO;
    if (tx->generation != generation || rx->generation != generation)
        return -UX_ESTALE;
    return 0;
}

static int positions(uint64_t published, uint64_t consumed, uint32_t *used)
{
    /* Treat every peer-controlled position as untrusted. In particular do
     * not let integer wrap make a forged cursor look like unlimited space. */
    if ((published & ~(UNIX_TRANSPORT_CLOSED | UINT64_C(0xffffffff))) != 0 ||
        ((uint32_t)published & 31u) != 0 || ((uint32_t)consumed & 31u) != 0)
        return -UX_EPROTO;
    *used = (uint32_t)published - (uint32_t)consumed;
    if (*used > UNIX_TRANSPORT_BYTES ||
        ((consumed >> 32) & UINT64_C(0x7fffffff)) > UNIX_TRANSPORT_BYTES)
        return -UX_EPROTO;
    return 0;
}

static uint32_t record_size(uint32_t length)
{
    return (length + (uint32_t)sizeof(struct unix_record) + 31u) & ~31u;
}

int unix_transport_init(struct unix_tx *tx, struct unix_rx *rx,
    uint64_t generation, uint32_t type)
{
    if (tx == NULL || rx == NULL || generation == 0 ||
        (type != UNIX_TRANSPORT_STREAM && type != UNIX_TRANSPORT_DGRAM &&
         type != UNIX_TRANSPORT_SEQPACKET)) return -UX_EINVAL;
    /* Called by unixd before either VMO is shared. Native VMO pages are zeroed. */
    tx->magic = rx->magic = UNIX_TRANSPORT_MAGIC;
    tx->generation = rx->generation = generation;
    tx->type = type;
    tx->capacity = UNIX_TRANSPORT_BYTES;
    tx->published = rx->consumed = 0;
    tx->owner = rx->owner = 0;
    tx->flags = 0;
    rx->options = 0;
    tx->changes = rx->changes = 0;
    tx->progress = rx->progress = 0;
    for (unsigned i = 0; i < UNIX_WAIT_SLOTS; i++) {
        __atomic_store_n(&tx->waiters.armed[i], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&rx->waiters.armed[i], 0, __ATOMIC_RELAXED);
    }
    return 0;
}

int unix_transport_write_begin(struct unix_tx *tx, const struct unix_rx *rx,
    uint64_t generation, uint64_t owner, size_t length,
    uint64_t ticket, uint64_t operation, struct unix_write *out)
{
    if (out == NULL || !valid_owner(owner)) return -UX_EINVAL;
    *out = (struct unix_write){0};
    int status = valid(tx, rx, generation);
    if (status != 0) return status;
    if ((status = lock(&tx->owner, owner)) != 0) return status;
    const uint64_t published = acquire(&tx->published);
    const uint64_t consumed = acquire(&rx->consumed);
    uint32_t used;
    status = positions(published, consumed, &used);
    if (status != 0) goto fail;
    if (((published | consumed) & UNIX_TRANSPORT_CLOSED) != 0) {
        status = -UX_EPIPE;
        goto fail;
    }
    if (length == 0 && tx->type == UNIX_TRANSPORT_STREAM) {
        status = ticket != 0 ? -UX_EINVAL : 0;
        goto fail;
    }
    const uint32_t max_payload = UNIX_TRANSPORT_BYTES - sizeof(struct unix_record);
    if (tx->type != UNIX_TRANSPORT_STREAM && length > max_payload) {
        status = -UX_EMSGSIZE;
        goto fail;
    }
    const uint32_t available = UNIX_TRANSPORT_BYTES - used;
    if (available < sizeof(struct unix_record) ||
        (length != 0 && available == sizeof(struct unix_record))) {
        status = -UX_EAGAIN;
        goto fail;
    }
    uint32_t count = length > max_payload ? max_payload : (uint32_t)length;
    if (count > available - sizeof(struct unix_record)) {
        if (tx->type != UNIX_TRANSPORT_STREAM) {
            status = -UX_EAGAIN;
            goto fail;
        }
        count = available - sizeof(struct unix_record);
    }
    const uint32_t start = (uint32_t)published;
    struct unix_record *record = (void *)(tx->data + (start & (UNIX_TRANSPORT_BYTES - 1u)));
    *record = (struct unix_record){ .length = count, .ticket = ticket,
        .operation = operation };
    const uint32_t offset = (start + sizeof(*record)) & (UNIX_TRANSPORT_BYTES - 1u);
    const uint32_t first = count < UNIX_TRANSPORT_BYTES - offset ? count :
        UNIX_TRANSPORT_BYTES - offset;
    *out = (struct unix_write){
        .spans = {{ tx->data + offset, first }, { tx->data, count - first }},
        .owner = owner, .before = published,
        .after = (uint32_t)(start + record_size(count)), .length = count,
    };
    return 0;
fail:
    unlock(&tx->owner, owner, &tx->changes);
    return status;
}

int unix_transport_write_commit(struct unix_tx *tx, struct unix_write *write)
{
    if (tx == NULL || write == NULL) return -UX_EINVAL;
    if (write->owner == 0) return write->length == 0 ? 0 : -UX_EINVAL;
    if (acquire(&tx->owner) != write->owner) return -UX_ESTALE;
    uint64_t expected = write->before;
    const int committed = __atomic_compare_exchange_n(&tx->published,
        &expected, write->after, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    if (committed) __atomic_fetch_add(&tx->progress, 1, __ATOMIC_SEQ_CST);
    unlock(&tx->owner, write->owner, &tx->changes);
    write->owner = 0;
    return committed ? 0 : (expected & UNIX_TRANSPORT_CLOSED) ? -UX_EPIPE : -UX_ESTALE;
}

void unix_transport_write_cancel(struct unix_tx *tx, struct unix_write *write)
{
    if (tx != NULL && write != NULL && valid_owner(write->owner)) {
        unlock(&tx->owner, write->owner, &tx->changes);
        write->owner = 0;
    }
}

int unix_transport_read_begin(const struct unix_tx *tx, struct unix_rx *rx,
    uint64_t generation, uint64_t owner, size_t capacity, struct unix_read *out)
{
    if (out == NULL || !valid_owner(owner)) return -UX_EINVAL;
    *out = (struct unix_read){0};
    int status = valid(tx, rx, generation);
    if (status != 0) return status;
    if ((status = lock(&rx->owner, owner)) != 0) return status;
    const uint64_t consumed = acquire(&rx->consumed);
    const uint64_t published = acquire(&tx->published);
    uint32_t used;
    status = positions(published, consumed, &used);
    if (status != 0) goto fail;
    if ((consumed & UNIX_TRANSPORT_CLOSED) != 0 ||
        (used == 0 && (published & UNIX_TRANSPORT_CLOSED) != 0)) {
        out->eof = 1;
        status = 0;
        goto fail;
    }
    if (capacity == 0 && tx->type == UNIX_TRANSPORT_STREAM) {
        status = 0;
        goto fail;
    }
    if (used == 0) { status = -UX_EAGAIN; goto fail; }
    const uint32_t start = (uint32_t)consumed;
    const uint32_t partial = (uint32_t)(consumed >> 32);
    const struct unix_record *shared = (const void *)(tx->data + (start & (UNIX_TRANSPORT_BYTES - 1u)));
    /* Copy the descriptor once. An untrusted sender must not change a size
     * between validation and use. Payload itself is always untrusted bytes. */
    const struct unix_record record = {
        .length = __atomic_load_n(&shared->length, __ATOMIC_RELAXED),
        .reserved = __atomic_load_n(&shared->reserved, __ATOMIC_RELAXED),
        .ticket = __atomic_load_n(&shared->ticket, __ATOMIC_RELAXED),
        .operation = __atomic_load_n(&shared->operation, __ATOMIC_RELAXED),
        .reserved1 = __atomic_load_n(&shared->reserved1, __ATOMIC_RELAXED),
    };
    if (record.length > UNIX_TRANSPORT_BYTES - sizeof(record) ||
        record.reserved != 0 || record.reserved1 != 0 ||
        record_size(record.length) > used || partial > record.length ||
        (tx->type != UNIX_TRANSPORT_STREAM && partial != 0) ||
        (tx->type == UNIX_TRANSPORT_STREAM && record.length == 0)) {
        status = -UX_EPROTO;
        goto fail;
    }
    const uint32_t remaining = record.length - partial;
    const uint32_t count = capacity < remaining ? (uint32_t)capacity : remaining;
    const uint32_t offset = (start + sizeof(record) + partial) & (UNIX_TRANSPORT_BYTES - 1u);
    const uint32_t first = count < UNIX_TRANSPORT_BYTES - offset ? count :
        UNIX_TRANSPORT_BYTES - offset;
    const uint64_t after = tx->type != UNIX_TRANSPORT_STREAM || count == remaining ?
        (uint32_t)(start + record_size(record.length)) :
        (uint64_t)start | ((uint64_t)(partial + count) << 32);
    *out = (struct unix_read){
        .spans = {{ tx->data + offset, first }, { tx->data, count - first }},
        .owner = owner, .before = consumed, .after = after,
        /* Credentials apply to every part of a record. The broker releases
         * FD references at first consumption and retains only metadata for
         * the remaining bytes, so partial reads cannot duplicate rights. */
        .ticket = record.ticket,
        .operation = record.operation, .length = count,
        .message_length = record.length,
        .truncated = tx->type != UNIX_TRANSPORT_STREAM && count < remaining,
    };
    return 0;
fail:
    unlock(&rx->owner, owner, &rx->changes);
    return status;
}

int unix_transport_read_commit(struct unix_rx *rx, struct unix_read *read)
{
    if (rx == NULL || read == NULL) return -UX_EINVAL;
    if (read->owner == 0) return read->length == 0 ? 0 : -UX_EINVAL;
    if (acquire(&rx->owner) != read->owner) return -UX_ESTALE;
    uint64_t expected = read->before;
    const int committed = __atomic_compare_exchange_n(&rx->consumed,
        &expected, read->after, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    if (committed) __atomic_fetch_add(&rx->progress, 1, __ATOMIC_SEQ_CST);
    unlock(&rx->owner, read->owner, &rx->changes);
    read->owner = 0;
    return committed ? 0 : -UX_ESTALE;
}

int unix_transport_packet_begin(const struct unix_tx *tx, struct unix_rx *rx,
    uint64_t generation, uint64_t owner, uint32_t position, uint32_t length,
    uint64_t ticket, uint64_t operation, size_t capacity, struct unix_read *out)
{
    if (tx == NULL || rx == NULL || out == NULL || !valid_owner(owner) ||
        length > UNIX_TRANSPORT_BYTES - sizeof(struct unix_record) ||
        (position & 31u) != 0) return -UX_EINVAL;
    *out = (struct unix_read){0};
    if (rx->magic != UNIX_TRANSPORT_MAGIC || rx->generation != generation)
        return -UX_ESTALE;
    int status = lock(&rx->owner, owner);
    if (status != 0) return status;
    const uint64_t consumed = acquire(&rx->consumed);
    if (consumed != position) {
        unlock(&rx->owner, owner, &rx->changes);
        return -UX_ESTALE;
    }
    const uint32_t count = capacity < length ? (uint32_t)capacity : length;
    const uint32_t offset = (position + sizeof(struct unix_record)) & (UNIX_TRANSPORT_BYTES - 1u);
    const uint32_t first = count < UNIX_TRANSPORT_BYTES - offset ? count : UNIX_TRANSPORT_BYTES - offset;
    *out = (struct unix_read){
        .spans = {{ tx->data + offset, first }, { tx->data, count - first }},
        .owner = owner, .before = consumed,
        .after = (uint32_t)(position + record_size(length)), .ticket = ticket,
        .operation = operation, .length = count, .message_length = length,
        .truncated = count < length,
    };
    return 0;
}

void unix_transport_read_cancel(struct unix_rx *rx, struct unix_read *read)
{
    if (rx != NULL && read != NULL && valid_owner(read->owner)) {
        unlock(&rx->owner, read->owner, &rx->changes);
        read->owner = 0;
    }
}

int unix_transport_pending(const struct unix_tx *tx, const struct unix_rx *rx,
    uint64_t generation, uint32_t *bytes)
{
    if (!bytes) return -UX_EINVAL;
    *bytes = 0;
    int status = valid(tx, rx, generation);
    if (status != 0) return status;
    for (unsigned retry = 0; retry < 8; retry++) {
        const uint64_t consumed = acquire(&rx->consumed), published = acquire(&tx->published);
        if (consumed & UNIX_TRANSPORT_CLOSED) return 0;
        uint32_t used, cursor = (uint32_t)consumed, partial = (uint32_t)(consumed >> 32), total = 0;
        status = positions(published, consumed, &used);
        while (status == 0 && used) {
            const struct unix_record *record = (const void *)(tx->data + (cursor & (UNIX_TRANSPORT_BYTES - 1u)));
            const uint32_t length = __atomic_load_n(&record->length, __ATOMIC_RELAXED);
            if (length > UNIX_TRANSPORT_BYTES - sizeof(*record) || partial > length ||
                record_size(length) > used || (tx->type == UNIX_TRANSPORT_STREAM && !length)) {
                status = -UX_EPROTO; break;
            }
            total += length - partial;
            if (tx->type != UNIX_TRANSPORT_STREAM) break;
            uint32_t size = record_size(length);
            cursor += size; used -= size; partial = 0;
        }
        if (consumed != acquire(&rx->consumed)) continue;
        if (status == 0) *bytes = total;
        return status;
    }
    return -UX_EBUSY;
}

int unix_transport_readable(const struct unix_tx *tx, const struct unix_rx *rx,
    uint64_t generation, int *readable, int *eof)
{
    if (readable == NULL || eof == NULL) return -UX_EINVAL;
    *readable = *eof = 0;
    int status = valid(tx, rx, generation);
    if (status != 0) return status;
    uint64_t consumed, published;
    unsigned retry = 0;
    do {
        if (++retry > 8) return -UX_EBUSY;
        consumed = acquire(&rx->consumed);
        published = acquire(&tx->published);
    } while (consumed != acquire(&rx->consumed));
    uint32_t used;
    if ((status = positions(published, consumed, &used)) != 0) return status;
    *eof = (consumed & UNIX_TRANSPORT_CLOSED) != 0 ||
        (used == 0 && (published & UNIX_TRANSPORT_CLOSED) != 0);
    *readable = used != 0 || *eof;
    return 0;
}

int unix_transport_writable(const struct unix_tx *tx, const struct unix_rx *rx,
    uint64_t generation, int *writable)
{
    return unix_transport_writable_for(tx, rx, generation, 1, writable);
}

int unix_transport_writable_for(const struct unix_tx *tx, const struct unix_rx *rx,
    uint64_t generation, size_t length, int *writable)
{
    if (writable == NULL) return -UX_EINVAL;
    *writable = 0;
    int status = valid(tx, rx, generation);
    if (status != 0) return status;
    uint64_t published, consumed;
    unsigned retry = 0;
    do {
        if (++retry > 8) return -UX_EBUSY;
        published = acquire(&tx->published);
        consumed = acquire(&rx->consumed);
    } while (published != acquire(&tx->published));
    uint32_t used;
    if ((status = positions(published, consumed, &used)) != 0) return status;
    if (tx->type != UNIX_TRANSPORT_STREAM && length > UNIX_TRANSPORT_BYTES - sizeof(struct unix_record))
        return -UX_EMSGSIZE;
    const uint32_t required = tx->type == UNIX_TRANSPORT_STREAM ? 64u : record_size((uint32_t)length);
    *writable = ((published | consumed) & UNIX_TRANSPORT_CLOSED) != 0 ||
        UNIX_TRANSPORT_BYTES - used >= required;
    return 0;
}

void unix_transport_shutdown_tx(struct unix_tx *tx)
{
    __atomic_fetch_or(&tx->published, UNIX_TRANSPORT_CLOSED, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&tx->progress, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&tx->changes, 1, __ATOMIC_SEQ_CST);
}

void unix_transport_shutdown_rx(struct unix_rx *rx)
{
    __atomic_fetch_or(&rx->consumed, UNIX_TRANSPORT_CLOSED, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&rx->progress, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&rx->changes, 1, __ATOMIC_SEQ_CST);
}

static int recover(uint64_t *word, uint64_t dead_owner, uint64_t *changes)
{
    if (!valid_owner(dead_owner)) return -UX_EINVAL;
    if (!__atomic_compare_exchange_n(word, &dead_owner,
            UNIX_TRANSPORT_RECOVERING, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 0;
    /* The publication/cursor is the journal. Never roll it back, even when
     * its owner died between the commit store and releasing this lock. */
    __atomic_store_n(word, 0, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(changes, 1, __ATOMIC_SEQ_CST);
    return 1;
}

int unix_transport_recover_tx(struct unix_tx *tx, uint64_t dead_owner)
{
    if (!tx) return -UX_EINVAL;
    int recovered = recover(&tx->owner, dead_owner, &tx->changes);
    if (recovered > 0) __atomic_fetch_add(&tx->progress, 1, __ATOMIC_SEQ_CST);
    return recovered;
}

int unix_transport_recover_rx(struct unix_rx *rx, uint64_t dead_owner)
{
    if (!rx) return -UX_EINVAL;
    int recovered = recover(&rx->owner, dead_owner, &rx->changes);
    if (recovered > 0) __atomic_fetch_add(&rx->progress, 1, __ATOMIC_SEQ_CST);
    return recovered;
}
