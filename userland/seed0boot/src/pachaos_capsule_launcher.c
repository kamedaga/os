#include "pachaos_capsule_launcher.h"

#include "bootfs_reader.h"
#include "bootstrap_abi.h"
#include "next_stage_loader.h"
#include "netd/boot_config.h"
#include "pacha/ipc.h"
#include "pachaos_capsule/boot_config.h"
#include "storage_boot/boot_config.h"
#include "termd/boot_config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    SEED0_QEMU_NVME_VENDOR_ID = 0x1b36,
    SEED0_QEMU_NVME_DEVICE_ID = 0x0010,
    SEED0_VIRTIO_VENDOR_ID = 0x1af4,
    SEED0_VIRTIO_NET_LEGACY_DEVICE_ID = 0x1000,
    SEED0_VIRTIO_NET_MODERN_DEVICE_ID = 0x1041,
    SEED0_VIRTIO_CONSOLE_LEGACY_DEVICE_ID = 0x1003,
    SEED0_VIRTIO_CONSOLE_MODERN_DEVICE_ID = 0x1043,
    SEED0_FILED_ENDPOINT_FD = 240,
    SEED0_NETD_SOCKET_ENDPOINT_FD = 241,
    SEED0_TERMD_TTY_ENDPOINT_FD = 242,
};

static const uint64_t seed0_netd_endpoint_rights =
    PACHA_FD_RIGHT_INSPECT |
    PACHA_FD_RIGHT_DUP |
    PACHA_FD_RIGHT_WAIT |
    PACHA_FD_RIGHT_POLL |
    PACHA_FD_RIGHT_SET_FLAGS |
    PACHA_FD_RIGHT_CLOSE |
    PACHA_FD_RIGHT_SEND |
    PACHA_FD_RIGHT_RECV |
    PACHA_FD_RIGHT_CALL |
    PACHA_FD_RIGHT_TRANSFER;

static const uint64_t seed0_termd_endpoint_rights =
    PACHA_FD_RIGHT_INSPECT |
    PACHA_FD_RIGHT_DUP |
    PACHA_FD_RIGHT_WAIT |
    PACHA_FD_RIGHT_POLL |
    PACHA_FD_RIGHT_SET_FLAGS |
    PACHA_FD_RIGHT_CLOSE |
    PACHA_FD_RIGHT_SEND |
    PACHA_FD_RIGHT_RECV |
    PACHA_FD_RIGHT_CALL |
    PACHA_FD_RIGHT_TRANSFER;

static const struct seed0_device_descriptor *find_nvme_device(const struct seed0_init_descriptor_page *desc)
{
    if (desc == 0) {
        return 0;
    }
    uint64_t count = desc->device_count;
    if (count > SEED0_INIT_MAX_DEVICE_DESCRIPTORS) {
        count = SEED0_INIT_MAX_DEVICE_DESCRIPTORS;
    }
    for (uint64_t i = 0; i < count; i++) {
        const struct seed0_device_descriptor *device = &desc->devices[i];
        if ((device->flags & SEED0_INIT_DEVICE_FLAG_PRESENT) == 0 ||
            !seed0_fd_is_dynamic(device->init_device_fd)) {
            continue;
        }
        if (device->vendor_id == SEED0_QEMU_NVME_VENDOR_ID &&
            device->device_id == SEED0_QEMU_NVME_DEVICE_ID) {
            return device;
        }
    }
    return 0;
}

static const struct seed0_device_descriptor *find_virtio_net_device(const struct seed0_init_descriptor_page *desc)
{
    if (desc == 0) {
        return 0;
    }
    uint64_t count = desc->device_count;
    if (count > SEED0_INIT_MAX_DEVICE_DESCRIPTORS) {
        count = SEED0_INIT_MAX_DEVICE_DESCRIPTORS;
    }
    for (uint64_t i = 0; i < count; i++) {
        const struct seed0_device_descriptor *device = &desc->devices[i];
        if ((device->flags & SEED0_INIT_DEVICE_FLAG_PRESENT) == 0 ||
            !seed0_fd_is_dynamic(device->init_device_fd)) {
            continue;
        }
        if (device->vendor_id == SEED0_VIRTIO_VENDOR_ID &&
            (device->device_id == SEED0_VIRTIO_NET_LEGACY_DEVICE_ID ||
             device->device_id == SEED0_VIRTIO_NET_MODERN_DEVICE_ID)) {
            return device;
        }
    }
    return 0;
}

