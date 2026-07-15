#include "private.h"

static uint64_t filed_vfs_next_vnode_clock(filed_vfs_t *vfs)
{
    if (vfs->vnode_clock == UINT64_MAX) {
        uint64_t next = 1;
        for (uint32_t i = 0; i < FILED_MAX_VNODES; ++i) {
            filed_vnode_t *vnode = &vfs->vnodes[i];
            if (vnode->active) {
                vnode->last_used = next++;
            }
        }
        vfs->vnode_clock = next;
    }
    ++vfs->vnode_clock;
    if (vfs->vnode_clock == 0) {
        vfs->vnode_clock = 1;
    }
    return vfs->vnode_clock;
}

static void filed_reclaim_result_set(
    const filed_vfs_t *vfs,
    filed_vfs_reclaim_result_t *out_reclaim,
    const filed_vnode_t *vnode)
{
    if (out_reclaim == NULL || vnode == NULL || vnode->backend_object == 0) {
        return;
    }
    for (uint32_t i = 0; vfs != NULL && i < FILED_MAX_VNODES; ++i) {
        const filed_vnode_t *alias = &vfs->vnodes[i];
        if (alias != vnode &&
            alias->active &&
            alias->backend_object == vnode->backend_object &&
            (alias->linked || alias->refcount != 0 || filed_vnode_mount_pins(vfs, alias->id) != 0))
        {
            return;
        }
    }
    out_reclaim->released = true;
    out_reclaim->backend_object = vnode->backend_object;
}

static void filed_reclaim_vnode_if_dead_ex(
    filed_vfs_t *vfs,
    filed_vnode_t *vnode,
    filed_vfs_reclaim_result_t *out_reclaim)
{
    if (!filed_vnode_is_dead(vfs, vnode)) {
        return;
    }
    filed_reclaim_result_set(vfs, out_reclaim, vnode);
    memset(vnode, 0, sizeof(*vnode));
}

static filed_vnode_t *filed_find_child_vnode(
    filed_vfs_t *vfs,
    filed_vnode_id_t parent,
    const char *name)
{
    size_t i;

    if (vfs == NULL || parent == 0 || name == NULL) {
        return NULL;
    }
    for (i = 0; i < FILED_MAX_VNODES; ++i) {
        if (vfs->vnodes[i].active &&
            vfs->vnodes[i].linked &&
            vfs->vnodes[i].parent == parent &&
            strcmp(vfs->vnodes[i].name, name) == 0)
        {
            return &vfs->vnodes[i];
        }
    }
    return NULL;
}

