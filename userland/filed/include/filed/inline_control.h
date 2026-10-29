/* SPDX-License-Identifier: MIT */
#pragma once
#include <kobox2/filesystem.h>
#include <pacha/ipc.h>

/* Fixed-size control requests use the native message words, without a
 * wire VMO. The authenticated endpoint selects the client; the kernel's
 * implicit reply selects this call, so neither identity nor a reply sequence
 * comes from the message. The sandbox filesystem protocol is unchanged.
 *
 * Request: marker | opcode | flags<<32 | prot<<34, handle, offset, length.
 * TRANSFER_DUP: marker | opcode, handle, 0, 0; one copied lease FD.
 * Reply: marker | opcode, signed result, VMO offset / lease handle, service.
 * The caller retains its source until a checked completion, just as for the
 * VMO-backed control request. The endpoint still supplies client authority.
 */
#define FILED_CONTROL_INLINE (UINT64_C(1) << 63)
#define FILED_CONTROL_MMAP_FIELDS (UINT64_C(31) << 32)

static inline int filed_control_request_decode(const struct pacha_ipc_msg *message,
    kb2_fs_request_t *request)
{
    uint32_t opcode = (uint32_t)message->word0;
    if (opcode == KB2_FILESYSTEM_OP_TRANSFER_DUP) {
        if (message->word0 != (FILED_CONTROL_INLINE | opcode) ||
            message->fd_count != 1 || message->word2 || message->word3) return -1;
        *request = (kb2_fs_request_t){.opcode=opcode,.generation=1,.sequence=1,
            .handle=message->word1};
        return 0;
    }
    if (!(message->word0 & FILED_CONTROL_INLINE) || message->fd_count ||
        (message->word0 & ~(FILED_CONTROL_INLINE | FILED_CONTROL_MMAP_FIELDS | UINT64_C(0xffffffff))) ||
        (opcode != KB2_FILESYSTEM_OP_FILE_VMO &&
         opcode != KB2_FILESYSTEM_OP_SHARED_FILE_VMO)) return -1;
    *request = (kb2_fs_request_t){.opcode = opcode, .generation = 1, .sequence = 1,
        .flags = (message->word0 >> 32) & 3, .mask = (message->word0 >> 34) & 7,
        .handle = message->word1, .offset = (int64_t)message->word2,
        .length = message->word3};
    return 0;
}

static inline void filed_control_request_encode(const kb2_fs_request_t *request,
    struct pacha_ipc_msg *message)
{
    *message = (struct pacha_ipc_msg){
        .word0 = FILED_CONTROL_INLINE | request->opcode |
            (request->flags << 32) | ((uint64_t)request->mask << 34),
        .word1 = request->handle, .word2 = (uint64_t)request->offset,
        .word3 = request->length};
}
