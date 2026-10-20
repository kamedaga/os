#include "lpr_ipc_mock.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"
#include "../userland/personality/linux/runtime/lpr_vfs/session.h"
#include "../userland/personality/linux/runtime/lpr_common/boot_diagnostic.h"

lpr_state_t lpr_state;
static unsigned log_calls;
static int64_t log_return;
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
lpr_boot_diagnostic_state_t lpr_boot_diagnostic_state;
#endif
static unsigned char *mapping;
static size_t mapping_size;
static uint64_t control_calls, queue_calls, closes, unmaps;
static unsigned malformed;
static int64_t injected;
static int return_capability;
static struct pacha_ipc_msg completion;
static uint32_t expected_opcode;
static const char *expected_first, *expected_second;
static uint64_t expected_handle, expected_second_handle, expected_flags, expected_offset, expected_length;
static lpr_filed_backend_t first_file = {.active = 1, .handle = 112};
static lpr_filed_backend_t second_file = {.active = 1, .handle = 223};
static int remap_on_bind;
static int terminal;
static unsigned endpoint_calls;
static int route_case;
static unsigned route_opens, route_probes, service_opens;
static const char *expected_service_path;
static uint64_t service_flags;
int lpr_fs_session_terminal(void) { return terminal; }
static int64_t service_open(const char *path,uint64_t flags)
{ assert(expected_service_path && !strcmp(path,expected_service_path)); assert(flags==service_flags); ++service_opens; return 77; }
int64_t lpr_drm_open_path(const char *path,uint64_t flags) { return service_open(path,flags); }
int64_t lpr_input_open_path(const char *path,uint64_t flags) { return service_open(path,flags); }
int64_t lpr_tty_open_path(const char *path,uint64_t flags) { return service_open(path,flags); }

void lpr_cwd_init(void) { if (!lpr_cwd_checked) { lpr_cwd_checked = 1; lpr_cwd_handle = 313; } }
lpr_filed_backend_t *lpr_filed_backend(uint64_t fd)
{
    return fd == 3 ? &first_file : fd == 4 ? &second_file : NULL;
}
int lpr_fd_is_filed(uint64_t fd) { return lpr_filed_backend(fd) != NULL; }
lpr_block_backend_t *lpr_block_backend(uint64_t fd) { (void)fd; return NULL; }
int64_t lpr_block_size(uint64_t fd) { (void)fd; assert(0); return 0; }

void *lpr_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
size_t lpr_strnlen(const char *s, size_t n) { size_t i = 0; while (i < n && s[i]) ++i; return i; }
uint64_t lpr_page_align_up(uint64_t n) { return n > UINT64_MAX - 4095 ? 0 : (n + 4095) & ~4095ull; }
int64_t lpr_filed_endpoint_ready(void)
{
    ++endpoint_calls;
    lpr_filed_client_fd = 50;
    lpr_filed_client_id = 2; lpr_filed_endpoint_checked = 1;
    if (remap_on_bind) {
        first_file.handle = 511; second_file.handle = 522;
        lpr_cwd_handle = 533; remap_on_bind = 0;
    }
    return 0;
}
int64_t lpr_filed_fd_binding(uint64_t fd)
{ return lpr_fd_is_filed(fd) ? lpr_filed_endpoint_ready() : 0; }
int64_t lpr_pacha_status_to_errno(int64_t status) { return status; }
uint64_t lpr_next_request_id(volatile uint64_t *value) { return ++*value; }