static filed_status_t filed_handle_vnode(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint32_t required_rights,
    filed_vnode_t **out_vnode)
{
    const filed_handle_t *handle;
    const filed_file_t *file;
    filed_vnode_t *vnode;

    if (vfs == NULL || handle_id == 0 || out_vnode == NULL) {
        return FILED_ERR_INVALID;
    }
    *out_vnode = NULL;

    handle = filed_find_handle_const(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    if (!filed_rights_include(handle->rights, required_rights)) {
        return FILED_ERR_DENIED;
    }
    file = filed_find_file_const(vfs, (filed_file_id_t)handle->target_id);
    if (file == NULL) {
        return FILED_ERR_INVALID;
    }
    vnode = filed_find_vnode(vfs, file->vnode_id);
    if (vnode == NULL) {
        return FILED_ERR_INVALID;
    }

    *out_vnode = vnode;
    return FILED_OK;
}

static filed_status_t filed_open_vnode(
    filed_vfs_t *vfs,
    filed_vnode_t *vnode,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open)
{
    filed_file_t *file;
    filed_handle_t *handle;
    filed_file_id_t file_id;
    filed_handle_id_t handle_id;
    filed_status_t status;

    if (vfs == NULL || vnode == NULL || out_open == NULL || rights == 0) {
        return FILED_ERR_INVALID;
    }
    vnode->last_used = filed_vfs_next_vnode_clock(vfs);
    if ((open_flags & FILED_OPEN_DIRECTORY) != 0 &&
        vnode->kind != FILED_VNODE_DIRECTORY)
    {
        return FILED_ERR_NOT_DIR;
    }
    if (vnode->kind == FILED_VNODE_DIRECTORY &&
        (rights & FILED_RIGHT_WRITE) != 0)
    {
        return FILED_ERR_IS_DIR;
    }
    if (vfs->next_file_id == 0 || vfs->next_handle_id == 0) {
        return FILED_ERR_OVERFLOW;
    }

    file = filed_alloc_file(vfs);
    handle = filed_alloc_handle(vfs);
    if (file == NULL || handle == NULL) {
        return FILED_ERR_FULL;
    }

    status = filed_vnode_ref_inc(vnode);
    if (status != FILED_OK) {
        return status;
    }

    file_id = vfs->next_file_id;
    handle_id = vfs->next_handle_id;

    memset(file, 0, sizeof(*file));
    filed_file_init_locks(file);
    file->active = true;
    file->id = file_id;
    file->vnode_id = vnode->id;
    file->offset = 0;
    file->status_flags = filed_file_status_flags_from_open(open_flags);
    file->rights = rights;
    file->refcount = 1;

    memset(handle, 0, sizeof(*handle));
    handle->active = true;
    handle->id = handle_id;
    handle->target_kind = FILED_HANDLE_FILE;
    handle->target_id = file_id;
    handle->rights = rights;
    handle->fd_flags = filed_fd_flags_from_open(open_flags);
    handle->generation = 1;

    filed_remember_file_slot(vfs, file);
    filed_remember_handle_slot(vfs, handle);

    ++vfs->next_file_id;
    ++vfs->next_handle_id;

    memset(out_open, 0, sizeof(*out_open));
    out_open->handle_id = handle_id;
    out_open->vnode_id = vnode->id;
    out_open->backend_object = vnode->backend_object;
    out_open->kind = vnode->kind;
    filed_lock_acquire(&vnode->lock);
    out_open->object_generation = vnode->object_generation;
    out_open->dir_generation = vnode->dir_generation;
    filed_lock_release(&vnode->lock);
    return FILED_OK;
}

static filed_status_t filed_open_backend_child_at(
    filed_vfs_t *vfs,
    filed_vnode_t *parent_vnode,
    filed_backend_object_id_t child_backend_object,
    filed_vnode_kind_t child_kind,
    const char *name,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open)
{
    filed_vnode_t *child;
    filed_status_t status;

    if (vfs == NULL ||
        parent_vnode == NULL ||
        child_backend_object == 0 ||
        child_kind == 0 ||
        name == NULL ||
        out_open == NULL)
    {
        return FILED_ERR_INVALID;
    }

    child = filed_find_backend_vnode(
        vfs,
        parent_vnode->mount_id,
        child_backend_object,
        parent_vnode->id,
        name);
    if (child == NULL) {
        if (vfs->next_vnode_id == 0) {
            return FILED_ERR_OVERFLOW;
        }
        child = filed_alloc_vnode(vfs);
        if (child == NULL) {
            return FILED_ERR_FULL;
        }
        memset(child, 0, sizeof(*child));
        filed_vnode_init_lock(child);
        child->active = true;
        child->linked = true;
        child->id = vfs->next_vnode_id;
        child->mount_id = parent_vnode->mount_id;
        child->backend_object = child_backend_object;
        child->kind = child_kind;
        child->parent = parent_vnode->id;
        child->generation = 1;
        child->object_generation = 1;
        child->dir_generation = 1;
        child->last_used = filed_vfs_next_vnode_clock(vfs);
        child->refcount = 0;
        status = filed_copy_name(child->name, sizeof(child->name), name);
        if (status != FILED_OK) {
            memset(child, 0, sizeof(*child));
            return status;
        }
        filed_remember_vnode_slot(vfs, child);
        ++vfs->next_vnode_id;
    }

    return filed_open_vnode(vfs, child, rights, open_flags, out_open);
}

filed_status_t filed_vfs_mount_root(
    filed_vfs_t *vfs,
    filed_fs_kind_t fs_kind,
    filed_backend_id_t backend,
    filed_backend_object_id_t root_backend_object,
    filed_mount_id_t *out_mount_id)
{
    filed_mount_t *mount;
    filed_vnode_t *root;
    filed_mount_id_t mount_id;
    filed_vnode_id_t root_id;
    filed_status_t status;

    if (vfs == NULL || out_mount_id == NULL) {
        return FILED_ERR_INVALID;
    }
    if (vfs->next_mount_id == 0 || vfs->next_vnode_id == 0) {
        return FILED_ERR_OVERFLOW;
    }

    mount = filed_alloc_mount(vfs);
    root = filed_alloc_vnode(vfs);
    if (mount == NULL || root == NULL) {
        return FILED_ERR_FULL;
    }

    mount_id = vfs->next_mount_id;
    root_id = vfs->next_vnode_id;

    memset(mount, 0, sizeof(*mount));
    mount->active = true;
    mount->id = mount_id;
    mount->root_vnode = root_id;
    mount->backend = backend;
    mount->fs_kind = fs_kind;

    memset(root, 0, sizeof(*root));
    filed_vnode_init_lock(root);
    root->active = true;
    root->linked = true;
    root->id = root_id;
    root->mount_id = mount_id;
    root->backend_object = root_backend_object;
    root->kind = FILED_VNODE_DIRECTORY;
    root->parent = 0;
    root->generation = 1;
    root->object_generation = 1;
    root->dir_generation = 1;
    root->last_used = filed_vfs_next_vnode_clock(vfs);
    root->refcount = 1;

    status = filed_copy_name(root->name, sizeof(root->name), "/");
    if (status != FILED_OK) {
        memset(mount, 0, sizeof(*mount));
        memset(root, 0, sizeof(*root));
        return status;
    }

    filed_remember_vnode_slot(vfs, root);

    ++vfs->next_mount_id;
    ++vfs->next_vnode_id;
    *out_mount_id = mount_id;
    return FILED_OK;
}

filed_status_t filed_vfs_open_root(
    filed_vfs_t *vfs,
    filed_mount_id_t mount_id,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open)
{
    filed_mount_t *mount;
    filed_vnode_t *root;

    if (vfs == NULL || out_open == NULL) {
        return FILED_ERR_INVALID;
    }

    mount = filed_find_mount(vfs, mount_id);
    if (mount == NULL) {
        return FILED_ERR_INVALID;
    }
    root = filed_find_vnode(vfs, mount->root_vnode);
    if (root == NULL) {
        return FILED_ERR_INVALID;
    }

    return filed_open_vnode(vfs, root, rights, open_flags | FILED_OPEN_DIRECTORY, out_open);
}

filed_status_t filed_vfs_open_backend_child(
    filed_vfs_t *vfs,
    filed_handle_id_t parent_handle,
    filed_backend_object_id_t child_backend_object,
    filed_vnode_kind_t child_kind,
    const char *name,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open)
{
    const filed_handle_t *parent;
    const filed_file_t *parent_file;
    filed_vnode_t *parent_vnode;
    filed_status_t status;

    if (vfs == NULL ||
        child_backend_object == 0 ||
        child_kind == 0 ||
        name == NULL ||
        out_open == NULL)
    {
        return FILED_ERR_INVALID;
    }
    if (!filed_name_is_component(name)) {
        return FILED_ERR_INVALID;
    }

    parent = filed_find_handle_const(vfs, parent_handle);
    if (parent == NULL || parent->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    if (!filed_rights_include(parent->rights, FILED_RIGHT_LOOKUP)) {
        return FILED_ERR_DENIED;
    }
    parent_file = filed_find_file_const(vfs, (filed_file_id_t)parent->target_id);
    if (parent_file == NULL) {
        return FILED_ERR_INVALID;
    }
    parent_vnode = filed_find_vnode(vfs, parent_file->vnode_id);
    if (parent_vnode == NULL) {
        return FILED_ERR_INVALID;
    }
    if (parent_vnode->kind != FILED_VNODE_DIRECTORY) {
        return FILED_ERR_NOT_DIR;
    }

    filed_vnode_write_lock(parent_vnode);
    status = filed_open_backend_child_at(
        vfs,
        parent_vnode,
        child_backend_object,
        child_kind,
        name,
        rights,
        open_flags,
        out_open);
    filed_vnode_write_unlock(parent_vnode);
    return status;
}

filed_status_t filed_vfs_open_cached_child(
    filed_vfs_t *vfs,
    filed_handle_id_t parent_handle,
    const char *name,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open)
{
    const filed_handle_t *parent;
    const filed_file_t *parent_file;
    filed_vnode_t *parent_vnode;
    filed_vnode_t *child;
    filed_status_t status;

    if (vfs == NULL || name == NULL || out_open == NULL) {
        return FILED_ERR_INVALID;
    }
    if (!filed_name_is_component(name)) {
        return FILED_ERR_INVALID;
    }

    parent = filed_find_handle_const(vfs, parent_handle);
    if (parent == NULL || parent->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    if (!filed_rights_include(parent->rights, FILED_RIGHT_LOOKUP)) {
        return FILED_ERR_DENIED;
    }
    parent_file = filed_find_file_const(vfs, (filed_file_id_t)parent->target_id);
    if (parent_file == NULL) {
        return FILED_ERR_INVALID;
    }
    parent_vnode = filed_find_vnode(vfs, parent_file->vnode_id);
    if (parent_vnode == NULL) {
        return FILED_ERR_INVALID;
    }
    if (parent_vnode->kind != FILED_VNODE_DIRECTORY) {
        return FILED_ERR_NOT_DIR;
    }

    filed_vnode_write_lock(parent_vnode);
    child = filed_find_child_vnode(vfs, parent_vnode->id, name);
    if (child == NULL) {
        filed_vnode_write_unlock(parent_vnode);
        return FILED_ERR_NOT_FOUND;
    }
    status = filed_open_vnode(vfs, child, rights, open_flags, out_open);
    filed_vnode_write_unlock(parent_vnode);
    return status;
}

filed_status_t filed_vfs_cached_child_backend_object(
    const filed_vfs_t *vfs,
    filed_handle_id_t parent_handle,
    const char *name,
    filed_backend_object_id_t *out_backend_object)
{
    filed_vnode_t *parent_vnode;
    filed_vnode_t *child;
    filed_status_t status;

    if (vfs == NULL || parent_handle == 0 || name == NULL || out_backend_object == NULL) {
        return FILED_ERR_INVALID;
    }
    *out_backend_object = 0;
    if (!filed_name_is_component(name)) {
        return FILED_ERR_INVALID;
    }

    status = filed_handle_vnode(
        (filed_vfs_t *)(uintptr_t)vfs,
        parent_handle,
        FILED_RIGHT_LOOKUP,
        &parent_vnode);
    if (status != FILED_OK) {
        return status;
    }
    if (parent_vnode->kind != FILED_VNODE_DIRECTORY) {
        return FILED_ERR_NOT_DIR;
    }

    filed_vnode_write_lock(parent_vnode);
    child = filed_find_child_vnode((filed_vfs_t *)(uintptr_t)vfs, parent_vnode->id, name);
    if (child != NULL) {
        filed_lock_acquire(&child->lock);
        *out_backend_object = child->backend_object;
        filed_lock_release(&child->lock);
    }
    filed_vnode_write_unlock(parent_vnode);
    return child != NULL && *out_backend_object != 0 ? FILED_OK : FILED_ERR_NOT_FOUND;
}

filed_status_t filed_vfs_create_backend_child(
    filed_vfs_t *vfs,
    filed_handle_id_t parent_handle,
    filed_backend_object_id_t child_backend_object,
    filed_vnode_kind_t child_kind,
    const char *name,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open)
{
    const filed_handle_t *parent;
    const filed_file_t *parent_file;
    filed_vnode_t *parent_vnode;
    filed_status_t status;

    if (vfs == NULL || name == NULL || out_open == NULL) {
        return FILED_ERR_INVALID;
    }
    if (!filed_name_is_component(name)) {
        return FILED_ERR_INVALID;
    }
    parent = filed_find_handle_const(vfs, parent_handle);
    if (parent == NULL || parent->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    if (!filed_rights_include(parent->rights, FILED_RIGHT_CREATE)) {
        return FILED_ERR_DENIED;
    }
    parent_file = filed_find_file_const(vfs, (filed_file_id_t)parent->target_id);
    if (parent_file == NULL) {
        return FILED_ERR_INVALID;
    }
    parent_vnode = filed_find_vnode(vfs, parent_file->vnode_id);
    if (parent_vnode == NULL) {
        return FILED_ERR_INVALID;
    }
    if (parent_vnode->kind != FILED_VNODE_DIRECTORY) {
        return FILED_ERR_NOT_DIR;
    }
    filed_vnode_write_lock(parent_vnode);
    if (filed_find_child_vnode(vfs, parent_vnode->id, name) != NULL) {
        filed_vnode_write_unlock(parent_vnode);
        return FILED_ERR_EXISTS;
    }
    status = filed_open_backend_child_at(
        vfs,
        parent_vnode,
        child_backend_object,
        child_kind,
        name,
        rights,
        open_flags,
        out_open);
    if (status == FILED_OK) {
        filed_vnode_bump_dir_generation_locked(parent_vnode);
    }
    filed_vnode_write_unlock(parent_vnode);
    return status;
}

filed_status_t filed_vfs_open_existing(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open)
{
    filed_vnode_t *vnode;
    filed_status_t status;

    if (vfs == NULL || out_open == NULL) {
        return FILED_ERR_INVALID;
    }

    status = filed_handle_vnode(vfs, handle_id, 0, &vnode);
    if (status != FILED_OK) {
        return status;
    }
    return filed_open_vnode(vfs, vnode, rights, open_flags, out_open);
}

filed_status_t filed_vfs_open_parent(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open)
{
    filed_vnode_t *vnode;
    filed_vnode_t *parent;
    filed_status_t status;

    if (vfs == NULL || out_open == NULL) {
        return FILED_ERR_INVALID;
    }

    status = filed_handle_vnode(vfs, handle_id, FILED_RIGHT_LOOKUP, &vnode);
    if (status != FILED_OK) {
        return status;
    }
    if (vnode->kind != FILED_VNODE_DIRECTORY) {
        return FILED_ERR_NOT_DIR;
    }
    if (vnode->parent == 0) {
        parent = vnode;
    } else {
        parent = filed_find_vnode(vfs, vnode->parent);
        if (parent == NULL) {
            return FILED_ERR_INVALID;
        }
    }

    return filed_open_vnode(vfs, parent, rights, open_flags, out_open);
}

filed_status_t filed_vfs_close_handle_ex(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_reclaim_result_t *out_reclaim)
{
    filed_handle_t *handle;
    filed_file_t *file;
    filed_vnode_t *vnode;

    if (out_reclaim != NULL) {
        memset(out_reclaim, 0, sizeof(*out_reclaim));
    }
    if (vfs == NULL || handle_id == 0) {
        return FILED_ERR_INVALID;
    }
    handle = filed_find_handle(vfs, handle_id);
    if (handle == NULL) {
        return FILED_ERR_INVALID;
    }

    if (handle->target_kind == FILED_HANDLE_FILE) {
        file = filed_find_file(vfs, (filed_file_id_t)handle->target_id);
        if (file != NULL) {
            if (filed_file_ref_dec_if_nonzero(file) == 0) {
                vnode = filed_find_vnode(vfs, file->vnode_id);
                if (vnode != NULL) {
                    (void)filed_vnode_ref_dec_if_nonzero(vnode);
                    filed_reclaim_vnode_if_dead_ex(vfs, vnode, out_reclaim);
                }
                memset(file, 0, sizeof(*file));
            }
        }
    }

    memset(handle, 0, sizeof(*handle));
    return FILED_OK;
}

filed_status_t filed_vfs_close_handle(filed_vfs_t *vfs, filed_handle_id_t handle_id)
{
    return filed_vfs_close_handle_ex(vfs, handle_id, NULL);
}

filed_status_t filed_vfs_set_handle_owner(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint32_t owner_session)
{
    if (vfs == NULL || handle_id == 0 || owner_session == 0)
        return FILED_ERR_INVALID;
    filed_handle_t *handle = filed_find_handle(vfs, handle_id);
    if (handle == NULL) return FILED_ERR_INVALID;
    handle->owner_session = owner_session;
    return FILED_OK;
}

filed_status_t filed_vfs_set_handle_lease(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    int lease_fd)
{
    if (vfs == NULL || handle_id == 0 || lease_fd < 16)
        return FILED_ERR_INVALID;
    filed_handle_t *handle = filed_find_handle(vfs, handle_id);
    if (handle == NULL || handle->owner_session != 0 || handle->lease_fd >= 16)
        return FILED_ERR_INVALID;
    uint32_t lease_count = 0;
    for (uint32_t i = 0; i < FILED_MAX_HANDLES; ++i)
        lease_count += vfs->handles[i].active && vfs->handles[i].lease_fd >= 16;
    if (lease_count >= FILED_MAX_TRANSFER_LEASES) return FILED_ERR_FULL;
    handle->lease_fd = lease_fd;
    return FILED_OK;
}

int filed_vfs_get_handle_lease(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id)
{
    const filed_handle_t *handle = filed_find_handle_const(vfs, handle_id);
    return handle != NULL ? handle->lease_fd : -1;
}

static bool filed_vfs_backend_object_is_unused_linked_leaf(
    const filed_vfs_t *vfs,
    filed_backend_object_id_t backend_object,
    uint64_t *out_last_used)
{
    uint64_t last_used = 0;
    bool found = false;
    if (vfs == NULL || backend_object == 0) {
        return false;
    }
    for (uint32_t i = 0; i < FILED_MAX_VNODES; ++i) {
        const filed_vnode_t *vnode = &vfs->vnodes[i];
        if (!vnode->active || vnode->backend_object != backend_object) {
            continue;
        }
        found = true;
        if (!vnode->linked || vnode->refcount != 0 ||
            filed_vnode_mount_pins(vfs, vnode->id) != 0)
        {
            return false;
        }
        if (vnode->last_used > last_used) {
            last_used = vnode->last_used;
        }
        for (uint32_t j = 0; j < FILED_MAX_VNODES; ++j) {
            if (vfs->vnodes[j].active && vfs->vnodes[j].parent == vnode->id) {
                return false;
            }
        }
    }
    if (found && out_last_used != NULL) {
        *out_last_used = last_used;
    }
    return found;
}

filed_status_t filed_vfs_evict_lru_unused_linked(
    filed_vfs_t *vfs,
    uint32_t max_cached,
    filed_vfs_backend_evictable_fn evictable,
    void *context,
    filed_vfs_reclaim_result_t *out_reclaim)
{
    filed_backend_object_id_t oldest_object = 0;
    uint64_t oldest_last_used = UINT64_MAX;
    uint32_t cached = 0;
    if (vfs == NULL || out_reclaim == NULL) {
        return FILED_ERR_INVALID;
    }
    memset(out_reclaim, 0, sizeof(*out_reclaim));
    for (uint32_t i = 0; i < FILED_MAX_VNODES; ++i) {
        const filed_vnode_t *vnode = &vfs->vnodes[i];
        if (!vnode->active || vnode->backend_object == 0) {
            continue;
        }
        bool first_alias = true;
        for (uint32_t j = 0; j < i; ++j) {
            if (vfs->vnodes[j].active &&
                vfs->vnodes[j].backend_object == vnode->backend_object)
            {
                first_alias = false;
                break;
            }
        }
        if (!first_alias) {
            continue;
        }
        uint64_t last_used = 0;
        if (!filed_vfs_backend_object_is_unused_linked_leaf(
                vfs,
                vnode->backend_object,
                &last_used))
        {
            continue;
        }
        ++cached;
        if ((evictable == NULL || evictable(context, vnode->backend_object)) &&
            (oldest_object == 0 || last_used < oldest_last_used))
        {
            oldest_object = vnode->backend_object;
            oldest_last_used = last_used;
        }
    }
    if (cached <= max_cached || oldest_object == 0) {
        return FILED_OK;
    }
    filed_vnode_t *candidate = NULL;
    filed_vnode_t *parent = NULL;
    uint32_t aliases = 0;
    for (uint32_t i = 0; i < FILED_MAX_VNODES; ++i) {
        filed_vnode_t *vnode = &vfs->vnodes[i];
        if (vnode->active && vnode->backend_object == oldest_object) {
            candidate = vnode;
            ++aliases;
        }
    }
    if (candidate == NULL || aliases != 1) {
        return FILED_OK;
    }
    if (candidate->parent != 0) {
        parent = filed_find_vnode(vfs, candidate->parent);
    }
    filed_vnode_write_lock(parent);
    filed_vnode_write_lock(candidate);
    const bool still_evictable =
        candidate->active &&
        candidate->backend_object == oldest_object &&
        filed_vfs_backend_object_is_unused_linked_leaf(vfs, oldest_object, NULL) &&
        (evictable == NULL || evictable(context, oldest_object));
    if (!still_evictable) {
        filed_vnode_write_unlock(candidate);
        filed_vnode_write_unlock(parent);
        return FILED_OK;
    }
    memset(candidate, 0, sizeof(*candidate));
    filed_vnode_write_unlock(candidate);
    filed_vnode_write_unlock(parent);
    out_reclaim->released = true;
    out_reclaim->backend_object = oldest_object;
    return FILED_OK;
}

filed_status_t filed_vfs_dup_handle(
    filed_vfs_t *vfs,
    filed_handle_id_t source_handle_id,
    uint32_t fd_flags,
    filed_handle_id_t *out_handle_id)
{
    const filed_handle_t *source;
    filed_file_t *file;
    filed_handle_t *handle;
    filed_handle_id_t handle_id;
    filed_status_t status;

    if (vfs == NULL || source_handle_id == 0 || out_handle_id == NULL) {
        return FILED_ERR_INVALID;
    }
    *out_handle_id = 0;
    if (!filed_fd_flags_are_known(fd_flags)) {
        return FILED_ERR_INVALID;
    }

    source = filed_find_handle_const(vfs, source_handle_id);
    if (source == NULL || source->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    file = filed_find_file(vfs, (filed_file_id_t)source->target_id);
    if (file == NULL) {
        return FILED_ERR_INVALID;
    }
    if (vfs->next_handle_id == 0) {
        return FILED_ERR_OVERFLOW;
    }

    handle = filed_alloc_handle(vfs);
    if (handle == NULL) {
        return FILED_ERR_FULL;
    }

    handle_id = vfs->next_handle_id;
    memset(handle, 0, sizeof(*handle));
    handle->active = true;
    handle->id = handle_id;
    handle->target_kind = FILED_HANDLE_FILE;
    handle->target_id = source->target_id;
    handle->rights = source->rights;
    handle->fd_flags = fd_flags;
    handle->generation = 1;

    status = filed_file_ref_inc(file);
    if (status != FILED_OK) {
        memset(handle, 0, sizeof(*handle));
        return status;
    }
    filed_remember_handle_slot(vfs, handle);
    ++vfs->next_handle_id;
    *out_handle_id = handle_id;
    return FILED_OK;
}

filed_status_t filed_vfs_dup_handle_for_exec(
    filed_vfs_t *vfs,
    filed_handle_id_t source_handle_id,
    filed_handle_id_t *out_handle_id)
{
    const filed_handle_t *source;

    if (vfs == NULL || source_handle_id == 0 || out_handle_id == NULL) {
        return FILED_ERR_INVALID;
    }
    *out_handle_id = 0;

    source = filed_find_handle_const(vfs, source_handle_id);
    if (source == NULL || source->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    if ((source->fd_flags & FILED_FD_CLOEXEC) != 0) {
        return FILED_ERR_DENIED;
    }

    return filed_vfs_dup_handle(
        vfs,
        source_handle_id,
        source->fd_flags & ~((uint32_t)FILED_FD_CLOEXEC),
        out_handle_id);
}

filed_status_t filed_vfs_get_handle_flags(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_handle_flags_t *out_flags)
{
    const filed_handle_t *handle;
    const filed_file_t *file;

    if (vfs == NULL || handle_id == 0 || out_flags == NULL) {
        return FILED_ERR_INVALID;
    }
    memset(out_flags, 0, sizeof(*out_flags));

    handle = filed_find_handle_const(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    file = filed_find_file_const(vfs, (filed_file_id_t)handle->target_id);
    if (file == NULL) {
        return FILED_ERR_INVALID;
    }

    out_flags->fd_flags = handle->fd_flags;
    filed_lock_acquire(filed_mutable_lock(&file->lock));
    out_flags->status_flags = file->status_flags;
    filed_lock_release(filed_mutable_lock(&file->lock));
    return FILED_OK;
}

filed_status_t filed_vfs_set_handle_flags(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    const filed_vfs_handle_flags_t *flags)
{
    filed_handle_t *handle;
    filed_file_t *file;

    if (vfs == NULL || handle_id == 0 || flags == NULL) {
        return FILED_ERR_INVALID;
    }
    if (!filed_fd_flags_are_known(flags->fd_flags) ||
        !filed_file_status_flags_are_known(flags->status_flags))
    {
        return FILED_ERR_INVALID;
    }

    handle = filed_find_handle(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    file = filed_find_file(vfs, (filed_file_id_t)handle->target_id);
    if (file == NULL) {
        return FILED_ERR_INVALID;
    }

    handle->fd_flags = flags->fd_flags;
    filed_lock_acquire(&file->lock);
    file->status_flags = flags->status_flags;
    filed_lock_release(&file->lock);
    return FILED_OK;
}

static filed_status_t filed_prepare_file_handle(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint32_t required_rights,
    filed_vfs_io_decision_t *out_decision)
{
    const filed_handle_t *handle;
    const filed_file_t *file;
    const filed_vnode_t *vnode;

    if (vfs == NULL || handle_id == 0 || out_decision == NULL) {
        return FILED_ERR_INVALID;
    }

    handle = filed_find_handle_const(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    if (!filed_rights_include(handle->rights, required_rights)) {
        return FILED_ERR_DENIED;
    }
    file = filed_find_file_const(vfs, (filed_file_id_t)handle->target_id);
    if (file == NULL) {
        return FILED_ERR_INVALID;
    }
    vnode = filed_find_vnode_const(vfs, file->vnode_id);
    if (vnode == NULL) {
        return FILED_ERR_INVALID;
    }

    memset(out_decision, 0, sizeof(*out_decision));
    out_decision->backend_object = vnode->backend_object;
    out_decision->kind = vnode->kind;
    filed_lock_acquire(filed_mutable_lock(&vnode->lock));
    out_decision->object_generation = vnode->object_generation;
    out_decision->dir_generation = vnode->dir_generation;
    filed_lock_release(filed_mutable_lock(&vnode->lock));
    return FILED_OK;
}

filed_status_t filed_vfs_stat_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_io_decision_t *out_decision)
{
    return filed_prepare_file_handle(vfs, handle_id, FILED_RIGHT_STAT, out_decision);
}

filed_status_t filed_vfs_get_stat_snapshot(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_stat_snapshot_t *out_snapshot)
{
    const filed_handle_t *handle;
    const filed_file_t *file;
    const filed_vnode_t *vnode;

    if (vfs == NULL || handle_id == 0 || out_snapshot == NULL) {
        return FILED_ERR_INVALID;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));

    handle = filed_find_handle_const(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    if (!filed_rights_include(handle->rights, FILED_RIGHT_STAT)) {
        return FILED_ERR_DENIED;
    }
    file = filed_find_file_const(vfs, (filed_file_id_t)handle->target_id);
    if (file == NULL) {
        return FILED_ERR_INVALID;
    }
    vnode = filed_find_vnode_const(vfs, file->vnode_id);
    if (vnode == NULL) {
        return FILED_ERR_INVALID;
    }

    filed_lock_acquire(filed_mutable_lock(&vnode->lock));
    out_snapshot->object_generation = vnode->object_generation;
    out_snapshot->dir_generation = vnode->dir_generation;
    if (vnode->stat_valid) {
        out_snapshot->valid = true;
        out_snapshot->handle_id = handle_id;
        out_snapshot->mode = vnode->stat_mode;
        out_snapshot->size = vnode->stat_size;
        out_snapshot->blocks = vnode->stat_blocks;
        out_snapshot->nlink = vnode->stat_nlink;
        out_snapshot->kind = vnode->stat_kind;
        out_snapshot->rdev = vnode->stat_rdev;
        out_snapshot->times_valid = vnode->stat_times_valid;
        out_snapshot->atime_sec = vnode->stat_atime_sec;
        out_snapshot->atime_nsec = vnode->stat_atime_nsec;
        out_snapshot->mtime_sec = vnode->stat_mtime_sec;
        out_snapshot->mtime_nsec = vnode->stat_mtime_nsec;
        out_snapshot->ctime_sec = vnode->stat_ctime_sec;
        out_snapshot->ctime_nsec = vnode->stat_ctime_nsec;
    }
    filed_lock_release(filed_mutable_lock(&vnode->lock));
    return FILED_OK;
}

static bool filed_path_component_equals(
    const char *path,
    size_t start,
    size_t end,
    const char *name)
{
    size_t name_len;

    if (path == NULL || name == NULL || end <= start) {
        return false;
    }
    name_len = strlen(name);
    if (name_len != end - start) {
        return false;
    }
    return memcmp(path + start, name, name_len) == 0;
}

filed_status_t filed_vfs_validate_cached_handle_path(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    const char *absolute_path,
    uint32_t rights,
    filed_generation_t object_generation)
{
    const filed_handle_t *handle;
    const filed_file_t *file;
    const filed_vnode_t *vnode;
    const filed_vnode_t *current;
    size_t end;

    if (vfs == NULL ||
        handle_id == 0 ||
        absolute_path == NULL ||
        absolute_path[0] != '/')
    {
        return FILED_ERR_INVALID;
    }

    handle = filed_find_handle_const(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_NOT_FOUND;
    }
    if (!filed_rights_include(handle->rights, rights)) {
        return FILED_ERR_DENIED;
    }
    file = filed_find_file_const(vfs, (filed_file_id_t)handle->target_id);
    if (file == NULL) {
        return FILED_ERR_NOT_FOUND;
    }
    vnode = filed_find_vnode_const(vfs, file->vnode_id);
    if (vnode == NULL || !vnode->active || !vnode->linked) {
        return FILED_ERR_NOT_FOUND;
    }
    if (object_generation != 0 && vnode->object_generation != object_generation) {
        return FILED_ERR_NOT_FOUND;
    }

    end = 0;
    while (end < 256 && absolute_path[end] != '\0') {
        ++end;
    }
    if (end == 0 || end >= 256) {
        return FILED_ERR_INVALID;
    }
    if (end == 1) {
        return vnode->parent == 0 ? FILED_OK : FILED_ERR_NOT_FOUND;
    }
    if (absolute_path[end - 1] == '/') {
        return FILED_ERR_NOT_FOUND;
    }

    current = vnode;
    while (current != NULL && current->parent != 0) {
        size_t slash = end;
        while (slash > 0 && absolute_path[slash - 1] != '/') {
            --slash;
        }
        if (slash == 0 || slash == end) {
            return FILED_ERR_NOT_FOUND;
        }
        if (!filed_path_component_equals(absolute_path, slash, end, current->name)) {
            return FILED_ERR_NOT_FOUND;
        }
        current = filed_find_vnode_const(vfs, current->parent);
        end = slash - 1;
    }

    if (current == NULL || current->parent != 0) {
        return FILED_ERR_NOT_FOUND;
    }
    return end == 0 ? FILED_OK : FILED_ERR_NOT_FOUND;
}

filed_status_t filed_vfs_update_stat_snapshot(
    filed_vfs_t *vfs,
    filed_backend_object_id_t backend_object,
    const filed_vfs_stat_snapshot_t *snapshot)
{
    bool updated = false;

    if (vfs == NULL || backend_object == 0 || snapshot == NULL || !snapshot->valid) {
        return FILED_ERR_INVALID;
    }

    for (uint32_t i = 0; i < FILED_MAX_VNODES; ++i) {
        filed_vnode_t *vnode = &vfs->vnodes[i];
        if (!vnode->active || vnode->backend_object != backend_object) {
            continue;
        }
        filed_lock_acquire(&vnode->lock);
        vnode->stat_valid = true;
        vnode->stat_mode = snapshot->mode;
        vnode->stat_size = snapshot->size;
        vnode->stat_blocks = snapshot->blocks;
        vnode->stat_nlink = snapshot->nlink;
        vnode->stat_kind = snapshot->kind;
        vnode->stat_rdev = snapshot->rdev;
        if (snapshot->times_valid) {
            vnode->stat_times_valid = true;
            vnode->stat_atime_sec = snapshot->atime_sec;
            vnode->stat_atime_nsec = snapshot->atime_nsec;
            vnode->stat_mtime_sec = snapshot->mtime_sec;
            vnode->stat_mtime_nsec = snapshot->mtime_nsec;
            vnode->stat_ctime_sec = snapshot->ctime_sec;
            vnode->stat_ctime_nsec = snapshot->ctime_nsec;
        }
        filed_lock_release(&vnode->lock);
        updated = true;
    }
    return updated ? FILED_OK : FILED_ERR_NOT_FOUND;
}

filed_status_t filed_vfs_note_write(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t offset,
    uint64_t bytes_written)
{
    filed_handle_t *handle;
    filed_file_t *file;
    filed_vnode_t *vnode;
    uint64_t end;

    if (vfs == NULL || handle_id == 0) {
        return FILED_ERR_INVALID;
    }
    if (bytes_written > UINT64_MAX - offset) {
        return FILED_ERR_OVERFLOW;
    }
    end = offset + bytes_written;
    handle = filed_find_handle(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    file = filed_find_file(vfs, (filed_file_id_t)handle->target_id);
    if (file == NULL) {
        return FILED_ERR_INVALID;
    }
    vnode = filed_find_vnode(vfs, file->vnode_id);
    if (vnode == NULL) {
        return FILED_ERR_INVALID;
    }

    for (uint32_t i = 0; i < FILED_MAX_VNODES; ++i) {
        filed_vnode_t *alias = &vfs->vnodes[i];
        if (!alias->active || alias->backend_object != vnode->backend_object) {
            continue;
        }
        filed_lock_acquire(&alias->lock);
        if (alias->stat_valid && end > alias->stat_size) {
            alias->stat_size = end;
        }
        filed_vnode_bump_object_generation_locked(alias);
        filed_lock_release(&alias->lock);
    }
    return FILED_OK;
}

filed_status_t filed_vfs_note_truncate(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t size)
{
    filed_handle_t *handle;
    filed_file_t *file;
    filed_vnode_t *vnode;

    if (vfs == NULL || handle_id == 0) {
        return FILED_ERR_INVALID;
    }
    handle = filed_find_handle(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    file = filed_find_file(vfs, (filed_file_id_t)handle->target_id);
    if (file == NULL) {
        return FILED_ERR_INVALID;
    }
    vnode = filed_find_vnode(vfs, file->vnode_id);
    if (vnode == NULL) {
        return FILED_ERR_INVALID;
    }

    for (uint32_t i = 0; i < FILED_MAX_VNODES; ++i) {
        filed_vnode_t *alias = &vfs->vnodes[i];
        if (!alias->active || alias->backend_object != vnode->backend_object) {
            continue;
        }
        filed_lock_acquire(&alias->lock);
        if (alias->stat_valid) {
            alias->stat_size = size;
        }
        filed_vnode_bump_object_generation_locked(alias);
        filed_lock_release(&alias->lock);
    }
    return FILED_OK;
}

filed_status_t filed_vfs_update_times(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint32_t mask,
    int64_t atime_sec,
    int64_t atime_nsec,
    int64_t mtime_sec,
    int64_t mtime_nsec)
{
    filed_handle_t *handle;
    filed_file_t *file;
    filed_vnode_t *vnode;

    if (vfs == NULL || handle_id == 0) {
        return FILED_ERR_INVALID;
    }
    if ((mask & ~((uint32_t)FILED_TIME_UPDATE_ATIME | (uint32_t)FILED_TIME_UPDATE_MTIME)) != 0) {
        return FILED_ERR_INVALID;
    }
    if (mask == 0) {
        return FILED_OK;
    }
    if (atime_nsec < 0 || atime_nsec >= 1000000000ll ||
        mtime_nsec < 0 || mtime_nsec >= 1000000000ll)
    {
        return FILED_ERR_INVALID;
    }
    handle = filed_find_handle(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    if (!filed_rights_include(handle->rights, FILED_RIGHT_WRITE)) {
        return FILED_ERR_DENIED;
    }
    file = filed_find_file(vfs, (filed_file_id_t)handle->target_id);
    if (file == NULL) {
        return FILED_ERR_INVALID;
    }
    vnode = filed_find_vnode(vfs, file->vnode_id);
    if (vnode == NULL) {
        return FILED_ERR_INVALID;
    }

    filed_lock_acquire(&vnode->lock);
    vnode->stat_times_valid = true;
    if ((mask & (uint32_t)FILED_TIME_UPDATE_ATIME) != 0) {
        vnode->stat_atime_sec = atime_sec;
        vnode->stat_atime_nsec = atime_nsec;
    }
    if ((mask & (uint32_t)FILED_TIME_UPDATE_MTIME) != 0) {
        vnode->stat_mtime_sec = mtime_sec;
        vnode->stat_mtime_nsec = mtime_nsec;
    }
    vnode->stat_ctime_sec =
        (mask & (uint32_t)FILED_TIME_UPDATE_MTIME) != 0 ? mtime_sec : atime_sec;
    vnode->stat_ctime_nsec =
        (mask & (uint32_t)FILED_TIME_UPDATE_MTIME) != 0 ? mtime_nsec : atime_nsec;
    filed_vnode_bump_object_generation_locked(vnode);
    filed_lock_release(&vnode->lock);
    return FILED_OK;
}

filed_status_t filed_vfs_update_mode(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t mode)
{
    filed_handle_t *handle;
    filed_file_t *file;
    filed_vnode_t *vnode;

    if (vfs == NULL || handle_id == 0 || (mode & ~07777ull) != 0) {
        return FILED_ERR_INVALID;
    }
    handle = filed_find_handle(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    if (!filed_rights_include(handle->rights, FILED_RIGHT_WRITE)) {
        return FILED_ERR_DENIED;
    }
    file = filed_find_file(vfs, (filed_file_id_t)handle->target_id);
    if (file == NULL) {
        return FILED_ERR_INVALID;
    }
    vnode = filed_find_vnode(vfs, file->vnode_id);
    if (vnode == NULL) {
        return FILED_ERR_INVALID;
    }

    filed_lock_acquire(&vnode->lock);
    if (!vnode->stat_valid) {
        filed_lock_release(&vnode->lock);
        return FILED_ERR_NOT_FOUND;
    }
    vnode->stat_mode = (vnode->stat_mode & 0170000ull) | (mode & 07777ull);
    filed_vnode_bump_object_generation_locked(vnode);
    filed_lock_release(&vnode->lock);
    return FILED_OK;
}

filed_status_t filed_vfs_lookup_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_io_decision_t *out_decision)
{
    filed_status_t status = filed_prepare_file_handle(
        vfs,
        handle_id,
        FILED_RIGHT_LOOKUP,
        out_decision);
    if (status != FILED_OK) {
        return status;
    }
    if (out_decision->kind != FILED_VNODE_DIRECTORY) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_NOT_DIR;
    }
    return FILED_OK;
}

filed_status_t filed_vfs_create_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t parent_handle_id,
    const char *name,
    filed_vfs_io_decision_t *out_decision)
{
    filed_status_t status = filed_prepare_file_handle(
        vfs,
        parent_handle_id,
        FILED_RIGHT_CREATE,
        out_decision);
    if (status != FILED_OK) {
        return status;
    }
    if (!filed_name_is_component(name)) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_INVALID;
    }
    if (out_decision->kind != FILED_VNODE_DIRECTORY) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_NOT_DIR;
    }
    return FILED_OK;
}

filed_status_t filed_vfs_pread_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t offset,
    uint64_t length,
    filed_vfs_io_decision_t *out_decision)
{
    filed_status_t status = filed_prepare_file_handle(
        vfs,
        handle_id,
        FILED_RIGHT_READ,
        out_decision);
    if (status != FILED_OK) {
        return status;
    }
    if (out_decision->kind == FILED_VNODE_DIRECTORY) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_IS_DIR;
    }
    out_decision->offset = offset;
    out_decision->length = length;
    return FILED_OK;
}

filed_status_t filed_vfs_pwrite_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t offset,
    uint64_t length,
    filed_vfs_io_decision_t *out_decision)
{
    filed_status_t status = filed_prepare_file_handle(
        vfs,
        handle_id,
        FILED_RIGHT_WRITE,
        out_decision);
    if (status != FILED_OK) {
        return status;
    }
    if (out_decision->kind == FILED_VNODE_DIRECTORY) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_IS_DIR;
    }
    out_decision->offset = offset;
    out_decision->length = length;
    return FILED_OK;
}

filed_status_t filed_vfs_read_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t length,
    filed_vfs_io_decision_t *out_decision)
{
    const filed_handle_t *handle;
    const filed_file_t *file;
    uint64_t offset;
    filed_status_t status = filed_prepare_file_handle(
        vfs,
        handle_id,
        FILED_RIGHT_READ,
        out_decision);
    if (status != FILED_OK) {
        return status;
    }
    if (out_decision->kind == FILED_VNODE_DIRECTORY) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_IS_DIR;
    }

    handle = filed_find_handle_const(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_INVALID;
    }
    file = filed_find_file_const(vfs, (filed_file_id_t)handle->target_id);
    status = filed_file_offset_snapshot(file, &offset);
    if (status != FILED_OK) {
        memset(out_decision, 0, sizeof(*out_decision));
        return status;
    }
    out_decision->offset = offset;
    out_decision->length = length;
    return FILED_OK;
}

filed_status_t filed_vfs_read_commit(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t bytes_read)
{
    const filed_handle_t *handle;
    filed_file_t *file;

    if (vfs == NULL || handle_id == 0) {
        return FILED_ERR_INVALID;
    }
    handle = filed_find_handle_const(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    file = filed_find_file(vfs, (filed_file_id_t)handle->target_id);
    return filed_file_offset_advance(file, bytes_read);
}

filed_status_t filed_vfs_write_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t length,
    filed_vfs_io_decision_t *out_decision)
{
    const filed_handle_t *handle;
    const filed_file_t *file;
    uint64_t offset;
    filed_status_t status = filed_prepare_file_handle(
        vfs,
        handle_id,
        FILED_RIGHT_WRITE,
        out_decision);
    if (status != FILED_OK) {
        return status;
    }
    if (out_decision->kind == FILED_VNODE_DIRECTORY) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_IS_DIR;
    }

    handle = filed_find_handle_const(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_INVALID;
    }
    file = filed_find_file_const(vfs, (filed_file_id_t)handle->target_id);
    status = filed_file_offset_snapshot(file, &offset);
    if (status != FILED_OK) {
        memset(out_decision, 0, sizeof(*out_decision));
        return status;
    }
    if ((filed_file_status_flags_snapshot(file) & FILED_FILE_APPEND) != 0) {
        out_decision->offset = UINT64_MAX;
    } else {
        out_decision->offset = offset;
    }
    out_decision->length = length;
    return FILED_OK;
}

