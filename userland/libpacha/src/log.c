/* SPDX-License-Identifier: MIT */
#include "pacha/log.h"
#include "pacha/ipc.h"
#include "pacha/syscall.h"

static uint32_t next_record;

int pacha_log_emit(pacha_fd_t endpoint_fd, unsigned source, const char *text, size_t length)
{
    if (endpoint_fd < 16 || source == 0 || source > UINT8_MAX ||
        (length != 0 && text == NULL)) return -1;
    while (length != 0) {
        const size_t record_bytes = length < PACHA_LOG_MAX_RECORD_BYTES ?
            length : PACHA_LOG_MAX_RECORD_BYTES;
        const unsigned count = (unsigned)((record_bytes +
            PACHA_LOG_FRAGMENT_BYTES - 1) / PACHA_LOG_FRAGMENT_BYTES);
        const uint32_t record = __atomic_add_fetch(&next_record, 1, __ATOMIC_RELAXED);
        for (unsigned index = 0; index < count; ++index) {
            const size_t offset = index * PACHA_LOG_FRAGMENT_BYTES;
            const unsigned bytes = (unsigned)(record_bytes - offset <
                PACHA_LOG_FRAGMENT_BYTES ? record_bytes - offset :
                PACHA_LOG_FRAGMENT_BYTES);
            struct pacha_ipc_msg message = {0};
            message.word0 = PACHA_LOG_WIRE_MAGIC;
            message.word1 = ((uint64_t)source << 56) |
                ((uint64_t)record << 24) | ((uint64_t)count << 16) |
                ((uint64_t)index << 8) | bytes;
            unsigned char *payload = (unsigned char *)&message.word2;
            for (unsigned i = 0; i < bytes; ++i) payload[i] = (unsigned char)text[offset + i];
            if (pacha_ipc_message_syscall(PACHA_IPC_SYSCALL_SEND, (uint64_t)endpoint_fd, (struct pacha_ipc_msg *)(uintptr_t)((uint64_t)(uintptr_t)&message), 0, 0, 0, 0) != 0)
                return -1;
        }
        text += record_bytes;
        length -= record_bytes;
    }
    return 0;
}
