#include "kobox/device_pachaos_capsule.h"
#include "kobox/module.h"
#include "kobox/shim.h"
#include "linux_subsystem/block/block.h"
#include "linux_subsystem/fs/fs.h"
#include "koboxd/ipc_protocol.h"
#include "pacha/abi.h"
#include "pacha/capsule.h"
#include "pacha/ipc.h"

#include "block_service.h"
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

static const uint64_t koboxd_service_channel_rights =
    PACHA_FD_RIGHT_INSPECT |
    PACHA_FD_RIGHT_WAIT |
    PACHA_FD_RIGHT_POLL |
    PACHA_FD_RIGHT_SET_FLAGS |
    PACHA_FD_RIGHT_CLOSE |
    PACHA_FD_RIGHT_SEND |
    PACHA_FD_RIGHT_RECV |
    PACHA_FD_RIGHT_CALL |
    PACHA_FD_RIGHT_TRANSFER;

static int recv_ipc_wait(int fd, struct pacha_ipc_msg *msg)
{
    if (fd < 16 || msg == NULL) {
        return -1;
    }
    for (unsigned i = 0; i < 262144; i++) {
        const int status = pacha_ipc_recv(fd, msg);
        if (status == 0) {
            return 0;
        }
        if (status != PACHA_ERR_EMPTY && status != PACHA_ERR_NOT_READY && status != -2) {
            return status;
        }
        struct pacha_pollfd pollfd = {
            .fd = fd,
            .events = PACHA_FD_EVENT_READABLE,
            .revents = 0,
        };
        (void)pacha_fd_wait_many(&pollfd, 1, 1);
    }
    return -2;
}

static int send_ipc_wait(int fd, const struct pacha_ipc_msg *msg)
{
    if (fd < 16 || msg == NULL) {
        return -1;
    }
    for (unsigned i = 0; i < 262144; i++) {
        const int status = pacha_ipc_send(fd, msg);
        if (status == 0) {
            return 0;
        }
        if (status != PACHA_ERR_EMPTY && status != PACHA_ERR_NOT_READY && status != -2) {
            return status;
        }
        struct pacha_pollfd pollfd = {
            .fd = fd,
            .events = PACHA_FD_EVENT_WRITABLE,
            .revents = 0,
        };
        (void)pacha_fd_wait_many(&pollfd, 1, 1);
    }
    return -2;
}

static int send_endpoint_fd(int control_fd, uint64_t request_id, uint64_t endpoint_kind, int client_fd)
{
    struct pacha_ipc_fd fd_item = {
        .fd = (uint64_t)(uint32_t)client_fd,
        .rights = koboxd_service_channel_rights,
        .flags = 0,
        .transfer_flags = PACHA_IPC_TRANSFER_MOVE | PACHA_IPC_TRANSFER_CLOEXEC,
    };
    struct pacha_ipc_msg reply = {
        .word0 = KOBOXD_WIRE_REPLY_MAGIC,
        .word1 = 0,
        .word2 = endpoint_kind,
        .word3 = request_id,
        .fds = &fd_item,
        .fd_count = 1,
    };
    return send_ipc_wait(control_fd, &reply);
}

static int send_status_reply(int fd, uint64_t request_id, uint64_t word2)
{
    const struct pacha_ipc_msg reply = {
        .word0 = KOBOXD_WIRE_REPLY_MAGIC,
        .word1 = 0,
        .word2 = word2,
        .word3 = request_id,
    };
    return send_ipc_wait(fd, &reply);
}

static int send_status_reply_ex(int fd, uint64_t request_id, int64_t status, uint64_t result)
{
    const struct pacha_ipc_msg reply = {
        .word0 = KOBOXD_WIRE_REPLY_MAGIC,
        .word1 = (uint64_t)status,
        .word2 = result,
        .word3 = request_id,
    };
    return send_ipc_wait(fd, &reply);
}

