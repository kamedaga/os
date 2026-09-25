/* SPDX-License-Identifier: MIT */
/* Boot-only device selection and verification, not gpud service policy. */
#include "host.h"
#include "device.h"
#include "bootfs.h"
#include "boot/virtio_gate.h"
#include "../seed0boot/src/bootstrap_abi.h"

static struct ph_device device;
static struct ph_device_config config;
static uint64_t irq_cookie_sequence;

/* Keep failure diagnostics outside the leaf implementation. PRINTK can be
 * disabled in the fixed core, so a failed driver probe otherwise loses the
 * native mapping error which caused it. This observer never changes policy. */
static int observe_mmio_map(void *context, void *address, uint64_t physical,
    size_t length, unsigned int protection, enum kobox_mmio_cache cache) {
    int result = device.pci.host.memory_map(context, address, physical, length, protection, cache);
    if (result) {
        ph_number("device MMIO failed address", (uintptr_t)address);
        ph_number("device MMIO failed physical", physical);
        ph_number("device MMIO failed cache", cache);
        ph_number("device MMIO failed result", result);
    }
    return result;
}

void ph_device_gate_prepare(const struct kobox_linux_boot_layout *layout) {
    PH_CHECK(seed0_bootstrap_descriptor() != NULL);
    uint64_t count = 0;
    PH_CHECK(pacha_syscall2(PACHA_CAPSULE_SYSCALL_PCI_ENUMERATE,
        UINT64_MAX, (uint64_t)(uintptr_t)&count) == 0);
    int gpu_fd = -1;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t words[8] = {0};
        PH_CHECK(pacha_syscall3(PACHA_CAPSULE_SYSCALL_PCI_ENUMERATE,
            i, (uint64_t)(uintptr_t)words, 8) == 8);
        if (words[1] == 0x1af4 && words[2] == 0x1050) {
            PH_CHECK(gpu_fd < 0);
            gpu_fd = (int)pacha_syscall1(PACHA_CAPSULE_SYSCALL_PCI_CLAIM, i);
        }
    }
    PH_CHECK(gpu_fd >= 16 && gpu_fd < PACHA_FD_TABLE_LIMIT);
    config = (struct ph_device_config){
        .device_fd = gpu_fd, .generation = 1,
        /* Linux topology is local. The native FD still limits every operation
         * to the one discovered physical function. */
        .segment = 0, .bus = 0, .devfn = 0,
        .irq_cookie_sequence = &irq_cookie_sequence, .image = &ph_core,
        .memory = layout->task.memory,
    };
    PH_OK(ph_device_init(&device, &config));
}

void ph_verify_device_gate(void) {
    static const struct {
        const char *file;
        const char *name;
    } module_names[] = {
        {"i2c-core.ko", "i2c_core"},
        {"drm_panel_orientation_quirks.ko", "drm_panel_orientation_quirks"},
        {"drm.ko", "drm"},
        {"drm_shmem_helper.ko", "drm_shmem_helper"},
        /* Native module metadata declares virtio -> virtio_ring. */
        {"virtio_ring.ko", "virtio_ring"},
        {"virtio.ko", "virtio"},
        {"virtio_pci_modern_dev.ko", "virtio_pci_modern_dev"},
        {"virtio_pci.ko", "virtio_pci"},
        {"virtio_dma_buf.ko", "virtio_dma_buf"},
        {"drm_kms_helper.ko", "drm_kms_helper"},
        {"virtio-gpu.ko", "virtio_gpu"},
    };
    struct kobox_linux_native_module modules[sizeof(module_names) / sizeof(module_names[0])];
    int (*verify)(const struct kobox_linux_virtio_test *, struct kobox_linux_virtio_report *);
    void *symbol = ph_image_lookup(&ph_core, "kobox_linux_virtio_verify");
    PH_CHECK(symbol);
    memcpy(&verify, &symbol, sizeof(verify));
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i) {
        char path[128] = "/srv/kobox2/modules/";
        size_t prefix = strlen(path), length = strlen(module_names[i].file);
        PH_CHECK(prefix + length < sizeof(path));
        memcpy(path + prefix, module_names[i].file, length + 1);
        modules[i] = (struct kobox_linux_native_module){.name = module_names[i].name};
        modules[i].image = ph_bootfs_file(path, &modules[i].length);
    }
    for (unsigned int round = 0; round < 2; ++round) {
        if (round) {
            ++config.generation;
            PH_OK(ph_device_init(&device, &config));
        }
        struct kobox_linux_virtio_report report = {.size = sizeof(report)};
        struct kobox_linux_pci_host pci = device.pci.host;
        pci.memory_map = observe_mmio_map;
        struct kobox_linux_virtio_test test = {
            .size = sizeof(test), .pci = &pci, .dma = &device.dma.host,
            .irq = &device.irq.host, .modules = modules,
            .count = sizeof(modules) / sizeof(modules[0]),
        };
        ph_number("device Gate round", round + 1);
        int result = verify(&test, &report);
        ph_number("device phase", report.phase);
        ph_number("device line", report.line);
        ph_number("device result", report.result);
        ph_number("device cleanup", report.cleanup);
        ph_number("device modules loaded", report.loaded);
        ph_number("device modules unloaded", report.unloaded);
        ph_number("device bound", report.bound);
        ph_number("device vectors", report.vectors);
        ph_number("device DRM nodes", report.nodes);
        ph_number("device Linux IRQs", report.interrupts);
        ph_number("device drained", report.drained);
        ph_number("device warnings", report.warnings);
        report.diagnostics[sizeof(report.diagnostics) - 1] = 0;
        if (report.diagnostics[0]) ph_log(report.diagnostics);
        PH_OK(result);
        PH_CHECK(!report.result && !report.cleanup && !report.warnings && report.bound &&
            report.vectors == 3 && report.nodes == 2 && report.interrupts && report.drained &&
            report.loaded == test.count && report.unloaded == report.loaded);
        PH_OK(ph_device_finish(&device, config.generation));
    }
    ph_log("PACHA_KOBOX_DEVICE=PASS\n");
}
