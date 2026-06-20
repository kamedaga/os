#include "fs_backend.h"

#include "kobox/shim.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KOBOXD_FAKE_FILE_BYTES = 512,
    KOBOXD_FAKE_KIOCB_BYTES = 128,
    KOBOXD_FAKE_IOV_ITER_BYTES = 128,
    KOBOXD_FAKE_MAPPING_BYTES = 256,
    KOBOXD_FAKE_DENTRY_BYTES = 512,
    KOBOXD_DENTRY_FLAGS_OFFSET = 0x0,
    KOBOXD_DENTRY_PARENT_OFFSET = 0x18,
    KOBOXD_DENTRY_NAME_HASH_OFFSET = 0x20,
    KOBOXD_DENTRY_NAME_LEN_OFFSET = 0x24,
    KOBOXD_DENTRY_NAME_PTR_OFFSET = 0x28,
    KOBOXD_DENTRY_INODE_OFFSET = 0x38,
    KOBOXD_INODE_MODE_OFFSET = 0x0,
    KOBOXD_INODE_NUMBER_OFFSET = 0x40,
    KOBOXD_INODE_NLINK_OFFSET = 0x48,
    KOBOXD_INODE_SIZE_OFFSET = 0x50,
    KOBOXD_INODE_BLOCKS_OFFSET = 0x88,
    KOBOXD_FILE_PATH_DENTRY_OFFSET = 0x18,
    KOBOXD_FILE_MAPPING_OFFSET = 0x20,
    KOBOXD_FILE_INODE_OFFSET = 0x28,
    KOBOXD_KIOCB_FILE_OFFSET = 0x0,
    KOBOXD_KIOCB_POS_OFFSET = 0x8,
    KOBOXD_KIOCB_FLAGS_OFFSET = 0x20,
    KOBOXD_IOV_ITER_COUNT_OFFSET = 0x18,
    KOBOXD_IOV_ITER_BUFFER_OFFSET = 0x20,
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
} koboxd_ext4_operations_t;

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

