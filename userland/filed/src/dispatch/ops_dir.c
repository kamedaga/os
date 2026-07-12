#include "common.h"

filed_page_dispatch_result_t filed_dispatch_unlink_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_unlink_t *unlink = (filed_unlink_t *)page;
    filed_vfs_io_decision_t decision;
    filed_status_t status = FILED_ERR_INVALID;
    int64_t reply_status = -22;
    uint64_t target_object = 0;
    bool target_lookup_owned = false;
    if (unlink->reserved0 == 0 &&
        filed_name_is_terminated(unlink->name, sizeof(unlink->name)))
    {
        filed_handle_id_t dir_handle = 0;
        int dir_owned = 0;
        char name[FILED_NAME_BYTES];
        const filed_handle_id_t base_dir =
            unlink->dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)unlink->dir_handle;

        memset(name, 0, sizeof(name));
        reply_status = filed_resolve_parent_path(
            runtime,
            base_dir,
            unlink->name,
            FILED_RIGHT_REMOVE,
            &dir_handle,
            &dir_owned,
            name,
            sizeof(name));
        if (reply_status == 0) {
            status = filed_vfs_unlink_prepare(&runtime->vfs, dir_handle, name, &decision);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK) {
                target_object = filed_lookup_cache_target_object(
                    runtime,
                    dir_handle,
                    decision.backend_object,
                    name,
                    &target_lookup_owned);
                reply_status = filed_flush_mutated_object(runtime, target_object);
                if (reply_status == 0) {
                    reply_status = filed_backend_unlink(
                        runtime,
                        decision.backend_object,
                        name);
                }
                if (reply_status == 0) {
                    filed_cache_invalidate(runtime, decision.backend_object);
                    filed_invalidate_mutated_object(runtime, target_object);
                    filed_vfs_reclaim_result_t reclaim;
                    memset(&reclaim, 0, sizeof(reclaim));
                    status = filed_vfs_unlink_commit_ex(&runtime->vfs, dir_handle, name, &reclaim);
                    if (status != FILED_OK) {
                        reply_status = filed_status_to_wire(status);
                    } else {
                        filed_runtime_publish_backend_object_generation(
                            runtime,
                            decision.backend_object);
                        if (target_object != 0) {
                            filed_runtime_publish_backend_object_generation(
                                runtime,
                                target_object);
                        }
                        const int release_status = filed_release_reclaimed_object(runtime, &reclaim);
                        if (release_status != 0) {
                            reply_status = release_status;
                        }
                    }
                }
            }
            if (target_lookup_owned) {
                (void)filed_backend_release_object(runtime, target_object);
            }
            filed_close_walk_handle(runtime, dir_handle, dir_owned);
        }
    }

    return filed_page_result(reply_status, 0);
}

filed_page_dispatch_result_t filed_dispatch_mkdir_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_mkdir_t *mkdir = (filed_mkdir_t *)page;
    filed_vfs_io_decision_t decision;
    filed_status_t status = FILED_ERR_INVALID;
    int64_t reply_status = -22;
    uint64_t object_id = 0;
    if (filed_name_is_terminated(mkdir->name, sizeof(mkdir->name))) {
        filed_handle_id_t dir_handle = 0;
        int dir_owned = 0;
        char name[FILED_NAME_BYTES];
        const filed_handle_id_t base_dir =
            mkdir->dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)mkdir->dir_handle;

        memset(name, 0, sizeof(name));
        reply_status = filed_resolve_parent_path(
            runtime,
            base_dir,
            mkdir->name,
            FILED_RIGHT_CREATE,
            &dir_handle,
            &dir_owned,
            name,
            sizeof(name));
        if (reply_status == 0) {
            status = filed_vfs_create_prepare(&runtime->vfs, dir_handle, name, &decision);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK) {
                reply_status = filed_backend_mkdir(
                    runtime,
                    decision.backend_object,
                    name,
                    mkdir->mode,
                    &object_id);
                if (reply_status == 0 && object_id != 0) {
                    filed_cache_invalidate(runtime, decision.backend_object);
                    filed_vfs_open_result_t opened;
                    memset(&opened, 0, sizeof(opened));
                    if (filed_vfs_create_backend_child(
                            &runtime->vfs,
                            dir_handle,
                            object_id,
                            FILED_VNODE_DIRECTORY,
                            name,
                            FILED_RIGHT_LOOKUP |
                                FILED_RIGHT_STAT |
                                FILED_RIGHT_GETDENTS |
                                FILED_RIGHT_CREATE |
                                FILED_RIGHT_REMOVE |
                                FILED_RIGHT_RENAME,
                            FILED_OPEN_DIRECTORY,
                            &opened) == FILED_OK)
                    {
                        const filed_vfs_stat_snapshot_t snapshot =
                            filed_directory_snapshot_from_create(
                                opened.handle_id,
                                mkdir->mode,
                                opened.object_generation,
                                opened.dir_generation);
                        (void)filed_vfs_update_stat_snapshot(
                            &runtime->vfs,
                            object_id,
                            &snapshot);
                        filed_runtime_publish_backend_object_generation(
                            runtime,
                            decision.backend_object);
                        (void)filed_vfs_close_handle(&runtime->vfs, opened.handle_id);
                    }
                }
            }
            filed_close_walk_handle(runtime, dir_handle, dir_owned);
        }
    }

    return filed_page_result(reply_status, 0);
}

