/* SPDX-License-Identifier: MIT */
#include "host.h"
#include "device.h"
#include "bootfs.h"
#include "boot/module_launch.h"
#include "../userland/seed0boot/src/bootstrap_abi.h"
#include <errno.h>

static struct ph_device device;
static uint64_t irq_cookie;
static atomic_uint tested;
static bool ready_seen;
static int (*snapshot)(struct kobox_linux_block_port *, size_t,
    struct kobox_linux_block_info *, size_t, size_t *, size_t *);
static int (*read_disk)(struct kobox_linux_block_port *,
    const struct kobox_linux_block_key *, uint64_t, void *, size_t);
static int (*write_disk)(struct kobox_linux_block_port *,
    const struct kobox_linux_block_key *, uint64_t, const void *, size_t);

#define RESOLVE(target, name) do { \
    void *symbol = ph_image_lookup(&ph_core, name); \
    PH_CHECK(symbol); memcpy(&(target), &symbol, sizeof(target)); \
} while (0)

void ph_device_gate_prepare(const struct kobox_linux_boot_layout *layout) {
    PH_CHECK(seed0_bootstrap_descriptor());
    uint64_t count = 0;
    PH_OK(pacha_syscall2(PACHA_CAPSULE_SYSCALL_PCI_ENUMERATE, UINT64_MAX, (uintptr_t)&count));
    int fd = -1;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t words[8] = {0};
        PH_CHECK(pacha_syscall3(PACHA_CAPSULE_SYSCALL_PCI_ENUMERATE, i,
            (uintptr_t)words, 8) == 8);
        if (words[4] == 0x010802) {
            PH_CHECK(fd < 0);
            fd = (int)pacha_syscall1(PACHA_CAPSULE_SYSCALL_PCI_CLAIM, i);
        }
    }
    PH_CHECK(fd >= 16);
    const struct ph_device_config config = {.device_fd=fd, .generation=1,
        .image=&ph_core, .memory=layout->task.memory, .irq_cookie_sequence=&irq_cookie};
    PH_OK(ph_device_init(&device, &config));
}

static int ready(void *context) {
    (void)context;
    ready_seen = true;
    return 0;
}

static int pending(void *context) {
    (void)context;
    return atomic_load_explicit(&tested, memory_order_acquire) ? 1 : 2;
}

static int dispatch(void *context, void *opaque) {
    (void)context;
    struct kobox_linux_io_service *service = opaque;
    struct kobox_linux_block_info info = {0};
    size_t count = 0, next = 0;
    PH_CHECK(ready_seen && service && service->block);
    PH_OK(snapshot(service->block, 0, &info, 1, &count, &next));
    PH_CHECK(count == 1 && !next && info.bytes == (16U << 20));
    PH_CHECK(!memcmp(info.name, "nvme", 4));
    struct kobox_linux_block_key key = {info.diskseq, info.major, info.minor};
    unsigned char *buffer = ph_alloc(PH_PAGE_SIZE);
    PH_CHECK(buffer);
    for (unsigned pass = 0; pass < 2; ++pass) {
        memset(buffer, 0xa5, PH_PAGE_SIZE);
        PH_OK(read_disk(service->block, &key,
            pass ? info.bytes - PH_PAGE_SIZE : 0, buffer, PH_PAGE_SIZE));
        for (size_t i = 0; i < PH_PAGE_SIZE; ++i) PH_CHECK(!buffer[i]);
    }
    /* A readable disk is not a write grant; do not actually write scratch or
     * backing media to establish that the production policy rejects writes.
     */
    PH_CHECK(write_disk(service->block, &key, 0, buffer, PH_PAGE_SIZE) == -EACCES);
    ph_free(buffer, PH_PAGE_SIZE);
    atomic_store_explicit(&tested, 1, memory_order_release);
    ph_log("PACHA_KOBOX_NVME_READ=PASS first-last-page write-denied\n");
    return 0;
}

void ph_verify_device_gate(void) {
    static const char *names[] = {"nvme_core", "nvme"};
    struct kobox_linux_native_module modules[2] = {0};
    int (*run)(const struct kobox_linux_module_launch *, struct kobox_linux_module_launch_report *);
    RESOLVE(run, "kobox_linux_modules_run");
    RESOLVE(snapshot, "kobox_linux_block_port_snapshot");
    RESOLVE(read_disk, "kobox_linux_block_port_read");
    RESOLVE(write_disk, "kobox_linux_block_port_write");
    for (unsigned i = 0; i < 2; ++i) {
        char path[96] = "/srv/kobox2/modules/";
        size_t offset = strlen(path);
        memcpy(path + offset, names[i], strlen(names[i]) + 1);
        offset += strlen(names[i]);
        memcpy(path + offset, ".ko", 4);
        modules[i].name = names[i];
        modules[i].image = ph_bootfs_file(path, &modules[i].length);
    }
    const struct kobox_linux_lifecycle lifecycle = {.size=sizeof(lifecycle),
        .context=&tested, .ready=ready, .pending=pending, .dispatch=dispatch};
    const struct kobox_linux_device_port_config pci = {.size=sizeof(pci),
        .pci=&device.pci.host, .dma=&device.dma.host, .irq=&device.irq.host};
    const struct kobox_linux_module_launch launch = {.size=sizeof(launch),
        .modules=modules, .count=2, .pci_devices=&pci, .pci_device_count=1,
        .capture_block=1, .lifecycle=&lifecycle};
    struct kobox_linux_module_launch_report report = {.size=sizeof(report)};
    PH_OK(run(&launch, &report));
    PH_CHECK(atomic_load(&tested) && !report.result && !report.cleanup_result &&
        report.loaded == 2 && report.unloaded == 2 && report.pci_bound && report.pci_detached);
    PH_OK(ph_device_finish(&device, 1));
    ph_log("PACHA_KOBOX_NVME=PASS probed read quiesce unload detach\n");
}
