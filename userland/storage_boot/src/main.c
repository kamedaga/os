#define _GNU_SOURCE

#include "kobox/device_pachaos_capsule.h"
#include "kobox/module.h"
#include "kobox/shim.h"
#include "linux_subsystem/block/block.h"
#include "linux_subsystem/fs/fs.h"
#include "pacha/capsule.h"
#include "storage_boot/boot_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    STORAGE_BOOT_FAKE_FILE_BYTES = 512,
    STORAGE_BOOT_FAKE_KIOCB_BYTES = 128,
    STORAGE_BOOT_FAKE_IOV_ITER_BYTES = 128,
    STORAGE_BOOT_FAKE_MAPPING_BYTES = 256,
    STORAGE_BOOT_FAKE_DENTRY_BYTES = 512,
    STORAGE_BOOT_DENTRY_FLAGS_OFFSET = 0x0,
    STORAGE_BOOT_DENTRY_PARENT_OFFSET = 0x18,
    STORAGE_BOOT_DENTRY_NAME_HASH_OFFSET = 0x20,
    STORAGE_BOOT_DENTRY_NAME_LEN_OFFSET = 0x24,
    STORAGE_BOOT_DENTRY_NAME_PTR_OFFSET = 0x28,
    STORAGE_BOOT_DENTRY_INODE_OFFSET = 0x38,
    STORAGE_BOOT_FILE_PATH_DENTRY_OFFSET = 0x18,
    STORAGE_BOOT_FILE_MAPPING_OFFSET = 0x20,
    STORAGE_BOOT_FILE_INODE_OFFSET = 0x28,
    STORAGE_BOOT_KIOCB_FILE_OFFSET = 0x0,
    STORAGE_BOOT_KIOCB_POS_OFFSET = 0x8,
    STORAGE_BOOT_KIOCB_FLAGS_OFFSET = 0x20,
    STORAGE_BOOT_IOV_ITER_COUNT_OFFSET = 0x18,
    STORAGE_BOOT_IOV_ITER_BUFFER_OFFSET = 0x20,
};

static const char storage_boot_ext4_file[] = "hello.txt";
static const char storage_boot_ext4_initial[] = "storage_boot ext4 initial payload\n";
static const char storage_boot_ext4_written[] = "storage_boot ext4 write_iter payload\n";

typedef struct storage_boot_ext4_operations {
    void *dir_operations;
    void *file_operations;
    void *dir_inode_operations;
    void *readdir;
    void *file_read_iter;
    void *file_write_iter;
    void *file_fsync;
    void *lookup;
    int file_operations_has_read_iter;
    int file_operations_has_write_iter;
    int file_operations_has_fsync;
    int dir_inode_operations_has_lookup;
    int dir_operations_has_readdir;
} storage_boot_ext4_operations_t;

static const char *status_name(kb_status_t status);

