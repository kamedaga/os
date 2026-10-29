/* SPDX-License-Identifier: MIT */
/* Exercise the production runner and real static ELF in PachaOS/QEMU. */
#include "host.h"
#include "boot.h"
#include "bootfs.h"
#include "vm_internal.h"
#include "exec_image.h"
#include "entropy.h"
#include "firmware_bundle.h"
#include "boot/module_launch.h"
#include "boot/user_process.h"
#include "boot/wifi_control.h"
#include "wifi_modules.h"
#include <errno.h>
#ifndef PH_USER_WIFI_RADIO
#define PH_USER_WIFI_RADIO 0
#endif
#define WIFI_TEST_MODULES (8 + PH_USER_WIFI_RADIO)

static struct ph_vm_space *spaces[5];
static struct ph_exec_image image, crypto_image;
static struct kobox_linux_native_module modules[WIFI_TEST_MODULES];
static struct kobox_linux_firmware_file firmware[KOBOX_FIRMWARE_MAX_FILES];
static size_t firmware_count;
static int (*spawn_program)(const struct kobox_linux_user_process_config *,
    struct kobox_linux_user_process **);
static int (*wait_program)(struct kobox_linux_user_process *, uint32_t);
static int (*program_status)(struct kobox_linux_user_process *,
    struct kobox_linux_user_process_status *);
static int (*read_log)(struct kobox_linux_user_process *, uint64_t, void *, size_t, size_t *);
static int (*destroy_program)(struct kobox_linux_user_process *);
static int (*control_open)(struct kobox_linux_user_process *,
    struct kobox_linux_wifi_control **, uint32_t);
static int (*control_request)(struct kobox_linux_wifi_control *, const void *,
    size_t, void *, size_t, size_t *, uint32_t);
static int (*control_close)(struct kobox_linux_wifi_control *);

#define RESOLVE(function, name) do { \
    void *symbol = ph_image_lookup(&ph_core, name); \
    PH_CHECK(symbol); memcpy(&(function), &symbol, sizeof(function)); \
} while (0)

static void prepare(const struct kobox_linux_boot_layout *layout, void *context) {
    (void)layout; (void)context;
    size_t bytes;
    const void *bootstrap = ph_bootfs_file("/srv/kobox2/bootstrap.elf", &bytes);
    for (unsigned i = 0; i < 5; i++) {
        ph_number("USER_PROCESS_VM", i);
        spaces[i] = ph_vm_create(bootstrap, bytes,
            KOBOX_X86_USER_START, KOBOX_X86_USER_END - KOBOX_X86_USER_START);
    }
    const void *program = ph_bootfs_file("/srv/kobox2/wpa_supplicant", &bytes);
    image = ph_exec_prepare_image(program, bytes);
    spaces[0]->exec_image = &image;
    spaces[3]->exec_image = &image;
    program = ph_bootfs_file("/srv/kobox2/crypto-smoke.elf", &bytes);
    crypto_image = ph_exec_prepare_image(program, bytes);
    spaces[4]->exec_image = &crypto_image;
    for (unsigned i = 0; i < WIFI_TEST_MODULES; i++) {
        char path[96] = "/srv/kobox2/modules/";
        size_t offset = strlen(path), length = strlen(module_names[i]);
        PH_CHECK(offset + length + 4 < sizeof(path));
        memcpy(path + offset, module_names[i], length);
        memcpy(path + offset + length, ".ko", 4);
        modules[i].image = ph_bootfs_file(path, &modules[i].length);
        modules[i].name = module_names[i];
    }
#if PH_USER_WIFI_RADIO
    modules[8].parameters = "radios=1";
#endif
    const void *bundle = ph_bootfs_file("/srv/kobox2/firmware.bundle", &bytes);
    PH_OK(ph_firmware_bundle_open(bundle, bytes, firmware_digest, firmware, &firmware_count));
    RESOLVE(spawn_program, "kobox_linux_user_process_spawn");
    RESOLVE(wait_program, "kobox_linux_user_process_wait");
    RESOLVE(program_status, "kobox_linux_user_process_status");
    RESOLVE(read_log, "kobox_linux_user_process_read_log");
    RESOLVE(destroy_program, "kobox_linux_user_process_destroy");
    RESOLVE(control_open, "kobox_linux_wifi_control_open");
    RESOLVE(control_request, "kobox_linux_wifi_control_request");
    RESOLVE(control_close, "kobox_linux_wifi_control_close");
}

