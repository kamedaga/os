#include "filed/vfs.h"

#include <string.h>

static filed_status_t filed_copy_name(char *dst, size_t dst_size, const char *src)
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

static bool filed_name_is_dot_or_dotdot(const char *name)
{
    return name != NULL &&
        (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
}

static bool filed_name_is_component(const char *name)
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

static bool filed_fd_flags_are_known(uint32_t flags)
{
    return (flags & ~((uint32_t)FILED_FD_CLOEXEC)) == 0;
}

static bool filed_file_status_flags_are_known(uint32_t flags)
{
    const uint32_t known =
        FILED_FILE_APPEND |
        FILED_FILE_NONBLOCK |
        FILED_FILE_SYNC;
    return (flags & ~known) == 0;
}

static filed_mount_t *filed_alloc_mount(filed_vfs_t *vfs)
{
    size_t i;

    for (i = 0; i < FILED_MAX_MOUNTS; ++i) {
        if (!vfs->mounts[i].active) {
            return &vfs->mounts[i];
        }
    }

    return NULL;
}

static filed_vnode_t *filed_alloc_vnode(filed_vfs_t *vfs)
{
    size_t i;

    for (i = 0; i < FILED_MAX_VNODES; ++i) {
        if (!vfs->vnodes[i].active) {
            return &vfs->vnodes[i];
        }
    }

    return NULL;
}

static filed_file_t *filed_alloc_file(filed_vfs_t *vfs)
{
    size_t i;

    for (i = 0; i < FILED_MAX_FILES; ++i) {
        if (!vfs->files[i].active) {
            return &vfs->files[i];
        }
    }

    return NULL;
}

static filed_handle_t *filed_alloc_handle(filed_vfs_t *vfs)
{
    size_t i;

    for (i = 0; i < FILED_MAX_HANDLES; ++i) {
        if (!vfs->handles[i].active) {
            return &vfs->handles[i];
        }
    }

    return NULL;
}

static bool filed_mount_id_exists(const filed_vfs_t *vfs, filed_mount_id_t id)
{
    size_t i;

    for (i = 0; i < FILED_MAX_MOUNTS; ++i) {
        if (vfs->mounts[i].active && vfs->mounts[i].id == id) {
            return true;
        }
    }

    return false;
}

static bool filed_vnode_id_exists(const filed_vfs_t *vfs, filed_vnode_id_t id)
{
    size_t i;

    for (i = 0; i < FILED_MAX_VNODES; ++i) {
        if (vfs->vnodes[i].active && vfs->vnodes[i].id == id) {
            return true;
        }
    }

    return false;
}

static filed_mount_t *filed_find_mount(filed_vfs_t *vfs, filed_mount_id_t id)
{
    size_t i;

    for (i = 0; i < FILED_MAX_MOUNTS; ++i) {
        if (vfs->mounts[i].active && vfs->mounts[i].id == id) {
            return &vfs->mounts[i];
        }
    }

    return NULL;
}

static filed_vnode_t *filed_find_vnode(filed_vfs_t *vfs, filed_vnode_id_t id)
{
    size_t i;

    for (i = 0; i < FILED_MAX_VNODES; ++i) {
        if (vfs->vnodes[i].active && vfs->vnodes[i].id == id) {
            return &vfs->vnodes[i];
        }
    }

    return NULL;
}

static const filed_vnode_t *filed_find_vnode_const(const filed_vfs_t *vfs, filed_vnode_id_t id)
{
    return filed_find_vnode((filed_vfs_t *)(uintptr_t)vfs, id);
}

static filed_file_t *filed_find_file(filed_vfs_t *vfs, filed_file_id_t id)
{
    size_t i;

    for (i = 0; i < FILED_MAX_FILES; ++i) {
        if (vfs->files[i].active && vfs->files[i].id == id) {
            return &vfs->files[i];
        }
    }

    return NULL;
}

static const filed_file_t *filed_find_file_const(const filed_vfs_t *vfs, filed_file_id_t id)
{
    return filed_find_file((filed_vfs_t *)(uintptr_t)vfs, id);
}

static filed_handle_t *filed_find_handle(filed_vfs_t *vfs, filed_handle_id_t id)
{
    size_t i;

    for (i = 0; i < FILED_MAX_HANDLES; ++i) {
        if (vfs->handles[i].active && vfs->handles[i].id == id) {
            return &vfs->handles[i];
        }
    }

    return NULL;
}

static const filed_handle_t *filed_find_handle_const(const filed_vfs_t *vfs, filed_handle_id_t id)
{
    return filed_find_handle((filed_vfs_t *)(uintptr_t)vfs, id);
}

static filed_vnode_t *filed_find_backend_vnode(
    filed_vfs_t *vfs,
    filed_mount_id_t mount_id,
    filed_backend_object_id_t backend_object)
{
    size_t i;

    for (i = 0; i < FILED_MAX_VNODES; ++i) {
        if (vfs->vnodes[i].active &&
            vfs->vnodes[i].linked &&
            vfs->vnodes[i].mount_id == mount_id &&
            vfs->vnodes[i].backend_object == backend_object)
        {
            return &vfs->vnodes[i];
        }
    }

    return NULL;
}

static uint32_t filed_vnode_mount_pins(const filed_vfs_t *vfs, filed_vnode_id_t vnode_id)
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

static bool filed_vnode_is_dead(const filed_vfs_t *vfs, const filed_vnode_t *vnode)
{
    if (vfs == NULL || vnode == NULL || !vnode->active) {
        return false;
    }
    if (vnode->linked || vnode->refcount != 0 || filed_vnode_mount_pins(vfs, vnode->id) != 0) {
        return false;
    }
    return true;
}

static void filed_reclaim_result_set(
    filed_vfs_reclaim_result_t *out_reclaim,
    const filed_vnode_t *vnode)
{
    if (out_reclaim == NULL || vnode == NULL || vnode->backend_object == 0) {
        return;
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
    filed_reclaim_result_set(out_reclaim, vnode);
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

    if (vfs == NULL || vnode == NULL || out_open == NULL || rights == 0) {
        return FILED_ERR_INVALID;
    }
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

    file_id = vfs->next_file_id;
    handle_id = vfs->next_handle_id;

    memset(file, 0, sizeof(*file));
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

    ++vnode->refcount;
    ++vfs->next_file_id;
    ++vfs->next_handle_id;

    memset(out_open, 0, sizeof(*out_open));
    out_open->handle_id = handle_id;
    out_open->vnode_id = vnode->id;
    out_open->backend_object = vnode->backend_object;
    out_open->kind = vnode->kind;
    return FILED_OK;
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
    root->active = true;
    root->linked = true;
    root->id = root_id;
    root->mount_id = mount_id;
    root->backend_object = root_backend_object;
    root->kind = FILED_VNODE_DIRECTORY;
    root->parent = 0;
    root->generation = 1;
    root->refcount = 1;

    status = filed_copy_name(root->name, sizeof(root->name), "/");
    if (status != FILED_OK) {
        memset(mount, 0, sizeof(*mount));
        memset(root, 0, sizeof(*root));
        return status;
    }

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
    filed_vnode_t *child;
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

    child = filed_find_backend_vnode(vfs, parent_vnode->mount_id, child_backend_object);
    if (child == NULL) {
        if (vfs->next_vnode_id == 0) {
            return FILED_ERR_OVERFLOW;
        }
        child = filed_alloc_vnode(vfs);
        if (child == NULL) {
            return FILED_ERR_FULL;
        }
        memset(child, 0, sizeof(*child));
        child->active = true;
        child->linked = true;
        child->id = vfs->next_vnode_id;
        child->mount_id = parent_vnode->mount_id;
        child->backend_object = child_backend_object;
        child->kind = child_kind;
        child->parent = parent_vnode->id;
        child->generation = 1;
        child->refcount = 0;
        status = filed_copy_name(child->name, sizeof(child->name), name);
        if (status != FILED_OK) {
            memset(child, 0, sizeof(*child));
            return status;
        }
        ++vfs->next_vnode_id;
    }

    return filed_open_vnode(vfs, child, rights, open_flags, out_open);
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
    if (filed_find_child_vnode(vfs, parent_vnode->id, name) != NULL) {
        return FILED_ERR_EXISTS;
    }
    return filed_vfs_open_backend_child(
        vfs,
        parent_handle,
        child_backend_object,
        child_kind,
        name,
        rights,
        open_flags,
        out_open);
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
            if (file->refcount > 0) {
                --file->refcount;
            }
            if (file->refcount == 0) {
                vnode = filed_find_vnode(vfs, file->vnode_id);
                if (vnode != NULL && vnode->refcount > 0) {
                    --vnode->refcount;
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
    if (file->refcount == UINT32_MAX || vfs->next_handle_id == 0) {
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

    ++file->refcount;
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
    out_flags->status_flags = file->status_flags;
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
    file->status_flags = flags->status_flags;
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
    return FILED_OK;
}

filed_status_t filed_vfs_stat_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    filed_vfs_io_decision_t *out_decision)
{
    return filed_prepare_file_handle(vfs, handle_id, FILED_RIGHT_STAT, out_decision);
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
    if (file == NULL || file->offset < 0) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_INVALID;
    }
    out_decision->offset = (uint64_t)file->offset;
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
    uint64_t old_offset;

    if (vfs == NULL || handle_id == 0) {
        return FILED_ERR_INVALID;
    }
    handle = filed_find_handle_const(vfs, handle_id);
    if (handle == NULL || handle->target_kind != FILED_HANDLE_FILE) {
        return FILED_ERR_INVALID;
    }
    file = filed_find_file(vfs, (filed_file_id_t)handle->target_id);
    if (file == NULL || file->offset < 0) {
        return FILED_ERR_INVALID;
    }
    old_offset = (uint64_t)file->offset;
    if (bytes_read > (uint64_t)INT64_MAX - old_offset) {
        return FILED_ERR_OVERFLOW;
    }
    file->offset = (int64_t)(old_offset + bytes_read);
    return FILED_OK;
}

filed_status_t filed_vfs_write_prepare(
    const filed_vfs_t *vfs,
    filed_handle_id_t handle_id,
    uint64_t length,
    filed_vfs_io_decision_t *out_decision)
{
    const filed_handle_t *handle;
    const filed_file_t *file;
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
    if (file == NULL || file->offset < 0) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_INVALID;
    }
    if ((file->status_flags & FILED_FILE_APPEND) != 0) {
        out_decision->offset = UINT64_MAX;
    } else {
        out_decision->offset = (uint64_t)file->offset;
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
    child = filed_find_child_vnode(vfs, parent_vnode->id, name);
    if (child != NULL) {
        child->linked = false;
        ++child->generation;
        filed_reclaim_vnode_if_dead_ex(vfs, child, out_reclaim);
    }
    return FILED_OK;
}

filed_status_t filed_vfs_unlink_commit(
    filed_vfs_t *vfs,
    filed_handle_id_t parent_handle_id,
    const char *name)
{
    return filed_vfs_unlink_commit_ex(vfs, parent_handle_id, name, NULL);
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
    old_child = filed_find_child_vnode(vfs, old_parent->id, old_name);
    if (!filed_name_is_component(old_name) || !filed_name_is_component(new_name)) {
        return FILED_ERR_INVALID;
    }
    replaced = filed_find_child_vnode(vfs, new_parent->id, new_name);
    if (replaced != NULL && replaced != old_child) {
        replaced->linked = false;
        ++replaced->generation;
        filed_reclaim_vnode_if_dead_ex(vfs, replaced, out_reclaim);
    }
    if (old_child != NULL) {
        old_child->parent = new_parent->id;
        old_child->backend_object = backend_object;
        ++old_child->generation;
        return filed_copy_name(old_child->name, sizeof(old_child->name), new_name);
    }
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
    if (file == NULL || file->offset < 0) {
        memset(out_decision, 0, sizeof(*out_decision));
        return FILED_ERR_INVALID;
    }
    out_decision->offset = (uint64_t)file->offset;
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
