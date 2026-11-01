/* SPDX-License-Identifier: MIT */
/* Actual Linux-compiled wifi_service.c, existing external-boundary fixture.
 * Model upstream's explicit disconnected latch: reload alone preserves it. */
#define main existing_wifi_service_fixture
#define kobox_linux_wifi_control_request fixture_control_request
#define kobox_linux_user_process_replace_file fixture_replace_file
#include "kobox2_wifi_service_unit.c"
#undef main
#undef kobox_linux_wifi_control_request
#undef kobox_linux_user_process_replace_file

/* Existing parallel performance hook is outside this functional regression. */
int kobox_linux_net_port_perf_request(struct kobox_linux_net_port *port,
    const void *request, size_t length, void *reply, size_t capacity, size_t *received)
{
    (void)port; (void)request; (void)length; (void)reply; (void)capacity; (void)received;
    return 0;
}

static char command_order[8];
static unsigned command_count, reconnect_effects;
static int disconnected_flag, reconfigure_error, reconnect_error;
static const char *reconfigure_reply = "OK\n", *reconnect_reply = "OK\n";
static const char empty_config[] = "update_config=0\n";

static void record(char command)
{
    assert(command_count < sizeof(command_order));
    command_order[command_count++] = command;
}

int kobox_linux_user_process_replace_file(struct kobox_linux_user_process *p,
    const char *path, const void *bytes, size_t size)
{
    record('F');
    return fixture_replace_file(p, path, bytes, size);
}

int kobox_linux_wifi_control_request(struct kobox_linux_wifi_control *p,
    const void *request, size_t length, void *reply, size_t capacity,
    size_t *received, uint32_t timeout)
{
    assert(p == (void *)&control_token && timeout && length < 128);
    char command[128];
    memcpy(command, request, length); command[length] = 0;
    const char *answer;
    int error;
    if (!strcmp(command, "IFNAME=wlp88s0 RECONFIGURE")) {
        record('R'); assert(!authorized);
        error = reconfigure_error; answer = reconfigure_reply;
        /* Upstream reload requests scanning but does not clear disconnected.
         * Normal connected reload can scan; explicit DISCONNECT cannot. */
        if (!error && !strcmp(answer, "OK\n") && !disconnected_flag)
            state = "SCANNING";
    } else if (!strcmp(command, "IFNAME=wlp88s0 RECONNECT")) {
        record('C'); assert(!authorized);
        error = reconnect_error; answer = reconnect_reply;
        if (!error && !strcmp(answer, "OK\n") && disconnected_flag) {
            disconnected_flag = 0; state = "SCANNING"; ++reconnect_effects;
        }
    } else {
        if (!strcmp(command, "IFNAME=wlp88s0 DISCONNECT")) disconnected_flag = 1;
        return fixture_control_request(p, request, length, reply, capacity, received, timeout);
    }
    if (error) return error;
    *received = strlen(answer); assert(*received <= capacity);
    memcpy(reply, answer, *received);
    return 0;
}

static struct kobox_linux_wifi_service *open_service(int disconnect)
{
    static const char program = 0;
    struct kobox_linux_user_process_file files[] = {
        {.path = KOBOX_WIFI_PROGRAM, .data = &program, .size = 1, .mode = 0755},
        {.path = KOBOX_WIFI_CONFIG, .data = empty_config,
         .size = sizeof(empty_config) - 1, .mode = 0600}};
    struct kobox_linux_user_process_config config = {
        .size = sizeof(config), .host_space = &host_token, .files = files,
        .file_count = 2, .capability_mask = (1u << 12) | (1u << 13)};
    struct kobox_linux_wifi_service *service = NULL;
    current_owner = &owner;
    state = "DISCONNECTED"; pending_events = query_waiting = operation_count = 0;
    disconnected_flag = 0; reconnect_effects = 0;
    assert(!kobox_linux_wifi_service_open(&service, (void *)&port_token, &config));
    assert(!kobox_linux_wifi_service_poll_events(service));
    if (disconnect) {
        char reply[16]; size_t received;
        assert(!kobox_linux_wifi_service_request(service, "DISCONNECT", 10,
            reply, sizeof(reply), &received));
        assert(disconnected_flag && !authorized);
    }
    command_count = 0;
    return service;
}

