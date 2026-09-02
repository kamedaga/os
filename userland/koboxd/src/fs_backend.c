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

static koboxd_fs_hotpath_profile_t fs_hotpath_profile;

#if defined(KOBOX_STORAGE_PROFILE) && KOBOX_STORAGE_PROFILE
static uint64_t fs_profile_tsc(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}
#define FS_PROFILE_BEGIN(name) const uint64_t name = fs_profile_tsc()
#define FS_PROFILE_END(field, name) \
    (fs_hotpath_profile.field += fs_profile_tsc() - (name))
#else
#define FS_PROFILE_BEGIN(name) ((void)0)
#define FS_PROFILE_END(field, name) ((void)0)
#endif

void koboxd_fs_hotpath_profile_reset(void)
{
    memset(&fs_hotpath_profile, 0, sizeof(fs_hotpath_profile));
}

void koboxd_fs_hotpath_profile_snapshot(koboxd_fs_hotpath_profile_t *out_profile)
{
    if (out_profile != NULL) {
        *out_profile = fs_hotpath_profile;
    }
}

static int fs_release_deferred_unlinked_objects(koboxd_fs_backend_t *backend);
static int fs_retire_cached_inode(
    koboxd_fs_backend_t *backend,
    void *inode,
    void *dentry);
static int fs_object_close_native_files(koboxd_fs_object_t *object);
static int enter_ext4_call(void *function, unsigned long *old_gs);

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
    KOBOXD_FAKE_KIOCB_BYTES = 128,
    KOBOXD_FAKE_IOV_ITER_BYTES = 128,
    KOBOXD_DENTRY_INODE_OFFSET = 0x30,
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
    KOBOXD_INODE_BLOCKS_OFFSET = 0x88,
    KOBOXD_INODE_RDEV_OFFSET = 0x4c,
    KOBOXD_SUPER_BLOCK_OPS_OFFSET = 0x30,
    KOBOXD_SUPER_BLOCK_FS_INFO_OFFSET = 0x380,
    KOBOXD_SUPER_OP_STATFS_OFFSET = 0x68,
    /* Native Linux 6.12 struct file / file_operations layout. */
    KOBOXD_NATIVE_FILE_PRIVATE_DATA_OFFSET = 0x20,
    KOBOXD_NATIVE_FILE_POSITION_OFFSET = 0x70,
    KOBOXD_FILE_OP_ITERATE_SHARED_OFFSET = 0x40,
    KOBOXD_FILE_OP_FSYNC_OFFSET = 0x80,
    KOBOXD_KIOCB_FILE_OFFSET = 0x0,
    KOBOXD_KIOCB_POS_OFFSET = 0x8,
    KOBOXD_KIOCB_FLAGS_OFFSET = 0x20,
    KOBOXD_IOV_ITER_COUNT_OFFSET = 0x18,
    KOBOXD_IOV_ITER_BUFFER_OFFSET = 0x20,
    KOBOXD_IOV_ITER_BUFFER_CAPACITY_OFFSET = 0x78,
    KOBOXD_IOV_ITER_DATA_SOURCE_OFFSET = 0x2,
    KOBOXD_IOCB_DIRECT = 1u << 17,
    KOBOXD_DIRECT_READ_ALIGNMENT = 512u,
    KOBOXD_DIRECT_READ_MIN_BYTES = 64u * 1024u,
    KOBOXD_INODE_OP_CREATE_OFFSET = 0x28,
    KOBOXD_INODE_OP_GET_LINK_OFFSET = 0x08,
    KOBOXD_INODE_OP_LINK_OFFSET = 0x30,
    KOBOXD_INODE_OP_UNLINK_OFFSET = 0x38,
    KOBOXD_INODE_OP_SYMLINK_OFFSET = 0x40,
    KOBOXD_INODE_OP_MKDIR_OFFSET = 0x48,
    KOBOXD_INODE_OP_RMDIR_OFFSET = 0x50,
    KOBOXD_INODE_OP_MKNOD_OFFSET = 0x58,
    KOBOXD_INODE_OP_RENAME_OFFSET = 0x60,
    KOBOXD_INODE_OP_GETATTR_OFFSET = 0x70,
    KOBOXD_IATTR_MODE = 1u << 0,
    KOBOXD_IATTR_SIZE = 1u << 3,
    KOBOXD_IATTR_ATIME = 1u << 4,
    KOBOXD_IATTR_MTIME = 1u << 5,
    KOBOXD_IATTR_CTIME = 1u << 6,
    KOBOXD_MODE_REGULAR_0644 = 0100000 | 0644,
    KOBOXD_MODE_DIRECTORY_0755 = 0040000 | 0755,
    KOBOXD_MODE_TYPE_MASK = 0170000,
    KOBOXD_MODE_PERM_MASK = 07777,
    KOBOXD_ERR_NOT_EMPTY = -39,
    KOBOXD_TIME_UPDATE_ATIME = 1u << 0,
    KOBOXD_TIME_UPDATE_MTIME = 1u << 1,
    KOBOXD_OBJECT_DIRTY_METADATA = 1u << 0,
    KOBOXD_OBJECT_DIRTY_DATA = 1u << 1,
    KOBOXD_STATX_BASIC_STATS = 0x000007ffu,
    KOBOXD_STATX_BTIME = 0x00000800u,
    /* Internal page storage only; this is not a directory-size limit. */
    KOBOXD_READDIR_PAGE_MAX_ENTRIES = 128,
};

static void fs_inode_write_lock(void *inode, const char *site)
{
    if (inode != NULL) {
        kb_rwsem_note_site(site);
        kb_down_write((uint8_t *)inode + KOBOXD_INODE_RWSEM_OFFSET);
    }
}

static void fs_inode_read_lock(void *inode, const char *site)
{
    if (inode != NULL) {
        kb_rwsem_note_site(site);
        kb_down_read((uint8_t *)inode + KOBOXD_INODE_RWSEM_OFFSET);
    }
}

static void fs_inode_read_unlock(void *inode)
{
    if (inode != NULL) {
        kb_up_read((uint8_t *)inode + KOBOXD_INODE_RWSEM_OFFSET);
    }
}

static void fs_inode_write_unlock(void *inode)
{
    if (inode != NULL) {
        kb_up_write((uint8_t *)inode + KOBOXD_INODE_RWSEM_OFFSET);
    }
}

typedef struct fs_inode_lock_set {
    void *inodes[4];
    size_t count;
} fs_inode_lock_set_t;

static void fs_inode_lock_set_add(fs_inode_lock_set_t *set, void *inode)
{
    if (set == NULL || inode == NULL) {
        return;
    }
    for (size_t i = 0; i < set->count; ++i) {
        if (set->inodes[i] == inode) {
            return;
        }
    }
    if (set->count < sizeof(set->inodes) / sizeof(set->inodes[0])) {
        set->inodes[set->count++] = inode;
    }
}

static void fs_inode_lock_set_acquire(fs_inode_lock_set_t *set, const char *site)
{
    if (set == NULL) {
        return;
    }
    for (size_t i = 1; i < set->count; ++i) {
        void *inode = set->inodes[i];
        size_t j = i;
        while (j != 0 && (uintptr_t)set->inodes[j - 1] > (uintptr_t)inode) {
            set->inodes[j] = set->inodes[j - 1];
            --j;
        }
        set->inodes[j] = inode;
    }
    for (size_t i = 0; i < set->count; ++i) {
        fs_inode_write_lock(set->inodes[i], site);
    }
}

static void fs_inode_lock_set_release(const fs_inode_lock_set_t *set)
{
    if (set == NULL) {
        return;
    }
    for (size_t i = set->count; i != 0; --i) {
        fs_inode_write_unlock(set->inodes[i - 1]);
    }
}

typedef struct koboxd_ext4_operations {
    void *dir_operations;
    void *file_operations;
    void *dir_inode_operations;
    void *dir_iterate_shared;
    void *file_read_iter;
    void *file_write_iter;
    void *lookup;
    void *create;
    void *link;
    void *unlink;
    void *symlink;
    void *mkdir;
    void *rmdir;
    void *mknod;
    void *rename;
    void *setattr;
    void *inode_is_fast_symlink;
} koboxd_ext4_operations_t;

typedef struct koboxd_linux_timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
} koboxd_linux_timespec64_t;

typedef struct koboxd_linux_kstat {
    uint32_t result_mask;
    uint16_t mode;
    uint16_t reserved0;
    uint32_t nlink;
    uint32_t blksize;
    uint64_t attributes;
    uint64_t attributes_mask;
    uint64_t ino;
    uint32_t dev;
    uint32_t rdev;
    uint32_t uid;
    uint32_t gid;
    int64_t size;
    koboxd_linux_timespec64_t atime;
    koboxd_linux_timespec64_t mtime;
    koboxd_linux_timespec64_t ctime;
    koboxd_linux_timespec64_t btime;
    uint64_t blocks;
    uint64_t mnt_id;
    uint32_t dio_mem_align;
    uint32_t dio_offset_align;
    uint64_t change_cookie;
} koboxd_linux_kstat_t;

_Static_assert(
    offsetof(koboxd_linux_kstat_t, btime) == 0x70,
    "Linux 6.12 struct kstat btime layout");
_Static_assert(
    offsetof(koboxd_linux_kstat_t, dio_mem_align) == 0x90,
    "Linux 6.12 struct kstat DIO layout");
_Static_assert(
    sizeof(koboxd_linux_kstat_t) == 0xa0,
    "Linux 6.12 struct kstat size");

typedef struct koboxd_linux_iattr {
    uint32_t ia_valid;
    uint16_t ia_mode;
    uint16_t reserved0;
    uint32_t ia_uid;
    uint32_t ia_gid;
    int64_t ia_size;
    koboxd_linux_timespec64_t ia_atime;
    koboxd_linux_timespec64_t ia_mtime;
    koboxd_linux_timespec64_t ia_ctime;
    void *ia_file;
} koboxd_linux_iattr_t;

_Static_assert(sizeof(koboxd_linux_iattr_t) == 80, "Linux 6.12 struct iattr layout");

typedef enum koboxd_ext4_child_create_kind {
    KOBOXD_EXT4_CREATE_REGULAR,
    KOBOXD_EXT4_CREATE_DIRECTORY,
} koboxd_ext4_child_create_kind_t;