static struct kobox_linux_user_process_status finish(
    struct kobox_linux_user_process *process) {
    int result = wait_program(process, 10000);
    ph_number("user process wait", (uint64_t)(int64_t)result);
    PH_OK(result);
    struct kobox_linux_user_process_status status = {.size = sizeof(status)};
    PH_OK(program_status(process, &status));
    ph_number("user process launch", (uint64_t)(int64_t)status.launch_error);
    ph_number("user process exit", (uint64_t)(int64_t)status.exit_code);
    PH_CHECK(status.reaped && status.host_closed && status.exited && status.pid > 1);
    return status;
}

static int exercise(void *context, void *service) {
    (void)service; (void)context;
    struct kobox_linux_user_process_file file = {
        .path = "/usr/sbin/wpa_supplicant", .data = image.bytes,
        .size = image.length, .mode = 0755};
    const char *const argv[] = {file.path, "-v", NULL};
    const char *const envp[] = {"HOME=/", "LANG=C", NULL};
    struct kobox_linux_user_process_config config = {
        .size = sizeof(config), .vm_operations = &ph_vm_ops,
        .host_space = spaces[0], .start = KOBOX_X86_USER_START,
        .length = KOBOX_X86_USER_END - KOBOX_X86_USER_START,
        .files = &file, .file_count = 1, .argv = argv, .envp = envp};
    struct kobox_linux_user_process *process = NULL;
    config.capability_mask = 1ull << 21; /* CAP_SYS_ADMIN is never admitted. */
    PH_CHECK(spawn_program(&config, &process) == -EINVAL && !process);
    config.capability_mask = 0;
    file.path = "/../escape";
    PH_CHECK(spawn_program(&config, &process) == -EINVAL && !process);
    file.path = argv[0];
    file.mode = 0777;
    PH_CHECK(spawn_program(&config, &process) == -EINVAL && !process);
    file.mode = 0755;
    ph_log("USER_PROCESS_ADMISSION=PASS\n");
    PH_OK(spawn_program(&config, &process));
    struct kobox_linux_user_process_status status = finish(process);
    PH_CHECK(status.exec_ready && !status.launch_error && !status.exit_code && !status.term_signal);
    char log[1024] = {0};
    size_t count = 0;
    PH_OK(read_log(process, 0, log, sizeof(log) - 1, &count));
    PH_CHECK(count && count < sizeof(log));
    log[count] = 0;
    ph_log(log);
    const char expected[] = "wpa_supplicant v2.12";
    PH_CHECK(count >= sizeof(expected) - 1 && !memcmp(log, expected, sizeof(expected) - 1));
    PH_OK(destroy_program(process));
    ph_vm_destroy(spaces[0]);
    ph_log("USER_PROCESS_REAL_WPA_ELF=PASS\n");

#if PH_USER_WIFI_RADIO
    const char empty_configuration[] = "update_config=0\n";
    struct kobox_linux_user_process_file radio_files[] = {
        file, {.path="/etc/wpa_supplicant.conf", .data=empty_configuration,
            .size=sizeof(empty_configuration)-1, .mode=0600}};
    const char *const daemon_argv[] = {file.path, "-Dnl80211", "-i", "wlan0",
        "-c", radio_files[1].path, "-g", KOBOX_WIFI_CONTROL_ARGUMENT, NULL};
    config.files = radio_files; config.file_count = 2;
#else
    const char *const daemon_argv[] = {file.path, "-g", KOBOX_WIFI_CONTROL_ARGUMENT, NULL};
#endif
    config.host_space = spaces[3];
    config.argv = daemon_argv;
    config.capability_mask = (1ull << 12) | (1ull << 13);
    process = NULL;
    PH_OK(spawn_program(&config, &process));
    struct kobox_linux_wifi_control *control = NULL;
    int opened = control_open(process, &control, 5000);
    if (opened) {
        count = 0;
        PH_OK(read_log(process, 0, log, sizeof(log) - 1, &count));
        log[count] = 0;
        ph_log(log);
    }
    PH_OK(opened);
    count = 0;
    PH_OK(control_request(control, "PING", 4, log, sizeof(log), &count, 2000));
    PH_CHECK(count == 5 && !memcmp(log, "PONG\n", 5));
    PH_OK(control_request(control, "INTERFACES", 10, log, sizeof(log), &count, 2000));
#if PH_USER_WIFI_RADIO
    PH_CHECK(count == 6 && !memcmp(log, "wlan0\n", 6));
    const char status_command[] = "IFNAME=wlan0 STATUS";
    PH_OK(control_request(control, status_command, sizeof(status_command)-1,
        log, sizeof(log)-1, &count, 2000));
    log[count] = 0; ph_log(log);
    bool have_state = false;
    for (size_t i = 0; i + 10 <= count; ++i)
        if (!memcmp(log + i, "wpa_state=", 10)) have_state = true;
    PH_CHECK(have_state);
    ph_log("USER_PROCESS_NL80211_RADIO=PASS\n");
#else
    PH_CHECK(!count); /* No radio; no fabricated association. */
#endif
    PH_OK(control_request(control, "TERMINATE", 9, log, sizeof(log), &count, 2000));
    PH_CHECK(count == 3 && !memcmp(log, "OK\n", 3));
    PH_OK(control_close(control));
    status = finish(process);
    PH_CHECK(status.exec_ready && !status.launch_error && !status.exit_code);
    PH_OK(destroy_program(process));
    ph_vm_destroy(spaces[3]);
    ph_exec_release_image(&image);
    ph_log("USER_PROCESS_WPA_CONTROL=PASS\n");
    config.files = &file; config.file_count = 1;

    file.path = "/usr/bin/crypto-smoke";
    file.data = crypto_image.bytes;
    file.size = crypto_image.length;
    const char *const crypto_argv[] = {file.path, NULL};
    config.argv = crypto_argv;
    config.host_space = spaces[4];
    config.capability_mask = 0;
    process = NULL;
    PH_OK(spawn_program(&config, &process));
    status = finish(process);
    count = 0;
    PH_OK(read_log(process, 0, log, sizeof(log) - 1, &count));
    log[count] = 0;
    ph_log(log);
    PH_CHECK(status.exec_ready && !status.launch_error && !status.exit_code && !status.term_signal);
    const char crypto_marker[] = "CRYPTO_SMOKE=PASS";
    bool crypto_pass = false;
    for (size_t i = 0; i + sizeof(crypto_marker) - 1 <= count; ++i)
        if (!memcmp(log + i, crypto_marker, sizeof(crypto_marker) - 1)) crypto_pass = true;
    PH_CHECK(crypto_pass);
    PH_OK(destroy_program(process));
    ph_vm_destroy(spaces[4]);
    ph_exec_release_image(&crypto_image);
    ph_log("USER_PROCESS_CRYPTO=PASS\n");

    const char bad[] = "not an ELF";
    file.path = argv[0];
    file.data = bad;
    file.size = sizeof(bad);
    config.host_space = spaces[1];
    config.argv = argv;
    config.capability_mask = 0;
    process = NULL;
    PH_OK(spawn_program(&config, &process));
    status = finish(process);
    PH_CHECK(!status.exec_ready && status.launch_error == -ENOEXEC && status.exit_code == 127);
    PH_OK(destroy_program(process));
    ph_vm_destroy(spaces[1]);
    ph_log("USER_PROCESS_EXEC_ROLLBACK=PASS\n");

    PH_OK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, spaces[2]->lifeline_fd));
    spaces[2]->lifeline_fd = -1;
    struct pacha_pollfd child = {.fd = spaces[2]->process_fd,
        .events = PACHA_FD_EVENT_READABLE};
    PH_CHECK(pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
        (uintptr_t)&child, 1, 5000, 0) == 1 && child.revents == PACHA_FD_EVENT_READABLE);
    ph_vm_destroy(spaces[2]);
    ph_log("USER_PROCESS_LIFELINE=PASS\n");
    return 0;
}

