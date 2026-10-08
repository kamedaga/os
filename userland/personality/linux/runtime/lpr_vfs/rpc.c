#include "../lpr_filed_internal.h"
#include "rpc.h"
#include "session.h"
#include "diagnostic.h"
#include "../lpr_common/boot_diagnostic.h"

#ifdef PH_FS_CHANNEL_PROFILE
void ph_fs_channel_profile(uint32_t opcode, const uint64_t cycles[4],
    unsigned notifications, unsigned waits)
{
    /* The FS session serializes these counters. Emit once per 1024 calls,
     * outside the measured partition, instead of logging every doorbell. */
    static uint64_t rows[3][7];
    unsigned index;
    if (opcode == KB2_FILESYSTEM_OP_STATX) index = 0;
    else if (opcode == KB2_FILESYSTEM_OP_PREAD) index = 1;
    else if (opcode == KB2_FILESYSTEM_OP_PWRITE) index = 2;
    else return;
    uint64_t *row = rows[index];
    ++row[0];
    for (unsigned i = 0; i < 4; ++i) row[i + 1] += cycles[i];
    row[5] += notifications;
    row[6] += waits;
    if (row[0] != 1024) return;
    char line[192] = "[fs-ring]";
    unsigned n = 9;
    const uint64_t values[] = {lpr_linux_current_pid, opcode, row[0],
        row[1], row[2], row[3], row[4], row[5], row[6]};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        line[n++] = ' ';
        for (int shift = 60; shift >= 0; shift -= 4)
            line[n++] = "0123456789abcdef"[(values[i] >> shift) & 15];
    }
    line[n++] = '\n';
    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_LOG, (uint64_t)(uintptr_t)line, n);
    for (unsigned i = 0; i < 7; ++i) row[i] = 0;
}
#endif

#if defined(PH_FS_CHANNEL_PROFILE) && defined(PH_FS_CHANNEL_TRACE)
void ph_fs_channel_trace(uint64_t channel, uint32_t opcode, uint64_t sequence,
    uint64_t publish, uint64_t completion)
{
    if ((sequence & 255) || (opcode != KB2_FILESYSTEM_OP_STATX &&
        opcode != KB2_FILESYSTEM_OP_PREAD && opcode != KB2_FILESYSTEM_OP_PWRITE)) return;
    char line[144] = "[fs-client-trace]";
    unsigned n = 17;
    const uint64_t values[] = {channel, lpr_linux_current_pid, opcode,
        sequence, publish, completion};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        line[n++] = ' ';
        for (int shift = 60; shift >= 0; shift -= 4)
            line[n++] = "0123456789abcdef"[(values[i] >> shift) & 15];
    }
    line[n++] = '\n';
    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_LOG, (uint64_t)(uintptr_t)line, n);
}
#endif

#ifdef PH_FS_CHANNEL_DIAGNOSTICS
void ph_fs_channel_diagnostic(unsigned stage, int status, uint64_t value)
{
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
    lpr_boot_diagnostic_fs_emit(stage == 26 ? "fs.queue.publish" : "fs.queue.error",
        value, stage, 0, status, 0);
#else
    lpr_fs_diag(stage, status, value);
#endif
}
#endif

static int64_t path_bytes(const char *path, uint64_t *bytes)
{
    *bytes = 0;
    if (!path) return 0;
    /* This is Linux's PATH_MAX, not an inline transport capacity. Linux
     * cannot accept a pathname whose terminating NUL is beyond this point. */
    const size_t length = lpr_strnlen(path, 4096u);
    if (length == 4096u) return -LPR_LINUX_ENAMETOOLONG;
    *bytes = length + 1u;
    return 0;
}

static void stamp_request(kb2_fs_request_t *request)
{
    if (!request->generation) request->generation = 1;
    request->sequence = lpr_next_request_id(&lpr_request_id);
    request->client = request->credential_generation = 0;
}

