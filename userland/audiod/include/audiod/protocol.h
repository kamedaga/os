/* SPDX-License-Identifier: MIT */
#ifndef PACHA_AUDIOD_PROTOCOL_H
#define PACHA_AUDIOD_PROTOCOL_H
#include <stdint.h>
#include <pacha/abi.h>
/* One checked contract for creation, transfer and client validation. CALL is
 * independent of SEND; the per-open RPC channel needs both. */
#define AUDIOD_CLIENT_RIGHTS (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | \
    PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_SET_FLAGS | \
    PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_CALL | \
    PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL)
#define AUDIOD_SERVER_RIGHTS (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | \
    PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV | \
    PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL)
#define AUDIOD_MAGIC UINT64_C(0x415544494f50434d)
enum {
    AUDIO_OPEN, AUDIO_CLOSE, AUDIO_WRITE, AUDIO_IOCTL, AUDIO_POLL, AUDIO_FLAGS,
    AUDIO_DATA_BYTES = 4096,
    AUDIO_SESSION_LIMIT = 32,
    AUDIO_STAGING_BYTES = 8192,
    PH_AUDIO_ATTACH = 0x700,
    PH_AUDIO_EXECUTE = 0x701,
};
/* Public calls carry a per-open channel capability, never a numeric handle.
 * OPEN transfers its server end. WRITE/IOCTL transfer a single data VMO.
 * word0=magic, word1=operation, word2=argument, word3=correlation;
 * replies use word1=status and word2=result. Unknown operations fail closed.
 * The channel is also a coalesced poll notice and its HANGUP retires the PCM.
 * This copied staging record is private to the native owner and sandbox. */
struct ph_audio_request {
    uint32_t operation, slot;
    uint64_t handle, argument, size;
    int64_t result;
    uint8_t data[AUDIO_DATA_BYTES];
};
_Static_assert(sizeof(struct ph_audio_request) <= AUDIO_STAGING_BYTES, "PCM staging extent");
#endif
