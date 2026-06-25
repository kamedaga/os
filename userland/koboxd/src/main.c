#include "kobox/device_pachaos_capsule.h"
#include "kobox/module.h"
#include "kobox/shim.h"
#include "linux_subsystem/block/block.h"
#include "linux_subsystem/fs/fs.h"
#include "koboxd/ipc_protocol.h"
#include "pacha/abi.h"

#include "block_service.h"
#include "control_endpoint.h"
#include "fs_endpoint.h"
#include "fs_backend.h"
#include "ipc_service.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KOBOXD_BOOTSTRAP_MAGIC = 0x3150474b42584f4bull,
    KOBOXD_BOOTSTRAP_MAX_MODULES = 8,
    KOBOXD_BOOTSTRAP_NAME_BYTES = 64,
    KOBOXD_PAGE_SIZE = 4096,
    KOBOXD_ROOTFS_GPT_PARTITION_INDEX = 2,
};

struct koboxd_bootstrap_module {
    char name[KOBOXD_BOOTSTRAP_NAME_BYTES];
    uint64_t image_fd;
    uint64_t image_size;
};

struct koboxd_bootstrap {
    uint64_t magic;
    uint64_t device_fd;
    uint64_t control_fd;
    uint64_t module_count;
    struct koboxd_bootstrap_module modules[KOBOXD_BOOTSTRAP_MAX_MODULES];
};

static const char *const koboxd_expected_modules[] = {
    "nvme-auth.ko",
    "nvme-core.ko",
    "nvme.ko",
    "crc16.ko",
    "mbcache.ko",
    "jbd2.ko",
    "ext4.ko",
};

static const char *status_name(kb_status_t status)
{
    switch (status) {
    case KB_OK: return "KB_OK";
    case KB_ERR_INVALID: return "KB_ERR_INVALID";
    case KB_ERR_NOT_FOUND: return "KB_ERR_NOT_FOUND";
    case KB_ERR_DENIED: return "KB_ERR_DENIED";
    case KB_ERR_NOMEM: return "KB_ERR_NOMEM";
    case KB_ERR_IO: return "KB_ERR_IO";
    case KB_ERR_UNSUPPORTED: return "KB_ERR_UNSUPPORTED";
    case KB_ERR_PCI_CONFIG: return "KB_ERR_PCI_CONFIG";
    default: return "KB_ERR_UNKNOWN";
    }
}

static int align_image_size(uint64_t size, uint64_t *out_size)
{
    if (out_size == NULL || size == 0 || size > UINT64_MAX - (KOBOXD_PAGE_SIZE - 1)) {
        return -1;
    }
    *out_size = (size + (KOBOXD_PAGE_SIZE - 1)) & ~(uint64_t)(KOBOXD_PAGE_SIZE - 1);
    return 0;
}

static int find_bootstrap(
    char **argv,
    int *out_bootstrap_fd)
{
    if (argv == NULL || out_bootstrap_fd == NULL) {
        return -1;
    }
    *out_bootstrap_fd = -1;

    char **p = argv;
    while (*p != NULL) {
        p++;
    }
    p++;
    while (*p != NULL) {
        p++;
    }
    p++;

    uint64_t bootstrap_fd = 0;
    const uint64_t *auxv = (const uint64_t *)(const void *)p;
    for (unsigned i = 0; i < 64; i++) {
        const uint64_t type = auxv[i * 2u];
        const uint64_t value = auxv[i * 2u + 1u];
        if (type == 0) {
            break;
        }
        if (type == PACHA_AT_BOOTSTRAP_FD) {
            bootstrap_fd = value;
        }
    }
    if (bootstrap_fd < 16) {
        return -2;
    }
    *out_bootstrap_fd = (int)bootstrap_fd;
    return 0;
}

static int read_bootstrap_fd(int fd, struct koboxd_bootstrap *out_bootstrap)
{
    if (fd < 16 || out_bootstrap == NULL) {
        return -1;
    }
    const long got = pacha_fd_read(fd, out_bootstrap, sizeof(*out_bootstrap));
    if (got != (long)sizeof(*out_bootstrap)) {
        fprintf(stderr,
            "[koboxd] bootstrap fd read failed fd=%d got=%ld size=%llu\n",
            fd,
            got,
            (unsigned long long)sizeof(*out_bootstrap));
        return -2;
    }
    return 0;
}