static size_t answer(unsigned char *wire, size_t request_size, size_t capacity)
{
    kb2_fs_request_t request;
    assert(!kb2_fs_request_decode(wire, request_size, request_size, &request));
    assert(!kb2_fs_request_validate_paths(wire, request_size, &request));
    assert(!request.client && !request.credential_generation);
    if (expected_opcode) {
        /* Decode real RPC bytes against the service contract, not the
         * caller's operand order, so swapping either pathname fails here. */
        if (request.opcode != expected_opcode || request.handle != expected_handle)
            fprintf(stderr, "wire capture mismatch: opcode=%u expected=%u handle=%llu expected=%llu\n",
                request.opcode, expected_opcode, (unsigned long long)request.handle,
                (unsigned long long)expected_handle);
        assert(request.opcode == expected_opcode && request.handle == expected_handle);
        assert(request.second_handle == expected_second_handle && request.flags == expected_flags);
        assert((uint64_t)request.offset == expected_offset && request.length == expected_length && !request.data.length);
        if (expected_first) {
            assert(request.path.length == strlen(expected_first) + 1);
            assert(!memcmp(wire + request.path.offset, expected_first, request.path.length));
        } else assert(!request.path.length);
        if (expected_second) {
            assert(request.second_path.length == strlen(expected_second) + 1);
            assert(!memcmp(wire + request.second_path.offset, expected_second, request.second_path.length));
        } else assert(!request.second_path.length);
    }
    kb2_fs_response_t response = {.opcode = request.opcode,
        .generation = request.generation, .sequence = request.sequence,
        .result = injected};
    if (route_case && request.opcode == KB2_FILESYSTEM_OP_OPENAT2) {
        ++route_opens;
        assert(request.mask == 0x7ffu);
        if (route_case >= 4) {
            response.handle = 432;
            response.data_length = route_case == 5 ? 0 : KB2_FILESYSTEM_STATX_SIZE;
        } else response.result = route_case == 3 ? -LPR_LINUX_EACCES : -LPR_LINUX_ENXIO;
    }
    if (route_case && request.opcode == KB2_FILESYSTEM_OP_DEVICE_ROUTE) {
        ++route_probes;
        assert(request.flags == service_flags && request.mode == 0600 &&
            request.path.length == sizeof("/alias"));
        if (route_case == 2) response.result = -LPR_LINUX_ENODEV;
        else {
            response.result = 0; response.handle = 432;
            response.service = FILED_SERVICE_GPUD_DRM;
            response.data_length = KB2_FILESYSTEM_STATX_SIZE;
        }
    }
    if (!injected) {
        if (request.opcode == KB2_FILESYSTEM_OP_READ) {
            response.result = request.length;
            response.data_length = request.length;
        } else if (request.opcode == KB2_FILESYSTEM_OP_WRITE) {
            assert(request.data.length == request.length);
            for (uint64_t i = 0; i < request.data.length; ++i)
                assert(wire[request.data.offset + i] == (unsigned char)(i * 19u));
            response.result = request.length;
        } else if (request.opcode == KB2_FILESYSTEM_OP_FILE_VMO) {
            response.result = request.length;
        } else if (request.opcode == KB2_FILESYSTEM_OP_TRANSFER_DUP) {
            response.handle = 999;
        } else if (request.opcode == KB2_FILESYSTEM_OP_SESSION_DETACH) {
            response.handle = request.second_handle;
        }
    }
    assert(!kb2_fs_response_encode(wire, capacity, &response));
    for (uint64_t i = 0; i < response.data_length; ++i)
        wire[KB2_FILESYSTEM_RESPONSE_SIZE + i] = (unsigned char)(i * 13u);
    if (route_case && request.opcode == KB2_FILESYSTEM_OP_DEVICE_ROUTE && !response.result) {
        memset(wire+KB2_FILESYSTEM_RESPONSE_SIZE,0,KB2_FILESYSTEM_STATX_SIZE);
        wire[KB2_FILESYSTEM_RESPONSE_SIZE+KB2_FILESYSTEM_STATX_MODE_OFFSET+1] = 0x20;
        wire[KB2_FILESYSTEM_RESPONSE_SIZE+KB2_FILESYSTEM_STATX_RDEV_MAJOR_OFFSET] = 226;
        wire[KB2_FILESYSTEM_RESPONSE_SIZE+KB2_FILESYSTEM_STATX_RDEV_MINOR_OFFSET] = 128;
    }
    if (malformed == 1) wire[0] = 'X';
    if (malformed == 2) wire[KB2_FILESYSTEM_RESPONSE_GENERATION_OFFSET] ^= 1;
    completion = (struct pacha_ipc_msg){.word0 = request.opcode,
        .word1 = response.result, .word2 = KB2_FILESYSTEM_RESPONSE_SIZE + response.data_length,
        .word3 = request.sequence};
    return (size_t)completion.word2;
}

int64_t lpr_pacha_syscall3(uint64_t nr, uint64_t size, uint64_t rights, uint64_t flags)
{
    assert(nr == PACHAOS_SYSCALL_VMO_CREATE);
    assert(rights & PACHA_FD_RIGHT_INSPECT);
    assert(flags == (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC));
    mapping_size = size;
    return 40;
}

