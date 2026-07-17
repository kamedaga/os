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
    for (uint32_t offset = 0; offset < FILED_MAX_VNODES; ++offset) {
        const uint32_t slot = (start + offset) % FILED_MAX_VNODES;
        if (!vfs->vnodes[slot].active) {
            vfs->next_vnode_slot =
                (uint16_t)((slot + 1u) % FILED_MAX_VNODES);
            return &vfs->vnodes[slot];
        }
    }

    return NULL;
}

filed_file_t *filed_alloc_file(filed_vfs_t *vfs)
{
    const uint32_t start = vfs->next_file_slot;
    for (uint32_t offset = 0; offset < FILED_MAX_FILES; ++offset) {
        const uint32_t slot = (start + offset) % FILED_MAX_FILES;
        if (!vfs->files[slot].active) {
            vfs->next_file_slot =
                (uint16_t)((slot + 1u) % FILED_MAX_FILES);
            return &vfs->files[slot];
        }
    }

    return NULL;
}

filed_handle_t *filed_alloc_handle(filed_vfs_t *vfs)
{
    const uint32_t start = vfs->next_handle_slot;
    for (uint32_t offset = 0; offset < FILED_MAX_HANDLES; ++offset) {
        const uint32_t slot = (start + offset) % FILED_MAX_HANDLES;
        if (!vfs->handles[slot].active) {
            vfs->next_handle_slot =
                (uint16_t)((slot + 1u) % FILED_MAX_HANDLES);
            return &vfs->handles[slot];
        }
    }

    return NULL;
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
    const ptrdiff_t slot = vnode - vfs->vnodes;
    if (slot < 0 || slot >= (ptrdiff_t)FILED_MAX_VNODES) {
        return;
    }
    vfs->vnode_slot_hints[filed_id_hint_index(vnode->id)] = (uint16_t)(slot + 1);
}

void filed_remember_file_slot(filed_vfs_t *vfs, const filed_file_t *file)
{
    if (vfs == NULL || file == NULL || file->id == 0) {
        return;
    }
    const ptrdiff_t slot = file - vfs->files;
    if (slot < 0 || slot >= (ptrdiff_t)FILED_MAX_FILES) {
        return;
    }
    vfs->file_slot_hints[filed_id_hint_index(file->id)] = (uint16_t)(slot + 1);
}

void filed_remember_handle_slot(filed_vfs_t *vfs, const filed_handle_t *handle)
{
    if (vfs == NULL || handle == NULL || handle->id == 0) {
        return;
    }
    const ptrdiff_t slot = handle - vfs->handles;
    if (slot < 0 || slot >= (ptrdiff_t)FILED_MAX_HANDLES) {
        return;
    }
    vfs->handle_slot_hints[filed_id_hint_index(handle->id)] = (uint16_t)(slot + 1);
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

    for (i = 0; i < FILED_MAX_VNODES; ++i) {
        if (vfs->vnodes[i].active && vfs->vnodes[i].id == id) {
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
    uint16_t hinted_slot;

    if (vfs == NULL || id == 0) {
        return NULL;
    }

    hinted_slot = vfs->vnode_slot_hints[filed_id_hint_index(id)];
    if (hinted_slot != 0 && hinted_slot <= FILED_MAX_VNODES) {
        filed_vnode_t *candidate = &vfs->vnodes[hinted_slot - 1u];
        if (candidate->active && candidate->id == id) {
            return candidate;
        }
    }

    for (i = 0; i < FILED_MAX_VNODES; ++i) {
        if (vfs->vnodes[i].active && vfs->vnodes[i].id == id) {
            filed_remember_vnode_slot(vfs, &vfs->vnodes[i]);
            return &vfs->vnodes[i];
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
    uint16_t hinted_slot;

    if (vfs == NULL || id == 0) {
        return NULL;
    }

    hinted_slot = vfs->file_slot_hints[filed_id_hint_index(id)];
    if (hinted_slot != 0 && hinted_slot <= FILED_MAX_FILES) {
        filed_file_t *candidate = &vfs->files[hinted_slot - 1u];
        if (candidate->active && candidate->id == id) {
            return candidate;
        }
    }

    for (i = 0; i < FILED_MAX_FILES; ++i) {
        if (vfs->files[i].active && vfs->files[i].id == id) {
            filed_remember_file_slot(vfs, &vfs->files[i]);
            return &vfs->files[i];
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
    uint16_t hinted_slot;

    if (vfs == NULL || id == 0) {
        return NULL;
    }

    hinted_slot = vfs->handle_slot_hints[filed_id_hint_index(id)];
    if (hinted_slot != 0 && hinted_slot <= FILED_MAX_HANDLES) {
        filed_handle_t *candidate = &vfs->handles[hinted_slot - 1u];
        if (candidate->active && candidate->id == id) {
            return candidate;
        }
    }

    for (i = 0; i < FILED_MAX_HANDLES; ++i) {
        if (vfs->handles[i].active && vfs->handles[i].id == id) {
            filed_remember_handle_slot(vfs, &vfs->handles[i]);
            return &vfs->handles[i];
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
    for (i = 0; i < FILED_MAX_VNODES; ++i) {
        if (vfs->vnodes[i].active &&
            vfs->vnodes[i].linked &&
            vfs->vnodes[i].mount_id == mount_id &&
            vfs->vnodes[i].backend_object == backend_object &&
            vfs->vnodes[i].parent == parent &&
            strcmp(vfs->vnodes[i].name, name) == 0)
        {
            return &vfs->vnodes[i];
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
    for (uint32_t i = 0; i < FILED_MAX_VNODES; ++i) {
        if (vfs->vnodes[i].active &&
            vfs->vnodes[i].backend_object == backend_object)
        {
            return &vfs->vnodes[i];
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
