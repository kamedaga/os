#define _GNU_SOURCE

#include "kobox/device_pachaos_capsule.h"
#include "kobox/module.h"
#include "kobox/shim.h"
#include "linux_subsystem/block/block.h"
#include "pacha/capsule.h"
#include "pachaos_capsule/boot_config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *status_name(kb_status_t status)
{
    switch (status) {
    case KB_OK:
        return "KB_OK";
    case KB_ERR_INVALID:
        return "KB_ERR_INVALID";
    case KB_ERR_NOT_FOUND:
        return "KB_ERR_NOT_FOUND";
    case KB_ERR_DENIED:
        return "KB_ERR_DENIED";
    case KB_ERR_NOMEM:
        return "KB_ERR_NOMEM";
    case KB_ERR_IO:
        return "KB_ERR_IO";
    case KB_ERR_UNSUPPORTED:
        return "KB_ERR_UNSUPPORTED";
    case KB_ERR_PCI_CONFIG:
        return "KB_ERR_PCI_CONFIG";
    default:
        return "KB_ERR_UNKNOWN";
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const struct pachaos_capsule_boot_config *cfg =
        (const struct pachaos_capsule_boot_config *)(uintptr_t)PACHAOS_CAPSULE_BOOT_CONFIG_VA;
    if (cfg->magic != PACHAOS_CAPSULE_BOOT_CONFIG_MAGIC ||
        cfg->version != PACHAOS_CAPSULE_BOOT_CONFIG_VERSION ||
        cfg->device_fd < 16 ||
        cfg->module_count == 0 ||
        cfg->module_count > PACHAOS_CAPSULE_MAX_MODULES) {
        fprintf(stderr,
            "[pachaos_capsule] invalid boot config magic=0x%llx version=%llu fd=%llu modules=%llu\n",
            (unsigned long long)cfg->magic,
            (unsigned long long)cfg->version,
            (unsigned long long)cfg->device_fd,
            (unsigned long long)cfg->module_count);
        return 2;
    }

    printf("[pachaos_capsule] start device_fd=%llu modules=%llu loader=%s\n",
        (unsigned long long)cfg->device_fd,
        (unsigned long long)cfg->module_count,
        kb_module_loader_version());

    kb_device_backend_t *backend = NULL;
    kb_status_t status = kb_pachaos_capsule_device_create(cfg->device_fd, &backend);
    if (status != KB_OK || backend == NULL) {
        fprintf(stderr, "[pachaos_capsule] device backend create failed status=%d\n", status);
        return 3;
    }

    for (unsigned bar_index = 0; bar_index < 2; bar_index++) {
        struct pacha_capsule_bar_info bar = {0};
        int bar_status = pacha_capsule_pci_bar_info((int)cfg->device_fd, bar_index, &bar);
        printf("[pachaos_capsule] pci bar%u status=%d start=0x%llx end=0x%llx size=0x%llx flags=0x%llx\n",
            bar_index,
            bar_status,
            (unsigned long long)bar.start,
            (unsigned long long)bar.end,
            (unsigned long long)bar.size,
            (unsigned long long)bar.flags);
    }
    for (uint16_t offset = 0x10; offset <= 0x14; offset += 4) {
        uint32_t value = 0;
        int config_status = pacha_capsule_pci_config_read((int)cfg->device_fd, offset, 4, &value);
        printf("[pachaos_capsule] pci config[0x%02x] status=%d value=0x%08x\n",
            offset,
            config_status,
            value);
    }

    kb_module_t *modules[PACHAOS_CAPSULE_MAX_MODULES];
    memset(modules, 0, sizeof(modules));
    for (uint64_t i = 0; i < cfg->module_count; i++) {
        const struct pachaos_capsule_module_config *module_cfg = &cfg->modules[i];
        if (module_cfg->image_va == 0 || module_cfg->image_size == 0 || module_cfg->name[0] == '\0') {
            fprintf(stderr, "[pachaos_capsule] invalid module slot=%llu\n", (unsigned long long)i);
            return 4;
        }
        const kb_module_image_t image = {
            .data = (const void *)(uintptr_t)module_cfg->image_va,
            .size = (size_t)module_cfg->image_size,
            .name = module_cfg->name,
        };
        status = kb_module_open_image(&image, backend, &modules[i]);
        if (status != KB_OK || modules[i] == NULL) {
            fprintf(stderr, "[pachaos_capsule] %s open failed status=%s(%d)\n",
                module_cfg->name,
                status_name(status),
                status);
            return 4;
        }

        int init_result = 0;
        printf("[pachaos_capsule] %s init begin\n", module_cfg->name);
        fflush(stdout);
        status = kb_module_call_init(modules[i], &init_result);
        if (status == KB_ERR_NOT_FOUND && i + 1u < cfg->module_count) {
            printf("[pachaos_capsule] %s has no init_module\n", module_cfg->name);
            continue;
        }
        printf("[pachaos_capsule] %s init returned status=%s(%d) result=%d\n",
            module_cfg->name,
            status_name(status),
            status,
            init_result);
        fflush(stdout);
        if (status != KB_OK || init_result != 0) {
            fprintf(stderr,
                "[pachaos_capsule] %s init failed status=%s(%d) result=%d\n",
                module_cfg->name,
                status_name(status),
                status,
                init_result);
            return 5;
        }
    }

    printf("[pachaos_capsule] NVMe module stack loaded\n");
    kb_shim_set_device_backend(backend);
    for (unsigned i = 0; i < 2048; i++) {
        kb_run_deferred_work();
        (void)kb_handle_any_irq(0);
        if (kb_block_subsystem_first_registered_disk() != NULL) {
            break;
        }
    }
    void *disk = kb_block_subsystem_first_registered_disk();
    if (disk == NULL) {
        fprintf(stderr, "[pachaos_capsule] NVMe module stack registered no disk\n");
        return 6;
    }
    unsigned char sector[512];
    memset(sector, 0, sizeof(sector));
    int read_status = kb_block_subsystem_disk_read(disk, 0, sector, sizeof(sector));
    if (read_status != 0) {
        fprintf(stderr, "[pachaos_capsule] NVMe disk read sector0 failed status=%d\n", read_status);
        return 7;
    }
    printf("[pachaos_capsule] NVMe disk read sector0=%02x %02x %02x %02x OK\n",
        sector[0],
        sector[1],
        sector[2],
        sector[3]);

    enum { smoke_sector = 32 };
    unsigned char original[512];
    unsigned char pattern[512];
    unsigned char verify[512];
    memset(original, 0, sizeof(original));
    memset(pattern, 0, sizeof(pattern));
    memset(verify, 0, sizeof(verify));
    for (size_t i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (unsigned char)((i * 37u) ^ 0x5au);
    }

    read_status = kb_block_subsystem_disk_read(disk, smoke_sector, original, sizeof(original));
    if (read_status != 0) {
        fprintf(stderr, "[pachaos_capsule] NVMe disk read sector%u before write failed status=%d\n",
            smoke_sector,
            read_status);
        return 8;
    }
    int write_status = kb_block_subsystem_disk_write(disk, smoke_sector, pattern, sizeof(pattern));
    if (write_status != 0) {
        fprintf(stderr, "[pachaos_capsule] NVMe disk write sector%u failed status=%d\n",
            smoke_sector,
            write_status);
        return 9;
    }
    read_status = kb_block_subsystem_disk_read(disk, smoke_sector, verify, sizeof(verify));
    if (read_status != 0 || memcmp(pattern, verify, sizeof(pattern)) != 0) {
        fprintf(stderr, "[pachaos_capsule] NVMe disk readback sector%u failed status=%d match=%d\n",
            smoke_sector,
            read_status,
            memcmp(pattern, verify, sizeof(pattern)) == 0);
        return 10;
    }
    write_status = kb_block_subsystem_disk_write(disk, smoke_sector, original, sizeof(original));
    if (write_status != 0) {
        fprintf(stderr, "[pachaos_capsule] NVMe disk restore sector%u failed status=%d\n",
            smoke_sector,
            write_status);
        return 11;
    }
    printf("[pachaos_capsule] NVMe disk read/write sector%u=%02x %02x %02x %02x OK\n",
        smoke_sector,
        verify[0],
        verify[1],
        verify[2],
        verify[3]);
    fflush(stdout);
    fflush(stderr);
    for (;;) {
        __asm__ volatile("pause");
    }
}