int64_t lpr_pacha_syscall6(uint64_t nr, uint64_t fd, uint64_t addr,
    uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset)
{
    assert(nr == PACHAOS_SYSCALL_MMAP && fd == 40 && !addr && !offset);
    assert(size == mapping_size && flags == PACHAOS_MMAP_SHARED);
    assert(prot == (PACHAOS_PROT_READ | PACHAOS_PROT_WRITE));
    mapping = calloc(1, size);
    assert(mapping);
    return (int64_t)(uintptr_t)mapping;
}

int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE && fd >= 16);
    ++closes;
    return 0;
}

static int64_t fixture_prepare_request(uint64_t nr, uint64_t a, uint64_t b)
{
    if (nr == PACHAOS_SYSCALL_LOG) {
        assert(b > 0 && b <= 256 && ((const char *)(uintptr_t)a)[b - 1] == '\n');
        ++log_calls;
        return log_return;
    }
    if (nr == PACHAOS_SYSCALL_MUNMAP) {
        assert((void *)(uintptr_t)a == mapping && b == mapping_size);
        free(mapping); mapping = NULL; ++unmaps; return 0;
    }
    assert(nr == PACHAOS_SYSCALL_IPC_CALL && a == 50);
    const struct pacha_ipc_msg *request = (const void *)(uintptr_t)b;
    assert(request->fds[0].fd == 40 &&
        (request->fds[0].rights & PACHA_FD_RIGHT_INSPECT));
    assert(request->word1 <= mapping_size && request->word2 <= mapping_size);
    if (request->word0 == KB2_FILESYSTEM_OP_TRANSFER_DUP) {
        assert(request->fd_count == 2 && request->fds[1].fd == 77);
    } else assert(request->fd_count == 1);
    answer(mapping, request->word1, request->word2);
    if (malformed == 3) completion.word2 = request->word2 + 1;
    ++control_calls;
    return 41;
}

static int64_t fixture_finish_request(uint64_t nr, uint64_t fd, uint64_t raw,
    uint64_t timeout, uint64_t flags)
{
    assert(nr == PACHAOS_SYSCALL_IPC_RECV_WAIT && fd == 41 &&
        timeout == UINT64_MAX && flags == PACHA_WAIT_UNINTERRUPTIBLE);
    struct pacha_ipc_msg *reply = (void *)(uintptr_t)raw;
    struct pacha_ipc_fd *caps = reply->fds;
    assert(reply->fd_capacity == 3);
    *reply = completion;
    reply->fds = caps;
    if (return_capability) { caps[0].fd = 42; reply->fd_count = 1; }
    return 0;
}

int lpr_fs_session_call(kb2_fs_request_t *request,
    const struct lpr_fs_buffer *input, unsigned input_count, kb2_fs_response_t *response,
    const struct lpr_fs_buffer *output, unsigned output_count, size_t *used)
{
    size_t bytes = KB2_FILESYSTEM_REQUEST_SIZE, capacity = KB2_FILESYSTEM_RESPONSE_SIZE;
    for (unsigned i = 0; i < input_count; ++i) bytes += input[i].length;
    for (unsigned i = 0; i < output_count; ++i) capacity += output[i].length;
    size_t allocated = bytes > capacity ? bytes : capacity;
    unsigned char *wire = calloc(1, allocated); assert(wire);
    request->generation = 7;
    assert(!kb2_fs_request_encode(wire, KB2_FILESYSTEM_REQUEST_SIZE, bytes, request));
    size_t copied = KB2_FILESYSTEM_REQUEST_SIZE;
    for (unsigned i = 0; i < input_count; ++i) {
        if (input[i].length) memcpy(wire + copied, input[i].bytes, input[i].length);
        copied += input[i].length;
    }
    ++queue_calls;
    *used = answer(wire, bytes, capacity);
    if (kb2_fs_response_decode(wire, *used, response) ||
        !kb2_fs_response_matches(response, request)) { free(wire); return -LPR_LINUX_EIO; }
    copied = 0;
    for (unsigned i = 0; i < output_count && copied < response->data_length; ++i) {
        size_t n = output[i].length;
        if (n > response->data_length - copied) n = response->data_length - copied;
        if (n) memcpy((void *)output[i].bytes, wire + KB2_FILESYSTEM_RESPONSE_SIZE + copied, n);
        copied += n;
    }
    free(wire);
    return 0;
}

