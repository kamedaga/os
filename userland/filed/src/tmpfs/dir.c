#include "private.h"

static uint32_t filed_tmpfs_child_hash(uint16_t parent_inode_slot, const char *name)
{
    uint32_t hash = 2166136261u ^ parent_inode_slot;
    if (name != NULL) {
        for (size_t i = 0; name[i] != '\0'; ++i) {
            hash ^= (uint8_t)name[i];
            hash *= 16777619u;
        }
    }
    return hash % FILED_TMPFS_CHILD_HASH_BUCKETS;
}

filed_tmpfs_dentry_t *filed_tmpfs_dentry_by_slot(filed_tmpfs_backend_t *backend, uint16_t slot)
{
    if (backend == NULL || slot >= FILED_TMPFS_MAX_DENTRIES) {
        return NULL;
    }
    filed_tmpfs_dentry_t *dentry = &backend->dentries[slot];
    return dentry->used ? dentry : NULL;
}

filed_tmpfs_dentry_t *filed_tmpfs_alloc_dentry(filed_tmpfs_backend_t *backend)
{
    if (backend == NULL || backend->free_dentry_count == 0) {
        return NULL;
    }
    const uint16_t slot = backend->free_dentry_stack[--backend->free_dentry_count];
    if (slot == 0 || slot >= FILED_TMPFS_MAX_DENTRIES || backend->dentries[slot].used) {
        return NULL;
    }
    return &backend->dentries[slot];
}

static void filed_tmpfs_free_dentry(filed_tmpfs_backend_t *backend, filed_tmpfs_dentry_t *dentry)
{
    if (backend == NULL || dentry == NULL) {
        return;
    }
    const uint16_t slot = dentry->slot_index;
    memset(dentry, 0, sizeof(*dentry));
    if (slot != 0 && slot < FILED_TMPFS_MAX_DENTRIES && backend->free_dentry_count < FILED_TMPFS_MAX_DENTRIES) {
        backend->free_dentry_stack[backend->free_dentry_count++] = slot;
    }
}

filed_tmpfs_dentry_t *filed_tmpfs_find_child_dentry(
    filed_tmpfs_backend_t *backend,
    filed_tmpfs_inode_t *parent,
    const char *name)
{
    if (backend == NULL || parent == NULL || parent->kind != FILED_VNODE_DIRECTORY || name == NULL) {
        return NULL;
    }
    const uint16_t parent_slot = filed_tmpfs_inode_slot(backend, parent);
    if (parent_slot == FILED_TMPFS_NO_SLOT) {
        return NULL;
    }
    uint16_t slot = backend->child_hash_buckets[filed_tmpfs_child_hash(parent_slot, name)];
    while (slot != FILED_TMPFS_NO_SLOT) {
        filed_tmpfs_dentry_t *dentry = filed_tmpfs_dentry_by_slot(backend, slot);
        if (dentry == NULL) {
            return NULL;
        }
        if (dentry->linked &&
            dentry->parent_inode_slot == parent_slot &&
            strcmp(dentry->name, name) == 0)
        {
            return dentry;
        }
        slot = dentry->hash_next_slot;
    }
    return NULL;
}

filed_tmpfs_inode_t *filed_tmpfs_find_child_inode(
    filed_tmpfs_backend_t *backend,
    filed_tmpfs_inode_t *parent,
    const char *name)
{
    filed_tmpfs_dentry_t *dentry = filed_tmpfs_find_child_dentry(backend, parent, name);
    return dentry != NULL ? filed_tmpfs_inode_by_slot(backend, dentry->inode_slot) : NULL;
}

