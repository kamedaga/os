/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "logd/boot_config.h"
#include "pacha/log.h"
#include "pacha/bootstrap.h"
#include "pacha/ipc.h"
#include "pacha/syscall.h"
#include "filed_client/client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define LOGD_PATH "/run/pacha/service.log"
#define KERNEL_LOG_PATH "/run/pacha/kernel.log"
#define LOGD_DIR "/run/pacha"
#define LOGD_RING_BYTES 7168u
#define KERNEL_LOG_RING_BYTES (64u * 1024u)
#define LOGD_PENDING_SLOTS 16u

struct logd_pending {
    uint32_t record;
    uint16_t received;
    uint8_t source, count, last_bytes;
    char text[PACHA_LOG_MAX_RECORD_BYTES];
};

struct logd_state {
    pacha_fd_t filed_fd;
    uint64_t line_sequence;
    unsigned next_slot;
    struct logd_pending pending[LOGD_PENDING_SLOTS];
    char ring[LOGD_RING_BYTES];
    size_t used;
    char kernel_ring[KERNEL_LOG_RING_BYTES];
    size_t kernel_used;
    uint64_t kernel_cursor;
    uint64_t kernel_dropped;
    pacha_fd_t kernel_timer_fd;
    unsigned kernel_publish_pending;
};

static int filed_call(struct logd_state *state, kb2_fs_request_t *request,
    const char *path, const void *data, size_t length, kb2_fs_response_t *response)
{
    int status = filedfs_call(state->filed_fd, request, path, NULL, data, length,
        NULL, 0, response, NULL, 0, NULL, NULL);
    return status ? status : response->result < 0 ? (int)response->result : 0;
}

static int ensure_directory(struct logd_state *state)
{
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_MKDIRAT, .mode = 0755};
    kb2_fs_response_t response;
    int result = filed_call(state, &request, LOGD_DIR, NULL, 0, &response);
    return result == -EEXIST ? 0 : result;
}

static int publish_file(struct logd_state *state, const char *path,
    const char *bytes, size_t length)
{
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_OPENAT2,
        .flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, .mode = 0600};
    kb2_fs_response_t response;
    int result = filed_call(state, &request, path, NULL, 0, &response);
    if (result) return result;
    const uint64_t handle = response.handle;
    for (size_t offset = 0; !result && offset < length;) {
        request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_PWRITE,
            .handle = handle, .offset = (int64_t)offset, .length = length - offset};
        result = filed_call(state, &request, NULL, bytes + offset,
            length - offset, &response);
        if (!result && (response.result <= 0 ||
            (uint64_t)response.result > length - offset)) result = -EIO;
        if (!result) offset += (size_t)response.result;
    }
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_CLOSE, .handle = handle};
    int closed = filed_call(state, &request, NULL, NULL, 0, &response);
    return result ? result : closed;
}

static int publish(struct logd_state *state)
{
    return publish_file(state, LOGD_PATH, state->ring, state->used);
}

static void kernel_ring_append(struct logd_state *state,
    const char *bytes, size_t length)
{
    if (length > KERNEL_LOG_RING_BYTES) {
        bytes += length - KERNEL_LOG_RING_BYTES;
        length = KERNEL_LOG_RING_BYTES;
    }
    if (state->kernel_used + length > KERNEL_LOG_RING_BYTES) {
        const size_t removed = state->kernel_used + length - KERNEL_LOG_RING_BYTES;
        memmove(state->kernel_ring, state->kernel_ring + removed,
            state->kernel_used - removed);
        state->kernel_used -= removed;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char byte = (unsigned char)bytes[i];
        state->kernel_ring[state->kernel_used + i] =
            byte == '\n' || byte == '\t' || (byte >= 32 && byte < 127) ?
            (char)byte : '?';
    }
    state->kernel_used += length;
}

static int drain_kernel_log(struct logd_state *state)
{
    int dirty = 0;
    for (unsigned i = 0; i < 64; ++i) {
        struct pacha_kernel_log_read request = {
            .cursor = state->kernel_cursor,
            .limit = PACHA_KERNEL_LOG_READ_MAX_BYTES,
        };
        const long status = pacha_syscall2(PACHA_KERNEL_LOG_SYSCALL_READ,
            (uint64_t)(uintptr_t)&request, sizeof(request));
        if (status != PACHA_SYSCALL_OK) return dirty ? dirty : (int)-status;
        if (request.overrun) {
            static const char lost[] = "\n[kernel log ring overrun]\n";
            kernel_ring_append(state, lost, sizeof(lost) - 1);
            dirty = 1;
        }
        if (request.dropped > state->kernel_dropped) {
            /* The kernel deliberately drops contended fragments. Without
             * this marker, a missing failure line looks like no refusal was
             * logged at all, which makes a physical-only fault ambiguous. */
            char marker[96];
            const int length = snprintf(marker, sizeof(marker),
                "\n[kernel log ring dropped fragments=%llu]\n",
                (unsigned long long)request.dropped);
            if (length > 0 && (size_t)length < sizeof(marker)) {
                kernel_ring_append(state, marker, (size_t)length);
                dirty = 1;
            }
        }
        state->kernel_dropped = request.dropped;
        if (request.written) {
            kernel_ring_append(state, (const char *)request.bytes,
                (size_t)request.written);
            dirty = 1;
        }
        state->kernel_cursor = request.next_cursor;
        if (request.written < request.limit) break;
    }
    return dirty;
}

