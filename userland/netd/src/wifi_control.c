/* SPDX-License-Identifier: MIT */
#include "wifi_control.h"
#include "wifi_files.h"
#include "kobox2_nic.h"
#include "network_config.h"
#include "netd_internal.h"
#include "link.h"
#include "libuinet_backend.h"
#include "boot/wifi_control.h"
#include <pacha/syscall.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum operation { NONE, SCAN, CONNECT, REPLY };
static struct {
    int started, ipv4_ready, configuration_error, autoloaded;
    pacha_fd_t endpoint;
    uint64_t generation, deadline;
    enum operation pending;
    unsigned link_transition;
    int reply_error;
    char session[17], seen[33], pending_id[33];
    char status[8192], response[8256], reply_text[8192];
} wifi;

static void wipe(void *memory, size_t size)
{
    volatile unsigned char *bytes = memory;
    while (size--) *bytes++ = 0;
}

static int session_create(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    if (a < 7) return -EOPNOTSUPP;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    if (!(b & (1u << 18))) return -EOPNOTSUPP;
    for (unsigned attempt = 0; attempt < 1024; ++attempt) {
        uint64_t word;
        unsigned char ready;
        __asm__ volatile("rdseed %0; setc %1" : "=r"(word), "=qm"(ready) : : "cc");
        if (ready) {
            /* Instance correlation, not a password or a permission bypass. */
            (void)snprintf(wifi.session, sizeof(wifi.session), "%016llx", (unsigned long long)word);
            wipe(&word, sizeof(word));
            return 0;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    return -EAGAIN;
}

static int response(const char *id, int error, const char *text)
{
    int length = snprintf(wifi.response, sizeof(wifi.response), "%s %d\n%s", id, error, text ? text : "");
    if (length < 0 || (size_t)length >= sizeof(wifi.response)) return -EOVERFLOW;
    return netd_wifi_file_write(wifi.endpoint, "response", wifi.response, (size_t)length);
}

static void retry_response(void)
{
    if (wifi.pending == REPLY &&
        !response(wifi.pending_id, wifi.reply_error, wifi.reply_text)) {
        wifi.pending = NONE;
        wifi.reply_text[0] = 0;
    }
}

static void complete(int error, const char *text)
{
    wifi.pending = REPLY;
    wifi.reply_error = error;
    (void)snprintf(wifi.reply_text, sizeof(wifi.reply_text), "%s", text ? text : "");
    retry_response();
}

static int command(const char *text, char *reply, size_t capacity, size_t *length)
{
    if (capacity < 2) return -EINVAL;
    *length = 0;
    int error = netd_kobox2_wifi_command(text, strlen(text), reply, capacity - 1, length);
    if (!error) {
        if (*length >= capacity || memchr(reply, 0, *length)) return -EPROTO;
        reply[*length] = 0;
    }
    return error;
}

static int acknowledged(const char *text)
{
    char reply[64];
    size_t length;
    int error = command(text, reply, sizeof(reply), &length);
    if (error) return error;
    return length == 3 && !memcmp(reply, "OK\n", 3) ? 0 : -EIO;
}

static int apply_configuration(void)
{
    char configuration[8193];
    size_t length = 0;
    int error = netd_wifi_file_read(wifi.endpoint, NETD_WIFI_CONFIG,
        configuration, sizeof(configuration), &length);
    if (!error) error = netd_kobox2_wifi_configuration(configuration, length);
    wipe(configuration, sizeof(configuration));
    return error;
}

static int valid_id(const char *request)
{
    for (unsigned i = 0; i < 32; ++i)
        if (!((request[i] >= '0' && request[i] <= '9') ||
              (request[i] >= 'a' && request[i] <= 'f'))) return 0;
    return request[32] == ' ' && !memcmp(request, wifi.session, 16);
}

static unsigned receive_request(uint64_t now_ms)
{
    char request[96];
    size_t length;
    int error = netd_wifi_file_read(wifi.endpoint, NETD_WIFI_DIRECTORY "/request",
        request, sizeof(request), &length);
    if (error || length < 35 || !valid_id(request) || !memcmp(request, wifi.seen, 32)) return 0;
    memcpy(wifi.seen, request, 32);
    wifi.seen[32] = 0;
    if (wifi.pending != NONE) { (void)response(wifi.seen, -EBUSY, ""); return 0; }
    memcpy(wifi.pending_id, wifi.seen, sizeof(wifi.pending_id));
    const char *operation = request + 33;
    unsigned transition = 0;
    if (!strcmp(operation, "scan\n")) {
        uint32_t old_events;
        error = netd_kobox2_wifi_take_events(&old_events);
        if (!error) error = acknowledged("SCAN");
        if (!error) { wifi.pending = SCAN; wifi.deadline = now_ms + 10000; }
    } else if (!strcmp(operation, "connect\n")) {
        error = apply_configuration();
        wifi.configuration_error = error;
        if (!error) {
            wifi.pending = CONNECT;
            wifi.deadline = now_ms + 25000;
            wifi.ipv4_ready = 0;
            transition = NETD_WIFI_NETWORK_CONNECT;
        }
    } else if (!strcmp(operation, "disconnect\n")) {
        error = acknowledged("DISCONNECT");
        if (!error) { wifi.ipv4_ready = 0; transition = NETD_WIFI_NETWORK_DISCONNECT; }
    } else error = -EINVAL;
    if (error || wifi.pending == NONE) complete(error, error ? "" : "Disconnected.\n");
    return transition;
}

static int field_is(const char *text, const char *field, const char *value)
{
    size_t name_length = strlen(field), value_length = strlen(value);
    for (const char *line = text; *line;) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        if ((size_t)(end - line) == name_length + 1 + value_length &&
            !memcmp(line, field, name_length) && line[name_length] == '=' &&
            !memcmp(line + name_length + 1, value, value_length)) return 1;
        line = *end ? end + 1 : end;
    }
    return 0;
}

static int publish_status(const char *supplicant, int error)
{
    const struct netd_network_config *policy = netd_network_config_get();
    const struct netd_link_info *link = netd_link_current();
    int connected = !error && field_is(supplicant, "wpa_state", "COMPLETED");
    const char *address = connected && wifi.ipv4_ready && link && link->carrier && policy ?
        policy->address_text : "unconfigured";
    int size = snprintf(wifi.status, sizeof(wifi.status),
        "service=%s\nstatus=%d\nconfiguration_status=%d\nip_address=%s\n%s",
        error ? "error" : "ready", error, wifi.configuration_error,
        address, error ? "" : supplicant);
    if (size < 0 || (size_t)size >= sizeof(wifi.status)) return -EOVERFLOW;
    return netd_wifi_file_write(wifi.endpoint, "status", wifi.status, (size_t)size);
}

int netd_wifi_control_start(struct netd_runtime *runtime)
{
    if (!runtime || !runtime->cfg || wifi.started) return -EINVAL;
    wifi.endpoint = (pacha_fd_t)runtime->cfg->filed_endpoint_fd;
    int error = netd_wifi_files_prepare(wifi.endpoint);
    if (!error) error = session_create();
    /* Invalidate the previous instance's status before publishing the new
     * session. A CLI must never pair a new session with a recent old UP. */
    if (!error) error = publish_status("", -EAGAIN);
    if (!error) {
        char session[18];
        (void)snprintf(session, sizeof(session), "%s\n", wifi.session);
        error = netd_wifi_file_write(wifi.endpoint, "session", session, 17);
    }
    if (error) return error;
    wifi.started = 1;
    return 0;
}

unsigned netd_wifi_control_poll(struct netd_runtime *runtime, uint64_t now_ms)
{
    (void)runtime;
    if (!wifi.started) return 0;
    uint64_t generation = netd_kobox2_wifi_generation();
    if (!generation) {
        if (wifi.pending != NONE) complete(-ENODEV, "");
        (void)publish_status("", -ENODEV);
        return 0;
    }
    unsigned transition = 0;
    if (generation != wifi.generation) {
        wifi.generation = generation;
        wifi.autoloaded = 0;
        wifi.ipv4_ready = 0;
        if (wifi.pending != NONE) {
            complete(-ESTALE, "");
        }
    }
    retry_response();
    transition |= receive_request(now_ms);
    if (transition) wifi.autoloaded = 1;
    if (!wifi.autoloaded && wifi.pending == NONE) {
        int error = apply_configuration();
        wifi.autoloaded = 1;
        if (!error) transition |= NETD_WIFI_NETWORK_CONNECT;
        else if (error != -ENOENT) wifi.configuration_error = error;
    }
    char state[4097];
    size_t length;
    int error = command("STATUS", state, sizeof(state), &length);
    if (!error && !strstr(state, "wpa_state=")) error = -EPROTO;
    if (!error) error = publish_status(state, 0);
    else (void)publish_status("", error);
    uint32_t events = 0;
    if (!error) error = netd_kobox2_wifi_take_events(&events);
    if (wifi.pending == SCAN && !error && (events & KOBOX_WIFI_SCAN_COMPLETE)) {
        char scan[8193];
        error = command("SCAN_RESULTS", scan, sizeof(scan), &length);
        if (!error) error = netd_wifi_file_write(wifi.endpoint, "scan-results", scan, length);
        if (!error) complete(0, "");
    }
    if (wifi.pending == SCAN && !error && (events & KOBOX_WIFI_SCAN_FAILED)) error = -EIO;
    if (wifi.pending == CONNECT && !error) {
        if (events & KOBOX_WIFI_AUTH_FAILED) error = -EACCES;
        else if (field_is(state, "wpa_state", "COMPLETED") && wifi.ipv4_ready &&
            netd_link_current() && netd_link_current()->carrier && netd_network_config_get()) {
            complete(0, wifi.status);
        }
    }
    if ((wifi.pending == SCAN || wifi.pending == CONNECT) && !error && now_ms >= wifi.deadline)
        error = -ETIMEDOUT;
    if ((wifi.pending == SCAN || wifi.pending == CONNECT) && error) complete(error, "");
    return transition;
}

void netd_wifi_control_ipv4_ready(int ready, int error)
{
    wifi.ipv4_ready = ready != 0;
    if (error && wifi.started && wifi.pending == CONNECT) complete(error, "");
}

void netd_wifi_control_link_changed(int carrier)
{
    netd_libuinet_ipv4_suspend();
    netd_packet_io_discard_rx();
    wifi.ipv4_ready = 0;
    wifi.link_transition = carrier ? NETD_WIFI_NETWORK_CONNECT : NETD_WIFI_NETWORK_LOST;
}

unsigned netd_wifi_control_take_link_transition(void)
{
    unsigned transition = wifi.link_transition;
    wifi.link_transition = 0;
    return transition;
}