static void test_operation_operands(void)
{
    expected_opcode = KB2_FILESYSTEM_OP_SYMLINKAT;
    expected_first = "../missing//./target";
    expected_second = "subdir/new-link";
    expected_handle = 112;
    assert(!lpr_linux_symlinkat((uintptr_t)expected_first, 3, (uintptr_t)expected_second));
    expected_first = "/absolute/nonexistent/target";
    expected_handle = 223;
    assert(!lpr_linux_symlinkat((uintptr_t)expected_first, 4, (uintptr_t)expected_second));
    expected_second = "/absolute/new-link";
    expected_handle = 0;
    assert(!lpr_linux_symlinkat((uintptr_t)expected_first, UINT64_MAX, (uintptr_t)expected_second));
    expected_second = "relative/new-link";
    expected_handle = 313;
    assert(!lpr_linux_symlinkat((uintptr_t)expected_first, (uint64_t)LPR_LINUX_AT_FDCWD,
        (uintptr_t)expected_second));
    const uint64_t before = queue_calls + control_calls;
    assert(lpr_linux_symlinkat((uintptr_t)expected_first, UINT64_MAX,
        (uintptr_t)expected_second) == -LPR_LINUX_EBADF);
    assert(lpr_linux_symlinkat(0, 3, (uintptr_t)expected_second) == -LPR_LINUX_EFAULT);
    assert(lpr_linux_symlinkat((uintptr_t)expected_first, 3, 0) == -LPR_LINUX_EFAULT);
    assert(queue_calls + control_calls == before);

    expected_opcode = KB2_FILESYSTEM_OP_LINKAT;
    expected_first = "old/../source";
    expected_second = "new//hard-link";
    expected_handle = 112;
    expected_second_handle = 223;
    expected_flags = 0x400; /* AT_SYMLINK_FOLLOW remains a Linux decision. */
    assert(!lpr_linux_linkat(3, (uintptr_t)expected_first, 4,
        (uintptr_t)expected_second, expected_flags));
    expected_first = "";
    expected_flags = LPR_LINUX_AT_EMPTY_PATH;
    assert(!lpr_linux_linkat(3, (uintptr_t)expected_first, 4,
        (uintptr_t)expected_second, expected_flags));
    expected_first = "/source";
    expected_second = "/hard-link";
    expected_handle = expected_second_handle = expected_flags = 0;
    assert(!lpr_linux_linkat(UINT64_MAX, (uintptr_t)expected_first, UINT64_MAX,
        (uintptr_t)expected_second, 0));

    expected_first = expected_second = NULL;
    expected_opcode = KB2_FILESYSTEM_OP_SYNC;
    assert(!lpr_linux_sync());
    expected_opcode = KB2_FILESYSTEM_OP_SYNCFS;
    expected_handle = 112;
    assert(!lpr_linux_syncfs(3));
    expected_handle = 223;
    assert(!lpr_linux_syncfs(4));
    const uint64_t before_syncfs = queue_calls + control_calls;
    assert(lpr_linux_syncfs(UINT64_MAX) == -LPR_LINUX_EBADF);
    assert(queue_calls + control_calls == before_syncfs);

    expected_opcode = KB2_FILESYSTEM_OP_FTRUNCATE;
    expected_handle = 112;
    const uint64_t lengths[] = {0, 4097, UINT64_C(0x100000000)};
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        expected_offset = lengths[i];
        assert(!lpr_linux_ftruncate(3, lengths[i]));
    }
    expected_offset = UINT64_C(0x8000000000000000);
    injected = -LPR_LINUX_EINVAL;
    assert(lpr_linux_ftruncate(3, expected_offset) == injected);
    injected = 0;
    expected_opcode = 0;
}