filed_page_dispatch_result_t filed_dispatch_mknod_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_mknod_t *mknod = (filed_mknod_t *)page;
    filed_vfs_io_decision_t decision;
    int64_t reply_status = -22;
    uint64_t object_id = 0;
    const uint64_t type = mknod != NULL ? mknod->mode & 0170000u : 0;
    if (runtime == NULL || mknod == NULL ||
        (type != 0010000u && type != 0020000u &&
         type != 0060000u && type != 0140000u) ||
        !filed_name_is_terminated(mknod->name, sizeof(mknod->name)))
    {
        return filed_page_result(-22, 0);
    }

    filed_handle_id_t dir_handle = 0;
    int dir_owned = 0;
    char name[FILED_NAME_BYTES];
    const filed_handle_id_t base_dir = mknod->dir_handle == 0 ?
        runtime->root_handle_id : (filed_handle_id_t)(uint32_t)mknod->dir_handle;
    memset(name, 0, sizeof(name));
    reply_status = filed_resolve_parent_path(
        runtime, base_dir, mknod->name, FILED_RIGHT_CREATE,
        &dir_handle, &dir_owned, name, sizeof(name));
    if (reply_status == 0) {
        filed_status_t status = filed_vfs_create_prepare(
            &runtime->vfs, dir_handle, name, &decision);
        reply_status = filed_status_to_wire(status);
        if (status == FILED_OK) {
            reply_status = filed_backend_mknod(
                runtime, decision.backend_object, name,
                mknod->mode, mknod->dev, &object_id);
            if (reply_status == 0 && object_id != 0) {
                storage_statx_reply_t backend_stat;
                memset(&backend_stat, 0, sizeof(backend_stat));
                reply_status = filed_backend_statx(runtime, object_id, &backend_stat);
                if (reply_status == 0) {
                    filed_cache_invalidate(runtime, decision.backend_object);
                    filed_vfs_open_result_t opened;
                    memset(&opened, 0, sizeof(opened));
                    status = filed_vfs_create_backend_child(
                        &runtime->vfs, dir_handle, object_id,
                        filed_kind_from_unix_type(backend_stat.kind), name,
                        FILED_RIGHT_STAT, 0, &opened);
                    reply_status = filed_status_to_wire(status);
                    if (status == FILED_OK) {
                        const filed_vfs_stat_snapshot_t snapshot =
                            filed_stat_snapshot_from_backend(
                                &backend_stat, opened.handle_id,
                                opened.object_generation, opened.dir_generation);
                        (void)filed_vfs_update_stat_snapshot(
                            &runtime->vfs, object_id, &snapshot);
                        filed_runtime_publish_backend_object_generation(
                            runtime, decision.backend_object);
                        (void)filed_vfs_close_handle(&runtime->vfs, opened.handle_id);
                    }
                }
            }
        }
        filed_close_walk_handle(runtime, dir_handle, dir_owned);
    }
    return filed_page_result(reply_status, object_id);
}