filed_status_t filed_vfs_write_commit(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t bytes_written)
{
    return filed_vfs_read_commit(vfs, handle_id, bytes_written);
}

filed_status_t filed_vfs_seek(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    int64_t offset,
    int whence,
    uint64_t file_size,
    int64_t *out_offset)
{
    const filed_handle_t *handle;
    filed_file_t *file;
    int64_t base;
    int64_t next;

    if (vfs == NULL || handle_id == 0 || out_offset == NULL) {
        return FILED_ERR_INVALID;
    }

    handle = filed_find_handle_const(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    file = filed_find_file(vfs, (filed_file_id_t)handle->target_id);
    if (file == NULL) {
        return FILED_ERR_INVALID;
    }

    filed_lock_acquire(&file->offset_lock);
    switch (whence) {
    case 0:
        base = 0;
        break;
    case 1:
        base = file->offset;
        break;
    case 2:
        if (file_size > (uint64_t)INT64_MAX) {
            filed_lock_release(&file->offset_lock);
            return FILED_ERR_OVERFLOW;
        }
        base = (int64_t)file_size;
        break;
    default:
        filed_lock_release(&file->offset_lock);
        return FILED_ERR_INVALID;
    }

    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && base < INT64_MIN - offset))
    {
        filed_lock_release(&file->offset_lock);
        return FILED_ERR_OVERFLOW;
    }
    next = base + offset;
    if (next < 0) {
        filed_lock_release(&file->offset_lock);
        return FILED_ERR_INVALID;
    }

    file->offset = next;
    *out_offset = next;
    filed_lock_release(&file->offset_lock);
    return FILED_OK;
}