static void test_binding_before_handle_capture(void)
{
    /* A real first endpoint lookup changes connection-relative handles in
     * place. Decode actual outgoing RPC bytes, not the caller's old values. */
    expected_offset = expected_flags = expected_second_handle = 0;
    expected_opcode = KB2_FILESYSTEM_OP_SYNCFS; expected_handle = 511;
    remap_on_bind = 1;
    assert(!lpr_linux_syncfs(3));
    first_file.handle = 112; second_file.handle = 223;
    expected_opcode = KB2_FILESYSTEM_OP_LINKAT;
    expected_first = "source"; expected_second = "destination";
    expected_handle = 511; expected_second_handle = 522;
    remap_on_bind = 1;
    assert(!lpr_linux_linkat(3, (uintptr_t)expected_first, 4, (uintptr_t)expected_second, 0));
    first_file.handle = 112; second_file.handle = 223;
    expected_second_handle = 0; expected_handle = 533;
    expected_second = "/destination";
    remap_on_bind = 1;
    assert(!lpr_linux_linkat((uint64_t)LPR_LINUX_AT_FDCWD, (uintptr_t)expected_first,
        UINT64_MAX, (uintptr_t)"/destination", 0));
    first_file.handle = 112; second_file.handle = 223;
    expected_first = expected_second = NULL;
    expected_opcode = KB2_FILESYSTEM_OP_READ; expected_handle = 511;
    expected_length = 17;
    unsigned char bytes[17];
    remap_on_bind = 1;
    assert(lpr_filed_io(KB2_FILESYSTEM_OP_READ, 3, (uintptr_t)bytes, sizeof(bytes), 0) == 17);
    for (unsigned i = 0; i < sizeof(bytes); ++i) assert(bytes[i] == (unsigned char)(i * 13u));
    /* Numeric transfer operands are captured after binding, just as the
     * manifest producer must do; decode the actual outgoing wire ID. */
    expected_opcode = KB2_FILESYSTEM_OP_TRANSFER_DUP; expected_length = 0;
    uint64_t transferred = 0;
    assert(!lpr_filed_transfer_dup_handle(first_file.handle, 0, 77, &transferred));
    assert(transferred == 999);
    first_file.handle = 112; second_file.handle = 223;
    expected_opcode = 0; expected_first = expected_second = NULL;
    expected_handle = expected_second_handle = 0;
}

static void test_device_routes(void)
{
    service_flags = LPR_LINUX_O_CREAT | LPR_LINUX_O_RDWR | LPR_LINUX_O_TRUNC;
    uint64_t handle,kind;
    route_case = 1;
    const uint64_t control_before=control_calls, queue_before=queue_calls;
    assert(!lpr_filed_open_handle_at(LPR_LINUX_AT_FDCWD,"/alias",service_flags,0600,&handle,&kind));
    assert(handle==432 && kind==FILED_SERVICE_GPUD_DRM && route_opens==1 && route_probes==1);
    assert(control_calls==control_before+1 && queue_calls==queue_before+1);
    route_case = 2;
    assert(lpr_filed_open_handle_at(LPR_LINUX_AT_FDCWD,"/alias",service_flags,0600,&handle,&kind)==-LPR_LINUX_ENXIO);
    assert(route_opens==2 && route_probes==2); /* Never replay ordinary OPENAT2. */
    route_case = 3;
    assert(lpr_filed_open_handle_at(LPR_LINUX_AT_FDCWD,"/alias",service_flags,0600,&handle,&kind)==-LPR_LINUX_EACCES);
    assert(route_opens==3 && route_probes==2); /* Other errors do not trigger any probe. */
    route_case = 4;
    uint64_t queued = queue_calls, controlled = control_calls;
    assert(!lpr_filed_open_handle_at(LPR_LINUX_AT_FDCWD,"/regular",0,0,&handle,&kind));
    assert(handle == 432 && kind == 0);
    assert(queue_calls == queued + 1 && control_calls == controlled);
    /* Missing metadata is malformed, not permission to guess the inode
     * type or issue the old extra FSTAT. Retire the newly opened handle. */
    route_case = 5;
    queued = queue_calls;
    assert(lpr_filed_open_handle_at(LPR_LINUX_AT_FDCWD,"/regular",0,0,&handle,&kind)==-LPR_LINUX_EIO);
    assert(!handle && queue_calls == queued + 2 && control_calls == controlled);
    route_case = 0;
    uint8_t statx[KB2_FILESYSTEM_STATX_SIZE]={0};
    statx[KB2_FILESYSTEM_STATX_MODE_OFFSET+1]=0x20;
    statx[KB2_FILESYSTEM_STATX_RDEV_MAJOR_OFFSET]=226;
    statx[KB2_FILESYSTEM_STATX_RDEV_MINOR_OFFSET]=128;
    expected_service_path="/dev/dri/renderD128";
    assert(lpr_filed_device_open_route(FILED_SERVICE_GPUD_DRM,statx,service_flags)==77);
    statx[KB2_FILESYSTEM_STATX_RDEV_MINOR_OFFSET]=0;
    expected_service_path="/dev/dri/card0";
    assert(lpr_filed_device_open_route(FILED_SERVICE_GPUD_DRM,statx,service_flags)==77);
    statx[KB2_FILESYSTEM_STATX_RDEV_MAJOR_OFFSET]=13;
    statx[KB2_FILESYSTEM_STATX_RDEV_MINOR_OFFSET]=95;
    expected_service_path="/dev/input/event31";
    assert(lpr_filed_device_open_route(FILED_SERVICE_INPUTD_INPUT,statx,service_flags)==77);
    uint32_t large_minor=100064;
    for(unsigned i=0;i<4;++i) statx[KB2_FILESYSTEM_STATX_RDEV_MINOR_OFFSET+i]=(uint8_t)(large_minor>>(8*i));
    expected_service_path="/dev/input/event100000";
    assert(lpr_filed_device_open_route(FILED_SERVICE_INPUTD_INPUT,statx,service_flags)==77);
    memset(statx+KB2_FILESYSTEM_STATX_RDEV_MINOR_OFFSET,0,4);
    statx[KB2_FILESYSTEM_STATX_RDEV_MAJOR_OFFSET]=5;
    statx[KB2_FILESYSTEM_STATX_RDEV_MINOR_OFFSET]=2;
    expected_service_path="/dev/ptmx";
    assert(lpr_filed_device_open_route(FILED_SERVICE_TERMD_TTY,statx,service_flags)==77);
    const unsigned before=service_opens;
    assert(lpr_filed_device_open_route(FILED_SERVICE_GPUD_DRM,statx,service_flags)==-LPR_LINUX_ENODEV);
    statx[KB2_FILESYSTEM_STATX_MODE_OFFSET+1]=0x80;
    assert(lpr_filed_device_open_route(FILED_SERVICE_TERMD_TTY,statx,service_flags)==-LPR_LINUX_ENODEV);
    assert(service_opens==before);
}