filed_page_dispatch_result_t filed_dispatch_symlink_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_symlink_t *symlink = (filed_symlink_t *)page;
    filed_vfs_io_decision_t decision;
    filed_status_t status = FILED_ERR_INVALID;
    int64_t reply_status = -22;
    uint64_t object_id = 0;
    if (symlink->target_length > 0 &&
        symlink->target_length < sizeof(symlink->target) &&
        symlink->target[symlink->target_length] == '\0' &&
        filed_name_is_terminated(symlink->name, sizeof(symlink->name)))
    {
        filed_handle_id_t dir_handle = 0;
        int dir_owned = 0;
        char name[FILED_NAME_BYTES];
        const filed_handle_id_t base_dir =
            symlink->dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)symlink->dir_handle;

        memset(name, 0, sizeof(name));
        reply_status = filed_resolve_parent_path(
            runtime,
            base_dir,
            symlink->name,
            FILED_RIGHT_CREATE,
            &dir_handle,
            &dir_owned,
            name,
            sizeof(name));
        if (reply_status == 0) {
            status = filed_vfs_create_prepare(&runtime->vfs, dir_handle, name, &decision);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK) {
                reply_status = filed_backend_symlink(
                    runtime,
                    decision.backend_object,
                    name,
                    symlink->target,
                    symlink->target_length,
                    &object_id);
                if (reply_status == 0 && object_id != 0) {
                    filed_cache_invalidate(runtime, decision.backend_object);
                    filed_vfs_open_result_t opened;
                    memset(&opened, 0, sizeof(opened));
                    if (filed_vfs_create_backend_child(
                            &runtime->vfs,
                            dir_handle,
                            object_id,
                            FILED_VNODE_SYMLINK,
                            name,
                            FILED_RIGHT_LOOKUP | FILED_RIGHT_READ | FILED_RIGHT_STAT,
                            FILED_OPEN_NOFOLLOW,
                            &opened) == FILED_OK)
                    {
                        const filed_vfs_stat_snapshot_t snapshot =
                            filed_symlink_snapshot_from_create(
                                opened.handle_id,
                                symlink->target_length,
                                opened.object_generation,
                                opened.dir_generation);
                        (void)filed_vfs_update_stat_snapshot(
                            &runtime->vfs,
                            object_id,
                            &snapshot);
                        filed_runtime_publish_backend_object_generation(
                            runtime,
                            decision.backend_object);
                        (void)filed_vfs_close_handle(&runtime->vfs, opened.handle_id);
                    }
                }
            }
            filed_close_walk_handle(runtime, dir_handle, dir_owned);
        }
    }

    return filed_page_result(reply_status, object_id);
}

filed_page_dispatch_result_t filed_dispatch_readlink_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_readlink_t *readlink = (filed_readlink_t *)page;
    filed_vfs_io_decision_t parent_decision;
    uint64_t object_id = 0;
    storage_statx_reply_t backend_stat;
    int64_t reply_status = -22;
    uint64_t target_length = 0;
    bool lookup_owned = false;
    if (filed_name_is_terminated(readlink->name, sizeof(readlink->name))) {
        filed_handle_id_t dir_handle = 0;
        int dir_owned = 0;
        char name[FILED_NAME_BYTES];
        const filed_handle_id_t base_dir =
            readlink->dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)readlink->dir_handle;

        memset(name, 0, sizeof(name));
        reply_status = filed_resolve_parent_path(
            runtime,
            base_dir,
            readlink->name,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT | FILED_RIGHT_READ,
            &dir_handle,
            &dir_owned,
            name,
            sizeof(name));
        if (reply_status == 0) {
            filed_status_t status = filed_vfs_lookup_prepare(&runtime->vfs, dir_handle, &parent_decision);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK) {
                reply_status = filed_backend_lookup(
                    runtime,
                    parent_decision.backend_object,
                    name,
                    &object_id);
                lookup_owned = reply_status == 0;
            }
            if (reply_status == 0) {
                memset(&backend_stat, 0, sizeof(backend_stat));
                reply_status = filed_backend_statx(runtime, object_id, &backend_stat);
            }
            if (reply_status == 0 &&
                filed_kind_from_unix_type(backend_stat.kind) != FILED_VNODE_SYMLINK)
            {
                reply_status = filed_status_to_wire(FILED_ERR_INVALID);
            }
            if (reply_status == 0) {
                memset(readlink->target, 0, sizeof(readlink->target));
                reply_status = filed_backend_readlink(
                    runtime,
                    object_id,
                    readlink->target,
                    sizeof(readlink->target) - 1u,
                    &target_length);
                if (reply_status == 0) {
                    readlink->target_length = target_length;
                    if (target_length < sizeof(readlink->target)) {
                        readlink->target[target_length] = '\0';
                    }
                }
            }
            if (lookup_owned) {
                (void)filed_backend_release_object(runtime, object_id);
            }
            filed_close_walk_handle(runtime, dir_handle, dir_owned);
        }
    }
    return filed_page_result(reply_status, target_length);
}