void filed_tmpfs_link_dentry_locked(
    filed_tmpfs_backend_t *backend,
    filed_tmpfs_inode_t *parent,
    filed_tmpfs_inode_t *inode,
    filed_tmpfs_dentry_t *dentry,
    const char *name)
{
    if (backend == NULL || parent == NULL || inode == NULL || dentry == NULL || name == NULL) {
        return;
    }
    const uint16_t parent_slot = filed_tmpfs_inode_slot(backend, parent);
    const uint16_t inode_slot = filed_tmpfs_inode_slot(backend, inode);
    if (parent_slot == FILED_TMPFS_NO_SLOT || inode_slot == FILED_TMPFS_NO_SLOT) {
        return;
    }
    memset(dentry, 0, sizeof(*dentry));
    dentry->used = true;
    dentry->linked = true;
    dentry->slot_index = (uint16_t)(dentry - &backend->dentries[0]);
    dentry->parent_inode_slot = parent_slot;
    dentry->inode_slot = inode_slot;
    dentry->next_sibling_slot = parent->first_child_dentry_slot;
    parent->first_child_dentry_slot = dentry->slot_index;
    snprintf(dentry->name, sizeof(dentry->name), "%s", name);

    const uint32_t bucket = filed_tmpfs_child_hash(parent_slot, dentry->name);
    dentry->hash_next_slot = backend->child_hash_buckets[bucket];
    backend->child_hash_buckets[bucket] = dentry->slot_index;

    if (inode->primary_dentry_slot == FILED_TMPFS_NO_SLOT) {
        inode->primary_dentry_slot = dentry->slot_index;
    }
    ++inode->nlink;
}

void filed_tmpfs_unlink_dentry_locked(
    filed_tmpfs_backend_t *backend,
    filed_tmpfs_inode_t *parent,
    filed_tmpfs_dentry_t *dentry)
{
    if (backend == NULL || parent == NULL || dentry == NULL || !dentry->linked) {
        return;
    }

    uint16_t *sibling_link = &parent->first_child_dentry_slot;
    while (*sibling_link != FILED_TMPFS_NO_SLOT) {
        filed_tmpfs_dentry_t *candidate = filed_tmpfs_dentry_by_slot(backend, *sibling_link);
        if (candidate == NULL) {
            *sibling_link = FILED_TMPFS_NO_SLOT;
            break;
        }
        if (candidate == dentry) {
            *sibling_link = dentry->next_sibling_slot;
            break;
        }
        sibling_link = &candidate->next_sibling_slot;
    }

    uint16_t *hash_link = &backend->child_hash_buckets[filed_tmpfs_child_hash(dentry->parent_inode_slot, dentry->name)];
    while (*hash_link != FILED_TMPFS_NO_SLOT) {
        filed_tmpfs_dentry_t *candidate = filed_tmpfs_dentry_by_slot(backend, *hash_link);
        if (candidate == NULL) {
            *hash_link = FILED_TMPFS_NO_SLOT;
            break;
        }
        if (candidate == dentry) {
            *hash_link = dentry->hash_next_slot;
            break;
        }
        hash_link = &candidate->hash_next_slot;
    }

    filed_tmpfs_inode_t *inode = filed_tmpfs_inode_by_slot(backend, dentry->inode_slot);
    if (inode != NULL && inode->nlink > 0) {
        --inode->nlink;
        if (inode->primary_dentry_slot == dentry->slot_index) {
            inode->primary_dentry_slot = FILED_TMPFS_NO_SLOT;
        }
    }
    filed_tmpfs_free_dentry(backend, dentry);
}

static void filed_tmpfs_detach_dentry_locked(
    filed_tmpfs_backend_t *backend,
    filed_tmpfs_inode_t *parent,
    filed_tmpfs_dentry_t *dentry)
{
    if (backend == NULL || parent == NULL || dentry == NULL || !dentry->linked) {
        return;
    }

    uint16_t *sibling_link = &parent->first_child_dentry_slot;
    while (*sibling_link != FILED_TMPFS_NO_SLOT) {
        filed_tmpfs_dentry_t *candidate = filed_tmpfs_dentry_by_slot(backend, *sibling_link);
        if (candidate == NULL) {
            *sibling_link = FILED_TMPFS_NO_SLOT;
            break;
        }
        if (candidate == dentry) {
            *sibling_link = dentry->next_sibling_slot;
            break;
        }
        sibling_link = &candidate->next_sibling_slot;
    }

    uint16_t *hash_link = &backend->child_hash_buckets[filed_tmpfs_child_hash(dentry->parent_inode_slot, dentry->name)];
    while (*hash_link != FILED_TMPFS_NO_SLOT) {
        filed_tmpfs_dentry_t *candidate = filed_tmpfs_dentry_by_slot(backend, *hash_link);
        if (candidate == NULL) {
            *hash_link = FILED_TMPFS_NO_SLOT;
            break;
        }
        if (candidate == dentry) {
            *hash_link = dentry->hash_next_slot;
            break;
        }
        hash_link = &candidate->hash_next_slot;
    }

    dentry->next_sibling_slot = FILED_TMPFS_NO_SLOT;
    dentry->hash_next_slot = FILED_TMPFS_NO_SLOT;
}