typedef struct koboxd_readdir_scan_entry {
    uint64_t inode_number;
    uint16_t mode;
    char name[KOBOXD_FS_BACKEND_NAME_BYTES];
} koboxd_readdir_scan_entry_t;

typedef struct koboxd_readdir_scan {
    koboxd_readdir_scan_entry_t entries[KOBOXD_READDIR_PAGE_MAX_ENTRIES];
    uint64_t skip;
    uint64_t visible;
    size_t capacity;
    size_t count;
    int full;
} koboxd_readdir_scan_t;

typedef int (*koboxd_dir_actor_fn)(
    void *context,
    const char *name,
    int name_len,
    int64_t position,
    uint64_t inode_number,
    unsigned int d_type);

typedef struct koboxd_native_dir_context {
    koboxd_dir_actor_fn actor;
    int64_t position;
    koboxd_readdir_scan_t *scan;
} koboxd_native_dir_context_t;

typedef struct koboxd_deferred_release_batch {
    koboxd_fs_object_t *objects[KOBOXD_FS_BACKEND_MAX_OBJECTS];
    size_t count;
} koboxd_deferred_release_batch_t;

static int fs_file_fsync(
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *object,
    void *vfsmount);
static int fs_file_read(
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *object,
    void *vfsmount,
    uint64_t offset,
    void *buffer,
    size_t length,
    size_t buffer_capacity);

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

static uint32_t fs_encode_linux_dev(uint32_t kernel_dev)
{
    const uint32_t major = kernel_dev >> 20u;
    const uint32_t minor = kernel_dev & ((1u << 20u) - 1u);
    return (minor & 0xffu) | (major << 8u) | ((minor & ~0xffu) << 12u);
}

static uint32_t fs_decode_linux_dev(uint32_t encoded_dev)
{
    const uint32_t major = (encoded_dev & 0xfff00u) >> 8u;
    const uint32_t minor = (encoded_dev & 0xffu) | ((encoded_dev >> 12u) & 0xfff00u);
    return (major << 20u) | minor;
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

static int fs_getattr_inode(
    void *inode,
    void *dentry,
    koboxd_linux_kstat_t *out_stat)
{
    if (inode == NULL || out_stat == NULL) {
        return -22;
    }
    memset(out_stat, 0, sizeof(*out_stat));
    void *inode_operations = read_pointer_field(inode, KOBOXD_INODE_OP_OFFSET);
    void *getattr_operation = inode_operations == NULL ? NULL :
        read_pointer_field(
            inode_operations,
            KOBOXD_INODE_OP_GETATTR_OFFSET);
    if (getattr_operation == NULL || dentry == NULL) {
        kb_fs_subsystem_generic_fillattr(
            NULL,
            KOBOXD_STATX_BASIC_STATS | KOBOXD_STATX_BTIME,
            inode,
            out_stat);
        return 0;
    }

    static uint8_t nop_mnt_idmap[136];
    void *path[2] = {NULL, dentry};
    int (*getattr_fn)(void *, const void *, void *, uint32_t, unsigned int) = NULL;
    memcpy(&getattr_fn, &getattr_operation, sizeof(getattr_fn));
    unsigned long old_gs = 0;
    const int has_gs = enter_ext4_call(getattr_operation, &old_gs);
    const int status = getattr_fn(
        nop_mnt_idmap,
        path,
        out_stat,
        KOBOXD_STATX_BASIC_STATS | KOBOXD_STATX_BTIME,
        0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return status;
}

static int fill_object_from_inode(
    koboxd_fs_object_t *object,
    uint64_t object_id,
    uint64_t parent_object_id,
    void *inode,
    void *dentry,
    const char *name,
    uint32_t references,
    uint64_t last_used)
{
    if (object == NULL) {
        return -22;
    }
    koboxd_linux_kstat_t stat;
    const int getattr_status = fs_getattr_inode(inode, dentry, &stat);
    if (getattr_status != 0) {
        return getattr_status;
    }
    memset(object, 0, sizeof(*object));
    object->object_id = object_id;
    object->parent_object_id = parent_object_id;
    object->inode_number = stat.ino;
    object->inode = inode;
    object->dentry = dentry;
    object->mode = stat.mode;
    object->nlink = stat.nlink;
    object->size = (uint64_t)stat.size;
    object->blocks = stat.blocks;
    object->rdev = fs_encode_linux_dev(stat.rdev);
    object->atime_sec = stat.atime.tv_sec;
    object->atime_nsec = stat.atime.tv_nsec;
    object->mtime_sec = stat.mtime.tv_sec;
    object->mtime_nsec = stat.mtime.tv_nsec;
    object->ctime_sec = stat.ctime.tv_sec;
    object->ctime_nsec = stat.ctime.tv_nsec;
    if (name != NULL) {
        snprintf(object->name, sizeof(object->name), "%s", name);
    }
    object->references = references;
    object->last_used = last_used;
    object->used = 1;
    object->linked = 1;
    return 0;
}

static uint64_t fs_object_next_clock(koboxd_fs_backend_t *backend)
{
    if (backend->object_clock == UINT64_MAX) {
        uint64_t next = 1;
        for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS; ++i) {
            koboxd_fs_object_t *object = &backend->objects[i];
            if (object->used) {
                object->last_used = next++;
            }
        }
        backend->object_clock = next;
    }
    ++backend->object_clock;
    if (backend->object_clock == 0) {
        backend->object_clock = 1;
    }
    return backend->object_clock;
}

static int fs_object_acquire(koboxd_fs_backend_t *backend, koboxd_fs_object_t *object)
{
    if (backend == NULL || object == NULL || !object->used || object->references == UINT32_MAX) {
        return -75;
    }
    ++object->references;
    object->last_used = fs_object_next_clock(backend);
    return 0;
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
    uint32_t references,
    uint64_t *out_object_id)
{
    if (backend == NULL || inode == NULL || name == NULL || out_object_id == NULL) {
        return -22;
    }
    for (unsigned int attempt = 0; attempt < 3; attempt++) {
        for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS; i++) {
            if (!backend->objects[i].used) {
                const uint64_t object_id = backend->next_object_id++;
                const int fill_status = fill_object_from_inode(
                    &backend->objects[i],
                    object_id,
                    parent_object_id,
                    inode,
                    dentry,
                    name,
                    references,
                    fs_object_next_clock(backend));
                if (fill_status != 0) {
                    return fill_status;
                }
                *out_object_id = object_id;
                return 0;
            }
        }
        if (attempt == 0 && backend->deferred_unlinked_count != 0) {
            const int release_status = fs_release_deferred_unlinked_objects(backend);
            if (release_status != 0) {
                return release_status;
            }
            continue;
        }
        koboxd_fs_object_t *oldest = NULL;
        for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS; ++i) {
            koboxd_fs_object_t *candidate = &backend->objects[i];
            if (!candidate->used || !candidate->linked ||
                candidate->references != 0 || candidate->object_id == 1)
            {
                continue;
            }
            if (oldest == NULL || candidate->last_used < oldest->last_used) {
                oldest = candidate;
            }
        }
        if (oldest == NULL) {
            break;
        }
        const int close_status = fs_object_close_native_files(oldest);
        if (close_status != 0) {
            return close_status;
        }
        const int retire_status = fs_retire_cached_inode(
            backend,
            oldest->inode,
            oldest->dentry);
        if (retire_status != 0) {
            fprintf(stderr,
                "FILED_STORAGE_FAULT layer=kobox_cache_evict status=%d object=%llu "
                "inode=%llu deferred=%u\n",
                retire_status,
                (unsigned long long)oldest->object_id,
                (unsigned long long)oldest->inode_number,
                backend->deferred_unlinked_count);
            return retire_status;
        }
        memset(oldest, 0, sizeof(*oldest));
        ++backend->object_evictions;
    }
    koboxd_fs_object_stats_t stats;
    koboxd_fs_backend_object_stats(backend, &stats);
    fprintf(stderr,
        "FILED_STORAGE_FAULT layer=kobox_object_capacity status=-12 objects=%u "
        "referenced=%u cached=%u evictions=%llu deferred=%u\n",
        stats.used,
        stats.referenced,
        stats.cached,
        (unsigned long long)stats.evictions,
        backend->deferred_unlinked_count);
    return -12;
}

static int fs_discard_unregistered_lookup(
    koboxd_fs_backend_t *backend,
    void *inode,
    void *dentry)
{
    return fs_retire_cached_inode(backend, inode, dentry);
}

static void fs_object_unregister(koboxd_fs_backend_t *backend, uint64_t object_id)
{
    koboxd_fs_object_t *object = fs_object_by_id(backend, object_id);
    if (object != NULL && object->object_id != 1) {
        memset(object, 0, sizeof(*object));
    }
}

