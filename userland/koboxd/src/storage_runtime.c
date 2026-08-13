#include "storage_runtime.h"
#include "storage_benchmark.h"

#include "kobox/device_pachaos_capsule.h"
#include "kobox/shim.h"
#include "linux_subsystem/block/block.h"
#include "pacha/abi.h"
#include "pacha/ipc.h"

#include <stdio.h>
#include <string.h>

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

static int load_one_module(
    const koboxd_bootstrap_t *bootstrap,
    kb_device_backend_t *backend,
    const char *name,
    int allow_missing_init,
    kb_module_t **out_module)
{
    printf("[filed-storage] module lookup name=%s\n", name);
    fflush(stdout);
    if (out_module != NULL) {
        *out_module = NULL;
    }
    const koboxd_bootstrap_module_t *desc = koboxd_bootstrap_find_module(bootstrap, name);
    if (desc == NULL || desc->image_fd < 16 || desc->image_size == 0) {
        fprintf(stderr, "[filed-storage] module missing name=%s\n", name);
        return -1;
    }

    uint64_t map_size = 0;
    if (koboxd_align_image_size(desc->image_size, &map_size) != 0) {
        return -2;
    }
    const void *mapped_image = pacha_mmap(
        (int)desc->image_fd,
        map_size,
        PACHA_PROT_READ,
        PACHA_MMAP_SHARED,
        0);
    if (mapped_image == NULL) {
        fprintf(stderr, "[filed-storage] %s mmap failed fd=%llu bytes=%llu\n",
            name,
            (unsigned long long)desc->image_fd,
            (unsigned long long)desc->image_size);
        return -2;
    }
    printf("[filed-storage] module mapped name=%s addr=%p map_bytes=%llu image_bytes=%llu\n",
        name,
        mapped_image,
        (unsigned long long)map_size,
        (unsigned long long)desc->image_size);
    fflush(stdout);

    kb_module_t *module = NULL;
    const kb_module_image_t image = {
        .data = mapped_image,
        .size = (size_t)desc->image_size,
        .name = desc->name,
    };
    printf("[filed-storage] module open name=%s bytes=%llu\n",
        name,
        (unsigned long long)desc->image_size);
    fflush(stdout);
    kb_status_t status = kb_module_open_image(&image, backend, &module);
    if (status != KB_OK || module == NULL) {
        fprintf(stderr, "[filed-storage] %s open failed status=%s(%d)\n",
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
    printf("[filed-storage] module init start name=%s module=%p\n", name, (void *)module);
    fflush(stdout);
    status = kb_module_call_init(module, &init_result);
    if (status == KB_ERR_NOT_FOUND && allow_missing_init) {
        return 0;
    }
    if (status != KB_OK || init_result != 0) {
        fprintf(stderr, "[filed-storage] %s init failed status=%s(%d) result=%d\n",
            name,
            status_name(status),
            status,
            init_result);
        return -4;
    }
    printf("[filed-storage] module init done name=%s result=%d\n", name, init_result);
    fflush(stdout);
    return 0;
}

static int load_module_phase(
    const koboxd_bootstrap_t *bootstrap,
    kb_device_backend_t *backend,
    storage_stack_phase_t phase,
    kb_module_t **out_ext4_module)
{
    for (size_t i = 0; i < STORAGE_STACK_MODULE_COUNT; ++i) {
        const storage_stack_module_spec_t *spec = &storage_stack_modules[i];
        if (spec->phase != phase) {
            continue;
        }
        kb_module_t **out_module = NULL;
        if (spec->id == STORAGE_STACK_MODULE_EXT4) {
            out_module = out_ext4_module;
        }
        const int status = load_one_module(
            bootstrap,
            backend,
            spec->name,
            (spec->flags & STORAGE_STACK_MODULE_FLAG_ALLOW_MISSING_INIT) != 0,
            out_module);
        if (status != 0) {
            fprintf(stderr,
                "[filed-storage] module phase failed phase=%u name=%s status=%d\n",
                (unsigned int)phase,
                spec->name,
                status);
            return status;
        }
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

int koboxd_storage_runtime_init(
    koboxd_storage_runtime_t *runtime,
    const koboxd_bootstrap_t *bootstrap)
{
    if (runtime == NULL || bootstrap == NULL) {
        return -1;
    }
    memset(runtime, 0, sizeof(*runtime));

    printf("[filed-storage] nvme starting\n");
    fflush(stdout);

    printf("[filed-storage] device backend create start fd=%llu\n",
        (unsigned long long)bootstrap->device_fd);
    fflush(stdout);
    kb_status_t status = kb_pachaos_capsule_device_create(bootstrap->device_fd, &runtime->device_backend);
    printf("[filed-storage] device backend create done status=%s(%d) backend=%p\n",
        status_name(status),
        status,
        (void *)runtime->device_backend);
    fflush(stdout);
    if (status != KB_OK || runtime->device_backend == NULL) {
        fprintf(stderr, "[filed-storage] device backend create failed status=%s(%d)\n",
            status_name(status),
            status);
        return -2;
    }

    printf("[filed-storage] set device backend start backend=%p\n", (void *)runtime->device_backend);
    fflush(stdout);
    kb_shim_set_device_backend(runtime->device_backend);
    printf("[filed-storage] set device backend done backend=%p\n", (void *)runtime->device_backend);
    fflush(stdout);
    int load_status = load_module_phase(
        bootstrap,
        runtime->device_backend,
        STORAGE_STACK_PHASE_NVME,
        NULL);
    if (load_status != 0) {
        return load_status;
    }
    printf("[filed-storage] nvme ready\n");
    kb_shim_set_device_backend(runtime->device_backend);

    runtime->disk = wait_for_first_disk();
    if (runtime->disk == NULL) {
        fprintf(stderr, "[filed-storage] NVMe module stack registered no disk\n");
        return -3;
    }
    const int queue_status = kb_nvme_recreate_io_queue();
    if (queue_status != 0) {
        fprintf(stderr, "[filed-storage] nvme queue setup failed status=%d\n", queue_status);
        return -17;
    }

    load_status = load_module_phase(
        bootstrap,
        runtime->device_backend,
        STORAGE_STACK_PHASE_FILESYSTEM,
        &runtime->ext4_module);
    if (load_status != 0 || runtime->ext4_module == NULL) {
        return -9;
    }
    printf("[filed-storage] ext4 ready\n");
    kb_shim_set_device_backend(runtime->device_backend);

    int fs_status = kb_fs_block_device_create_from_disk_gpt_partition(
        STORAGE_STACK_ROOT_DEVICE_NAME,
        runtime->disk,
        STORAGE_STACK_ROOTFS_GPT_PARTITION_INDEX,
        &runtime->root_device);
    if (fs_status != 0 || runtime->root_device == NULL) {
        fprintf(stderr, "[filed-storage] rootfs block device create failed status=%d\n", fs_status);
        return -10;
    }

    fs_status = koboxd_fs_backend_mount_ext4(
        &runtime->fs_backend,
        runtime->ext4_module,
        runtime->root_device);
    if (fs_status != 0) {
        fprintf(stderr, "[filed-storage] fs-backend ext4 mount failed status=%d\n", fs_status);
        return -11;
    }
#if defined(KOBOXD_STORAGE_BENCHMARK)
    fs_status = koboxd_run_ext4_nvme_benchmark(&runtime->fs_backend);
    if (fs_status != 0) {
        return -18;
    }
#endif
    runtime->fs_ready = 1;
    return 0;
}

koboxd_fs_backend_t *koboxd_storage_runtime_fs_backend(koboxd_storage_runtime_t *runtime)
{
    if (runtime == NULL || !runtime->fs_ready) {
        return NULL;
    }
    return &runtime->fs_backend;
}
