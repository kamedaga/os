#include "fs_backend.h"

#include "kobox/shim.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef KOBOXD_FS_TRACE_MUTATION
#define KOBOXD_FS_TRACE_MUTATION 0
#endif

#ifndef KOBOXD_FS_STAGE_TRACE
#define KOBOXD_FS_STAGE_TRACE 0
#endif

#if KOBOXD_FS_TRACE_MUTATION
#define KOBOXD_FS_TRACE(...) printf(__VA_ARGS__)
#else
#define KOBOXD_FS_TRACE(...) ((void)0)
#endif

#if KOBOXD_FS_STAGE_TRACE
#define KOBOXD_FS_STAGE(...) printf(__VA_ARGS__)
#else
#define KOBOXD_FS_STAGE(...) do { if (0) printf(__VA_ARGS__); } while (0)
#endif

static int fs_release_deferred_unlinked_objects(koboxd_fs_backend_t *backend);

static uint64_t fs_now_ns(void)
{
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t fs_elapsed_us(uint64_t start_ns, uint64_t end_ns)
{
    if (start_ns == 0 || end_ns < start_ns) {
        return 0;
    }
    return (end_ns - start_ns) / 1000ull;
}

enum {
    KOBOXD_FAKE_FILE_BYTES = 512,
    KOBOXD_FAKE_KIOCB_BYTES = 128,
    KOBOXD_FAKE_IOV_ITER_BYTES = 128,
    KOBOXD_FAKE_MAPPING_BYTES = 256,
    KOBOXD_FAKE_DENTRY_BYTES = 512,
    KOBOXD_FAKE_WRITEBACK_CONTROL_BYTES = 384,
    KOBOXD_DENTRY_FLAGS_OFFSET = 0x0,
    KOBOXD_DENTRY_PARENT_OFFSET = 0x18,
    KOBOXD_DENTRY_NAME_HASH_OFFSET = 0x20,
    KOBOXD_DENTRY_NAME_LEN_OFFSET = 0x24,
    KOBOXD_DENTRY_NAME_PTR_OFFSET = 0x28,
    KOBOXD_DENTRY_INODE_OFFSET = 0x30,
    KOBOXD_DENTRY_SB_OFFSET = 0x68,
    KOBOXD_DENTRY_NAME_STORAGE_OFFSET = 0x100,
    KOBOXD_DENTRY_NAME_STORAGE_BYTES = 256,
    KOBOXD_INODE_MODE_OFFSET = 0x0,
    KOBOXD_INODE_OP_OFFSET = 0x20,
    KOBOXD_INODE_SB_OFFSET = 0x28,
    KOBOXD_INODE_MAPPING_OFFSET = 0x30,
    KOBOXD_INODE_NUMBER_OFFSET = 0x40,
    KOBOXD_INODE_NLINK_OFFSET = 0x48,
    KOBOXD_INODE_SIZE_OFFSET = 0x50,
    KOBOXD_INODE_ATIME_SEC_OFFSET = 0x58,
    KOBOXD_INODE_MTIME_SEC_OFFSET = 0x60,
    KOBOXD_INODE_CTIME_SEC_OFFSET = 0x68,
    KOBOXD_INODE_ATIME_NSEC_OFFSET = 0x70,
    KOBOXD_INODE_MTIME_NSEC_OFFSET = 0x74,
    KOBOXD_INODE_CTIME_NSEC_OFFSET = 0x78,
    KOBOXD_INODE_RWSEM_OFFSET = 0x98,
    KOBOXD_INODE_RWSEM_HELD = 1,
    KOBOXD_INODE_BLOCKS_OFFSET = 0x88,
    KOBOXD_SUPER_BLOCK_FS_INFO_OFFSET = 0x380,
    KOBOXD_EXT4_SBI_CLUSTER_BITS_OFFSET = 0x54,
    KOBOXD_EXT4_SBI_SUPER_BUFFER_HEAD_OFFSET = 0x60,
    KOBOXD_EXT4_SBI_EXT4_SUPER_OFFSET = 0x68,
    KOBOXD_EXT4_SBI_FREECLUSTERS_COUNTER_OFFSET = 0xe0,
    KOBOXD_EXT4_SUPER_FREE_BLOCKS_COUNT_LO_OFFSET = 0xc,
    KOBOXD_EXT4_SUPER_FREE_BLOCKS_COUNT_HI_OFFSET = 0x158,
    KOBOXD_INODE_EXT4_DISKSIZE_BACK_OFFSET = 0x30,
    KOBOXD_INODE_EXT4_DATA_SEM_BACK_OFFSET = 0x28,
    KOBOXD_FILE_PATH_DENTRY_OFFSET = 0x18,
    KOBOXD_FILE_MAPPING_OFFSET = 0x20,
    KOBOXD_FILE_INODE_OFFSET = 0x28,
    KOBOXD_KIOCB_FILE_OFFSET = 0x0,
    KOBOXD_KIOCB_POS_OFFSET = 0x8,
    KOBOXD_KIOCB_FLAGS_OFFSET = 0x20,
    KOBOXD_IOV_ITER_COUNT_OFFSET = 0x18,
    KOBOXD_IOV_ITER_BUFFER_OFFSET = 0x20,
    KOBOXD_IOV_ITER_BUFFER_CAPACITY_OFFSET = 0x78,
    KOBOXD_INODE_OP_CREATE_OFFSET = 0x28,
    KOBOXD_INODE_OP_UNLINK_OFFSET = 0x38,
    KOBOXD_INODE_OP_MKDIR_OFFSET = 0x48,
    KOBOXD_INODE_OP_RMDIR_OFFSET = 0x50,
    KOBOXD_INODE_OP_RENAME_OFFSET = 0x60,
    KOBOXD_WRITEBACK_CONTROL_SYNC_MODE_OFFSET = 0x20,
    KOBOXD_WRITEBACK_CONTROL_WB_SYNC_ALL = 1,
    KOBOXD_MODE_REGULAR_0644 = 0100000 | 0644,
    KOBOXD_MODE_DIRECTORY_0755 = 0040000 | 0755,
    KOBOXD_MODE_TYPE_MASK = 0170000,
    KOBOXD_MODE_PERM_MASK = 07777,
    KOBOXD_TIME_UPDATE_ATIME = 1u << 0,
    KOBOXD_TIME_UPDATE_MTIME = 1u << 1,
    KOBOXD_OBJECT_DIRTY_METADATA = 1u << 0,
    KOBOXD_OBJECT_DIRTY_DATA = 1u << 1,
};

typedef struct koboxd_ext4_operations {
    void *dir_operations;
    void *file_operations;
    void *dir_inode_operations;
    void *readdir;
    void *file_read_iter;
    void *file_write_iter;
    void *file_fsync;
    void *lookup;
    void *create;
    void *unlink;
    void *mkdir;
    void *rmdir;
    void *rename;
    void *dirty_inode;
    void *write_inode;
    void *force_commit;
    void *truncate_inode;
    void *superblock_csum_set;
    void *evict_inode;
} koboxd_ext4_operations_t;

typedef enum koboxd_ext4_child_create_kind {
    KOBOXD_EXT4_CREATE_REGULAR,
    KOBOXD_EXT4_CREATE_DIRECTORY,
} koboxd_ext4_child_create_kind_t;

static int fs_file_writeback_inode(const koboxd_ext4_operations_t *ops, void *inode, int commit_metadata);
static int fs_file_fsync(const koboxd_ext4_operations_t *ops, void *inode);

static void koboxd_fs_lock_init(koboxd_fs_lock_t *lock)
{
    if (lock != NULL) {
        atomic_flag_clear_explicit(&lock->flag, memory_order_release);
    }
}

void koboxd_fs_backend_lock(koboxd_fs_backend_t *backend)
{
    if (backend == NULL) {
        return;
    }
    while (atomic_flag_test_and_set_explicit(&backend->lock.flag, memory_order_acquire)) {
    }
}

void koboxd_fs_backend_unlock(koboxd_fs_backend_t *backend)
{
    if (backend != NULL) {
        atomic_flag_clear_explicit(&backend->lock.flag, memory_order_release);
    }
}

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

static void write_le32(void *base, uint32_t value)
{
    uint8_t *bytes = (uint8_t *)base;
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t read_u32_field(const void *base, size_t offset)
{
    uint32_t value = 0;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static uint16_t read_u16_field(const void *base, size_t offset)
{
    uint16_t value = 0;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static uint64_t read_u64_field(const void *base, size_t offset)
{
    uint64_t value = 0;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static int64_t read_i64_field(const void *base, size_t offset)
{
    int64_t value = 0;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static void fill_object_from_inode(
    koboxd_fs_object_t *object,
    uint64_t object_id,
    uint64_t parent_object_id,
    void *inode,
    void *dentry,
    const char *name)
{
    if (object == NULL) {
        return;
    }
    memset(object, 0, sizeof(*object));
    object->object_id = object_id;
    object->parent_object_id = parent_object_id;
    object->inode_number = inode != NULL ? read_u64_field(inode, KOBOXD_INODE_NUMBER_OFFSET) : 0;
    object->inode = inode;
    object->dentry = dentry;
    object->mode = inode != NULL ? read_u16_field(inode, KOBOXD_INODE_MODE_OFFSET) : 0;
    object->nlink = inode != NULL ? read_u32_field(inode, KOBOXD_INODE_NLINK_OFFSET) : 0;
    object->size = inode != NULL ? read_u64_field(inode, KOBOXD_INODE_SIZE_OFFSET) : 0;
    object->blocks = inode != NULL ? read_u64_field(inode, KOBOXD_INODE_BLOCKS_OFFSET) : 0;
    object->atime_sec = inode != NULL ? read_i64_field(inode, KOBOXD_INODE_ATIME_SEC_OFFSET) : 0;
    object->atime_nsec = inode != NULL ? (int64_t)read_u32_field(inode, KOBOXD_INODE_ATIME_NSEC_OFFSET) : 0;
    object->mtime_sec = inode != NULL ? read_i64_field(inode, KOBOXD_INODE_MTIME_SEC_OFFSET) : 0;
    object->mtime_nsec = inode != NULL ? (int64_t)read_u32_field(inode, KOBOXD_INODE_MTIME_NSEC_OFFSET) : 0;
    object->ctime_sec = inode != NULL ? read_i64_field(inode, KOBOXD_INODE_CTIME_SEC_OFFSET) : 0;
    object->ctime_nsec = inode != NULL ? (int64_t)read_u32_field(inode, KOBOXD_INODE_CTIME_NSEC_OFFSET) : 0;
    if (name != NULL) {
        snprintf(object->name, sizeof(object->name), "%s", name);
    }
    object->used = 1;
    object->linked = 1;
}

static koboxd_fs_object_t *fs_object_by_id(koboxd_fs_backend_t *backend, uint64_t object_id)
{
    if (backend == NULL || object_id == 0) {
        return NULL;
    }
    for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS; i++) {
        if (backend->objects[i].used && backend->objects[i].object_id == object_id) {
            return &backend->objects[i];
        }
    }
    return NULL;
}

static koboxd_fs_object_t *fs_object_by_parent_name(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name)
{
    if (backend == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS; i++) {
        if (backend->objects[i].used &&
            backend->objects[i].linked &&
            backend->objects[i].parent_object_id == parent_object_id &&
            strcmp(backend->objects[i].name, name) == 0)
        {
            return &backend->objects[i];
        }
    }
    return NULL;
}

static int fs_object_register(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    void *inode,
    void *dentry,
    const char *name,
    uint64_t *out_object_id)
{
    if (backend == NULL || inode == NULL || name == NULL || out_object_id == NULL) {
        return -22;
    }
    for (unsigned int attempt = 0; attempt < 2; attempt++) {
        for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS; i++) {
            if (!backend->objects[i].used) {
                const uint64_t object_id = backend->next_object_id++;
                fill_object_from_inode(&backend->objects[i], object_id, parent_object_id, inode, dentry, name);
                *out_object_id = object_id;
                return 0;
            }
        }
        if (backend->deferred_unlinked_count == 0) {
            break;
        }
        const int release_status = fs_release_deferred_unlinked_objects(backend);
        if (release_status != 0) {
            return release_status;
        }
    }
    return -12;
}

static void fs_discard_unregistered_lookup(void *inode, void *dentry)
{
    free(dentry);
    kb_fs_subsystem_free_fake_inode(inode);
}

static void fs_object_unregister(koboxd_fs_backend_t *backend, uint64_t object_id)
{
    koboxd_fs_object_t *object = fs_object_by_id(backend, object_id);
    if (object != NULL && object->object_id != 1) {
        memset(object, 0, sizeof(*object));
    }
}

static void fs_object_refresh(koboxd_fs_object_t *object)
{
    char name[KOBOXD_FS_BACKEND_NAME_BYTES];
    uint64_t parent_object_id;
    uint8_t linked;
    uint8_t dirty;
    if (object == NULL || !object->used) {
        return;
    }
    snprintf(name, sizeof(name), "%s", object->name);
    parent_object_id = object->parent_object_id;
    linked = object->linked;
    dirty = object->dirty;
    fill_object_from_inode(
        object,
        object->object_id,
        parent_object_id,
        object->inode,
        object->dentry,
        name);
    object->linked = linked;
    object->dirty = dirty;
}

static int module_symbol(kb_module_t *module, const char *name, void **out_address)
{
    kb_status_t status = kb_module_find_symbol(module, name, out_address);
    if (status != KB_OK || out_address == NULL || *out_address == NULL) {
        fprintf(stderr, "[filed-storage] fs-backend missing symbol %s status=%s(%d)\n",
            name,
            status_name(status),
            status);
        return 0;
    }
    return 1;
}

static int enter_ext4_call(void *function, unsigned long *old_gs)
{
    if (function == NULL || old_gs == NULL) {
        return 0;
    }
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(function);
    return kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, old_gs) == 0;
}

static void prepare_named_dentry(
    void *dentry,
    void *parent,
    void *inode,
    const char *name)
{
    if (dentry == NULL || name == NULL) {
        return;
    }
    memset(dentry, 0, KOBOXD_FAKE_DENTRY_BYTES);
    size_t name_len = strlen(name);
    if (name_len >= KOBOXD_DENTRY_NAME_STORAGE_BYTES) {
        name_len = KOBOXD_DENTRY_NAME_STORAGE_BYTES - 1u;
    }
    char *stable_name = (char *)dentry + KOBOXD_DENTRY_NAME_STORAGE_OFFSET;
    memcpy(stable_name, name, name_len);
    stable_name[name_len] = '\0';
    write_u32_field(dentry, KOBOXD_DENTRY_FLAGS_OFFSET, 0);
    write_pointer_field(dentry, KOBOXD_DENTRY_PARENT_OFFSET, parent == NULL ? dentry : parent);
    write_u32_field(dentry, KOBOXD_DENTRY_NAME_HASH_OFFSET, 0);
    write_u32_field(dentry, KOBOXD_DENTRY_NAME_LEN_OFFSET, (uint32_t)name_len);
    write_pointer_field(dentry, KOBOXD_DENTRY_NAME_PTR_OFFSET, stable_name);
    if (inode != NULL) {
        write_pointer_field(dentry, KOBOXD_DENTRY_INODE_OFFSET, inode);
        write_pointer_field(dentry, KOBOXD_DENTRY_SB_OFFSET, read_pointer_field(inode, KOBOXD_INODE_SB_OFFSET));
    } else if (parent != NULL) {
        write_pointer_field(dentry, KOBOXD_DENTRY_SB_OFFSET, read_pointer_field(parent, KOBOXD_DENTRY_SB_OFFSET));
    }
}

static int load_ext4_operation_tables(kb_module_t *module, koboxd_ext4_operations_t *out_ops)
{
    static kb_module_t *cached_module;
    static koboxd_ext4_operations_t cached_ops;
    static int cached_ready;
    if (module == NULL || out_ops == NULL) {
        return 0;
    }
    if (cached_ready && cached_module == module) {
        *out_ops = cached_ops;
        return 1;
    }
    memset(out_ops, 0, sizeof(*out_ops));
    if (!(module_symbol(module, "ext4_dir_operations", &out_ops->dir_operations) &&
        module_symbol(module, "ext4_file_operations", &out_ops->file_operations) &&
        module_symbol(module, "ext4_dir_inode_operations", &out_ops->dir_inode_operations) &&
        module_symbol(module, "ext4_readdir", &out_ops->readdir) &&
        module_symbol(module, "ext4_file_read_iter", &out_ops->file_read_iter) &&
        module_symbol(module, "ext4_file_write_iter", &out_ops->file_write_iter) &&
        module_symbol(module, "ext4_sync_file", &out_ops->file_fsync) &&
        module_symbol(module, "ext4_lookup", &out_ops->lookup) &&
        module_symbol(module, "ext4_dirty_inode", &out_ops->dirty_inode) &&
        module_symbol(module, "ext4_write_inode", &out_ops->write_inode) &&
        module_symbol(module, "ext4_force_commit", &out_ops->force_commit) &&
        module_symbol(module, "ext4_truncate", &out_ops->truncate_inode) &&
        module_symbol(module, "ext4_superblock_csum_set", &out_ops->superblock_csum_set) &&
        module_symbol(module, "ext4_evict_inode", &out_ops->evict_inode)))
    {
        return 0;
    }

    out_ops->create = read_pointer_field(out_ops->dir_inode_operations, KOBOXD_INODE_OP_CREATE_OFFSET);
    out_ops->unlink = read_pointer_field(out_ops->dir_inode_operations, KOBOXD_INODE_OP_UNLINK_OFFSET);
    out_ops->mkdir = read_pointer_field(out_ops->dir_inode_operations, KOBOXD_INODE_OP_MKDIR_OFFSET);
    out_ops->rmdir = read_pointer_field(out_ops->dir_inode_operations, KOBOXD_INODE_OP_RMDIR_OFFSET);
    out_ops->rename = read_pointer_field(out_ops->dir_inode_operations, KOBOXD_INODE_OP_RENAME_OFFSET);
    const int ready = out_ops->create != NULL &&
        out_ops->unlink != NULL &&
        out_ops->mkdir != NULL &&
        out_ops->rmdir != NULL &&
        out_ops->rename != NULL;
    if (ready) {
        cached_module = module;
        cached_ops = *out_ops;
        cached_ready = 1;
    }
    return ready;
}

static int fs_sync_super_free_blocks(const koboxd_ext4_operations_t *ops, void *super_block)
{
    (void)ops;
    if (super_block == NULL) {
        return -22;
    }
    return kb_fs_subsystem_ext4_sync_super_free_blocks(super_block);
}

static int fs_commit_superblock(const koboxd_ext4_operations_t *ops, void *super_block)
{
    if (ops == NULL || ops->force_commit == NULL || super_block == NULL) {
        return -22;
    }
    int (*force_commit_fn)(void *) = NULL;
    memcpy(&force_commit_fn, &ops->force_commit, sizeof(force_commit_fn));
    unsigned long old_gs = 0;
    int has_gs = enter_ext4_call(ops->force_commit, &old_gs);
    int commit_result = force_commit_fn(super_block);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (commit_result != 0) {
        return commit_result;
    }
    int group_result = kb_fs_subsystem_ext4_sync_group_free_counts(super_block);
    if (group_result != 0) {
        return group_result;
    }
    int super_result = fs_sync_super_free_blocks(ops, super_block);
    if (super_result != 0) {
        return super_result;
    }
    int buffer_result = kb_fs_subsystem_flush_dirty_buffers();
    if (buffer_result != 0) {
        return buffer_result;
    }
    return 0;
}

static void fs_mark_metadata_dirty(koboxd_fs_backend_t *backend)
{
    if (backend != NULL) {
        backend->metadata_dirty = 1;
    }
}

static void fs_mark_object_unlinked(koboxd_fs_backend_t *backend, koboxd_fs_object_t *object)
{
    if (backend == NULL || object == NULL) {
        return;
    }
    if (object->linked) {
        object->linked = 0;
        backend->deferred_unlinked_count++;
    }
}

static void fs_mark_object_metadata_dirty(koboxd_fs_backend_t *backend, koboxd_fs_object_t *object)
{
    if (object != NULL) {
        object->dirty |= KOBOXD_OBJECT_DIRTY_METADATA;
    }
    fs_mark_metadata_dirty(backend);
}

static void fs_mark_object_data_dirty(koboxd_fs_object_t *object)
{
    if (object != NULL) {
        object->dirty |= KOBOXD_OBJECT_DIRTY_DATA;
    }
}

static int fs_flush_dirty_object(
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *object,
    int commit_metadata)
{
    if (object == NULL || object->inode == NULL) {
        return -22;
    }
    if ((object->dirty & KOBOXD_OBJECT_DIRTY_DATA) != 0) {
        const int status = fs_file_fsync(ops, object->inode);
        if (status != 0) {
            return status;
        }
    }
    const int status = fs_file_writeback_inode(ops, object->inode, commit_metadata);
    if (status != 0) {
        return status;
    }
    object->dirty = 0;
    fs_object_refresh(object);
    return 0;
}

static uint16_t regular_create_mode(uint16_t mode)
{
    if (mode == 0) {
        return KOBOXD_MODE_REGULAR_0644;
    }
    if ((mode & 0170000u) == 0) {
        return (uint16_t)(0100000u | (mode & 07777u));
    }
    return mode;
}

static uint16_t directory_create_mode(uint16_t mode)
{
    return (uint16_t)(0040000u | (mode == 0 ? 0755u : (mode & 07777u)));
}

static int ext4_lookup_name_at(
    const koboxd_ext4_operations_t *ops,
    void *parent_inode,
    void *parent_dentry,
    const char *name,
    void **out_inode,
    void **out_dentry)
{
    if (ops == NULL || ops->lookup == NULL || parent_inode == NULL ||
        parent_dentry == NULL || name == NULL || out_inode == NULL || out_dentry == NULL)
    {
        return -22;
    }
    *out_inode = NULL;
    *out_dentry = NULL;

    void *dentry = calloc(1, KOBOXD_FAKE_DENTRY_BYTES);
    if (dentry == NULL) {
        return -12;
    }

    prepare_named_dentry(dentry, parent_dentry, NULL, name);

    unsigned long old_gs = 0;
    int has_gs = enter_ext4_call(ops->lookup, &old_gs);
    void *(*lookup_fn)(void *, void *, unsigned int) = NULL;
    memcpy(&lookup_fn, &ops->lookup, sizeof(lookup_fn));
    void *result = lookup_fn(parent_inode, dentry, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    void *inode = read_pointer_field(dentry, KOBOXD_DENTRY_INODE_OFFSET);
    if (inode == NULL) {
        free(dentry);
        return -2;
    }
    *out_inode = inode;
    *out_dentry = dentry;
    return 0;
}

static int fs_get_parent_object(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    koboxd_fs_object_t **out_parent)
{
    if (backend == NULL || out_parent == NULL) {
        return -22;
    }
    *out_parent = fs_object_by_id(backend, parent_object_id);
    if (*out_parent == NULL || (*out_parent)->inode == NULL || (*out_parent)->dentry == NULL) {
        return -2;
    }
    return 0;
}

static int fs_lookup_or_cache_child(
    koboxd_fs_backend_t *backend,
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *parent,
    const char *name,
    koboxd_fs_object_t **out_object)
{
    if (backend == NULL || ops == NULL || parent == NULL || name == NULL || out_object == NULL) {
        return -22;
    }
    *out_object = fs_object_by_parent_name(backend, parent->object_id, name);
    if (*out_object != NULL) {
        return 0;
    }

    void *lookup_inode = NULL;
    void *lookup_dentry = NULL;
    int lookup_status = ext4_lookup_name_at(
        ops,
        parent->inode,
        parent->dentry,
        name,
        &lookup_inode,
        &lookup_dentry);
    if (lookup_status != 0 || lookup_inode == NULL || lookup_dentry == NULL) {
        return lookup_status != 0 ? lookup_status : -2;
    }

    uint64_t object_id = 0;
    int status = fs_object_register(
        backend,
        parent->object_id,
        lookup_inode,
        lookup_dentry,
        name,
        &object_id);
    if (status != 0) {
        fs_discard_unregistered_lookup(lookup_inode, lookup_dentry);
        return status;
    }
    *out_object = fs_object_by_id(backend, object_id);
    return *out_object != NULL ? 0 : -2;
}

static int fs_call_ext4_child_create(
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *parent,
    const char *name,
    uint16_t mode,
    koboxd_ext4_child_create_kind_t kind,
    void **out_inode,
    void **out_dentry)
{
    if (ops == NULL || parent == NULL || parent->inode == NULL || parent->dentry == NULL ||
        name == NULL || out_inode == NULL || out_dentry == NULL)
    {
        return -22;
    }
    *out_inode = NULL;
    *out_dentry = NULL;

    void *dentry = calloc(1, KOBOXD_FAKE_DENTRY_BYTES);
    if (dentry == NULL) {
        return -12;
    }
    prepare_named_dentry(dentry, parent->dentry, NULL, name);

    static uint8_t mnt_idmap[136];
    unsigned long old_gs = 0;
    int has_gs = 0;
    int result = -22;
    const int defer_metadata = kind == KOBOXD_EXT4_CREATE_DIRECTORY;
    if (defer_metadata) {
        kb_fs_subsystem_begin_deferred_metadata_writes();
    }
    if (kind == KOBOXD_EXT4_CREATE_DIRECTORY) {
        int (*mkdir_fn)(void *, void *, void *, uint16_t) = NULL;
        memcpy(&mkdir_fn, &ops->mkdir, sizeof(mkdir_fn));
        has_gs = enter_ext4_call(ops->mkdir, &old_gs);
        result = mkdir_fn(mnt_idmap, parent->inode, dentry, directory_create_mode(mode));
    } else {
        int (*create_fn)(void *, void *, void *, uint16_t, int) = NULL;
        memcpy(&create_fn, &ops->create, sizeof(create_fn));
        has_gs = enter_ext4_call(ops->create, &old_gs);
        result = create_fn(mnt_idmap, parent->inode, dentry, regular_create_mode(mode), 0);
    }
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (defer_metadata) {
        const int flush_status = kb_fs_subsystem_end_deferred_metadata_writes();
        if (result == 0 && flush_status != 0) {
            result = flush_status;
        }
    }

    void *inode = read_pointer_field(dentry, KOBOXD_DENTRY_INODE_OFFSET);
    if (result != 0 || inode == NULL) {
        free(dentry);
        return result != 0 ? result : -5;
    }
    *out_inode = inode;
    *out_dentry = dentry;
    return 0;
}

static int fs_create_child_object(
    koboxd_fs_backend_t *backend,
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *parent,
    const char *name,
    uint16_t mode,
    koboxd_ext4_child_create_kind_t kind,
    uint64_t *out_object_id)
{
    if (backend == NULL || ops == NULL || parent == NULL || name == NULL || out_object_id == NULL) {
        return -22;
    }
    if (fs_object_by_parent_name(backend, parent->object_id, name) != NULL) {
        return -17;
    }

    void *inode = NULL;
    void *dentry = NULL;
    int status = fs_call_ext4_child_create(ops, parent, name, mode, kind, &inode, &dentry);
    if (status != 0) {
        return status;
    }
    status = fs_object_register(backend, parent->object_id, inode, dentry, name, out_object_id);
    if (status != 0) {
        fs_discard_unregistered_lookup(inode, dentry);
        return status;
    }
    fs_mark_metadata_dirty(backend);
    return 0;
}

static int fs_file_read(
    const koboxd_ext4_operations_t *ops,
    void *inode,
    uint64_t offset,
    void *buffer,
    size_t length,
    size_t buffer_capacity)
{
    (void)ops;
    if (inode == NULL || buffer == NULL) {
        return -22;
    }
    uint8_t file[KOBOXD_FAKE_FILE_BYTES];
    uint8_t kiocb[KOBOXD_FAKE_KIOCB_BYTES];
    uint8_t iter[KOBOXD_FAKE_IOV_ITER_BYTES];
    void *mapping = read_pointer_field(inode, KOBOXD_INODE_MAPPING_OFFSET);
    if (mapping == NULL) {
        return -12;
    }
    memset(file, 0, sizeof(file));
    memset(kiocb, 0, sizeof(kiocb));
    memset(iter, 0, sizeof(iter));

    write_pointer_field(file, KOBOXD_FILE_MAPPING_OFFSET, mapping);
    write_pointer_field(file, KOBOXD_FILE_INODE_OFFSET, inode);
    write_pointer_field(kiocb, KOBOXD_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, KOBOXD_KIOCB_POS_OFFSET, offset);
    write_u64_field(iter, KOBOXD_IOV_ITER_COUNT_OFFSET, (uint64_t)length);
    write_pointer_field(iter, KOBOXD_IOV_ITER_BUFFER_OFFSET, buffer);
    write_u64_field(iter, KOBOXD_IOV_ITER_BUFFER_CAPACITY_OFFSET, (uint64_t)buffer_capacity);

    long result = kb_fs_subsystem_generic_file_read_iter(kiocb, iter);
    return result >= 0 ? (int)result : (int)result;
}

static int fs_file_write(
    const koboxd_ext4_operations_t *ops,
    void *inode,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    if (ops == NULL || ops->file_write_iter == NULL || inode == NULL || buffer == NULL) {
        return -22;
    }
    uint8_t file[KOBOXD_FAKE_FILE_BYTES];
    uint8_t kiocb[KOBOXD_FAKE_KIOCB_BYTES];
    uint8_t iter[KOBOXD_FAKE_IOV_ITER_BYTES];
    uint8_t dentry[KOBOXD_FAKE_DENTRY_BYTES];
    void *mapping = read_pointer_field(inode, KOBOXD_INODE_MAPPING_OFFSET);
    if (mapping == NULL) {
        return -12;
    }
    memset(file, 0, sizeof(file));
    memset(kiocb, 0, sizeof(kiocb));
    memset(iter, 0, sizeof(iter));
    memset(dentry, 0, sizeof(dentry));

    write_pointer_field(dentry, 0, inode);
    write_pointer_field(file, KOBOXD_FILE_PATH_DENTRY_OFFSET, dentry);
    write_pointer_field(file, KOBOXD_FILE_MAPPING_OFFSET, mapping);
    write_pointer_field(file, KOBOXD_FILE_INODE_OFFSET, inode);
    write_pointer_field(kiocb, KOBOXD_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, KOBOXD_KIOCB_POS_OFFSET, offset);
    write_u64_field(iter, KOBOXD_IOV_ITER_COUNT_OFFSET, (uint64_t)length);
    write_pointer_field(iter, KOBOXD_IOV_ITER_BUFFER_OFFSET, (void *)(uintptr_t)buffer);
    write_u64_field(iter, KOBOXD_IOV_ITER_BUFFER_CAPACITY_OFFSET, (uint64_t)length);

    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(ops->file_write_iter);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    long (*write_iter_fn)(void *, void *) = NULL;
    memcpy(&write_iter_fn, &ops->file_write_iter, sizeof(write_iter_fn));
    long result = write_iter_fn(kiocb, iter);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    return result >= 0 ? (int)result : (int)result;
}

static int fs_file_writeback_inode(const koboxd_ext4_operations_t *ops, void *inode, int commit_metadata)
{
    if (ops == NULL || inode == NULL ||
        ops->dirty_inode == NULL ||
        ops->write_inode == NULL ||
        (commit_metadata && ops->force_commit == NULL))
    {
        return -22;
    }

    void *super_block = read_pointer_field(inode, KOBOXD_INODE_SB_OFFSET);
    if (commit_metadata && super_block == NULL) {
        return -22;
    }

    uint8_t writeback_control[KOBOXD_FAKE_WRITEBACK_CONTROL_BYTES];
    memset(writeback_control, 0, sizeof(writeback_control));
    write_u32_field(
        writeback_control,
        KOBOXD_WRITEBACK_CONTROL_SYNC_MODE_OFFSET,
        KOBOXD_WRITEBACK_CONTROL_WB_SYNC_ALL);

    void (*dirty_inode_fn)(void *, int) = NULL;
    int (*write_inode_fn)(void *, void *) = NULL;
    int (*force_commit_fn)(void *) = NULL;
    memcpy(&dirty_inode_fn, &ops->dirty_inode, sizeof(dirty_inode_fn));
    memcpy(&write_inode_fn, &ops->write_inode, sizeof(write_inode_fn));
    if (commit_metadata) {
        memcpy(&force_commit_fn, &ops->force_commit, sizeof(force_commit_fn));
    }

    unsigned long old_gs = 0;
    int has_gs = enter_ext4_call(ops->dirty_inode, &old_gs);
    dirty_inode_fn(inode, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    old_gs = 0;
    has_gs = enter_ext4_call(ops->write_inode, &old_gs);
    int write_result = write_inode_fn(inode, writeback_control);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (write_result != 0) {
        return write_result;
    }
    if (!commit_metadata) {
        return 0;
    }

    old_gs = 0;
    has_gs = enter_ext4_call(ops->force_commit, &old_gs);
    int commit_result = force_commit_fn(super_block);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (commit_result != 0) {
        return commit_result;
    }
    int group_result = kb_fs_subsystem_ext4_sync_group_free_counts(super_block);
    if (group_result != 0) {
        return group_result;
    }
    int buffer_result = kb_fs_subsystem_flush_dirty_buffers();
    if (buffer_result != 0) {
        return buffer_result;
    }
    return fs_sync_super_free_blocks(ops, super_block);
}

static int fs_file_fsync(const koboxd_ext4_operations_t *ops, void *inode)
{
    if (ops == NULL || ops->file_fsync == NULL || inode == NULL) {
        return -22;
    }
    void *file = calloc(1, KOBOXD_FAKE_FILE_BYTES);
    void *mapping = read_pointer_field(inode, KOBOXD_INODE_MAPPING_OFFSET);
    void *dentry = calloc(1, KOBOXD_FAKE_DENTRY_BYTES);
    if (file == NULL || mapping == NULL || dentry == NULL) {
        free(dentry);
        free(file);
        return -12;
    }

    write_pointer_field(dentry, 0, inode);
    write_pointer_field(file, KOBOXD_FILE_PATH_DENTRY_OFFSET, dentry);
    write_pointer_field(file, KOBOXD_FILE_MAPPING_OFFSET, mapping);
    write_pointer_field(file, KOBOXD_FILE_INODE_OFFSET, inode);

    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(ops->file_fsync);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    int (*fsync_fn)(void *, int64_t, int64_t, int) = NULL;
    memcpy(&fsync_fn, &ops->file_fsync, sizeof(fsync_fn));
    int result = fsync_fn(file, 0, INT64_MAX, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    free(dentry);
    free(file);
    return result;
}

static int fs_file_truncate(const koboxd_ext4_operations_t *ops, void *inode, uint64_t size)
{
    if (ops == NULL || inode == NULL ||
        ops->dirty_inode == NULL ||
        ops->write_inode == NULL ||
        ops->force_commit == NULL ||
        ops->truncate_inode == NULL)
    {
        return -22;
    }
    write_u64_field(inode, KOBOXD_INODE_SIZE_OFFSET, size);
    write_u64_field((uint8_t *)inode - KOBOXD_INODE_EXT4_DISKSIZE_BACK_OFFSET, 0, size);

    uint64_t saved_rwsem = 0;
    uint64_t saved_data_sem = 0;
    memcpy(&saved_rwsem, (const uint8_t *)inode + KOBOXD_INODE_RWSEM_OFFSET, sizeof(saved_rwsem));
    memcpy(&saved_data_sem, (const uint8_t *)inode - KOBOXD_INODE_EXT4_DATA_SEM_BACK_OFFSET, sizeof(saved_data_sem));
    write_u64_field(inode, KOBOXD_INODE_RWSEM_OFFSET, KOBOXD_INODE_RWSEM_HELD);
    write_u64_field((uint8_t *)inode - KOBOXD_INODE_EXT4_DATA_SEM_BACK_OFFSET, 0, KOBOXD_INODE_RWSEM_HELD);

    int (*truncate_fn)(void *) = NULL;
    memcpy(&truncate_fn, &ops->truncate_inode, sizeof(truncate_fn));
    unsigned long old_gs = 0;
    int has_gs = enter_ext4_call(ops->truncate_inode, &old_gs);
    int truncate_result = truncate_fn(inode);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    write_u64_field(inode, KOBOXD_INODE_RWSEM_OFFSET, saved_rwsem);
    write_u64_field((uint8_t *)inode - KOBOXD_INODE_EXT4_DATA_SEM_BACK_OFFSET, 0, saved_data_sem);
    if (truncate_result != 0) {
        return truncate_result;
    }
    return fs_file_writeback_inode(ops, inode, 1);
}

static int fs_file_update_metadata(
    void *inode,
    uint32_t time_mask,
    uint16_t mode,
    int update_mode,
    int64_t atime_sec,
    int64_t atime_nsec,
    int64_t mtime_sec,
    int64_t mtime_nsec)
{
    if (inode == NULL) {
        return -22;
    }
    if ((time_mask & ~((uint32_t)KOBOXD_TIME_UPDATE_ATIME | (uint32_t)KOBOXD_TIME_UPDATE_MTIME)) != 0) {
        return -22;
    }
    if (atime_nsec < 0 || atime_nsec >= 1000000000ll ||
        mtime_nsec < 0 || mtime_nsec >= 1000000000ll)
    {
        return -22;
    }
    if (update_mode) {
        const uint16_t old_mode = read_u16_field(inode, KOBOXD_INODE_MODE_OFFSET);
        const uint16_t new_mode = (uint16_t)((old_mode & KOBOXD_MODE_TYPE_MASK) | (mode & KOBOXD_MODE_PERM_MASK));
        memcpy((uint8_t *)inode + KOBOXD_INODE_MODE_OFFSET, &new_mode, sizeof(new_mode));
    }
    if ((time_mask & KOBOXD_TIME_UPDATE_ATIME) != 0) {
        write_u64_field(inode, KOBOXD_INODE_ATIME_SEC_OFFSET, (uint64_t)atime_sec);
        write_u32_field(inode, KOBOXD_INODE_ATIME_NSEC_OFFSET, (uint32_t)atime_nsec);
    }
    if ((time_mask & KOBOXD_TIME_UPDATE_MTIME) != 0) {
        write_u64_field(inode, KOBOXD_INODE_MTIME_SEC_OFFSET, (uint64_t)mtime_sec);
        write_u32_field(inode, KOBOXD_INODE_MTIME_NSEC_OFFSET, (uint32_t)mtime_nsec);
        write_u64_field(inode, KOBOXD_INODE_CTIME_SEC_OFFSET, (uint64_t)mtime_sec);
        write_u32_field(inode, KOBOXD_INODE_CTIME_NSEC_OFFSET, (uint32_t)mtime_nsec);
    } else if ((time_mask & KOBOXD_TIME_UPDATE_ATIME) != 0) {
        write_u64_field(inode, KOBOXD_INODE_CTIME_SEC_OFFSET, (uint64_t)atime_sec);
        write_u32_field(inode, KOBOXD_INODE_CTIME_NSEC_OFFSET, (uint32_t)atime_nsec);
    }
    return 0;
}

int koboxd_fs_backend_mount_ext4(
    koboxd_fs_backend_t *backend,
    kb_module_t *ext4_module,
    kb_fs_block_device_t *root_device)
{
    if (backend == NULL || ext4_module == NULL || root_device == NULL) {
        return -1;
    }
    memset(backend, 0, sizeof(*backend));
    koboxd_fs_lock_init(&backend->lock);
    int status = kb_fs_subsystem_set_mount_block_device(root_device);
    if (status != 0) {
        return status;
    }
    status = kb_fs_subsystem_mount_registered_root("ext4", &backend->mount_result);
    if (backend->mount_result.fill_super_result != 0 || backend->mount_result.observed_ext4_magic != 0xef53u) {
        return -5;
    }
    status = kb_fs_subsystem_ext4_sync_group_free_counts(backend->mount_result.super_block);
    if (status != 0) {
        return status;
    }
    printf("[filed-storage] rootfs mounted fs=ext4 reads=%u\n", backend->mount_result.block_read_count);
    backend->ext4_module = ext4_module;
    backend->next_object_id = 2;
    fill_object_from_inode(
        &backend->objects[0],
        1,
        1,
        backend->mount_result.root_inode,
        backend->mount_result.root_dentry,
        "/");
    backend->mounted = 1;
    return 0;
}

int koboxd_fs_backend_lookup(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint64_t *out_object_id)
{
    if (backend == NULL || name == NULL || out_object_id == NULL || !backend->mounted) {
        return -22;
    }
    *out_object_id = 0;
    koboxd_fs_object_t *cached = fs_object_by_parent_name(backend, parent_object_id, name);
    if (cached != NULL) {
        *out_object_id = cached->object_id;
        return 0;
    }
    koboxd_fs_object_t *parent = fs_object_by_id(backend, parent_object_id);
    if (parent == NULL || parent->inode == NULL || parent->dentry == NULL) {
        return -2;
    }

    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }

    void *inode = NULL;
    void *dentry = NULL;
    int status = ext4_lookup_name_at(&ops, parent->inode, parent->dentry, name, &inode, &dentry);
    if (status != 0) {
        return status;
    }
    return fs_object_register(backend, parent_object_id, inode, dentry, name, out_object_id);
}

int koboxd_fs_backend_create(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint16_t mode,
    uint64_t *out_object_id)
{
    if (backend == NULL || name == NULL || out_object_id == NULL || !backend->mounted) {
        return -22;
    }
    *out_object_id = 0;
    koboxd_fs_object_t *parent = NULL;
    int status = fs_get_parent_object(backend, parent_object_id, &parent);
    if (status != 0) {
        return status;
    }
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    KOBOXD_FS_TRACE("[koboxd-fs-trace] create parent=%llu name=%s mode=%o\n",
        (unsigned long long)parent_object_id,
        name,
        mode);
    int create_status = fs_create_child_object(
        backend,
        &ops,
        parent,
        name,
        mode,
        KOBOXD_EXT4_CREATE_REGULAR,
        out_object_id);
    KOBOXD_FS_TRACE("[koboxd-fs-trace] create done status=%d object=%llu\n",
        create_status,
        (unsigned long long)*out_object_id);
    return create_status;
}

int koboxd_fs_backend_truncate(
    koboxd_fs_backend_t *backend,
    uint64_t object_id,
    uint64_t size)
{
    if (backend == NULL || !backend->mounted) {
        return -22;
    }
    koboxd_fs_object_t *object = fs_object_by_id(backend, object_id);
    if (object == NULL || object->inode == NULL) {
        return -2;
    }
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    int status = fs_file_truncate(&ops, object->inode, size);
    if (status == 0) {
        fs_object_refresh(object);
    }
    return status;
}

int koboxd_fs_backend_utimens(
    koboxd_fs_backend_t *backend,
    uint64_t object_id,
    uint32_t mask,
    int64_t atime_sec,
    int64_t atime_nsec,
    int64_t mtime_sec,
    int64_t mtime_nsec)
{
    if (backend == NULL || !backend->mounted) {
        return -22;
    }
    koboxd_fs_object_t *object = fs_object_by_id(backend, object_id);
    if (object == NULL || object->inode == NULL) {
        return -2;
    }
    int status = fs_file_update_metadata(
        object->inode,
        mask,
        0,
        0,
        atime_sec,
        atime_nsec,
        mtime_sec,
        mtime_nsec);
    if (status == 0) {
        fs_mark_object_metadata_dirty(backend, object);
        fs_object_refresh(object);
    }
    return status;
}

int koboxd_fs_backend_chmod(
    koboxd_fs_backend_t *backend,
    uint64_t object_id,
    uint16_t mode)
{
    if (backend == NULL || !backend->mounted || (mode & ~KOBOXD_MODE_PERM_MASK) != 0) {
        return -22;
    }
    koboxd_fs_object_t *object = fs_object_by_id(backend, object_id);
    if (object == NULL || object->inode == NULL) {
        return -2;
    }
    int status = fs_file_update_metadata(
        object->inode,
        0,
        mode,
        1,
        0,
        0,
        0,
        0);
    if (status == 0) {
        fs_mark_object_metadata_dirty(backend, object);
        fs_object_refresh(object);
    }
    return status;
}

int koboxd_fs_backend_unlink(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name)
{
    if (backend == NULL || name == NULL || !backend->mounted) {
        return -22;
    }
    koboxd_fs_object_t *parent = NULL;
    int status = fs_get_parent_object(backend, parent_object_id, &parent);
    if (status != 0) {
        return status;
    }
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    koboxd_fs_object_t *object = NULL;
    status = fs_lookup_or_cache_child(backend, &ops, parent, name, &object);
    if (status != 0) {
        return status;
    }

    KOBOXD_FS_TRACE("[koboxd-fs-trace] unlink parent=%llu object=%llu name=%s\n",
        (unsigned long long)parent_object_id,
        object != NULL ? (unsigned long long)object->object_id : 0ull,
        name);
    int (*unlink_fn)(void *, void *) = NULL;
    memcpy(&unlink_fn, &ops.unlink, sizeof(unlink_fn));
    unsigned long old_gs = 0;
    int has_gs = enter_ext4_call(ops.unlink, &old_gs);
    int result = object->dentry != NULL ? unlink_fn(parent->inode, object->dentry) : -2;
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (result != 0 &&
        object->inode != NULL &&
        read_u32_field(object->inode, KOBOXD_INODE_NLINK_OFFSET) == 0)
    {
        result = 0;
    }
    if (result == 0) {
        fs_mark_object_unlinked(backend, object);
        fs_object_refresh(object);
        fs_mark_metadata_dirty(backend);
    }
    KOBOXD_FS_TRACE("[koboxd-fs-trace] unlink done status=%d object=%llu linked=%u nlink=%u\n",
        result,
        object != NULL ? (unsigned long long)object->object_id : 0ull,
        object != NULL ? object->linked : 0u,
        object != NULL ? object->nlink : 0u);
    return result;
}

int koboxd_fs_backend_mkdir(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint16_t mode,
    uint64_t *out_object_id)
{
    if (backend == NULL || name == NULL || out_object_id == NULL || !backend->mounted) {
        return -22;
    }
    *out_object_id = 0;
    koboxd_fs_object_t *parent = NULL;
    int status = fs_get_parent_object(backend, parent_object_id, &parent);
    if (status != 0) {
        return status;
    }
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    KOBOXD_FS_TRACE("[koboxd-fs-trace] mkdir parent=%llu name=%s mode=%o\n",
        (unsigned long long)parent_object_id,
        name,
        mode);
    const uint64_t create_start_ns = fs_now_ns();
    int mkdir_status = fs_create_child_object(
        backend,
        &ops,
        parent,
        name,
        mode,
        KOBOXD_EXT4_CREATE_DIRECTORY,
        out_object_id);
    const uint64_t create_end_ns = fs_now_ns();
    KOBOXD_FS_STAGE("[koboxd-fs-stage] op=mkdir create_us=%llu status=%d object=%llu\n",
        (unsigned long long)fs_elapsed_us(create_start_ns, create_end_ns),
        mkdir_status,
        (unsigned long long)*out_object_id);
    KOBOXD_FS_TRACE("[koboxd-fs-trace] mkdir done status=%d object=%llu\n",
        mkdir_status,
        (unsigned long long)*out_object_id);
    return mkdir_status;
}

int koboxd_fs_backend_rmdir(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name)
{
    if (backend == NULL || name == NULL || !backend->mounted) {
        return -22;
    }
    koboxd_fs_object_t *parent = NULL;
    int status = fs_get_parent_object(backend, parent_object_id, &parent);
    if (status != 0) {
        return status;
    }
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    koboxd_fs_object_t *object = NULL;
    status = fs_lookup_or_cache_child(backend, &ops, parent, name, &object);
    if (status != 0) {
        return status;
    }

    KOBOXD_FS_TRACE("[koboxd-fs-trace] rmdir parent=%llu object=%llu name=%s\n",
        (unsigned long long)parent_object_id,
        object != NULL ? (unsigned long long)object->object_id : 0ull,
        name);
    int (*rmdir_fn)(void *, void *) = NULL;
    memcpy(&rmdir_fn, &ops.rmdir, sizeof(rmdir_fn));
    unsigned long old_gs = 0;
    int has_gs = enter_ext4_call(ops.rmdir, &old_gs);
    int result = object->dentry != NULL ? rmdir_fn(parent->inode, object->dentry) : -2;
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (result == 0) {
        fs_mark_object_unlinked(backend, object);
        fs_object_refresh(object);
        fs_mark_metadata_dirty(backend);
    }
    KOBOXD_FS_STAGE("[koboxd-fs-stage] op=rmdir status=%d object=%llu name=%s\n",
        result,
        object != NULL ? (unsigned long long)object->object_id : 0ull,
        name);
    KOBOXD_FS_TRACE("[koboxd-fs-trace] rmdir done status=%d object=%llu linked=%u nlink=%u\n",
        result,
        object != NULL ? (unsigned long long)object->object_id : 0ull,
        object != NULL ? object->linked : 0u,
        object != NULL ? object->nlink : 0u);
    return result;
}

int koboxd_fs_backend_rename(
    koboxd_fs_backend_t *backend,
    uint64_t old_parent_object_id,
    const char *old_name,
    uint64_t new_parent_object_id,
    const char *new_name,
    uint64_t *out_object_id)
{
    if (backend == NULL || old_name == NULL || new_name == NULL || out_object_id == NULL || !backend->mounted) {
        return -22;
    }
    *out_object_id = 0;
    koboxd_fs_object_t *old_parent = fs_object_by_id(backend, old_parent_object_id);
    koboxd_fs_object_t *new_parent = fs_object_by_id(backend, new_parent_object_id);
    koboxd_fs_object_t *object = fs_object_by_parent_name(backend, old_parent_object_id, old_name);
    koboxd_fs_object_t *replaced = fs_object_by_parent_name(backend, new_parent_object_id, new_name);
    if (old_parent == NULL || new_parent == NULL || object == NULL ||
        old_parent->inode == NULL || new_parent->inode == NULL || object->dentry == NULL)
    {
        return -2;
    }
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    void *call_new_dentry = NULL;
    void *new_object_dentry = NULL;
    if (replaced != NULL && replaced != object) {
        call_new_dentry = replaced->dentry;
        new_object_dentry = calloc(1, KOBOXD_FAKE_DENTRY_BYTES);
        if (new_object_dentry == NULL) {
            return -12;
        }
        prepare_named_dentry(new_object_dentry, new_parent->dentry, object->inode, new_name);
    } else {
        call_new_dentry = calloc(1, KOBOXD_FAKE_DENTRY_BYTES);
        if (call_new_dentry == NULL) {
            return -12;
        }
        prepare_named_dentry(call_new_dentry, new_parent->dentry, NULL, new_name);
        new_object_dentry = call_new_dentry;
    }

    KOBOXD_FS_TRACE("[koboxd-fs-trace] rename old_parent=%llu old=%s new_parent=%llu new=%s object=%llu replaced=%llu\n",
        (unsigned long long)old_parent_object_id,
        old_name,
        (unsigned long long)new_parent_object_id,
        new_name,
        (unsigned long long)object->object_id,
        replaced != NULL ? (unsigned long long)replaced->object_id : 0ull);
    static uint8_t mnt_idmap[136];
    int (*rename_fn)(void *, void *, void *, void *, void *, unsigned int) = NULL;
    memcpy(&rename_fn, &ops.rename, sizeof(rename_fn));
    unsigned long old_gs = 0;
    int has_gs = enter_ext4_call(ops.rename, &old_gs);
    int result = rename_fn(
        mnt_idmap,
        old_parent->inode,
        object->dentry,
        new_parent->inode,
        call_new_dentry,
        0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (result != 0) {
        if (call_new_dentry != NULL && (replaced == NULL || replaced == object)) {
            free(call_new_dentry);
        }
        if (new_object_dentry != NULL && new_object_dentry != call_new_dentry) {
            free(new_object_dentry);
        }
        return result;
    }

    if (replaced != NULL && replaced != object) {
        fs_mark_object_unlinked(backend, replaced);
        fs_object_refresh(replaced);
    }
    free(object->dentry);
    object->dentry = new_object_dentry;
    object->inode_number = read_u64_field(object->inode, KOBOXD_INODE_NUMBER_OFFSET);
    write_pointer_field(object->dentry, KOBOXD_DENTRY_INODE_OFFSET, object->inode);
    object->parent_object_id = new_parent_object_id;
    snprintf(object->name, sizeof(object->name), "%s", new_name);
    object->linked = 1;
    fs_object_refresh(object);
    *out_object_id = object->object_id;
    fs_mark_metadata_dirty(backend);
    KOBOXD_FS_TRACE("[koboxd-fs-trace] rename done status=0 object=%llu\n",
        (unsigned long long)*out_object_id);
    return 0;
}

static int fs_prepare_unlinked_object_release(
    koboxd_fs_backend_t *backend,
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *object,
    uint64_t *out_inode_number)
{
    if (out_inode_number != NULL) {
        *out_inode_number = 0;
    }
    if (backend == NULL || ops == NULL || object == NULL ||
        !backend->mounted || object->object_id == 0 || object->object_id == 1)
    {
        return -22;
    }
    KOBOXD_FS_TRACE("[koboxd-fs-trace] release_now object=%llu name=%s inode=%llu\n",
        object != NULL ? (unsigned long long)object->object_id : 0ull,
        object != NULL ? object->name : "",
        object != NULL ? (unsigned long long)object->inode_number : 0ull);
    if (object->inode == NULL) {
        if (out_inode_number != NULL) {
            *out_inode_number = 0;
        }
        return 0;
    }

    uint64_t inode_number = object->inode_number;
    if (inode_number == 0) {
        inode_number = read_u64_field(object->inode, KOBOXD_INODE_NUMBER_OFFSET);
    }
    if ((object->mode & KOBOXD_MODE_TYPE_MASK) == 0100000) {
        int detach_status = kb_fs_subsystem_ext4_detach_inode_data_blocks_deferred(object->inode);
        if (detach_status != 0) {
            return detach_status;
        }
    }
    if (ops->evict_inode == NULL) {
        return -5;
    }
    kb_fs_subsystem_mark_inode_freeing(object->inode);
    void (*evict_inode_fn)(void *) = NULL;
    memcpy(&evict_inode_fn, &ops->evict_inode, sizeof(evict_inode_fn));
    unsigned long old_gs = 0;
    int has_gs = enter_ext4_call(ops->evict_inode, &old_gs);
    evict_inode_fn(object->inode);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (out_inode_number != NULL) {
        *out_inode_number = inode_number;
    }

    fs_mark_metadata_dirty(backend);
    return 0;
}

static void fs_finalize_unlinked_object_release(koboxd_fs_backend_t *backend, koboxd_fs_object_t *object)
{
    if (backend == NULL || object == NULL) {
        return;
    }
    free(object->dentry);
    if (object->inode != NULL) {
        kb_fs_subsystem_free_fake_inode(object->inode);
    }
    fs_object_unregister(backend, object->object_id);
    if (backend->deferred_unlinked_count != 0) {
        backend->deferred_unlinked_count--;
    }
    KOBOXD_FS_TRACE("[koboxd-fs-trace] release_now done status=0 inode=%llu\n",
        (unsigned long long)object->inode_number);
}

static int fs_release_deferred_unlinked_objects(koboxd_fs_backend_t *backend)
{
    if (backend == NULL || !backend->mounted) {
        return -22;
    }
    if (backend->deferred_unlinked_count == 0) {
        return 0;
    }
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    uint64_t inode_numbers[KOBOXD_FS_BACKEND_MAX_OBJECTS];
    koboxd_fs_object_t *released_objects[KOBOXD_FS_BACKEND_MAX_OBJECTS];
    size_t inode_count = 0;
    size_t released_count = 0;
    for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS; i++) {
        koboxd_fs_object_t *object = &backend->objects[i];
        if (!object->used || object->linked) {
            continue;
        }
        uint64_t inode_number = 0;
        const int status = fs_prepare_unlinked_object_release(backend, &ops, object, &inode_number);
        if (status != 0) {
            return status;
        }
        released_objects[released_count++] = object;
        if (inode_number != 0) {
            inode_numbers[inode_count++] = inode_number;
        }
    }
    if (inode_count != 0) {
        int status = kb_fs_subsystem_ext4_release_inode_records(
            backend->mount_result.super_block,
            inode_numbers,
            inode_count);
        if (status != 0) {
            return status;
        }
    }
    for (size_t i = 0; i < released_count; i++) {
        fs_finalize_unlinked_object_release(backend, released_objects[i]);
    }
    return 0;
}

int koboxd_fs_backend_release_object(koboxd_fs_backend_t *backend, uint64_t object_id)
{
    if (backend == NULL || !backend->mounted || object_id == 0 || object_id == 1) {
        return -22;
    }
    koboxd_fs_object_t *object = fs_object_by_id(backend, object_id);
    if (object == NULL) {
        return -2;
    }
    KOBOXD_FS_TRACE("[koboxd-fs-trace] release object=%llu linked=%u dirty=%u name=%s\n",
        (unsigned long long)object_id,
        object->linked,
        object->dirty,
        object->name);
    if (object->linked) {
        if (object->dirty && object->inode != NULL) {
            koboxd_ext4_operations_t ops;
            if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
                return -5;
            }
            int flush_status = fs_flush_dirty_object(&ops, object, 1);
            if (flush_status != 0) {
                return flush_status;
            }
        }
        return 0;
    }
    fs_mark_metadata_dirty(backend);
    KOBOXD_FS_TRACE("[koboxd-fs-trace] release deferred object=%llu\n",
        (unsigned long long)object_id);
    return 0;
}

int koboxd_fs_backend_pread(
    koboxd_fs_backend_t *backend,
    uint64_t object_id,
    uint64_t offset,
    void *buffer,
    size_t length,
    size_t buffer_capacity)
{
    if (backend == NULL || buffer == NULL || !backend->mounted) {
        return -22;
    }
    koboxd_fs_object_t *object = fs_object_by_id(backend, object_id);
    if (object == NULL || object->inode == NULL) {
        return -2;
    }
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    return fs_file_read(&ops, object->inode, offset, buffer, length, buffer_capacity);
}

int koboxd_fs_backend_pwrite(
    koboxd_fs_backend_t *backend,
    uint64_t object_id,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    if (backend == NULL || buffer == NULL || !backend->mounted) {
        return -22;
    }
    koboxd_fs_object_t *object = fs_object_by_id(backend, object_id);
    if (object == NULL || object->inode == NULL) {
        return -2;
    }
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    KOBOXD_FS_TRACE("[koboxd-fs-trace] pwrite object=%llu offset=%llu length=%zu\n",
        (unsigned long long)object_id,
        (unsigned long long)offset,
        length);
    const uint64_t write_start_ns = fs_now_ns();
    int status = fs_file_write(&ops, object->inode, offset, buffer, length);
    const uint64_t write_end_ns = fs_now_ns();
    if (status >= 0) {
        fs_mark_object_data_dirty(object);
        fs_object_refresh(object);
    }
    KOBOXD_FS_STAGE("[koboxd-fs-stage] op=pwrite write_us=%llu status=%d object=%llu length=%zu\n",
        (unsigned long long)fs_elapsed_us(write_start_ns, write_end_ns),
        status,
        (unsigned long long)object_id,
        length);
    KOBOXD_FS_TRACE("[koboxd-fs-trace] pwrite done status=%d object=%llu dirty=%u size=%llu\n",
        status,
        (unsigned long long)object_id,
        object->dirty,
        (unsigned long long)object->size);
    return status;
}

int koboxd_fs_backend_fsync(koboxd_fs_backend_t *backend, uint64_t object_id)
{
    if (backend == NULL || !backend->mounted) {
        return -22;
    }
    koboxd_fs_object_t *object = fs_object_by_id(backend, object_id);
    if (object == NULL || object->inode == NULL) {
        return -2;
    }
    if (!object->dirty) {
        return 0;
    }
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    return fs_flush_dirty_object(&ops, object, 1);
}

int koboxd_fs_backend_sync_all(koboxd_fs_backend_t *backend)
{
    if (backend == NULL || !backend->mounted) {
        return -22;
    }
    int has_dirty_objects = 0;
    for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS; i++) {
        const koboxd_fs_object_t *object = &backend->objects[i];
        if (object->used && object->dirty && object->inode != NULL) {
            has_dirty_objects = 1;
            break;
        }
    }
    if (!backend->metadata_dirty && !has_dirty_objects) {
        return 0;
    }

    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    KOBOXD_FS_TRACE("[koboxd-fs-trace] sync_all begin metadata_dirty=%u\n", backend->metadata_dirty);
    uint64_t flush_us = 0;
    uint64_t flush_count = 0;
    for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS; i++) {
        koboxd_fs_object_t *object = &backend->objects[i];
        if (!object->used || !object->dirty || object->inode == NULL) {
            continue;
        }
        const uint64_t flush_start_ns = fs_now_ns();
        int status = fs_flush_dirty_object(&ops, object, 0);
        const uint64_t flush_end_ns = fs_now_ns();
        flush_us += fs_elapsed_us(flush_start_ns, flush_end_ns);
        flush_count++;
        if (status != 0) {
            return status;
        }
    }
    const uint64_t drain_start_ns = fs_now_ns();
    int drain_status = fs_release_deferred_unlinked_objects(backend);
    const uint64_t drain_end_ns = fs_now_ns();
    if (drain_status != 0) {
        return drain_status;
    }
    if (backend->mount_result.super_block != NULL) {
        const uint64_t commit_start_ns = fs_now_ns();
        int status = fs_commit_superblock(&ops, backend->mount_result.super_block);
        const uint64_t commit_end_ns = fs_now_ns();
        if (status != 0) {
            return status;
        }
        backend->metadata_dirty = 0;
        KOBOXD_FS_STAGE("[koboxd-fs-stage] op=sync_all flush_count=%llu flush_us=%llu drain_us=%llu commit_us=%llu status=0\n",
            (unsigned long long)flush_count,
            (unsigned long long)flush_us,
            (unsigned long long)fs_elapsed_us(drain_start_ns, drain_end_ns),
            (unsigned long long)fs_elapsed_us(commit_start_ns, commit_end_ns));
        KOBOXD_FS_TRACE("[koboxd-fs-trace] sync_all done status=0\n");
        return 0;
    }
    return 0;
}

int koboxd_fs_backend_statx(
    koboxd_fs_backend_t *backend,
    uint64_t object_id,
    koboxd_fs_object_t *out_stat)
{
    if (backend == NULL || out_stat == NULL || !backend->mounted) {
        return -22;
    }
    koboxd_fs_object_t *object = fs_object_by_id(backend, object_id);
    if (object == NULL || object->inode == NULL) {
        return -2;
    }
    fs_object_refresh(object);
    *out_stat = *object;
    return 0;
}

int koboxd_fs_backend_getdents(
    koboxd_fs_backend_t *backend,
    uint64_t dir_object_id,
    uint64_t offset,
    koboxd_fs_object_t *out_entries,
    size_t capacity,
    size_t *out_count)
{
    if (backend == NULL || out_entries == NULL || out_count == NULL || !backend->mounted) {
        return -22;
    }
    *out_count = 0;

    koboxd_fs_object_t *dir = fs_object_by_id(backend, dir_object_id);
    if (dir == NULL || dir->inode == NULL || dir->dentry == NULL) {
        return -2;
    }
    if ((dir->mode & KOBOXD_MODE_TYPE_MASK) != 0040000u) {
        return -20;
    }

    size_t skipped = 0;
    for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS && *out_count < capacity; i++) {
        if (!backend->objects[i].used ||
            !backend->objects[i].linked ||
            backend->objects[i].object_id == dir_object_id ||
            backend->objects[i].parent_object_id != dir_object_id)
        {
            continue;
        }
        if (skipped < offset) {
            skipped++;
            continue;
        }
        out_entries[*out_count] = backend->objects[i];
        *out_count += 1;
    }
    return 0;
}