filed_status_t filed_vfs_fsync_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_io_decision_t *out_decision)
{
    filed_status_t status = filed_prepare_file_handle(
        vfs,
        handle_id,
        FILED_RIGHT_WRITE,
        out_decision);
    if (status != FILED_OK) {
        return status;
    }
    if (out_decision->kind == FILED_VNODE_DIRECTORY) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_IS_DIR;
    }
    return FILED_OK;
}

filed_status_t filed_vfs_close_flush_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_io_decision_t *out_decision)
{
    const filed_handle_t *handle;
    const filed_file_t *file;
    const filed_vnode_t *vnode;

    if (vfs == NULL || handle_id == 0 || out_decision == NULL) {
        return FILED_ERR_INVALID;
    }

    memset(out_decision, 0, sizeof(*out_decision));
    handle = filed_find_handle_const(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    if (!filed_rights_include(handle->rights, FILED_RIGHT_WRITE)) {
        return FILED_OK;
    }

    file = filed_find_file_const(vfs, (filed_file_id_t)handle->target_id);
    if (file == NULL) {
        return FILED_ERR_INVALID;
    }
    vnode = filed_find_vnode_const(vfs, file->vnode_id);
    if (vnode == NULL) {
        return FILED_ERR_INVALID;
    }
    if (vnode->kind == FILED_VNODE_DIRECTORY) {
        return FILED_OK;
    }

    out_decision->backend_object = vnode->backend_object;
    out_decision->kind = vnode->kind;
    return FILED_OK;
}

filed_status_t filed_vfs_truncate_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t size,
    filed_vfs_io_decision_t *out_decision)
{
    filed_status_t status = filed_prepare_file_handle(
        vfs,
        handle_id,
        FILED_RIGHT_WRITE,
        out_decision);
    if (status != FILED_OK) {
        return status;
    }
    if (out_decision->kind == FILED_VNODE_DIRECTORY) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_IS_DIR;
    }
    out_decision->offset = 0;
    out_decision->length = size;
    return FILED_OK;
}

