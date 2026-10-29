/* Actual native controller; only filed/NIC/link/policy boundaries are models.
 * File writes model atomic publication: a failed write keeps the old file.
 * This suite does not claim filesystem permission or Linux driver coverage. */
#include <sys/wait.h>
#include <unistd.h>
#include "../userland/netd/src/wifi_control.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", \
    __func__, __LINE__, #x); exit(1); } } while (0)

struct fixture_file { const char *name; char text[16384]; int present; };
static struct fixture_file files[] = {{"request", "", 0}, {"response", "", 0},
    {"status", "", 0}, {"session", "", 0}, {"scan-results", "", 0}};
static int response_fail, status_fail, prepare_error, configuration_error;
static unsigned response_attempts, response_commits, scan_calls, results_calls;
static unsigned disconnect_calls, configuration_calls, publication_count;
static char publication_order[32], last_attempt[16384];
static int config_present, status_error;
static uint64_t generation;
static uint32_t events;
static const char *nic_status;
static const char scan_text[] = "bssid / frequency / signal level / flags / ssid\n";
static struct netd_link_info test_link = {.carrier = 1};
static struct netd_network_config test_policy = {.address_text = "192.0.2.10"};
static struct netd_boot_config boot = {.filed_endpoint_fd = 32};
static struct netd_runtime runtime = {.cfg = &boot};

static struct fixture_file *file_named(const char *name)
{
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
        if (!strcmp(files[i].name, name)) return &files[i];
    CHECK(!"unexpected file"); return NULL;
}

int netd_wifi_files_prepare(pacha_fd_t endpoint)
{ CHECK(endpoint == 32); return prepare_error; }

int netd_wifi_file_read(pacha_fd_t endpoint, const char *path, void *bytes,
    size_t capacity, size_t *length)
{
    CHECK(endpoint == 32); *length = 0;
    const char *text;
    if (!strcmp(path, NETD_WIFI_CONFIG)) {
        if (!config_present) return -ENOENT;
        text = "update_config=0\n";
    } else {
        CHECK(!strncmp(path, NETD_WIFI_DIRECTORY "/", strlen(NETD_WIFI_DIRECTORY) + 1));
        struct fixture_file *file = file_named(strrchr(path, '/') + 1);
        if (!file->present) return -ENOENT;
        text = file->text;
    }
    CHECK(strlen(text) < capacity);
    *length = strlen(text); memcpy(bytes, text, *length + 1); return 0;
}

int netd_wifi_file_write(pacha_fd_t endpoint, const char *name,
    const void *bytes, size_t length)
{
    CHECK(endpoint == 32);
    struct fixture_file *file = file_named(name);
    CHECK(length < sizeof(file->text) && !memchr(bytes, 0, length));
    if (!strcmp(name, "response")) {
        ++response_attempts;
        memcpy(last_attempt, bytes, length); last_attempt[length] = 0;
        if (response_fail) return -EIO;
        ++response_commits;
    }
    if (!strcmp(name, "status") && status_fail) return -EIO;
    if (!strcmp(name, "session")) {
        /* Observe the exact publication boundary, not just the final files. */
        CHECK(file_named("status")->present);
        CHECK(strstr(file_named("status")->text, "service=error\nstatus=-11\n"));
    }
    if (publication_count < sizeof(publication_order))
        publication_order[publication_count++] = !strcmp(name, "status") ? 'T' :
            !strcmp(name, "session") ? 'S' : '?';
    memcpy(file->text, bytes, length); file->text[length] = 0; file->present = 1;
    return 0;
}

uint64_t netd_kobox2_wifi_generation(void) { return generation; }
int netd_kobox2_wifi_configuration(const void *bytes, size_t length)
{
    CHECK(length == strlen("update_config=0\n"));
    CHECK(!memcmp(bytes, "update_config=0\n", length));
    ++configuration_calls; return configuration_error;
}
int netd_kobox2_wifi_take_events(uint32_t *mask)
{ *mask = events; events = 0; return 0; }
int netd_kobox2_wifi_command(const void *bytes, size_t length, void *reply,
    size_t capacity, size_t *received)
{
    char request[64]; CHECK(length < sizeof(request));
    memcpy(request, bytes, length); request[length] = 0;
    const char *text = "OK\n";
    if (!strcmp(request, "STATUS")) {
        if (status_error) return status_error;
        text = nic_status;
    } else if (!strcmp(request, "SCAN")) ++scan_calls;
    else if (!strcmp(request, "SCAN_RESULTS")) { ++results_calls; text = scan_text; }
    else if (!strcmp(request, "DISCONNECT")) ++disconnect_calls;
    else CHECK(!"unexpected NIC command");
    *received = strlen(text); CHECK(*received <= capacity);
    memcpy(reply, text, *received); return 0;
}
const struct netd_link_info *netd_link_current(void) { return &test_link; }
const struct netd_network_config *netd_network_config_get(void) { return &test_policy; }
void netd_libuinet_ipv4_suspend(void) { }
void netd_packet_io_discard_rx(void) { }