static int fs_object_refresh(koboxd_fs_object_t *object)
{
    FS_PROFILE_BEGIN(profile_start);
    char name[KOBOXD_FS_BACKEND_NAME_BYTES];
    uint64_t parent_object_id;
    uint64_t last_used;
    uint32_t references;
    void *native_read_file;
    void *native_write_file;
    uint8_t linked;
    uint8_t dirty;
    if (object == NULL || !object->used) {
        return -22;
    }
    snprintf(name, sizeof(name), "%s", object->name);
    parent_object_id = object->parent_object_id;
    last_used = object->last_used;
    references = object->references;
    native_read_file = object->native_read_file;
    native_write_file = object->native_write_file;
    linked = object->linked;
    dirty = object->dirty;
    const int status = fill_object_from_inode(
        object,
        object->object_id,
        parent_object_id,
        object->inode,
        object->dentry,
        name,
        references,
        last_used);
    if (status != 0) {
        return status;
    }
    object->linked = linked;
    object->dirty = dirty;
    object->native_read_file = native_read_file;
    object->native_write_file = native_write_file;
    FS_PROFILE_END(object_refresh_cycles, profile_start);
    return 0;
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

static int fs_object_ensure_write_file(
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *object,
    void *vfsmount)
{
    if (ops == NULL || object == NULL || vfsmount == NULL) {
        return -22;
    }
    if (object->native_write_file != NULL) {
        return 0;
    }
    void *file = NULL;
    const int status = kb_fs_subsystem_file_open(
        vfsmount,
        object->dentry,
        KB_FS_FILE_ACCESS_WRITE,
        &file);
    if (status != 0) {
        return status;
    }
    object->native_write_file = file;
    return 0;
}

static int fs_object_ensure_read_file(
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *object,
    void *vfsmount)
{
    if (ops == NULL || object == NULL || vfsmount == NULL) {
        return -22;
    }
    if (object->native_read_file != NULL) {
        return 0;
    }
    void *file = NULL;
    const int status = kb_fs_subsystem_file_open(
        vfsmount,
        object->dentry,
        KB_FS_FILE_ACCESS_READ,
        &file);
    if (status != 0) {
        return status;
    }
    object->native_read_file = file;
    return 0;
}

static int fs_object_close_native_files(koboxd_fs_object_t *object)
{
    if (object == NULL) {
        return -22;
    }
    int status = 0;
    if (object->native_write_file != NULL) {
        void *file = object->native_write_file;
        object->native_write_file = NULL;
        status = kb_fs_subsystem_file_close(file);
    }
    if (object->native_read_file != NULL) {
        void *file = object->native_read_file;
        object->native_read_file = NULL;
        const int read_status = kb_fs_subsystem_file_close(file);
        if (status == 0) {
            status = read_status;
        }
    }
    return status;
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
        module_symbol(module, "ext4_file_read_iter", &out_ops->file_read_iter) &&
        module_symbol(module, "ext4_file_write_iter", &out_ops->file_write_iter) &&
        module_symbol(module, "ext4_lookup", &out_ops->lookup) &&
        module_symbol(module, "ext4_setattr", &out_ops->setattr) &&
        module_symbol(module, "ext4_inode_is_fast_symlink", &out_ops->inode_is_fast_symlink)))
    {
        return 0;
    }

    out_ops->create = read_pointer_field(out_ops->dir_inode_operations, KOBOXD_INODE_OP_CREATE_OFFSET);
    out_ops->link = read_pointer_field(out_ops->dir_inode_operations, KOBOXD_INODE_OP_LINK_OFFSET);
    out_ops->unlink = read_pointer_field(out_ops->dir_inode_operations, KOBOXD_INODE_OP_UNLINK_OFFSET);
    out_ops->symlink = read_pointer_field(out_ops->dir_inode_operations, KOBOXD_INODE_OP_SYMLINK_OFFSET);
    out_ops->mkdir = read_pointer_field(out_ops->dir_inode_operations, KOBOXD_INODE_OP_MKDIR_OFFSET);
    out_ops->rmdir = read_pointer_field(out_ops->dir_inode_operations, KOBOXD_INODE_OP_RMDIR_OFFSET);
    out_ops->mknod = read_pointer_field(out_ops->dir_inode_operations, KOBOXD_INODE_OP_MKNOD_OFFSET);
    out_ops->rename = read_pointer_field(out_ops->dir_inode_operations, KOBOXD_INODE_OP_RENAME_OFFSET);
    out_ops->dir_iterate_shared = read_pointer_field(
        out_ops->dir_operations,
        KOBOXD_FILE_OP_ITERATE_SHARED_OFFSET);
    const int ready = out_ops->dir_iterate_shared != NULL &&
        read_pointer_field(out_ops->file_operations, KOBOXD_FILE_OP_FSYNC_OFFSET) != NULL &&
        out_ops->create != NULL &&
        out_ops->link != NULL &&
        out_ops->unlink != NULL &&
        out_ops->symlink != NULL &&
        out_ops->mkdir != NULL &&
        out_ops->rmdir != NULL &&
        out_ops->mknod != NULL &&
        out_ops->rename != NULL &&
        out_ops->setattr != NULL;
    if (ready) {
        cached_module = module;
        cached_ops = *out_ops;
        cached_ready = 1;
    }
    return ready;
}

static int fs_retire_cached_inode(
    koboxd_fs_backend_t *backend,
    void *inode,
    void *dentry)
{
    if (backend == NULL || inode == NULL) {
        return -22;
    }
    const int dentry_owns_inode = dentry != NULL &&
        read_pointer_field(dentry, KOBOXD_DENTRY_INODE_OFFSET) == inode;
    kb_fs_subsystem_dput(dentry);
    if (!dentry_owns_inode) {
        kb_fs_subsystem_iput(inode);
    }
    return 0;
}

static int fs_ext4_inode_is_fast_symlink(
    const koboxd_ext4_operations_t *ops,
    void *inode)
{
    if (ops == NULL || ops->inode_is_fast_symlink == NULL || inode == NULL) {
        return 0;
    }
    int (*is_fast_fn)(void *) = NULL;
    memcpy(&is_fast_fn, &ops->inode_is_fast_symlink, sizeof(is_fast_fn));
    unsigned long old_gs = 0;
    const int has_gs = enter_ext4_call(ops->inode_is_fast_symlink, &old_gs);
    const int is_fast = is_fast_fn(inode);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return is_fast != 0;
}

static int fs_read_fast_symlink(
    void *dentry,
    void *inode,
    char *out_target,
    size_t length)
{
    if (dentry == NULL || inode == NULL || out_target == NULL) {
        return -22;
    }
    void *inode_ops = read_pointer_field(inode, KOBOXD_INODE_OP_OFFSET);
    if (inode_ops == NULL) {
        return -95;
    }
    void *get_link_op = read_pointer_field(inode_ops, KOBOXD_INODE_OP_GET_LINK_OFFSET);
    if (get_link_op == NULL) {
        return -95;
    }
    const char *(*get_link_fn)(void *, void *, void *) = NULL;
    memcpy(&get_link_fn, &get_link_op, sizeof(get_link_fn));
    uint8_t delayed_call[32];
    memset(delayed_call, 0, sizeof(delayed_call));
    unsigned long old_gs = 0;
    const int has_gs = enter_ext4_call(get_link_op, &old_gs);
    const char *target = get_link_fn(dentry, inode, delayed_call);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    const uintptr_t value = (uintptr_t)target;
    if (target == NULL || value >= UINTPTR_MAX - 4095u) {
        return target == NULL ? -5 : (int)(intptr_t)target;
    }
    memcpy(out_target, target, length);
    return 0;
}

static int fs_sync_filesystem(void *super_block)
{
    if (super_block == NULL) {
        return -22;
    }
    return kb_fs_subsystem_sync_filesystem(super_block);
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
    /* ext4 has already updated i_nlink before returning from unlink/rmdir or
     * replacement rename.  Keep filed's cached mirror coherent from that
     * authoritative inode instead of issuing a second ->getattr call. */
    if (object->inode != NULL) {
        object->nlink = read_u32_field(
            object->inode, KOBOXD_INODE_NLINK_OFFSET);
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
    void *vfsmount)
{
    if (object == NULL || object->inode == NULL) {
        return -22;
    }
    const int status = fs_file_fsync(ops, object, vfsmount);
    if (status != 0) {
        return status;
    }
    object->dirty = 0;
    return fs_object_refresh(object);
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

static int ext4_lookup_dentry_at(
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

    void *dentry = kb_fs_subsystem_d_alloc_name(parent_dentry, name);
    if (dentry == NULL) {
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=kobox_lookup_dentry_alloc status=-12 name=%s\n",
            name);
        return -12;
    }

    unsigned long old_gs = 0;
    int has_gs = enter_ext4_call(ops->lookup, &old_gs);
    void *(*lookup_fn)(void *, void *, unsigned int) = NULL;
    memcpy(&lookup_fn, &ops->lookup, sizeof(lookup_fn));
    void *result = lookup_fn(parent_inode, dentry, 0);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }

    if ((intptr_t)result < 0 && (intptr_t)result >= -4095) {
        kb_fs_subsystem_dput(dentry);
        return (int)(intptr_t)result;
    }
    if (result != NULL) {
        kb_fs_subsystem_dput(dentry);
        dentry = result;
    }
    *out_inode = read_pointer_field(dentry, KOBOXD_DENTRY_INODE_OFFSET);
    *out_dentry = dentry;
    return 0;
}

static int ext4_lookup_name_at(
    const koboxd_ext4_operations_t *ops,
    void *parent_inode,
    void *parent_dentry,
    const char *name,
    void **out_inode,
    void **out_dentry)
{
    int status = ext4_lookup_dentry_at(
        ops,
        parent_inode,
        parent_dentry,
        name,
        out_inode,
        out_dentry);
    if (status != 0) {
        return status;
    }
    if (*out_inode == NULL) {
        kb_fs_subsystem_dput(*out_dentry);
        *out_dentry = NULL;
        return -2;
    }
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

static int fs_lookup_or_prepare_child(
    koboxd_fs_backend_t *backend,
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *parent,
    const char *name,
    koboxd_fs_object_t **out_object,
    void **out_negative_dentry)
{
    if (backend == NULL || ops == NULL || parent == NULL || name == NULL ||
        out_object == NULL || out_negative_dentry == NULL)
    {
        return -22;
    }
    FS_PROFILE_BEGIN(cache_lookup_start);
    *out_object = fs_object_by_parent_name(backend, parent->object_id, name);
    *out_negative_dentry = NULL;
    FS_PROFILE_END(cache_lookup_cycles, cache_lookup_start);
    if (*out_object != NULL) {
        return 0;
    }

    void *lookup_inode = NULL;
    void *lookup_dentry = NULL;
    FS_PROFILE_BEGIN(ext4_lookup_start);
    int lookup_status = ext4_lookup_dentry_at(
        ops,
        parent->inode,
        parent->dentry,
        name,
        &lookup_inode,
        &lookup_dentry);
    FS_PROFILE_END(ext4_lookup_cycles, ext4_lookup_start);
    if (lookup_status != 0 || lookup_dentry == NULL) {
        return lookup_status != 0 ? lookup_status : -5;
    }
    if (lookup_inode == NULL) {
        *out_negative_dentry = lookup_dentry;
        return 0;
    }

    uint64_t object_id = 0;
    FS_PROFILE_BEGIN(object_register_start);
    int status = fs_object_register(
        backend,
        parent->object_id,
        lookup_inode,
        lookup_dentry,
        name,
        0,
        &object_id);
    FS_PROFILE_END(object_register_cycles, object_register_start);
    if (status != 0) {
        const int discard_status = fs_discard_unregistered_lookup(
            backend,
            lookup_inode,
            lookup_dentry);
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=kobox_lookup_cache status=%d discard=%d "
            "parent=%llu name=%s\n",
            status,
            discard_status,
            (unsigned long long)parent->object_id,
            name);
        return discard_status != 0 ? discard_status : status;
    }
    *out_object = fs_object_by_id(backend, object_id);
    return *out_object != NULL ? 0 : -2;
}

