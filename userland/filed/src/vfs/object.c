#include "private.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

void filed_lock_init(filed_lock_t *lock)
{
    if (lock != NULL) {
        atomic_flag_clear_explicit(&lock->flag, memory_order_release);
    }
}

void filed_lock_acquire(filed_lock_t *lock)
{
    if (lock == NULL) {
        return;
    }
    while (atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire)) {
    }
}

void filed_lock_release(filed_lock_t *lock)
{
    if (lock != NULL) {
        atomic_flag_clear_explicit(&lock->flag, memory_order_release);
    }
}

filed_lock_t *filed_mutable_lock(const filed_lock_t *lock)
{
    return (filed_lock_t *)(uintptr_t)lock;
}

void filed_vnode_write_lock(filed_vnode_t *vnode)
{
    if (vnode != NULL) {
        filed_lock_acquire(&vnode->lock);
    }
}

void filed_vnode_write_unlock(filed_vnode_t *vnode)
{
    if (vnode != NULL) {
        filed_lock_release(&vnode->lock);
    }
}

bool filed_vnode_lock_before(const filed_vnode_t *a, const filed_vnode_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    if (a->mount_id != b->mount_id) {
        return a->mount_id < b->mount_id;
    }
    return a->id < b->id;
}

void filed_vnode_write_lock_pair(filed_vnode_t *a, filed_vnode_t *b)
{
    if (a == NULL || b == NULL) {
        filed_vnode_write_lock(a != NULL ? a : b);
        return;
    }
    if (a == b) {
        filed_vnode_write_lock(a);
        return;
    }
    if (filed_vnode_lock_before(b, a)) {
        filed_vnode_write_lock(b);
        filed_vnode_write_lock(a);
    } else {
        filed_vnode_write_lock(a);
        filed_vnode_write_lock(b);
    }
}

void filed_vnode_write_unlock_pair(filed_vnode_t *a, filed_vnode_t *b)
{
    if (a == NULL || b == NULL) {
        filed_vnode_write_unlock(a != NULL ? a : b);
        return;
    }
    if (a == b) {
        filed_vnode_write_unlock(a);
        return;
    }
    if (filed_vnode_lock_before(b, a)) {
        filed_vnode_write_unlock(a);
        filed_vnode_write_unlock(b);
    } else {
        filed_vnode_write_unlock(b);
        filed_vnode_write_unlock(a);
    }
}

filed_status_t filed_copy_name(char *dst, size_t dst_size, const char *src)
{
    size_t len;

    if (dst == NULL || src == NULL || dst_size == 0) {
        return FILED_ERR_INVALID;
    }

    len = strlen(src);
    if (len >= dst_size) {
        return FILED_ERR_INVALID;
    }

    memcpy(dst, src, len + 1);
    return FILED_OK;
}

