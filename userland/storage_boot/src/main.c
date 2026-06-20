#define _GNU_SOURCE

#include "kobox/device_pachaos_capsule.h"
#include "kobox/module.h"
#include "kobox/shim.h"
#include "linux_subsystem/block/block.h"
#include "linux_subsystem/fs/fs.h"
#include "pacha/abi.h"
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
    STORAGE_BOOT_MAX_ROOTFS_ELF_BYTES = 16 * 1024 * 1024,
    STORAGE_BOOT_READ_ALLOC_CHUNK_BYTES = 1024 * 1024,
    STORAGE_BOOT_PAGE_SIZE = 4096,
    STORAGE_BOOT_ELF64_EHDR_BYTES = 64,
    STORAGE_BOOT_ELF64_PHDR_BYTES = 56,
    STORAGE_BOOT_ELF_CLASS_64 = 2,
    STORAGE_BOOT_ELF_DATA_LSB = 1,
    STORAGE_BOOT_ELF_VERSION_CURRENT = 1,
    STORAGE_BOOT_ELF_TYPE_EXEC = 2,
    STORAGE_BOOT_ELF_TYPE_DYN = 3,
    STORAGE_BOOT_ELF_MACHINE_X86_64 = 0x3e,
    STORAGE_BOOT_ELF_PT_LOAD = 1,
    STORAGE_BOOT_ELF_PF_X = 1,
    STORAGE_BOOT_ELF_PF_W = 2,
    STORAGE_BOOT_ELF_PF_R = 4,
    STORAGE_BOOT_SEED0ROOT_BOOTSTRAP_MAGIC = 0x305254424f4f5453ull,
    STORAGE_BOOT_BOOTSTRAP_MAX_MODULES = 8,
    STORAGE_BOOT_BOOTSTRAP_NAME_BYTES = 64,
    STORAGE_BOOT_AT_NULL = 0,
    STORAGE_BOOT_AT_PHDR = 3,
    STORAGE_BOOT_AT_PHENT = 4,
    STORAGE_BOOT_AT_PHNUM = 5,
    STORAGE_BOOT_AT_PAGESZ = 6,
    STORAGE_BOOT_AT_BASE = 7,
    STORAGE_BOOT_AT_RANDOM = 25,
    STORAGE_BOOT_AT_EXECFN = 31,
    STORAGE_BOOT_ROOTFS_GPT_PARTITION_INDEX = 2,
    STORAGE_BOOT_FD_RIGHT_INSPECT = 1ull << 0,
    STORAGE_BOOT_FD_RIGHT_TRANSFER = 1ull << 2,
    STORAGE_BOOT_FD_RIGHT_WAIT = 1ull << 3,
    STORAGE_BOOT_FD_RIGHT_SET_FLAGS = 1ull << 5,
    STORAGE_BOOT_FD_RIGHT_CLOSE = 1ull << 6,
    STORAGE_BOOT_FD_RIGHT_MAP_READ = 1ull << 13,
    STORAGE_BOOT_FD_RIGHT_MAP_WRITE = 1ull << 14,
    STORAGE_BOOT_FD_RIGHT_MAP_EXEC = 1ull << 15,
    STORAGE_BOOT_FD_RIGHT_SPAWN = 1ull << 20,
    STORAGE_BOOT_FD_RIGHT_START = 1ull << 21,
    STORAGE_BOOT_FD_RIGHT_KILL = 1ull << 22,
    STORAGE_BOOT_FD_RIGHT_MAP_INTO = 1ull << 24,
    STORAGE_BOOT_FD_RIGHT_SET_CONTEXT = 1ull << 25,
    STORAGE_BOOT_FD_RIGHT_READ = 1ull << 42,
    STORAGE_BOOT_FD_FCNTL_SET_FLAGS = 2,
    STORAGE_BOOT_FD_FLAG_INHERIT = 1u << 2,
    STORAGE_BOOT_MMAP_SHARED = 1ull << 3,
    STORAGE_BOOT_PROT_READ = 1ull << 0,
    STORAGE_BOOT_PROT_WRITE = 1ull << 1,
    STORAGE_BOOT_PROT_EXEC = 1ull << 2,
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
static const char storage_boot_seed0root_file[] = "sbin/seed0root.elf";
static const char storage_boot_seed0root_argv0[] = "/sbin/seed0root.elf";
static const char storage_boot_koboxd_file[] = "sbin/koboxd.elf";
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

struct storage_boot_bootstrap_module {
    char name[STORAGE_BOOT_BOOTSTRAP_NAME_BYTES];
    uint64_t image_fd;
    uint64_t image_size;
};

struct storage_boot_seed0root_bootstrap {
    uint64_t magic;
    uint64_t device_fd;
    uint64_t koboxd_image_fd;
    uint64_t koboxd_image_size;
    uint64_t module_count;
    struct storage_boot_bootstrap_module modules[STORAGE_BOOT_BOOTSTRAP_MAX_MODULES];
};

struct storage_boot_rootfs_module {
    const char *name;
    const char *path;
};

struct storage_boot_module_image {
    const char *name;
    const char *path;
    unsigned char *data;
    uint64_t size;
    int image_fd;
};

static const struct storage_boot_rootfs_module storage_boot_rootfs_modules[] = {
    { "nvme-auth.ko", "/usr/lib/kobox/nvme-auth.ko" },
    { "nvme-core.ko", "/usr/lib/kobox/nvme-core.ko" },
    { "nvme.ko", "/usr/lib/kobox/nvme.ko" },
    { "crc16.ko", "/usr/lib/kobox/crc16.ko" },
    { "mbcache.ko", "/usr/lib/kobox/mbcache.ko" },
    { "jbd2.ko", "/usr/lib/kobox/jbd2.ko" },
    { "ext4.ko", "/usr/lib/kobox/ext4.ko" },
};

static const char *status_name(kb_status_t status);