static uint16_t read_u16_field(const void *base, size_t offset)
{
    uint16_t value = 0;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static uint32_t read_u32_field(const void *base, size_t offset)
{
    uint32_t value = 0;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static uint64_t read_u64_field(const void *base, size_t offset)
{
    uint64_t value = 0;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

static void fill_object_from_inode(
    koboxd_fs_object_t *object,
    uint64_t object_id,
    void *inode,
    void *dentry,
    const char *name)
{
    if (object == NULL) {
        return;
    }
    memset(object, 0, sizeof(*object));
    object->object_id = object_id;
    object->inode = inode;
    object->dentry = dentry;
    object->mode = inode != NULL ? read_u16_field(inode, KOBOXD_INODE_MODE_OFFSET) : 0;
    object->nlink = inode != NULL ? read_u32_field(inode, KOBOXD_INODE_NLINK_OFFSET) : 0;
    object->size = inode != NULL ? read_u64_field(inode, KOBOXD_INODE_SIZE_OFFSET) : 0;
    object->blocks = inode != NULL ? read_u64_field(inode, KOBOXD_INODE_BLOCKS_OFFSET) : 0;
    if (name != NULL) {
        snprintf(object->name, sizeof(object->name), "%s", name);
    }
    object->used = 1;
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
    if (backend == NULL || parent_object_id != 1 || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS; i++) {
        if (backend->objects[i].used && strcmp(backend->objects[i].name, name) == 0) {
            return &backend->objects[i];
        }
    }
    return NULL;
}

static int fs_object_register(
    koboxd_fs_backend_t *backend,
    void *inode,
    void *dentry,
    const char *name,
    uint64_t *out_object_id)
{
    if (backend == NULL || inode == NULL || name == NULL || out_object_id == NULL) {
        return -22;
    }
    for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS; i++) {
        if (!backend->objects[i].used) {
            const uint64_t object_id = backend->next_object_id++;
            fill_object_from_inode(&backend->objects[i], object_id, inode, dentry, name);
            *out_object_id = object_id;
            return 0;
        }
    }
    return -12;
}

static int module_symbol(kb_module_t *module, const char *name, void **out_address)
{
    kb_status_t status = kb_module_find_symbol(module, name, out_address);
    if (status != KB_OK || out_address == NULL || *out_address == NULL) {
        fprintf(stderr, "[koboxd] fs-backend missing symbol %s status=%s(%d)\n",
            name,
            status_name(status),
            status);
        return 0;
    }
    return 1;
}

static int load_ext4_operation_tables(kb_module_t *module, koboxd_ext4_operations_t *out_ops)
{
    if (module == NULL || out_ops == NULL) {
        return 0;
    }
    memset(out_ops, 0, sizeof(*out_ops));
    return module_symbol(module, "ext4_dir_operations", &out_ops->dir_operations) &&
        module_symbol(module, "ext4_file_operations", &out_ops->file_operations) &&
        module_symbol(module, "ext4_dir_inode_operations", &out_ops->dir_inode_operations) &&
        module_symbol(module, "ext4_readdir", &out_ops->readdir) &&
        module_symbol(module, "ext4_file_read_iter", &out_ops->file_read_iter) &&
        module_symbol(module, "ext4_file_write_iter", &out_ops->file_write_iter) &&
        module_symbol(module, "ext4_sync_file", &out_ops->file_fsync) &&
        module_symbol(module, "ext4_lookup", &out_ops->lookup);
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

    write_u32_field(dentry, KOBOXD_DENTRY_FLAGS_OFFSET, 0);
    write_pointer_field(dentry, KOBOXD_DENTRY_PARENT_OFFSET, parent_dentry);
    write_u32_field(dentry, KOBOXD_DENTRY_NAME_HASH_OFFSET, 0);
    write_u32_field(dentry, KOBOXD_DENTRY_NAME_LEN_OFFSET, (uint32_t)strlen(name));
    write_pointer_field(dentry, KOBOXD_DENTRY_NAME_PTR_OFFSET, (void *)(uintptr_t)name);

    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(ops->lookup);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    void *(*lookup_fn)(void *, void *, unsigned int) = NULL;
    memcpy(&lookup_fn, &ops->lookup, sizeof(lookup_fn));
    void *result = lookup_fn(parent_inode, dentry, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    void *inode = read_pointer_field(dentry, KOBOXD_DENTRY_INODE_OFFSET);
    if (inode == NULL) {
        free(dentry);
        return -5;
    }
    *out_inode = inode;
    *out_dentry = dentry;
    return 0;
}

static int fs_file_read(
    const koboxd_ext4_operations_t *ops,
    void *inode,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    if (ops == NULL || ops->file_read_iter == NULL || inode == NULL || buffer == NULL) {
        return -22;
    }
    void *file = calloc(1, KOBOXD_FAKE_FILE_BYTES);
    void *kiocb = calloc(1, KOBOXD_FAKE_KIOCB_BYTES);
    void *iter = calloc(1, KOBOXD_FAKE_IOV_ITER_BYTES);
    void *mapping = calloc(1, KOBOXD_FAKE_MAPPING_BYTES);
    if (file == NULL || kiocb == NULL || iter == NULL || mapping == NULL) {
        free(mapping);
        free(iter);
        free(kiocb);
        free(file);
        return -12;
    }

    write_pointer_field(file, KOBOXD_FILE_MAPPING_OFFSET, mapping);
    write_pointer_field(file, KOBOXD_FILE_INODE_OFFSET, inode);
    write_pointer_field(kiocb, KOBOXD_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, KOBOXD_KIOCB_POS_OFFSET, offset);
    write_u64_field(iter, KOBOXD_IOV_ITER_COUNT_OFFSET, (uint64_t)length);
    write_pointer_field(iter, KOBOXD_IOV_ITER_BUFFER_OFFSET, buffer);

    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(ops->file_read_iter);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    long (*read_iter_fn)(void *, void *) = NULL;
    memcpy(&read_iter_fn, &ops->file_read_iter, sizeof(read_iter_fn));
    long result = read_iter_fn(kiocb, iter);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    free(mapping);
    free(iter);
    free(kiocb);
    free(file);
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
    void *file = calloc(1, KOBOXD_FAKE_FILE_BYTES);
    void *kiocb = calloc(1, KOBOXD_FAKE_KIOCB_BYTES);
    void *iter = calloc(1, KOBOXD_FAKE_IOV_ITER_BYTES);
    void *mapping = calloc(1, KOBOXD_FAKE_MAPPING_BYTES);
    if (file == NULL || kiocb == NULL || iter == NULL || mapping == NULL) {
        free(mapping);
        free(iter);
        free(kiocb);
        free(file);
        return -12;
    }

    write_pointer_field(file, KOBOXD_FILE_MAPPING_OFFSET, mapping);
    write_pointer_field(file, KOBOXD_FILE_INODE_OFFSET, inode);
    write_pointer_field(kiocb, KOBOXD_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, KOBOXD_KIOCB_POS_OFFSET, offset);
    write_u32_field(kiocb, KOBOXD_KIOCB_FLAGS_OFFSET, 0x2u);
    write_u64_field(iter, KOBOXD_IOV_ITER_COUNT_OFFSET, (uint64_t)length);
    write_pointer_field(iter, KOBOXD_IOV_ITER_BUFFER_OFFSET, (void *)(uintptr_t)buffer);

    unsigned long old_gs = 0;
    unsigned long kernel_gs = kb_module_kernel_gs_for_address(ops->file_write_iter);
    int has_gs = kernel_gs != 0 && kb_shim_enter_kernel_gs(kernel_gs, &old_gs) == 0;
    long (*write_iter_fn)(void *, void *) = NULL;
    memcpy(&write_iter_fn, &ops->file_write_iter, sizeof(write_iter_fn));
    long result = write_iter_fn(kiocb, iter);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    free(mapping);
    free(iter);
    free(kiocb);
    free(file);
    return result >= 0 ? (int)result : (int)result;
}

static int fs_file_fsync(const koboxd_ext4_operations_t *ops, void *inode)
{
    if (ops == NULL || ops->file_fsync == NULL || inode == NULL) {
        return -22;
    }
    void *file = calloc(1, KOBOXD_FAKE_FILE_BYTES);
    void *mapping = calloc(1, KOBOXD_FAKE_MAPPING_BYTES);
    void *dentry = calloc(1, KOBOXD_FAKE_DENTRY_BYTES);
    if (file == NULL || mapping == NULL || dentry == NULL) {
        free(dentry);
        free(mapping);
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
    free(mapping);
    free(file);
    return result;
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
    int status = kb_fs_subsystem_set_mount_block_device(root_device);
    if (status != 0) {
        return status;
    }
    status = kb_fs_subsystem_mount_registered_root("ext4", &backend->mount_result);
    if (backend->mount_result.fill_super_result != 0 || backend->mount_result.observed_ext4_magic != 0xef53u) {
        return -5;
    }
    printf("[koboxd] rootfs mounted fs=ext4 reads=%u\n", backend->mount_result.block_read_count);
    backend->ext4_module = ext4_module;
    backend->next_object_id = 2;
    fill_object_from_inode(
        &backend->objects[0],
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
    return fs_object_register(backend, inode, dentry, name, out_object_id);
}

int koboxd_fs_backend_pread(
    koboxd_fs_backend_t *backend,
    uint64_t object_id,
    uint64_t offset,
    void *buffer,
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
    return fs_file_read(&ops, object->inode, offset, buffer, length);
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
    const int status = fs_file_write(&ops, object->inode, offset, buffer, length);
    if (status >= 0) {
        char name[KOBOXD_FS_BACKEND_NAME_BYTES];
        snprintf(name, sizeof(name), "%s", object->name);
        fill_object_from_inode(object, object->object_id, object->inode, object->dentry, name);
    }
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
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    return fs_file_fsync(&ops, object->inode);
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
    char name[KOBOXD_FS_BACKEND_NAME_BYTES];
    snprintf(name, sizeof(name), "%s", object->name);
    fill_object_from_inode(object, object->object_id, object->inode, object->dentry, name);
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
    if (dir_object_id != 1) {
        return -95;
    }
    size_t skipped = 0;
    for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS && *out_count < capacity; i++) {
        if (!backend->objects[i].used || backend->objects[i].object_id == dir_object_id) {
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

int koboxd_fs_backend_handle_ipc(void *ctx, const koboxd_ipc_request_t *request, koboxd_ipc_reply_t *reply)
{
    koboxd_fs_backend_t *backend = (koboxd_fs_backend_t *)ctx;
    if (backend == NULL || request == NULL || reply == NULL || !backend->mounted) {
        return -1;
    }
    int status = koboxd_ipc_validate_request(request, KOBOXD_IPC_ENDPOINT_FS_BACKEND);
    if (status != 0) {
        return status;
    }

    switch (request->header.op) {
    case KOBOXD_FS_MOUNT_ROOT:
        return koboxd_ipc_make_reply(request, 0, backend->mount_result.observed_ext4_magic, 0, NULL, 0, reply);
    case KOBOXD_FS_LOOKUP: {
        const koboxd_fs_lookup_request_t *lookup =
            (const koboxd_fs_lookup_request_t *)(const void *)request->inline_payload;
        if (request->header.inline_bytes < sizeof(*lookup)) {
            return koboxd_ipc_make_reply(request, -22, 0, 0, NULL, 0, reply);
        }
        uint64_t object_id = 0;
        status = koboxd_fs_backend_lookup(backend, 1, lookup->name, &object_id);
        if (status != 0) {
            return koboxd_ipc_make_reply(request, status, 0, 0, NULL, 0, reply);
        }
        return koboxd_ipc_make_reply(request, 0, object_id, 0, NULL, 0, reply);
    }
    case KOBOXD_FS_PREAD: {
        const koboxd_fs_io_request_t *io =
            (const koboxd_fs_io_request_t *)(const void *)request->inline_payload;
        if (request->header.inline_bytes < sizeof(*io)) {
            return koboxd_ipc_make_reply(request, -22, 0, 0, NULL, 0, reply);
        }
        koboxd_fs_io_reply_t payload;
        memset(&payload, 0, sizeof(payload));
        size_t length = io->length;
        if (length > sizeof(payload.data)) {
            length = sizeof(payload.data);
        }
        status = koboxd_fs_backend_pread(backend, io->object_id, io->offset, payload.data, length);
        if (status < 0) {
            return koboxd_ipc_make_reply(request, status, 0, 0, NULL, 0, reply);
        }
        return koboxd_ipc_make_reply(request, 0, (uint64_t)status, 0, &payload, sizeof(payload), reply);
    }
    case KOBOXD_FS_PWRITE: {
        const koboxd_fs_io_request_t *io =
            (const koboxd_fs_io_request_t *)(const void *)request->inline_payload;
        if (request->header.inline_bytes < sizeof(*io)) {
            return koboxd_ipc_make_reply(request, -22, 0, 0, NULL, 0, reply);
        }
        size_t length = io->length;
        if (length > sizeof(io->data)) {
            length = sizeof(io->data);
        }
        status = koboxd_fs_backend_pwrite(backend, io->object_id, io->offset, io->data, length);
        if (status < 0) {
            return koboxd_ipc_make_reply(request, status, 0, 0, NULL, 0, reply);
        }
        return koboxd_ipc_make_reply(request, 0, (uint64_t)status, 0, NULL, 0, reply);
    }
    case KOBOXD_FS_FSYNC: {
        const koboxd_fs_io_request_t *io =
            (const koboxd_fs_io_request_t *)(const void *)request->inline_payload;
        if (request->header.inline_bytes < sizeof(*io)) {
            return koboxd_ipc_make_reply(request, -22, 0, 0, NULL, 0, reply);
        }
        status = koboxd_fs_backend_fsync(backend, io->object_id);
        return koboxd_ipc_make_reply(request, status, 0, 0, NULL, 0, reply);
    }
    default:
        return koboxd_ipc_make_reply(request, -95, 0, 0, NULL, 0, reply);
    }
}