int main(void)
{
    lpr_linux_current_pid = 1;
    unsigned char output[20000];
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_READ, .handle = 42,
        .length = 17, .client = UINT64_MAX, .credential_generation = UINT64_MAX};
    kb2_fs_response_t response;
    assert(lpr_fs_call(&request, NULL, NULL, NULL, 0, output, 17, &response, -1, NULL) == 17);
    assert(queue_calls == 1 && control_calls == 0 && response.generation == 7);
    for (unsigned i = 0; i < 17; ++i) assert(output[i] == (unsigned char)(i * 13u));
    request.length = sizeof(output);
    assert(lpr_fs_call(&request, NULL, NULL, NULL, 0, output, sizeof(output), &response, -1, NULL) == sizeof(output));
    assert(queue_calls == 2 && control_calls == 0);
    unsigned char input[20000];
    for (unsigned i = 0; i < sizeof(input); ++i) input[i] = (unsigned char)(i * 19u);
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_WRITE, .handle = 42, .length = sizeof(input)};
    assert(lpr_fs_call(&request, NULL, NULL, input, sizeof(input), NULL, 0, &response, -1, NULL) == sizeof(input));
    assert(queue_calls == 3 && control_calls == 0);
    char path[4096]; memset(path, 'p', sizeof(path)); path[4095] = 0;
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_HELLO};
    assert(!lpr_fs_call(&request, path, NULL, NULL, 0, NULL, 0, &response, -1, NULL));
    assert(request.path.length == 4096);
    path[4095] = 'p';
    assert(lpr_fs_call(&request, path, NULL, NULL, 0, NULL, 0, &response, -1, NULL) == -LPR_LINUX_ENAMETOOLONG);
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_TRANSFER_DUP, .handle = 42};
    assert(!lpr_fs_call(&request, NULL, NULL, NULL, 0, NULL, 0, &response, 77, NULL));
    assert(response.handle == 999);
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_FILE_VMO, .handle = 42, .length = 123456};
    pacha_fd_t cap = -1; return_capability = 1;
    assert(lpr_fs_call(&request, NULL, NULL, NULL, 0, NULL, 0, &response, -1, &cap) == 123456);
    assert(cap == 42 && response.result == 123456 && !response.handle);
    return_capability = 0;
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_HELLO};
    injected = -13;
    assert(lpr_fs_call(&request, NULL, NULL, NULL, 0, NULL, 0, &response, -1, NULL) == -13);
    injected = 0;
    for (malformed = 1; malformed <= 3; ++malformed)
        assert(lpr_fs_call(&request, NULL, NULL, NULL, 0, NULL, 0, &response, -1, NULL) == -LPR_LINUX_EIO);
    malformed = 0;
    test_operation_operands();
    test_device_routes();
    test_binding_before_handle_capture();
    terminal = 1;
    const uint64_t calls_before = queue_calls + control_calls;
    const unsigned binds_before = endpoint_calls;
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_HELLO};
    assert(lpr_fs_call(&request, NULL, NULL, NULL, 0, NULL, 0, &response, -1, NULL) == -LPR_LINUX_EIO);
    assert(queue_calls + control_calls == calls_before && endpoint_calls == binds_before);
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_SESSION_DETACH,
        .generation = 7, .second_handle = 2};
    assert(!lpr_fs_call(&request, NULL, NULL, NULL, 0, NULL, 0, &response, -1, NULL));
    assert(response.handle == 2 && endpoint_calls == binds_before && control_calls + queue_calls == calls_before + 1);
    lpr_filed_endpoint_checked = -1;
    assert(lpr_fs_call(&request, NULL, NULL, NULL, 0, NULL, 0, &response, -1, NULL) == -LPR_LINUX_EIO);
    assert(endpoint_calls == binds_before);
    lpr_filed_endpoint_checked = 1; terminal = 0;
