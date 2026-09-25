/* SPDX-License-Identifier: MIT */
#include "foundation.h"
#include "host.h"
#include "lifecycle.h"
#include "lifecycle_message.h"
#include "module_package.h"
#include "package.h"
#include "sandbox_config.h"
#include "boot.h"
#include "usb_input_service.h"
#include "net_frame_service.h"

#ifndef PH_SANDBOX_DEVICE
#define PH_SANDBOX_DEVICE 1
#endif
#ifndef PH_SANDBOX_USB_HID
#define PH_SANDBOX_USB_HID 0
#endif
#ifndef PH_SANDBOX_NET
#define PH_SANDBOX_NET 0
#endif
#if PH_SANDBOX_DEVICE
#include "device.h"
#include "device_grant.h"
#if !PH_SANDBOX_USB_HID && !PH_SANDBOX_NET
#include "gpu_queue.h"
#include <kobox2/virtqueue_x86_64.h>
#endif
#endif
#include "arch/x86_64/elf.h"
#include <errno.h>

static struct ph_ipc bootstrap_ipc;
static struct ph_bootstrap_receiver bundle;
static struct ph_package package;
static struct ph_module_package modules;
static struct ph_lifecycle lifecycle;
#if PH_SANDBOX_USB_HID
static struct ph_usb_input_service usb_input;
#endif
#if PH_SANDBOX_NET
static struct ph_net_frame_service net_frames;

static void report_network_progress(void *context, unsigned phase,
    size_t module_index, int status) {
    struct ph_ipc *ipc = context;
    if (!ipc->admitted ||
        atomic_load_explicit(&lifecycle.start, memory_order_acquire)) return;
    struct ph_ipc_packet progress = {
        .operation = PH_LIFECYCLE_PROGRESS,
        .generation = ipc->generation,
        .correlation = phase,
        .value = ((uint64_t)(uint32_t)status << 32) |
            (uint32_t)module_index,
    };
    /* Diagnostics cannot wait on a full channel while holding Linux's
     * module-init task; the owner consumes these packets concurrently. */
    (void)ph_ipc_send(ipc, &progress);
}

static void report_network_exception(const struct pacha_native_fault_frame *fault) {
    if (!bootstrap_ipc.admitted ||
        atomic_load_explicit(&lifecycle.start, memory_order_acquire)) return;
    const uintptr_t ip = (uintptr_t)fault->signal.rip;
    const uintptr_t base = (uintptr_t)ph_core.base;
    const unsigned core_relative = ip >= base && ip - base < ph_core.size;
    struct ph_ipc_packet failure = {
        .operation = ph_lifecycle_fault_operation((unsigned)fault->vector,
            (unsigned)fault->error_code, core_relative),
        .generation = bootstrap_ipc.generation,
        .correlation = core_relative ? ip - base : ip,
        .value = fault->address,
    };
    (void)ph_ipc_send(&bootstrap_ipc, &failure);
}

static void report_network_fatal(const char *file, unsigned line, uint64_t result) {
    if (!bootstrap_ipc.admitted ||
        atomic_load_explicit(&lifecycle.start, memory_order_acquire)) return;
    const char *base = file;
    for (const char *cursor = file; *cursor; ++cursor)
        if (*cursor == '/') base = cursor + 1;
    uint64_t prefix = 0;
    for (unsigned i = 0; i < 8 && base[i]; ++i)
        prefix |= (uint64_t)(unsigned char)base[i] << (i * 8);
    struct ph_ipc_packet failure = {
        .operation = PH_LIFECYCLE_READY,
        .generation = bootstrap_ipc.generation,
        .correlation = prefix,
        .value = ((uint64_t)line << 32) | (uint32_t)result,
    };
    (void)ph_ipc_send(&bootstrap_ipc, &failure);
}
#endif

