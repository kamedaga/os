#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <pacha/abi.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"
#include "../userland/netd/src/unix_socket.h"

enum {
    TEST_AF_UNIX = 1u,
    TEST_SOCK_STREAM = 1u,
    TEST_WAIT_SOURCE_FD = 50,
    TEST_WAIT_DUP_FD = 60,
    TEST_LEASE_FD = 70,
    TEST_REMOTE_LEASE_FD = 71,
    TEST_IMPORTED_FD = 23,
    TEST_TTY_HANDLE = 0x3344,
};

lpr_state_t lpr_state;

static int failures;
static uint64_t duplicated_handle;
static int duplicated_lease_fd;
static unsigned duplicate_handle_calls;
static lpr_socket_backend_t imported_socket;
static lpr_tty_backend_t imported_tty;
static lpr_fd_install_t imported_install;
static unsigned notifications[128];

static void expect(int condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

void *lpr_memset(void *dst, int c, size_t n)
{
    return memset(dst, c, n);
}

int lpr_native_fd_info(uint64_t fd, struct pacha_fd_info *out)
{
    if (fd != TEST_WAIT_SOURCE_FD || out == NULL) return 0;
    memset(out, 0, sizeof(*out));
    out->rights = PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER;
    return 1;
}

int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t a0)
{
    (void)nr;
    (void)a0;
    return 0;
}

int64_t lpr_pacha_syscall3(
    uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2)
{
    (void)a1;
    (void)a2;
    if (nr != PACHAOS_SYSCALL_IPC_CHANNEL_CREATE) return 1;
    uint64_t *pair = (uint64_t *)(uintptr_t)a0;
    pair[0] = TEST_LEASE_FD;
    pair[1] = TEST_REMOTE_LEASE_FD;
    return 0;
}

int64_t lpr_pacha_syscall4(
    uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    (void)a1;
    (void)a2;
    (void)a3;
    if (nr == PACHA_FD_SYSCALL_FCNTL && a0 == TEST_WAIT_SOURCE_FD)
        return TEST_WAIT_DUP_FD;
    return 1;
}

int64_t lpr_close_native_fd_if_open(uint64_t fd)
{
    (void)fd;
    return 0;
}

int64_t lpr_pacha_status_to_errno(int64_t status)
{
    return pacha_kernel_status_to_errno(status);
}

int64_t lpr_netd_transfer_dup_handle(uint64_t handle, int lease_fd)
{
    duplicated_handle = handle;
    duplicated_lease_fd = lease_fd;
    duplicate_handle_calls++;
    return netd_unix_socket_dup(handle);
}

int pacha_ipc_send(int fd, const struct pacha_ipc_msg *message)
{
    (void)message;
    if (fd >= 0 && (unsigned)fd < sizeof(notifications) /
            sizeof(notifications[0]))
        notifications[fd]++;
    return 0;
}

int pacha_fd_close(int fd)
{
    (void)fd;
    return 0;
}

int pacha_service_wait_add(
    struct pacha_service_wait_set *set, int fd, uint32_t events)
{
    (void)set;
    (void)fd;
    (void)events;
    return 0;
}

uint64_t pacha_service_wait_revents(
    const struct pacha_service_wait_set *set, int fd)
{
    (void)set;
    (void)fd;
    return 0;
}

int64_t lpr_filed_transfer_dup_handle(
    uint64_t handle, uint64_t flags, int lease_fd, uint64_t *out_handle)
{
    (void)handle;
    (void)flags;
    (void)lease_fd;
    (void)out_handle;
    return -LPR_LINUX_EOPNOTSUPP;
}

int64_t lpr_input_transfer_dup_handle(
    uint64_t handle, int lease_fd, uint64_t *out_handle)
{
    (void)handle;
    (void)lease_fd;
    (void)out_handle;
    return -LPR_LINUX_EOPNOTSUPP;
}

int64_t lpr_drm_transfer_dup_handle(
    uint64_t handle, int lease_fd, uint64_t *out_handle)
{
    (void)handle;
    (void)lease_fd;
    (void)out_handle;
    return -LPR_LINUX_EOPNOTSUPP;
}

