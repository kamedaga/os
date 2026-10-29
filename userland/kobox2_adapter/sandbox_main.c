/* SPDX-License-Identifier: MIT */
#include "foundation.h"
#include "host.h"
#include "lifecycle.h"
#include "lifecycle_message.h"
#include "module_package.h"
#include "package.h"
#include "sandbox_config.h"
#include "block_policy.h"
#include "boot.h"
#include "usb_input_service.h"
#include "net_frame_service.h"
#include "block_service.h"
#include "usb_storage_service.h"
#include "firmware_bundle.h"
#include "entropy.h"
#include "wifi_host.h"
#include "fs_queue.h"
#include "audio_service.h"
#include <kobox2/module_progress.h>
#ifndef PH_SANDBOX_PS2
#define PH_SANDBOX_PS2 0
#endif
#if PH_SANDBOX_PS2
#include "ps2_native.h"
#endif

#ifndef PH_SANDBOX_DEVICE
#define PH_SANDBOX_DEVICE 1
#endif
#ifndef PH_SANDBOX_USB_HID
#define PH_SANDBOX_USB_HID 0
#endif
#ifndef PH_SANDBOX_NET
#define PH_SANDBOX_NET 0
#endif
#ifndef PH_SANDBOX_WIFI
#define PH_SANDBOX_WIFI 0
#endif
#ifndef PH_SANDBOX_AMDGPU
#define PH_SANDBOX_AMDGPU 0
#endif
#ifndef PH_SANDBOX_BLOCK
#define PH_SANDBOX_BLOCK 0
#endif
#ifndef PH_SANDBOX_AUDIO
#define PH_SANDBOX_AUDIO 0
#endif
#ifndef PH_SANDBOX_FS
#define PH_SANDBOX_FS 0
#endif
#define PH_SANDBOX_VIRTIO_GPU \
    (PH_SANDBOX_DEVICE && !PH_SANDBOX_USB_HID && !PH_SANDBOX_NET && \
     !PH_SANDBOX_AMDGPU && !PH_SANDBOX_BLOCK && !PH_SANDBOX_FS && !PH_SANDBOX_AUDIO)
#define PH_SANDBOX_GPU (PH_SANDBOX_VIRTIO_GPU || PH_SANDBOX_AMDGPU)
#if PH_SANDBOX_DEVICE
#include "device.h"
#include "device_grant.h"
#if PH_SANDBOX_GPU || PH_SANDBOX_FS
#include "gpu_queue.h"
#include <kobox2/virtqueue_x86_64.h>
#endif
#endif
#include "arch/x86_64/elf.h"
#include <errno.h>
#include <string.h>

static struct ph_ipc bootstrap_ipc;
static struct ph_bootstrap_receiver bundle;
static struct ph_package package;
static struct ph_module_package modules;
static struct ph_lifecycle lifecycle;
#if PH_SANDBOX_AMDGPU || PH_SANDBOX_WIFI
static struct kobox_linux_firmware_file firmware_files[KOBOX_FIRMWARE_MAX_FILES];
static size_t firmware_count;
#endif
#if PH_SANDBOX_USB_HID || PH_SANDBOX_PS2
#if PH_SANDBOX_BLOCK
static struct ph_usb_storage_service usb_storage;
#else
static struct ph_usb_input_service usb_input;
#endif
#endif
#if PH_SANDBOX_AUDIO
static struct ph_audio_service audio_service;
#endif
#if PH_SANDBOX_NET
static struct ph_net_frame_service net_frames;
#endif
#if PH_SANDBOX_BLOCK && !PH_SANDBOX_USB_HID
static struct ph_block_service block_io;
#endif
#if PH_SANDBOX_BLOCK
static struct ph_block_write_policy block_write_policy;
#endif
#if PH_SANDBOX_FS
static struct ph_fs_queue fs_queue;
static struct kobox_linux_fs_mount_config fs_mount_config;
static struct ph_device_grant *storage_grants;
static struct ph_device *storage_devices;
static struct kobox_linux_device_port_config *storage_ports;
static size_t storage_device_count;
#endif

#if PH_SANDBOX_NET || PH_SANDBOX_AMDGPU || PH_SANDBOX_BLOCK || PH_SANDBOX_FS || PH_SANDBOX_AUDIO