bool filed_name_is_dot_or_dotdot(const char *name)
{
    return name != NULL &&
        (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
}

bool filed_name_is_component(const char *name)
{
    return name != NULL &&
        name[0] != '\0' &&
        !filed_name_is_dot_or_dotdot(name) &&
        strchr(name, '/') == NULL;
}

const char *filed_status_name(filed_status_t status)
{
    switch (status) {
    case FILED_OK:
        return "OK";
    case FILED_ERR_NOT_FOUND:
        return "NOT_FOUND";
    case FILED_ERR_NOT_DIR:
        return "NOT_DIR";
    case FILED_ERR_IS_DIR:
        return "IS_DIR";
    case FILED_ERR_EXISTS:
        return "EXISTS";
    case FILED_ERR_DENIED:
        return "DENIED";
    case FILED_ERR_INVALID:
        return "INVALID";
    case FILED_ERR_CROSS_MOUNT:
        return "CROSS_MOUNT";
    case FILED_ERR_NOT_EMPTY:
        return "NOT_EMPTY";
    case FILED_ERR_IO:
        return "IO";
    case FILED_ERR_UNSUPPORTED:
        return "UNSUPPORTED";
    case FILED_ERR_BAD_FORMAT:
        return "BAD_FORMAT";
    case FILED_ERR_INVALID_IMAGE:
        return "INVALID_IMAGE";
    case FILED_ERR_LOOP:
        return "LOOP";
    case FILED_ERR_OVERFLOW:
        return "OVERFLOW";
    case FILED_ERR_FULL:
        return "FULL";
    }
    return "UNKNOWN";
}

void filed_vfs_init(filed_vfs_t *vfs)
{
    if (vfs == NULL) {
        return;
    }

    memset(vfs, 0, sizeof(*vfs));
    vfs->next_mount_id = 1;
    vfs->next_vnode_id = 1;
    vfs->next_file_id = 1;
    vfs->next_handle_id = 1;
}

bool filed_rights_include(uint32_t available, uint32_t requested)
{
    return (available & requested) == requested;
}

uint32_t filed_fd_flags_from_open(uint32_t open_flags)
{
    uint32_t flags = 0;

    if ((open_flags & FILED_OPEN_CLOEXEC) != 0) {
        flags |= FILED_FD_CLOEXEC;
    }

    return flags;
}

uint32_t filed_file_status_flags_from_open(uint32_t open_flags)
{
    uint32_t flags = 0;

    if ((open_flags & FILED_OPEN_APPEND) != 0) {
        flags |= FILED_FILE_APPEND;
    }
    if ((open_flags & FILED_OPEN_NONBLOCK) != 0) {
        flags |= FILED_FILE_NONBLOCK;
    }
    if ((open_flags & FILED_OPEN_SYNC) != 0) {
        flags |= FILED_FILE_SYNC;
    }

    return flags;
}

bool filed_fd_flags_are_known(uint32_t flags)
{
    return (flags & ~((uint32_t)FILED_FD_CLOEXEC)) == 0;
}

bool filed_file_status_flags_are_known(uint32_t flags)
{
    const uint32_t known =
        FILED_FILE_APPEND |
        FILED_FILE_NONBLOCK |
        FILED_FILE_SYNC;
    return (flags & ~known) == 0;
}

void filed_vnode_init_lock(filed_vnode_t *vnode)
{
    if (vnode != NULL) {
        filed_lock_init(&vnode->lock);
    }
}

filed_status_t filed_vnode_ref_inc(filed_vnode_t *vnode)
{
    filed_status_t status = FILED_OK;

    if (vnode == NULL || !vnode->active) {
        return FILED_ERR_INVALID;
    }
    filed_lock_acquire(&vnode->lock);
    if (vnode->refcount == UINT32_MAX) {
        status = FILED_ERR_OVERFLOW;
    } else {
        ++vnode->refcount;
    }
    filed_lock_release(&vnode->lock);
    return status;
}

uint32_t filed_vnode_ref_dec_if_nonzero(filed_vnode_t *vnode)
{
    uint32_t refcount = 0;

    if (vnode == NULL || !vnode->active) {
        return 0;
    }
    filed_lock_acquire(&vnode->lock);
    if (vnode->refcount > 0) {
        --vnode->refcount;
    }
    refcount = vnode->refcount;
    filed_lock_release(&vnode->lock);
    return refcount;
}

bool filed_vnode_mark_unlinked(filed_vnode_t *vnode)
{
    bool changed = false;

    if (vnode == NULL || !vnode->active) {
        return false;
    }
    filed_lock_acquire(&vnode->lock);
    if (vnode->linked) {
        vnode->linked = false;
        ++vnode->generation;
        ++vnode->object_generation;
        changed = true;
    }
    filed_lock_release(&vnode->lock);
    return changed;
}

void filed_vnode_bump_object_generation_locked(filed_vnode_t *vnode)
{
    if (vnode == NULL || !vnode->active) {
        return;
    }
    ++vnode->generation;
    ++vnode->object_generation;
}

void filed_vnode_bump_dir_generation_locked(filed_vnode_t *vnode)
{
    if (vnode == NULL || !vnode->active) {
        return;
    }
    ++vnode->generation;
    ++vnode->dir_generation;
}

void filed_file_init_locks(filed_file_t *file)
{
    if (file != NULL) {
        filed_lock_init(&file->lock);
        filed_lock_init(&file->offset_lock);
    }
}

filed_status_t filed_file_ref_inc(filed_file_t *file)
{
    filed_status_t status = FILED_OK;

    if (file == NULL || !file->active) {
        return FILED_ERR_INVALID;
    }
    filed_lock_acquire(&file->lock);
    if (file->refcount == UINT32_MAX) {
        status = FILED_ERR_OVERFLOW;
    } else {
        ++file->refcount;
    }
    filed_lock_release(&file->lock);
    return status;
}

uint32_t filed_file_ref_dec_if_nonzero(filed_file_t *file)
{
    uint32_t refcount = 0;

    if (file == NULL || !file->active) {
        return 0;
    }
    filed_lock_acquire(&file->lock);
    if (file->refcount > 0) {
        --file->refcount;
    }
    refcount = file->refcount;
    filed_lock_release(&file->lock);
    return refcount;
}

filed_status_t filed_file_offset_snapshot(
    const filed_file_t *file,
    uint64_t *out_offset)
{
    if (file == NULL || out_offset == NULL) {
        return FILED_ERR_INVALID;
    }

    filed_lock_acquire(filed_mutable_lock(&file->offset_lock));
    if (file->offset < 0) {
        filed_lock_release(filed_mutable_lock(&file->offset_lock));
        return FILED_ERR_INVALID;
    }
    *out_offset = (uint64_t)file->offset;
    filed_lock_release(filed_mutable_lock(&file->offset_lock));
    return FILED_OK;
}

filed_status_t filed_file_offset_advance(
    filed_file_t *file,
    uint64_t amount)
{
    uint64_t old_offset;

    if (file == NULL) {
        return FILED_ERR_INVALID;
    }

    filed_lock_acquire(&file->offset_lock);
    if (file->offset < 0) {
        filed_lock_release(&file->offset_lock);
        return FILED_ERR_INVALID;
    }
    old_offset = (uint64_t)file->offset;
    if (amount > (uint64_t)INT64_MAX - old_offset) {
        filed_lock_release(&file->offset_lock);
        return FILED_ERR_OVERFLOW;
    }
    file->offset = (int64_t)(old_offset + amount);
    filed_lock_release(&file->offset_lock);
    return FILED_OK;
}

uint32_t filed_file_status_flags_snapshot(const filed_file_t *file)
{
    uint32_t flags = 0;

    if (file == NULL) {
        return 0;
    }
    filed_lock_acquire(filed_mutable_lock(&file->lock));
    flags = file->status_flags;
    filed_lock_release(filed_mutable_lock(&file->lock));
    return flags;
}

filed_mount_t *filed_alloc_mount(filed_vfs_t *vfs)
{
    size_t i;

    for (i = 0; i < FILED_MAX_MOUNTS; ++i) {
        if (!vfs->mounts[i].active) {
            return &vfs->mounts[i];
        }
    }

    return NULL;
}

filed_vnode_t *filed_alloc_vnode(filed_vfs_t *vfs)
{
    const uint32_t start = vfs->next_vnode_slot;
    for (uint32_t offset = 0; offset < vfs->vnode_capacity; ++offset) {
        const uint32_t slot = (uint32_t)(((uint64_t)start + offset) % vfs->vnode_capacity);
        if (!filed_vfs_vnode_at(vfs, slot)->active) {
            vfs->next_vnode_slot = (slot + 1u) % vfs->vnode_capacity;
            return filed_vfs_vnode_at(vfs, slot);
        }
    }

    /* Only the 32-bit internal slot/ID representation bounds this store.
     * Allocate the body first so directory failure leaves all old nodes
     * and hints untouched. Locks/pointers into existing banks stay valid. */
    if (vfs->vnode_capacity > UINT32_MAX - FILED_VNODE_BANK_ENTRIES)
        return NULL;
    const size_t banks = vfs->vnode_capacity / FILED_VNODE_BANK_ENTRIES;
    if (banks + 1 > SIZE_MAX / sizeof(*vfs->vnode_banks)) return NULL;
    filed_vnode_t *bank = calloc(FILED_VNODE_BANK_ENTRIES, sizeof(*bank));
    if (bank == NULL) return NULL;
    filed_vnode_t **directory = realloc(vfs->vnode_banks,
        (banks + 1) * sizeof(*directory));
    if (directory == NULL) {
        free(bank);
        return NULL;
    }
    directory[banks] = bank;
    vfs->vnode_banks = directory;
    vfs->next_vnode_slot = vfs->vnode_capacity + 1;
    vfs->vnode_capacity += FILED_VNODE_BANK_ENTRIES;
    return bank;
}

uint32_t filed_vnode_slot_index(const filed_vfs_t *vfs, const filed_vnode_t *vnode)
{
    const uintptr_t address = (uintptr_t)vnode;
    for (uint32_t first = 0; first < vfs->vnode_capacity; first += FILED_VNODE_BANK_ENTRIES) {
        const uintptr_t base = (uintptr_t)filed_vfs_vnode_at(vfs, first);
        if (address >= base && address - base < FILED_VNODE_BANK_ENTRIES * sizeof(*vnode) &&
            (address - base) % sizeof(*vnode) == 0)
            return first + (uint32_t)((address - base) / sizeof(*vnode));
    }
    return UINT32_MAX;
}

void filed_vfs_trim_vnodes(filed_vfs_t *vfs)
{
    if (vfs == NULL) return;
    uint32_t banks = vfs->vnode_capacity / FILED_VNODE_BANK_ENTRIES;
    const uint32_t old_banks = banks;
    for (uint32_t b = 0; b < banks;) {
        bool active = false;
        for (unsigned i = 0; i < FILED_VNODE_BANK_ENTRIES; ++i)
            active |= vfs->vnode_banks[b][i].active;
        if (active) { ++b; continue; }
        free(vfs->vnode_banks[b]);
        vfs->vnode_banks[b] = vfs->vnode_banks[--banks];
    }
    if (banks == old_banks) return;
    vfs->vnode_capacity = banks * FILED_VNODE_BANK_ENTRIES;
    vfs->next_vnode_slot = 0;
    /* Bank directory positions changed, not vnode addresses or IDs. */
    memset(vfs->vnode_slot_hints, 0, sizeof(vfs->vnode_slot_hints));
    memset(vfs->child_slot_hints, 0, sizeof(vfs->child_slot_hints));
    if (banks == 0) {
        free(vfs->vnode_banks);
        vfs->vnode_banks = NULL;
    } else {
        filed_vnode_t **directory = realloc(vfs->vnode_banks,
            (size_t)banks * sizeof(*directory));
        if (directory != NULL) vfs->vnode_banks = directory;
    }
}

void filed_vfs_trim_open_objects(filed_vfs_t *vfs)
{
    if (vfs == NULL) return;
    /* Only trailing empty banks can disappear without renumbering lease links.
     * Keep the first bank warm; the syncer calls this between operations. */
    {
        uint32_t banks = vfs->file_capacity / FILED_FILE_BANK_ENTRIES;
        const uint32_t old_banks = banks;
        while (banks > 1) {
            bool active = false;
            for (unsigned i = 0; i < FILED_FILE_BANK_ENTRIES; ++i)
                active |= vfs->file_banks[banks - 1][i].active;
            if (active) break;
            free(vfs->file_banks[--banks]);
        }
        if (banks != old_banks) {
            vfs->file_capacity = banks * FILED_FILE_BANK_ENTRIES;
            vfs->next_file_slot = 0;
            memset(vfs->file_slot_hints, 0, sizeof(vfs->file_slot_hints));
            filed_file_t **directory = realloc(vfs->file_banks, (size_t)banks * sizeof(*directory));
            if (directory != NULL) vfs->file_banks = directory;
        }
    }
    {
        uint32_t banks = vfs->handle_capacity / FILED_HANDLE_BANK_ENTRIES;
        const uint32_t old_banks = banks;
        while (banks > 1) {
            bool active = false;
            for (unsigned i = 0; i < FILED_HANDLE_BANK_ENTRIES; ++i)
                active |= vfs->handle_banks[banks - 1][i].active;
            if (active) break;
            free(vfs->handle_banks[--banks]);
        }
        if (banks != old_banks) {
            vfs->handle_capacity = banks * FILED_HANDLE_BANK_ENTRIES;
            vfs->next_handle_slot = 0;
            memset(vfs->handle_slot_hints, 0, sizeof(vfs->handle_slot_hints));
            filed_handle_t **directory = realloc(vfs->handle_banks, (size_t)banks * sizeof(*directory));
            if (directory != NULL) vfs->handle_banks = directory;
        }
    }
}

void filed_vfs_destroy(filed_vfs_t *vfs)
{
    if (vfs == NULL) return;
    for (uint32_t b = 0; b < vfs->vnode_capacity / FILED_VNODE_BANK_ENTRIES; ++b)
        free(vfs->vnode_banks[b]);
    free(vfs->vnode_banks);
    for (uint32_t b = 0; b < vfs->file_capacity / FILED_FILE_BANK_ENTRIES; ++b)
        free(vfs->file_banks[b]);
    free(vfs->file_banks);
    for (uint32_t b = 0; b < vfs->handle_capacity / FILED_HANDLE_BANK_ENTRIES; ++b)
        free(vfs->handle_banks[b]);
    free(vfs->handle_banks);
    memset(vfs, 0, sizeof(*vfs));
}

filed_file_t *filed_alloc_file(filed_vfs_t *vfs)
{
    const uint32_t start = vfs->next_file_slot;
    for (uint32_t offset = 0; offset < vfs->file_capacity; ++offset) {
        const uint32_t slot = (uint32_t)(((uint64_t)start + offset) % vfs->file_capacity);
        if (!filed_vfs_file_at(vfs, slot)->active) {
            vfs->next_file_slot = (slot + 1u) % vfs->file_capacity;
            return filed_vfs_file_at(vfs, slot);
        }
    }

    /* Only the 32-bit internal slot/ID representation bounds this store.
     * Allocate the body first so directory failure leaves all old nodes
     * and hints untouched. Locks/pointers into existing banks stay valid. */
    if (vfs->file_capacity > UINT32_MAX - FILED_FILE_BANK_ENTRIES)
        return NULL;
    const size_t banks = vfs->file_capacity / FILED_FILE_BANK_ENTRIES;
    if (banks + 1 > SIZE_MAX / sizeof(*vfs->file_banks)) return NULL;
    filed_file_t *bank = calloc(FILED_FILE_BANK_ENTRIES, sizeof(*bank));
    if (bank == NULL) return NULL;
    filed_file_t **directory = realloc(vfs->file_banks,
        (banks + 1) * sizeof(*directory));
    if (directory == NULL) {
        free(bank);
        return NULL;
    }
    directory[banks] = bank;
    vfs->file_banks = directory;
    vfs->next_file_slot = vfs->file_capacity + 1;
    vfs->file_capacity += FILED_FILE_BANK_ENTRIES;
    return bank;
}

uint32_t filed_file_slot_index(const filed_vfs_t *vfs, const filed_file_t *file)
{
    const uintptr_t address = (uintptr_t)file;
    for (uint32_t first = 0; first < vfs->file_capacity; first += FILED_FILE_BANK_ENTRIES) {
        const uintptr_t base = (uintptr_t)filed_vfs_file_at(vfs, first);
        if (address >= base && address - base < FILED_FILE_BANK_ENTRIES * sizeof(*file) &&
            (address - base) % sizeof(*file) == 0)
            return first + (uint32_t)((address - base) / sizeof(*file));
    }
    return UINT32_MAX;
}

filed_handle_t *filed_alloc_handle(filed_vfs_t *vfs)
{
    const uint32_t start = vfs->next_handle_slot;
    for (uint32_t offset = 0; offset < vfs->handle_capacity; ++offset) {
        const uint32_t slot = (uint32_t)(((uint64_t)start + offset) % vfs->handle_capacity);
        if (!filed_vfs_handle_at(vfs, slot)->active) {
            vfs->next_handle_slot = (slot + 1u) % vfs->handle_capacity;
            return filed_vfs_handle_at(vfs, slot);
        }
    }

    /* Only the 32-bit internal slot/ID representation bounds this store.
     * Allocate the body first so directory failure leaves all old nodes
     * and hints untouched. Locks/pointers into existing banks stay valid. */
    if (vfs->handle_capacity > UINT32_MAX - FILED_HANDLE_BANK_ENTRIES)
        return NULL;
    const size_t banks = vfs->handle_capacity / FILED_HANDLE_BANK_ENTRIES;
    if (banks + 1 > SIZE_MAX / sizeof(*vfs->handle_banks)) return NULL;
    filed_handle_t *bank = calloc(FILED_HANDLE_BANK_ENTRIES, sizeof(*bank));
    if (bank == NULL) return NULL;
    filed_handle_t **directory = realloc(vfs->handle_banks,
        (banks + 1) * sizeof(*directory));
    if (directory == NULL) {
        free(bank);
        return NULL;
    }
    directory[banks] = bank;
    vfs->handle_banks = directory;
    vfs->next_handle_slot = vfs->handle_capacity + 1;
    vfs->handle_capacity += FILED_HANDLE_BANK_ENTRIES;
    return bank;
}

uint32_t filed_handle_slot_index(const filed_vfs_t *vfs, const filed_handle_t *handle)
{
    const uintptr_t address = (uintptr_t)handle;
    for (uint32_t first = 0; first < vfs->handle_capacity; first += FILED_HANDLE_BANK_ENTRIES) {
        const uintptr_t base = (uintptr_t)filed_vfs_handle_at(vfs, first);
        if (address >= base && address - base < FILED_HANDLE_BANK_ENTRIES * sizeof(*handle) &&
            (address - base) % sizeof(*handle) == 0)
            return first + (uint32_t)((address - base) / sizeof(*handle));
    }
    return UINT32_MAX;
}

size_t filed_id_hint_index(uint32_t id)
{
    return ((uint32_t)(id * 2654435761u)) & (FILED_ID_HINT_SLOTS - 1u);
}

void filed_remember_vnode_slot(filed_vfs_t *vfs, const filed_vnode_t *vnode)
{
    if (vfs == NULL || vnode == NULL || vnode->id == 0) {
        return;
    }
    const uint32_t slot = filed_vnode_slot_index(vfs, vnode);
    if (slot >= vfs->vnode_capacity) {
        return;
    }
    vfs->vnode_slot_hints[filed_id_hint_index(vnode->id)] = slot + 1;
}

void filed_remember_file_slot(filed_vfs_t *vfs, const filed_file_t *file)
{
    if (vfs == NULL || file == NULL || file->id == 0) {
        return;
    }
    const uint32_t slot = filed_file_slot_index(vfs, file);
    if (slot >= vfs->file_capacity) {
        return;
    }
    vfs->file_slot_hints[filed_id_hint_index(file->id)] = slot + 1;
}

void filed_remember_handle_slot(filed_vfs_t *vfs, const filed_handle_t *handle)
{
    if (vfs == NULL || handle == NULL || handle->id == 0) {
        return;
    }
    const uint32_t slot = filed_handle_slot_index(vfs, handle);
    if (slot >= vfs->handle_capacity) {
        return;
    }
    vfs->handle_slot_hints[filed_id_hint_index(handle->id)] = slot + 1;
}

bool filed_mount_id_exists(const filed_vfs_t *vfs, filed_mount_id_t id)
{
    size_t i;

    for (i = 0; i < FILED_MAX_MOUNTS; ++i) {
        if (vfs->mounts[i].active && vfs->mounts[i].id == id) {
            return true;
        }
    }

    return false;
}

bool filed_vnode_id_exists(const filed_vfs_t *vfs, filed_vnode_id_t id)
{
    size_t i;

    for (i = 0; i < vfs->vnode_capacity; ++i) {
        if (filed_vfs_vnode_at(vfs, i)->active && filed_vfs_vnode_at(vfs, i)->id == id) {
            return true;
        }
    }

    return false;
}

filed_mount_t *filed_find_mount(filed_vfs_t *vfs, filed_mount_id_t id)
{
    size_t i;

    for (i = 0; i < FILED_MAX_MOUNTS; ++i) {
        if (vfs->mounts[i].active && vfs->mounts[i].id == id) {
            return &vfs->mounts[i];
        }
    }

    return NULL;
}

filed_vnode_t *filed_find_vnode(filed_vfs_t *vfs, filed_vnode_id_t id)
{
    size_t i;
    uint32_t hinted_slot;

    if (vfs == NULL || id == 0) {
        return NULL;
    }

    hinted_slot = vfs->vnode_slot_hints[filed_id_hint_index(id)];
    if (hinted_slot != 0 && hinted_slot <= vfs->vnode_capacity) {
        filed_vnode_t *candidate = filed_vfs_vnode_at(vfs, hinted_slot - 1u);
        if (candidate->active && candidate->id == id) {
            return candidate;
        }
    }

    for (i = 0; i < vfs->vnode_capacity; ++i) {
        if (filed_vfs_vnode_at(vfs, i)->active && filed_vfs_vnode_at(vfs, i)->id == id) {
            filed_remember_vnode_slot(vfs, filed_vfs_vnode_at(vfs, i));
            return filed_vfs_vnode_at(vfs, i);
        }
    }

    return NULL;
}

const filed_vnode_t *filed_find_vnode_const(const filed_vfs_t *vfs, filed_vnode_id_t id)
{
    return filed_find_vnode((filed_vfs_t *)(uintptr_t)vfs, id);
}

filed_file_t *filed_find_file(filed_vfs_t *vfs, filed_file_id_t id)
{
    size_t i;
    uint32_t hinted_slot;

    if (vfs == NULL || id == 0) {
        return NULL;
    }

    hinted_slot = vfs->file_slot_hints[filed_id_hint_index(id)];
    if (hinted_slot != 0 && hinted_slot <= vfs->file_capacity) {
        filed_file_t *candidate = filed_vfs_file_at(vfs, hinted_slot - 1u);
        if (candidate->active && candidate->id == id) {
            return candidate;
        }
    }

    for (i = 0; i < vfs->file_capacity; ++i) {
        if (filed_vfs_file_at(vfs, i)->active && filed_vfs_file_at(vfs, i)->id == id) {
            filed_remember_file_slot(vfs, filed_vfs_file_at(vfs, i));
            return filed_vfs_file_at(vfs, i);
        }
    }

    return NULL;
}

const filed_file_t *filed_find_file_const(const filed_vfs_t *vfs, filed_file_id_t id)
{
    return filed_find_file((filed_vfs_t *)(uintptr_t)vfs, id);
}

filed_handle_t *filed_find_handle(filed_vfs_t *vfs, filed_handle_id_t id)
{
    size_t i;
    uint32_t hinted_slot;

    if (vfs == NULL || id == 0) {
        return NULL;
    }

    hinted_slot = vfs->handle_slot_hints[filed_id_hint_index(id)];
    if (hinted_slot != 0 && hinted_slot <= vfs->handle_capacity) {
        filed_handle_t *candidate = filed_vfs_handle_at(vfs, hinted_slot - 1u);
        if (candidate->active && candidate->id == id) {
            return candidate;
        }
    }

    for (i = 0; i < vfs->handle_capacity; ++i) {
        if (filed_vfs_handle_at(vfs, i)->active && filed_vfs_handle_at(vfs, i)->id == id) {
            filed_remember_handle_slot(vfs, filed_vfs_handle_at(vfs, i));
            return filed_vfs_handle_at(vfs, i);
        }
    }

    return NULL;
}

const filed_handle_t *filed_find_handle_const(const filed_vfs_t *vfs, filed_handle_id_t id)
{
    return filed_find_handle((filed_vfs_t *)(uintptr_t)vfs, id);
}

filed_vnode_t *filed_find_backend_vnode(
    filed_vfs_t *vfs,
    filed_mount_id_t mount_id,
    filed_backend_object_id_t backend_object,
    filed_vnode_id_t parent,
    const char *name)
{
    size_t i;

    if (vfs == NULL || backend_object == 0 || parent == 0 || name == NULL) {
        return NULL;
    }
    for (i = 0; i < vfs->vnode_capacity; ++i) {
        if (filed_vfs_vnode_at(vfs, i)->active &&
            filed_vfs_vnode_at(vfs, i)->linked &&
            filed_vfs_vnode_at(vfs, i)->mount_id == mount_id &&
            filed_vfs_vnode_at(vfs, i)->backend_object == backend_object &&
            filed_vfs_vnode_at(vfs, i)->parent == parent &&
            strcmp(filed_vfs_vnode_at(vfs, i)->name, name) == 0)
        {
            return filed_vfs_vnode_at(vfs, i);
        }
    }

    return NULL;
}

filed_vnode_t *filed_find_backend_object_vnode(
    filed_vfs_t *vfs,
    filed_backend_object_id_t backend_object)
{
    if (vfs == NULL || backend_object == 0) {
        return NULL;
    }
    for (uint32_t i = 0; i < vfs->vnode_capacity; ++i) {
        if (filed_vfs_vnode_at(vfs, i)->active &&
            filed_vfs_vnode_at(vfs, i)->backend_object == backend_object)
        {
            return filed_vfs_vnode_at(vfs, i);
        }
    }
    return NULL;
}

uint32_t filed_vnode_mount_pins(const filed_vfs_t *vfs, filed_vnode_id_t vnode_id)
{
    uint32_t pins = 0;
    size_t i;

    if (vfs == NULL || vnode_id == 0) {
        return 0;
    }
    for (i = 0; i < FILED_MAX_MOUNTS; ++i) {
        if (vfs->mounts[i].active && vfs->mounts[i].root_vnode == vnode_id) {
            ++pins;
        }
    }
    return pins;
}

bool filed_vnode_is_dead(const filed_vfs_t *vfs, const filed_vnode_t *vnode)
{
    if (vfs == NULL || vnode == NULL || !vnode->active) {
        return false;
    }
    if (vnode->linked || vnode->refcount != 0 || filed_vnode_mount_pins(vfs, vnode->id) != 0) {
        return false;
    }
    return true;
}
