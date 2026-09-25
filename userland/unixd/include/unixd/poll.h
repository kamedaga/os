#pragma once
#include <stdint.h>

/* Opaque progress stamps, not authority. source distinguishes broker state
 * from an attached connection. A stamp is sampled before readiness. */
struct unix_poll_sequence {
    uint64_t source;
    uint64_t read;
    uint64_t write;
    uint64_t error;
};

static inline uint32_t unix_poll_sequence_changed(
    const struct unix_poll_sequence *before, const struct unix_poll_sequence *after)
{
    if (before->source != after->source) return 1u | 4u | 8u;
    return (before->read != after->read ? 1u : 0u) |
        (before->write != after->write ? 4u : 0u) |
        (before->error != after->error ? 8u : 0u);
}