int pacha_process_create(uint64_t rights, uint32_t flags);
int pacha_thread_create(int process_fd, uint64_t entry_rip, uint64_t stack_rsp, uint64_t flags, uint64_t fs_base, uint64_t rights);
int pacha_thread_start(int thread_fd);
long pacha_process_map(int process_fd, int vmo_fd, uint64_t target_va, uint64_t size, uint64_t prot, uint64_t vmo_offset);
int pacha_fd_close(int fd);
long pacha_fd_read(int fd, void *buf, uint64_t len);
long pacha_fd_fcntl(int fd, uint64_t cmd, uint64_t arg0, uint64_t arg1);
int pacha_vmo_create(uint64_t size, uint64_t rights, uint32_t flags);
void *pacha_mmap(int fd, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset);
int pacha_munmap(void *addr, uint64_t size);

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

static int mark_fd_inherit(int fd, const char *label)
{
    if (fd < 16) {
        return -1;
    }
    const long status = pacha_fd_fcntl(
        fd,
        STORAGE_BOOT_FD_FCNTL_SET_FLAGS,
        STORAGE_BOOT_FD_FLAG_INHERIT,
        STORAGE_BOOT_FD_FLAG_INHERIT);
    if (status != 0) {
        fprintf(stderr, "[storage_boot] %s: mark fd inherit failed fd=%d status=%ld\n",
            label,
            fd,
            status);
        return -2;
    }
    return 0;
}

static int create_inherited_vmo_from_bytes(const void *data, uint64_t size, const char *label)
{
    if (data == NULL || size == 0) {
        return -1;
    }
    if (size > UINT64_MAX - (STORAGE_BOOT_PAGE_SIZE - 1)) {
        return -2;
    }
    const uint64_t map_size = (size + (STORAGE_BOOT_PAGE_SIZE - 1)) & ~(uint64_t)(STORAGE_BOOT_PAGE_SIZE - 1);
    const uint64_t rights =
        STORAGE_BOOT_FD_RIGHT_INSPECT |
        STORAGE_BOOT_FD_RIGHT_TRANSFER |
        STORAGE_BOOT_FD_RIGHT_SET_FLAGS |
        STORAGE_BOOT_FD_RIGHT_CLOSE |
        STORAGE_BOOT_FD_RIGHT_READ |
        STORAGE_BOOT_FD_RIGHT_MAP_READ |
        STORAGE_BOOT_FD_RIGHT_MAP_WRITE;
    const int fd = pacha_vmo_create(map_size, rights, STORAGE_BOOT_FD_FLAG_INHERIT);
    if (fd < 16) {
        fprintf(stderr, "[storage_boot] %s: vmo_create failed status=%d\n", label, fd);
        return -3;
    }
    unsigned char *mapped = pacha_mmap(
        fd,
        map_size,
        STORAGE_BOOT_PROT_READ | STORAGE_BOOT_PROT_WRITE,
        STORAGE_BOOT_MMAP_SHARED,
        0);
    if (mapped == NULL) {
        (void)pacha_fd_close(fd);
        return -4;
    }
    memset(mapped, 0, (size_t)map_size);
    memcpy(mapped, data, (size_t)size);
    (void)pacha_munmap(mapped, map_size);
    return fd;
}

static int read_fd_exact(int fd, void *out, uint64_t size, const char *label)
{
    if (fd < 16 || out == NULL || size == 0) {
        return -1;
    }
    unsigned char *cursor = out;
    uint64_t done = 0;
    while (done < size) {
        const long got = pacha_fd_read(fd, cursor + done, size - done);
        if (got <= 0) {
            fprintf(stderr, "[storage_boot] %s: fd_read failed fd=%d got=%ld done=%llu size=%llu\n",
                label,
                fd,
                got,
                (unsigned long long)done,
                (unsigned long long)size);
            return -2;
        }
        done += (uint64_t)got;
    }
    return 0;
}

static int find_bootstrap_fd(char **argv, int *out_fd)
{
    if (argv == NULL || out_fd == NULL) {
        return -1;
    }
    *out_fd = -1;
    char **p = argv;
    while (*p != NULL) {
        p++;
    }
    p++;
    while (*p != NULL) {
        p++;
    }
    p++;
    for (;;) {
        const uint64_t key = (uint64_t)(uintptr_t)p[0];
        const uint64_t value = (uint64_t)(uintptr_t)p[1];
        if (key == STORAGE_BOOT_AT_NULL) {
            break;
        }
        if (key == PACHA_AT_BOOTSTRAP_FD && value >= 16 && value <= UINT32_MAX) {
            *out_fd = (int)value;
            return 0;
        }
        p += 2;
    }
    return -2;
}

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr64(unsigned char *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) {
        p[i] = (unsigned char)(value >> (i * 8));
    }
}

static uint64_t align_down(uint64_t value)
{
    return value & ~(uint64_t)(STORAGE_BOOT_PAGE_SIZE - 1);
}

static int align_up(uint64_t value, uint64_t *out)
{
    if (value > UINT64_MAX - (STORAGE_BOOT_PAGE_SIZE - 1)) {
        return -1;
    }
    *out = (value + (STORAGE_BOOT_PAGE_SIZE - 1)) & ~(uint64_t)(STORAGE_BOOT_PAGE_SIZE - 1);
    return 0;
}

static uint64_t prot_from_elf_flags(uint32_t flags)
{
    uint64_t prot = 0;
    if ((flags & STORAGE_BOOT_ELF_PF_R) != 0) {
        prot |= STORAGE_BOOT_PROT_READ;
    }
    if ((flags & STORAGE_BOOT_ELF_PF_W) != 0) {
        prot |= STORAGE_BOOT_PROT_WRITE;
    }
    if ((flags & STORAGE_BOOT_ELF_PF_X) != 0) {
        prot |= STORAGE_BOOT_PROT_EXEC;
    }
    return prot;
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

    return out_ops->file_operations_has_read_iter &&
        out_ops->file_operations_has_write_iter &&
        out_ops->file_operations_has_fsync &&
        out_ops->dir_inode_operations_has_lookup &&
        out_ops->dir_operations_has_readdir;
}