static void ring_append(struct logd_state *state, unsigned source,
    const char *text, size_t length)
{
    const char *prefix = source == 0 ? "logd: " :
        source == PACHA_LOG_SOURCE_NETD ? "netd: " :
        source == PACHA_LOG_SOURCE_KOBOX2_NET ? "kobox2-net: " :
        source == PACHA_LOG_SOURCE_GPUD ? "gpud: " :
        source == PACHA_LOG_SOURCE_KOBOX2_GPU ? "kobox2-gpu: " : "unknown: ";
    char line[320];
    struct timespec now = {0};
    const int timed = clock_gettime(CLOCK_MONOTONIC, &now) == 0;
    const int header = timed ? snprintf(line, sizeof(line),
        "#%llu +%lld.%03ld %s", (unsigned long long)++state->line_sequence,
        (long long)now.tv_sec, now.tv_nsec / 1000000, prefix) :
        snprintf(line, sizeof(line), "#%llu %s",
            (unsigned long long)++state->line_sequence, prefix);
    if (header < 0 || (size_t)header >= sizeof(line)) return;
    const size_t prefix_bytes = (size_t)header;
    size_t used = prefix_bytes;
    for (size_t i = 0; i < length && used + 2 < sizeof(line); ++i) {
        const unsigned char byte = (unsigned char)text[i];
        /* The file is intended for `cat` in an interactive TTY. Prevent
         * producer-controlled terminal escape sequences from reaching it. */
        line[used++] = byte >= 32 && byte < 127 ? (char)byte :
            byte == '\t' ? ' ' : byte == '\n' || byte == '\r' ? ' ' : '?';
    }
    while (used > prefix_bytes && line[used - 1] == ' ') --used;
    line[used++] = '\n';
    while (state->used + used > LOGD_RING_BYTES) {
        const char *end = memchr(state->ring, '\n', state->used);
        if (!end) { state->used = 0; break; }
        const size_t removed = (size_t)(end - state->ring) + 1;
        memmove(state->ring, state->ring + removed, state->used - removed);
        state->used -= removed;
    }
    memcpy(state->ring + state->used, line, used);
    state->used += used;
}

