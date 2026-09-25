#pragma once
#include <stdint.h>

#define UNIX_TRANSFER_BATCH 16u
#define UNIX_TRANSFER_CAPS_PER_ITEM 4u
#define UNIX_TRANSFER_SOCKET 0u

/* provider=0 names an authenticated unixd socket OFD and carries no native
 * capabilities. Other provider IDs describe userland objects backed by the
 * explicitly transferred capabilities, never authority from a numeric token
 * alone. The receiving provider validates its descriptor before import. */
struct unix_transfer_item {
    uint32_t provider;
    uint16_t capability_first;
    uint16_t capability_count;
    uint64_t object;
    uint64_t rights;
    uint32_t flags;
    /* Provider-defined metadata, preserved by escrow and validated on import.
     * Zero for unixd socket OFDs. LPR TTY: 0 console, 1 PTY master, 2 slave. */
    uint32_t provider_data;
};

_Static_assert(sizeof(struct unix_transfer_item) == 32, "unix transfer descriptor");