int64_t lpr_termd_transfer_dup_handle(
    uint64_t handle, int lease_fd, uint64_t *out_handle)
{
    if (handle != TEST_TTY_HANDLE || lease_fd != TEST_REMOTE_LEASE_FD ||
        out_handle == NULL)
        return -LPR_LINUX_EINVAL;
    *out_handle = handle;
    return 0;
}

uint64_t lpr_backend_state_bytes_for_ops(uint8_t ops_id)
{
    if (ops_id == LPR_FD_OPS_SOCKET) return sizeof(imported_socket);
    if (ops_id == LPR_FD_OPS_TTY) return sizeof(imported_tty);
    return 0;
}

void *lpr_backend_state_alloc(uint64_t state_bytes)
{
    if (state_bytes == sizeof(imported_socket)) {
        memset(&imported_socket, 0, sizeof(imported_socket));
        return &imported_socket;
    }
    if (state_bytes == sizeof(imported_tty)) {
        memset(&imported_tty, 0, sizeof(imported_tty));
        return &imported_tty;
    }
    return NULL;
}

int64_t lpr_backend_state_free(void *state, uint64_t state_bytes)
{
    (void)state;
    (void)state_bytes;
    return 0;
}

uint16_t lpr_control_fd_flags_from_linux(uint64_t flags)
{
    return (flags & LPR_LINUX_O_CLOEXEC) != 0 ? LPR_FD_ENTRY_CLOEXEC : 0;
}

uint32_t lpr_control_status_flags_from_linux(uint64_t flags)
{
    return (flags & LPR_LINUX_O_NONBLOCK) != 0 ? LPR_OFD_NONBLOCK : 0;
}

int lpr_fd_table_alloc_batch(
    lpr_fd_table_t *table,
    lpr_linux_fd_t min_fd,
    const lpr_fd_install_t *installs,
    uint32_t install_count,
    const lpr_linux_fd_t *excluded_fds,
    uint32_t excluded_count,
    lpr_linux_fd_t *out_fds)
{
    (void)table;
    (void)min_fd;
    (void)excluded_fds;
    (void)excluded_count;
    if (installs == NULL || install_count != 1 || out_fds == NULL) return -1;
    imported_install = installs[0];
    out_fds[0] = TEST_IMPORTED_FD;
    return 0;
}

int lpr_fd_table_ensure_capacity(uint64_t required_capacity)
{
    (void)required_capacity;
    return -1;
}