static void write_pointer_field(void *base, size_t offset, void *value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void *read_pointer_field(const void *base, size_t offset)
{
    void *value = NULL;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static void write_u32_field(void *base, size_t offset, uint32_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static void write_u64_field(void *base, size_t offset, uint64_t value)
{
    memcpy((uint8_t *)base + offset, &value, sizeof(value));
}

static int module_symbol(kb_module_t *module, const char *name, void **out_address)
{
    kb_status_t status = kb_module_find_symbol(module, name, out_address);
    if (status != KB_OK || out_address == NULL || *out_address == NULL) {
        fprintf(stderr, "[storage_boot] ext4 missing symbol %s status=%s(%d)\n",
            name,
            status_name(status),
            status);
        return 0;
    }
    return 1;
}

static int table_contains_pointer(const void *table, size_t bytes, const void *target)
{
    if (table == NULL || target == NULL || bytes < sizeof(void *)) {
        return 0;
    }
    const uint8_t *cursor = (const uint8_t *)table;
    for (size_t offset = 0; offset + sizeof(void *) <= bytes; offset += sizeof(void *)) {
        void *entry = NULL;
        memcpy(&entry, cursor + offset, sizeof(entry));
        if (entry == target) {
            return 1;
        }
    }
    return 0;
}

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

static int probe_ext4_operation_tables(
    kb_module_t *module,
    storage_boot_ext4_operations_t *out_ops)
{
    if (module == NULL || out_ops == NULL) {
        return 0;
    }
    memset(out_ops, 0, sizeof(*out_ops));
    if (!module_symbol(module, "ext4_dir_operations", &out_ops->dir_operations) ||
        !module_symbol(module, "ext4_file_operations", &out_ops->file_operations) ||
        !module_symbol(module, "ext4_dir_inode_operations", &out_ops->dir_inode_operations) ||
        !module_symbol(module, "ext4_readdir", &out_ops->readdir) ||
        !module_symbol(module, "ext4_file_read_iter", &out_ops->file_read_iter) ||
        !module_symbol(module, "ext4_file_write_iter", &out_ops->file_write_iter) ||
        !module_symbol(module, "ext4_sync_file", &out_ops->file_fsync) ||
        !module_symbol(module, "ext4_lookup", &out_ops->lookup))
    {
        return 0;
    }

    out_ops->file_operations_has_read_iter =
        table_contains_pointer(out_ops->file_operations, 256u, out_ops->file_read_iter);
    out_ops->file_operations_has_write_iter =
        table_contains_pointer(out_ops->file_operations, 256u, out_ops->file_write_iter);
    out_ops->file_operations_has_fsync =
        table_contains_pointer(out_ops->file_operations, 256u, out_ops->file_fsync);
    out_ops->dir_inode_operations_has_lookup =
        table_contains_pointer(out_ops->dir_inode_operations, 256u, out_ops->lookup);
    out_ops->dir_operations_has_readdir =
        table_contains_pointer(out_ops->dir_operations, 256u, out_ops->readdir);

    printf("[storage_boot] ext4 ops readdir=%d read_iter=%d write_iter=%d fsync=%d lookup=%d\n",
        out_ops->dir_operations_has_readdir,
        out_ops->file_operations_has_read_iter,
        out_ops->file_operations_has_write_iter,
        out_ops->file_operations_has_fsync,
        out_ops->dir_inode_operations_has_lookup);

    return out_ops->file_operations_has_read_iter &&
        out_ops->file_operations_has_write_iter &&
        out_ops->file_operations_has_fsync &&
        out_ops->dir_inode_operations_has_lookup &&
        out_ops->dir_operations_has_readdir;
}

static int ext4_lookup_root_name(
    kb_module_t *module,
    const storage_boot_ext4_operations_t *ops,
    const kb_fs_mount_path_probe_t *mount_probe,
    const char *name,
    void **out_inode)
{
    if (module == NULL ||
        ops == NULL ||
        mount_probe == NULL ||
        ops->lookup == NULL ||
        mount_probe->root_inode == NULL ||
        mount_probe->root_dentry == NULL ||
        name == NULL ||
        out_inode == NULL)
    {
        return -22;
    }
    *out_inode = NULL;

    void *dentry = calloc(1, STORAGE_BOOT_FAKE_DENTRY_BYTES);
    if (dentry == NULL) {
        return -12;
    }

    write_u32_field(dentry, STORAGE_BOOT_DENTRY_FLAGS_OFFSET, 0);
    write_pointer_field(dentry, STORAGE_BOOT_DENTRY_PARENT_OFFSET, mount_probe->root_dentry);
    write_u32_field(dentry, STORAGE_BOOT_DENTRY_NAME_HASH_OFFSET, 0);
    write_u32_field(dentry, STORAGE_BOOT_DENTRY_NAME_LEN_OFFSET, (uint32_t)strlen(name));
    write_pointer_field(dentry, STORAGE_BOOT_DENTRY_NAME_PTR_OFFSET, (void *)(uintptr_t)name);

    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(ops->lookup);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    void *(*lookup_fn)(void *, void *, unsigned int) = NULL;
    memcpy(&lookup_fn, &ops->lookup, sizeof(lookup_fn));
    void *result = lookup_fn(mount_probe->root_inode, dentry, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    void *inode = read_pointer_field(dentry, STORAGE_BOOT_DENTRY_INODE_OFFSET);
    printf("[storage_boot] ext4 lookup name=%s result=%p dentry=%p inode=%p\n",
        name,
        result,
        dentry,
        inode);

    int ok = result == dentry && inode != NULL;
    if (ok) {
        *out_inode = inode;
    }
    free(dentry);
    return ok ? 0 : -5;
}

static int ext4_file_read_iter(
    kb_module_t *module,
    const storage_boot_ext4_operations_t *ops,
    void *inode,
    const void *expected,
    size_t expected_size,
    uint64_t offset,
    const char *label)
{
    if (module == NULL || ops == NULL || ops->file_read_iter == NULL ||
        inode == NULL || expected == NULL || label == NULL)
    {
        return -22;
    }

    void *file = calloc(1, STORAGE_BOOT_FAKE_FILE_BYTES);
    void *kiocb = calloc(1, STORAGE_BOOT_FAKE_KIOCB_BYTES);
    void *iter = calloc(1, STORAGE_BOOT_FAKE_IOV_ITER_BYTES);
    void *mapping = calloc(1, STORAGE_BOOT_FAKE_MAPPING_BYTES);
    uint8_t *read_buffer = calloc(1, expected_size + 1u);
    if (file == NULL || kiocb == NULL || iter == NULL || mapping == NULL || read_buffer == NULL) {
        free(read_buffer);
        free(mapping);
        free(iter);
        free(kiocb);
        free(file);
        return -12;
    }

    write_pointer_field(file, STORAGE_BOOT_FILE_MAPPING_OFFSET, mapping);
    write_pointer_field(file, STORAGE_BOOT_FILE_INODE_OFFSET, inode);
    write_pointer_field(kiocb, STORAGE_BOOT_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, STORAGE_BOOT_KIOCB_POS_OFFSET, offset);
    write_u32_field(kiocb, STORAGE_BOOT_KIOCB_FLAGS_OFFSET, 0);
    write_u64_field(iter, STORAGE_BOOT_IOV_ITER_COUNT_OFFSET, (uint64_t)expected_size);
    write_pointer_field(iter, STORAGE_BOOT_IOV_ITER_BUFFER_OFFSET, read_buffer);

    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(ops->file_read_iter);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    long (*read_iter_fn)(void *, void *) = NULL;
    memcpy(&read_iter_fn, &ops->file_read_iter, sizeof(read_iter_fn));
    long result = read_iter_fn(kiocb, iter);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    printf("[storage_boot] ext4 read_iter label=%s offset=%llu result=%ld sample=%.*s\n",
        label,
        (unsigned long long)offset,
        result,
        result > 0 ? (int)(result < 64 ? result : 64) : 0,
        read_buffer);

    int ok = result == (long)expected_size &&
        memcmp(read_buffer, expected, expected_size) == 0;

    free(read_buffer);
    free(mapping);
    free(iter);
    free(kiocb);
    free(file);
    return ok ? 0 : -5;
}

static int ext4_file_write_iter(
    kb_module_t *module,
    const storage_boot_ext4_operations_t *ops,
    void *inode,
    const void *payload,
    size_t payload_size,
    uint64_t offset,
    const char *label)
{
    if (module == NULL || ops == NULL || ops->file_write_iter == NULL ||
        inode == NULL || payload == NULL || label == NULL)
    {
        return -22;
    }

    void *file = calloc(1, STORAGE_BOOT_FAKE_FILE_BYTES);
    void *kiocb = calloc(1, STORAGE_BOOT_FAKE_KIOCB_BYTES);
    void *iter = calloc(1, STORAGE_BOOT_FAKE_IOV_ITER_BYTES);
    void *mapping = calloc(1, STORAGE_BOOT_FAKE_MAPPING_BYTES);
    if (file == NULL || kiocb == NULL || iter == NULL || mapping == NULL) {
        free(mapping);
        free(iter);
        free(kiocb);
        free(file);
        return -12;
    }

    write_pointer_field(file, STORAGE_BOOT_FILE_MAPPING_OFFSET, mapping);
    write_pointer_field(file, STORAGE_BOOT_FILE_INODE_OFFSET, inode);
    write_pointer_field(kiocb, STORAGE_BOOT_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, STORAGE_BOOT_KIOCB_POS_OFFSET, offset);
    write_u32_field(kiocb, STORAGE_BOOT_KIOCB_FLAGS_OFFSET, 0x2u);
    write_u64_field(iter, STORAGE_BOOT_IOV_ITER_COUNT_OFFSET, (uint64_t)payload_size);
    write_pointer_field(iter, STORAGE_BOOT_IOV_ITER_BUFFER_OFFSET, (void *)(uintptr_t)payload);

    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(ops->file_write_iter);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    long (*write_iter_fn)(void *, void *) = NULL;
    memcpy(&write_iter_fn, &ops->file_write_iter, sizeof(write_iter_fn));
    long result = write_iter_fn(kiocb, iter);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    printf("[storage_boot] ext4 write_iter label=%s offset=%llu result=%ld sample=%.*s\n",
        label,
        (unsigned long long)offset,
        result,
        result > 0 ? (int)(result < 64 ? result : 64) : 0,
        (const char *)payload);

    free(mapping);
    free(iter);
    free(kiocb);
    free(file);
    return result == (long)payload_size ? 0 : -5;
}

static int ext4_file_fsync(
    kb_module_t *module,
    const storage_boot_ext4_operations_t *ops,
    void *inode,
    int64_t start,
    int64_t end,
    const char *label)
{
    if (module == NULL || ops == NULL || ops->file_fsync == NULL ||
        inode == NULL || label == NULL)
    {
        return -22;
    }

    void *file = calloc(1, STORAGE_BOOT_FAKE_FILE_BYTES);
    void *mapping = calloc(1, STORAGE_BOOT_FAKE_MAPPING_BYTES);
    void *dentry = calloc(1, STORAGE_BOOT_FAKE_DENTRY_BYTES);
    if (file == NULL || mapping == NULL || dentry == NULL) {
        free(dentry);
        free(mapping);
        free(file);
        return -12;
    }

    write_pointer_field(dentry, 0, inode);
    write_pointer_field(file, STORAGE_BOOT_FILE_PATH_DENTRY_OFFSET, dentry);
    write_pointer_field(file, STORAGE_BOOT_FILE_MAPPING_OFFSET, mapping);
    write_pointer_field(file, STORAGE_BOOT_FILE_INODE_OFFSET, inode);

    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(ops->file_fsync);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    int (*fsync_fn)(void *, int64_t, int64_t, int) = NULL;
    memcpy(&fsync_fn, &ops->file_fsync, sizeof(fsync_fn));
    int result = fsync_fn(file, start, end, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    printf("[storage_boot] ext4 fsync label=%s start=%lld end=%lld result=%d\n",
        label,
        (long long)start,
        (long long)end,
        result);

    free(dentry);
    free(mapping);
    free(file);
    return result == 0 ? 0 : -5;
}

static int run_ext4_file_ops_smoke(kb_module_t *ext4_module, const kb_fs_mount_path_probe_t *probe)
{
    storage_boot_ext4_operations_t ops;
    if (!probe_ext4_operation_tables(ext4_module, &ops)) {
        fprintf(stderr, "[storage_boot] ext4 operation table probe failed\n");
        return 11;
    }

    void *inode = NULL;
    int op_status = ext4_lookup_root_name(ext4_module, &ops, probe, storage_boot_ext4_file, &inode);
    if (op_status != 0) {
        fprintf(stderr, "[storage_boot] ext4 lookup failed file=%s status=%d\n",
            storage_boot_ext4_file,
            op_status);
        return 12;
    }

    op_status = ext4_file_read_iter(
        ext4_module,
        &ops,
        inode,
        storage_boot_ext4_initial,
        sizeof(storage_boot_ext4_initial) - 1u,
        0,
        "initial");
    if (op_status != 0) {
        fprintf(stderr, "[storage_boot] ext4 initial read failed status=%d\n", op_status);
        return 13;
    }

    op_status = ext4_file_write_iter(
        ext4_module,
        &ops,
        inode,
        storage_boot_ext4_written,
        sizeof(storage_boot_ext4_written) - 1u,
        0,
        "overwrite");
    if (op_status != 0) {
        fprintf(stderr, "[storage_boot] ext4 write failed status=%d\n", op_status);
        return 14;
    }

    op_status = ext4_file_fsync(
        ext4_module,
        &ops,
        inode,
        0,
        (int64_t)(sizeof(storage_boot_ext4_written) - 2u),
        "overwrite");
    if (op_status != 0) {
        fprintf(stderr, "[storage_boot] ext4 fsync failed status=%d\n", op_status);
        return 15;
    }

    op_status = ext4_file_read_iter(
        ext4_module,
        &ops,
        inode,
        storage_boot_ext4_written,
        sizeof(storage_boot_ext4_written) - 1u,
        0,
        "post-write");
    if (op_status != 0) {
        fprintf(stderr, "[storage_boot] ext4 post-write read failed status=%d\n", op_status);
        return 16;
    }

    printf("[storage_boot] ext4 rootfs read/write OK\n");
    return 0;
}

static int load_modules(
    const struct storage_boot_config *cfg,
    kb_device_backend_t *backend,
    kb_module_t **out_ext4_module)
{
    kb_module_t *modules[STORAGE_BOOT_MAX_MODULES];
    memset(modules, 0, sizeof(modules));
    if (out_ext4_module != NULL) {
        *out_ext4_module = NULL;
    }
    for (uint64_t i = 0; i < cfg->module_count; i++) {
        const struct storage_boot_module_config *module_cfg = &cfg->modules[i];
        if (module_cfg->image_va == 0 || module_cfg->image_size == 0 || module_cfg->name[0] == '\0') {
            fprintf(stderr, "[storage_boot] invalid module slot=%llu\n", (unsigned long long)i);
            return 4;
        }
        const kb_module_image_t image = {
            .data = (const void *)(uintptr_t)module_cfg->image_va,
            .size = (size_t)module_cfg->image_size,
            .name = module_cfg->name,
        };
        kb_status_t status = kb_module_open_image(&image, backend, &modules[i]);
        if (status != KB_OK || modules[i] == NULL) {
            fprintf(stderr, "[storage_boot] %s open failed status=%s(%d)\n",
                module_cfg->name,
                status_name(status),
                status);
            return 4;
        }
        if (out_ext4_module != NULL && strcmp(module_cfg->name, "ext4.ko") == 0) {
            *out_ext4_module = modules[i];
        }

        int init_result = 0;
        printf("[storage_boot] %s init begin\n", module_cfg->name);
        fflush(stdout);
        status = kb_module_call_init(modules[i], &init_result);
        if (status == KB_ERR_NOT_FOUND && i + 1u < cfg->module_count) {
            printf("[storage_boot] %s has no init_module\n", module_cfg->name);
            continue;
        }
        printf("[storage_boot] %s init returned status=%s(%d) result=%d\n",
            module_cfg->name,
            status_name(status),
            status,
            init_result);
        fflush(stdout);
        if (status != KB_OK || init_result != 0) {
            fprintf(stderr,
                "[storage_boot] %s init failed status=%s(%d) result=%d\n",
                module_cfg->name,
                status_name(status),
                status,
                init_result);
            return 5;
        }
    }
    return 0;
}

static void *wait_for_first_disk(void)
{
    for (unsigned i = 0; i < 2048; i++) {
        kb_run_deferred_work();
        (void)kb_handle_any_irq(1000000ull);
        void *disk = kb_block_subsystem_first_registered_disk();
        if (disk != NULL) {
            return disk;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const struct storage_boot_config *cfg =
        (const struct storage_boot_config *)(uintptr_t)STORAGE_BOOT_CONFIG_VA;
    if (cfg->magic != STORAGE_BOOT_CONFIG_MAGIC ||
        cfg->version != STORAGE_BOOT_CONFIG_VERSION ||
        cfg->device_fd < 16 ||
        cfg->module_count == 0 ||
        cfg->module_count > STORAGE_BOOT_MAX_MODULES) {
        fprintf(stderr,
            "[storage_boot] invalid boot config magic=0x%llx version=%llu fd=%llu modules=%llu\n",
            (unsigned long long)cfg->magic,
            (unsigned long long)cfg->version,
            (unsigned long long)cfg->device_fd,
            (unsigned long long)cfg->module_count);
        return 2;
    }

    printf("[storage_boot] start device_fd=%llu modules=%llu loader=%s\n",
        (unsigned long long)cfg->device_fd,
        (unsigned long long)cfg->module_count,
        kb_module_loader_version());

    kb_device_backend_t *backend = NULL;
    kb_status_t status = kb_pachaos_capsule_device_create(cfg->device_fd, &backend);
    if (status != KB_OK || backend == NULL) {
        fprintf(stderr, "[storage_boot] device backend create failed status=%s(%d)\n",
            status_name(status),
            status);
        return 3;
    }

    for (unsigned bar_index = 0; bar_index < 2; bar_index++) {
        struct pacha_capsule_bar_info bar = {0};
        int bar_status = pacha_capsule_pci_bar_info((int)cfg->device_fd, bar_index, &bar);
        printf("[storage_boot] pci bar%u status=%d start=0x%llx end=0x%llx size=0x%llx flags=0x%llx\n",
            bar_index,
            bar_status,
            (unsigned long long)bar.start,
            (unsigned long long)bar.end,
            (unsigned long long)bar.size,
            (unsigned long long)bar.flags);
    }

    kb_module_t *ext4_module = NULL;
    int load_status = load_modules(cfg, backend, &ext4_module);
    if (load_status != 0) {
        return load_status;
    }
    if (ext4_module == NULL) {
        fprintf(stderr, "[storage_boot] ext4.ko was not loaded\n");
        return 5;
    }

    printf("[storage_boot] module stack loaded\n");
    kb_shim_set_device_backend(backend);
    void *disk = wait_for_first_disk();
    if (disk == NULL) {
        fprintf(stderr, "[storage_boot] NVMe module stack registered no disk\n");
        return 6;
    }

    unsigned char sector[512];
    memset(sector, 0, sizeof(sector));
    int read_status = kb_block_subsystem_disk_read(disk, 0, sector, sizeof(sector));
    if (read_status != 0) {
        fprintf(stderr, "[storage_boot] NVMe disk read sector0 failed status=%d\n", read_status);
        return 7;
    }
    printf("[storage_boot] NVMe disk read sector0=%02x %02x %02x %02x OK\n",
        sector[0],
        sector[1],
        sector[2],
        sector[3]);

    kb_fs_block_device_t *root_device = NULL;
    int fs_status = kb_fs_block_device_create_from_disk("rootfs-nvme", disk, &root_device);
    if (fs_status != 0 || root_device == NULL) {
        fprintf(stderr, "[storage_boot] rootfs block device create failed status=%d\n", fs_status);
        return 8;
    }
    fs_status = kb_fs_subsystem_set_mount_probe_block_device(root_device);
    if (fs_status != 0) {
        fprintf(stderr, "[storage_boot] rootfs mount probe device set failed status=%d\n", fs_status);
        return 9;
    }

    kb_fs_mount_path_probe_t probe;
    memset(&probe, 0, sizeof(probe));
    fs_status = kb_fs_subsystem_probe_registered_mount_path("ext4", &probe);
    printf("[storage_boot] ext4 probe status=%d init=%d get_tree=%d fill_super=%d magic=0x%04x reads=%u get_tree_bdev=%llu\n",
        fs_status,
        probe.init_result,
        probe.get_tree_result,
        probe.fill_super_result,
        probe.observed_ext4_magic,
        probe.block_read_count,
        (unsigned long long)probe.get_tree_bdev_calls);
    if (probe.fill_super_result != 0 || probe.observed_ext4_magic != 0xef53u) {
        fprintf(stderr, "[storage_boot] ext4 rootfs probe failed\n");
        return 10;
    }

    printf("[storage_boot] ext4 rootfs probe OK\n");
    int ext4_status = run_ext4_file_ops_smoke(ext4_module, &probe);
    if (ext4_status != 0) {
        return ext4_status;
    }

    fflush(stdout);
    fflush(stderr);
    for (;;) {
        __asm__ volatile("pause");
    }
}
