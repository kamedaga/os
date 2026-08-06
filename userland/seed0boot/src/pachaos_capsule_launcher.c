#define _GNU_SOURCE

#include "pachaos_capsule_launcher.h"

#include "bootfs_reader.h"
#include "bootstrap_abi.h"
#include "next_stage_loader.h"
#include "pacha/ipc.h"
#include "storage_boot/boot_config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    SEED0_QEMU_NVME_VENDOR_ID = 0x1b36,
    SEED0_QEMU_NVME_DEVICE_ID = 0x0010,
};

static int mark_inherit(int fd);

static const struct seed0_device_descriptor *find_nvme_device(
    const struct seed0_init_descriptor_page *desc)
{
    if (desc == NULL) return NULL;
    uint64_t count = desc->device_count;
    if (count > SEED0_INIT_MAX_DEVICE_DESCRIPTORS)
        count = SEED0_INIT_MAX_DEVICE_DESCRIPTORS;
    for (uint64_t i = 0; i < count; i++) {
        const struct seed0_device_descriptor *device = &desc->devices[i];
        if ((device->flags & SEED0_INIT_DEVICE_FLAG_PRESENT) != 0 &&
            seed0_fd_is_dynamic(device->init_device_fd) &&
            device->vendor_id == SEED0_QEMU_NVME_VENDOR_ID &&
            device->device_id == SEED0_QEMU_NVME_DEVICE_ID)
            return device;
    }
    return NULL;
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
    const int fd = pacha_vmo_create(map_size, rights, PACHA_FD_FLAG_INHERIT);
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
    if (mark_inherit((int)readonly_fd) != 0) {
        (void)pacha_fd_close((int)readonly_fd);
        return -13;
    }
    return (int)readonly_fd;
}

static int mark_inherit(int fd)
{
    if (fd < 16) return -22;
    return (int)pacha_fd_fcntl(fd, PACHA_FD_FCNTL_SET_FLAGS,
        PACHA_FD_FLAG_INHERIT, PACHA_FD_FLAG_INHERIT);
}

int seed0_launch_storage_boot_nvme(int ready_channel_fd, int root_handoff_channel_fd)
{
    const struct seed0_init_descriptor_page *desc = seed0_bootstrap_descriptor();
    const struct seed0_device_descriptor *nvme = find_nvme_device(desc);
    if (nvme == NULL || ready_channel_fd < 16 || root_handoff_channel_fd < 16)
        return -22;

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

    const int device_fd = (int)nvme->init_device_fd;
    if (mark_inherit(device_fd) != 0 || mark_inherit(ready_channel_fd) != 0 ||
        mark_inherit(root_handoff_channel_fd) != 0) {
        (void)pacha_fd_close(bootfs_fd);
        return -13;
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
        (void)pacha_fd_close(bootfs_fd);
        return config_fd;
    }

    struct seed0_loaded_process loaded;
    status = seed0_load_elf_process(
        "/srv/storage_boot.elf", daemon, daemon_size, &loaded);
    if (status == 0)
        status = seed0_start_process(&loaded, "/srv/storage_boot.elf", config_fd);
    (void)pacha_fd_close(config_fd);
    (void)pacha_fd_close(bootfs_fd);
    (void)pacha_fd_close(device_fd);
    return status;
}