static int fs_lookup_or_cache_child(
    koboxd_fs_backend_t *backend,
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *parent,
    const char *name,
    koboxd_fs_object_t **out_object)
{
    if (out_object == NULL) {
        return -22;
    }
    void *negative_dentry = NULL;
    int status = fs_lookup_or_prepare_child(
        backend,
        ops,
        parent,
        name,
        out_object,
        &negative_dentry);
    if (status != 0) {
        return status;
    }
    if (*out_object != NULL) {
        return 0;
    }
    kb_fs_subsystem_dput(negative_dentry);
    return -2;
}

static int fs_call_ext4_child_create(
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *parent,
    const char *name,
    uint16_t mode,
    koboxd_ext4_child_create_kind_t kind,
    void *negative_dentry,
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

    void *dentry = negative_dentry;
    if (dentry == NULL) {
        dentry = kb_fs_subsystem_d_alloc_name(parent->dentry, name);
        if (dentry == NULL) {
            return -12;
        }
    }

    static uint8_t mnt_idmap[136];
    unsigned long old_gs = 0;
    int has_gs = 0;
    int result = -22;
    fs_inode_write_lock(parent->inode, "create_parent");
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
    fs_inode_write_unlock(parent->inode);
    void *inode = read_pointer_field(dentry, KOBOXD_DENTRY_INODE_OFFSET);
    if (result != 0 || inode == NULL) {
        kb_fs_subsystem_dput(dentry);
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
    koboxd_fs_object_t *existing = NULL;
    void *negative_dentry = NULL;
    int lookup_status = fs_lookup_or_prepare_child(
        backend,
        ops,
        parent,
        name,
        &existing,
        &negative_dentry);
    if (lookup_status != 0) {
        koboxd_fs_object_stats_t stats;
        koboxd_fs_backend_object_stats(backend, &stats);
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=ext4_precreate_lookup status=%d "
            "objects=%u referenced=%u cached=%u evictions=%llu "
            "parent=%llu name=%s\n",
            lookup_status,
            stats.used,
            stats.referenced,
            stats.cached,
            (unsigned long long)stats.evictions,
            (unsigned long long)parent->object_id,
            name);
        return lookup_status;
    }
    if (existing != NULL) {
        return -17;
    }

    void *inode = NULL;
    void *dentry = NULL;
    KOBOXD_FS_STAGE("[koboxd-fs-stage] op=create stage=call parent=%llu name=%s\n",
        (unsigned long long)parent->object_id,
        name);
    FS_PROFILE_BEGIN(ext4_create_start);
    int status = fs_call_ext4_child_create(
        ops,
        parent,
        name,
        mode,
        kind,
        negative_dentry,
        &inode,
        &dentry);
    FS_PROFILE_END(ext4_create_cycles, ext4_create_start);
    KOBOXD_FS_STAGE("[koboxd-fs-stage] op=create stage=returned parent=%llu name=%s status=%d\n",
        (unsigned long long)parent->object_id,
        name,
        status);
    if (status != 0) {
        koboxd_fs_object_stats_t stats;
        koboxd_fs_backend_object_stats(backend, &stats);
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=ext4_create status=%d objects=%u "
            "referenced=%u cached=%u evictions=%llu parent=%llu name=%s\n",
            status,
            stats.used,
            stats.referenced,
            stats.cached,
            (unsigned long long)stats.evictions,
            (unsigned long long)parent->object_id,
            name);
        return status;
    }
    FS_PROFILE_BEGIN(object_register_start);
    status = fs_object_register(backend, parent->object_id, inode, dentry, name, 1, out_object_id);
    FS_PROFILE_END(object_register_cycles, object_register_start);
    if (status != 0) {
        koboxd_fs_object_stats_t stats;
        koboxd_fs_backend_object_stats(backend, &stats);
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=kobox_object_register status=%d "
            "objects=%u referenced=%u cached=%u evictions=%llu "
            "parent=%llu name=%s\n",
            status,
            stats.used,
            stats.referenced,
            stats.cached,
            (unsigned long long)stats.evictions,
            (unsigned long long)parent->object_id,
            name);
        const int discard_status = fs_discard_unregistered_lookup(backend, inode, dentry);
        return discard_status != 0 ? discard_status : status;
    }
    fs_mark_metadata_dirty(backend);
    return 0;
}

static uint16_t fs_mode_from_linux_d_type(unsigned int d_type)
{
    switch (d_type) {
    case 1: return 0010000u;
    case 2: return 0020000u;
    case 4: return 0040000u;
    case 6: return 0060000u;
    case 8: return 0100000u;
    case 10: return 0120000u;
    case 12: return 0140000u;
    default: return 0;
    }
}

static int fs_readdir_scan_add(
    koboxd_readdir_scan_t *scan,
    const char *name,
    size_t name_len,
    uint64_t inode_number,
    unsigned int d_type)
{
    if (scan == NULL || name == NULL || name_len == 0) {
        return 1;
    }
    if ((name_len == 1 && name[0] == '.') ||
        (name_len == 2 && name[0] == '.' && name[1] == '.'))
    {
        return 1;
    }

    if (scan->visible < scan->skip) {
        scan->visible++;
        return 1;
    }
    if (scan->count >= scan->capacity) {
        scan->full = 1;
        return 0;
    }

    size_t copy_len = name_len;
    if (copy_len >= KOBOXD_FS_BACKEND_NAME_BYTES) {
        copy_len = KOBOXD_FS_BACKEND_NAME_BYTES - 1u;
    }
    koboxd_readdir_scan_entry_t *entry = &scan->entries[scan->count];
    entry->inode_number = inode_number;
    entry->mode = fs_mode_from_linux_d_type(d_type);
    memcpy(entry->name, name, copy_len);
    entry->name[copy_len] = '\0';
    scan->count++;
    scan->visible++;
    if (scan->count >= scan->capacity) {
        scan->full = 1;
        return 0;
    }
    return 1;
}

static int fs_native_readdir_actor(
    void *context,
    const char *name,
    int name_len,
    int64_t position,
    uint64_t inode_number,
    unsigned int d_type)
{
    (void)position;
    koboxd_native_dir_context_t *dir_context =
        (koboxd_native_dir_context_t *)context;
    if (dir_context == NULL || dir_context->scan == NULL || name_len < 0) {
        return 0;
    }
    return fs_readdir_scan_add(
        dir_context->scan,
        name,
        (size_t)name_len,
        inode_number,
        d_type);
}

static int fs_read_ext4_dir_native(
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *dir,
    void *vfsmount,
    uint64_t offset,
    size_t capacity,
    koboxd_readdir_scan_t *scan)
{
    if (ops == NULL || dir == NULL || dir->inode == NULL ||
        vfsmount == NULL || scan == NULL)
    {
        return -22;
    }

    memset(scan, 0, sizeof(*scan));
    if (capacity > KOBOXD_READDIR_PAGE_MAX_ENTRIES) {
        return -22;
    }
    scan->skip = offset;
    scan->capacity = capacity;
    if (capacity == 0) {
        return 0;
    }

    if (ops->dir_iterate_shared == NULL) {
        return -5;
    }

    void *file = NULL;
    int (*iterate_fn)(void *, void *) = NULL;
    memcpy(&iterate_fn, &ops->dir_iterate_shared, sizeof(iterate_fn));
    int result = kb_fs_subsystem_file_open(
        vfsmount,
        dir->dentry,
        KB_FS_FILE_ACCESS_READ,
        &file);
    if (result != 0) {
        return result;
    }
    write_u64_field(file, KOBOXD_NATIVE_FILE_POSITION_OFFSET, 0);

    koboxd_native_dir_context_t context = {
        .actor = fs_native_readdir_actor,
        .position = 0,
        .scan = scan,
    };
    /* iterate_shared() is normally entered through Linux iterate_dir(), which
     * holds the directory inode's shared lock for the whole callback.  ext4's
     * readdir implementation temporarily drops and reacquires that lock when
     * it detects a directory change.  Calling it without the outer lock leaves
     * the reacquired reader behind and the next create deadlocks on itself. */
    fs_inode_read_lock(dir->inode, "readdir_parent");
    unsigned long old_gs = 0;
    int has_gs = enter_ext4_call(ops->dir_iterate_shared, &old_gs);
    result = iterate_fn(file, &context);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    fs_inode_read_unlock(dir->inode);

    const int release_result = kb_fs_subsystem_file_close(file);
    return result != 0 ? result : release_result;
}

static int fs_scanned_dirent_to_object(
    koboxd_fs_backend_t *backend,
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *dir,
    const koboxd_readdir_scan_entry_t *entry,
    koboxd_fs_object_t *out_object)
{
    if (backend == NULL || ops == NULL || dir == NULL || entry == NULL || out_object == NULL) {
        return -22;
    }

    koboxd_fs_object_t *cached = fs_object_by_parent_name(backend, dir->object_id, entry->name);
    if (cached != NULL) {
        *out_object = *cached;
        return 0;
    }

    /* Directory enumeration must not materialize a backend object capability. */
    memset(out_object, 0, sizeof(*out_object));
    out_object->parent_object_id = dir->object_id;
    out_object->inode_number = entry->inode_number;
    out_object->mode = entry->mode;
    out_object->used = 1;
    out_object->linked = 1;
    snprintf(out_object->name, sizeof(out_object->name), "%s", entry->name);
    if (out_object->mode == 0) {
        void *inode = NULL;
        void *dentry = NULL;
        const int status = ext4_lookup_name_at(
            ops,
            dir->inode,
            dir->dentry,
            entry->name,
            &inode,
            &dentry);
        if (status != 0) {
            return status;
        }
        out_object->inode_number = read_u64_field(inode, KOBOXD_INODE_NUMBER_OFFSET);
        out_object->mode = read_u16_field(inode, KOBOXD_INODE_MODE_OFFSET);
        const int discard_status = fs_discard_unregistered_lookup(backend, inode, dentry);
        if (discard_status != 0) {
            return discard_status;
        }
    }
    return 0;
}