#if PH_SANDBOX_DEVICE
static struct ph_device_grant device_grant;
static struct ph_device device;
static uint64_t irq_cookie_sequence;
#if PH_SANDBOX_USB_HID || PH_SANDBOX_NET
static struct kobox_linux_device_port_config pci_device_launch;
#else
static struct kobox_linux_device_launch device_launch;
static struct ph_gpu_queue gpu_queue;
static struct gpud_gpu_sessions gpu_sessions;
static uint64_t client_id;
static uint64_t gpu_channel_id;
#endif

static void prepare_device(const struct kobox_linux_boot_layout *layout, void *context) {
    (void)context;
    const struct ph_device_config config = {.device_fd = device_grant.fd,
                                            .generation = device_grant.generation,
                                            .image = &ph_core,
                                            .memory = layout->task.memory,
                                            .irq_cookie_sequence = &irq_cookie_sequence};
    PH_OK(ph_device_init(&device, &config));
#if PH_SANDBOX_USB_HID || PH_SANDBOX_NET
    pci_device_launch = (struct kobox_linux_device_port_config){
        .size = sizeof(pci_device_launch),
        .pci = &device.pci.host,
        .dma = &device.dma.host,
        .irq = &device.irq.host,
        .expected_class = PH_SANDBOX_NET ? 0x020000 : 0x0c0330,
    };
#else
    device_launch =
        (struct kobox_linux_device_launch){.size = sizeof(device_launch),
                                           .pci = &device.pci.host,
                                           .dma = &device.dma.host,
                                           .irq = &device.irq.host,
                                           .drm_events = &gpu_queue.event_host,
                                           .render_file_limit = GPUD_GPU_NATIVE_SESSION_LIMIT};
#endif
}
#endif

static uint64_t now(void) {
    uint64_t time[2];
    PH_OK(pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME, 1, (uintptr_t)time));
    return time[0] * UINT64_C(1000000000) + time[1];
}

static void cleanup_fatal(void *context, long status) {
    (void)context;
    ph_fail(__FILE__, __LINE__, status);
}

