/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
/* Private userland boot/control protocol. No native syscall ABI changes.
 * Fresh channels authenticate each launch; the trusted boot handoff alone
 * accepts registration. Never delegate these endpoints to filesystem clients.
 */
#define PACHA_PRESSURE_EXPORT_FD 1000
#define PACHA_PRESSURE_REGISTER UINT64_C(0x5052455353524547)
#define PACHA_PRESSURE_BIND 0x5100u
#define PACHA_PRESSURE_REQUEST 0x5101u
#define PACHA_PRESSURE_ACK 0x5102u
#define PACHA_PRESSURE_MAX_PAGES 1024u
#define PACHA_PRESSURE_RIGHTS (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | \
 PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV)
static inline uint64_t pacha_pressure_tag(uint64_t sequence, unsigned op) {
 return (sequence << 16) | op;
}
/* request: tag(sequence, REQUEST), requested pages, absolute monotonic ns
 * deadline, zero. ack: tag(sequence, ACK), requested pages, Linux-freed pages,
 * reported_ok_pages. The latter counts successful reporting callbacks and
 * MAY INCLUDE ALREADY ABSENT PAGES. It is not newly returned PMM bytes.
 * Registration: magic, launch generation, 0, 0; three FDs: pressure channel,
 * sandbox process watch, owner liveness channel. Owner retains its peer.
 */