filed_status_t filed_vfs_unlink_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t parent_handle_id,
    const char *name,
    filed_vfs_io_decision_t *out_decision)
{
    filed_status_t status = filed_prepare_file_handle(
        vfs,
        parent_handle_id,
        FILED_RIGHT_REMOVE,
        out_decision);
    if (status != FILED_OK) {
        return status;
    }
    if (!filed_name_is_component(name)) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_INVALID;
    }
    if (out_decision->kind != FILED_VNODE_DIRECTORY) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_NOT_DIR;
    }
    return FILED_OK;
}

filed_status_t filed_vfs_unlink_commit_ex(
    filed_vfs_t *vfs,
    filed_handle_id_t parent_handle_id,
    const char *name,
    filed_vfs_reclaim_result_t *out_reclaim)
{
    filed_vnode_t *parent_vnode;
    filed_vnode_t *child;
    filed_status_t status = filed_handle_vnode(vfs, parent_handle_id, FILED_RIGHT_REMOVE, &parent_vnode);
    if (out_reclaim != NULL) {
        memset(out_reclaim, 0, sizeof(*out_reclaim));
    }
    if (status != FILED_OK) {
        return status;
    }
    if (!filed_name_is_component(name)) {
        return FILED_ERR_INVALID;
    }
    filed_vnode_write_lock(parent_vnode);
    child = filed_find_child_vnode(vfs, parent_vnode->id, name);
    if (child != NULL) {
        (void)filed_vnode_mark_unlinked(child);
        filed_reclaim_vnode_if_dead_ex(vfs, child, out_reclaim);
    }
    filed_vnode_bump_dir_generation_locked(parent_vnode);
    filed_vnode_write_unlock(parent_vnode);
    return FILED_OK;
}