int main(void)
{
    lpr_tty_backend_t console_tty;
    memset(&console_tty, 0, sizeof(console_tty));
    console_tty.active = 1;
    console_tty.flags = LPR_LINUX_O_RDWR | LPR_LINUX_O_CLOEXEC;
    console_tty.handle = TEST_TTY_HANDLE;
    console_tty.wait_fd.raw = TEST_WAIT_SOURCE_FD;
    const lpr_fd_pin_t tty_pin = {
        .effective_rights = LPR_FD_RIGHT_READ | LPR_FD_RIGHT_WRITE |
            LPR_FD_RIGHT_IOCTL | LPR_FD_RIGHT_DUP,
        .ops_id = LPR_FD_OPS_TTY,
        .state = &console_tty,
    };
    netd_transfer_occurrence_t tty_item;
    int tty_capabilities[2] = {-1, -1};
    uint32_t tty_capability_count = 0;
    expect(lpr_fd_transfer_prepare(
               &tty_pin, &tty_item, tty_capabilities, 2,
               &tty_capability_count) == 0,
           "console TTY is transferable for GApplication stdin");
    expect(tty_item.provider_id == LPR_FD_OPS_TTY &&
               tty_item.transfer_token == TEST_TTY_HANDLE &&
               tty_item.capability_count == 2,
           "TTY transfer preserves provider and termd handle");
    expect(tty_capability_count == 2 &&
               tty_capabilities[0] == TEST_WAIT_DUP_FD &&
               tty_capabilities[1] == TEST_LEASE_FD,
           "TTY transfer carries wait and lifetime capabilities");
    int imported_tty_fd = -1;
    expect(lpr_fd_transfer_import_batch(
               &tty_item, 1, tty_capabilities, tty_capability_count,
               LPR_LINUX_O_CLOEXEC, &imported_tty_fd) == 0,
           "received console TTY transfer imports atomically");
    expect(imported_tty_fd == TEST_IMPORTED_FD &&
               imported_install.ops_id == LPR_FD_OPS_TTY &&
               imported_tty.active &&
               imported_tty.handle == TEST_TTY_HANDLE,
           "TTY import installs the original termd handle");
    expect(imported_tty.wait_fd.raw == TEST_WAIT_DUP_FD &&
               imported_tty.lease_fd.raw == TEST_LEASE_FD &&
               (imported_tty.reserved1 & LPR_BACKEND_TRANSFER_LEASE) != 0,
           "TTY import owns readiness and termd lifetime leases");
    console_tty.reserved0 = LPR_TTY_BACKEND_PTY_MASTER;
    expect(lpr_fd_transfer_prepare(
               &tty_pin, &tty_item, tty_capabilities, 2,
               &tty_capability_count) == -LPR_LINUX_EOPNOTSUPP,
           "PTY roles are rejected until their role metadata is transferable");

    uint64_t data_pair[2] = {0, 0};
    expect(netd_unix_socket_pair(
               NETD_SOCK_STREAM, 0, 80, 81, data_pair) == 0,
           "create Glycin image stream pair");
    const uint64_t handle = data_pair[0];
    lpr_socket_backend_t source;
    memset(&source, 0, sizeof(source));
    source.active = 1;
    source.type = TEST_SOCK_STREAM;
    source.connected = 1;
    source.domain = TEST_AF_UNIX;
    source.flags = LPR_LINUX_O_RDWR | LPR_LINUX_O_CLOEXEC;
    source.handle = handle;
    source.wait_fd.raw = TEST_WAIT_SOURCE_FD;

    const lpr_fd_pin_t pin = {
        .effective_rights = LPR_FD_RIGHT_READ | LPR_FD_RIGHT_WRITE |
            LPR_FD_RIGHT_DUP,
        .ops_id = LPR_FD_OPS_SOCKET,
        .state = &source,
    };
    netd_transfer_occurrence_t item;
    int capabilities[2] = {-1, -1};
    uint32_t capability_count = 0;
    expect(lpr_fd_transfer_prepare(
               &pin, &item, capabilities, 2, &capability_count) == 0,
           "connected Unix stream is transferable through SCM_RIGHTS");
    expect(item.provider_id == LPR_FD_OPS_SOCKET &&
               item.transfer_token == handle && item.capability_count == 2,
           "socket transfer preserves provider and netd handle");
    expect(capability_count == 2 &&
               capabilities[0] == TEST_WAIT_DUP_FD &&
               capabilities[1] == TEST_LEASE_FD,
           "socket transfer carries wait and lifetime capabilities");
    expect(duplicate_handle_calls == 1 && duplicated_handle == handle &&
               duplicated_lease_fd == TEST_REMOTE_LEASE_FD,
           "netd retains the endpoint until the imported lease closes");
    expect((item.fd_flags & LPR_LINUX_O_CLOEXEC) == 0 &&
               (item.fd_flags & LPR_LINUX_O_NONBLOCK) == 0,
           "SCM_RIGHTS clears descriptor CLOEXEC and preserves blocking state");

    int imported_fd = -1;
    expect(lpr_fd_transfer_import_batch(
               &item, 1, capabilities, capability_count,
               LPR_LINUX_O_CLOEXEC, &imported_fd) == 0,
           "received Unix stream transfer imports atomically");
    expect(imported_fd == TEST_IMPORTED_FD &&
               imported_install.ops_id == LPR_FD_OPS_SOCKET,
           "import installs a socket descriptor");
    expect(imported_socket.active && imported_socket.connected &&
               imported_socket.domain == TEST_AF_UNIX &&
               imported_socket.type == TEST_SOCK_STREAM &&
               imported_socket.handle == handle,
           "import reconstructs the connected Unix stream endpoint");
    expect(imported_socket.wait_fd.raw == TEST_WAIT_DUP_FD &&
               imported_socket.lease_fd.raw == TEST_LEASE_FD &&
               (imported_socket.reserved1 & LPR_BACKEND_TRANSFER_LEASE) != 0,
           "import owns readiness and netd lifetime leases");
    expect((imported_socket.flags & LPR_LINUX_O_NONBLOCK) == 0 &&
               (imported_socket.flags & LPR_LINUX_O_CLOEXEC) != 0,
           "MSG_CMSG_CLOEXEC is applied without changing blocking state");

    /* The normal Glycin transfer then writes an 857,863-byte PNG in 65,535
     * byte sendto calls.  Exercise the same traffic through netd's smaller
     * receive window and verify that the transferred endpoint remains live
     * until EOF. */
    expect(netd_unix_socket_close(handle) == 0,
           "drop sender's original reference after SCM transfer");
    enum { IMAGE_BYTES = 857863 };
    static uint8_t image[IMAGE_BYTES];
    static uint8_t received_image[IMAGE_BYTES];
    for (size_t i = 0; i < IMAGE_BYTES; ++i)
        image[i] = (uint8_t)(i * 29u + 7u);
    size_t written = 0;
    size_t received_total = 0;
    while (written < IMAGE_BYTES) {
        netd_io_t send_req;
        memset(&send_req, 0, offsetof(netd_io_t, data));
        send_req.handle = data_pair[1];
        size_t request = IMAGE_BYTES - written;
        if (request > NETD_IO_BYTES) request = NETD_IO_BYTES;
        memcpy(send_req.data, image + written, request);
        send_req.length = request;
        size_t sent = 0;
        const int send_status = netd_unix_socket_send(
            &send_req, NULL, 0, &sent);
        if (send_status == 0) {
            expect(sent != 0, "image stream send makes progress");
            written += sent;
        } else {
            expect(send_status == -LPR_LINUX_EAGAIN,
                   "full image stream applies EAGAIN backpressure");
        }

        netd_io_t recv_req;
        memset(&recv_req, 0, offsetof(netd_io_t, data));
        recv_req.handle = imported_socket.handle;
        size_t got = 0;
        uint32_t got_caps = 0;
        const int recv_status = netd_unix_socket_recv(
            &recv_req, NETD_IO_BYTES, NULL, 0, &got_caps, &got);
        expect(recv_status == 0 && got != 0,
               "transferred endpoint drains image bytes");
        if (recv_status == 0 && got != 0) {
            memcpy(received_image + received_total, recv_req.data, got);
            received_total += got;
        }
    }
    while (received_total < IMAGE_BYTES) {
        netd_io_t recv_req;
        memset(&recv_req, 0, offsetof(netd_io_t, data));
        recv_req.handle = imported_socket.handle;
        size_t got = 0;
        uint32_t got_caps = 0;
        expect(netd_unix_socket_recv(
                   &recv_req, NETD_IO_BYTES, NULL, 0,
                   &got_caps, &got) == 0 && got != 0,
               "drain final image bytes");
        memcpy(received_image + received_total, recv_req.data, got);
        received_total += got;
    }
    expect(received_total == IMAGE_BYTES &&
               memcmp(image, received_image, IMAGE_BYTES) == 0,
           "857,863-byte Glycin image stream is byte exact");
    expect(netd_unix_socket_close(data_pair[1]) == 0,
           "image producer closes after final byte");
    netd_io_t eof_req;
    memset(&eof_req, 0, offsetof(netd_io_t, data));
    eof_req.handle = imported_socket.handle;
    size_t eof_received = 0;
    uint32_t eof_caps = 0;
    expect(netd_unix_socket_recv(
               &eof_req, 1, NULL, 0, &eof_caps, &eof_received) == 0 &&
               eof_received == 0,
           "transferred endpoint observes EOF after image payload");
    expect(netd_unix_socket_close(imported_socket.handle) == 0,
           "netd lease reap releases transferred endpoint");

    source.type = 5;
    expect(lpr_fd_transfer_prepare(
               &pin, &item, capabilities, 2, &capability_count) ==
               -LPR_LINUX_EOPNOTSUPP,
           "unsupported socket kinds are rejected instead of misrepresented");

    if (failures != 0) return 1;
    puts("lpr linux socket transfer unit: PASS");
    return 0;
}