filed_page_dispatch_result_t filed_dispatch_link_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_link_t *link = (filed_link_t *)page;
    filed_vfs_io_decision_t old_parent;
    filed_vfs_io_decision_t new_parent;
    filed_vfs_open_result_t opened;
    storage_statx_reply_t backend_stat;
    uint64_t old_object_id = 0;
    uint64_t linked_object_id = 0;
    int64_t reply_status = -22;
    bool old_lookup_owned = false;

    if (filed_name_is_terminated(link->old_name, sizeof(link->old_name)) &&
        filed_name_is_terminated(link->new_name, sizeof(link->new_name)))
    {
        filed_handle_id_t old_dir_handle = 0;
        filed_handle_id_t new_dir_handle = 0;
        int old_dir_owned = 0;
        int new_dir_owned = 0;
        char old_name[FILED_NAME_BYTES];
        char new_name[FILED_NAME_BYTES];
        const filed_handle_id_t old_base_dir =
            link->old_dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)link->old_dir_handle;
        const filed_handle_id_t new_base_dir =
            link->new_dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)link->new_dir_handle;

        memset(old_name, 0, sizeof(old_name));
        memset(new_name, 0, sizeof(new_name));
        reply_status = filed_resolve_parent_path(
            runtime,
            old_base_dir,
            link->old_name,
            FILED_RIGHT_LOOKUP | FILED_RIGHT_STAT,
            &old_dir_handle,
            &old_dir_owned,
            old_name,
            sizeof(old_name));
        if (reply_status == 0) {
            reply_status = filed_resolve_parent_path(
                runtime,
                new_base_dir,
                link->new_name,
                FILED_RIGHT_CREATE,
                &new_dir_handle,
                &new_dir_owned,
                new_name,
                sizeof(new_name));
        }
        if (reply_status == 0) {
            filed_status_t status = filed_vfs_link_prepare(
                &runtime->vfs,
                old_dir_handle,
                new_dir_handle,
                old_name,
                new_name,
                &old_parent,
                &new_parent);
            reply_status = filed_status_to_wire(status);
        }
        if (reply_status == 0) {
            reply_status = filed_backend_lookup(
                runtime,
                old_parent.backend_object,
                old_name,
                &old_object_id);
            old_lookup_owned = reply_status == 0;
        }
        if (reply_status == 0) {
            memset(&backend_stat, 0, sizeof(backend_stat));
            reply_status = filed_backend_statx(runtime, old_object_id, &backend_stat);
        }
        if (reply_status == 0) {
            reply_status = filed_backend_link(
                runtime,
                old_object_id,
                new_parent.backend_object,
                new_name,
                &linked_object_id);
        }
        if (reply_status == 0 && linked_object_id != 0) {
            filed_cache_invalidate(runtime, new_parent.backend_object);
            memset(&opened, 0, sizeof(opened));
            filed_status_t status = filed_vfs_link_commit(
                &runtime->vfs,
                new_dir_handle,
                linked_object_id,
                filed_kind_from_unix_type(backend_stat.kind),
                new_name,
                FILED_RIGHT_LOOKUP | FILED_RIGHT_READ | FILED_RIGHT_WRITE | FILED_RIGHT_STAT,
                (filed_kind_from_unix_type(backend_stat.kind) == FILED_VNODE_SYMLINK) ? FILED_OPEN_NOFOLLOW : 0,
                &opened);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK) {
                storage_statx_reply_t linked_stat;
                memset(&linked_stat, 0, sizeof(linked_stat));
                if (filed_backend_statx(runtime, linked_object_id, &linked_stat) == 0) {
                    filed_vfs_stat_snapshot_t snapshot;
                    memset(&snapshot, 0, sizeof(snapshot));
                    snapshot.valid = true;
                    snapshot.handle_id = opened.handle_id;
                    snapshot.mode = linked_stat.mode;
                    snapshot.size = linked_stat.size;
                    snapshot.blocks = linked_stat.blocks;
                    snapshot.nlink = linked_stat.nlink;
                    snapshot.kind = linked_stat.kind;
                    snapshot.times_valid = true;
                    snapshot.atime_sec = linked_stat.atime_sec;
                    snapshot.atime_nsec = linked_stat.atime_nsec;
                    snapshot.mtime_sec = linked_stat.mtime_sec;
                    snapshot.mtime_nsec = linked_stat.mtime_nsec;
                    snapshot.ctime_sec = linked_stat.ctime_sec;
                    snapshot.ctime_nsec = linked_stat.ctime_nsec;
                    snapshot.object_generation = opened.object_generation;
                    snapshot.dir_generation = opened.dir_generation;
                    (void)filed_vfs_update_stat_snapshot(&runtime->vfs, linked_object_id, &snapshot);
                }
                filed_runtime_publish_backend_object_generation(runtime, linked_object_id);
                filed_runtime_publish_backend_object_generation(runtime, new_parent.backend_object);
                (void)filed_vfs_close_handle(&runtime->vfs, opened.handle_id);
            }
        }
        if (new_dir_handle != 0) {
            filed_close_walk_handle(runtime, new_dir_handle, new_dir_owned);
        }
        if (old_dir_handle != 0) {
            filed_close_walk_handle(runtime, old_dir_handle, old_dir_owned);
        }
        if (old_lookup_owned) {
            (void)filed_backend_release_object(runtime, old_object_id);
        }
    }

    return filed_page_result(reply_status, linked_object_id);
}