#if defined(LPR_BOOT_DIAGNOSTICS) && LPR_BOOT_DIAGNOSTICS
    assert(log_calls > 0 && log_calls < LPR_BOOT_DIAGNOSTIC_RECORDS);
    assert(lpr_boot_diagnostic_state.fs_completed == 2);
    assert(!lpr_boot_diagnostic_fs_active());
    const unsigned observed = log_calls;
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_READ, .handle = 42, .length = 17};
    assert(lpr_fs_call(&request, NULL, NULL, NULL, 0, output, 17, &response, -1, NULL) == 17);
    assert(log_calls == observed); /* Later regular operations are not traced. */
    for (int sign = -1; sign <= 1; sign += 2) {
        memset(&lpr_boot_diagnostic_state, 0, sizeof(lpr_boot_diagnostic_state));
        log_return = sign * 19;
        const unsigned before = log_calls;
        assert(lpr_fs_call(&request, NULL, NULL, NULL, 0, output, 17, &response, -1, NULL) == 17);
        assert(log_calls == before + 1 && lpr_boot_diagnostic_state.failed);
        assert(lpr_boot_diagnostic_state.last_log_status == log_return);
    }
#else
    assert(!log_calls);
#endif
    assert(!mapping && unmaps == control_calls); /* DATA never allocates a staging VMO. */
    assert(closes == control_calls);
    puts("LPR_FS_RPC_UNIT_PASS");
}

/* Request preparation and reply rendering are fixture phases within one
 * synchronous syscall. Their transaction tags never become caller FDs. */
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b)
{
    assert(nr != PACHAOS_SYSCALL_IPC_CALL);
    return fixture_prepare_request(nr, a, b);
}
int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    return fixture_finish_request(nr, a, b, c, d);
}
int64_t lpr_ipc_message(uint64_t nr, uint64_t endpoint,
    uint64_t request, uint64_t reply, uint64_t timeout, uint64_t flags, int *sent)
{
    if (nr != PACHA_IPC_SYSCALL_CALL) return test_lpr_ipc_noncall(nr, endpoint, request, reply, timeout, flags, sent);

    assert(nr == PACHAOS_SYSCALL_IPC_CALL && request && reply);
    assert(timeout == UINT64_MAX && flags == PACHA_WAIT_UNINTERRUPTIBLE);
    *sent = 0;
    int64_t transaction = fixture_prepare_request(nr, endpoint, request);
    if (transaction < 16) return transaction;
    *sent = 1;
    int64_t status = fixture_finish_request(PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)transaction, reply, timeout, flags);

    return status;
}
