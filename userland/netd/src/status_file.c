/* SPDX-License-Identifier: MIT */
#include "status_file.h"

#include "dhcp.h"
#include "kobox2_nic.h"
#include "link.h"
#include "netd/boot_config.h"
#include "filed/flags.h"
#include "filed/ipc_protocol.h"
#include "filed/payload.h"
#include "pacha/capsule.h"
#include "pacha/ipc.h"

#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define NETD_STATUS_PATH "/tmp/netd-status"
#define NETD_POLICY_DIR "/run/pacha"
#define NETD_POLICY_PATH "/run/pacha/network.conf"
#define NETD_RESOLVER_PATH "/etc/resolv.conf"

enum { NETD_STATUS_LOG_LINES = 40, NETD_STATUS_LOG_LINE_BYTES = 80 };
static char progress_log[NETD_STATUS_LOG_LINES][NETD_STATUS_LOG_LINE_BYTES];
static char current_phase[NETD_STATUS_LOG_LINE_BYTES];
static unsigned progress_sequence;
static unsigned progress_count;
static const struct netd_dhcp_client *dhcp_client;
static char last_state[24] = "initializing";
static uint64_t last_stage = NETD_BOOT_STAGE_NIC;
static int last_status;

/* netd is a native service: its ordinary libc file calls do not go through
 * the Linux personality. Use the filed endpoint it was already granted. */
static int status_file_call(int endpoint_fd, int page_fd, void *page,
    uint32_t operation, uint64_t payload_size, uint64_t *out_result)
{
    static uint64_t request_id;
    pacha_service_envelope_t *header = page;
    const uint64_t id = ++request_id;
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = FILED_SERVICE_ID;
    header->op = operation;
    header->flags = PACHA_SERVICE_FLAG_PAGE_PAYLOAD;
    header->request_id = id;
    header->trace_id = id;
    header->payload_size = payload_size;
    header->fd_count = 0;

    struct pacha_ipc_fd transfer = {
        .fd = (uint64_t)(uint32_t)page_fd,
        .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE,
    };
    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word3 = id,
        .fds = &transfer,
        .fd_count = 1,
    };
    const int reply_fd = pacha_ipc_call(endpoint_fd, &request);
    if (reply_fd < 16) return reply_fd;
    struct pacha_ipc_msg reply = {0};
    const int received = pacha_ipc_recv_wait(reply_fd, &reply,
        PACHA_FD_WAIT_FOREVER);
    (void)pacha_fd_close(reply_fd);
    if (received != 0) return received;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC ||
        header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        header->service_id != FILED_SERVICE_ID || header->op != operation ||
        header->request_id != id || header->status != 0)
        return header->status ? (int)header->status : -5;
    if (out_result) *out_result = header->result;
    return 0;
}