static int64_t call_direct(kb2_fs_request_t *request,
    const struct lpr_fs_buffer *input, unsigned input_count, kb2_fs_response_t *response,
    const struct lpr_fs_buffer *output, unsigned output_count)
{
    size_t used = 0;
    lpr_boot_diagnostic_fs_emit("fs.queue.call", request->opcode, request->sequence, 0, 0, 0);
    int64_t status = lpr_fs_session_call(request, input, input_count, response, output, output_count, &used);
    lpr_boot_diagnostic_fs_emit("fs.queue.return", request->opcode, request->sequence, used, status, 0);
    if (status) lpr_fs_diag(11, status, request->opcode);
    else status = response->result;
    return status;
}

int64_t lpr_fs_call_buffers(kb2_fs_request_t *request,
    const struct lpr_fs_buffer *input, unsigned input_count, kb2_fs_response_t *response,
    const struct lpr_fs_buffer *output, unsigned output_count)
{
    if (!request || !response || (request->opcode != KB2_FILESYSTEM_OP_READ &&
        request->opcode != KB2_FILESYSTEM_OP_WRITE)) return -LPR_LINUX_EINVAL;
    if (lpr_fs_session_terminal()) return -LPR_LINUX_EIO;
    lpr_boot_diagnostic_fs_begin();
    int64_t status = lpr_filed_endpoint_ready();
    if (!status) {
        stamp_request(request);
        status = call_direct(request, input, input_count, response, output, output_count);
    }
    lpr_boot_diagnostic_fs_end(1);
    return status;
}

