/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "status_file.h"

#include "dhcp.h"
#include "kobox2_nic.h"
#include "link.h"
#include "netd/boot_config.h"
#include "filed_client/client.h"
#include "pacha/capsule.h"
#include "pacha/ipc.h"
#include "pacha/log.h"

#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
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

/* Native netd uses its granted filed endpoint; personality libc is not
 * present here. Request VMOs grow to the actual payload, not an inline cap. */
static int status_file_call(int endpoint, kb2_fs_request_t *request,
    const char *path, const void *data, size_t bytes, void *output, size_t capacity,
    kb2_fs_response_t *response)
{
    int status = filedfs_call(endpoint, request, path, NULL, data, bytes,
        output, capacity, response, NULL, 0, NULL, NULL);
    return status ? status : response->result < 0 ? (int)response->result : 0;
}
static int status_file_write(int endpoint, const char *path,
    const char *text, size_t length, uint64_t flags)
{
    if (endpoint < 16 || !path || (!text && length)) return -EINVAL;
    const int policy_write = strcmp(path, NETD_POLICY_PATH) == 0;
    if (policy_write && memchr(text, 0, length)) return -EINVAL;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_OPENAT2,
        .flags = flags | O_WRONLY | O_CLOEXEC, .mode = 0644};
    kb2_fs_response_t response;
    int result = status_file_call(endpoint, &request, path, NULL, 0, NULL, 0, &response);
    if (result) return result;
    const uint64_t handle = response.handle;
    for (size_t offset = 0; !result && offset < length;) {
        request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_PWRITE,
            .handle = handle, .offset = (int64_t)offset, .length = length - offset};
        result = status_file_call(endpoint, &request, NULL, text + offset,
            length - offset, NULL, 0, &response);
        if (!result && (response.result <= 0 ||
            (uint64_t)response.result > length - offset)) result = -EIO;
        if (!result) offset += (size_t)response.result;
    }
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_CLOSE, .handle = handle};
    const int closed = status_file_call(endpoint, &request, NULL, NULL, 0,
        NULL, 0, &response);
    if (!result) result = closed;
    if (policy_write && result) {
        /* A failed first exclusive write must not leave an unusable policy
         * that prevents the next DHCP ACK from committing. */
        request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_UNLINKAT};
        int cleanup = status_file_call(endpoint, &request, NETD_POLICY_PATH,
            NULL, 0, NULL, 0, &response);
        if (cleanup)
            fprintf(stderr, "[netd] incomplete DHCP policy cleanup failed status=%d\n",
                cleanup);
    }
    return result;
}
static int policy_dir_create(int endpoint)
{
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_MKDIRAT, .mode = 0755};
    kb2_fs_response_t response;
    int result = status_file_call(endpoint, &request, NETD_POLICY_DIR,
        NULL, 0, NULL, 0, &response);
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
        text, (size_t)length, O_CREAT | O_EXCL);
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
        text, (size_t)length, O_CREAT | O_EXCL);
}

int netd_policy_file_read(int endpoint, char *text,
    size_t capacity, size_t *out_length)
{
    if (endpoint < 16 || !text || !out_length || capacity < 2) return -EINVAL;
    *out_length = 0;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_OPENAT2,
        .flags = O_RDONLY | O_CLOEXEC};
    kb2_fs_response_t response;
    int result = status_file_call(endpoint, &request, NETD_POLICY_PATH,
        NULL, 0, NULL, 0, &response);
    if (result) return result;
    const uint64_t handle = response.handle;
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_PREAD,
        .handle = handle, .length = capacity};
    result = status_file_call(endpoint, &request, NULL, NULL, 0,
        text, capacity, &response);
    if (!result) {
        if (response.result <= 0 || (uint64_t)response.result >= capacity)
            result = response.result == 0 ? -EINVAL : -E2BIG;
        else if (response.data_length != (uint64_t)response.result ||
            memchr(text, 0, (size_t)response.result))
            result = -EPROTO;
        else {
            text[response.result] = 0;
            *out_length = (size_t)response.result;
        }
    }
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_CLOSE, .handle = handle};
    int closed = status_file_call(endpoint, &request, NULL, NULL, 0,
        NULL, 0, &response);
    return result ? result : closed;
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
        O_CREAT | O_TRUNC);
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
    if (cfg->log_endpoint_fd < 16 ||
        pacha_log_emit((int)cfg->log_endpoint_fd,
            PACHA_LOG_SOURCE_NETD, phase, strlen(phase)) != 0) {
        printf("[netd] progress #%u %s\n", progress_sequence, phase);
        fflush(stdout);
    }
    netd_status_file_publish(cfg, last_state, last_stage, last_status);
}

static void status_file_progress(const struct netd_boot_config *cfg,
    const char *phase, unsigned source)
{
    if (!cfg || !phase) return;
    (void)snprintf(current_phase, sizeof(current_phase), "%s", phase);
    (void)snprintf(progress_log[progress_sequence % NETD_STATUS_LOG_LINES],
        NETD_STATUS_LOG_LINE_BYTES, "#%u %s", progress_sequence + 1, phase);
    ++progress_sequence;
    if (progress_count < NETD_STATUS_LOG_LINES) ++progress_count;
    if (cfg->log_endpoint_fd < 16 ||
        pacha_log_emit((int)cfg->log_endpoint_fd,
            source, phase, strlen(phase)) != 0) {
        printf("[netd] nic progress #%u %s\n", progress_sequence, phase);
        fflush(stdout);
    }
    netd_status_file_publish(cfg, "initializing", NETD_BOOT_STAGE_NIC, 0);
}

void netd_status_file_progress(const struct netd_boot_config *cfg,
    const char *phase)
{
    status_file_progress(cfg, phase, PACHA_LOG_SOURCE_NETD);
}

void netd_status_file_kobox_progress(const struct netd_boot_config *cfg,
    const char *phase)
{
    status_file_progress(cfg, phase, PACHA_LOG_SOURCE_KOBOX2_NET);
}