static const struct seed0_device_descriptor *find_virtio_console_device(const struct seed0_init_descriptor_page *desc)
{
    if (desc == 0) {
        return 0;
    }
    uint64_t count = desc->device_count;
    if (count > SEED0_INIT_MAX_DEVICE_DESCRIPTORS) {
        count = SEED0_INIT_MAX_DEVICE_DESCRIPTORS;
    }
    for (uint64_t i = 0; i < count; i++) {
        const struct seed0_device_descriptor *device = &desc->devices[i];
        if ((device->flags & SEED0_INIT_DEVICE_FLAG_PRESENT) == 0 ||
            !seed0_fd_is_dynamic(device->init_device_fd)) {
            continue;
        }
        if (device->vendor_id == SEED0_VIRTIO_VENDOR_ID &&
            (device->device_id == SEED0_VIRTIO_CONSOLE_LEGACY_DEVICE_ID ||
             device->device_id == SEED0_VIRTIO_CONSOLE_MODERN_DEVICE_ID)) {
            return device;
        }
    }
    return 0;
}

struct seed0_capsule_module_spec {
    const char *path;
    const char *name;
};

static int seed0_create_inherited_vmo_from_bytes(const void *data, uint64_t size, const char *label)
{
    if (data == 0 || size == 0 || size > UINT64_MAX - 4095ull) {
        return -1;
    }
    const uint64_t map_size = (size + 4095ull) & ~4095ull;
    const uint64_t rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_READ |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int fd = pacha_vmo_create(map_size, rights, PACHA_FD_FLAG_INHERIT);
    if (fd < 16) {
        fprintf(stderr, "[seed0boot] %s: vmo_create failed status=%d\n", label, fd);
        return -2;
    }
    unsigned char *mapped = pacha_mmap(fd, map_size, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (mapped == 0) {
        (void)pacha_fd_close(fd);
        return -3;
    }
    memset(mapped, 0, (size_t)map_size);
    memcpy(mapped, data, (size_t)size);
    (void)pacha_munmap(mapped, map_size);
    return fd;
}

static int mark_device_inherit(int device_fd, const char *label)
{
    long flag_status = pacha_fd_fcntl(
        device_fd,
        PACHA_FD_FCNTL_SET_FLAGS,
        PACHA_FD_FLAG_INHERIT,
        PACHA_FD_FLAG_INHERIT);
    if (flag_status != 0) {
        fprintf(stderr, "[seed0boot] %s: mark device fd inherit failed fd=%d status=%ld\n",
            label,
            device_fd,
            flag_status);
        return -4;
    }
    return 0;
}

int seed0_launch_pachaos_capsule_nvme(void)
{
    static const struct seed0_capsule_module_spec module_specs[] = {
        {"/srv/kobox/nvme-auth.ko", "nvme-auth.ko"},
        {"/srv/kobox/nvme-core.ko", "nvme-core.ko"},
        {"/srv/kobox/nvme.ko", "nvme.ko"},
    };

    const struct seed0_init_descriptor_page *desc = seed0_bootstrap_descriptor();
    const struct seed0_device_descriptor *device = find_nvme_device(desc);
    if (device == 0) {
        fprintf(stderr, "[seed0boot] pachaos_capsule: NVMe device fd not found\n");
        return -1;
    }
    const int device_fd = (int)device->init_device_fd;

    const unsigned char *daemon_image = 0;
    uint32_t daemon_size = 0;
    int status = seed0_bootfs_open_file("/srv/pachaos_capsule.elf", &daemon_image, &daemon_size);
    if (status != 0) {
        fprintf(stderr, "[seed0boot] pachaos_capsule: open daemon from bootfs failed status=%d\n", status);
        return -2;
    }
    const unsigned char *module_images[PACHAOS_CAPSULE_MAX_MODULES];
    uint32_t module_sizes[PACHAOS_CAPSULE_MAX_MODULES];
    memset(module_images, 0, sizeof(module_images));
    memset(module_sizes, 0, sizeof(module_sizes));
    const uint64_t module_count = sizeof(module_specs) / sizeof(module_specs[0]);
    for (uint64_t i = 0; i < module_count; i++) {
        status = seed0_bootfs_open_file(module_specs[i].path, &module_images[i], &module_sizes[i]);
        if (status != 0) {
            fprintf(stderr, "[seed0boot] pachaos_capsule: open %s from bootfs failed status=%d\n",
                module_specs[i].path,
                status);
            return -3;
        }
        if ((uint64_t)module_sizes[i] > PACHAOS_CAPSULE_MODULE_IMAGE_STRIDE) {
            fprintf(stderr, "[seed0boot] pachaos_capsule: %s too large size=%u stride=%llu\n",
                module_specs[i].path,
                module_sizes[i],
                (unsigned long long)PACHAOS_CAPSULE_MODULE_IMAGE_STRIDE);
            return -3;
        }
    }

    status = mark_device_inherit(device_fd, "pachaos_capsule");
    if (status != 0) return status;

    struct seed0_loaded_process loaded;
    status = seed0_load_elf_process("/srv/pachaos_capsule.elf", daemon_image, daemon_size, &loaded);
    if (status != 0) {
        return status;
    }

    struct pachaos_capsule_boot_config config;
    memset(&config, 0, sizeof(config));
    config.magic = PACHAOS_CAPSULE_BOOT_CONFIG_MAGIC;
    config.version = PACHAOS_CAPSULE_BOOT_CONFIG_VERSION;
    config.device_fd = (uint64_t)(uint32_t)device_fd;
    config.module_count = module_count;
    config.workload = PACHAOS_CAPSULE_WORKLOAD_NVME;
    for (uint64_t i = 0; i < module_count; i++) {
        config.modules[i].image_va = PACHAOS_CAPSULE_MODULE_IMAGE_VA +
            (PACHAOS_CAPSULE_MODULE_IMAGE_STRIDE * i);
        config.modules[i].image_size = module_sizes[i];
        strncpy(config.modules[i].name, module_specs[i].name, sizeof(config.modules[i].name) - 1u);
    }

    status = seed0_map_bytes_into_process(
        loaded.process_fd,
        PACHAOS_CAPSULE_BOOT_CONFIG_VA,
        &config,
        sizeof(config),
        PACHA_PROT_READ);
    if (status != 0) {
        fprintf(stderr, "[seed0boot] pachaos_capsule: config map failed status=%d\n", status);
        return -5;
    }
    for (uint64_t i = 0; i < module_count; i++) {
        status = seed0_map_bytes_into_process(
            loaded.process_fd,
            config.modules[i].image_va,
            module_images[i],
            module_sizes[i],
            PACHA_PROT_READ);
        if (status != 0) {
            fprintf(stderr, "[seed0boot] pachaos_capsule: module map failed name=%s status=%d\n",
                module_specs[i].name,
                status);
            return -6;
        }
    }

    (void)device_fd;
    (void)daemon_size;
    printf("[seed0boot] pachaos_capsule modules=%llu\n",
        (unsigned long long)module_count);
    status = seed0_start_process(&loaded, "/srv/pachaos_capsule.elf", -1);
    if (status != 0) {
        return status;
    }
    return 0;
}

int seed0_launch_netd(int filed_endpoint_fd, int *out_socket_endpoint_fd)
{
    static const struct seed0_capsule_module_spec module_specs[] = {
        {"/srv/kobox/virtio.ko", "virtio.ko"},
        {"/srv/kobox/virtio_ring.ko", "virtio_ring.ko"},
        {"/srv/kobox/virtio_pci.ko", "virtio_pci.ko"},
        {"/srv/kobox/failover.ko", "failover.ko"},
        {"/srv/kobox/net_failover.ko", "net_failover.ko"},
        {"/srv/kobox/virtio_net.ko", "virtio_net.ko"},
    };

    if (out_socket_endpoint_fd != 0) {
        *out_socket_endpoint_fd = -1;
    }

    const struct seed0_init_descriptor_page *desc = seed0_bootstrap_descriptor();
    const struct seed0_device_descriptor *device = find_virtio_net_device(desc);
    if (device == 0) {
        printf("[seed0boot] netd: virtio-net device fd not found, skipping\n");
        return 0;
    }
    const int device_fd = (int)device->init_device_fd;
    if (filed_endpoint_fd < 16) {
        fprintf(stderr, "[seed0boot] netd: filed endpoint fd missing\n");
        return -1;
    }

    const unsigned char *daemon_image = 0;
    uint32_t daemon_size = 0;
    int status = seed0_bootfs_open_file("/srv/netd.elf", &daemon_image, &daemon_size);
    if (status != 0) {
        fprintf(stderr, "[seed0boot] netd: open daemon from bootfs failed status=%d\n", status);
        return -2;
    }
    const unsigned char *module_images[NETD_MAX_MODULES];
    uint32_t module_sizes[NETD_MAX_MODULES];
    memset(module_images, 0, sizeof(module_images));
    memset(module_sizes, 0, sizeof(module_sizes));
    const uint64_t module_count = sizeof(module_specs) / sizeof(module_specs[0]);
    for (uint64_t i = 0; i < module_count; i++) {
        status = seed0_bootfs_open_file(module_specs[i].path, &module_images[i], &module_sizes[i]);
        if (status != 0) {
            fprintf(stderr, "[seed0boot] netd: open %s from bootfs failed status=%d\n",
                module_specs[i].path,
                status);
            return -3;
        }
        if ((uint64_t)module_sizes[i] > NETD_MODULE_IMAGE_STRIDE) {
            fprintf(stderr, "[seed0boot] netd: %s too large size=%u stride=%llu\n",
                module_specs[i].path,
                module_sizes[i],
                (unsigned long long)NETD_MODULE_IMAGE_STRIDE);
            return -3;
        }
    }

    status = mark_device_inherit(device_fd, "netd");
    if (status != 0) return status;

    const long endpoint_dup = pacha_fd_fcntl(
        filed_endpoint_fd,
        PACHA_FD_FCNTL_DUP,
        SEED0_FILED_ENDPOINT_FD,
        PACHA_FD_RIGHT_INSPECT |
            PACHA_FD_RIGHT_DUP |
            PACHA_FD_RIGHT_WAIT |
            PACHA_FD_RIGHT_POLL |
            PACHA_FD_RIGHT_SET_FLAGS |
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_SEND |
            PACHA_FD_RIGHT_RECV |
            PACHA_FD_RIGHT_CALL |
            PACHA_FD_RIGHT_TRANSFER);
    if (endpoint_dup != SEED0_FILED_ENDPOINT_FD) {
        fprintf(stderr,
            "[seed0boot] netd: filed endpoint dup failed status=%ld target=%u\n",
            endpoint_dup,
            SEED0_FILED_ENDPOINT_FD);
        return -4;
    }
    status = mark_device_inherit(SEED0_FILED_ENDPOINT_FD, "netd filed endpoint");
    if (status != 0) return status;

    const int socket_endpoint_fd = pacha_ipc_endpoint_create(seed0_netd_endpoint_rights, 0);
    if (socket_endpoint_fd < 16) {
        fprintf(stderr, "[seed0boot] netd: socket endpoint create failed status=%d\n", socket_endpoint_fd);
        return socket_endpoint_fd < 0 ? socket_endpoint_fd : -2;
    }
    const long socket_endpoint_dup = pacha_fd_fcntl(
        socket_endpoint_fd,
        PACHA_FD_FCNTL_DUP,
        SEED0_NETD_SOCKET_ENDPOINT_FD,
        seed0_netd_endpoint_rights);
    if (socket_endpoint_dup != SEED0_NETD_SOCKET_ENDPOINT_FD) {
        fprintf(stderr,
            "[seed0boot] netd: socket endpoint dup failed status=%ld target=%u\n",
            socket_endpoint_dup,
            SEED0_NETD_SOCKET_ENDPOINT_FD);
        (void)pacha_fd_close(socket_endpoint_fd);
        return -4;
    }
    status = mark_device_inherit(SEED0_NETD_SOCKET_ENDPOINT_FD, "netd socket endpoint");
    if (status != 0) {
        (void)pacha_fd_close(socket_endpoint_fd);
        return status;
    }

    struct seed0_loaded_process loaded;
    status = seed0_load_elf_process("/srv/netd.elf", daemon_image, daemon_size, &loaded);
    if (status != 0) {
        (void)pacha_fd_close(socket_endpoint_fd);
        return status;
    }

    struct netd_boot_config config;
    memset(&config, 0, sizeof(config));
    config.magic = NETD_BOOT_CONFIG_MAGIC;
    config.device_fd = (uint64_t)(uint32_t)device_fd;
    config.socket_endpoint_fd = SEED0_NETD_SOCKET_ENDPOINT_FD;
    config.module_count = module_count;
    for (uint64_t i = 0; i < module_count; i++) {
        config.modules[i].image_va = NETD_MODULE_IMAGE_VA +
            (NETD_MODULE_IMAGE_STRIDE * i);
        config.modules[i].image_size = module_sizes[i];
        strncpy(config.modules[i].name, module_specs[i].name, sizeof(config.modules[i].name) - 1u);
    }

    status = seed0_map_bytes_into_process(
        loaded.process_fd,
        NETD_BOOT_CONFIG_VA,
        &config,
        sizeof(config),
        PACHA_PROT_READ);
    if (status != 0) {
        fprintf(stderr, "[seed0boot] netd: config map failed status=%d\n", status);
        return -5;
    }
    for (uint64_t i = 0; i < module_count; i++) {
        status = seed0_map_bytes_into_process(
            loaded.process_fd,
            config.modules[i].image_va,
            module_images[i],
            module_sizes[i],
            PACHA_PROT_READ);
        if (status != 0) {
            fprintf(stderr, "[seed0boot] netd: module map failed name=%s status=%d\n",
                module_specs[i].name,
                status);
            return -6;
        }
    }

    printf("[seed0boot] netd modules=%llu\n",
        (unsigned long long)module_count);
    status = seed0_start_process(&loaded, "/srv/netd.elf", -1);
    if (status != 0) {
        (void)pacha_fd_close(socket_endpoint_fd);
        return status;
    }
    (void)pacha_fd_close(SEED0_NETD_SOCKET_ENDPOINT_FD);
    if (out_socket_endpoint_fd != 0) {
        *out_socket_endpoint_fd = socket_endpoint_fd;
    } else {
        (void)pacha_fd_close(socket_endpoint_fd);
    }
    return 0;
}

int seed0_launch_termd(int *out_tty_endpoint_fd)
{
    static const struct seed0_capsule_module_spec module_specs[] = {
        {"/srv/kobox/linux_virtio.ko", "linux_virtio.ko"},
        {"/srv/kobox/linux_virtio_ring.ko", "linux_virtio_ring.ko"},
        {"/srv/kobox/linux_virtio_pci_modern_dev.ko", "linux_virtio_pci_modern_dev.ko"},
        {"/srv/kobox/linux_virtio_pci_legacy_dev.ko", "linux_virtio_pci_legacy_dev.ko"},
        {"/srv/kobox/linux_virtio_pci.ko", "linux_virtio_pci.ko"},
        {"/srv/kobox/linux_tty_core.ko", "linux_tty_core.ko"},
        {"/srv/kobox/linux_tty_n_null.ko", "linux_tty_n_null.ko"},
        {"/srv/kobox/linux_virtio_console.ko", "linux_virtio_console.ko"},
    };

    if (out_tty_endpoint_fd != 0) {
        *out_tty_endpoint_fd = -1;
    }

    const struct seed0_init_descriptor_page *desc = seed0_bootstrap_descriptor();
    const struct seed0_device_descriptor *device = find_virtio_console_device(desc);
    int device_fd = -1;
    if (device != 0) {
        device_fd = (int)device->init_device_fd;
        int inherit_status = mark_device_inherit(device_fd, "termd virtio-console");
        if (inherit_status != 0) {
            return inherit_status;
        }
    } else {
        printf("[seed0boot] termd: virtio-console device fd not found, using tty-only backend\n");
    }

    const unsigned char *daemon_image = 0;
    uint32_t daemon_size = 0;
    int status = seed0_bootfs_open_file("/srv/termd.elf", &daemon_image, &daemon_size);
    if (status != 0) {
        fprintf(stderr, "[seed0boot] termd: open daemon from bootfs failed status=%d\n", status);
        return -2;
    }
    const unsigned char *module_images[TERMD_MAX_MODULES];
    uint32_t module_sizes[TERMD_MAX_MODULES];
    memset(module_images, 0, sizeof(module_images));
    memset(module_sizes, 0, sizeof(module_sizes));
    const uint64_t module_count = sizeof(module_specs) / sizeof(module_specs[0]);
    for (uint64_t i = 0; i < module_count; i++) {
        status = seed0_bootfs_open_file(module_specs[i].path, &module_images[i], &module_sizes[i]);
        if (status != 0) {
            fprintf(stderr, "[seed0boot] termd: open %s from bootfs failed status=%d\n",
                module_specs[i].path,
                status);
            return -3;
        }
        if ((uint64_t)module_sizes[i] > TERMD_MODULE_IMAGE_STRIDE) {
            fprintf(stderr, "[seed0boot] termd: %s too large size=%u stride=%llu\n",
                module_specs[i].path,
                module_sizes[i],
                (unsigned long long)TERMD_MODULE_IMAGE_STRIDE);
            return -3;
        }
    }

    const int tty_endpoint_fd = pacha_ipc_endpoint_create(seed0_termd_endpoint_rights, 0);
    if (tty_endpoint_fd < 16) {
        fprintf(stderr, "[seed0boot] termd: tty endpoint create failed status=%d\n", tty_endpoint_fd);
        return tty_endpoint_fd < 0 ? tty_endpoint_fd : -2;
    }

    const long tty_endpoint_dup = pacha_fd_fcntl(
        tty_endpoint_fd,
        PACHA_FD_FCNTL_DUP,
        SEED0_TERMD_TTY_ENDPOINT_FD,
        seed0_termd_endpoint_rights);
    if (tty_endpoint_dup != SEED0_TERMD_TTY_ENDPOINT_FD) {
        fprintf(stderr,
            "[seed0boot] termd: tty endpoint dup failed status=%ld target=%u\n",
            tty_endpoint_dup,
            SEED0_TERMD_TTY_ENDPOINT_FD);
        (void)pacha_fd_close(tty_endpoint_fd);
        return -4;
    }

    status = mark_device_inherit(SEED0_TERMD_TTY_ENDPOINT_FD, "termd tty endpoint");
    if (status != 0) {
        (void)pacha_fd_close(tty_endpoint_fd);
        return status;
    }

    struct seed0_loaded_process loaded;
    status = seed0_load_elf_process("/srv/termd.elf", daemon_image, daemon_size, &loaded);
    if (status != 0) {
        (void)pacha_fd_close(tty_endpoint_fd);
        return status;
    }

    struct termd_boot_config config;
    memset(&config, 0, sizeof(config));
    config.magic = TERMD_BOOT_CONFIG_MAGIC;
    config.tty_endpoint_fd = SEED0_TERMD_TTY_ENDPOINT_FD;
    config.device_fd = device_fd >= 16 ? (uint64_t)(uint32_t)device_fd : 0;
    config.module_count = module_count;
    for (uint64_t i = 0; i < module_count; i++) {
        config.modules[i].image_va = TERMD_MODULE_IMAGE_VA +
            (TERMD_MODULE_IMAGE_STRIDE * i);
        config.modules[i].image_size = module_sizes[i];
        strncpy(config.modules[i].name, module_specs[i].name, sizeof(config.modules[i].name) - 1u);
    }

    status = seed0_map_bytes_into_process(
        loaded.process_fd,
        TERMD_BOOT_CONFIG_VA,
        &config,
        sizeof(config),
        PACHA_PROT_READ);
    if (status != 0) {
        fprintf(stderr, "[seed0boot] termd: config map failed status=%d\n", status);
        (void)pacha_fd_close(tty_endpoint_fd);
        return -5;
    }
    for (uint64_t i = 0; i < module_count; i++) {
        status = seed0_map_bytes_into_process(
            loaded.process_fd,
            config.modules[i].image_va,
            module_images[i],
            module_sizes[i],
            PACHA_PROT_READ);
        if (status != 0) {
            fprintf(stderr, "[seed0boot] termd: module map failed name=%s status=%d\n",
                module_specs[i].name,
                status);
            (void)pacha_fd_close(tty_endpoint_fd);
            return -6;
        }
    }

    printf("[seed0boot] termd modules=%llu\n",
        (unsigned long long)module_count);
    status = seed0_start_process(&loaded, "/srv/termd.elf", -1);
    if (status != 0) {
        (void)pacha_fd_close(tty_endpoint_fd);
        return status;
    }

    (void)pacha_fd_close(SEED0_TERMD_TTY_ENDPOINT_FD);
    if (out_tty_endpoint_fd != 0) {
        *out_tty_endpoint_fd = tty_endpoint_fd;
    } else {
        (void)pacha_fd_close(tty_endpoint_fd);
    }
    return 0;
}

int seed0_launch_storage_boot_nvme(int ready_channel_fd)
{
    static const struct seed0_capsule_module_spec module_specs[] = {
        {"/srv/kobox/nvme-auth.ko", "nvme-auth.ko"},
        {"/srv/kobox/nvme-core.ko", "nvme-core.ko"},
        {"/srv/kobox/nvme.ko", "nvme.ko"},
        {"/srv/kobox/crc16.ko", "crc16.ko"},
        {"/srv/kobox/mbcache.ko", "mbcache.ko"},
        {"/srv/kobox/jbd2.ko", "jbd2.ko"},
        {"/srv/kobox/ext4.ko", "ext4.ko"},
    };

    const struct seed0_init_descriptor_page *desc = seed0_bootstrap_descriptor();
    const struct seed0_device_descriptor *device = find_nvme_device(desc);
    if (device == 0) {
        fprintf(stderr, "[seed0boot] storage_boot: NVMe device fd not found\n");
        return -1;
    }
    const int device_fd = (int)device->init_device_fd;
    if (ready_channel_fd < 16) {
        fprintf(stderr, "[seed0boot] storage_boot: ready channel fd missing\n");
        return -1;
    }

    const unsigned char *daemon_image = 0;
    uint32_t daemon_size = 0;
    int status = seed0_bootfs_open_file("/srv/storage_boot.elf", &daemon_image, &daemon_size);
    if (status != 0) {
        fprintf(stderr, "[seed0boot] storage_boot: open daemon from bootfs failed status=%d\n", status);
        return -2;
    }
    const unsigned char *module_images[STORAGE_BOOT_MAX_MODULES];
    uint32_t module_sizes[STORAGE_BOOT_MAX_MODULES];
    int module_fds[STORAGE_BOOT_MAX_MODULES];
    memset(module_images, 0, sizeof(module_images));
    memset(module_sizes, 0, sizeof(module_sizes));
    memset(module_fds, 0, sizeof(module_fds));
    const uint64_t module_count = sizeof(module_specs) / sizeof(module_specs[0]);
    for (uint64_t i = 0; i < module_count; i++) {
        status = seed0_bootfs_open_file(module_specs[i].path, &module_images[i], &module_sizes[i]);
        if (status != 0) {
            fprintf(stderr, "[seed0boot] storage_boot: open %s from bootfs failed status=%d\n",
                module_specs[i].path,
                status);
            return -3;
        }
        module_fds[i] = seed0_create_inherited_vmo_from_bytes(module_images[i], module_sizes[i], module_specs[i].name);
        if (module_fds[i] < 16) return -3;
    }

    status = mark_device_inherit(device_fd, "storage_boot");
    if (status != 0) return status;
    struct storage_boot_config config;
    memset(&config, 0, sizeof(config));
    config.magic = STORAGE_BOOT_CONFIG_MAGIC;
    config.device_fd = (uint64_t)(uint32_t)device_fd;
    config.ready_channel_fd = (uint64_t)(uint32_t)ready_channel_fd;
    config.module_count = module_count;
    for (uint64_t i = 0; i < module_count; i++) {
        config.modules[i].image_fd = (uint64_t)(uint32_t)module_fds[i];
        config.modules[i].image_size = module_sizes[i];
        strncpy(config.modules[i].name, module_specs[i].name, sizeof(config.modules[i].name) - 1u);
    }

    const int config_fd = seed0_create_inherited_vmo_from_bytes(&config, sizeof(config), "storage_boot config");
    if (config_fd < 16) {
        return -5;
    }

    struct seed0_loaded_process loaded;
    status = seed0_load_elf_process("/srv/storage_boot.elf", daemon_image, daemon_size, &loaded);
    if (status != 0) {
        return status;
    }

    (void)daemon_size;
    printf("[seed0boot] storage_boot modules=%llu\n",
        (unsigned long long)module_count);
    status = seed0_start_process(&loaded, "/srv/storage_boot.elf", config_fd);
    if (status != 0) {
        return status;
    }
    return 0;
}