static void reset(void)
{
    memset(&wifi, 0, sizeof(wifi));
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        files[i].present = 0; files[i].text[0] = 0;
    }
    generation = 1; events = 0;
    response_fail = status_fail = prepare_error = configuration_error = status_error = 0;
    response_attempts = response_commits = scan_calls = results_calls = 0;
    disconnect_calls = configuration_calls = publication_count = 0;
    memset(publication_order, 0, sizeof(publication_order)); last_attempt[0] = 0;
    config_present = 0; nic_status = "wpa_state=DISCONNECTED\n";
}
static void start(void)
{
    reset(); CHECK(netd_wifi_control_start(&runtime) == 0);
    CHECK(netd_wifi_control_poll(&runtime, 0) == 0);
}
static void request(const char *op)
{
    struct fixture_file *file = file_named("request");
    CHECK(snprintf(file->text, sizeof(file->text), "%s%016x %s\n", wifi.session, 1, op) > 0);
    file->present = 1;
}
static void expect_reply(int error, const char *payload)
{
    char expected[16384];
    CHECK(snprintf(expected, sizeof(expected), "%s%016x %d\n%s",
        wifi.session, 1, error, payload) > 0);
    CHECK(file_named("response")->present);
    CHECK(!strcmp(file_named("response")->text, expected));
    CHECK(wifi.pending == NONE && response_commits == 1);
}
static void scan_pending(void)
{
    start(); request("scan"); CHECK(netd_wifi_control_poll(&runtime, 10) == 0);
    CHECK(scan_calls == 1 && !response_commits && wifi.pending != NONE);
}