#if PH_SANDBOX_AMDGPU
static void report_gpu_irq_ready(void);
#endif

static void report_device_progress(void *context, unsigned phase,
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
#if PH_SANDBOX_AMDGPU
    if (phase == KOBOX_MODULE_PROGRESS_LIFECYCLE_READY && !status)
        report_gpu_irq_ready();
#endif
}

static void report_device_exception(const struct pacha_native_fault_frame *fault) {
    const uintptr_t ip = (uintptr_t)fault->signal.rip;
    const uintptr_t base = (uintptr_t)ph_core.base;
    const unsigned core_relative = ip >= base && ip - base < ph_core.size;
    /* READY transfers channel ownership to the queue service. Keep runtime
     * diagnostics on the existing SEND-only log capability, not that queue. */
    ph_boot_log_fault(fault->vector, fault->error_code, core_relative,
        core_relative ? ip - base : ip, fault->address);
    if (!bootstrap_ipc.admitted ||
        atomic_load_explicit(&lifecycle.start, memory_order_acquire)) return;
    struct ph_ipc_packet failure = {
        .operation = ph_lifecycle_fault_operation((unsigned)fault->vector,
            (unsigned)fault->error_code, core_relative),
        .generation = bootstrap_ipc.generation,
        .correlation = core_relative ? ip - base : ip,
        .value = fault->address,
    };
    (void)ph_ipc_send(&bootstrap_ipc, &failure);
}