static void close_service(struct kobox_linux_wifi_service *service)
{
    operation_count = 0;
    assert(!kobox_linux_wifi_service_close(service));
    assert(!live_allocations && !binding && !query_live);
}

static void successful_recovery(void)
{
    struct kobox_linux_wifi_service *service = open_service(1);
    assert(!kobox_linux_wifi_service_configure(service, empty_config, sizeof(empty_config) - 1));
    /* Regression fails here with original RECONFIGURE-only implementation. */
    printf("WiFi reconnect sequence=%.*s disconnected=%d\n", (int)command_count,
        command_order, disconnected_flag);
    fflush(stdout);
    assert(command_count == 3 && !memcmp(command_order, "FRC", 3));
    assert(!disconnected_flag && reconnect_effects == 1 && !strcmp(state, "SCANNING"));
    assert(!authorized); /* Command acceptance is not authenticated readiness. */
    state = "COMPLETED"; pending_events = KOBOX_WIFI_CONNECTED;
    assert(!kobox_linux_wifi_service_monitor(service));
    assert(!kobox_linux_wifi_service_poll_events(service));
    assert(authorized);
    close_service(service);
    puts("WiFi reconnect: actual DISCONNECT -> configure FRC clears latch; authenticated STATUS gates authorization PASS");
}

static void connected_reconnect_noop(void)
{
    struct kobox_linux_wifi_service *service = open_service(0);
    state = "COMPLETED";
    assert(!kobox_linux_wifi_service_configure(service, empty_config, sizeof(empty_config) - 1));
    assert(command_count == 3 && !memcmp(command_order, "FRC", 3));
    assert(!disconnected_flag && !reconnect_effects && !authorized);
    close_service(service);
    puts("WiFi reconnect: normal reload retains upstream RECONNECT no-op semantics PASS");
}

static void failures(void)
{
    const char *bad[] = {"FAIL\n", "OK", "OK\nextra"};
    for (unsigned phase = 0; phase < 2; ++phase) for (unsigned i = 0; i < 3; ++i) {
        struct kobox_linux_wifi_service *service = open_service(1);
        reconfigure_reply = phase ? "OK\n" : bad[i];
        reconnect_reply = phase ? bad[i] : "OK\n";
        assert(kobox_linux_wifi_service_configure(service, empty_config, sizeof(empty_config) - 1) == -EINVAL);
        assert(command_count == (phase ? 3u : 2u) && disconnected_flag && !authorized);
        /* Even a COMPLETED status cannot authorize a rejected configuration. */
        state = "COMPLETED";
        assert(!kobox_linux_wifi_service_monitor(service));
        assert(!kobox_linux_wifi_service_poll_events(service) && !authorized);
        close_service(service);
        reconfigure_reply = reconnect_reply = "OK\n";
    }
    for (unsigned phase = 0; phase < 2; ++phase) {
        struct kobox_linux_wifi_service *service = open_service(1);
        reconfigure_error = phase ? 0 : -ETIMEDOUT;
        reconnect_error = phase ? -ETIMEDOUT : 0;
        assert(kobox_linux_wifi_service_configure(service, empty_config, sizeof(empty_config) - 1) == -ETIMEDOUT);
        assert(command_count == (phase ? 3u : 2u) && disconnected_flag && !authorized);
        assert(kobox_linux_wifi_service_monitor(service) == -EPIPE);
        close_service(service);
        reconfigure_error = reconnect_error = 0;
    }
    struct kobox_linux_wifi_service *service = open_service(1);
    replace_error = -EIO;
    assert(kobox_linux_wifi_service_configure(service, empty_config, sizeof(empty_config) - 1) == -EIO);
    assert(command_count == 1 && command_order[0] == 'F' && disconnected_flag && !authorized);
    replace_error = 0; close_service(service);
    puts("WiFi reconnect: file/reload/reconnect failures, exact OK validation and no authorization on rejected config PASS");
}

int main(void)
{
    successful_recovery();
    connected_reconnect_noop();
    failures();
    return 0;
}