static int ext4_lookup_name_at(
    kb_module_t *module,
    const storage_boot_ext4_operations_t *ops,
    void *parent_inode,
    void *parent_dentry,
    const char *name,
    void **out_inode,
    void **out_dentry)
{
    if (module == NULL ||
        ops == NULL ||
        ops->lookup == NULL ||
        parent_inode == NULL ||
        parent_dentry == NULL ||
        name == NULL ||
        out_inode == NULL ||
        out_dentry == NULL)
    {
        return -22;
    }
    *out_inode = NULL;
    *out_dentry = NULL;

    void *dentry = calloc(1, STORAGE_BOOT_FAKE_DENTRY_BYTES);
    if (dentry == NULL) {
        return -12;
    }

    write_u32_field(dentry, STORAGE_BOOT_DENTRY_FLAGS_OFFSET, 0);
    write_pointer_field(dentry, STORAGE_BOOT_DENTRY_PARENT_OFFSET, parent_dentry);
    write_u32_field(dentry, STORAGE_BOOT_DENTRY_NAME_HASH_OFFSET, 0);
    write_u32_field(dentry, STORAGE_BOOT_DENTRY_NAME_LEN_OFFSET, (uint32_t)strlen(name));
    write_pointer_field(dentry, STORAGE_BOOT_DENTRY_NAME_PTR_OFFSET, (void *)(uintptr_t)name);

    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(ops->lookup);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    void *(*lookup_fn)(void *, void *, unsigned int) = NULL;
    memcpy(&lookup_fn, &ops->lookup, sizeof(lookup_fn));
    void *result = lookup_fn(parent_inode, dentry, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    void *inode = read_pointer_field(dentry, STORAGE_BOOT_DENTRY_INODE_OFFSET);
    int ok = result == dentry && inode != NULL;
    if (ok) {
        *out_inode = inode;
        *out_dentry = dentry;
        return 0;
    }
    free(dentry);
    return -5;
}

static int ext4_lookup_root_name(
    kb_module_t *module,
    const storage_boot_ext4_operations_t *ops,
    const kb_fs_mount_path_probe_t *mount_probe,
    const char *name,
    void **out_inode)
{
    if (mount_probe == NULL || out_inode == NULL) {
        return -22;
    }
    void *dentry = NULL;
    int status = ext4_lookup_name_at(
        module,
        ops,
        mount_probe->root_inode,
        mount_probe->root_dentry,
        name,
        out_inode,
        &dentry);
    free(dentry);
    return status;
}

static int ext4_lookup_path(
    kb_module_t *module,
    const storage_boot_ext4_operations_t *ops,
    const kb_fs_mount_path_probe_t *mount_probe,
    const char *path,
    void **out_inode)
{
    if (mount_probe == NULL || path == NULL || out_inode == NULL) {
        return -22;
    }
    *out_inode = NULL;

    const char *cursor = path;
    while (*cursor == '/') {
        cursor++;
    }
    if (*cursor == '\0') {
        *out_inode = mount_probe->root_inode;
        return 0;
    }

    void *parent_inode = mount_probe->root_inode;
    void *parent_dentry = mount_probe->root_dentry;
    void *owned_parent_dentry = NULL;

    while (*cursor != '\0') {
        const char *slash = strchr(cursor, '/');
        const size_t name_len = slash == NULL ? strlen(cursor) : (size_t)(slash - cursor);
        if (name_len == 0 || name_len >= STORAGE_BOOT_BOOTSTRAP_NAME_BYTES) {
            free(owned_parent_dentry);
            return -22;
        }

        char name[STORAGE_BOOT_BOOTSTRAP_NAME_BYTES];
        memcpy(name, cursor, name_len);
        name[name_len] = '\0';

        void *child_inode = NULL;
        void *child_dentry = NULL;
        const int status = ext4_lookup_name_at(
            module,
            ops,
            parent_inode,
            parent_dentry,
            name,
            &child_inode,
            &child_dentry);
        if (status != 0) {
            free(owned_parent_dentry);
            return status;
        }

        if (owned_parent_dentry != NULL) {
            free(owned_parent_dentry);
        }
        parent_inode = child_inode;
        parent_dentry = child_dentry;
        owned_parent_dentry = child_dentry;

        if (slash == NULL) {
            break;
        }
        cursor = slash + 1;
        while (*cursor == '/') {
            cursor++;
        }
    }

    *out_inode = parent_inode;
    free(owned_parent_dentry);
    return 0;
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

    int ok = result == (long)expected_size &&
        memcmp(read_buffer, expected, expected_size) == 0;

    free(read_buffer);
    free(mapping);
    free(iter);
    free(kiocb);
    free(file);
    return ok ? 0 : -5;
}

static int ext4_file_read_alloc(
    kb_module_t *module,
    const storage_boot_ext4_operations_t *ops,
    void *inode,
    uint64_t offset,
    size_t max_size,
    unsigned char **out_data,
    uint64_t *out_size,
    const char *label)
{
    if (module == NULL || ops == NULL || ops->file_read_iter == NULL ||
        inode == NULL || out_data == NULL || out_size == NULL || label == NULL ||
        max_size == 0)
    {
        return -22;
    }
    *out_data = NULL;
    *out_size = 0;

    void *file = calloc(1, STORAGE_BOOT_FAKE_FILE_BYTES);
    void *kiocb = calloc(1, STORAGE_BOOT_FAKE_KIOCB_BYTES);
    void *iter = calloc(1, STORAGE_BOOT_FAKE_IOV_ITER_BYTES);
    void *mapping = calloc(1, STORAGE_BOOT_FAKE_MAPPING_BYTES);
    size_t capacity = max_size < STORAGE_BOOT_READ_ALLOC_CHUNK_BYTES ? max_size : STORAGE_BOOT_READ_ALLOC_CHUNK_BYTES;
    unsigned char *read_buffer = malloc(capacity);
    if (file == NULL || kiocb == NULL || iter == NULL || mapping == NULL || read_buffer == NULL) {
        free(read_buffer);
        free(mapping);
        free(iter);
        free(kiocb);
        free(file);
        return -12;
    }

    long (*read_iter_fn)(void *, void *) = NULL;
    memcpy(&read_iter_fn, &ops->file_read_iter, sizeof(read_iter_fn));

    uint64_t total = 0;
    for (unsigned chunk_index = 0; chunk_index < 1024 && total < max_size; chunk_index++) {
        if (total == capacity) {
            size_t next_capacity = capacity + STORAGE_BOOT_READ_ALLOC_CHUNK_BYTES;
            if (next_capacity > max_size) {
                next_capacity = max_size;
            }
            unsigned char *next_buffer = realloc(read_buffer, next_capacity);
            if (next_buffer == NULL) {
                total = 0;
                break;
            }
            read_buffer = next_buffer;
            capacity = next_capacity;
        }

        const uint64_t remaining = max_size - total;
        const uint64_t available = (uint64_t)capacity - total;
        const uint64_t request_size =
            remaining < available ? remaining : available;
        memset(file, 0, STORAGE_BOOT_FAKE_FILE_BYTES);
        memset(kiocb, 0, STORAGE_BOOT_FAKE_KIOCB_BYTES);
        memset(iter, 0, STORAGE_BOOT_FAKE_IOV_ITER_BYTES);
        memset(mapping, 0, STORAGE_BOOT_FAKE_MAPPING_BYTES);

        write_pointer_field(file, STORAGE_BOOT_FILE_MAPPING_OFFSET, mapping);
        write_pointer_field(file, STORAGE_BOOT_FILE_INODE_OFFSET, inode);
        write_pointer_field(kiocb, STORAGE_BOOT_KIOCB_FILE_OFFSET, file);
        write_u64_field(kiocb, STORAGE_BOOT_KIOCB_POS_OFFSET, offset + total);
        write_u32_field(kiocb, STORAGE_BOOT_KIOCB_FLAGS_OFFSET, 0);
        write_u64_field(iter, STORAGE_BOOT_IOV_ITER_COUNT_OFFSET, request_size);
        write_pointer_field(iter, STORAGE_BOOT_IOV_ITER_BUFFER_OFFSET, read_buffer + total);

        unsigned long old_gs = 0;
        unsigned long kernel_gs = kb_module_kernel_gs_for_address(ops->file_read_iter);
        int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
        long result = read_iter_fn(kiocb, iter);
        if (has_gs) {
            kb_shim_leave_kernel_gs(old_gs);
        }

        if (result < 0 && total > 0) {
            break;
        }
        if (result < 0 || (uint64_t)result > request_size) {
            fprintf(stderr,
                "[storage_boot] read failed label=%s chunk=%u offset=%llu request=%llu result=%ld total=%llu capacity=%zu\n",
                label,
                chunk_index,
                (unsigned long long)(offset + total),
                (unsigned long long)request_size,
                result,
                (unsigned long long)total,
                capacity);
            total = 0;
            break;
        }
        if (result == 0) {
            break;
        }
        total += (uint64_t)result;
    }

    free(mapping);
    free(iter);
    free(kiocb);
    free(file);

    if (total == 0 || total == max_size) {
        free(read_buffer);
        return -5;
    }

    *out_data = read_buffer;
    *out_size = total;
    return 0;
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

struct storage_boot_loaded_process {
    int process_fd;
    uint64_t runtime_entry;
    uint64_t phdr_va;
    uint64_t phent;
    uint64_t phnum;
    uint16_t load_segments;
};

static int validate_elf_header(const char *path, const unsigned char *image, uint64_t image_size)
{
    if (image == NULL || image_size < STORAGE_BOOT_ELF64_EHDR_BYTES) {
        return -1;
    }
    if (image[0] != 0x7f || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') {
        fprintf(stderr, "[storage_boot] exec: %s ELF magic invalid\n", path);
        return -2;
    }
    if (image[4] != STORAGE_BOOT_ELF_CLASS_64 || image[5] != STORAGE_BOOT_ELF_DATA_LSB ||
        image[6] != STORAGE_BOOT_ELF_VERSION_CURRENT) {
        fprintf(stderr,
            "[storage_boot] exec: %s unsupported ELF ident class=%u data=%u version=%u\n",
            path,
            image[4],
            image[5],
            image[6]);
        return -3;
    }
    const uint16_t e_type = rd16(image + 16);
    const uint16_t e_machine = rd16(image + 18);
    const uint32_t e_version = rd32(image + 20);
    const uint16_t e_phentsize = rd16(image + 54);
    const uint16_t e_phnum = rd16(image + 56);
    if ((e_type != STORAGE_BOOT_ELF_TYPE_EXEC && e_type != STORAGE_BOOT_ELF_TYPE_DYN) ||
        e_machine != STORAGE_BOOT_ELF_MACHINE_X86_64 ||
        e_version != STORAGE_BOOT_ELF_VERSION_CURRENT ||
        e_phentsize < STORAGE_BOOT_ELF64_PHDR_BYTES ||
        e_phnum == 0) {
        fprintf(stderr,
            "[storage_boot] exec: %s unsupported ELF type=%u machine=%04x version=%u phentsize=%u phnum=%u\n",
            path,
            e_type,
            e_machine,
            e_version,
            e_phentsize,
            e_phnum);
        return -4;
    }
    const uint64_t e_phoff = rd64(image + 32);
    const uint64_t phdr_bytes = (uint64_t)e_phentsize * e_phnum;
    if (e_phoff > image_size || phdr_bytes > image_size - e_phoff) {
        fprintf(stderr, "[storage_boot] exec: %s program headers out of range\n", path);
        return -5;
    }
    return 0;
}

static int map_elf_segment(
    const char *path,
    int process_fd,
    uint64_t target_va,
    const unsigned char *image,
    uint64_t image_size,
    const unsigned char *ph,
    uint16_t index,
    uint64_t *out_mapped_va)
{
    const uint32_t p_flags = rd32(ph + 4);
    const uint64_t p_offset = rd64(ph + 8);
    const uint64_t p_vaddr = rd64(ph + 16);
    const uint64_t p_filesz = rd64(ph + 32);
    const uint64_t p_memsz = rd64(ph + 40);
    if (p_memsz < p_filesz ||
        p_offset > image_size ||
        p_filesz > image_size - p_offset ||
        (p_memsz != 0 && p_vaddr > UINT64_MAX - p_memsz)) {
        fprintf(stderr, "[storage_boot] exec: %s invalid PT_LOAD[%u]\n", path, index);
        return -1;
    }
    if (p_memsz == 0) {
        return 0;
    }

    const uint64_t page_offset = p_vaddr - align_down(p_vaddr);
    uint64_t map_size = 0;
    if (align_up(page_offset + p_memsz, &map_size) != 0) {
        return -2;
    }

    const uint64_t vmo_rights =
        STORAGE_BOOT_FD_RIGHT_INSPECT |
        STORAGE_BOOT_FD_RIGHT_TRANSFER |
        STORAGE_BOOT_FD_RIGHT_CLOSE |
        STORAGE_BOOT_FD_RIGHT_MAP_READ |
        STORAGE_BOOT_FD_RIGHT_MAP_WRITE |
        STORAGE_BOOT_FD_RIGHT_MAP_EXEC;
    const int vmo_fd = pacha_vmo_create(map_size, vmo_rights, 0);
    if (vmo_fd < 16) {
        fprintf(stderr, "[storage_boot] exec: vmo_create failed segment=%u status=%d\n", index, vmo_fd);
        return -3;
    }
    unsigned char *mapped = pacha_mmap(vmo_fd, map_size, STORAGE_BOOT_PROT_READ | STORAGE_BOOT_PROT_WRITE, STORAGE_BOOT_MMAP_SHARED, 0);
    if (mapped == NULL) {
        fprintf(stderr, "[storage_boot] exec: mmap staging VMO failed segment=%u fd=%d\n", index, vmo_fd);
        (void)pacha_fd_close(vmo_fd);
        return -4;
    }

    memset(mapped, 0, (size_t)map_size);
    memcpy(mapped + page_offset, image + p_offset, (size_t)p_filesz);

    const uint64_t prot = prot_from_elf_flags(p_flags);
    const long map_result = pacha_process_map(process_fd, vmo_fd, target_va, map_size, prot, 0);
    (void)pacha_munmap(mapped, map_size);
    (void)pacha_fd_close(vmo_fd);
    if (map_result < 4096) {
        fprintf(stderr,
            "[storage_boot] exec: process_map failed segment=%u process_fd=%d status=%ld\n",
            index,
            process_fd,
            map_result);
        return -5;
    }
    if (out_mapped_va != NULL) *out_mapped_va = (uint64_t)map_result;

    return 0;
}

static int load_elf_process(
    const char *path,
    const unsigned char *image,
    uint64_t image_size,
    struct storage_boot_loaded_process *out)
{
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->process_fd = -1;
    int status = validate_elf_header(path, image, image_size);
    if (status != 0) {
        return status;
    }

    const uint64_t e_entry = rd64(image + 24);
    const uint16_t e_type = rd16(image + 16);
    const uint64_t e_phoff = rd64(image + 32);
    const uint16_t e_phentsize = rd16(image + 54);
    const uint16_t e_phnum = rd16(image + 56);
    uint64_t load_bias = 0;
    const int use_aslr = e_type == STORAGE_BOOT_ELF_TYPE_DYN;
    const uint64_t process_rights =
        STORAGE_BOOT_FD_RIGHT_INSPECT |
        STORAGE_BOOT_FD_RIGHT_CLOSE |
        STORAGE_BOOT_FD_RIGHT_WAIT |
        STORAGE_BOOT_FD_RIGHT_KILL |
        STORAGE_BOOT_FD_RIGHT_SPAWN |
        STORAGE_BOOT_FD_RIGHT_MAP_INTO |
        STORAGE_BOOT_FD_RIGHT_SET_CONTEXT;
    const int process_fd = pacha_process_create(process_rights, 0);
    if (process_fd < 16) {
        fprintf(stderr, "[storage_boot] exec: process_create failed status=%d\n", process_fd);
        return -7;
    }

    uint16_t load_count = 0;
    for (uint16_t i = 0; i < e_phnum; i++) {
        const unsigned char *ph = image + e_phoff + (uint64_t)i * e_phentsize;
        if (rd32(ph + 0) != STORAGE_BOOT_ELF_PT_LOAD) {
            continue;
        }
        const uint64_t p_vaddr = rd64(ph + 16);
        const uint64_t requested_va = (use_aslr && load_count == 0) ? 0 : align_down(p_vaddr + load_bias);
        uint64_t mapped_va = 0;
        status = map_elf_segment(path, process_fd, requested_va, image, image_size, ph, i, &mapped_va);
        if (status != 0) {
            (void)pacha_fd_close(process_fd);
            return status;
        }
        if (use_aslr && load_count == 0) {
            load_bias = mapped_va - align_down(p_vaddr);
        }
        load_count++;
    }
    if (load_count == 0) {
        fprintf(stderr, "[storage_boot] exec: %s has no PT_LOAD segments\n", path);
        (void)pacha_fd_close(process_fd);
        return -6;
    }

    out->process_fd = process_fd;
    out->runtime_entry = e_entry + load_bias;
    out->phdr_va = load_bias + e_phoff;
    out->phent = e_phentsize;
    out->phnum = e_phnum;
    out->load_segments = load_count;
    return 0;
}

static int push_u64(unsigned char *stack, uint64_t *sp, uint64_t value)
{
    if (*sp < 8) {
        return -1;
    }
    *sp -= 8;
    wr64(stack + *sp, value);
    return 0;
}

static int start_loaded_process(
    const struct storage_boot_loaded_process *loaded,
    const char *argv0,
    int bootstrap_fd)
{
    if (loaded == NULL || loaded->process_fd < 16 || loaded->runtime_entry == 0 ||
        loaded->phdr_va == 0 || loaded->phent == 0 || loaded->phnum == 0 || argv0 == NULL) {
        return -1;
    }

    const int process_fd = loaded->process_fd;
    const uint64_t stack_rights =
        STORAGE_BOOT_FD_RIGHT_INSPECT |
        STORAGE_BOOT_FD_RIGHT_TRANSFER |
        STORAGE_BOOT_FD_RIGHT_CLOSE |
        STORAGE_BOOT_FD_RIGHT_MAP_READ |
        STORAGE_BOOT_FD_RIGHT_MAP_WRITE;
    const int stack_fd = pacha_vmo_create(PACHA_PROCESS_DEFAULT_STACK_SIZE, stack_rights, 0);
    if (stack_fd < 16) {
        fprintf(stderr, "[storage_boot] exec: stack vmo_create failed status=%d\n", stack_fd);
        return -2;
    }
    unsigned char *stack = pacha_mmap(
        stack_fd,
        PACHA_PROCESS_DEFAULT_STACK_SIZE,
        STORAGE_BOOT_PROT_READ | STORAGE_BOOT_PROT_WRITE,
        STORAGE_BOOT_MMAP_SHARED,
        0);
    if (stack == NULL) {
        (void)pacha_fd_close(stack_fd);
        return -3;
    }
    memset(stack, 0, (size_t)PACHA_PROCESS_DEFAULT_STACK_SIZE);
    const long stack_map = pacha_process_map(
        process_fd,
        stack_fd,
        0,
        PACHA_PROCESS_DEFAULT_STACK_SIZE,
        STORAGE_BOOT_PROT_READ | STORAGE_BOOT_PROT_WRITE,
        0);
    if (stack_map < 4096) {
        fprintf(stderr, "[storage_boot] exec: stack process_map failed status=%ld\n", stack_map);
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -6;
    }
    const uint64_t stack_base = (uint64_t)stack_map;

    uint64_t sp = PACHA_PROCESS_DEFAULT_STACK_SIZE;
    const uint64_t argv0_len = (uint64_t)strlen(argv0) + 1;
    if (argv0_len > 256 || sp < argv0_len + 16) {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -4;
    }
    sp -= argv0_len;
    memcpy(stack + sp, argv0, (size_t)argv0_len);
    const uint64_t argv0_va = stack_base + sp;
    sp &= ~15ull;
    sp -= 16;
    const uint64_t random_va = stack_base + sp;
    for (unsigned i = 0; i < 16; i++) {
        stack[sp + i] = (unsigned char)(0x73u + i * 11u);
    }
    sp &= ~15ull;

    if (push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_NULL) != 0 ||
        (bootstrap_fd >= 16 && (push_u64(stack, &sp, (uint64_t)(uint32_t)bootstrap_fd) != 0 || push_u64(stack, &sp, PACHA_AT_BOOTSTRAP_FD) != 0)) ||
        push_u64(stack, &sp, argv0_va) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_EXECFN) != 0 ||
        push_u64(stack, &sp, random_va) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_RANDOM) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_BASE) != 0 ||
        push_u64(stack, &sp, STORAGE_BOOT_PAGE_SIZE) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_PAGESZ) != 0 ||
        push_u64(stack, &sp, loaded->phnum) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_PHNUM) != 0 ||
        push_u64(stack, &sp, loaded->phent) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_PHENT) != 0 ||
        push_u64(stack, &sp, loaded->phdr_va) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_PHDR) != 0 ||
        push_u64(stack, &sp, 0) != 0 ||
        push_u64(stack, &sp, 0) != 0 ||
        push_u64(stack, &sp, argv0_va) != 0 ||
        push_u64(stack, &sp, 1) != 0) {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -5;
    }

    (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
    (void)pacha_fd_close(stack_fd);

    const uint64_t thread_rights =
        STORAGE_BOOT_FD_RIGHT_INSPECT |
        STORAGE_BOOT_FD_RIGHT_CLOSE |
        STORAGE_BOOT_FD_RIGHT_WAIT |
        STORAGE_BOOT_FD_RIGHT_KILL |
        STORAGE_BOOT_FD_RIGHT_START |
        STORAGE_BOOT_FD_RIGHT_SET_CONTEXT;
    const int thread_fd = pacha_thread_create(process_fd, loaded->runtime_entry, stack_base + sp, 0, 0, thread_rights);
    if (thread_fd < 16) {
        fprintf(stderr, "[storage_boot] exec: thread_create failed status=%d\n", thread_fd);
        return -7;
    }
    const int start_status = pacha_thread_start(thread_fd);
    if (start_status != 0) {
        fprintf(stderr, "[storage_boot] exec: thread_start failed thread_fd=%d status=%d\n", thread_fd, start_status);
        (void)pacha_fd_close(thread_fd);
        return -8;
    }
    return 0;
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

static void free_bootstrap_module_images(struct storage_boot_module_image *images, size_t count)
{
    if (images == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(images[i].data);
        images[i].data = NULL;
        images[i].size = 0;
        if (images[i].image_fd >= 16) {
            (void)pacha_fd_close(images[i].image_fd);
            images[i].image_fd = -1;
        }
    }
}

static int read_bootstrap_module_images(
    kb_module_t *ext4_module,
    const storage_boot_ext4_operations_t *ops,
    const kb_fs_mount_path_probe_t *probe,
    struct storage_boot_module_image *images,
    size_t image_count)
{
    if (images == NULL || image_count > STORAGE_BOOT_BOOTSTRAP_MAX_MODULES) {
        return -22;
    }

    for (size_t i = 0; i < image_count; i++) {
        images[i].name = storage_boot_rootfs_modules[i].name;
        images[i].path = storage_boot_rootfs_modules[i].path;
        images[i].data = NULL;
        images[i].size = 0;
        images[i].image_fd = -1;

        void *inode = NULL;
        int status = ext4_lookup_path(
            ext4_module,
            ops,
            probe,
            images[i].path,
            &inode);
        if (status != 0) {
            fprintf(stderr, "[storage_boot] seed0root: lookup %s failed status=%d\n",
                images[i].path,
                status);
            return status;
        }

        status = ext4_file_read_alloc(
            ext4_module,
            ops,
            inode,
            0,
            STORAGE_BOOT_MAX_ROOTFS_ELF_BYTES,
            &images[i].data,
            &images[i].size,
            images[i].name);
        if (status != 0) {
            fprintf(stderr, "[storage_boot] seed0root: read %s failed status=%d\n",
                images[i].path,
                status);
            return status;
        }
        images[i].image_fd = create_inherited_vmo_from_bytes(images[i].data, images[i].size, images[i].name);
        if (images[i].image_fd < 16) {
            return -12;
        }
    }

    return 0;
}

static int launch_seed0root_from_ext4(
    kb_module_t *ext4_module,
    const kb_fs_mount_path_probe_t *probe,
    uint64_t device_fd)
{
    storage_boot_ext4_operations_t ops;
    if (!probe_ext4_operation_tables(ext4_module, &ops)) {
        fprintf(stderr, "[storage_boot] seed0root: ext4 operation table probe failed\n");
        return 21;
    }

    void *sbin_inode = NULL;
    void *sbin_dentry = NULL;
    int status = ext4_lookup_name_at(
        ext4_module,
        &ops,
        probe->root_inode,
        probe->root_dentry,
        "sbin",
        &sbin_inode,
        &sbin_dentry);
    if (status != 0) {
        fprintf(stderr, "[storage_boot] seed0root: lookup /sbin failed status=%d\n", status);
        return 22;
    }

    void *seed_inode = NULL;
    void *seed_dentry = NULL;
    status = ext4_lookup_name_at(
        ext4_module,
        &ops,
        sbin_inode,
        sbin_dentry,
        "seed0root.elf",
        &seed_inode,
        &seed_dentry);
    if (status != 0) {
        free(sbin_dentry);
        fprintf(stderr, "[storage_boot] seed0root: lookup %s failed status=%d\n",
            storage_boot_seed0root_file,
            status);
        return 23;
    }
    free(seed_dentry);

    void *koboxd_inode = NULL;
    void *koboxd_dentry = NULL;
    status = ext4_lookup_name_at(
        ext4_module,
        &ops,
        sbin_inode,
        sbin_dentry,
        "koboxd.elf",
        &koboxd_inode,
        &koboxd_dentry);
    free(sbin_dentry);
    if (status != 0) {
        fprintf(stderr, "[storage_boot] seed0root: lookup %s failed status=%d\n",
            storage_boot_koboxd_file,
            status);
        return 27;
    }
    free(koboxd_dentry);

    unsigned char *image = NULL;
    uint64_t image_size = 0;
    status = ext4_file_read_alloc(
        ext4_module,
        &ops,
        seed_inode,
        0,
        STORAGE_BOOT_MAX_ROOTFS_ELF_BYTES,
        &image,
        &image_size,
        "seed0root");
    if (status != 0) {
        fprintf(stderr, "[storage_boot] seed0root: read %s failed status=%d\n",
            storage_boot_seed0root_file,
            status);
        return 24;
    }
    unsigned char *koboxd_image = NULL;
    uint64_t koboxd_image_size = 0;
    status = ext4_file_read_alloc(
        ext4_module,
        &ops,
        koboxd_inode,
        0,
        STORAGE_BOOT_MAX_ROOTFS_ELF_BYTES,
        &koboxd_image,
        &koboxd_image_size,
        "koboxd");
    if (status != 0) {
        free(image);
        fprintf(stderr, "[storage_boot] seed0root: read %s failed status=%d\n",
            storage_boot_koboxd_file,
            status);
        return 28;
    }
    const int koboxd_image_fd = create_inherited_vmo_from_bytes(koboxd_image, koboxd_image_size, "koboxd.elf");
    if (koboxd_image_fd < 16) {
        free(image);
        free(koboxd_image);
        return 28;
    }

    enum {
        module_count = sizeof(storage_boot_rootfs_modules) / sizeof(storage_boot_rootfs_modules[0]),
    };
    struct storage_boot_module_image module_images[module_count];
    memset(module_images, 0, sizeof(module_images));
    status = read_bootstrap_module_images(
        ext4_module,
        &ops,
        probe,
        module_images,
        module_count);
    if (status != 0) {
        free(image);
        free(koboxd_image);
        (void)pacha_fd_close(koboxd_image_fd);
        free_bootstrap_module_images(module_images, module_count);
        return 30;
    }
    struct storage_boot_bootstrap_module module_table[module_count];
    memset(module_table, 0, sizeof(module_table));
    for (size_t i = 0; i < module_count; i++) {
        snprintf(module_table[i].name, sizeof(module_table[i].name), "%s", module_images[i].name);
        module_table[i].image_fd = (uint64_t)(uint32_t)module_images[i].image_fd;
        module_table[i].image_size = module_images[i].size;
    }

    const struct storage_boot_seed0root_bootstrap bootstrap = {
        .magic = STORAGE_BOOT_SEED0ROOT_BOOTSTRAP_MAGIC,
        .device_fd = device_fd,
        .koboxd_image_fd = (uint64_t)(uint32_t)koboxd_image_fd,
        .koboxd_image_size = koboxd_image_size,
        .module_count = module_count,
    };
    struct storage_boot_seed0root_bootstrap bootstrap_package = bootstrap;
    memcpy(bootstrap_package.modules, module_table, sizeof(module_table));
    free(koboxd_image);
    status = mark_fd_inherit((int)device_fd, "seed0root device fd");
    if (status != 0) {
        free(image);
        free_bootstrap_module_images(module_images, module_count);
        (void)pacha_fd_close(koboxd_image_fd);
        return 31;
    }

    const int bootstrap_fd = create_inherited_vmo_from_bytes(&bootstrap_package, sizeof(bootstrap_package), "seed0root bootstrap fd");
    if (bootstrap_fd < 16) {
        free(image);
        free_bootstrap_module_images(module_images, module_count);
        (void)pacha_fd_close(koboxd_image_fd);
        return 32;
    }

    struct storage_boot_loaded_process loaded;
    status = load_elf_process(storage_boot_seed0root_argv0, image, image_size, &loaded);
    free(image);
    if (status != 0) {
        (void)pacha_fd_close(bootstrap_fd);
        (void)pacha_fd_close(koboxd_image_fd);
        free_bootstrap_module_images(module_images, module_count);
        fprintf(stderr, "[storage_boot] seed0root: ELF load failed status=%d\n", status);
        return 25;
    }
    status = start_loaded_process(&loaded, storage_boot_seed0root_argv0, bootstrap_fd);
    free_bootstrap_module_images(module_images, module_count);
    (void)pacha_fd_close(bootstrap_fd);
    (void)pacha_fd_close(koboxd_image_fd);
    if (status != 0) {
        fprintf(stderr, "[storage_boot] seed0root: start failed status=%d\n", status);
        return 26;
    }
    printf("[storage_boot] seed0root started\n");
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
        if (module_cfg->image_fd < 16 || module_cfg->image_size == 0 || module_cfg->name[0] == '\0') {
            fprintf(stderr, "[storage_boot] invalid module slot=%llu\n", (unsigned long long)i);
            return 4;
        }
        void *module_bytes = malloc((size_t)module_cfg->image_size);
        if (module_bytes == NULL) {
            fprintf(stderr, "[storage_boot] module alloc failed name=%s bytes=%llu\n",
                module_cfg->name,
                (unsigned long long)module_cfg->image_size);
            return 4;
        }
        if (read_fd_exact((int)module_cfg->image_fd, module_bytes, module_cfg->image_size, module_cfg->name) != 0) {
            return 4;
        }
        const kb_module_image_t image = {
            .data = module_bytes,
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
        printf("[storage_boot] module starting name=%s\n", module_cfg->name);
        status = kb_module_call_init(modules[i], &init_result);
        if (status == KB_ERR_NOT_FOUND && i + 1u < cfg->module_count) {
            printf("[storage_boot] module ready name=%s init=missing\n", module_cfg->name);
            continue;
        }
        if (status != KB_OK || init_result != 0) {
            fprintf(stderr,
                "[storage_boot] %s init failed status=%s(%d) result=%d\n",
                module_cfg->name,
                status_name(status),
                status,
                init_result);
            return 5;
        }
        printf("[storage_boot] module ready name=%s\n", module_cfg->name);
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

    int bootstrap_fd = -1;
    if (find_bootstrap_fd(argv, &bootstrap_fd) != 0) {
        fprintf(stderr, "[storage_boot] bootstrap fd missing\n");
        return 2;
    }
    struct storage_boot_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (read_fd_exact(bootstrap_fd, &cfg, sizeof(cfg), "boot config") != 0 ||
        cfg.magic != STORAGE_BOOT_CONFIG_MAGIC ||
        cfg.device_fd < 16 ||
        cfg.module_count == 0 ||
        cfg.module_count > STORAGE_BOOT_MAX_MODULES) {
        fprintf(stderr,
            "[storage_boot] invalid boot config magic=0x%llx fd=%llu modules=%llu\n",
            (unsigned long long)cfg.magic,
            (unsigned long long)cfg.device_fd,
            (unsigned long long)cfg.module_count);
        return 2;
    }

    printf("[storage_boot] start modules=%llu\n",
        (unsigned long long)cfg.module_count);

    kb_device_backend_t *backend = NULL;
    kb_status_t status = kb_pachaos_capsule_device_create(cfg.device_fd, &backend);
    if (status != KB_OK || backend == NULL) {
        fprintf(stderr, "[storage_boot] device backend create failed status=%s(%d)\n",
            status_name(status),
            status);
        return 3;
    }

    kb_module_t *ext4_module = NULL;
    int load_status = load_modules(&cfg, backend, &ext4_module);
    if (load_status != 0) {
        return load_status;
    }
    if (ext4_module == NULL) {
        fprintf(stderr, "[storage_boot] ext4.ko was not loaded\n");
        return 5;
    }

    printf("[storage_boot] modules ready\n");
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
    printf("[storage_boot] disk ready\n");

    kb_fs_block_device_t *root_device = NULL;
    int fs_status = kb_fs_block_device_create_from_disk_gpt_partition(
        "rootfs-nvme",
        disk,
        STORAGE_BOOT_ROOTFS_GPT_PARTITION_INDEX,
        &root_device);
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
    (void)fs_status;
    if (probe.fill_super_result != 0 || probe.observed_ext4_magic != 0xef53u) {
        fprintf(stderr, "[storage_boot] ext4 rootfs probe failed\n");
        return 10;
    }

    printf("[storage_boot] rootfs ready fs=ext4 reads=%u\n", probe.block_read_count);

    int seed0root_status = launch_seed0root_from_ext4(ext4_module, &probe, cfg.device_fd);
    if (seed0root_status != 0) {
        return seed0root_status;
    }

    printf("[storage_boot] done\n");
    fflush(stdout);
    fflush(stderr);
    return 0;
}