static int validate_bootstrap_package(
    const struct koboxd_bootstrap *bootstrap,
    uint64_t bootstrap_size)
{
    if (bootstrap == NULL || bootstrap_size < sizeof(*bootstrap)) {
        return -1;
    }
    const uint64_t expected_count =
        sizeof(koboxd_expected_modules) / sizeof(koboxd_expected_modules[0]);
    if (bootstrap->magic != KOBOXD_BOOTSTRAP_MAGIC ||
        bootstrap->device_fd < 16 ||
        bootstrap->control_fd < 16 ||
        bootstrap->module_count != expected_count ||
        bootstrap->module_count > KOBOXD_BOOTSTRAP_MAX_MODULES)
    {
        fprintf(stderr,
            "[koboxd] bootstrap invalid magic=0x%llx device_fd=%llu control_fd=%llu modules=%llu size=%llu\n",
            (unsigned long long)bootstrap->magic,
            (unsigned long long)bootstrap->device_fd,
            (unsigned long long)bootstrap->control_fd,
            (unsigned long long)bootstrap->module_count,
            (unsigned long long)bootstrap_size);
        return -1;
    }

    for (uint64_t i = 0; i < bootstrap->module_count; i++) {
        const struct koboxd_bootstrap_module *module = &bootstrap->modules[i];
        if (strncmp(module->name, koboxd_expected_modules[i], KOBOXD_BOOTSTRAP_NAME_BYTES) != 0 ||
            module->image_fd < 16 ||
            module->image_size < 4)
        {
            fprintf(stderr,
                "[koboxd] bootstrap module invalid index=%llu name=%s fd=%llu size=%llu\n",
                (unsigned long long)i,
                module->name,
                (unsigned long long)module->image_fd,
                (unsigned long long)module->image_size);
            return -2;
        }

        uint64_t map_size = 0;
        if (align_image_size(module->image_size, &map_size) != 0) {
            return -3;
        }
        const unsigned char *image = pacha_mmap(
            (int)module->image_fd,
            map_size,
            PACHA_PROT_READ,
            PACHA_MMAP_SHARED,
            0);
        if (image == NULL) {
            fprintf(stderr, "[koboxd] bootstrap module mmap failed name=%s fd=%llu\n",
                module->name,
                (unsigned long long)module->image_fd);
            return -3;
        }
        if (image[0] != 0x7f || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') {
            fprintf(stderr, "[koboxd] bootstrap module is not ELF name=%s\n", module->name);
            (void)pacha_munmap((void *)image, map_size);
            return -4;
        }
        (void)pacha_munmap((void *)image, map_size);
    }

    return 0;
}

static const struct koboxd_bootstrap_module *find_module(
    const struct koboxd_bootstrap *bootstrap,
    const char *name)
{
    if (bootstrap == NULL || name == NULL) {
        return NULL;
    }
    for (uint64_t i = 0; i < bootstrap->module_count; i++) {
        if (strncmp(bootstrap->modules[i].name, name, KOBOXD_BOOTSTRAP_NAME_BYTES) == 0) {
            return &bootstrap->modules[i];
        }
    }
    return NULL;
}

static int load_one_module(
    const struct koboxd_bootstrap *bootstrap,
    kb_device_backend_t *backend,
    const char *name,
    int allow_missing_init,
    kb_module_t **out_module)
{
    if (out_module != NULL) {
        *out_module = NULL;
    }
    const struct koboxd_bootstrap_module *desc = find_module(bootstrap, name);
    if (desc == NULL || desc->image_fd < 16 || desc->image_size == 0) {
        fprintf(stderr, "[koboxd] module missing name=%s\n", name);
        return -1;
    }

    uint64_t map_size = 0;
    if (align_image_size(desc->image_size, &map_size) != 0) {
        return -2;
    }
    const void *mapped_image = pacha_mmap(
        (int)desc->image_fd,
        map_size,
        PACHA_PROT_READ,
        PACHA_MMAP_SHARED,
        0);
    if (mapped_image == NULL) {
        fprintf(stderr, "[koboxd] %s mmap failed fd=%llu bytes=%llu\n",
            name,
            (unsigned long long)desc->image_fd,
            (unsigned long long)desc->image_size);
        return -2;
    }

    kb_module_t *module = NULL;
    const kb_module_image_t image = {
        .data = mapped_image,
        .size = (size_t)desc->image_size,
        .name = desc->name,
    };
    kb_status_t status = kb_module_open_image(&image, backend, &module);
    if (status != KB_OK || module == NULL) {
        fprintf(stderr, "[koboxd] %s open failed status=%s(%d)\n",
            name,
            status_name(status),
            status);
        (void)pacha_munmap((void *)mapped_image, map_size);
        return -3;
    }
    if (out_module != NULL) {
        *out_module = module;
    }

    int init_result = 0;
    status = kb_module_call_init(module, &init_result);
    if (status == KB_ERR_NOT_FOUND && allow_missing_init) {
        return 0;
    }
    if (status != KB_OK || init_result != 0) {
        fprintf(stderr, "[koboxd] %s init failed status=%s(%d) result=%d\n",
            name,
            status_name(status),
            status,
            init_result);
        return -4;
    }
    return 0;
}

static void *wait_for_first_disk(void)
{
    for (unsigned i = 0; i < 2048; i++) {
        kb_run_deferred_work();
        (void)kb_handle_any_irq(0);
        void *disk = kb_block_subsystem_first_registered_disk();
        if (disk != NULL) {
            return disk;
        }
    }
    return NULL;
}

