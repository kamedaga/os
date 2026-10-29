/* SPDX-License-Identifier: MIT */
/* Link the actual kernel-compiled adapter; model only its external boundaries. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/socket.h>
#include "boot/wifi_service.h"
#include "boot/net_port.h"

struct kobox_linux_wifi_service;
struct kobox_linux_net_port;
extern int kobox_linux_wifi_service_open(struct kobox_linux_wifi_service **,
    struct kobox_linux_net_port *, const struct kobox_linux_user_process_config *);
extern int kobox_linux_wifi_service_monitor(void *);
extern int kobox_linux_wifi_service_poll_events(struct kobox_linux_wifi_service *);
extern int kobox_linux_wifi_service_request(struct kobox_linux_wifi_service *,
    const void *, size_t, void *, size_t, size_t *);
extern int kobox_linux_wifi_service_configure(struct kobox_linux_wifi_service *, const void *, size_t);
extern int kobox_linux_wifi_service_events(struct kobox_linux_wifi_service *, uint32_t *);
extern int kobox_linux_wifi_service_close(struct kobox_linux_wifi_service *);

static int owner, other_owner, process_token, control_token, port_token, host_token;
static void *current_owner = &owner;
static int authorized, live_allocations, stop_error, replace_error, spawn_error, exited;
static int control_open_error;
static unsigned log_reads, startup_failure_reports;
void kobox_linux_boot_diagnostic(const char *format, ...) {
    char line[256]; va_list args;
    va_start(args, format); int length = vsnprintf(line, sizeof(line), format, args); va_end(args);
    assert(length >= 0 && length < (int)sizeof(line));
    if (strstr(line, "phase=control-open error=-110")) ++startup_failure_reports;
}
int kobox_linux_user_process_read_log(struct kobox_linux_user_process *p,
    uint64_t offset, void *bytes, size_t capacity, size_t *read) {
    assert(p == (void *)&process_token && !offset && capacity == 1024);
    static const char output[] = "Successfully initialized wpa_supplicant\nstartup\033diagnostic\n";
    memcpy(bytes, output, sizeof(output)-1); *read = sizeof(output)-1;
    ++log_reads; return 0;
}
static unsigned destroyed, control_closed, pending_events;
static unsigned query_closed, status_sends, events_on_receive;
static uint64_t association_sequence;
static int query_token, query_live, query_waiting, hold_status, status_send_error;
static char query_reply[128];
static size_t query_length;
static void *binding;
static const char *state = "DISCONNECTED";
static char operations[32];
static size_t operation_count;
unsigned int __preempt_count;
unsigned long jiffies;
char init_net;
_Static_assert(offsetof(struct kobox_linux_net_info, association_sequence) == 8, "net info association offset");
_Static_assert(offsetof(struct kobox_linux_net_info, rx_drops) == 16, "net info shifted members");
_Static_assert(sizeof(struct kobox_linux_net_info) == 40, "net info current layout");

void *kobox_provider_current_task(void) { return current_owner; }
/* Hosted IRQ flags encode notification masking, not native RFLAGS.IF. */
unsigned long kobox_provider_irq_save_flags(void) { return 0; }
unsigned long kobox_provider_current_percpu_offset(void) { return 0; }
unsigned long kobox_provider_preempt_save(void) { return 0; }
void kobox_provider_preempt_restore(unsigned long flags) { (void)flags; }
void *__kmalloc_large_noprof(size_t size, unsigned int flags)
{
    (void)flags;
    void *p = calloc(1, size);
    assert(p); ++live_allocations;
    return p;
}
void kfree(const void *p) { if (p) --live_allocations; free((void *)p); }