static int ready(void *context) { (void)context; return 0; }
static int pending(void *context) { (void)context; return 1; }
static int idle(void *context, void *service) { (void)context; (void)service; return 0; }

static void run(const struct kobox_linux_task_report *report, void *context) {
    (void)report; (void)context;
    PH_OK(ph_seed_linux_entropy());
    ph_log("USER_PROCESS_HOST_ENTROPY=PASS\n");
    struct kobox_linux_lifecycle lifecycle = {.size = sizeof(lifecycle),
        .context = &image, .ready = ready, .pending = pending,
        .start = exercise, .advance = idle, .finish = idle};
    struct kobox_linux_module_launch launch = {.size = sizeof(launch),
        .modules = modules, .count = WIFI_TEST_MODULES, .firmware = firmware,
        .firmware_count = firmware_count, .lifecycle = &lifecycle};
    struct kobox_linux_module_launch_report result = {.size = sizeof(result)};
    int (*launch_modules)(const struct kobox_linux_module_launch *,
        struct kobox_linux_module_launch_report *);
    RESOLVE(launch_modules, "kobox_linux_modules_run");
    PH_OK(launch_modules(&launch, &result));
    PH_CHECK(result.loaded == WIFI_TEST_MODULES && result.unloaded == WIFI_TEST_MODULES &&
        !result.result && !result.cleanup_result);
    ph_log("USER_PROCESS_MODULE_DRAIN=PASS\nUSER_PROCESS_SMOKE=PASS\n");
}

void ph_main(void) {
    size_t size;
    const void *core = ph_bootfs_core(&size);
    ph_core.ram_size = 512ul << 20;
    ph_boot_run(core, size, prepare, run, NULL);
}