filed_status_t filed_vfs_unlink_commit(
    filed_vfs_t *vfs,
    filed_handle_id_t parent_handle_id,
    const char *name)
{
    return filed_vfs_unlink_commit_ex(vfs, parent_handle_id, name, NULL);
}

filed_status_t filed_vfs_link_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t old_parent_handle_id,
    filed_handle_id_t new_parent_handle_id,
    const char *old_name,
    const char *new_name,
    filed_vfs_io_decision_t *out_old_parent,
    filed_vfs_io_decision_t *out_new_parent)
{
    filed_status_t status = filed_prepare_file_handle(
        vfs,
        old_parent_handle_id,
        FILED_RIGHT_LOOKUP,
        out_old_parent);
    if (status != FILED_OK) {
        return status;
    }
    status = filed_prepare_file_handle(
        vfs,
        new_parent_handle_id,
        FILED_RIGHT_CREATE,
        out_new_parent);
    if (status != FILED_OK) {
        memset(out_old_parent, 0, sizeof(*out_old_parent));
        return status;
    }
    if (!filed_name_is_component(old_name) || !filed_name_is_component(new_name)) {
        memset(out_old_parent, 0, sizeof(*out_old_parent));
        memset(out_new_parent, 0, sizeof(*out_new_parent));
        return FILED_ERR_INVALID;
    }
    if (out_old_parent->kind != FILED_VNODE_DIRECTORY || out_new_parent->kind != FILED_VNODE_DIRECTORY) {
        memset(out_old_parent, 0, sizeof(*out_old_parent));
        memset(out_new_parent, 0, sizeof(*out_new_parent));
        return FILED_ERR_NOT_DIR;
    }
    return FILED_OK;
}