filed_page_dispatch_result_t filed_dispatch_rmdir_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_rmdir_t *rmdir = (filed_rmdir_t *)page;
    filed_vfs_io_decision_t decision;
    filed_status_t status = FILED_ERR_INVALID;
    int64_t reply_status = -22;
    uint64_t target_object = 0;
    bool target_lookup_owned = false;
    if (rmdir->reserved0 == 0 &&
        filed_name_is_terminated(rmdir->name, sizeof(rmdir->name)))
    {
        filed_handle_id_t dir_handle = 0;
        int dir_owned = 0;
        char name[FILED_NAME_BYTES];
        const filed_handle_id_t base_dir =
            rmdir->dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)rmdir->dir_handle;

        memset(name, 0, sizeof(name));
        reply_status = filed_resolve_parent_path(
            runtime,
            base_dir,
            rmdir->name,
            FILED_RIGHT_REMOVE,
            &dir_handle,
            &dir_owned,
            name,
            sizeof(name));
        if (reply_status == 0) {
            status = filed_vfs_unlink_prepare(&runtime->vfs, dir_handle, name, &decision);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK) {
                target_object = filed_lookup_cache_target_object(
                    runtime,
                    dir_handle,
                    decision.backend_object,
                    name,
                    &target_lookup_owned);
                reply_status = filed_flush_mutated_object(runtime, target_object);
                if (reply_status == 0) {
                    reply_status = filed_backend_rmdir(
                        runtime,
                        decision.backend_object,
                        name);
                }
                if (reply_status == 0) {
                    filed_cache_invalidate(runtime, decision.backend_object);
                    filed_invalidate_mutated_object(runtime, target_object);
                    filed_vfs_reclaim_result_t reclaim;
                    memset(&reclaim, 0, sizeof(reclaim));
                    status = filed_vfs_unlink_commit_ex(&runtime->vfs, dir_handle, name, &reclaim);
                    if (status != FILED_OK) {
                        reply_status = filed_status_to_wire(status);
                    } else {
                        filed_runtime_publish_backend_object_generation(
                            runtime,
                            decision.backend_object);
                        if (target_object != 0) {
                            filed_runtime_publish_backend_object_generation(
                                runtime,
                                target_object);
                        }
                        const int release_status = filed_release_reclaimed_object(runtime, &reclaim);
                        if (release_status != 0) {
                            reply_status = release_status;
                        }
                    }
                }
            }
            if (target_lookup_owned) {
                (void)filed_backend_release_object(runtime, target_object);
            }
            filed_close_walk_handle(runtime, dir_handle, dir_owned);
        }
    }

    return filed_page_result(reply_status, 0);
}