static void filed_tmpfs_attach_existing_dentry_locked(
    filed_tmpfs_backend_t *backend,
    filed_tmpfs_inode_t *parent,
    filed_tmpfs_dentry_t *dentry,
    const char *name)
{
    if (backend == NULL || parent == NULL || dentry == NULL || name == NULL) {
        return;
    }
    const uint16_t parent_slot = filed_tmpfs_inode_slot(backend, parent);
    if (parent_slot == FILED_TMPFS_NO_SLOT) {
        return;
    }
    dentry->parent_inode_slot = parent_slot;
    dentry->next_sibling_slot = parent->first_child_dentry_slot;
    parent->first_child_dentry_slot = dentry->slot_index;
    snprintf(dentry->name, sizeof(dentry->name), "%s", name);

    const uint32_t bucket = filed_tmpfs_child_hash(parent_slot, dentry->name);
    dentry->hash_next_slot = backend->child_hash_buckets[bucket];
    backend->child_hash_buckets[bucket] = dentry->slot_index;
}

int filed_tmpfs_directory_empty_locked(const filed_tmpfs_backend_t *backend, uint64_t object_id)
{
    filed_tmpfs_inode_t *dir = filed_tmpfs_find_inode((filed_tmpfs_backend_t *)backend, object_id);
    if (dir == NULL || dir->kind != FILED_VNODE_DIRECTORY) {
        return 0;
    }
    return dir->first_child_dentry_slot == FILED_TMPFS_NO_SLOT;
}

uint64_t filed_tmpfs_dir_nlink_locked(const filed_tmpfs_backend_t *backend, uint64_t object_id)
{
    uint64_t nlink = 2;
    filed_tmpfs_inode_t *dir = filed_tmpfs_find_inode((filed_tmpfs_backend_t *)backend, object_id);
    if (dir == NULL || dir->kind != FILED_VNODE_DIRECTORY) {
        return 0;
    }
    uint16_t slot = dir->first_child_dentry_slot;
    while (slot != FILED_TMPFS_NO_SLOT) {
        filed_tmpfs_dentry_t *dentry = filed_tmpfs_dentry_by_slot((filed_tmpfs_backend_t *)backend, slot);
        if (dentry == NULL) {
            break;
        }
        filed_tmpfs_inode_t *inode = filed_tmpfs_inode_by_slot((filed_tmpfs_backend_t *)backend, dentry->inode_slot);
        if (dentry->linked && inode != NULL && inode->kind == FILED_VNODE_DIRECTORY) {
            ++nlink;
        }
        slot = dentry->next_sibling_slot;
    }
    return nlink;
}