static int fs_file_read_once(
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *object,
    void *vfsmount,
    uint64_t offset,
    void *buffer,
    size_t length,
    size_t buffer_capacity,
    uint32_t ki_flags)
{
    if (ops == NULL || ops->file_read_iter == NULL ||
        object == NULL || object->inode == NULL || object->dentry == NULL ||
        vfsmount == NULL ||
        buffer == NULL || length > buffer_capacity)
    {
        return -22;
    }
    uint8_t kiocb[KOBOXD_FAKE_KIOCB_BYTES];
    uint8_t iter[KOBOXD_FAKE_IOV_ITER_BYTES];
    void *mapping = read_pointer_field(object->inode, KOBOXD_INODE_MAPPING_OFFSET);
    if (mapping == NULL) {
        return -12;
    }
    memset(kiocb, 0, sizeof(kiocb));
    memset(iter, 0, sizeof(iter));
    int status = fs_object_ensure_read_file(ops, object, vfsmount);
    if (status != 0) {
        return status;
    }
    void *file = object->native_read_file;
    write_pointer_field(kiocb, KOBOXD_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, KOBOXD_KIOCB_POS_OFFSET, offset);
    write_u32_field(kiocb, KOBOXD_KIOCB_FLAGS_OFFSET, ki_flags);
    write_u64_field(iter, KOBOXD_IOV_ITER_COUNT_OFFSET, (uint64_t)length);
    write_pointer_field(iter, KOBOXD_IOV_ITER_BUFFER_OFFSET, buffer);
    write_u64_field(iter, KOBOXD_IOV_ITER_BUFFER_CAPACITY_OFFSET, (uint64_t)buffer_capacity);

    long (*read_iter_fn)(void *, void *) = NULL;
    memcpy(&read_iter_fn, &ops->file_read_iter, sizeof(read_iter_fn));
    unsigned long old_gs = 0;
    int has_gs = enter_ext4_call(ops->file_read_iter, &old_gs);
    long result = read_iter_fn(kiocb, iter);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return (int)result;
}

static int fs_file_read(
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *object,
    void *vfsmount,
    uint64_t offset,
    void *buffer,
    size_t length,
    size_t buffer_capacity)
{
    if (buffer == NULL || length > buffer_capacity) {
        return -22;
    }

    const size_t direct_length =
        length & ~(size_t)(KOBOXD_DIRECT_READ_ALIGNMENT - 1u);
    const int direct_eligible =
        direct_length >= KOBOXD_DIRECT_READ_MIN_BYTES &&
        (offset & (KOBOXD_DIRECT_READ_ALIGNMENT - 1u)) == 0 &&
        ((uintptr_t)buffer & (KOBOXD_DIRECT_READ_ALIGNMENT - 1u)) == 0;
    if (!direct_eligible) {
        return fs_file_read_once(
            ops, object, vfsmount, offset, buffer, length, buffer_capacity, 0);
    }

    const int direct_result = fs_file_read_once(
        ops,
        object,
        vfsmount,
        offset,
        buffer,
        direct_length,
        buffer_capacity,
        KOBOXD_IOCB_DIRECT);
    if (direct_result < 0) {
        /* Some ext4 file types or mappings can reject direct I/O even when the
         * request itself is aligned.  They retain the established buffered
         * behavior instead of turning the optimization into a compatibility
         * requirement. */
        return fs_file_read_once(
            ops, object, vfsmount, offset, buffer, length, buffer_capacity, 0);
    }
    if ((size_t)direct_result < direct_length || direct_length == length) {
        return direct_result;
    }

    const size_t tail_length = length - direct_length;
    const int tail_result = fs_file_read_once(
        ops,
        object,
        vfsmount,
        offset + direct_length,
        (uint8_t *)buffer + direct_length,
        tail_length,
        buffer_capacity - direct_length,
        0);
    if (tail_result < 0) {
        return direct_result == 0 ? tail_result : direct_result;
    }
    return direct_result + tail_result;
}

static int fs_file_write(
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *object,
    void *vfsmount,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    if (ops == NULL || ops->file_write_iter == NULL ||
        object == NULL || object->inode == NULL || object->dentry == NULL ||
        vfsmount == NULL ||
        buffer == NULL)
    {
        return -22;
    }
    uint8_t kiocb[KOBOXD_FAKE_KIOCB_BYTES];
    uint8_t iter[KOBOXD_FAKE_IOV_ITER_BYTES];
    void *mapping = read_pointer_field(object->inode, KOBOXD_INODE_MAPPING_OFFSET);
    if (mapping == NULL) {
        return -12;
    }
    memset(kiocb, 0, sizeof(kiocb));
    memset(iter, 0, sizeof(iter));
    int status = fs_object_ensure_write_file(ops, object, vfsmount);
    if (status != 0) {
        return status;
    }
    void *file = object->native_write_file;
    write_pointer_field(kiocb, KOBOXD_KIOCB_FILE_OFFSET, file);
    write_u64_field(kiocb, KOBOXD_KIOCB_POS_OFFSET, offset);
    write_u64_field(iter, KOBOXD_IOV_ITER_COUNT_OFFSET, (uint64_t)length);
    write_pointer_field(iter, KOBOXD_IOV_ITER_BUFFER_OFFSET, (void *)(uintptr_t)buffer);
    write_u64_field(iter, KOBOXD_IOV_ITER_BUFFER_CAPACITY_OFFSET, (uint64_t)length);
    iter[KOBOXD_IOV_ITER_DATA_SOURCE_OFFSET] = 1;

    long (*write_iter_fn)(void *, void *) = NULL;
    memcpy(&write_iter_fn, &ops->file_write_iter, sizeof(write_iter_fn));
    unsigned long old_gs = 0;
    int has_gs = enter_ext4_call(ops->file_write_iter, &old_gs);
    long result = write_iter_fn(kiocb, iter);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    return (int)result;
}

static int fs_file_fsync(
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *object,
    void *vfsmount)
{
    if (ops == NULL || ops->file_operations == NULL ||
        object == NULL || object->inode == NULL || object->dentry == NULL ||
        vfsmount == NULL)
    {
        return -22;
    }
    void *mapping = read_pointer_field(object->inode, KOBOXD_INODE_MAPPING_OFFSET);
    if (mapping == NULL) {
        return -12;
    }
    int status = fs_object_ensure_write_file(ops, object, vfsmount);
    if (status != 0) {
        return status;
    }
    void *file = object->native_write_file;
    status = kb_fs_subsystem_vfs_fsync_range(file, 0, INT64_MAX, 0);
    return status;
}

static int fs_file_setattr(
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *object,
    const koboxd_linux_iattr_t *iattr)
{
    KOBOXD_FS_STAGE("[koboxd-fs-stage] op=setattr stage=enter object=%llu valid=0x%x\n",
        object != NULL ? (unsigned long long)object->object_id : 0ull,
        iattr != NULL ? iattr->ia_valid : 0u);
    if (ops == NULL || ops->setattr == NULL ||
        object == NULL || object->inode == NULL || object->dentry == NULL || iattr == NULL)
    {
        return -22;
    }
    static uint8_t mnt_idmap[136];
    int (*setattr_fn)(void *, void *, void *) = NULL;
    memcpy(&setattr_fn, &ops->setattr, sizeof(setattr_fn));
    fs_inode_write_lock(object->inode, "setattr_object");
    unsigned long old_gs = 0;
    int has_gs = enter_ext4_call(ops->setattr, &old_gs);
    int status = setattr_fn(mnt_idmap, object->dentry, (void *)(uintptr_t)iattr);
    KOBOXD_FS_STAGE("[koboxd-fs-stage] op=setattr stage=returned object=%llu status=%d\n",
        (unsigned long long)object->object_id,
        status);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    fs_inode_write_unlock(object->inode);
    return status;
}

static int fs_file_truncate(
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *object,
    void *vfsmount,
    uint64_t size)
{
    if (ops == NULL || object == NULL || vfsmount == NULL || size > INT64_MAX) {
        return -22;
    }
    /* Linux ftruncate operates on an already opened writable struct file.
     * ext4_file_open() attaches EXT4_I(inode)->jinode, which ordered-data
     * growth and partial-page zeroing later pass to JBD2.  Calling
     * ext4_setattr() directly after an inode-cache reload skips that required
     * lifecycle and hands JBD2 a NULL jinode. */
    const int open_status = fs_object_ensure_write_file(ops, object, vfsmount);
    if (open_status != 0) {
        return open_status;
    }
    koboxd_linux_iattr_t iattr;
    memset(&iattr, 0, sizeof(iattr));
    iattr.ia_valid = KOBOXD_IATTR_SIZE;
    iattr.ia_size = (int64_t)size;
    iattr.ia_file = object->native_write_file;
    return fs_file_setattr(ops, object, &iattr);
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
    printf("[filed-storage] rootfs mounted fs=ext4 reads=%u\n", backend->mount_result.block_read_count);
    backend->ext4_module = ext4_module;
    backend->next_object_id = 2;
    status = fill_object_from_inode(
        &backend->objects[0],
        1,
        1,
        backend->mount_result.root_inode,
        backend->mount_result.root_dentry,
        "/",
        1,
        fs_object_next_clock(backend));
    if (status != 0) {
        return status;
    }
    backend->mounted = 1;
    return 0;
}