filed_page_dispatch_result_t filed_dispatch_rename_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_rename_t *rename = (filed_rename_t *)page;
    filed_vfs_io_decision_t old_parent;
    filed_vfs_io_decision_t new_parent;
    filed_status_t status = FILED_ERR_INVALID;
    int64_t reply_status = -22;
    uint64_t object_id = 0;
    uint64_t replaced_object_id = 0;
    uint64_t acquired_object_id = 0;
    uint64_t acquired_replaced_object_id = 0;
    bool object_lookup_owned = false;
    bool replaced_lookup_owned = false;
    if (filed_name_is_terminated(rename->old_name, sizeof(rename->old_name)) &&
        filed_name_is_terminated(rename->new_name, sizeof(rename->new_name)))
    {
        filed_handle_id_t old_dir_handle = 0;
        filed_handle_id_t new_dir_handle = 0;
        int old_dir_owned = 0;
        int new_dir_owned = 0;
        char old_name[FILED_NAME_BYTES];
        char new_name[FILED_NAME_BYTES];
        const filed_handle_id_t old_base_dir =
            rename->old_dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)rename->old_dir_handle;
        const filed_handle_id_t new_base_dir =
            rename->new_dir_handle == 0 ?
                runtime->root_handle_id :
                (filed_handle_id_t)(uint32_t)rename->new_dir_handle;

        memset(old_name, 0, sizeof(old_name));
        memset(new_name, 0, sizeof(new_name));
        reply_status = filed_resolve_parent_path(
            runtime,
            old_base_dir,
            rename->old_name,
            FILED_RIGHT_RENAME,
            &old_dir_handle,
            &old_dir_owned,
            old_name,
            sizeof(old_name));
        if (reply_status == 0) {
            reply_status = filed_resolve_parent_path(
                runtime,
                new_base_dir,
                rename->new_name,
                FILED_RIGHT_RENAME,
                &new_dir_handle,
                &new_dir_owned,
                new_name,
                sizeof(new_name));
        }
        if (reply_status == 0) {
            status = filed_vfs_rename_prepare(
                &runtime->vfs,
                old_dir_handle,
                new_dir_handle,
                old_name,
                new_name,
                &old_parent,
                &new_parent);
            reply_status = filed_status_to_wire(status);
            if (status == FILED_OK) {
                replaced_object_id = filed_lookup_cache_target_object(
                    runtime,
                    new_dir_handle,
                    new_parent.backend_object,
                    new_name,
                    &replaced_lookup_owned);
                object_id = filed_lookup_cache_target_object(
                    runtime,
                    old_dir_handle,
                    old_parent.backend_object,
                    old_name,
                    &object_lookup_owned);
                acquired_object_id = object_id;
                acquired_replaced_object_id = replaced_object_id;
                if (replaced_object_id != 0 &&
                    replaced_object_id != object_id)
                {
                    reply_status = filed_flush_mutated_object(runtime, replaced_object_id);
                }
                if (reply_status == 0) {
                    reply_status = filed_backend_rename(
                        runtime,
                        old_parent.backend_object,
                        old_name,
                        new_parent.backend_object,
                        new_name,
                        &object_id);
                }
                if (reply_status == 0) {
                    filed_cache_invalidate(runtime, old_parent.backend_object);
                    if (new_parent.backend_object != old_parent.backend_object) {
                        filed_cache_invalidate(runtime, new_parent.backend_object);
                    }
                    if (replaced_object_id != 0 && replaced_object_id != object_id) {
                        filed_invalidate_mutated_object(runtime, replaced_object_id);
                    }
                    filed_vfs_reclaim_result_t reclaim;
                    memset(&reclaim, 0, sizeof(reclaim));
                    status = filed_vfs_rename_commit_ex(
                        &runtime->vfs,
                        old_dir_handle,
                        new_dir_handle,
                        old_name,
                        new_name,
                        object_id,
                        &reclaim);
                    if (status != FILED_OK) {
                        reply_status = filed_status_to_wire(status);
                    } else {
                        filed_runtime_publish_backend_object_generation(
                            runtime,
                            old_parent.backend_object);
                        if (new_parent.backend_object != old_parent.backend_object) {
                            filed_runtime_publish_backend_object_generation(
                                runtime,
                                new_parent.backend_object);
                        }
                        if (object_id != 0) {
                            filed_runtime_publish_backend_object_generation(
                                runtime,
                                object_id);
                        }
                        if (replaced_object_id != 0 && replaced_object_id != object_id) {
                            filed_runtime_publish_backend_object_generation(
                                runtime,
                                replaced_object_id);
                        }
                        const int release_status = filed_release_reclaimed_object(runtime, &reclaim);
                        if (release_status != 0) {
                            reply_status = release_status;
                        }
                    }
                }
            }
        }
        filed_close_walk_handle(runtime, old_dir_handle, old_dir_owned);
        filed_close_walk_handle(runtime, new_dir_handle, new_dir_owned);
        if (object_lookup_owned) {
            (void)filed_backend_release_object(runtime, acquired_object_id);
        }
        if (replaced_lookup_owned) {
            (void)filed_backend_release_object(runtime, acquired_replaced_object_id);
        }
    }

    return filed_page_result(reply_status, object_id);
}