static void report_device_fatal(const char *file, unsigned line, uint64_t result) {
    const char *base = file;
    for (const char *cursor = file; *cursor; ++cursor)
        if (*cursor == '/') base = cursor + 1;
    uint64_t prefix = 0;
    for (unsigned i = 0; i < 8 && base[i]; ++i)
        prefix |= (uint64_t)(unsigned char)base[i] << (i * 8);
    ph_boot_log_fatal(prefix, line, result);
    if (!bootstrap_ipc.admitted ||
        atomic_load_explicit(&lifecycle.start, memory_order_acquire)) return;
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
#if !PH_SANDBOX_FS
static struct ph_device_grant device_grant;
static struct ph_device device;
#endif
static uint64_t irq_cookie_sequence;
#if PH_SANDBOX_AMDGPU
static void report_gpu_irq_ready(void) {
    uint64_t received, dispatched, acknowledged;
    if (!ph_irq_snapshot(&device.irq, &received, &dispatched, &acknowledged))
        ph_boot_log_irq_ready(received, dispatched, acknowledged);
}
#endif
#if PH_SANDBOX_FS
#elif PH_SANDBOX_USB_HID || PH_SANDBOX_NET || PH_SANDBOX_BLOCK || PH_SANDBOX_AUDIO
static struct kobox_linux_device_port_config pci_device_launch;
#else
static struct kobox_linux_device_launch device_launch;
#if PH_SANDBOX_GPU
static struct ph_gpu_queue gpu_queue;
static struct gpud_gpu_sessions gpu_sessions;
static uint64_t client_id;
static uint64_t gpu_channel_id;
#endif
#endif

static void prepare_device(const struct kobox_linux_boot_layout *layout, void *context) {
    (void)context;
#if PH_SANDBOX_WIFI
    ph_wifi_host_prepare();
#endif
#if PH_SANDBOX_FS
    /* Every native grant has its own DMA/IRQ port and synthetic PCI domain.
     * The domain is transport topology, never a guessed hardware identity. */
    for (size_t i = 0; i < storage_device_count; ++i) {
        const struct ph_device_config config = {
            .device_fd = storage_grants[i].fd,
            .generation = storage_grants[i].generation, .segment = (uint32_t)i,
            .image = &ph_core, .memory = layout->task.memory,
            .irq_cookie_sequence = &irq_cookie_sequence};
        PH_OK(ph_device_init(&storage_devices[i], &config));
        storage_ports[i] = (struct kobox_linux_device_port_config){
            .size = sizeof(*storage_ports), .pci = &storage_devices[i].pci.host,
            .dma = &storage_devices[i].dma.host, .irq = &storage_devices[i].irq.host};
    }
#else
    const struct ph_device_config config = {.device_fd = device_grant.fd,
                                            .generation = device_grant.generation,
                                            .image = &ph_core,
                                            .memory = layout->task.memory,
                                            .irq_cookie_sequence = &irq_cookie_sequence};
    PH_OK(ph_device_init(&device, &config));
#if PH_SANDBOX_USB_HID || PH_SANDBOX_NET || PH_SANDBOX_BLOCK || PH_SANDBOX_AUDIO || PH_SANDBOX_FS
    pci_device_launch = (struct kobox_linux_device_port_config){
        .size = sizeof(pci_device_launch),
        .pci = &device.pci.host,
        .dma = &device.dma.host,
        .irq = &device.irq.host,
        .expected_class = PH_SANDBOX_USB_HID ? 0x0c0330 :
            PH_SANDBOX_NET ? (PH_SANDBOX_WIFI ? 0x028000 : 0x020000) :
            PH_SANDBOX_AUDIO ? 0x040100 : 0x010802,
    };
#else
    device_launch =
        (struct kobox_linux_device_launch){.size = sizeof(device_launch),
                                           .pci = &device.pci.host,
                                           .dma = &device.dma.host,
                                           .irq = &device.irq.host};
#if PH_SANDBOX_GPU
    device_launch.drm_events = &gpu_queue.event_host;
    device_launch.render_file_limit = GPUD_GPU_NATIVE_SESSION_LIMIT;
#endif
#endif
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
#if PH_SANDBOX_WIFI
    PH_OK(ph_seed_linux_entropy());
#endif
    PH_OK(ph_module_package_open(&modules, &package));
#if PH_SANDBOX_PS2
    for (size_t i = 0; i < modules.count; ++i)
        if (!strcmp(modules.modules[i].name, "i8042"))
            modules.modules[i].parameters = "noaux=1";
    struct ph_lifecycle_service ps2_service;
    PH_OK(ph_usb_input_service_init(&usb_input, &ph_core,
        bootstrap_ipc.generation, &ps2_service));
    PH_OK(ph_lifecycle_start_service(&lifecycle, &bootstrap_ipc, &ps2_service));
#endif
#if PH_SANDBOX_AMDGPU
    /* Firmware class reports the names requested by the unmodified Linux
     * driver, including failed lookups. The staged-file log alone cannot
     * establish which blobs the GPU actually requested. */
    size_t firmware_loader_count = 0;
    size_t drm_debug_count = 0;
    for (size_t i = 0; i < modules.count; ++i) {
        if (!strcmp(modules.modules[i].name, "firmware_class")) {
            modules.modules[i].parameters = "dyndbg=+p";
            firmware_loader_count++;
        } else if (!strcmp(modules.modules[i].name, "drm")) {
            /* DRM_UT_CORE prints every ioctl during sustained rendering.
             * The 7 KiB RAM service ring then loses thermal and stop events;
             * keep normal driver errors and the explicit inspection samples. */
            modules.modules[i].parameters = "debug=0";
            drm_debug_count++;
        }
    }
    PH_CHECK(firmware_loader_count == 1 && drm_debug_count == 1);
#endif
#if PH_SANDBOX_DEVICE
#if PH_SANDBOX_FS
    struct ph_lifecycle_service service;
    const struct ph_sandbox_config *config = (void *)PH_SANDBOX_CONFIG_ADDRESS;
    struct ph_fs_channel_config data = {
        .generation = bootstrap_ipc.generation, .channel_id = config->fs_channel_id,
        .arena_bytes = config->fs_arena_bytes, .queue_size = config->fs_queue_size};
    struct ph_fs_channel_config owner = data;
    owner.channel_id = config->fs_owner_channel_id;
    PH_OK(ph_fs_queue_init(&fs_queue, &ph_core, &data, &owner,
        &kb2_vq_x86_64_atomics, &service));
    PH_OK(ph_lifecycle_start_service(&lifecycle, &bootstrap_ipc, &service));
#elif PH_SANDBOX_USB_HID
    struct ph_lifecycle_service service;
#if PH_SANDBOX_BLOCK
    PH_OK(ph_usb_storage_service_init(&usb_storage, &ph_core,
        bootstrap_ipc.generation, &service));
#else
    PH_OK(ph_usb_input_service_init(&usb_input, &ph_core,
        bootstrap_ipc.generation, &service));
#endif
    PH_OK(ph_lifecycle_start_service(&lifecycle, &bootstrap_ipc, &service));
#elif PH_SANDBOX_BLOCK
    struct ph_lifecycle_service service;
    PH_OK(ph_block_service_init(&block_io, &ph_core,
        bootstrap_ipc.generation, &service));
    PH_OK(ph_lifecycle_start_service(&lifecycle, &bootstrap_ipc, &service));
#elif PH_SANDBOX_AUDIO
    struct ph_lifecycle_service service;
    PH_OK(ph_audio_service_init(&audio_service, &ph_core,
        bootstrap_ipc.generation, &service));
    PH_OK(ph_lifecycle_start_service(&lifecycle, &bootstrap_ipc, &service));
#elif PH_SANDBOX_NET
    struct ph_lifecycle_service service;
    PH_OK(ph_net_frame_service_init(&net_frames, &ph_core,
        bootstrap_ipc.generation, &service));
#if PH_SANDBOX_WIFI
    PH_OK(ph_lifecycle_start_monitored_service(&lifecycle, &bootstrap_ipc, &service));
#else
    PH_OK(ph_lifecycle_start_service(&lifecycle, &bootstrap_ipc, &service));
#endif
#elif PH_SANDBOX_AMDGPU
    struct ph_lifecycle_service service;
    PH_OK(ph_gpu_sessions_init(
        &gpu_sessions, bootstrap_ipc.generation, GPUD_GPU_NATIVE_SESSION_LIMIT));
    PH_OK(ph_gpu_queue_init_profile(&gpu_queue, &gpu_sessions, client_id,
        gpu_channel_id, KB2_GPU_PROFILE_AMDGPU, &kb2_vq_x86_64_atomics, &service));
    gpu_queue.service.query.pci = &device.pci;
    /* The physical GPU retains the sensor/time abort even while its common
     * DRM file service is answering render-node requests. */
    PH_OK(ph_lifecycle_start_monitored_service(&lifecycle, &bootstrap_ipc, &service));
#else
    struct ph_lifecycle_service service;
    PH_OK(ph_gpu_sessions_init(
        &gpu_sessions, bootstrap_ipc.generation, GPUD_GPU_NATIVE_SESSION_LIMIT));
    PH_OK(ph_gpu_queue_init(
        &gpu_queue, &gpu_sessions, client_id, gpu_channel_id, &kb2_vq_x86_64_atomics, &service));
    PH_OK(ph_lifecycle_start_service(&lifecycle, &bootstrap_ipc, &service));
#endif
#else
    if (!PH_SANDBOX_PS2) PH_OK(ph_lifecycle_start(&lifecycle, &bootstrap_ipc));
#endif
    struct kobox_linux_module_launch launch = {.size = sizeof(launch),
                                               .modules = modules.modules,
                                               .count = modules.count,
                                               .lifecycle = &lifecycle.port};
#if PH_SANDBOX_PS2
    launch.capture_input = 1;
#endif
#if PH_SANDBOX_DEVICE
#if PH_SANDBOX_FS
    launch.pci_devices = storage_ports;
    launch.pci_device_count = storage_device_count;
    launch.filesystem = &fs_mount_config;
    launch.progress = report_device_progress;
    launch.progress_context = &bootstrap_ipc;
#elif PH_SANDBOX_USB_HID
    launch.pci_devices = &pci_device_launch;
    launch.pci_device_count = 1;
    launch.capture_input = 1;
    launch.capture_block = PH_SANDBOX_BLOCK;
#if PH_SANDBOX_BLOCK
    launch.block_write = (uint32_t)block_write_policy.enabled;
    launch.block_write_key = (struct kobox_linux_block_key){
        .diskseq = block_write_policy.diskseq,
        .major = block_write_policy.major,
        .minor = block_write_policy.minor};
    launch.progress = report_device_progress;
    launch.progress_context = &bootstrap_ipc;
#endif
#elif PH_SANDBOX_BLOCK
    launch.pci_devices = &pci_device_launch;
    launch.pci_device_count = 1;
    launch.capture_block = 1;
    launch.block_write = (uint32_t)block_write_policy.enabled;
    launch.block_write_key = (struct kobox_linux_block_key){
        .diskseq = block_write_policy.diskseq,
        .major = block_write_policy.major,
        .minor = block_write_policy.minor};
    launch.progress = report_device_progress;
    launch.progress_context = &bootstrap_ipc;
#elif PH_SANDBOX_AUDIO
    launch.pci_devices = &pci_device_launch;
    launch.pci_device_count = 1;
    launch.progress = report_device_progress;
    launch.progress_context = &bootstrap_ipc;
#elif PH_SANDBOX_NET
    launch.pci_devices = &pci_device_launch;
    launch.pci_device_count = 1;
    launch.capture_network = 1;
#if PH_SANDBOX_WIFI
    launch.firmware = firmware_files;
    launch.firmware_count = firmware_count;
    launch.wireless = ph_wifi_host_config();
#endif
    launch.progress = report_device_progress;
    launch.progress_context = &bootstrap_ipc;
#elif PH_SANDBOX_AMDGPU
    launch.device = &device_launch;
    launch.firmware = firmware_files;
    launch.firmware_count = firmware_count;
    launch.progress = report_device_progress;
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
#if PH_SANDBOX_NET || PH_SANDBOX_AMDGPU || PH_SANDBOX_BLOCK || PH_SANDBOX_FS || PH_SANDBOX_AUDIO
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
#if PH_SANDBOX_FS
    PH_OK(ph_fs_queue_finish(&fs_queue));
#endif
#if PH_SANDBOX_GPU
    PH_OK(ph_gpu_queue_finish(&gpu_queue));
#endif
#endif
    ph_number("package modules loaded", report.loaded);
    ph_number("package modules unloaded", report.unloaded);
    ph_number("package module result", report.result);
    ph_number("package module cleanup", report.cleanup_result);
#if PH_SANDBOX_WIFI
    /* Failed Linux cleanup retains its binding and executable. Native owner
     * failure/reaping then retires PCI/DMA authority; never free it early. */
    if (!report.cleanup_result) ph_wifi_host_finish();
#endif
    PH_OK(result);
    PH_CHECK(report.loaded == modules.count && report.unloaded == modules.count && !report.result &&
             !report.cleanup_result && lifecycle.token);
#if PH_SANDBOX_DEVICE
#if PH_SANDBOX_USB_HID || PH_SANDBOX_NET || PH_SANDBOX_BLOCK || PH_SANDBOX_AUDIO || PH_SANDBOX_FS
    PH_CHECK(report.pci_bound && report.pci_detached);
#elif PH_SANDBOX_AMDGPU
    PH_CHECK(report.device.bound && report.device.primary_major &&
             report.device.render_major && report.device.render_opened &&
             report.device.drm_queried && report.device.render_closed &&
             report.device.drained);
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
#if PH_SANDBOX_FS
    PH_CHECK(report.pci_bound == storage_device_count &&
        report.pci_detached == storage_device_count);
    for (size_t i = storage_device_count; i--;) {
        PH_OK(ph_device_finish(&storage_devices[i], storage_grants[i].generation));
        PH_OK(ph_device_grant_close(&storage_grants[i], storage_grants[i].generation));
    }
#else
    PH_OK(ph_device_finish(&device, device_grant.generation));
    PH_OK(ph_device_grant_close(&device_grant, device_grant.generation));
#endif
#if PH_SANDBOX_FS
    ph_log("NATIVE_FS_DEVICE_LIFECYCLE=PASS granted-pci uuid-root vfs quiesce unmount detach\n");
#elif PH_SANDBOX_USB_HID
    ph_log("NATIVE_USB_DEVICE_LIFECYCLE=PASS granted-pci bound unload detach\n");
#elif PH_SANDBOX_BLOCK
    ph_log("NATIVE_BLOCK_DEVICE_LIFECYCLE=PASS granted-pci bound unload detach\n");
#elif PH_SANDBOX_AUDIO
    ph_log("NATIVE_AUDIO_LIFECYCLE=PASS verified-closure pcm-close unload detach\n");
#elif PH_SANDBOX_NET
    ph_log("NATIVE_NET_DEVICE_LIFECYCLE=PASS granted-pci bound unload detach\n");
#else
    ph_log("NATIVE_GPUD_DEVICE_LIFECYCLE=PASS verified-grant bind drm-nodes ready quiesce unload "
           "detach\n");
#endif
#endif
#if PH_SANDBOX_FS
    ph_log("NATIVE_FS_MODULE_LIFECYCLE=PASS verified-closure ready quiesce unload\n");
#elif PH_SANDBOX_USB_HID
    ph_log("NATIVE_USB_MODULE_LIFECYCLE=PASS verified-closure ready quiesce unload\n");
#elif PH_SANDBOX_BLOCK
    ph_log("NATIVE_BLOCK_MODULE_LIFECYCLE=PASS verified-closure ready quiesce unload\n");
#elif PH_SANDBOX_AUDIO
    ph_log("NATIVE_AUDIO_LIFECYCLE=PASS verified-closure pcm-close unload detach\n");
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

#if PH_SANDBOX_PS2
static void prepare_ps2(const struct kobox_linux_boot_layout *layout, void *context) {
    (void)layout; (void)context;
    PH_OK(ph_ps2_prepare(&ph_core));
}
#endif

void ph_main(void) {
    const struct ph_sandbox_config *config = (void *)PH_SANDBOX_CONFIG_ADDRESS;
    /* Reject a different launch extent before reading storage or device policy. */
    PH_CHECK(config->size == sizeof(*config));
#if PH_SANDBOX_WIFI
    PH_CHECK(config->ram_bytes >= (384ul << 20) && config->ram_bytes <= (2ul << 30));
    ph_core.ram_size = config->ram_bytes;
#endif
#if PH_SANDBOX_VIRTIO_GPU
    /* World geometry needs multiple concurrent 128 MiB GEM buffers. This
     * budget belongs to the GPU core, independently of guest RAM/JVM heap. */
    ph_core.ram_size = 1ul << 30;
    ph_log("virtio-gpu core RAM budget=1024 MiB\n");
#endif
#if PH_SANDBOX_FS
    PH_CHECK(config->fs_channel_id && config->fs_owner_channel_id &&
        config->fs_channel_id != config->fs_owner_channel_id &&
        config->fs_root_kind <= PH_FS_ROOT_TMPFS && config->ram_bytes && config->ram_bytes <= SIZE_MAX);
    ph_core.ram_size = (size_t)config->ram_bytes;
    fs_mount_config = (struct kobox_linux_fs_mount_config){
        .size = sizeof(fs_mount_config), .generation = config->identity.generation,
        .root_kind = config->fs_root_kind};
    memcpy(fs_mount_config.root_uuid, config->root_uuid, sizeof(config->root_uuid));
#endif
#if PH_SANDBOX_BLOCK
    const struct ph_block_write_policy *policy = (void *)PH_BLOCK_POLICY_ADDRESS;
    PH_CHECK(policy->magic == PH_BLOCK_POLICY_MAGIC &&
             policy->generation == config->identity.generation &&
             policy->native_device == config->device.native_device &&
             policy->enabled <= 1 &&
             (policy->enabled ? policy->diskseq != 0 :
              policy->diskseq == 0 && policy->major == 0 &&
              policy->minor == 0));
    block_write_policy = *policy;
#endif
    PH_CHECK(((PH_SANDBOX_USB_HID || PH_SANDBOX_NET || PH_SANDBOX_AMDGPU ||
               PH_SANDBOX_BLOCK || PH_SANDBOX_FS || PH_SANDBOX_AUDIO || PH_SANDBOX_PS2) ?
              config->artifact_count >= 2 &&
              config->artifact_count <= PH_BOOTSTRAP_MAX_ARTIFACTS :
              config->artifact_count == (PH_SANDBOX_DEVICE ? 13u : 2u)) &&
             config->resource_count == (PH_SANDBOX_FS ? config->storage_device_count :
                (PH_SANDBOX_DEVICE || PH_SANDBOX_PS2) ? 1u : 0u));
#if PH_SANDBOX_AMDGPU || PH_SANDBOX_WIFI
    PH_CHECK(config->log_endpoint_fd == 0 ||
             config->log_endpoint_fd == PH_SANDBOX_LOG_FD);
    ph_boot_set_log_fd((pacha_fd_t)config->log_endpoint_fd);
    PH_CHECK(config->firmware_address == PH_SANDBOX_FIRMWARE_ADDRESS &&
             config->firmware_size >= sizeof(struct ph_firmware_bundle) &&
             config->firmware_size <=
                 sizeof(struct ph_firmware_bundle) + PH_FIRMWARE_BUNDLE_MAX_BYTES);
    PH_OK(ph_firmware_bundle_open((const void *)(uintptr_t)config->firmware_address,
        (size_t)config->firmware_size, config->firmware_sha256,
        firmware_files, &firmware_count));
    ph_number("verified firmware files", firmware_count);
#else
    static const uint8_t absent_sha256[32];
    PH_CHECK(!config->log_endpoint_fd &&
             !config->firmware_address && !config->firmware_size &&
             !memcmp(config->firmware_sha256, absent_sha256, 32));
#endif
    PH_OK(ph_ipc_init(&bootstrap_ipc, PH_SANDBOX_CHANNEL_FD, config->identity.generation));
#if PH_SANDBOX_NET || PH_SANDBOX_AMDGPU || PH_SANDBOX_BLOCK || PH_SANDBOX_FS || PH_SANDBOX_AUDIO
    ph_set_failure_reporter(report_device_fatal);
    ph_set_exception_reporter(report_device_exception);
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
#if PH_SANDBOX_PS2
    PH_OK(ph_ps2_grant_take(&bundle, &package, &config->platform));
    ph_log("NATIVE_PS2_CONTROLLER_GRANT=PASS platform-PIO IRQ1 no-DMA\n");
#endif
#if PH_SANDBOX_DEVICE
#if PH_SANDBOX_GPU
    client_id = config->client_id;
    gpu_channel_id = config->gpu_channel_id;
    PH_CHECK(client_id && gpu_channel_id);
#else
    PH_CHECK(!config->client_id && !config->gpu_channel_id);
#endif
#if PH_SANDBOX_FS
    PH_CHECK((config->fs_root_kind == PH_FS_ROOT_TMPFS ?
        (!config->storage_devices_address && !config->storage_device_count) :
        (config->storage_devices_address == PH_SANDBOX_STORAGE_DEVICES_ADDRESS && config->storage_device_count)) &&
        config->storage_device_count <= UINT16_MAX + UINT64_C(1) &&
        config->storage_device_count <= SIZE_MAX / sizeof(*storage_devices));
    storage_device_count = (size_t)config->storage_device_count;
    const struct ph_device_authority *authorities =
        (const void *)(uintptr_t)config->storage_devices_address;
    storage_grants = storage_device_count ? ph_alloc(storage_device_count * sizeof(*storage_grants)) : NULL;
    storage_devices = storage_device_count ? ph_alloc(storage_device_count * sizeof(*storage_devices)) : NULL;
    storage_ports = storage_device_count ? ph_alloc(storage_device_count * sizeof(*storage_ports)) : NULL;
    /* Validate the complete set before any Linux or bus-master import. The
     * grant codec verifies logical uniqueness; native identities must also
     * be distinct so two ports cannot own one physical function. */
    for (size_t i = 0; i < storage_device_count; ++i) {
        for (size_t j = 0; j < i; ++j)
            PH_CHECK(authorities[j].native_device != authorities[i].native_device);
        PH_OK(ph_device_grant_take(&storage_grants[i], &bundle, &package, &authorities[i]));
    }
#else
    PH_OK(ph_device_grant_take(&device_grant, &bundle, &package, &config->device));
#endif
#if PH_SANDBOX_USB_HID || PH_SANDBOX_NET || PH_SANDBOX_BLOCK || PH_SANDBOX_AUDIO || PH_SANDBOX_FS
    ph_log(config->resource_count ? "NATIVE_PCI_DEVICE_GRANT=PASS native-identity attenuated-rights\n" :
        "NATIVE_RAM_FS_AUTHORITY=PASS no PCI capability\n");
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
#elif PH_SANDBOX_PS2
        prepare_ps2,
#else
        NULL,
#endif
        service_main, NULL);
#endif
}