static int64_t call_impl(kb2_fs_request_t *request, const char *path,
    const char *second_path, const void *data, uint64_t data_bytes,
    void *output, uint64_t output_bytes, kb2_fs_response_t *response,
    int transfer_fd, int second_transfer_fd, int *returned_fds, uint64_t returned_capacity)
{
    uint64_t first_bytes, second_bytes;
    if (!request || !response || (!data && data_bytes) || (!output && output_bytes))
        return -LPR_LINUX_EFAULT;
    if (lpr_fs_session_terminal() && request->opcode != KB2_FILESYSTEM_OP_SESSION_DETACH)
        return -LPR_LINUX_EIO;
    if (returned_capacity > 3 || (returned_capacity && !returned_fds))
        return -LPR_LINUX_EINVAL;
    for (uint64_t i = 0; i < returned_capacity; ++i) returned_fds[i] = -1;
    int64_t status = path_bytes(path, &first_bytes);
    if (!status) status = path_bytes(second_path, &second_bytes);
    if (status) return status;
    if (data_bytes > UINT64_MAX - KB2_FILESYSTEM_REQUEST_SIZE - first_bytes - second_bytes ||
        output_bytes > UINT64_MAX - KB2_FILESYSTEM_RESPONSE_SIZE)
        return -LPR_LINUX_EOVERFLOW;
    const uint64_t request_bytes = KB2_FILESYSTEM_REQUEST_SIZE +
        first_bytes + second_bytes + data_bytes;
    const uint64_t reply_bytes = KB2_FILESYSTEM_RESPONSE_SIZE + output_bytes;
    const uint64_t used = request_bytes > reply_bytes ? request_bytes : reply_bytes;
    const uint64_t map_bytes = lpr_page_align_up(used);
    if (!map_bytes) return -LPR_LINUX_EOVERFLOW;
    lpr_boot_diagnostic_fs_begin();
    if (request->opcode >= KB2_FILESYSTEM_OP_OPENAT2)
        lpr_boot_diagnostic_fs_emit("fs.rpc.enter", request->opcode, request->handle,
            map_bytes, 0, 0);
    /* The one retirement request must use the existing authenticated client,
     * never bind a new identity while tearing down the old queue generation. */
    status = request->opcode == KB2_FILESYSTEM_OP_SESSION_DETACH ?
        (lpr_filed_client_fd >= 16 && lpr_filed_endpoint_checked > 0 &&
            lpr_filed_client_id != 0 ? 0 : -LPR_LINUX_EIO) :
        lpr_filed_endpoint_ready();
    if (status) {
        lpr_boot_diagnostic_fs_emit("fs.rpc.endpoint-error", request->opcode, 0, 0, status, 0);
        lpr_boot_diagnostic_fs_end(request->opcode >= KB2_FILESYSTEM_OP_OPENAT2);
        return status;
    }
    stamp_request(request);
    request->path = (kb2_fs_span_t){0};
    request->second_path = (kb2_fs_span_t){0};
    request->groups = (kb2_fs_span_t){0};
    request->data = (kb2_fs_span_t){0};
    uint64_t at = KB2_FILESYSTEM_REQUEST_SIZE;
    if (first_bytes) { request->path = (kb2_fs_span_t){at, first_bytes}; at += first_bytes; }
    if (second_bytes) { request->second_path = (kb2_fs_span_t){at, second_bytes}; at += second_bytes; }
    if (data_bytes) request->data = (kb2_fs_span_t){at, data_bytes};
    uint64_t required_rights;
    if (kb2_fs_request_policy(request, &required_rights) == KB2_FS_OPERATION_DATA) {
        if (transfer_fd >= 16 || second_transfer_fd >= 16 || returned_capacity) status = -LPR_LINUX_EINVAL;
        else {
            /* No pre-admission per-call VMO. Input is assembled directly into
             * the reserved arena, whose whole lifetime is charged by filed. */
            const struct lpr_fs_buffer input[] = {{path, first_bytes}, {second_path, second_bytes},
                {data, data_bytes}};
            const struct lpr_fs_buffer destination = {output, output_bytes};
            status = call_direct(request, input, 3, response, &destination, 1);
        }
        lpr_boot_diagnostic_fs_end(1);
        return status;
    }
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    const int64_t vmo = lpr_pacha_syscall3(PACHAOS_SYSCALL_VMO_CREATE,
        map_bytes, rights, PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC);
    if (vmo < 16) {
        lpr_boot_diagnostic_fs_emit("fs.rpc.vmo-error", request->opcode, map_bytes,
            vmo, lpr_pacha_status_to_errno(vmo), vmo);
        lpr_boot_diagnostic_fs_end(request->opcode >= KB2_FILESYSTEM_OP_OPENAT2);
        return lpr_pacha_status_to_errno(vmo);
    }
    const int64_t mapped = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP, vmo,
        0, map_bytes, PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_SHARED, 0);
    if (mapped < 4096) {
        lpr_boot_diagnostic_fs_emit("fs.rpc.map-error", request->opcode, vmo,
            map_bytes, lpr_pacha_status_to_errno(mapped), mapped);
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, vmo);
        lpr_boot_diagnostic_fs_end(request->opcode >= KB2_FILESYSTEM_OP_OPENAT2);
        return lpr_pacha_status_to_errno(mapped);
    }
    uint8_t *wire = (uint8_t *)(uintptr_t)mapped;
    at = KB2_FILESYSTEM_REQUEST_SIZE;
    if (first_bytes) {
        request->path = (kb2_fs_span_t){at, first_bytes};
        lpr_memcpy(wire + at, path, first_bytes);
        at += first_bytes;
    }
    if (second_bytes) {
        request->second_path = (kb2_fs_span_t){at, second_bytes};
        lpr_memcpy(wire + at, second_path, second_bytes);
        at += second_bytes;
    }
    if (data_bytes) {
        request->data = (kb2_fs_span_t){at, data_bytes};
        lpr_memcpy(wire + at, data, data_bytes);
    }
    if (kb2_fs_request_encode(wire, KB2_FILESYSTEM_REQUEST_SIZE,
        request_bytes, request)) {
        status = -LPR_LINUX_EINVAL;
        goto release;
    }
    struct pacha_ipc_fd grants[3] = {{.fd = vmo,
        .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE}};
    struct pacha_ipc_fd received[3] = {{0}};
    struct pacha_ipc_msg message = {.word0 = request->opcode,
        .word1 = request_bytes, .word2 = reply_bytes,
        .word3 = request->sequence, .fds = grants, .fd_count = 1};
    if (transfer_fd >= 16) {
        grants[1].fd = (uint32_t)transfer_fd;
        grants[1].rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_WAIT |
            PACHA_FD_RIGHT_POLL;
        message.fd_count = 2;
        if (request->opcode == KB2_FILESYSTEM_OP_EXEC_SELF)
            grants[1].rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP |
                PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SET_FLAGS |
                PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ;
    }
    if (second_transfer_fd >= 16) {
        grants[1].rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER;
        grants[2].fd = (uint32_t)second_transfer_fd;
        grants[2].rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_SEND |
            PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL |
            PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER;
        message.fd_count = 3;
    }
    lpr_boot_diagnostic_fs_emit("fs.control.call", request->opcode, request->sequence,
        lpr_filed_client_fd, 0, 0);
    const int64_t reply = lpr_native_ipc_call_wait(lpr_filed_client_fd, &message);
    lpr_boot_diagnostic_fs_emit("fs.control.called", request->opcode, request->sequence,
        reply, reply < 16 ? lpr_pacha_status_to_errno(reply) : 0, reply);
    if (reply < 16) {
        status = lpr_pacha_status_to_errno(reply);
        goto release;
    }
    struct pacha_ipc_msg completion = {.fds = received, .fd_capacity = 3};
    lpr_boot_diagnostic_fs_emit("fs.control.recv", request->opcode, reply, 0, 0, 0);
    status = lpr_native_ipc_recv_wait(reply, &completion);
    lpr_boot_diagnostic_fs_emit("fs.control.received", completion.word0,
        completion.word3, completion.word2,
        status ? lpr_pacha_status_to_errno(status) : (int64_t)completion.word1, status);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, reply);
    if (status) {
        status = lpr_pacha_status_to_errno(status);
    } else if (completion.word0 != request->opcode ||
        completion.word3 != request->sequence || completion.word2 > reply_bytes ||
        kb2_fs_response_decode(wire, completion.word2, response) ||
        !kb2_fs_response_matches(response, request) ||
        response->data_length > output_bytes ||
        (int64_t)completion.word1 != response->result) {
        status = -LPR_LINUX_EIO;
    } else {
        status = response->result;
        if (response->data_length)
            lpr_memcpy(output, wire + KB2_FILESYSTEM_RESPONSE_SIZE, response->data_length);
        if (completion.fd_count > returned_capacity) {
            status = -LPR_LINUX_EIO;
        } else if (status >= 0) {
            for (uint64_t i = 0; i < completion.fd_count; ++i) {
                returned_fds[i] = (int)received[i].fd;
                received[i].fd = 0;
            }
        }
    }
    for (unsigned i = 0; i < 3; ++i)
        if (received[i].fd >= 16)
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, received[i].fd);
release:
    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP, mapped, map_bytes);
    (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE, vmo);
    if (request->opcode >= KB2_FILESYSTEM_OP_OPENAT2)
        lpr_boot_diagnostic_fs_emit("fs.rpc.return", request->opcode, request->sequence, 0, status, 0);
    lpr_boot_diagnostic_fs_end(request->opcode >= KB2_FILESYSTEM_OP_OPENAT2);
    return status;
}

int64_t lpr_fs_call_many(kb2_fs_request_t *request, const char *path,
    const char *second_path, const void *data, uint64_t data_bytes,
    void *output, uint64_t output_bytes, kb2_fs_response_t *response,
    int transfer_fd, int *returned_fds, uint64_t returned_capacity)
{
    return call_impl(request, path, second_path, data, data_bytes, output,
        output_bytes, response, transfer_fd, -1, returned_fds, returned_capacity);
}

int64_t lpr_fs_call_attach(kb2_fs_request_t *request, int vmo, int doorbell,
    kb2_fs_response_t *response)
{
    return call_impl(request, 0, 0, 0, 0, 0, 0, response, vmo, doorbell, 0, 0);
}

int64_t lpr_fs_call(kb2_fs_request_t *request, const char *path,
    const char *second_path, const void *data, uint64_t data_bytes,
    void *output, uint64_t output_bytes, kb2_fs_response_t *response,
    int transfer_fd, int *returned_fd)
{
    return lpr_fs_call_many(request, path, second_path, data, data_bytes,
        output, output_bytes, response, transfer_fd, returned_fd,
        returned_fd ? 1 : 0);
}