static int accept_fragment(struct logd_state *state,
    const struct pacha_ipc_msg *message)
{
    if (message->word0 != PACHA_LOG_WIRE_MAGIC || message->fd_count != 0)
        return 0;
    const uint64_t metadata = message->word1;
    const unsigned source = (unsigned)(metadata >> 56);
    const uint32_t record = (uint32_t)(metadata >> 24);
    const unsigned count = (unsigned)((metadata >> 16) & 0xffu);
    const unsigned index = (unsigned)((metadata >> 8) & 0xffu);
    const unsigned bytes = (unsigned)(metadata & 0xffu);
    if ((source != PACHA_LOG_SOURCE_NETD &&
            source != PACHA_LOG_SOURCE_KOBOX2_NET &&
            source != PACHA_LOG_SOURCE_GPUD &&
            source != PACHA_LOG_SOURCE_KOBOX2_GPU) || record == 0 ||
        count == 0 || count > PACHA_LOG_MAX_FRAGMENTS || index >= count ||
        bytes == 0 || bytes > PACHA_LOG_FRAGMENT_BYTES ||
        (index + 1 < count && bytes != PACHA_LOG_FRAGMENT_BYTES))
        return 0;
    struct logd_pending *pending = NULL;
    for (unsigned i = 0; i < LOGD_PENDING_SLOTS; ++i) {
        if (state->pending[i].record == record &&
            state->pending[i].source == source) {
            pending = &state->pending[i];
            break;
        }
    }
    if (!pending) {
        pending = &state->pending[state->next_slot++ % LOGD_PENDING_SLOTS];
        memset(pending, 0, sizeof(*pending));
        pending->record = record;
        pending->source = source;
        pending->count = count;
    }
    if (pending->count != count) {
        pending->record = 0;
        return 0;
    }
    memcpy(pending->text + index * PACHA_LOG_FRAGMENT_BYTES,
        &message->word2, bytes);
    pending->received |= (uint16_t)(1u << index);
    if (index + 1 == count) pending->last_bytes = bytes;
    if (pending->received != (uint16_t)((1u << count) - 1u)) return 0;
    ring_append(state, source, pending->text,
        (count - 1) * PACHA_LOG_FRAGMENT_BYTES + pending->last_bytes);
    pending->record = 0;
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc;
    const pacha_fd_t bootstrap = pacha_bootstrap_fd_from_argv(argv);
    struct logd_boot_config config = {0};
    if (bootstrap < 16 ||
        pacha_fd_read(bootstrap, &config, sizeof(config)) != (long)sizeof(config) ||
        config.magic != LOGD_BOOT_MAGIC || config.size != sizeof(config) ||
        config.endpoint_fd < 16 || config.filed_endpoint_fd < 16 ||
        config.ready_channel_fd < 16)
        return 2;
    (void)pacha_fd_close(bootstrap);
    struct logd_state state = {
        .filed_fd = (pacha_fd_t)config.filed_endpoint_fd,
        .kernel_timer_fd = -1,
    };
    int status = ensure_directory(&state);
    if (status == 0) {
        ring_append(&state, 0, "RAM service log started", 23);
        status = publish(&state);
    }
    if (status == 0) {
        const int kernel_dirty = drain_kernel_log(&state);
        if (kernel_dirty >= 0 ||
            kernel_dirty == -(int)PACHA_SYSCALL_ERR_EMPTY) {
            const int kernel_status = publish_file(&state, KERNEL_LOG_PATH,
                state.kernel_ring, state.kernel_used);
            if (kernel_status == 0) {
                const uint64_t rights = PACHA_FD_RIGHT_WAIT |
                    PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE |
                    PACHA_FD_RIGHT_READ;
                state.kernel_timer_fd = pacha_timerfd_create(
                    UINT64_C(250000000), UINT64_C(250000000),
                    rights, PACHA_FD_FLAG_CLOEXEC);
                if (state.kernel_timer_fd < 16)
                    fprintf(stderr, "[logd] kernel log timer unavailable\n");
            }
            else fprintf(stderr, "[logd] kernel log file unavailable status=%d\n",
                kernel_status);
        } else if (kernel_dirty != -(int)PACHA_SYSCALL_ERR_NOT_READY) {
            fprintf(stderr, "[logd] kernel log read unavailable status=%d\n",
                kernel_dirty);
        }
    }
    const struct pacha_ipc_msg ready = {
        .word0 = LOGD_READY_MAGIC,
        .word1 = (uint64_t)(int64_t)status,
    };
    const int ready_status = pacha_ipc_send((pacha_fd_t)config.ready_channel_fd, &ready);
    (void)pacha_fd_close((pacha_fd_t)config.ready_channel_fd);
    if (status != 0 || ready_status != 0) {
        fprintf(stderr, "[logd] RAM log setup failed status=%d\n",
            status != 0 ? status : ready_status);
        return 4;
    }
    for (;;) {
        struct pacha_ipc_msg message = {0};
        if (state.kernel_timer_fd >= 16) {
            struct pacha_pollfd waits[2] = {
                { .fd = (pacha_fd_t)config.endpoint_fd, .events = PACHA_FD_EVENT_READABLE },
                { .fd = state.kernel_timer_fd, .events = PACHA_FD_EVENT_READABLE },
            };
            status = (pacha_fd_t)pacha_fd_wait_many(waits, 2, PACHA_FD_WAIT_FOREVER);
            if (status < 0) break;
            if (waits[1].revents & PACHA_FD_EVENT_READABLE) {
                uint64_t expirations = 0;
                (void)pacha_fd_read(state.kernel_timer_fd, &expirations,
                    sizeof(expirations));
                if (drain_kernel_log(&state) > 0) state.kernel_publish_pending = 1;
                if (state.kernel_publish_pending &&
                    publish_file(&state, KERNEL_LOG_PATH,
                        state.kernel_ring, state.kernel_used) == 0)
                    state.kernel_publish_pending = 0;
            }
            if (!(waits[0].revents & PACHA_FD_EVENT_READABLE)) continue;
        }
        status = pacha_ipc_recv((pacha_fd_t)config.endpoint_fd, &message);
        if (status != 0) continue;
        unsigned dirty = accept_fragment(&state, &message);
        /* Drain a bounded burst before touching filed. This keeps Linux
         * printk bursts from filling the producer's IPC queue behind three
         * filesystem RPCs per individual line. */
        for (unsigned i = 0; i < 127; ++i) {
            message = (struct pacha_ipc_msg){0};
            if (pacha_ipc_recv((pacha_fd_t)config.endpoint_fd, &message) != 0) break;
            dirty |= (unsigned)accept_fragment(&state, &message);
        }
        if (dirty) {
            status = publish(&state);
            if (status != 0)
                fprintf(stderr, "[logd] RAM log update failed status=%d\n", status);
        }
    }
    return 4;
}
