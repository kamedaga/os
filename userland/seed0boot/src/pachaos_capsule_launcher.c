#define _GNU_SOURCE

#include "pachaos_capsule_launcher.h"

#include "bootfs_reader.h"
#include "bootstrap_abi.h"
#include "next_stage_loader.h"
#include "pacha/capsule.h"
#include "pacha/ipc.h"
#include "pacha/launch.h"
#include "storage_boot/boot_config.h"
#include "filed/live_bootstrap.h"
#include "pacha/live_bootstrap.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static long find_nvme_index(void)
{
    const long count = pacha_capsule_pci_function_count();
    if (count < 0) return count;
    for (uint64_t i = 0; i < (uint64_t)count; i++) {
        struct pacha_capsule_pci_function device;
        const int status = pacha_capsule_pci_function_at(i, &device);
        if (status != 0) return status;
        if (device.class_code == 0x010802)
            return (long)i;
    }
    return -19;
}

static int create_inherited_vmo(const void *data, uint64_t size, const char *label)
{
    if (data == NULL || size == 0 || size > UINT64_MAX - 4095u) return -22;
    const uint64_t map_size = (size + 4095u) & ~4095ull;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_SET_FLAGS | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int fd = pacha_vmo_create(map_size, rights, 0);
    if (fd < 16) return fd;
    void *mapped = pacha_mmap(fd, map_size,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (mapped == NULL) {
        (void)pacha_fd_close(fd);
        return -5;
    }
    memset(mapped, 0, (size_t)map_size);
    memcpy(mapped, data, (size_t)size);
    (void)pacha_munmap(mapped, map_size);
    printf("[seed0boot] inherited VMO ready label=%s fd=%d size=%llu\n",
        label, fd, (unsigned long long)size);
    return fd;
}

static int create_inherited_readonly_vmo(
    const void *data, uint64_t size, const char *label)
{
    const int writable_fd = create_inherited_vmo(data, size, label);
    if (writable_fd < 16) return writable_fd;
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_SET_FLAGS | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_MAP_READ;
    const long readonly_fd = pacha_fd_fcntl(
        writable_fd, PACHA_FD_FCNTL_DUP, 16, rights);
    (void)pacha_fd_close(writable_fd);
    if (readonly_fd < 16) return (int)readonly_fd;
    return (int)readonly_fd;
}

int seed0_launch_storage_boot_nvme(int ready_channel_fd, int root_handoff_channel_fd)
{
    const struct seed0_init_descriptor_page *desc = seed0_bootstrap_descriptor();
    if (desc == NULL || ready_channel_fd < 16 || root_handoff_channel_fd < 16)
        return -22;
    const long nvme_index = find_nvme_index();
    if (nvme_index < 0) return (int)nvme_index;

    const unsigned char *daemon = NULL;
    uint32_t daemon_size = 0;
    int status = seed0_bootfs_open_file(
        "/srv/storage_boot.elf", &daemon, &daemon_size);
    if (status != 0) return status;

    if (desc->bootfs_archive.image_va == 0 || desc->bootfs_archive.size_bytes == 0)
        return -5;
    const int bootfs_fd = create_inherited_readonly_vmo(
        (const void *)(uintptr_t)desc->bootfs_archive.image_va,
        desc->bootfs_archive.size_bytes,
        "bootfs archive");
    if (bootfs_fd < 16) return bootfs_fd;

    const int device_fd = pacha_capsule_pci_claim((uint64_t)nvme_index);
    if (device_fd < 16) {
        (void)pacha_fd_close(bootfs_fd);
        return device_fd;
    }

    struct storage_boot_config config;
    memset(&config, 0, sizeof(config));
    config.magic = STORAGE_BOOT_CONFIG_MAGIC;
    config.version = STORAGE_BOOT_CONFIG_VERSION;
    config.device_fd = (uint64_t)(uint32_t)device_fd;
    config.ready_channel_fd = (uint64_t)(uint32_t)ready_channel_fd;
    config.root_handoff_channel_fd = (uint64_t)(uint32_t)root_handoff_channel_fd;
    config.bootfs_fd = (uint64_t)(uint32_t)bootfs_fd;
    config.bootfs_size = desc->bootfs_archive.size_bytes;
    const int config_fd = create_inherited_vmo(&config, sizeof(config),
        "storage_boot config");
    if (config_fd < 16) {
        (void)pacha_fd_close(device_fd);
        (void)pacha_fd_close(bootfs_fd);
        return config_fd;
    }

    struct seed0_loaded_process loaded;
    const struct pacha_process_fd_grant grants[] = {
        PACHA_LAUNCH_LOG_GRANTS(PACHA_FD_RIGHT_TRANSFER),
        PACHA_LAUNCH_GRANT(device_fd, device_fd,
            PACHA_LAUNCH_DEVICE_DRIVER | PACHA_FD_RIGHT_TRANSFER),
        PACHA_LAUNCH_GRANT(ready_channel_fd, ready_channel_fd,
            PACHA_LAUNCH_SIGNAL | PACHA_FD_RIGHT_TRANSFER),
        PACHA_LAUNCH_GRANT(root_handoff_channel_fd, root_handoff_channel_fd,
            PACHA_LAUNCH_SERVER | PACHA_LAUNCH_SIGNAL | PACHA_FD_RIGHT_TRANSFER),
        PACHA_LAUNCH_GRANT(bootfs_fd, bootfs_fd, PACHA_LAUNCH_BLOB),
        PACHA_LAUNCH_GRANT(config_fd, config_fd, PACHA_LAUNCH_BLOB),
    };
    status = seed0_load_elf_process(
        "/srv/storage_boot.elf", daemon, daemon_size,
        grants, sizeof(grants) / sizeof(grants[0]), &loaded);
    if (status == 0)
        status = seed0_start_process(&loaded, "/srv/storage_boot.elf", config_fd);
    (void)pacha_fd_close(config_fd);
    (void)pacha_fd_close(bootfs_fd);
    (void)pacha_fd_close(device_fd);
    return status;
}

int seed0_launch_live_filed(int ready_channel_fd, int unix_path_fd,
    int *out_endpoint_fd)
{
    if (ready_channel_fd < 16 || unix_path_fd < 16 ||
        out_endpoint_fd == NULL) return -22;
    *out_endpoint_fd = -1;
    const struct seed0_init_descriptor_page *desc = seed0_bootstrap_descriptor();
    if (desc == NULL || desc->bootfs_archive.image_va == 0 ||
        desc->bootfs_archive.size_bytes == 0) return -5;
    const unsigned char *image = NULL;
    uint32_t image_size = 0;
    int status = seed0_bootfs_open_file("/sbin/filed.elf", &image, &image_size);
    if (status != 0) return status;

    const int bootfs_fd = create_inherited_readonly_vmo(
        (const void *)(uintptr_t)desc->bootfs_archive.image_va,
        desc->bootfs_archive.size_bytes, "live bootfs archive");
    if (bootfs_fd < 16) return bootfs_fd;
    const uint64_t endpoint_rights = PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_CALL | PACHA_FD_RIGHT_TRANSFER;
    const int endpoint_fd = pacha_ipc_endpoint_create(endpoint_rights, 0);
    if (endpoint_fd < 16) {
        (void)pacha_fd_close(bootfs_fd);
        return endpoint_fd;
    }
    const filed_live_bootstrap_t config = {
        .magic = FILED_LIVE_BOOTSTRAP_MAGIC,
        .bootfs_fd = (uint64_t)(uint32_t)bootfs_fd,
        .bootfs_size = desc->bootfs_archive.size_bytes,
        .public_endpoint_fd = (uint64_t)(uint32_t)endpoint_fd,
        .ready_channel_fd = (uint64_t)(uint32_t)ready_channel_fd,
        .unix_path_fd = (uint64_t)(uint32_t)unix_path_fd,
    };
    const int config_fd = create_inherited_vmo(&config, sizeof(config),
        "live filed config");
    if (config_fd < 16) {
        (void)pacha_fd_close(endpoint_fd);
        (void)pacha_fd_close(bootfs_fd);
        return config_fd;
    }
    const struct pacha_process_fd_grant grants[] = {
        PACHA_LAUNCH_LOG_GRANTS(PACHA_FD_RIGHT_TRANSFER),
        PACHA_LAUNCH_GRANT(bootfs_fd, bootfs_fd, PACHA_LAUNCH_BLOB),
        PACHA_LAUNCH_GRANT(endpoint_fd, endpoint_fd,
            PACHA_LAUNCH_SERVER | PACHA_LAUNCH_CLIENT |
                PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER |
                PACHA_FD_RIGHT_SET_FLAGS),
        PACHA_LAUNCH_GRANT(ready_channel_fd, ready_channel_fd, PACHA_LAUNCH_SIGNAL),
        PACHA_LAUNCH_GRANT(unix_path_fd, unix_path_fd, PACHA_LAUNCH_SERVER),
        PACHA_LAUNCH_GRANT(config_fd, config_fd, PACHA_LAUNCH_BLOB),
    };
    struct seed0_loaded_process loaded;
    status = seed0_load_elf_process("/sbin/filed.elf", image, image_size,
        grants, sizeof(grants) / sizeof(grants[0]), &loaded);
    if (status == 0)
        status = seed0_start_process(&loaded, "/sbin/filed.elf", config_fd);
    (void)pacha_fd_close(config_fd);
    (void)pacha_fd_close(bootfs_fd);
    if (status != 0) {
        (void)pacha_fd_close(endpoint_fd);
        return status;
    }
    *out_endpoint_fd = endpoint_fd;
    return 0;
}

int seed0_launch_live_seed0root(int filed_endpoint_fd, int unix_path_fd,
    int root_handoff_fd, int power_channel_fd)
{
    const struct seed0_init_descriptor_page *desc = seed0_bootstrap_descriptor();
    if (desc == NULL || filed_endpoint_fd < 16 || unix_path_fd < 16 ||
        root_handoff_fd < 16 || power_channel_fd < 16) return -22;
    const unsigned char *image = NULL;
    uint32_t image_size = 0;
    int status = seed0_bootfs_open_file("/sbin/seed0root.elf", &image,
        &image_size);
    if (status != 0) return status;
    const pacha_live_root_bootstrap_t config = {
        .magic = PACHA_LIVE_ROOT_BOOTSTRAP_MAGIC,
        .version = PACHA_LIVE_ROOT_BOOTSTRAP_VERSION,
        .filed_endpoint_fd = (uint64_t)(uint32_t)filed_endpoint_fd,
        .unix_path_fd = (uint64_t)(uint32_t)unix_path_fd,
        .root_handoff_fd = (uint64_t)(uint32_t)root_handoff_fd,
        .power_channel_fd = (uint64_t)(uint32_t)power_channel_fd,
        .framebuffer_paddr = desc->primary_display.framebuffer_paddr,
        .framebuffer_size = desc->primary_display.framebuffer_size_bytes,
        .width = desc->primary_display.width,
        .height = desc->primary_display.height,
        .pitch = desc->primary_display.pitch,
    };
    const int config_fd = create_inherited_vmo(&config, sizeof(config),
        "live root config");
    if (config_fd < 16) return config_fd;
    const struct pacha_process_fd_grant grants[] = {
        PACHA_LAUNCH_LOG_GRANTS(0),
        PACHA_LAUNCH_GRANT(config_fd, config_fd, PACHA_LAUNCH_BLOB),
        PACHA_LAUNCH_GRANT(filed_endpoint_fd, filed_endpoint_fd,
            PACHA_LAUNCH_CLIENT | PACHA_FD_RIGHT_DUP |
                PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SET_FLAGS),
        PACHA_LAUNCH_GRANT(unix_path_fd, unix_path_fd,
            PACHA_LAUNCH_CLIENT | PACHA_FD_RIGHT_TRANSFER),
        PACHA_LAUNCH_GRANT(root_handoff_fd, root_handoff_fd,
            PACHA_LAUNCH_SERVER),
        PACHA_LAUNCH_GRANT(power_channel_fd, power_channel_fd,
            PACHA_LAUNCH_CLIENT | PACHA_FD_RIGHT_TRANSFER),
    };
    struct seed0_loaded_process loaded;
    status = seed0_load_elf_process("/sbin/seed0root.elf", image,
        image_size, grants, sizeof(grants) / sizeof(grants[0]), &loaded);
    if (status == 0)
        status = seed0_start_process(&loaded, "/sbin/seed0root.elf",
            config_fd);
    (void)pacha_fd_close(config_fd);
    return status;
}
