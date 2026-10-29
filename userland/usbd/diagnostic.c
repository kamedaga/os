/* SPDX-License-Identifier: MIT */
#include "diagnostic.h"
#include <filed_client/client.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

static int call(const struct usbd_boot_config *config, kb2_fs_request_t *request,
    const char *path, const char *path2, const void *data, size_t length,
    void *output, size_t capacity, kb2_fs_response_t *response) {
    int result = filedfs_call(config->filed_endpoint_fd, request, path, path2,
        data, length, output, capacity, response, NULL, 0, NULL, NULL);
    return result ? result : response->result < 0 ? (int)response->result : 0;
}

static void path_for(const struct usbd_boot_config *config, const char *suffix,
    char *path, size_t size) {
    snprintf(path, size, "/run/pacha/usbd-%04x-%02x-%02x.%u.%s",
        config->pci_segment, config->pci_bus, config->pci_device,
        config->pci_function, suffix);
}

int usbd_diagnostic_open(struct usbd_diagnostic *log, uint64_t generation) {
    memset(log, 0, sizeof(*log));
    log->endpoint = -1;
    log->used = (size_t)snprintf(log->text, sizeof(log->text),
        "generation=%llu\n", (unsigned long long)generation);
    log->dirty = 1;
    log->endpoint = pacha_ipc_endpoint_create(
        PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER, 0);
    return log->endpoint >= 16 ? 0 : -ENOMEM;
}

static void accept(struct usbd_diagnostic *log, const struct pacha_ipc_msg *message) {
    if (message->word0 != PACHA_LOG_WIRE_MAGIC || message->fd_count) return;
    const uint64_t meta = message->word1;
    const unsigned source = meta >> 56, count = (meta >> 16) & 255;
    const unsigned index = (meta >> 8) & 255, bytes = meta & 255;
    const uint32_t record = (uint32_t)(meta >> 24);
    if (source != PACHA_LOG_SOURCE_KOBOX2_USB || !record || !count ||
        count > PACHA_LOG_MAX_FRAGMENTS || index >= count || !bytes ||
        bytes > PACHA_LOG_FRAGMENT_BYTES ||
        (index + 1 < count && bytes != PACHA_LOG_FRAGMENT_BYTES)) return;
    struct usbd_log_pending *pending = NULL;
    for (unsigned i = 0; i < 16; ++i)
        if (log->pending[i].record == record) { pending = &log->pending[i]; break; }
    if (!pending) {
        pending = &log->pending[log->next_slot++ % 16];
        memset(pending, 0, sizeof(*pending));
        pending->record = record;
        pending->count = count;
    }
    if (pending->count != count) { pending->record = 0; return; }
    memcpy(pending->text + index * PACHA_LOG_FRAGMENT_BYTES, &message->word2, bytes);
    pending->received |= (uint16_t)(1u << index);
    if (index + 1 == count) pending->last_bytes = bytes;
    if (pending->received != (uint16_t)((1u << count) - 1u)) return;
    size_t length = (count - 1) * PACHA_LOG_FRAGMENT_BYTES + pending->last_bytes;
    if (log->used + length + 1 > sizeof(log->text)) {
        size_t discard = log->used / 2;
        while (discard < log->used && log->text[discard - 1] != '\n') ++discard;
        memmove(log->text, log->text + discard, log->used - discard);
        log->used -= discard;
    }
    memcpy(log->text + log->used, pending->text, length);
    log->used += length;
    log->text[log->used++] = '\n';
    log->dirty = 1;
    pending->record = 0;
}

void usbd_diagnostic_drain(struct usbd_diagnostic *log) {
    if (log->endpoint < 16) return;
    /* A noisy or broken sandbox cannot starve lifecycle/input handling. No
     * ancillary capacity is offered: unsolicited capabilities stay rejected. */
    for (unsigned i = 0; i < 256; ++i) {
        struct pacha_ipc_msg message = {0};
        if (pacha_ipc_recv(log->endpoint, &message)) break;
        accept(log, &message);
    }
}

int usbd_diagnostic_publish(struct usbd_diagnostic *log,
    const struct usbd_boot_config *config) {
    if (!log->dirty) return 0;
    char path[96], temporary[100];
    path_for(config, "log", path, sizeof(path));
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_MKDIRAT, .mode = 0755};
    kb2_fs_response_t response = {0};
    int result = call(config, &request, "/run/pacha", NULL, NULL, 0, NULL, 0, &response);
    if (result && result != -EEXIST) return result;
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_OPENAT2,
        .flags = O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, .mode = 0600};
    result = call(config, &request, temporary, NULL, NULL, 0, NULL, 0, &response);
    if (result) return result;
    const uint64_t handle = response.handle;
    for (size_t offset = 0; !result && offset < log->used;) {
        size_t amount = log->used - offset;
        if (amount > 4096) amount = 4096;
        request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_PWRITE,
            .handle = handle, .offset = (int64_t)offset, .length = amount};
        result = call(config, &request, NULL, NULL, log->text + offset, amount,
            NULL, 0, &response);
        if (!result && (response.result <= 0 || (uint64_t)response.result > amount)) result = -EIO;
        if (!result) offset += (size_t)response.result;
    }
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_CLOSE, .handle = handle};
    int closed = call(config, &request, NULL, NULL, NULL, 0, NULL, 0, &response);
    if (!result) result = closed;
    if (!result) {
        request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_RENAMEAT2};
        result = call(config, &request, temporary, path, NULL, 0, NULL, 0, &response);
    }
    if (!result) log->dirty = 0;
    else {
        request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_UNLINKAT};
        (void)call(config, &request, temporary, NULL, NULL, 0, NULL, 0, &response);
    }
    return result;
}

int usbd_diagnostic_close(struct usbd_diagnostic *log) {
    if (log->endpoint < 16) return 0;
    int result = pacha_fd_close(log->endpoint);
    if (!result) log->endpoint = -1;
    return result;
}

int usbd_reload_requested(const struct usbd_boot_config *config, uint64_t generation) {
    char path[96], expected[64], data[64];
    path_for(config, "control", path, sizeof(path));
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_OPENAT2,
        .flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW};
    kb2_fs_response_t response = {0};
    int result = call(config, &request, path, NULL, NULL, 0, NULL, 0, &response);
    if (result == -ENOENT) return 0;
    if (result) return result;
    const uint64_t handle = response.handle;
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_PREAD,
        .handle = handle, .length = sizeof(data)};
    result = call(config, &request, NULL, NULL, NULL, 0, data, sizeof(data), &response);
    int length = result ? -1 : (int)response.result;
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_CLOSE, .handle = handle};
    int closed = call(config, &request, NULL, NULL, NULL, 0, NULL, 0, &response);
    if (result || closed) return result ? result : closed;
    const int wanted = snprintf(expected, sizeof(expected), "reload %llu\n",
        (unsigned long long)generation);
    /* Incomplete writes are ignored until the next tick. Stale generations
     * cannot trigger a different sandbox, even when FD numbers are reused. */
    if (length != wanted || memcmp(data, expected, (size_t)wanted)) return 0;
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_UNLINKAT};
    result = call(config, &request, path, NULL, NULL, 0, NULL, 0, &response);
    return result ? result : 1;
}