static void run_service(void *context) {
    (void)context;
    PH_OK(ph_module_package_open(&modules, &package));
#if PH_SANDBOX_DEVICE
#if PH_SANDBOX_USB_HID
    struct ph_lifecycle_service service;
    PH_OK(ph_usb_input_service_init(&usb_input, &ph_core,
        bootstrap_ipc.generation, &service));
    PH_OK(ph_lifecycle_start_service(&lifecycle, &bootstrap_ipc, &service));
#elif PH_SANDBOX_NET
    struct ph_lifecycle_service service;
    PH_OK(ph_net_frame_service_init(&net_frames, &ph_core,
        bootstrap_ipc.generation, &service));
    PH_OK(ph_lifecycle_start_service(&lifecycle, &bootstrap_ipc, &service));
#else
    struct ph_lifecycle_service service;
    PH_OK(ph_gpu_sessions_init(
        &gpu_sessions, bootstrap_ipc.generation, GPUD_GPU_NATIVE_SESSION_LIMIT));
    PH_OK(ph_gpu_queue_init(
        &gpu_queue, &gpu_sessions, client_id, gpu_channel_id, &kb2_vq_x86_64_atomics, &service));
    PH_OK(ph_lifecycle_start_service(&lifecycle, &bootstrap_ipc, &service));
#endif
#else
    PH_OK(ph_lifecycle_start(&lifecycle, &bootstrap_ipc));
#endif
    struct kobox_linux_module_launch launch = {.size = sizeof(launch),
                                               .modules = modules.modules,
                                               .count = modules.count,
                                               .lifecycle = &lifecycle.port};
#if PH_SANDBOX_DEVICE
#if PH_SANDBOX_USB_HID
    launch.pci_device = &pci_device_launch;
    launch.capture_input = 1;
#elif PH_SANDBOX_NET
    launch.pci_device = &pci_device_launch;
    launch.capture_network = 1;
    launch.progress = report_network_progress;
    launch.progress_context = &bootstrap_ipc;
#else
    launch.device = &device_launch;
#endif
#endif
    struct kobox_linux_module_launch_report report = {.size = sizeof(report)};
    int (*run)(const struct kobox_linux_module_launch *, struct kobox_linux_module_launch_report *);
    void *symbol = ph_image_lookup(&ph_core, "kobox_linux_modules_run");
    PH_CHECK(symbol);
    memcpy(&run, &symbol, sizeof(run));
    int result = run(&launch, &report);
    ph_lifecycle_finish(&lifecycle);
#if PH_SANDBOX_NET
    if (result) {
        /* The native owner has no serial console on a live machine. Report
         * the upstream module-launch errno before fatal process exit. */
        struct ph_ipc_packet failure = {
            .operation = PH_LIFECYCLE_READY,
            .generation = bootstrap_ipc.generation,
            .correlation = ((uint64_t)report.loaded << 1) |
                (report.pci_bound ? 1u : 0u),
            .value = (uint64_t)(int64_t)result,
        };
        (void)ph_ipc_send(&bootstrap_ipc, &failure);
    }
#endif
#if PH_SANDBOX_DEVICE
#if !PH_SANDBOX_USB_HID && !PH_SANDBOX_NET
    PH_OK(ph_gpu_queue_finish(&gpu_queue));
#endif
#endif
    ph_number("package modules loaded", report.loaded);
    ph_number("package modules unloaded", report.unloaded);
    ph_number("package module result", report.result);
    ph_number("package module cleanup", report.cleanup_result);
    PH_OK(result);
    PH_CHECK(report.loaded == modules.count && report.unloaded == modules.count && !report.result &&
             !report.cleanup_result && lifecycle.token);
#if PH_SANDBOX_DEVICE
#if PH_SANDBOX_USB_HID || PH_SANDBOX_NET
    PH_CHECK(report.pci_bound && report.pci_detached);
#else
    PH_CHECK(report.device.bound && report.device.primary_major && report.device.render_major &&
             report.device.drained && modules.count == 12 && report.device.render_opened &&
             report.device.drm_queried &&
             report.device.render_closed);
    PH_CHECK(!gpu_queue.mapping);
#ifdef PH_SANDBOX_FOUNDATION_GATES
    PH_CHECK(report.device.drm_checks == 6);
    ph_log("NATIVE_GPUD_DRM_FILE=PASS render-open version get-cap bounded-output sync-close\n");
    PH_CHECK(gpu_queue.service.command_count == 7);
    PH_CHECK(report.device.files.opened == 3 && report.device.files.closed == 3 &&
             !report.device.files.active && report.device.files.peak == 2 &&
             report.device.files.distinct_checks >= 2 && !report.device.files.close_error);
    ph_log("NATIVE_GPUD_GPU_SESSION=PASS independent-files close-one quota quiesce-close-all\n");
    ph_log("NATIVE_GPUD_GPU_DISPATCH=PASS private-command owner-task real-ioctl "
           "canonical-completion\n");
#endif
#endif
    PH_OK(ph_device_finish(&device, device_grant.generation));
    PH_OK(ph_device_grant_close(&device_grant, device_grant.generation));
#if PH_SANDBOX_USB_HID
    ph_log("NATIVE_USB_HID_DEVICE_LIFECYCLE=PASS granted-pci bound unload detach\n");
#elif PH_SANDBOX_NET
    ph_log("NATIVE_NET_DEVICE_LIFECYCLE=PASS granted-pci bound unload detach\n");
#else
    ph_log("NATIVE_GPUD_DEVICE_LIFECYCLE=PASS verified-grant bind drm-nodes ready quiesce unload "
           "detach\n");
#endif
#endif
#if PH_SANDBOX_USB_HID
    ph_log("NATIVE_USB_HID_MODULE_LIFECYCLE=PASS verified-closure ready quiesce unload\n");
#elif PH_SANDBOX_NET
    ph_log("NATIVE_NET_MODULE_LIFECYCLE=PASS verified-closure ready quiesce unload\n");
#else
    ph_log(
        "NATIVE_GPUD_MODULE_LIFECYCLE=PASS verified-module ready control-event quiesce unload\n");
#endif
    /* The Linux core still uses package bytes and RAM. Native process exit,
     * not premature package_close(), retires this process-lifetime storage. */
}