int koboxd_fs_backend_statfs(
    koboxd_fs_backend_t *backend,
    storage_statfs_reply_t *out_statfs)
{
    if (backend == NULL || out_statfs == NULL || !backend->mounted ||
        backend->mount_result.super_block == NULL ||
        backend->mount_result.root_dentry == NULL)
    {
        return -22;
    }
    void *super_operations = read_pointer_field(
        backend->mount_result.super_block,
        KOBOXD_SUPER_BLOCK_OPS_OFFSET);
    if (super_operations == NULL) return -95;
    void *statfs_operation = read_pointer_field(
        super_operations,
        KOBOXD_SUPER_OP_STATFS_OFFSET);
    if (statfs_operation == NULL) return -95;

    int (*statfs_fn)(void *, void *) = NULL;
    memcpy(&statfs_fn, &statfs_operation, sizeof(statfs_fn));
    storage_statfs_reply_t native_statfs;
    memset(&native_statfs, 0, sizeof(native_statfs));
    unsigned long old_gs = 0;
    const int has_gs = enter_ext4_call(statfs_operation, &old_gs);
    const int status = statfs_fn(
        backend->mount_result.root_dentry,
        &native_statfs);
    if (has_gs) kb_shim_leave_kernel_gs(old_gs);
    if (status != 0) return status;
    *out_statfs = native_statfs;
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
        const int acquire_status = fs_object_acquire(backend, cached);
        if (acquire_status != 0) {
            return acquire_status;
        }
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
    status = fs_object_register(backend, parent_object_id, inode, dentry, name, 1, out_object_id);
    if (status != 0) {
        koboxd_fs_object_stats_t stats;
        koboxd_fs_backend_object_stats(backend, &stats);
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=kobox_lookup_register status=%d "
            "objects=%u referenced=%u cached=%u evictions=%llu "
            "parent=%llu name=%s\n",
            status,
            stats.used,
            stats.referenced,
            stats.cached,
            (unsigned long long)stats.evictions,
            (unsigned long long)parent_object_id,
            name);
        const int discard_status = fs_discard_unregistered_lookup(backend, inode, dentry);
        if (discard_status != 0) {
            return discard_status;
        }
    }
    return status;
}

int koboxd_fs_backend_readlink(
    koboxd_fs_backend_t *backend,
    uint64_t object_id,
    char *out_target,
    size_t target_capacity,
    size_t *out_length)
{
    if (backend == NULL || out_target == NULL || out_length == NULL ||
        target_capacity == 0 || !backend->mounted)
    {
        return -22;
    }
    *out_length = 0;
    koboxd_fs_object_t *object = fs_object_by_id(backend, object_id);
    if (object == NULL || object->inode == NULL) {
        return -2;
    }
    if ((object->mode & KOBOXD_MODE_TYPE_MASK) != 0120000) {
        return -22;
    }

    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }

    size_t length = object->size < target_capacity ?
        (size_t)object->size : target_capacity;
    if (length == 0) {
        return 0;
    }
    if (fs_ext4_inode_is_fast_symlink(&ops, object->inode)) {
        const int status = fs_read_fast_symlink(
            object->dentry,
            object->inode,
            out_target,
            length);
        if (status != 0) {
            return status;
        }
        *out_length = length;
        return 0;
    }

    const int read_result = fs_file_read(
        &ops,
        object,
        backend->mount_result.root_vfsmount,
        0,
        out_target,
        length,
        target_capacity);
    if (read_result < 0) {
        return read_result;
    }
    *out_length = (size_t)read_result;
    return 0;
}

int koboxd_fs_backend_symlink(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    const char *target,
    uint64_t *out_object_id)
{
    if (backend == NULL || name == NULL || target == NULL ||
        target[0] == '\0' || out_object_id == NULL || !backend->mounted)
    {
        return -22;
    }
    *out_object_id = 0;
    koboxd_fs_object_t *parent = NULL;
    int status = fs_get_parent_object(backend, parent_object_id, &parent);
    if (status != 0) return status;
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) return -5;
    koboxd_fs_object_t *existing = NULL;
    void *dentry = NULL;
    status = fs_lookup_or_prepare_child(
        backend, &ops, parent, name, &existing, &dentry);
    if (status != 0) return status;
    if (existing != NULL) return -17;

    static uint8_t mnt_idmap[136];
    int (*symlink_fn)(void *, void *, void *, const char *) = NULL;
    memcpy(&symlink_fn, &ops.symlink, sizeof(symlink_fn));
    fs_inode_write_lock(parent->inode, "symlink_parent");
    unsigned long old_gs = 0;
    const int has_gs = enter_ext4_call(ops.symlink, &old_gs);
    status = symlink_fn(mnt_idmap, parent->inode, dentry, target);
    if (has_gs) kb_shim_leave_kernel_gs(old_gs);
    fs_inode_write_unlock(parent->inode);
    void *inode = read_pointer_field(dentry, KOBOXD_DENTRY_INODE_OFFSET);
    if (status != 0 || inode == NULL) {
        kb_fs_subsystem_dput(dentry);
        return status != 0 ? status : -5;
    }
    status = fs_object_register(
        backend, parent->object_id, inode, dentry, name, 1, out_object_id);
    if (status != 0) {
        const int discard_status = fs_discard_unregistered_lookup(backend, inode, dentry);
        return discard_status != 0 ? discard_status : status;
    }
    fs_mark_metadata_dirty(backend);
    return 0;
}

