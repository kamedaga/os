#include "storage_runtime.h"

#include "control_endpoint.h"
#include "filed/dispatch.h"
#include "filed_direct_backend.h"
#include "fs_endpoint.h"
#include "kobox/device_pachaos_capsule.h"
#include "kobox/shim.h"
#include "koboxd/ipc_protocol.h"
#include "linux_subsystem/block/block.h"
#include "pacha/abi.h"
#include "pacha/ipc.h"

#include <stdio.h>
#include <string.h>

enum {
    KOBOXD_FILED_ENDPOINT_FD = 240,
};

static const uint64_t koboxd_filed_endpoint_rights =
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
    if (out_module != NULL) {
        *out_module = NULL;
    }
    const koboxd_bootstrap_module_t *desc = koboxd_bootstrap_find_module(bootstrap, name);
    if (desc == NULL || desc->image_fd < 16 || desc->image_size == 0) {
        fprintf(stderr, "[koboxd] module missing name=%s\n", name);
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

int koboxd_storage_runtime_init(
    koboxd_storage_runtime_t *runtime,
    koboxd_ipc_service_t *ipc_service,
    const koboxd_bootstrap_t *bootstrap)
{
    if (runtime == NULL || ipc_service == NULL || bootstrap == NULL) {
        return -1;
    }
    memset(runtime, 0, sizeof(*runtime));
    runtime->ipc_service = ipc_service;

    printf("[koboxd] nvme starting\n");

    kb_status_t status = kb_pachaos_capsule_device_create(bootstrap->device_fd, &runtime->device_backend);
    if (status != KB_OK || runtime->device_backend == NULL) {
        fprintf(stderr, "[koboxd] device backend create failed status=%s(%d)\n",
            status_name(status),
            status);
        return -2;
    }

    kb_shim_set_device_backend(runtime->device_backend);
    int load_status = load_one_module(bootstrap, runtime->device_backend, "nvme-auth.ko", 1, NULL);
    if (load_status != 0) {
        return load_status;
    }
    load_status = load_one_module(bootstrap, runtime->device_backend, "nvme-core.ko", 1, NULL);
    if (load_status != 0) {
        return load_status;
    }
    load_status = load_one_module(bootstrap, runtime->device_backend, "nvme.ko", 0, NULL);
    if (load_status != 0) {
        return load_status;
    }
    printf("[koboxd] nvme ready\n");
    kb_shim_set_device_backend(runtime->device_backend);

    runtime->disk = wait_for_first_disk();
    if (runtime->disk == NULL) {
        fprintf(stderr, "[koboxd] NVMe module stack registered no disk\n");
        return -3;
    }
    const int queue_status = kb_nvme_recreate_io_queue();
    if (queue_status != 0) {
        fprintf(stderr, "[koboxd] nvme queue setup failed status=%d\n", queue_status);
        return -17;
    }

    int block_status = koboxd_block_service_init(&runtime->block_service, runtime->disk);
    if (block_status != 0) {
        fprintf(stderr, "[koboxd] block service init failed status=%d\n", block_status);
        return -4;
    }
    ipc_service->block.ready = 1;
    runtime->block_ready = 1;

    load_status = load_one_module(bootstrap, runtime->device_backend, "crc16.ko", 1, NULL);
    if (load_status != 0) {
        return -6;
    }
    load_status = load_one_module(bootstrap, runtime->device_backend, "mbcache.ko", 1, NULL);
    if (load_status != 0) {
        return -7;
    }
    load_status = load_one_module(bootstrap, runtime->device_backend, "jbd2.ko", 1, NULL);
    if (load_status != 0) {
        return -8;
    }
    load_status = load_one_module(bootstrap, runtime->device_backend, "ext4.ko", 0, &runtime->ext4_module);
    if (load_status != 0 || runtime->ext4_module == NULL) {
        return -9;
    }
    printf("[koboxd] ext4 ready\n");
    kb_shim_set_device_backend(runtime->device_backend);

    int fs_status = kb_fs_block_device_create_from_disk_gpt_partition(
        "rootfs-nvme",
        runtime->disk,
        KOBOXD_ROOTFS_GPT_PARTITION_INDEX,
        &runtime->root_device);
    if (fs_status != 0 || runtime->root_device == NULL) {
        fprintf(stderr, "[koboxd] rootfs block device create failed status=%d\n", fs_status);
        return -10;
    }

    fs_status = koboxd_fs_backend_mount_ext4(
        &runtime->fs_backend,
        runtime->ext4_module,
        runtime->root_device);
    if (fs_status != 0) {
        fprintf(stderr, "[koboxd] fs-backend ext4 mount failed status=%d\n", fs_status);
        return -11;
    }
    ipc_service->fs_backend.ready = 1;
    ipc_service->event.ready = 1;
    ipc_service->filed.ready = 1;
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

static int koboxd_storage_runtime_start_filed(koboxd_storage_runtime_t *runtime)
{
    if (runtime == NULL ||
        runtime->ipc_service == NULL ||
        runtime->ipc_service->filed.endpoint_fd < 16 ||
        !runtime->fs_ready)
    {
        return -1;
    }

    int filed_endpoint_fd = runtime->ipc_service->filed.endpoint_fd;
    if (filed_endpoint_fd != KOBOXD_FILED_ENDPOINT_FD) {
        const long dup_fd = pacha_fd_fcntl(
            filed_endpoint_fd,
            PACHA_FD_FCNTL_DUP,
            KOBOXD_FILED_ENDPOINT_FD,
            koboxd_filed_endpoint_rights);
        if (dup_fd != KOBOXD_FILED_ENDPOINT_FD) {
            if (dup_fd >= 16) {
                (void)pacha_fd_close((int)dup_fd);
            }
            return -24;
        }
        (void)pacha_fd_close(filed_endpoint_fd);
        runtime->ipc_service->filed.endpoint_fd = (int)dup_fd;
        filed_endpoint_fd = (int)dup_fd;
    }
    const long inherit_status = pacha_fd_fcntl(
        filed_endpoint_fd,
        PACHA_FD_FCNTL_SET_FLAGS,
        PACHA_FD_FLAG_INHERIT,
        PACHA_FD_FLAG_INHERIT);
    if (inherit_status != 0) {
        return -13;
    }

    filed_runtime_init(&runtime->filed_runtime);
    filed_kobox_backend_init_direct(
        &runtime->filed_runtime.backend,
        &runtime->fs_backend,
        koboxd_filed_direct_ops());
    runtime->filed_runtime.client_endpoint_fd = filed_endpoint_fd;

    const int status = filed_runtime_mount_root(&runtime->filed_runtime);
    if (status != 0) {
        return status;
    }
    runtime->filed_ready = 1;
    printf("[filed] ready\n");
    return 0;
}

static int koboxd_storage_runtime_serve_filed_ready(koboxd_storage_runtime_t *runtime, uint64_t session_index)
{
    if (runtime == NULL || !runtime->filed_ready) {
        return -1;
    }
    if (session_index == UINT64_MAX) {
        const int status = filed_dispatch_client_once(
            &runtime->filed_runtime,
            runtime->filed_runtime.client_endpoint_fd);
        if (status == 0 ||
            status == PACHA_ERR_EMPTY ||
            status == PACHA_ERR_NOT_READY ||
            status == -2)
        {
            return 0;
        }
        return status;
    }

    const int status = filed_dispatch_session_once(&runtime->filed_runtime, session_index);
    if (status == 0 ||
        status == PACHA_ERR_EMPTY ||
        status == PACHA_ERR_NOT_READY ||
        status == -2)
    {
        return 0;
    }
    return status;
}

static int koboxd_storage_runtime_loop(koboxd_storage_runtime_t *runtime)
{
    enum {
        FS_SLOT = UINT64_MAX - 1ull,
        FILED_CLIENT_SLOT = UINT64_MAX,
    };

    for (;;) {
        struct pacha_pollfd fds[2 + FILED_RUNTIME_MAX_SESSIONS];
        uint64_t slots[2 + FILED_RUNTIME_MAX_SESSIONS];
        uint64_t count = 0;

        const koboxd_ipc_endpoint_t *fs_endpoint =
            koboxd_ipc_service_endpoint_const(runtime->ipc_service, KOBOXD_IPC_ENDPOINT_FS_BACKEND);
        if (fs_endpoint != NULL && fs_endpoint->endpoint_fd >= 16) {
            fds[count] = (struct pacha_pollfd){
                .fd = fs_endpoint->endpoint_fd,
                .events = PACHA_FD_EVENT_READABLE,
                .revents = 0,
            };
            slots[count++] = FS_SLOT;
        }

        if (runtime->filed_ready && runtime->filed_runtime.client_endpoint_fd >= 16) {
            fds[count] = (struct pacha_pollfd){
                .fd = runtime->filed_runtime.client_endpoint_fd,
                .events = PACHA_FD_EVENT_READABLE,
                .revents = 0,
            };
            slots[count++] = FILED_CLIENT_SLOT;
        }

        if (runtime->filed_ready) {
            for (uint64_t i = 0; i < FILED_RUNTIME_MAX_SESSIONS; ++i) {
                if (!runtime->filed_runtime.sessions[i].active) {
                    continue;
                }
                fds[count] = (struct pacha_pollfd){
                    .fd = runtime->filed_runtime.sessions[i].channel_fd,
                    .events = PACHA_FD_EVENT_READABLE,
                    .revents = 0,
                };
                slots[count++] = i;
            }
        }

        if (count == 0) {
            return -1;
        }

        const long wait_status = pacha_fd_wait_many(fds, count, PACHA_FD_WAIT_FOREVER);
        if (wait_status < 0) {
            return (int)wait_status;
        }

        for (uint64_t i = 0; i < count; ++i) {
            if ((fds[i].revents & (PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_ERROR | PACHA_FD_EVENT_HANGUP)) == 0) {
                continue;
            }
            if (slots[i] == FS_SLOT) {
                const int status = koboxd_fs_endpoint_serve_once(runtime->ipc_service, &runtime->fs_backend);
                if (status != 0) {
                    return status;
                }
            } else if (slots[i] == FILED_CLIENT_SLOT) {
                const int status = koboxd_storage_runtime_serve_filed_ready(runtime, UINT64_MAX);
                if (status != 0) {
                    return status;
                }
            } else {
                const int status = koboxd_storage_runtime_serve_filed_ready(runtime, slots[i]);
                if (status != 0) {
                    return status;
                }
            }
        }
    }
}

int koboxd_storage_runtime_serve(
    koboxd_storage_runtime_t *runtime,
    int control_fd)
{
    if (runtime == NULL || runtime->ipc_service == NULL || control_fd < 16) {
        return -1;
    }
    if (koboxd_control_serve_get_endpoint(runtime->ipc_service, control_fd, KOBOXD_WIRE_ENDPOINT_BLOCK) != 0) {
        return -13;
    }
    if (koboxd_block_serve_identify(runtime->ipc_service, &runtime->block_service) != 0) {
        return -14;
    }
    if (koboxd_control_serve_get_endpoint(runtime->ipc_service, control_fd, KOBOXD_WIRE_ENDPOINT_FS_BACKEND) != 0) {
        return -15;
    }
    if (koboxd_control_serve_get_endpoint(runtime->ipc_service, control_fd, KOBOXD_WIRE_ENDPOINT_FILED) != 0) {
        return -16;
    }
    const int filed_status = koboxd_storage_runtime_start_filed(runtime);
    if (filed_status != 0) {
        return filed_status;
    }
    return koboxd_storage_runtime_loop(runtime);
}

int koboxd_run_storage(
    koboxd_ipc_service_t *ipc_service,
    const koboxd_bootstrap_t *bootstrap)
{
    static koboxd_storage_runtime_t runtime;
    const int init_status = koboxd_storage_runtime_init(&runtime, ipc_service, bootstrap);
    if (init_status != 0) {
        return init_status;
    }
    return koboxd_storage_runtime_serve(&runtime, (int)(uint32_t)bootstrap->control_fd);
}