filed_status_t filed_vfs_link_commit(
    filed_vfs_t *vfs,
    filed_handle_id_t new_parent_handle_id,
    filed_backend_object_id_t child_backend_object,
    filed_vnode_kind_t child_kind,
    const char *new_name,
    uint32_t rights,
    uint32_t open_flags,
    filed_vfs_open_result_t *out_open)
{
    return filed_vfs_create_backend_child(
        vfs,
        new_parent_handle_id,
        child_backend_object,
        child_kind,
        new_name,
        rights,
        open_flags,
        out_open);
}

filed_status_t filed_vfs_rename_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t old_parent_handle_id,
    filed_handle_id_t new_parent_handle_id,
    const char *old_name,
    const char *new_name,
    filed_vfs_io_decision_t *out_old_parent,
    filed_vfs_io_decision_t *out_new_parent)
{
    filed_status_t status = filed_prepare_file_handle(
        vfs,
        old_parent_handle_id,
        FILED_RIGHT_RENAME,
        out_old_parent);
    if (status != FILED_OK) {
        return status;
    }
    status = filed_prepare_file_handle(
        vfs,
        new_parent_handle_id,
        FILED_RIGHT_RENAME,
        out_new_parent);
    if (status != FILED_OK) {
        memset(out_old_parent, 0, sizeof(*out_old_parent));
        return status;
    }
    if (!filed_name_is_component(old_name) || !filed_name_is_component(new_name)) {
        memset(out_old_parent, 0, sizeof(*out_old_parent));
        memset(out_new_parent, 0, sizeof(*out_new_parent));
        return FILED_ERR_INVALID;
    }
    if (out_old_parent->kind != FILED_VNODE_DIRECTORY || out_new_parent->kind != FILED_VNODE_DIRECTORY) {
        memset(out_old_parent, 0, sizeof(*out_old_parent));
        memset(out_new_parent, 0, sizeof(*out_new_parent));
        return FILED_ERR_NOT_DIR;
    }
    return FILED_OK;
}