struct status_kvec { void *base; size_t length; };
int sock_create_kern(void *net, int family, int type, int protocol, void **out)
{
    assert(net && family == AF_UNIX && type == SOCK_DGRAM && !protocol && !query_live);
    *out = &query_token; query_live = 1;
    return 0;
}
int kernel_bind(void *socket, const void *address, int length)
{
    assert(socket == &query_token && length == 110);
    assert(!memcmp((const char *)address + 2, "\0status-", 8));
    return 0;
}
int kernel_connect(void *socket, const void *address, int length, int flags)
{
    assert(socket == &query_token && length == 110 && flags);
    assert(!memcmp((const char *)address + 2, "\0" KOBOX_WIFI_CONTROL_NAME,
        sizeof(KOBOX_WIFI_CONTROL_NAME)));
    return 0;
}
/* Fixture randomness is only an abstract socket-name input, never crypto. */
void get_random_bytes(void *bytes, size_t size) { memset(bytes, 0x42, size); }
char *bin2hex(char *out, const void *bytes, size_t size)
{
    (void)bytes;
    memset(out, '4', size * 2);
    return out + size * 2;
}
int kernel_sendmsg(void *socket, const void *message, const struct status_kvec *v,
    size_t count, size_t length)
{
    assert(socket == &query_token && message && count == 1 && v->length == length);
    assert(length == strlen("IFNAME=wlp88s0 STATUS") &&
        !memcmp(v->base, "IFNAME=wlp88s0 STATUS", length));
    assert(!query_waiting);
    if (status_send_error) return status_send_error;
    ++status_sends;
    query_length = (size_t)snprintf(query_reply, sizeof(query_reply), "wpa_state=%s\n", state);
    query_waiting = 1;
    return (int)length;
}
int kernel_recvmsg(void *socket, void *message, const struct status_kvec *v,
    size_t count, size_t length, int flags)
{
    assert(socket == &query_token && message && count == 1 && v->length == length);
    assert(flags & MSG_DONTWAIT);
    if (!query_waiting || hold_status) return -EAGAIN;
    assert(query_length <= length);
    memcpy(v->base, query_reply, query_length);
    query_waiting = 0;
    pending_events |= events_on_receive;
    events_on_receive = 0;
    return (int)query_length;
}
void sock_release(void *socket)
{
    assert(socket == &query_token && query_live);
    query_live = query_waiting = 0;
    operations[operation_count++] = 'Q';
    ++query_closed;
}