static int status_file_write(int endpoint_fd, const char *path,
    const char *text, size_t length, uint64_t flags)
{
    if (endpoint_fd < 16 || path == NULL ||
        strlen(path) >= FILED_PATH_BYTES || length > FILED_IO_BYTES)
        return -EINVAL;
    const int policy_write = strcmp(path, NETD_POLICY_PATH) == 0;
    if (policy_write && memchr(text, 0, length) != NULL)
        return -EINVAL;
    const uint64_t rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    const int page_fd = pacha_vmo_create(FILED_PAGE_BYTES, rights, 0);
    if (page_fd < 16) return page_fd;
    void *page = pacha_mmap(page_fd, FILED_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (!page) {
        (void)pacha_fd_close(page_fd);
        return -5;
    }
    memset(page, 0, FILED_PAGE_BYTES);
    filed_path_request_t *open = (void *)((unsigned char *)page +
        PACHA_SERVICE_HEADER_BYTES);
    open->dir_handle = 0;
    open->rights = FILED_RIGHT_WRITE | FILED_RIGHT_STAT;
    open->flags = flags | FILED_OPEN_CLOEXEC;
    (void)snprintf(open->path, sizeof(open->path), "%s", path);
    uint64_t handle = 0;
    int result = status_file_call(endpoint_fd, page_fd, page,
        FILED_OP_VFS_OPENAT, sizeof(*open), &handle);
    const int created_policy = policy_write && result == 0;
    if (result == 0) {
        memset(page, 0, FILED_PAGE_BYTES);
        /* Filed's page operations consume filed_io_t (24-byte prefix), not
         * the older 32-byte filed_io_request_t transport declaration. */
        filed_io_t *io = (void *)((unsigned char *)page +
            PACHA_SERVICE_HEADER_BYTES);
        io->handle = handle;
        io->length = length;
        memcpy(io->data, text, length);
        if (policy_write && memcmp(io->data, text, length) != 0)
            result = -EIO;
        uint64_t written = 0;
        if (result == 0)
            result = status_file_call(endpoint_fd, page_fd, page,
                FILED_OP_VFS_PWRITE, sizeof(*io), &written);
        if (result == 0 && written != length) result = -5;
        if (policy_write && result == 0 &&
            memcmp(io->data, text, length) != 0)
            fprintf(stderr,
                "[netd] policy payload changed during filed PWRITE\n");

        memset(page, 0, FILED_PAGE_BYTES);
        filed_handle_request_t *close = (void *)((unsigned char *)page +
            PACHA_SERVICE_HEADER_BYTES);
        close->handle = handle;
        const int closed = status_file_call(endpoint_fd, page_fd, page,
            FILED_OP_VFS_CLOSE, sizeof(*close), NULL);
        if (result == 0) result = closed;
    }
    if (created_policy && result != 0) {
        /* A failed first write must not leave an exclusive but unusable
         * network.conf that prevents the next DHCP ACK from committing. */
        memset(page, 0, FILED_PAGE_BYTES);
        filed_unlink_t *unlink = (void *)((uint8_t *)page +
            PACHA_SERVICE_HEADER_BYTES);
        memcpy(unlink->name, NETD_POLICY_PATH, sizeof(NETD_POLICY_PATH));
        const int cleanup = status_file_call(endpoint_fd, page_fd, page,
            FILED_OP_VFS_UNLINK, sizeof(*unlink), NULL);
        if (cleanup != 0)
            fprintf(stderr,
                "[netd] incomplete DHCP policy cleanup failed status=%d\n",
                cleanup);
    }
    (void)pacha_munmap(page, FILED_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return result;
}

static int policy_dir_create(int endpoint_fd)
{
    const uint64_t rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    int page_fd = pacha_vmo_create(FILED_PAGE_BYTES, rights, 0);
    if (page_fd < 16) return page_fd;
    void *page = pacha_mmap(page_fd, FILED_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (page == NULL) {
        (void)pacha_fd_close(page_fd);
        return -EIO;
    }
    memset(page, 0, FILED_PAGE_BYTES);
    filed_mkdir_t *mkdir = (void *)((uint8_t *)page +
        PACHA_SERVICE_HEADER_BYTES);
    mkdir->mode = 0755;
    memcpy(mkdir->name, NETD_POLICY_DIR, sizeof(NETD_POLICY_DIR));
    int result = status_file_call(endpoint_fd, page_fd, page,
        FILED_OP_VFS_MKDIR, sizeof(*mkdir), NULL);
    (void)pacha_munmap(page, FILED_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return result == -EEXIST ? 0 : result;
}

int netd_policy_file_create(int filed_endpoint_fd,
    const struct netd_dhcp_lease *lease)
{
    if (lease == NULL) return -EINVAL;
    int result = policy_dir_create(filed_endpoint_fd);
    if (result != 0) return result;
    char text[256];
    int length = snprintf(text, sizeof(text),
        "# generated from DHCP on this live boot\n"
        "address=%u.%u.%u.%u\nnetmask=%u.%u.%u.%u\n"
        "gateway=%u.%u.%u.%u\ndns=%u.%u.%u.%u\n",
        lease->address[0], lease->address[1], lease->address[2], lease->address[3],
        lease->netmask[0], lease->netmask[1], lease->netmask[2], lease->netmask[3],
        lease->gateway[0], lease->gateway[1], lease->gateway[2], lease->gateway[3],
        lease->dns[0], lease->dns[1], lease->dns[2], lease->dns[3]);
    if (length < 0 || (size_t)length >= sizeof(text)) return -EOVERFLOW;
    if (strlen(text) != (size_t)length) return -EIO;
    /* Exclusive creation keeps a manually supplied RAM policy authoritative. */
    return status_file_write(filed_endpoint_fd, NETD_POLICY_PATH,
        text, (size_t)length, FILED_OPEN_CREATE | FILED_OPEN_EXCLUSIVE);
}

int netd_resolver_file_create(int filed_endpoint_fd, const uint8_t dns[4])
{
    if (dns == NULL) return -EINVAL;
    if (!(dns[0] | dns[1] | dns[2] | dns[3])) return -ENODATA;
    char text[80];
    int length = snprintf(text, sizeof(text),
        "# generated from live network policy\n"
        "nameserver %u.%u.%u.%u\n",
        dns[0], dns[1], dns[2], dns[3]);
    if (length < 0 || (size_t)length >= sizeof(text)) return -EOVERFLOW;
    /* This is a RAM-root file. Exclusive creation preserves a resolver
     * policy supplied by the user before netd applies its IPv4 policy. */
    return status_file_write(filed_endpoint_fd, NETD_RESOLVER_PATH,
        text, (size_t)length, FILED_OPEN_CREATE | FILED_OPEN_EXCLUSIVE);
}

int netd_policy_file_read(int endpoint_fd, char *text,
    size_t capacity, size_t *out_length)
{
    if (endpoint_fd < 16 || text == NULL || out_length == NULL ||
        capacity < 2 || capacity > FILED_IO_BYTES)
        return -EINVAL;
    const uint64_t rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    const int page_fd = pacha_vmo_create(FILED_PAGE_BYTES, rights, 0);
    if (page_fd < 16) return page_fd;
    void *page = pacha_mmap(page_fd, FILED_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (page == NULL) {
        (void)pacha_fd_close(page_fd);
        return -EIO;
    }
    memset(page, 0, FILED_PAGE_BYTES);
    filed_path_request_t *open = (void *)((uint8_t *)page +
        PACHA_SERVICE_HEADER_BYTES);
    open->rights = FILED_RIGHT_READ | FILED_RIGHT_STAT;
    open->flags = FILED_OPEN_CLOEXEC;
    memcpy(open->path, NETD_POLICY_PATH, sizeof(NETD_POLICY_PATH));
    uint64_t handle = 0;
    int result = status_file_call(endpoint_fd, page_fd, page,
        FILED_OP_VFS_OPENAT, sizeof(*open), &handle);
    if (result == 0) {
        memset(page, 0, FILED_PAGE_BYTES);
        filed_io_t *io = (void *)((uint8_t *)page +
            PACHA_SERVICE_HEADER_BYTES);
        io->handle = handle;
        io->length = capacity;
        uint64_t read_count = 0;
        result = status_file_call(endpoint_fd, page_fd, page,
            FILED_OP_VFS_PREAD, sizeof(*io), &read_count);
        if (result == 0) {
            if (read_count == 0 || read_count >= capacity) {
                result = read_count == 0 ? -EINVAL : -E2BIG;
            } else {
                memcpy(text, io->data, (size_t)read_count);
                text[read_count] = '\0';
                size_t actual = strnlen(text, (size_t)read_count);
                if (actual != (size_t)read_count) {
                    fprintf(stderr,
                        "[netd] IPv4 policy contains NUL at %zu of %llu bytes\n",
                        actual, (unsigned long long)read_count);
                    result = -EPROTO;
                }
                *out_length = (size_t)read_count;
            }
        }
        memset(page, 0, FILED_PAGE_BYTES);
        filed_handle_request_t *close = (void *)((uint8_t *)page +
            PACHA_SERVICE_HEADER_BYTES);
        close->handle = handle;
        const int closed = status_file_call(endpoint_fd, page_fd, page,
            FILED_OP_VFS_CLOSE, sizeof(*close), NULL);
        if (result == 0) result = closed;
    }
    (void)pacha_munmap(page, FILED_PAGE_BYTES);
    (void)pacha_fd_close(page_fd);
    return result;
}

void netd_status_file_bind_dhcp(const struct netd_dhcp_client *client)
{
    dhcp_client = client;
}

void netd_status_file_publish(const struct netd_boot_config *cfg,
    const char *state, uint64_t stage, int status)
{
    if (!cfg || !state) return;
    if (state != last_state)
        (void)snprintf(last_state, sizeof(last_state), "%s", state);
    last_stage = stage;
    last_status = status;
    const struct netd_link_info *link = netd_link_current();
    const struct netd_nic_diagnostic *diagnostic =
        netd_kobox2_nic_diagnostic();
    const char *stage_name = stage == NETD_BOOT_STAGE_NIC ? "nic" :
        stage == NETD_BOOT_STAGE_SOCKET ? "socket" :
        stage == NETD_BOOT_STAGE_TIMER ? "timer" :
        stage == NETD_BOOT_STAGE_READY ? "ready" :
        stage == NETD_BOOT_STAGE_IPV4 ? "ipv4" :
        stage == NETD_BOOT_STAGE_LINK ? "link" : "unknown";
    char source[9] = {0};
    for (unsigned i = 0; i < 8; ++i)
        source[i] = (char)(diagnostic->source >> (i * 8));
    char report[4096];
    const int length = snprintf(report, sizeof(report), "log:\n");
    if (length < 0 || (size_t)length >= sizeof(report)) return;
    size_t used = (size_t)length;
    const unsigned oldest = progress_sequence - progress_count;
    for (unsigned sequence = oldest; sequence < progress_sequence; ++sequence) {
        const char *line = progress_log[sequence % NETD_STATUS_LOG_LINES];
        const int added = snprintf(report + used, sizeof(report) - used,
            "%s\n", line);
        if (added < 0 || (size_t)added >= sizeof(report) - used) return;
        used += (size_t)added;
    }
    const int summary = snprintf(report + used, sizeof(report) - used,
        "nic_step=%u\ndetail=%d\nloaded=%u\npci_bound=%u\n"
        "source=%s\nline=%u\nfault_vector=%u\nfault_error=0x%x\n"
        "fault_core_relative=%u\nfault_ip=0x%llx\nfault_address=0x%llx\n"
        "state=%s\nstage=%s\nphase=%s\nstatus=%d\ncarrier=%s\nmtu=%u\n",
        diagnostic->step, diagnostic->detail, diagnostic->loaded,
        diagnostic->pci_bound, source, diagnostic->line,
        diagnostic->fault_vector, diagnostic->fault_error_code,
        diagnostic->fault_core_relative,
        (unsigned long long)diagnostic->fault_ip,
        (unsigned long long)diagnostic->fault_address,
        state, stage_name, stage == NETD_BOOT_STAGE_NIC && current_phase[0] ?
            current_phase : stage_name, status,
        link && link->carrier ? "up" : "down", link ? link->mtu : 0);
    if (summary < 0 || (size_t)summary >= sizeof(report) - used) return;
    used += (size_t)summary;
    if (dhcp_client != NULL) {
        const struct netd_dhcp_client *d = dhcp_client;
        const int dhcp_summary = snprintf(report + used,
            sizeof(report) - used,
            "dhcp_state=%s\ndhcp_attempts=%u\ndhcp_offers=%u\n"
            "dhcp_acks=%u\ndhcp_rejected=%u\ndhcp_error=%d\n"
            "dhcp_address=%u.%u.%u.%u\ndhcp_server=%u.%u.%u.%u\n"
            "dhcp_lease_seconds=%u\n",
            netd_dhcp_state_name(d->state), d->attempts, d->offers,
            d->acks, d->rejected, d->last_error,
            d->lease.address[0], d->lease.address[1],
            d->lease.address[2], d->lease.address[3],
            d->lease.server[0], d->lease.server[1],
            d->lease.server[2], d->lease.server[3], d->lease.seconds);
        if (dhcp_summary < 0 ||
            (size_t)dhcp_summary >= sizeof(report) - used) return;
        used += (size_t)dhcp_summary;
    }
    const int written = status_file_write((int)cfg->filed_endpoint_fd,
        NETD_STATUS_PATH, report, used,
        FILED_OPEN_CREATE | FILED_OPEN_TRUNCATE);
    if (written != 0)
        fprintf(stderr, "[netd] status file update failed status=%d\n", written);
}

void netd_status_file_note(const struct netd_boot_config *cfg,
    const char *phase)
{
    if (!cfg || !phase) return;
    (void)snprintf(progress_log[progress_sequence % NETD_STATUS_LOG_LINES],
        NETD_STATUS_LOG_LINE_BYTES, "#%u %s", progress_sequence + 1, phase);
    ++progress_sequence;
    if (progress_count < NETD_STATUS_LOG_LINES) ++progress_count;
    printf("[netd] progress #%u %s\n", progress_sequence, phase);
    fflush(stdout);
    netd_status_file_publish(cfg, last_state, last_stage, last_status);
}

void netd_status_file_progress(const struct netd_boot_config *cfg,
    const char *phase)
{
    if (!cfg || !phase) return;
    (void)snprintf(current_phase, sizeof(current_phase), "%s", phase);
    (void)snprintf(progress_log[progress_sequence % NETD_STATUS_LOG_LINES],
        NETD_STATUS_LOG_LINE_BYTES, "#%u %s", progress_sequence + 1, phase);
    ++progress_sequence;
    if (progress_count < NETD_STATUS_LOG_LINES) ++progress_count;
    printf("[netd] nic progress #%u %s\n", progress_sequence, phase);
    fflush(stdout);
    netd_status_file_publish(cfg, "initializing", NETD_BOOT_STAGE_NIC, 0);
}