int koboxd_fs_backend_link(
    koboxd_fs_backend_t *backend,
    uint64_t old_object_id,
    uint64_t new_parent_object_id,
    const char *new_name,
    uint64_t *out_object_id)
{
    if (backend == NULL || old_object_id == 0 || new_parent_object_id == 0 ||
        new_name == NULL || out_object_id == NULL || !backend->mounted)
    {
        return -22;
    }
    *out_object_id = 0;

    koboxd_fs_object_t *old_object = fs_object_by_id(backend, old_object_id);
    if (old_object == NULL || old_object->inode == NULL || old_object->dentry == NULL) {
        return -2;
    }
    if ((old_object->mode & KOBOXD_MODE_TYPE_MASK) == 0040000u) {
        return -1;
    }

    koboxd_fs_object_t *new_parent = NULL;
    int status = fs_get_parent_object(backend, new_parent_object_id, &new_parent);
    if (status != 0) {
        return status;
    }
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    if (old_object->dirty) {
        status = fs_flush_dirty_object(
            &ops,
            old_object,
            backend->mount_result.root_vfsmount);
        if (status != 0) {
            return status;
        }
    }

    koboxd_fs_object_t *existing = NULL;
    void *new_dentry = NULL;
    status = fs_lookup_or_prepare_child(
        backend,
        &ops,
        new_parent,
        new_name,
        &existing,
        &new_dentry);
    if (status != 0) {
        return status;
    }
    if (existing != NULL) {
        return -17;
    }

    int (*link_fn)(void *, void *, void *) = NULL;
    memcpy(&link_fn, &ops.link, sizeof(link_fn));
    /* Match vfs_link(): the destination directory is locked by the path
     * creation side and the source inode is locked around ->link.  ext4_link
     * updates the source inode's link count, ctime, and journal state. */
    fs_inode_write_lock(new_parent->inode, "link_new_parent");
    fs_inode_write_lock(old_object->inode, "link_source");
    unsigned long old_gs = 0;
    const int has_gs = enter_ext4_call(ops.link, &old_gs);
    status = link_fn(old_object->dentry, new_parent->inode, new_dentry);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    fs_inode_write_unlock(old_object->inode);
    fs_inode_write_unlock(new_parent->inode);

    void *inode = read_pointer_field(new_dentry, KOBOXD_DENTRY_INODE_OFFSET);
    if (status != 0 || inode == NULL) {
        kb_fs_subsystem_dput(new_dentry);
        return status != 0 ? status : -5;
    }
    status = fs_object_register(
        backend,
        new_parent->object_id,
        inode,
        new_dentry,
        new_name,
        1,
        out_object_id);
    if (status != 0) {
        const int discard_status = fs_discard_unregistered_lookup(backend, inode, new_dentry);
        return discard_status != 0 ? discard_status : status;
    }
    fs_mark_metadata_dirty(backend);
    return 0;
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
    if (create_status != 0) {
        koboxd_fs_object_stats_t stats;
        koboxd_fs_backend_object_stats(backend, &stats);
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=kobox_create_return status=%d "
            "objects=%u referenced=%u cached=%u evictions=%llu "
            "parent=%llu name=%s\n",
            create_status,
            stats.used,
            stats.referenced,
            stats.cached,
            (unsigned long long)stats.evictions,
            (unsigned long long)parent_object_id,
            name);
    }
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
    int status = fs_file_truncate(
        &ops,
        object,
        backend->mount_result.root_vfsmount,
        size);
    if (status == 0) {
        fs_mark_object_metadata_dirty(backend, object);
        status = fs_object_refresh(object);
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
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    koboxd_linux_iattr_t iattr;
    memset(&iattr, 0, sizeof(iattr));
    if ((mask & KOBOXD_TIME_UPDATE_ATIME) != 0) {
        iattr.ia_valid |= KOBOXD_IATTR_ATIME;
        iattr.ia_atime.tv_sec = atime_sec;
        iattr.ia_atime.tv_nsec = atime_nsec;
    }
    if ((mask & KOBOXD_TIME_UPDATE_MTIME) != 0) {
        iattr.ia_valid |= KOBOXD_IATTR_MTIME | KOBOXD_IATTR_CTIME;
        iattr.ia_mtime.tv_sec = mtime_sec;
        iattr.ia_mtime.tv_nsec = mtime_nsec;
        iattr.ia_ctime = iattr.ia_mtime;
    } else if ((mask & KOBOXD_TIME_UPDATE_ATIME) != 0) {
        iattr.ia_valid |= KOBOXD_IATTR_CTIME;
        iattr.ia_ctime = iattr.ia_atime;
    }
    int status = fs_file_setattr(&ops, object, &iattr);
    if (status == 0) {
        fs_mark_object_metadata_dirty(backend, object);
        status = fs_object_refresh(object);
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
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    koboxd_linux_iattr_t iattr;
    memset(&iattr, 0, sizeof(iattr));
    iattr.ia_valid = KOBOXD_IATTR_MODE;
    iattr.ia_mode = (uint16_t)((read_u16_field(object->inode, KOBOXD_INODE_MODE_OFFSET) &
        KOBOXD_MODE_TYPE_MASK) | (mode & KOBOXD_MODE_PERM_MASK));
    int status = fs_file_setattr(&ops, object, &iattr);
    if (status == 0) {
        fs_mark_object_metadata_dirty(backend, object);
        status = fs_object_refresh(object);
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
    if ((object->mode & KOBOXD_MODE_TYPE_MASK) == 0040000) {
        return -21;
    }

    KOBOXD_FS_TRACE("[koboxd-fs-trace] unlink parent=%llu object=%llu name=%s\n",
        (unsigned long long)parent_object_id,
        object != NULL ? (unsigned long long)object->object_id : 0ull,
        name);
    int (*unlink_fn)(void *, void *) = NULL;
    memcpy(&unlink_fn, &ops.unlink, sizeof(unlink_fn));
    fs_inode_write_lock(parent->inode, "unlink_parent");
    if (object->inode != parent->inode) {
        fs_inode_write_lock(object->inode, "unlink_target");
    }
    unsigned long old_gs = 0;
    int has_gs = enter_ext4_call(ops.unlink, &old_gs);
    FS_PROFILE_BEGIN(ext4_unlink_start);
    int result = object->dentry != NULL ? unlink_fn(parent->inode, object->dentry) : -2;
    FS_PROFILE_END(ext4_unlink_cycles, ext4_unlink_start);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (object->inode != parent->inode) {
        fs_inode_write_unlock(object->inode);
    }
    fs_inode_write_unlock(parent->inode);
    FS_PROFILE_BEGIN(unlink_post_start);
    if (result != 0 &&
        object->inode != NULL &&
        read_u32_field(object->inode, KOBOXD_INODE_NLINK_OFFSET) == 0)
    {
        result = 0;
    }
    if (result == 0) {
        fs_mark_object_unlinked(backend, object);
        fs_mark_metadata_dirty(backend);
    } else {
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=ext4_unlink status=%d parent=%llu "
            "object=%llu name=%s\n",
            result,
            (unsigned long long)parent_object_id,
            (unsigned long long)object->object_id,
            name);
    }
    KOBOXD_FS_TRACE("[koboxd-fs-trace] unlink done status=%d object=%llu linked=%u nlink=%u\n",
        result,
        object != NULL ? (unsigned long long)object->object_id : 0ull,
        object != NULL ? object->linked : 0u,
        object != NULL ? object->nlink : 0u);
    FS_PROFILE_END(unlink_post_cycles, unlink_post_start);
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

int koboxd_fs_backend_mknod(
    koboxd_fs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint16_t mode,
    uint64_t dev,
    uint64_t *out_object_id)
{
    const uint16_t type = mode & KOBOXD_MODE_TYPE_MASK;
    if (backend == NULL || name == NULL || out_object_id == NULL ||
        !backend->mounted ||
        (type != 0010000u && type != 0020000u &&
         type != 0060000u && type != 0140000u))
    {
        return -22;
    }
    *out_object_id = 0;
    koboxd_fs_object_t *parent = NULL;
    int status = fs_get_parent_object(backend, parent_object_id, &parent);
    if (status != 0) return status;
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) return -5;
    koboxd_fs_object_t *existing = NULL;
    void *dentry = NULL;
    status = fs_lookup_or_prepare_child(
        backend, &ops, parent, name, &existing, &dentry);
    if (status != 0) return status;
    if (existing != NULL) return -17;

    static uint8_t mnt_idmap[136];
    int (*mknod_fn)(void *, void *, void *, uint16_t, uint64_t) = NULL;
    memcpy(&mknod_fn, &ops.mknod, sizeof(mknod_fn));
    fs_inode_write_lock(parent->inode, "mknod_parent");
    unsigned long old_gs = 0;
    const int has_gs = enter_ext4_call(ops.mknod, &old_gs);
    status = mknod_fn(
        mnt_idmap,
        parent->inode,
        dentry,
        mode,
        fs_decode_linux_dev((uint32_t)dev));
    if (has_gs) kb_shim_leave_kernel_gs(old_gs);
    fs_inode_write_unlock(parent->inode);
    void *inode = read_pointer_field(dentry, KOBOXD_DENTRY_INODE_OFFSET);
    if (status != 0 || inode == NULL) {
        kb_fs_subsystem_dput(dentry);
        return status != 0 ? status : -5;
    }
    status = fs_object_register(
        backend, parent->object_id, inode, dentry, name, 1, out_object_id);
    if (status != 0) {
        const int discard_status = fs_discard_unregistered_lookup(backend, inode, dentry);
        return discard_status != 0 ? discard_status : status;
    }
    fs_mark_metadata_dirty(backend);
    return 0;
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
    if ((object->mode & KOBOXD_MODE_TYPE_MASK) != 0040000) {
        return -20;
    }

    KOBOXD_FS_TRACE("[koboxd-fs-trace] rmdir parent=%llu object=%llu name=%s\n",
        (unsigned long long)parent_object_id,
        object != NULL ? (unsigned long long)object->object_id : 0ull,
        name);
    int (*rmdir_fn)(void *, void *) = NULL;
    memcpy(&rmdir_fn, &ops.rmdir, sizeof(rmdir_fn));
    fs_inode_write_lock(parent->inode, "rmdir_parent");
    if (object->inode != parent->inode) {
        fs_inode_write_lock(object->inode, "rmdir_target");
    }
    unsigned long old_gs = 0;
    int has_gs = enter_ext4_call(ops.rmdir, &old_gs);
    int result = object->dentry != NULL ? rmdir_fn(parent->inode, object->dentry) : -2;
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    if (object->inode != parent->inode) {
        fs_inode_write_unlock(object->inode);
    }
    fs_inode_write_unlock(parent->inode);
    if (result == 0) {
        fs_mark_object_unlinked(backend, object);
        fs_mark_metadata_dirty(backend);
    } else if (result != KOBOXD_ERR_NOT_EMPTY) {
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=ext4_rmdir status=%d parent=%llu "
            "object=%llu name=%s\n",
            result,
            (unsigned long long)parent_object_id,
            (unsigned long long)object->object_id,
            name);
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
    KOBOXD_FS_STAGE("[koboxd-fs-stage] op=rename stage=enter old=%s new=%s\n",
        old_name != NULL ? old_name : "(null)",
        new_name != NULL ? new_name : "(null)");
    if (backend == NULL || old_name == NULL || new_name == NULL || out_object_id == NULL || !backend->mounted) {
        return -22;
    }
    *out_object_id = 0;
    koboxd_fs_object_t *old_parent = fs_object_by_id(backend, old_parent_object_id);
    koboxd_fs_object_t *new_parent = fs_object_by_id(backend, new_parent_object_id);
    if (old_parent == NULL || new_parent == NULL ||
        old_parent->inode == NULL || new_parent->inode == NULL)
    {
        return -2;
    }
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    koboxd_fs_object_t *object = NULL;
    int lookup_status = fs_lookup_or_cache_child(
        backend, &ops, old_parent, old_name, &object);
    if (lookup_status != 0 || object == NULL || object->dentry == NULL) {
        return lookup_status != 0 ? lookup_status : -2;
    }
    koboxd_fs_object_t *replaced = NULL;
    void *negative_new_dentry = NULL;
    if (old_parent_object_id == new_parent_object_id &&
        strcmp(old_name, new_name) == 0)
    {
        replaced = object;
    } else {
        lookup_status = fs_lookup_or_prepare_child(
            backend,
            &ops,
            new_parent,
            new_name,
            &replaced,
            &negative_new_dentry);
        if (lookup_status != 0) {
            return lookup_status;
        }
    }
    void *call_new_dentry = NULL;
    void *new_object_dentry = NULL;
    if (replaced != NULL && replaced != object) {
        call_new_dentry = replaced->dentry;
        new_object_dentry = kb_fs_subsystem_d_alloc_name(new_parent->dentry, new_name);
        if (new_object_dentry == NULL) {
            return -12;
        }
    } else {
        call_new_dentry = negative_new_dentry != NULL ?
            negative_new_dentry :
            kb_fs_subsystem_d_alloc_name(new_parent->dentry, new_name);
        if (call_new_dentry == NULL) {
            return -12;
        }
        new_object_dentry = call_new_dentry;
    }

    /* VFS dentry instantiation consumes the inode reference returned by
     * lookup/new_inode.  The cached source dentry currently owns that sole
     * reference.  Hold a transfer reference across ext4_rename(): on success
     * dput(old) releases the old dentry's ownership and the held reference is
     * transferred to the new dentry; on failure it is released explicitly. */
    if (kb_fs_subsystem_igrab(object->inode) == NULL) {
        if (new_object_dentry != NULL && new_object_dentry != call_new_dentry) {
            kb_fs_subsystem_dput(new_object_dentry);
        }
        if (call_new_dentry != NULL && (replaced == NULL || replaced == object)) {
            kb_fs_subsystem_dput(call_new_dentry);
        }
        return -5;
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
    fs_inode_lock_set_t rename_locks = {0};
    fs_inode_lock_set_add(&rename_locks, old_parent->inode);
    fs_inode_lock_set_add(&rename_locks, new_parent->inode);
    fs_inode_lock_set_add(&rename_locks, object->inode);
    if (replaced != NULL && replaced != object) {
        fs_inode_lock_set_add(&rename_locks, replaced->inode);
    }
    fs_inode_lock_set_acquire(&rename_locks, "rename_set");
    KOBOXD_FS_STAGE("[koboxd-fs-stage] op=rename stage=locked object=%llu\n",
        (unsigned long long)object->object_id);
    unsigned long old_gs = 0;
    int has_gs = enter_ext4_call(ops.rename, &old_gs);
    FS_PROFILE_BEGIN(ext4_rename_start);
    int result = rename_fn(
        mnt_idmap,
        old_parent->inode,
        object->dentry,
        new_parent->inode,
        call_new_dentry,
        0);
    FS_PROFILE_END(ext4_rename_cycles, ext4_rename_start);
    KOBOXD_FS_STAGE("[koboxd-fs-stage] op=rename stage=returned object=%llu status=%d\n",
        (unsigned long long)object->object_id,
        result);
    if (has_gs) {
        kb_shim_leave_kernel_gs(old_gs);
    }
    fs_inode_lock_set_release(&rename_locks);
    if (result != 0) {
        kb_fs_subsystem_iput(object->inode);
        if (call_new_dentry != NULL && (replaced == NULL || replaced == object)) {
            kb_fs_subsystem_dput(call_new_dentry);
        }
        if (new_object_dentry != NULL && new_object_dentry != call_new_dentry) {
            kb_fs_subsystem_dput(new_object_dentry);
        }
        return result;
    }

    FS_PROFILE_BEGIN(rename_post_start);
    if (replaced != NULL && replaced != object) {
        fs_mark_object_unlinked(backend, replaced);
    }
    kb_fs_subsystem_dput(object->dentry);
    object->dentry = new_object_dentry;
    object->inode_number = read_u64_field(object->inode, KOBOXD_INODE_NUMBER_OFFSET);
    kb_fs_subsystem_d_instantiate(object->dentry, object->inode);
    object->parent_object_id = new_parent_object_id;
    snprintf(object->name, sizeof(object->name), "%s", new_name);
    object->linked = 1;
    object->nlink = read_u32_field(object->inode, KOBOXD_INODE_NLINK_OFFSET);
    *out_object_id = object->object_id;
    fs_mark_metadata_dirty(backend);
    KOBOXD_FS_TRACE("[koboxd-fs-trace] rename done status=0 object=%llu\n",
        (unsigned long long)*out_object_id);
    FS_PROFILE_END(rename_post_cycles, rename_post_start);
    return 0;
}

static int fs_prepare_unlinked_object_release(
    koboxd_fs_backend_t *backend,
    const koboxd_ext4_operations_t *ops,
    koboxd_fs_object_t *object)
{
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
        return 0;
    }
    (void)ops;
    const int close_status = fs_object_close_native_files(object);
    if (close_status != 0) {
        return close_status;
    }
    const int dentry_owns_inode = object->dentry != NULL &&
        read_pointer_field(object->dentry, KOBOXD_DENTRY_INODE_OFFSET) == object->inode;
    kb_fs_subsystem_dput(object->dentry);
    if (!dentry_owns_inode) {
        kb_fs_subsystem_iput(object->inode);
    }
    object->dentry = NULL;
    object->inode = NULL;
    fs_mark_metadata_dirty(backend);
    return 0;
}

static void fs_finalize_unlinked_object_release(koboxd_fs_backend_t *backend, koboxd_fs_object_t *object)
{
    if (backend == NULL || object == NULL) {
        return;
    }
    fs_object_unregister(backend, object->object_id);
    if (backend->deferred_unlinked_count != 0) {
        backend->deferred_unlinked_count--;
    }
    KOBOXD_FS_TRACE("[koboxd-fs-trace] release_now done status=0 inode=%llu\n",
        (unsigned long long)object->inode_number);
}

static int fs_prepare_deferred_unlinked_objects(
    koboxd_fs_backend_t *backend,
    const koboxd_ext4_operations_t *ops,
    koboxd_deferred_release_batch_t *batch)
{
    if (backend == NULL || ops == NULL || batch == NULL || !backend->mounted) {
        return -22;
    }
    memset(batch, 0, sizeof(*batch));
    for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS; i++) {
        koboxd_fs_object_t *object = &backend->objects[i];
        if (!object->used || object->linked || object->references != 0) {
            continue;
        }
        if (!object->release_prepared) {
            const int status = fs_prepare_unlinked_object_release(backend, ops, object);
            if (status != 0) {
                return status;
            }
            object->release_prepared = 1;
        }
        batch->objects[batch->count++] = object;
    }
    return 0;
}