static int run_storage(koboxd_ipc_service_t *ipc_service, const struct koboxd_bootstrap *bootstrap)
{
    if (ipc_service == NULL || bootstrap == NULL) {
        return -1;
    }

    printf("[koboxd] nvme starting\n");

    kb_device_backend_t *backend = NULL;
    kb_status_t status = kb_pachaos_capsule_device_create(bootstrap->device_fd, &backend);
    if (status != KB_OK || backend == NULL) {
        fprintf(stderr, "[koboxd] device backend create failed status=%s(%d)\n",
            status_name(status),
            status);
        return -2;
    }

    kb_shim_set_device_backend(backend);
    int load_status = load_one_module(bootstrap, backend, "nvme-auth.ko", 1, NULL);
    if (load_status != 0) {
        return load_status;
    }
    load_status = load_one_module(bootstrap, backend, "nvme-core.ko", 1, NULL);
    if (load_status != 0) {
        return load_status;
    }
    load_status = load_one_module(bootstrap, backend, "nvme.ko", 0, NULL);
    if (load_status != 0) {
        return load_status;
    }
    printf("[koboxd] nvme ready\n");
    kb_shim_set_device_backend(backend);

    void *disk = wait_for_first_disk();
    if (disk == NULL) {
        fprintf(stderr, "[koboxd] NVMe module stack registered no disk\n");
        return -3;
    }
    const int queue_status = kb_nvme_recreate_io_queue();
    if (queue_status != 0) {
        fprintf(stderr, "[koboxd] nvme queue setup failed status=%d\n", queue_status);
        return -17;
    }

    koboxd_block_service_t block_service;
    int block_status = koboxd_block_service_init(&block_service, disk);
    if (block_status != 0) {
        fprintf(stderr, "[koboxd] block service init failed status=%d\n", block_status);
        return -4;
    }
    ipc_service->block.ready = 1;

    load_status = load_one_module(bootstrap, backend, "crc16.ko", 1, NULL);
    if (load_status != 0) {
        return -6;
    }
    load_status = load_one_module(bootstrap, backend, "mbcache.ko", 1, NULL);
    if (load_status != 0) {
        return -7;
    }
    load_status = load_one_module(bootstrap, backend, "jbd2.ko", 1, NULL);
    if (load_status != 0) {
        return -8;
    }
    kb_module_t *ext4_module = NULL;
    load_status = load_one_module(bootstrap, backend, "ext4.ko", 0, &ext4_module);
    if (load_status != 0 || ext4_module == NULL) {
        return -9;
    }
    printf("[koboxd] ext4 ready\n");
    kb_shim_set_device_backend(backend);

    kb_fs_block_device_t *root_device = NULL;
    int fs_status = kb_fs_block_device_create_from_disk_gpt_partition(
        "rootfs-nvme",
        disk,
        KOBOXD_ROOTFS_GPT_PARTITION_INDEX,
        &root_device);
    if (fs_status != 0 || root_device == NULL) {
        fprintf(stderr, "[koboxd] rootfs block device create failed status=%d\n", fs_status);
        return -10;
    }

    koboxd_fs_backend_t fs_backend;
    fs_status = koboxd_fs_backend_mount_ext4(&fs_backend, ext4_module, root_device);
    if (fs_status != 0) {
        fprintf(stderr, "[koboxd] fs-backend ext4 mount failed status=%d\n", fs_status);
        return -11;
    }
    ipc_service->fs_backend.ready = 1;
    ipc_service->event.ready = 1;
    if (koboxd_control_serve_get_endpoint(ipc_service, (int)bootstrap->control_fd, KOBOXD_WIRE_ENDPOINT_BLOCK) != 0) {
        return -13;
    }
    if (koboxd_block_serve_identify(ipc_service, &block_service) != 0) {
        return -14;
    }
    if (koboxd_control_serve_get_endpoint(ipc_service, (int)bootstrap->control_fd, KOBOXD_WIRE_ENDPOINT_FS_BACKEND) != 0) {
        return -15;
    }
    for (;;) {
        if (koboxd_fs_endpoint_serve_once(ipc_service, &fs_backend) != 0) {
            return -16;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    printf("[koboxd] start\n");
    fflush(stdout);
    koboxd_ipc_service_t ipc_service;
    koboxd_ipc_service_init(&ipc_service);
    int bootstrap_fd = -1;
    struct koboxd_bootstrap bootstrap;
    int status = find_bootstrap(argv, &bootstrap_fd);
    printf("[koboxd] bootstrap fd=%d status=%d\n", bootstrap_fd, status);
    fflush(stdout);
    if (status != 0) {
        return 4;
    }
    status = read_bootstrap_fd(bootstrap_fd, &bootstrap);
    printf("[koboxd] bootstrap read status=%d magic=0x%llx device_fd=%llu control_fd=%llu modules=%llu\n",
        status,
        (unsigned long long)bootstrap.magic,
        (unsigned long long)bootstrap.device_fd,
        (unsigned long long)bootstrap.control_fd,
        (unsigned long long)bootstrap.module_count);
    fflush(stdout);
    if (status != 0 ||
        validate_bootstrap_package(&bootstrap, sizeof(bootstrap)) != 0) {
        return 4;
    }
    if (run_storage(&ipc_service, &bootstrap) != 0) {
        return 5;
    }
    koboxd_ipc_service_debug_dump(&ipc_service, stdout);
    printf("[koboxd] ready\n");
    fflush(stdout);
    fflush(stderr);
    return 0;
}