#ifndef PH_SANDBOX_FOUNDATION_GATES
static void service_main(const struct kobox_linux_task_report *report, void *context) {
    (void)report;
    run_service(context);
}
#endif

void ph_main(void) {
    const struct ph_sandbox_config *config = (void *)PH_SANDBOX_CONFIG_ADDRESS;
    PH_CHECK(((PH_SANDBOX_USB_HID || PH_SANDBOX_NET) ?
              config->artifact_count >= 2 &&
              config->artifact_count <= PH_BOOTSTRAP_MAX_ARTIFACTS :
              config->artifact_count == (PH_SANDBOX_DEVICE ? 13u : 2u)) &&
             config->resource_count == (PH_SANDBOX_DEVICE ? 1u : 0u));
    PH_OK(ph_ipc_init(&bootstrap_ipc, PH_SANDBOX_CHANNEL_FD, config->identity.generation));
#if PH_SANDBOX_NET
    ph_set_failure_reporter(report_network_fatal);
    ph_set_exception_reporter(report_network_exception);
#endif
    PH_OK(ph_bootstrap_receiver_init(
        &bundle, config->identity.generation, config->artifact_count, config->resource_count));
    uint64_t deadline = now() + UINT64_C(10000000000);
    while (!bundle.complete) {
        int result = ph_bootstrap_receive_next(&bundle, &bootstrap_ipc);
        PH_CHECK(!result || result == -EAGAIN || result == -ENOMEM);
        PH_CHECK(now() < deadline);
    }
    PH_OK(ph_package_open(
        &package, &bundle, &config->identity, kobox_x86_64_elf_matches, cleanup_fatal, NULL));
#if PH_SANDBOX_DEVICE
#if !PH_SANDBOX_USB_HID && !PH_SANDBOX_NET
    client_id = config->client_id;
    gpu_channel_id = config->gpu_channel_id;
    PH_CHECK(client_id && gpu_channel_id);
#else
    PH_CHECK(!config->client_id && !config->gpu_channel_id);
#endif
    PH_OK(ph_device_grant_take(&device_grant, &bundle, &package, &config->device));
#if PH_SANDBOX_USB_HID || PH_SANDBOX_NET
    ph_log("NATIVE_USB_HID_DEVICE_GRANT=PASS native-identity attenuated-rights\n");
#else
    ph_log(
        "NATIVE_GPUD_DEVICE_GRANT=PASS native-identity attenuated-rights private-child-import\n");
#endif
#endif
    PH_OK(ph_bootstrap_release(&bundle));
    ph_log("NATIVE_GPUD_CORE_PACKAGE=PASS verified-private-snapshots inputs-closed\n");
    /* The package verifier selects the fixed core. Gate selection is a build
     * choice; the production sandbox enters the same upstream boot directly. */
#ifdef PH_SANDBOX_FOUNDATION_GATES
#if PH_SANDBOX_DEVICE
    ph_foundation_run_prepared(package.common.artifacts[0].data,
        package.common.artifacts[0].size, prepare_device, run_service, NULL);
#else
    ph_foundation_run(package.common.artifacts[0].data,
        package.common.artifacts[0].size, run_service, NULL);
#endif
#else
    ph_boot_run(package.common.artifacts[0].data, package.common.artifacts[0].size,
#if PH_SANDBOX_DEVICE
        prepare_device,
#else
        NULL,
#endif
        service_main, NULL);
#endif
}