static void *map_wire_vmo_from_msg(const struct pacha_ipc_msg *request, uint64_t size, int *out_fd)
{
    if (request == NULL || request->fd_count < 1 || request->fds == NULL || request->fds[0].fd < 16 || out_fd == NULL) {
        return NULL;
    }
    *out_fd = (int)request->fds[0].fd;
    return pacha_mmap(*out_fd, size, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
}

static int create_service_channel_pair(struct pacha_ipc_channel_pair *pair)
{
    if (pair == NULL) {
        return -1;
    }
    pair->a = -1;
    pair->b = -1;
    const int status = pacha_ipc_channel_create(pair, koboxd_service_channel_rights, 0);
    if (status != 0 || pair->a < 16 || pair->b < 16) {
        return status != 0 ? status : -1;
    }
    return 0;
}

static int serve_control_get_endpoint(koboxd_ipc_service_t *ipc_service, int control_fd, uint64_t expected_kind)
{
    struct pacha_ipc_msg request = {0};
    int status = recv_ipc_wait(control_fd, &request);
    if (status != 0) {
        fprintf(stderr, "[koboxd] control recv failed status=%d\n", status);
        return status;
    }
    if (request.word0 != KOBOXD_WIRE_CONTROL_MAGIC ||
        request.word1 != KOBOXD_WIRE_CONTROL_GET_ENDPOINT ||
        request.word2 != expected_kind ||
        request.word3 != KOBOXD_WIRE_VERSION)
    {
        fprintf(stderr,
            "[koboxd] control request invalid word0=0x%llx op=%llu kind=%llu version=%llu\n",
            (unsigned long long)request.word0,
            (unsigned long long)request.word1,
            (unsigned long long)request.word2,
            (unsigned long long)request.word3);
        return -2;
    }

    struct pacha_ipc_channel_pair pair;
    status = create_service_channel_pair(&pair);
    if (status != 0) {
        fprintf(stderr, "[koboxd] service channel create failed status=%d\n", status);
        return status;
    }
    koboxd_ipc_endpoint_t *endpoint = koboxd_ipc_service_endpoint(ipc_service, (koboxd_ipc_endpoint_kind_t)expected_kind);
    if (endpoint == NULL) {
        return -3;
    }
    endpoint->endpoint_fd = pair.b;
    endpoint->ready = 1;

    status = send_endpoint_fd(control_fd, request.word3, expected_kind, pair.a);
    if (status != 0) {
        fprintf(stderr,
            "[koboxd] endpoint fd send failed kind=%llu status=%d\n",
            (unsigned long long)expected_kind,
            status);
        return status;
    }
    return 0;
}

static int serve_block_identify(koboxd_ipc_service_t *ipc_service, const koboxd_block_service_t *block_service)
{
    const koboxd_ipc_endpoint_t *endpoint =
        koboxd_ipc_service_endpoint_const(ipc_service, KOBOXD_IPC_ENDPOINT_BLOCK);
    if (endpoint == NULL || endpoint->endpoint_fd < 16 || block_service == NULL) {
        return -1;
    }
    struct pacha_ipc_msg request = {0};
    int status = recv_ipc_wait(endpoint->endpoint_fd, &request);
    if (status != 0) {
        return status;
    }
    if (request.word0 != KOBOXD_WIRE_ENDPOINT_MAGIC || request.word1 != KOBOXD_WIRE_BLOCK_IDENTIFY) {
        return -2;
    }
    status = send_status_reply(endpoint->endpoint_fd, request.word3, block_service->logical_block_size);
    return status;
}

static int serve_fs_backend_once(koboxd_ipc_service_t *ipc_service, koboxd_fs_backend_t *fs_backend)
{
    const koboxd_ipc_endpoint_t *endpoint =
        koboxd_ipc_service_endpoint_const(ipc_service, KOBOXD_IPC_ENDPOINT_FS_BACKEND);
    if (endpoint == NULL || endpoint->endpoint_fd < 16 || fs_backend == NULL) {
        return -1;
    }
    struct pacha_ipc_fd fds[1];
    memset(fds, 0, sizeof(fds));
    struct pacha_ipc_msg request = {
        .fds = fds,
        .fd_capacity = 1,
    };
    int status = recv_ipc_wait(endpoint->endpoint_fd, &request);
    if (status != 0) {
        return status;
    }
    if (request.word0 != KOBOXD_WIRE_ENDPOINT_MAGIC) {
        return -2;
    }

    int64_t reply_status = 0;
    uint64_t result = 0;
    int vmo_fd = -1;
    void *mapped = NULL;

    switch (request.word1) {
    case KOBOXD_WIRE_FS_MOUNT_ROOT:
        result = fs_backend->mount_result.observed_ext4_magic;
        break;
    case KOBOXD_WIRE_FS_LOOKUP: {
        mapped = map_wire_vmo_from_msg(&request, KOBOXD_WIRE_FS_PAGE_BYTES, &vmo_fd);
        if (mapped == NULL) {
            reply_status = -22;
            break;
        }
        koboxd_wire_fs_lookup_t *lookup = (koboxd_wire_fs_lookup_t *)mapped;
        lookup->name[KOBOXD_WIRE_FS_NAME_BYTES - 1] = '\0';
        uint64_t object_id = 0;
        reply_status = koboxd_fs_backend_lookup(fs_backend, lookup->parent_object_id, lookup->name, &object_id);
        result = object_id;
        break;
    }
    case KOBOXD_WIRE_FS_STATX: {
        mapped = map_wire_vmo_from_msg(&request, KOBOXD_WIRE_FS_PAGE_BYTES, &vmo_fd);
        if (mapped == NULL) {
            reply_status = -22;
            break;
        }
        koboxd_wire_fs_statx_t *wire_stat = (koboxd_wire_fs_statx_t *)mapped;
        koboxd_fs_object_t stat;
        reply_status = koboxd_fs_backend_statx(fs_backend, request.word2, &stat);
        if (reply_status == 0) {
            memset(wire_stat, 0, sizeof(*wire_stat));
            wire_stat->object_id = stat.object_id;
            wire_stat->mode = stat.mode;
            wire_stat->size = stat.size;
            wire_stat->blocks = stat.blocks;
            wire_stat->nlink = stat.nlink;
            wire_stat->kind = (stat.mode & 0170000u);
            result = stat.size;
        }
        break;
    }
    case KOBOXD_WIRE_FS_PREAD: {
        mapped = map_wire_vmo_from_msg(&request, KOBOXD_WIRE_FS_PAGE_BYTES, &vmo_fd);
        if (mapped == NULL) {
            reply_status = -22;
            break;
        }
        koboxd_wire_fs_io_t *io = (koboxd_wire_fs_io_t *)mapped;
        size_t length = (size_t)io->length;
        if (length > sizeof(io->data)) {
            length = sizeof(io->data);
        }
        reply_status = koboxd_fs_backend_pread(fs_backend, io->object_id, io->offset, io->data, length);
        if (reply_status >= 0) {
            result = (uint64_t)reply_status;
            reply_status = 0;
        }
        break;
    }
    case KOBOXD_WIRE_FS_PWRITE: {
        mapped = map_wire_vmo_from_msg(&request, KOBOXD_WIRE_FS_PAGE_BYTES, &vmo_fd);
        if (mapped == NULL) {
            reply_status = -22;
            break;
        }
        koboxd_wire_fs_io_t *io = (koboxd_wire_fs_io_t *)mapped;
        size_t length = (size_t)io->length;
        if (length > sizeof(io->data)) {
            length = sizeof(io->data);
        }
        reply_status = koboxd_fs_backend_pwrite(fs_backend, io->object_id, io->offset, io->data, length);
        if (reply_status >= 0) {
            result = (uint64_t)reply_status;
            reply_status = 0;
        }
        break;
    }
    case KOBOXD_WIRE_FS_FSYNC:
        reply_status = koboxd_fs_backend_fsync(fs_backend, request.word2);
        break;
    case KOBOXD_WIRE_FS_GETDENTS: {
        mapped = map_wire_vmo_from_msg(&request, KOBOXD_WIRE_FS_PAGE_BYTES, &vmo_fd);
        if (mapped == NULL) {
            reply_status = -22;
            break;
        }
        koboxd_wire_fs_getdents_t *wire_dir = (koboxd_wire_fs_getdents_t *)mapped;
        koboxd_fs_object_t entries[KOBOXD_WIRE_FS_DIRENT_CAPACITY];
        memset(entries, 0, sizeof(entries));
        size_t count = 0;
        size_t capacity = (size_t)wire_dir->capacity;
        if (capacity > KOBOXD_WIRE_FS_DIRENT_CAPACITY) {
            capacity = KOBOXD_WIRE_FS_DIRENT_CAPACITY;
        }
        reply_status = koboxd_fs_backend_getdents(
            fs_backend,
            wire_dir->dir_object_id,
            wire_dir->offset,
            entries,
            capacity,
            &count);
        if (reply_status == 0) {
            wire_dir->count = count;
            for (size_t i = 0; i < count; i++) {
                wire_dir->entries[i].object_id = entries[i].object_id;
                wire_dir->entries[i].kind = entries[i].mode & 0170000u;
                wire_dir->entries[i].name_len = strlen(entries[i].name);
                snprintf(wire_dir->entries[i].name, sizeof(wire_dir->entries[i].name), "%s", entries[i].name);
            }
            result = count;
        }
        break;
    }
    default:
        reply_status = -95;
        break;
    }

    if (mapped != NULL) {
        (void)pacha_munmap(mapped, KOBOXD_WIRE_FS_PAGE_BYTES);
    }
    if (vmo_fd >= 16) {
        (void)pacha_fd_close(vmo_fd);
    }

    status = send_status_reply_ex(endpoint->endpoint_fd, request.word3, reply_status, result);
    return status;
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
    if (serve_control_get_endpoint(ipc_service, (int)bootstrap->control_fd, KOBOXD_WIRE_ENDPOINT_BLOCK) != 0) {
        return -13;
    }
    if (serve_block_identify(ipc_service, &block_service) != 0) {
        return -14;
    }
    if (serve_control_get_endpoint(ipc_service, (int)bootstrap->control_fd, KOBOXD_WIRE_ENDPOINT_FS_BACKEND) != 0) {
        return -15;
    }
    for (;;) {
        if (serve_fs_backend_once(ipc_service, &fs_backend) != 0) {
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