static void startup_order(void)
{
    reset();
    strcpy(file_named("status")->text, "service=ready\nstatus=0\nwpa_state=COMPLETED\n");
    file_named("status")->present = 1;
    CHECK(netd_wifi_control_start(&runtime) == 0);
    CHECK(publication_count == 2 && publication_order[0] == 'T' && publication_order[1] == 'S');
    CHECK(strlen(file_named("session")->text) == 17 && wifi.started);
}
static void startup_status_failure(void)
{
    reset(); status_fail = 1;
    strcpy(file_named("session")->text, "0000000000000000\n");
    file_named("session")->present = 1;
    CHECK(netd_wifi_control_start(&runtime) == -EIO);
    CHECK(!wifi.started && publication_count == 0);
    CHECK(!strcmp(file_named("session")->text, "0000000000000000\n"));
}
static void scan_reply_retry(void)
{
    scan_pending(); response_fail = 1; events = KOBOX_WIFI_SCAN_COMPLETE;
    netd_wifi_control_poll(&runtime, 20);
    CHECK(wifi.pending != NONE && !response_commits && results_calls == 1);
    char pending[sizeof(last_attempt)]; strcpy(pending, last_attempt);
    status_error = -ENETDOWN;
    netd_wifi_control_poll(&runtime, 50000); /* Completed replies do not expire. */
    CHECK(wifi.pending != NONE && !strcmp(pending, last_attempt));
    response_fail = 0; netd_wifi_control_poll(&runtime, 50001);
    expect_reply(0, "");
    CHECK(!strcmp(file_named("scan-results")->text, scan_text));
    netd_wifi_control_poll(&runtime, 50002);
    CHECK(scan_calls == 1 && results_calls == 1 && response_commits == 1);
}
static void generation_cancel_retry(void)
{
    scan_pending(); response_fail = 1; generation = 2;
    netd_wifi_control_poll(&runtime, 20);
    CHECK(wifi.pending != NONE && !response_commits);
    netd_wifi_control_poll(&runtime, 21); CHECK(wifi.pending != NONE);
    response_fail = 0; netd_wifi_control_poll(&runtime, 22);
    expect_reply(-ESTALE, ""); CHECK(scan_calls == 1 && results_calls == 0);
}
static void generation_overrides_success(void)
{
    scan_pending(); response_fail = 1; events = KOBOX_WIFI_SCAN_COMPLETE;
    netd_wifi_control_poll(&runtime, 20); CHECK(wifi.pending != NONE);
    generation = 2; response_fail = 0; netd_wifi_control_poll(&runtime, 21);
    expect_reply(-ESTALE, ""); CHECK(scan_calls == 1 && results_calls == 1);
}
static void generation_loss_overrides_success(void)
{
    scan_pending(); response_fail = 1; events = KOBOX_WIFI_SCAN_COMPLETE;
    netd_wifi_control_poll(&runtime, 20); CHECK(wifi.pending != NONE);
    generation = 0; netd_wifi_control_poll(&runtime, 21);
    CHECK(wifi.pending != NONE && !response_commits);
    response_fail = 0; netd_wifi_control_poll(&runtime, 22);
    expect_reply(-ENODEV, ""); CHECK(scan_calls == 1 && results_calls == 1);
}
static void connect_reply_snapshot(void)
{
    start(); config_present = 1; request("connect");
    CHECK(netd_wifi_control_poll(&runtime, 10) == NETD_WIFI_NETWORK_CONNECT);
    nic_status = "wpa_state=COMPLETED\nssid=first\n";
    netd_wifi_control_poll(&runtime, 20); CHECK(!response_attempts); /* DHCP not ready. */
    response_fail = 1; netd_wifi_control_ipv4_ready(1, 0);
    netd_wifi_control_poll(&runtime, 21); CHECK(wifi.pending != NONE);
    char pending[sizeof(last_attempt)]; strcpy(pending, last_attempt);
    nic_status = "wpa_state=COMPLETED\nssid=second\n";
    netd_wifi_control_poll(&runtime, 22); CHECK(!strcmp(pending, last_attempt));
    response_fail = 0; netd_wifi_control_poll(&runtime, 23);
    CHECK(wifi.pending == NONE && response_commits == 1 && configuration_calls == 1);
    CHECK(!strcmp(file_named("response")->text, pending));
    CHECK(strstr(pending, "ssid=first\n") && !strstr(pending, "ssid=second\n"));
}
static void immediate_error_retry(void)
{
    start(); config_present = 1; configuration_error = -EACCES; response_fail = 1;
    request("connect"); netd_wifi_control_poll(&runtime, 10);
    CHECK(wifi.pending != NONE && configuration_calls == 1);
    netd_wifi_control_poll(&runtime, 11); CHECK(configuration_calls == 1);
    response_fail = 0; netd_wifi_control_poll(&runtime, 12);
    expect_reply(-EACCES, ""); CHECK(configuration_calls == 1);
}
static void disconnect_retry(void)
{
    start(); request("disconnect"); response_fail = 1;
    CHECK(netd_wifi_control_poll(&runtime, 10) == NETD_WIFI_NETWORK_DISCONNECT);
    CHECK(wifi.pending != NONE && disconnect_calls == 1);
    netd_wifi_control_poll(&runtime, 11); CHECK(disconnect_calls == 1);
    response_fail = 0; netd_wifi_control_poll(&runtime, 12);
    expect_reply(0, "Disconnected.\n"); CHECK(disconnect_calls == 1);
}

int main(void)
{
    const struct { const char *name; void (*run)(void); } cases[] = {
        {"startup status before session", startup_order},
        {"failed startup status preserves old session", startup_status_failure},
        {"SCAN reply retries without event/command replay", scan_reply_retry},
        {"generation ESTALE reply survives failed publication", generation_cancel_retry},
        {"generation change overrides unpublished success", generation_overrides_success},
        {"device loss overrides unpublished success", generation_loss_overrides_success},
        {"CONNECT caches completed reply text after DHCP", connect_reply_snapshot},
        {"immediate configuration error reply retry", immediate_error_retry},
        {"DISCONNECT reply retry without command replay", disconnect_retry},
    };
    setbuf(stdout, NULL); unsigned failures = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        pid_t child = fork(); CHECK(child >= 0);
        if (!child) { cases[i].run(); _exit(0); }
        int result; CHECK(waitpid(child, &result, 0) == child);
        int passed = WIFEXITED(result) && WEXITSTATUS(result) == 0;
        printf("%s %s\n", passed ? "PASS" : "FAIL", cases[i].name);
        failures += !passed;
    }
    printf("NETD_WIFI_CONTROL_UNIT=%s cases=%zu\n", failures ? "FAIL" : "PASS",
        sizeof(cases) / sizeof(cases[0]));
    return failures ? 1 : 0;
}