int filed_tmpfs_backend_lookup(filed_tmpfs_backend_t *backend, uint64_t parent_object_id, const char *name, uint64_t *out_object_id)
{
    if (backend == NULL || name == NULL || out_object_id == NULL) {
        return -22;
    }
    *out_object_id = 0;
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_inode_t *parent = filed_tmpfs_find_inode(backend, parent_object_id);
    if (parent == NULL || parent->nlink == 0) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (parent->kind != FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -20;
    }
    if (strcmp(name, ".") == 0) {
        *out_object_id = parent->object_id;
        filed_tmpfs_lock_release(&backend->lock);
        return 0;
    }
    if (strcmp(name, "..") == 0) {
        filed_tmpfs_dentry_t *primary = filed_tmpfs_dentry_by_slot(backend, parent->primary_dentry_slot);
        filed_tmpfs_inode_t *parent_dir = primary != NULL ?
            filed_tmpfs_inode_by_slot(backend, primary->parent_inode_slot) :
            parent;
        *out_object_id = parent_dir != NULL ? parent_dir->object_id : parent->object_id;
        filed_tmpfs_lock_release(&backend->lock);
        return 0;
    }
    filed_tmpfs_inode_t *child = filed_tmpfs_find_child_inode(backend, parent, name);
    if (child == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    *out_object_id = child->object_id;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

static int filed_tmpfs_create_kind(
    filed_tmpfs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    uint64_t mode,
    filed_vnode_kind_t kind,
    uint64_t *out_object_id)
{
    if (backend == NULL || out_object_id == NULL || !filed_tmpfs_name_valid(name)) {
        return -22;
    }
    *out_object_id = 0;
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_inode_t *parent = filed_tmpfs_find_inode(backend, parent_object_id);
    if (parent == NULL || parent->nlink == 0) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (parent->kind != FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -20;
    }
    if (filed_tmpfs_find_child_dentry(backend, parent, name) != NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -17;
    }
    if (backend->free_inode_count == 0 || backend->free_dentry_count == 0) {
        filed_tmpfs_lock_release(&backend->lock);
        return -28;
    }
    filed_tmpfs_inode_t *inode = filed_tmpfs_alloc_inode(backend);
    filed_tmpfs_dentry_t *dentry = filed_tmpfs_alloc_dentry(backend);
    if (inode == NULL || dentry == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -28;
    }
    memset(inode, 0, sizeof(*inode));
    inode->slot_index = filed_tmpfs_inode_slot(backend, inode);
    if (inode->slot_index == FILED_TMPFS_NO_SLOT) {
        filed_tmpfs_lock_release(&backend->lock);
        return -5;
    }
    inode->used = true;
    inode->primary_dentry_slot = FILED_TMPFS_NO_SLOT;
    inode->first_child_dentry_slot = FILED_TMPFS_NO_SLOT;
    inode->object_id = filed_tmpfs_make_object_id(inode->slot_index, backend->next_object_generation++);
    if (inode->object_id == 0) {
        memset(inode, 0, sizeof(*inode));
        filed_tmpfs_lock_release(&backend->lock);
        return -5;
    }
    inode->mode = filed_tmpfs_mode_for_kind(kind, mode);
    inode->generation = 1;
    inode->kind = kind;
    filed_tmpfs_link_dentry_locked(backend, parent, inode, dentry, name);
    ++parent->generation;
    *out_object_id = inode->object_id;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_create(filed_tmpfs_backend_t *backend, uint64_t parent_object_id, const char *name, uint64_t mode, uint64_t *out_object_id)
{
    return filed_tmpfs_create_kind(backend, parent_object_id, name, mode, FILED_VNODE_REGULAR, out_object_id);
}

int filed_tmpfs_backend_mkdir(filed_tmpfs_backend_t *backend, uint64_t parent_object_id, const char *name, uint64_t mode, uint64_t *out_object_id)
{
    return filed_tmpfs_create_kind(backend, parent_object_id, name, mode, FILED_VNODE_DIRECTORY, out_object_id);
}

int filed_tmpfs_backend_link(
    filed_tmpfs_backend_t *backend,
    uint64_t old_object_id,
    uint64_t new_parent_object_id,
    const char *new_name,
    uint64_t *out_object_id)
{
    if (backend == NULL || !filed_tmpfs_name_valid(new_name) || out_object_id == NULL) {
        return -22;
    }
    *out_object_id = 0;
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_inode_t *inode = filed_tmpfs_find_inode(backend, old_object_id);
    filed_tmpfs_inode_t *new_parent = filed_tmpfs_find_inode(backend, new_parent_object_id);
    if (inode == NULL || new_parent == NULL || inode->nlink == 0 || new_parent->nlink == 0) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (new_parent->kind != FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -20;
    }
    if (inode->kind == FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -1;
    }
    if (filed_tmpfs_find_child_dentry(backend, new_parent, new_name) != NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -17;
    }
    filed_tmpfs_dentry_t *dentry = filed_tmpfs_alloc_dentry(backend);
    if (dentry == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -28;
    }
    filed_tmpfs_link_dentry_locked(backend, new_parent, inode, dentry, new_name);
    ++inode->generation;
    ++new_parent->generation;
    *out_object_id = inode->object_id;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_symlink(
    filed_tmpfs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name,
    const char *target,
    uint64_t target_length,
    uint64_t *out_object_id)
{
    uint64_t object_id = 0;
    uint64_t written = 0;
    if (target == NULL || target_length == 0 || target_length > FILED_TMPFS_MAX_FILE_BYTES) {
        return -22;
    }
    int status = filed_tmpfs_create_kind(
        backend,
        parent_object_id,
        name,
        FILED_TMPFS_MODE_SYMLINK | 0777u,
        FILED_VNODE_SYMLINK,
        &object_id);
    if (status != 0) {
        return status;
    }
    status = filed_tmpfs_backend_pwrite(backend, object_id, 0, target, target_length, &written);
    if (status != 0 || written != target_length) {
        (void)filed_tmpfs_backend_unlink(backend, parent_object_id, name);
        return status != 0 ? status : -5;
    }
    if (out_object_id != NULL) {
        *out_object_id = object_id;
    }
    return 0;
}

int filed_tmpfs_backend_unlink(filed_tmpfs_backend_t *backend, uint64_t parent_object_id, const char *name)
{
    if (backend == NULL || !filed_tmpfs_name_valid(name)) {
        return -22;
    }
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_inode_t *parent = filed_tmpfs_find_inode(backend, parent_object_id);
    filed_tmpfs_dentry_t *dentry = parent != NULL ? filed_tmpfs_find_child_dentry(backend, parent, name) : NULL;
    filed_tmpfs_inode_t *inode = dentry != NULL ? filed_tmpfs_inode_by_slot(backend, dentry->inode_slot) : NULL;
    if (parent == NULL || dentry == NULL || inode == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (parent->kind != FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -20;
    }
    if (inode->kind == FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -21;
    }
    filed_tmpfs_unlink_dentry_locked(backend, parent, dentry);
    ++inode->generation;
    ++parent->generation;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_rmdir(filed_tmpfs_backend_t *backend, uint64_t parent_object_id, const char *name)
{
    if (backend == NULL || !filed_tmpfs_name_valid(name)) {
        return -22;
    }
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_inode_t *parent = filed_tmpfs_find_inode(backend, parent_object_id);
    filed_tmpfs_dentry_t *dentry = parent != NULL ? filed_tmpfs_find_child_dentry(backend, parent, name) : NULL;
    filed_tmpfs_inode_t *inode = dentry != NULL ? filed_tmpfs_inode_by_slot(backend, dentry->inode_slot) : NULL;
    if (parent == NULL || dentry == NULL || inode == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (parent->kind != FILED_VNODE_DIRECTORY || inode->kind != FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -20;
    }
    if (!filed_tmpfs_directory_empty_locked(backend, inode->object_id)) {
        filed_tmpfs_lock_release(&backend->lock);
        return -39;
    }
    filed_tmpfs_unlink_dentry_locked(backend, parent, dentry);
    ++inode->generation;
    ++parent->generation;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

static int filed_tmpfs_inode_is_ancestor_locked(
    filed_tmpfs_backend_t *backend,
    uint64_t maybe_ancestor,
    uint64_t child)
{
    while (child != 0) {
        if (child == maybe_ancestor) {
            return 1;
        }
        filed_tmpfs_inode_t *inode = filed_tmpfs_find_inode(backend, child);
        if (inode == NULL || inode->primary_dentry_slot == FILED_TMPFS_NO_SLOT) {
            return 0;
        }
        filed_tmpfs_dentry_t *dentry = filed_tmpfs_dentry_by_slot(backend, inode->primary_dentry_slot);
        if (dentry == NULL || dentry->parent_inode_slot == inode->slot_index) {
            return 0;
        }
        filed_tmpfs_inode_t *parent = filed_tmpfs_inode_by_slot(backend, dentry->parent_inode_slot);
        child = parent != NULL ? parent->object_id : 0;
    }
    return 0;
}

int filed_tmpfs_backend_rename(
    filed_tmpfs_backend_t *backend,
    uint64_t old_parent_object_id,
    const char *old_name,
    uint64_t new_parent_object_id,
    const char *new_name,
    uint64_t *out_object_id)
{
    if (backend == NULL || out_object_id == NULL || !filed_tmpfs_name_valid(old_name) || !filed_tmpfs_name_valid(new_name)) {
        return -22;
    }
    *out_object_id = 0;
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_inode_t *old_parent = filed_tmpfs_find_inode(backend, old_parent_object_id);
    filed_tmpfs_inode_t *new_parent = filed_tmpfs_find_inode(backend, new_parent_object_id);
    filed_tmpfs_dentry_t *source_dentry = old_parent != NULL ? filed_tmpfs_find_child_dentry(backend, old_parent, old_name) : NULL;
    filed_tmpfs_dentry_t *target_dentry = new_parent != NULL ? filed_tmpfs_find_child_dentry(backend, new_parent, new_name) : NULL;
    filed_tmpfs_inode_t *source = source_dentry != NULL ? filed_tmpfs_inode_by_slot(backend, source_dentry->inode_slot) : NULL;
    filed_tmpfs_inode_t *target = target_dentry != NULL ? filed_tmpfs_inode_by_slot(backend, target_dentry->inode_slot) : NULL;
    if (old_parent == NULL || new_parent == NULL || source == NULL || source_dentry == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (old_parent->kind != FILED_VNODE_DIRECTORY || new_parent->kind != FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -20;
    }
    if (source->kind == FILED_VNODE_DIRECTORY &&
        filed_tmpfs_inode_is_ancestor_locked(backend, source->object_id, new_parent->object_id))
    {
        filed_tmpfs_lock_release(&backend->lock);
        return -22;
    }
    if (target != NULL && target->object_id == source->object_id) {
        *out_object_id = source->object_id;
        filed_tmpfs_lock_release(&backend->lock);
        return 0;
    }
    if (target != NULL) {
        if (source->kind == FILED_VNODE_DIRECTORY && target->kind != FILED_VNODE_DIRECTORY) {
            filed_tmpfs_lock_release(&backend->lock);
            return -20;
        }
        if (source->kind != FILED_VNODE_DIRECTORY && target->kind == FILED_VNODE_DIRECTORY) {
            filed_tmpfs_lock_release(&backend->lock);
            return -21;
        }
        if (target->kind == FILED_VNODE_DIRECTORY && !filed_tmpfs_directory_empty_locked(backend, target->object_id)) {
            filed_tmpfs_lock_release(&backend->lock);
            return -39;
        }
        filed_tmpfs_unlink_dentry_locked(backend, new_parent, target_dentry);
        ++target->generation;
    }

    filed_tmpfs_detach_dentry_locked(backend, old_parent, source_dentry);
    filed_tmpfs_attach_existing_dentry_locked(backend, new_parent, source_dentry, new_name);
    ++source->generation;
    ++old_parent->generation;
    if (new_parent->object_id != old_parent->object_id) {
        ++new_parent->generation;
    }
    *out_object_id = source->object_id;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_getdents(
    filed_tmpfs_backend_t *backend,
    uint64_t dir_object_id,
    uint64_t offset,
    storage_v2_getdents_request_t *out_entries)
{
    if (backend == NULL || out_entries == NULL) {
        return -22;
    }
    memset(out_entries, 0, sizeof(*out_entries));
    out_entries->dir_object_id = dir_object_id;
    out_entries->offset = offset;
    out_entries->capacity = STORAGE_V2_DIRENT_CAPACITY;

    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_inode_t *dir = filed_tmpfs_find_inode(backend, dir_object_id);
    if (dir == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (dir->kind != FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -20;
    }
    uint64_t skipped = 0;
    uint16_t slot = dir->first_child_dentry_slot;
    while (slot != FILED_TMPFS_NO_SLOT && out_entries->count < STORAGE_V2_DIRENT_CAPACITY) {
        filed_tmpfs_dentry_t *dentry = filed_tmpfs_dentry_by_slot(backend, slot);
        if (dentry == NULL) {
            break;
        }
        slot = dentry->next_sibling_slot;
        if (!dentry->linked) {
            continue;
        }
        filed_tmpfs_inode_t *inode = filed_tmpfs_inode_by_slot(backend, dentry->inode_slot);
        if (inode == NULL) {
            continue;
        }
        if (skipped < offset) {
            ++skipped;
            continue;
        }
        storage_v2_dirent_t *entry = &out_entries->entries[out_entries->count++];
        entry->object_id = inode->object_id;
        entry->kind = inode->mode & FILED_TMPFS_MODE_TYPE_MASK;
        entry->name_len = strlen(dentry->name);
        snprintf(entry->name, sizeof(entry->name), "%s", dentry->name);
    }
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}