int kobox_linux_net_port_name(struct kobox_linux_net_port *p, char *name, size_t capacity)
{
    assert(p == (void *)&port_token && capacity >= 16);
    strcpy(name, "wlp88s0");
    return 0;
}
int kobox_linux_net_port_wifi_bind(struct kobox_linux_net_port *p, struct kobox_linux_wifi_service *s)
{
    assert(p == (void *)&port_token);
    if (s) assert(!binding);
    else operations[operation_count++] = 'U';
    binding = s;
    return 0;
}
void kobox_linux_net_port_wifi_authorize(struct kobox_linux_net_port *p, bool allowed)
{
    assert(p == (void *)&port_token);
    if (authorized != allowed) ++association_sequence;
    authorized = allowed;
}
void kobox_linux_net_port_wifi_association(struct kobox_linux_net_port *p)
{
    assert(p == (void *)&port_token);
    ++association_sequence;
    authorized = 0;
}
int kobox_linux_user_process_spawn(const struct kobox_linux_user_process_config *c,
    struct kobox_linux_user_process **out)
{
    assert(c->host_space == &host_token && c->capability_mask == ((1u << 12) | (1u << 13)));
    const char *expected[] = {KOBOX_WIFI_PROGRAM, "-Dnl80211", "-i", "wlp88s0",
        "-c", KOBOX_WIFI_CONFIG, "-g", KOBOX_WIFI_CONTROL_ARGUMENT};
    for (size_t i = 0; i < 8; ++i) assert(!strcmp(c->argv[i], expected[i]));
    assert(!c->argv[8]);
    if (spawn_error) return spawn_error;
    *out = (void *)&process_token;
    return 0;
}
int kobox_linux_user_process_status(struct kobox_linux_user_process *p,
    struct kobox_linux_user_process_status *status)
{
    assert(p == (void *)&process_token);
    status->exited = exited;
    status->launch_error = 0;
    return 0;
}
int kobox_linux_user_process_stop(struct kobox_linux_user_process *p, uint32_t term, uint32_t kill)
{
    assert(p == (void *)&process_token && term && kill);
    operations[operation_count++] = 'S';
    return stop_error;
}
int kobox_linux_user_process_destroy(struct kobox_linux_user_process *p)
{
    assert(p == (void *)&process_token);
    operations[operation_count++] = 'D';
    ++destroyed;
    return 0;
}
int kobox_linux_user_process_replace_file(struct kobox_linux_user_process *p,
    const char *path, const void *bytes, size_t size)
{
    assert(p == (void *)&process_token && !strcmp(path, KOBOX_WIFI_CONFIG));
    assert(bytes && size && size <= 8192 && !authorized);
    return replace_error;
}
int kobox_linux_wifi_control_open(struct kobox_linux_user_process *p,
    struct kobox_linux_wifi_control **out, uint32_t timeout)
{
    assert(p == (void *)&process_token && timeout);
    if (control_open_error) return control_open_error;
    *out = (void *)&control_token;
    return 0;
}
int kobox_linux_wifi_control_close(struct kobox_linux_wifi_control *p)
{
    assert(p == (void *)&control_token);
    operations[operation_count++] = 'C';
    ++control_closed;
    return 0;
}
int kobox_linux_wifi_control_events(struct kobox_linux_wifi_control *p, uint32_t *events)
{
    assert(p == (void *)&control_token);
    *events = pending_events;
    pending_events = 0;
    return 0;
}
int kobox_linux_wifi_control_request(struct kobox_linux_wifi_control *p,
    const void *request, size_t length, void *reply, size_t capacity, size_t *received, uint32_t timeout)
{
    assert(p == (void *)&control_token && timeout && length < 128);
    char command[128], status[128];
    memcpy(command, request, length); command[length] = 0;
    const char *response = "OK\n";
    if (!strcmp(command, "PING")) response = "PONG\n";
    else {
        const char prefix[] = "IFNAME=wlp88s0 ";
        assert(!strncmp(command, prefix, sizeof(prefix) - 1));
        const char *operation = command + sizeof(prefix) - 1;
        if (!strcmp(operation, "STATUS")) {
            snprintf(status, sizeof(status), "wpa_state=%s\n", state);
            response = status;
        } else if (!strcmp(operation, "RECONFIGURE")) assert(!authorized);
        else if (!strcmp(operation, "DISCONNECT")) {
            assert(!authorized); state = "DISCONNECTED"; pending_events |= KOBOX_WIFI_DISCONNECTED;
        }
        else if (!strcmp(operation, "RECONNECT")) {
            assert(!authorized); state = "COMPLETED"; pending_events |= KOBOX_WIFI_CONNECTED;
        }
        else assert(0);
    }
    *received = strlen(response);
    assert(*received <= capacity);
    memcpy(reply, response, *received);
    return 0;
}