filed_status_t filed_vfs_rename_commit_ex(
    filed_vfs_t *vfs,
    filed_handle_id_t old_parent_handle_id,
    filed_handle_id_t new_parent_handle_id,
    const char *old_name,
    const char *new_name,
    filed_backend_object_id_t backend_object,
    filed_vfs_reclaim_result_t *out_reclaim)
{
    filed_vnode_t *old_parent;
    filed_vnode_t *new_parent;
    filed_vnode_t *old_child;
    filed_vnode_t *replaced;
    filed_status_t status = filed_handle_vnode(vfs, old_parent_handle_id, FILED_RIGHT_RENAME, &old_parent);
    if (out_reclaim != NULL) {
        memset(out_reclaim, 0, sizeof(*out_reclaim));
    }
    if (status != FILED_OK) {
        return status;
    }
    status = filed_handle_vnode(vfs, new_parent_handle_id, FILED_RIGHT_RENAME, &new_parent);
    if (status != FILED_OK) {
        return status;
    }
    if (!filed_name_is_component(old_name) || !filed_name_is_component(new_name)) {
        return FILED_ERR_INVALID;
    }
    filed_vnode_write_lock_pair(old_parent, new_parent);
    old_child = filed_find_child_vnode(vfs, old_parent->id, old_name);
    replaced = filed_find_child_vnode(vfs, new_parent->id, new_name);
    if (old_child != NULL && replaced == old_child) {
        filed_vnode_write_unlock_pair(old_parent, new_parent);
        return FILED_OK;
    }
    if (old_child != NULL &&
        replaced != NULL &&
        old_child != replaced &&
        old_child->backend_object == backend_object &&
        replaced->backend_object == backend_object)
    {
        filed_vnode_write_unlock_pair(old_parent, new_parent);
        return FILED_OK;
    }
    if (replaced != NULL && replaced != old_child) {
        (void)filed_vnode_mark_unlinked(replaced);
        filed_reclaim_vnode_if_dead_ex(vfs, replaced, out_reclaim);
    }
    if (old_child != NULL) {
        filed_status_t copy_status;
        filed_vnode_bump_dir_generation_locked(old_parent);
        if (new_parent != old_parent) {
            filed_vnode_bump_dir_generation_locked(new_parent);
        }
        filed_lock_acquire(&old_child->lock);
        old_child->parent = new_parent->id;
        old_child->backend_object = backend_object;
        filed_vnode_bump_object_generation_locked(old_child);
        copy_status = filed_copy_name(old_child->name, sizeof(old_child->name), new_name);
        filed_lock_release(&old_child->lock);
        filed_vnode_write_unlock_pair(old_parent, new_parent);
        return copy_status;
    }
    filed_vnode_write_unlock_pair(old_parent, new_parent);
    return FILED_OK;
}

filed_status_t filed_vfs_rename_commit(
    filed_vfs_t *vfs,
    filed_handle_id_t old_parent_handle_id,
    filed_handle_id_t new_parent_handle_id,
    const char *old_name,
    const char *new_name,
    filed_backend_object_id_t backend_object)
{
    return filed_vfs_rename_commit_ex(
        vfs,
        old_parent_handle_id,
        new_parent_handle_id,
        old_name,
        new_name,
        backend_object,
        NULL);
}

filed_status_t filed_vfs_getdents_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_io_decision_t *out_decision)
{
    const filed_handle_t *handle;
    const filed_file_t *file;
    uint64_t offset;
    filed_status_t status = filed_prepare_file_handle(
        vfs,
        handle_id,
        FILED_RIGHT_GETDENTS,
        out_decision);
    if (status != FILED_OK) {
        return status;
    }
    if (out_decision->kind != FILED_VNODE_DIRECTORY) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_NOT_DIR;
    }
    handle = filed_find_handle_const(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_INVALID;
    }
    file = filed_find_file_const(vfs, (filed_file_id_t)handle->target_id);
    status = filed_file_offset_snapshot(file, &offset);
    if (status != FILED_OK) {
        memset(out_decision, 0, sizeof(*out_decision));
        return status;
    }
    out_decision->offset = offset;
    return FILED_OK;
}

filed_status_t filed_vfs_getdents_commit(
    filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t entries_read)
{
    return filed_vfs_read_commit(vfs, handle_id, entries_read);
}

filed_status_t filed_vfs_check_basic(const filed_vfs_t *vfs)
{
    size_t i;

    if (vfs == NULL) {
        return FILED_ERR_INVALID;
    }

    for (i = 0; i < FILED_MAX_MOUNTS; ++i) {
        const filed_mount_t *mount = &vfs->mounts[i];
        if (!mount->active) {
            continue;
        }
        if (mount->id == 0 || mount->root_vnode == 0) {
            return FILED_ERR_INVALID;
        }
        if (!filed_vnode_id_exists(vfs, mount->root_vnode)) {
            return FILED_ERR_INVALID;
        }
    }

    for (i = 0; i < FILED_MAX_VNODES; ++i) {
        const filed_vnode_t *node = &vfs->vnodes[i];
        uint32_t expected_refcount;
        size_t j;
        if (!node->active) {
            continue;
        }
        if (node->id == 0 || !filed_mount_id_exists(vfs, node->mount_id)) {
            return FILED_ERR_INVALID;
        }
        if (node->refcount == 0 && !node->linked) {
            return FILED_ERR_INVALID;
        }
        if (node->parent != 0 && !filed_vnode_id_exists(vfs, node->parent)) {
            return FILED_ERR_INVALID;
        }
        if (node->linked && node->name[0] == '\0') {
            return FILED_ERR_INVALID;
        }
        if (node->generation == 0 ||
            node->object_generation == 0 ||
            node->dir_generation == 0)
        {
            return FILED_ERR_INVALID;
        }
        expected_refcount = filed_vnode_mount_pins(vfs, node->id);
        for (j = 0; j < FILED_MAX_FILES; ++j) {
            if (vfs->files[j].active && vfs->files[j].vnode_id == node->id) {
                ++expected_refcount;
            }
        }
        if (node->refcount != expected_refcount) {
            return FILED_ERR_INVALID;
        }
        if (node->linked) {
            for (j = i + 1; j < FILED_MAX_VNODES; ++j) {
                const filed_vnode_t *other = &vfs->vnodes[j];
                if (other->active &&
                    other->linked &&
                    other->mount_id == node->mount_id &&
                    other->parent == node->parent &&
                    strcmp(other->name, node->name) == 0)
                {
                    return FILED_ERR_INVALID;
                }
            }
        }
    }

    for (i = 0; i < FILED_MAX_FILES; ++i) {
        const filed_file_t *file = &vfs->files[i];
        uint32_t handle_count = 0;
        size_t j;
        if (!file->active) {
            continue;
        }
        if (file->id == 0 ||
            file->offset < 0 ||
            file->refcount == 0 ||
            !filed_vnode_id_exists(vfs, file->vnode_id))
        {
            return FILED_ERR_INVALID;
        }
        for (j = 0; j < FILED_MAX_HANDLES; ++j) {
            if (vfs->handles[j].active &&
                vfs->handles[j].target_kind == FILED_HANDLE_FILE &&
                vfs->handles[j].target_id == file->id)
            {
                ++handle_count;
            }
        }
        if (file->refcount != handle_count) {
            return FILED_ERR_INVALID;
        }
    }

    for (i = 0; i < FILED_MAX_HANDLES; ++i) {
        const filed_handle_t *handle = &vfs->handles[i];
        if (!handle->active) {
            continue;
        }
        if (handle->id == 0 || handle->rights == 0) {
            return FILED_ERR_INVALID;
        }
        if (handle->target_kind == FILED_HANDLE_FILE) {
            if (filed_find_file_const(vfs, (filed_file_id_t)handle->target_id) == NULL) {
                return FILED_ERR_INVALID;
            }
        } else if (handle->target_kind != FILED_HANDLE_MOUNT &&
            handle->target_kind != FILED_HANDLE_VNODE)
        {
            return FILED_ERR_INVALID;
        }
    }

    return FILED_OK;
}