filed_page_dispatch_result_t filed_dispatch_getdents_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_getdents_t *out = (filed_getdents_t *)page;
    storage_getdents_request_t backend_entries;
    filed_vfs_io_decision_t decision;
    filed_status_t status = filed_vfs_getdents_prepare(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)out->dir_handle,
        &decision);
    bool splice_tmpfs = false;
    uint64_t backend_offset = 0;
    if (status == FILED_OK) {
        splice_tmpfs = filed_root_getdents_splices_tmpfs(runtime, decision.backend_object);
        backend_offset = filed_root_getdents_backend_offset(
            runtime,
            decision.backend_object,
            decision.offset);
    }
    int64_t reply_status = filed_status_to_wire(status);
    uint64_t result = 0;
    memset(&backend_entries, 0, sizeof(backend_entries));
    backend_entries.capacity = FILED_DIRENT_CAPACITY;
    if (status == FILED_OK &&
        !filed_dir_cache_get(runtime, decision.backend_object, backend_offset, &backend_entries))
    {
        reply_status = filed_backend_getdents(
            runtime,
            decision.backend_object,
            backend_offset,
            &backend_entries);
        if (reply_status == 0) {
            filed_dir_cache_store(runtime, decision.backend_object, backend_offset, &backend_entries);
        }
    }
    if (reply_status == 0) {
        const uint64_t backend_start = splice_tmpfs && decision.offset == 0 ? 1u : 0u;
        const uint64_t backend_capacity = FILED_DIRENT_CAPACITY - backend_start;
        const uint64_t backend_count =
            backend_entries.count > backend_capacity ?
                backend_capacity :
                backend_entries.count;
        const uint64_t count = backend_start + backend_count;
        const uint64_t dir_handle = out->dir_handle;
        memset(out, 0, sizeof(*out));
        out->dir_handle = dir_handle;
        out->offset = decision.offset;
        out->count = count;
        out->dir_generation = decision.dir_generation;
        if (backend_start != 0) {
            out->entries[0].handle = 0;
            out->entries[0].kind = 0040000u;
            out->entries[0].name_len = 3;
            snprintf(
                out->entries[0].name,
                sizeof(out->entries[0].name),
                "%s",
                "tmp");
        }
        for (uint64_t i = 0; i < backend_count; ++i) {
            const uint64_t out_index = backend_start + i;
            out->entries[out_index].handle = 0;
            out->entries[out_index].kind = backend_entries.entries[i].kind;
            out->entries[out_index].name_len = backend_entries.entries[i].name_len;
            snprintf(
                out->entries[out_index].name,
                sizeof(out->entries[out_index].name),
                "%s",
                backend_entries.entries[i].name);
        }
        result = count;
        status = filed_vfs_getdents_commit(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)dir_handle,
            count);
        if (status != FILED_OK) {
            reply_status = filed_status_to_wire(status);
        } else {
            filed_runtime_publish_generation(
                runtime,
                (filed_handle_id_t)(uint32_t)dir_handle,
                decision.object_generation,
                decision.dir_generation);
        }
    }
    return filed_page_result(reply_status, result);
}