int main(void)
{
    const char program = 0;
    const char initial[] = "update_config=0\n";
    struct kobox_linux_user_process_file files[] = {
        {.path=KOBOX_WIFI_PROGRAM, .data=&program, .size=1, .mode=0755},
        {.path=KOBOX_WIFI_CONFIG, .data=initial, .size=sizeof(initial)-1, .mode=0600},
    };
    struct kobox_linux_user_process_config c = {.size=sizeof(c), .host_space=&host_token,
        .files=files, .file_count=2, .capability_mask=(1u<<12)|(1u<<13)};
    struct kobox_linux_wifi_service *s = NULL;
    int opened = kobox_linux_wifi_service_open(&s, (void *)&port_token, &c);
    if (opened) fprintf(stderr, "service open failed: %d bound=%d allocations=%d\n",
        opened, binding != NULL, live_allocations);
    assert(!opened);
    assert(s && binding == s && !authorized);
    assert(!kobox_linux_wifi_service_poll_events(s) && !authorized); /* initial STATUS */
    state = "COMPLETED";
    pending_events = KOBOX_WIFI_CONNECTED;
    hold_status = 1;
    uint64_t before_association = association_sequence;
    assert(!kobox_linux_wifi_service_poll_events(s) && !authorized);
    assert(association_sequence > before_association);
    unsigned long before_ticks = jiffies;
    assert(!kobox_linux_wifi_service_poll_events(s) && jiffies == before_ticks && !authorized);
    hold_status = 0;
    assert(!kobox_linux_wifi_service_poll_events(s) && authorized);
    uint32_t events;
    assert(!kobox_linux_wifi_service_events(s, &events) && events == KOBOX_WIFI_CONNECTED);
    assert(!kobox_linux_wifi_service_events(s, &events) && !events);

    /* A second CONNECTED while already authorized still closes/advances. */
    before_association = association_sequence;
    pending_events = KOBOX_WIFI_CONNECTED;
    status_send_error = -EAGAIN;
    assert(!kobox_linux_wifi_service_poll_events(s) && !authorized);
    assert(association_sequence > before_association);
    unsigned before_sends = status_sends;
    assert(!kobox_linux_wifi_service_poll_events(s) && status_sends == before_sends);
    status_send_error = 0;
    assert(!kobox_linux_wifi_service_poll_events(s) && !authorized);
    assert(!kobox_linux_wifi_service_poll_events(s) && authorized);
    assert(!kobox_linux_wifi_service_events(s, &events) && events == KOBOX_WIFI_CONNECTED);

    /* A stale COMPLETED reply must not undo a disconnect/association change. */
    assert(!kobox_linux_wifi_service_monitor(s)); /* sends old COMPLETED STATUS */
    pending_events = KOBOX_WIFI_DISCONNECTED;
    state = "DISCONNECTED";
    assert(!kobox_linux_wifi_service_poll_events(s) && !authorized);
    assert(!kobox_linux_wifi_service_poll_events(s) && !authorized);
    assert(!kobox_linux_wifi_service_events(s, &events) && events == KOBOX_WIFI_DISCONNECTED);
    state = "COMPLETED"; pending_events = KOBOX_WIFI_CONNECTED;
    assert(!kobox_linux_wifi_service_poll_events(s) && !authorized);
    assert(!kobox_linux_wifi_service_poll_events(s) && authorized);
    assert(!kobox_linux_wifi_service_events(s, &events) && events == KOBOX_WIFI_CONNECTED);

    /* Obsolete malformed/FAIL STATUS is drained, not assigned to the new epoch. */
    assert(!kobox_linux_wifi_service_monitor(s));
    memcpy(query_reply, "FAIL\n", 5); query_length = 5;
    pending_events = KOBOX_WIFI_DISCONNECTED; state = "DISCONNECTED";
    assert(!kobox_linux_wifi_service_poll_events(s) && !authorized);
    assert(!kobox_linux_wifi_service_poll_events(s) && !authorized);
    assert(!kobox_linux_wifi_service_events(s, &events) && events == KOBOX_WIFI_DISCONNECTED);
    state = "COMPLETED"; pending_events = KOBOX_WIFI_CONNECTED;
    assert(!kobox_linux_wifi_service_poll_events(s));
    assert(!kobox_linux_wifi_service_poll_events(s) && authorized);
    assert(!kobox_linux_wifi_service_events(s, &events) && events == KOBOX_WIFI_CONNECTED);

    /* Also observe a disconnect arriving during the STATUS receive itself. */
    assert(!kobox_linux_wifi_service_monitor(s));
    events_on_receive = KOBOX_WIFI_DISCONNECTED;
    state = "DISCONNECTED";
    assert(!kobox_linux_wifi_service_poll_events(s) && !authorized);
    assert(!kobox_linux_wifi_service_poll_events(s) && !authorized);
    assert(!kobox_linux_wifi_service_events(s, &events) && events == KOBOX_WIFI_DISCONNECTED);
    state = "COMPLETED"; pending_events = KOBOX_WIFI_CONNECTED;
    assert(!kobox_linux_wifi_service_poll_events(s));
    assert(!kobox_linux_wifi_service_poll_events(s) && authorized);
    assert(!kobox_linux_wifi_service_events(s, &events) && events == KOBOX_WIFI_CONNECTED);

    replace_error = -EIO;
    assert(kobox_linux_wifi_service_configure(s, initial, sizeof(initial)-1) == -EIO);
    assert(!authorized);
    assert(!kobox_linux_wifi_service_monitor(s));
    assert(!kobox_linux_wifi_service_poll_events(s) && !authorized);
    replace_error = 0;
    assert(!kobox_linux_wifi_service_configure(s, initial, sizeof(initial)-1));
    assert(!kobox_linux_wifi_service_monitor(s) && authorized);
    char reply[128]; size_t received;
    assert(!kobox_linux_wifi_service_request(s, "DISCONNECT", 10, reply, sizeof(reply), &received));
    assert(!authorized);
    assert(!kobox_linux_wifi_service_request(s, "RECONNECT", 9, reply, sizeof(reply), &received));
    assert(!authorized);
    assert(!kobox_linux_wifi_service_monitor(s) && authorized);
    pending_events = KOBOX_WIFI_SCAN_COMPLETE;
    assert(!kobox_linux_wifi_service_monitor(s));
    assert(!kobox_linux_wifi_service_events(s, &events) &&
        events == (KOBOX_WIFI_SCAN_COMPLETE | KOBOX_WIFI_DISCONNECTED | KOBOX_WIFI_CONNECTED));
    current_owner = &other_owner;
    assert(kobox_linux_wifi_service_close(s) == -EINVAL);
    current_owner = &owner;
    stop_error = -ETIMEDOUT;
    assert(kobox_linux_wifi_service_close(s) == -ETIMEDOUT);
    assert(binding == s && live_allocations == 1 && !destroyed && !control_closed && !query_closed);
    assert(operation_count == 1 && operations[0] == 'S');
    stop_error = 0; operation_count = 0;
    assert(!kobox_linux_wifi_service_close(s));
    assert(operation_count == 5 && !memcmp(operations, "SQCDU", 5));
    assert(!binding && !live_allocations);

    s = NULL; spawn_error = -ENOMEM; operation_count = 0;
    assert(kobox_linux_wifi_service_open(&s, (void *)&port_token, &c) == -ENOMEM && s);
    assert(!kobox_linux_wifi_service_close(s));
    assert(operation_count == 1 && operations[0] == 'U' && !live_allocations);
    spawn_error = 0; s = NULL; state = "COMPLETED"; exited = 0;
    assert(!kobox_linux_wifi_service_open(&s, (void *)&port_token, &c));
    assert(!kobox_linux_wifi_service_poll_events(s) && authorized);
    exited = 1;
    assert(kobox_linux_wifi_service_monitor(s) == -EPIPE && !authorized);
    assert(!kobox_linux_wifi_service_close(s) && !live_allocations);

    /* Pending STATUS has a deadline, but INFO never sleeps until it. */
    s = NULL; exited = 0;
    assert(!kobox_linux_wifi_service_open(&s, (void *)&port_token, &c));
    hold_status = 1;
    assert(!kobox_linux_wifi_service_poll_events(s));
    jiffies += 100000;
    assert(kobox_linux_wifi_service_poll_events(s) == -ETIMEDOUT && !authorized);
    assert(!kobox_linux_wifi_service_close(s) && !live_allocations);
    hold_status = 0;
    /* Failed bootstrap reports actual process state and bounded output only
     * for the known empty config; caller-provided credentials stay private. */
    s = NULL; control_open_error = -ETIMEDOUT; operation_count = 0;
    assert(kobox_linux_wifi_service_open(&s, (void *)&port_token, &c) == -ETIMEDOUT);
    assert(startup_failure_reports == 1 && log_reads == 1);
    assert(!kobox_linux_wifi_service_close(s) && !live_allocations);
    static const char private_config[] = "network={\npsk=secret\n}\n";
    files[1].data = private_config; files[1].size = sizeof(private_config)-1;
    s = NULL; operation_count = 0;
    assert(kobox_linux_wifi_service_open(&s, (void *)&port_token, &c) == -ETIMEDOUT);
    assert(startup_failure_reports == 2 && log_reads == 1);
    assert(!kobox_linux_wifi_service_close(s) && !live_allocations);
    control_open_error = 0;
    s = NULL; c.capability_mask |= 1u << 21;
    assert(kobox_linux_wifi_service_open(&s, (void *)&port_token, &c) == -EINVAL && !s);
    puts("WiFi service: nonblocking STATUS, repeated CONNECTED, stale-reply rejection, retained events and stop lifetime PASS");
    return 0;
}