static void fs_finalize_deferred_release_batch(
    koboxd_fs_backend_t *backend,
    const koboxd_deferred_release_batch_t *batch)
{
    if (backend == NULL || batch == NULL) {
        return;
    }
    for (size_t i = 0; i < batch->count; i++) {
        fs_finalize_unlinked_object_release(backend, batch->objects[i]);
    }
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
    koboxd_deferred_release_batch_t batch;
    int status = fs_prepare_deferred_unlinked_objects(backend, &ops, &batch);
    if (status == 0 && backend->mount_result.super_block != NULL) {
        status = fs_sync_filesystem(backend->mount_result.super_block);
    }
    if (status != 0) {
        return status;
    }
    fs_finalize_deferred_release_batch(backend, &batch);
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
    if (object->references == 0) {
        return -22;
    }
    --object->references;
    object->last_used = fs_object_next_clock(backend);
    if (object->references != 0) {
        return 0;
    }
    const int close_status = fs_object_close_native_files(object);
    if (close_status != 0) {
        return close_status;
    }
    if (object->linked) {
        return 0;
    }
    fs_mark_metadata_dirty(backend);
    KOBOXD_FS_TRACE("[koboxd-fs-trace] release deferred object=%llu\n",
        (unsigned long long)object_id);
    return 0;
}

void koboxd_fs_backend_object_stats(
    const koboxd_fs_backend_t *backend,
    koboxd_fs_object_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->capacity = KOBOXD_FS_BACKEND_MAX_OBJECTS;
    if (backend == NULL) {
        return;
    }
    out_stats->evictions = backend->object_evictions;
    for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS; ++i) {
        const koboxd_fs_object_t *object = &backend->objects[i];
        if (!object->used) {
            continue;
        }
        ++out_stats->used;
        if (object->references != 0) {
            ++out_stats->referenced;
        } else if (object->linked) {
            ++out_stats->cached;
        }
    }
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
    return fs_file_read(
        &ops,
        object,
        backend->mount_result.root_vfsmount,
        offset,
        buffer,
        length,
        buffer_capacity);
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
    int status = fs_file_write(
        &ops,
        object,
        backend->mount_result.root_vfsmount,
        offset,
        buffer,
        length);
    const uint64_t write_end_ns = fs_now_ns();
    if (status < 0) {
        fprintf(stderr,
            "FILED_STORAGE_FAULT layer=ext4_file_write status=%d "
            "object=%llu inode=%llu offset=%llu length=%zu size=%llu dirty=%u\n",
            status,
            (unsigned long long)object_id,
            (unsigned long long)object->inode_number,
            (unsigned long long)offset,
            length,
            (unsigned long long)object->size,
            object->dirty);
    }
    if (status >= 0) {
        fs_mark_object_data_dirty(object);
        uint64_t write_end = 0;
        if (__builtin_add_overflow(
                offset,
                (uint64_t)(unsigned int)status,
                &write_end))
        {
            status = -75;
        } else if (write_end > object->size) {
            /* The ext4 inode remains authoritative.  Do not issue a full
             * ->getattr after every transport-sized pwrite: Linux pwrite
             * advances i_size in the write path and observes the remaining
             * metadata on stat/fsync.  Keep only the size mirror needed by
             * filed coherent here; fsync/statx performs the native refresh. */
            object->size = write_end;
        }
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
    return fs_flush_dirty_object(
        &ops,
        object,
        backend->mount_result.root_vfsmount);
}

int koboxd_fs_backend_sync_all(koboxd_fs_backend_t *backend)
{
    if (backend == NULL || !backend->mounted) {
        return -22;
    }
    uint64_t dirty_object_count = 0;
    for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS; i++) {
        const koboxd_fs_object_t *object = &backend->objects[i];
        if (object->used && object->dirty && object->inode != NULL) {
            dirty_object_count++;
        }
    }
    KOBOXD_FS_TRACE("[koboxd-fs-trace] sync_all begin metadata_dirty=%u dirty_objects=%llu\n",
        backend->metadata_dirty,
        (unsigned long long)dirty_object_count);
    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    KOBOXD_FS_TRACE("[koboxd-fs-trace] sync_all begin metadata_dirty=%u\n", backend->metadata_dirty);
    koboxd_deferred_release_batch_t release_batch;
    memset(&release_batch, 0, sizeof(release_batch));
    int transaction_status = 0;
    const uint64_t drain_start_ns = fs_now_ns();
    if (transaction_status == 0) {
        transaction_status = fs_prepare_deferred_unlinked_objects(backend, &ops, &release_batch);
    }
    const uint64_t drain_end_ns = fs_now_ns();
    uint64_t commit_start_ns = 0;
    uint64_t commit_end_ns = 0;
    if (transaction_status == 0 && backend->mount_result.super_block != NULL) {
        commit_start_ns = fs_now_ns();
        /*
         * Sync the mounted filesystem once through the Linux VFS path.  The
         * old loop called ext4 fsync once per cached object, forcing the same
         * journal transaction hundreds of times when the bounded object
         * cache filled.  Kobox already tracks every native inode/address
         * space, so sync_filesystem provides the normal writeback -> sync_fs
         * -> buffer/block flush ordering without a filed-side substitute.
         */
        transaction_status = fs_sync_filesystem(backend->mount_result.super_block);
        commit_end_ns = fs_now_ns();
    }
    if (transaction_status != 0) {
        return transaction_status;
    }
    fs_finalize_deferred_release_batch(backend, &release_batch);
    for (size_t i = 0; i < KOBOXD_FS_BACKEND_MAX_OBJECTS; i++) {
        koboxd_fs_object_t *object = &backend->objects[i];
        if (object->used && object->inode != NULL) {
            object->dirty = 0;
            const int refresh_status = fs_object_refresh(object);
            if (refresh_status != 0) {
                return refresh_status;
            }
        }
    }
    backend->metadata_dirty = 0;
    KOBOXD_FS_TRACE("[koboxd-fs-trace] sync_all done dirty_objects=%llu status=0\n",
        (unsigned long long)dirty_object_count);
    KOBOXD_FS_STAGE("[koboxd-fs-stage] op=sync_all dirty_objects=%llu drain_us=%llu commit_us=%llu status=0\n",
        (unsigned long long)dirty_object_count,
        (unsigned long long)fs_elapsed_us(drain_start_ns, drain_end_ns),
        (unsigned long long)fs_elapsed_us(commit_start_ns, commit_end_ns));
    KOBOXD_FS_TRACE("[koboxd-fs-trace] sync_all done status=0\n");
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
    const int refresh_status = fs_object_refresh(object);
    if (refresh_status != 0) {
        return refresh_status;
    }
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

    koboxd_ext4_operations_t ops;
    if (!load_ext4_operation_tables(backend->ext4_module, &ops)) {
        return -5;
    }
    koboxd_readdir_scan_t scan;
    int scan_status = fs_read_ext4_dir_native(
        &ops,
        dir,
        backend->mount_result.root_vfsmount,
        offset,
        capacity,
        &scan);
    if (scan_status != 0) {
        return scan_status;
    }
    for (size_t i = 0; i < scan.count; i++) {
        const int status = fs_scanned_dirent_to_object(
            backend,
            &ops,
            dir,
            &scan.entries[i],
            &out_entries[*out_count]);
        if (status != 0) {
            return status;
        }
        *out_count += 1;
    }
    return 0;
}