filed_page_dispatch_result_t filed_dispatch_close_page(
    filed_runtime_t *runtime,
    const struct pacha_ipc_msg *request)
{
    const filed_handle_id_t handle_id = (filed_handle_id_t)(uint32_t)request->word2;
    if (handle_id == runtime->root_handle_id) {
        return filed_page_result(-13, 0);
    }
    return filed_page_result(filed_close_handle_runtime(runtime, handle_id), 0);
}

filed_page_dispatch_result_t filed_dispatch_dup_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_handle_flags_t *wire_flags = (filed_handle_flags_t *)page;
    filed_handle_id_t dup_handle = 0;
    int64_t reply_status = -22;
    uint64_t result = 0;
    if (wire_flags->reserved0 == 0 &&
        filed_flags_are_known(wire_flags->fd_flags, wire_flags->status_flags))
    {
        const filed_status_t status = filed_vfs_dup_handle(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)wire_flags->handle,
            filed_fd_flags_to_vfs(wire_flags->fd_flags),
            &dup_handle);
        reply_status = filed_status_to_wire(status);
        if (status == FILED_OK) {
            wire_flags->handle = dup_handle;
            result = dup_handle;
            filed_vfs_io_decision_t decision;
            if (filed_vfs_stat_prepare(&runtime->vfs, dup_handle, &decision) == FILED_OK) {
                filed_runtime_publish_generation(
                    runtime,
                    dup_handle,
                    decision.object_generation,
                    decision.dir_generation);
            }
        }
    }
    return filed_page_result(reply_status, result);
}

filed_page_dispatch_result_t filed_dispatch_get_flags_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_handle_flags_t *wire_flags = (filed_handle_flags_t *)page;
    filed_vfs_handle_flags_t flags;
    const uint64_t handle = wire_flags->handle;
    uint64_t result = 0;
    filed_status_t status;
    memset(&flags, 0, sizeof(flags));
    status = filed_vfs_get_handle_flags(
        &runtime->vfs,
        (filed_handle_id_t)(uint32_t)handle,
        &flags);
    if (status == FILED_OK) {
        memset(wire_flags, 0, sizeof(*wire_flags));
        wire_flags->handle = handle;
        wire_flags->fd_flags = filed_vfs_fd_flags_to_wire(flags.fd_flags);
        wire_flags->status_flags =
            filed_vfs_file_status_flags_to_wire(flags.status_flags);
        result = wire_flags->fd_flags;
    }
    return filed_page_result(filed_status_to_wire(status), result);
}

filed_page_dispatch_result_t filed_dispatch_set_flags_page(
    filed_runtime_t *runtime,
    void *page)
{
    filed_handle_flags_t *wire_flags = (filed_handle_flags_t *)page;
    filed_vfs_handle_flags_t flags;
    filed_status_t status = FILED_ERR_INVALID;
    if (wire_flags->reserved0 == 0 &&
        filed_flags_are_known(wire_flags->fd_flags, wire_flags->status_flags))
    {
        flags.fd_flags = filed_fd_flags_to_vfs(wire_flags->fd_flags);
        flags.status_flags =
            filed_file_status_flags_to_vfs(wire_flags->status_flags);
        status = filed_vfs_set_handle_flags(
            &runtime->vfs,
            (filed_handle_id_t)(uint32_t)wire_flags->handle,
            &flags);
    }
    return filed_page_result(filed_status_to_wire(status), 0);
}
