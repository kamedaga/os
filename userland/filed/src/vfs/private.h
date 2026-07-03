#pragma once

#include "filed/vfs.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

void filed_lock_init(filed_lock_t *lock);
void filed_lock_acquire(filed_lock_t *lock);
void filed_lock_release(filed_lock_t *lock);
filed_lock_t *filed_mutable_lock(const filed_lock_t *lock);

void filed_vnode_write_lock(filed_vnode_t *vnode);
void filed_vnode_write_unlock(filed_vnode_t *vnode);
bool filed_vnode_lock_before(const filed_vnode_t *a, const filed_vnode_t *b);
void filed_vnode_write_lock_pair(filed_vnode_t *a, filed_vnode_t *b);
void filed_vnode_write_unlock_pair(filed_vnode_t *a, filed_vnode_t *b);

filed_status_t filed_copy_name(char *dst, size_t dst_size, const char *src);
bool filed_name_is_dot_or_dotdot(const char *name);
bool filed_name_is_component(const char *name);

bool filed_fd_flags_are_known(uint32_t flags);
bool filed_file_status_flags_are_known(uint32_t flags);

void filed_vnode_init_lock(filed_vnode_t *vnode);
filed_status_t filed_vnode_ref_inc(filed_vnode_t *vnode);
uint32_t filed_vnode_ref_dec_if_nonzero(filed_vnode_t *vnode);
bool filed_vnode_mark_unlinked(filed_vnode_t *vnode);
void filed_vnode_bump_object_generation_locked(filed_vnode_t *vnode);
void filed_vnode_bump_dir_generation_locked(filed_vnode_t *vnode);

void filed_file_init_locks(filed_file_t *file);
filed_status_t filed_file_ref_inc(filed_file_t *file);
uint32_t filed_file_ref_dec_if_nonzero(filed_file_t *file);
filed_status_t filed_file_offset_snapshot(const filed_file_t *file, uint64_t *out_offset);
filed_status_t filed_file_offset_advance(filed_file_t *file, uint64_t amount);
uint32_t filed_file_status_flags_snapshot(const filed_file_t *file);

filed_mount_t *filed_alloc_mount(filed_vfs_t *vfs);
filed_vnode_t *filed_alloc_vnode(filed_vfs_t *vfs);
filed_file_t *filed_alloc_file(filed_vfs_t *vfs);
filed_handle_t *filed_alloc_handle(filed_vfs_t *vfs);

size_t filed_id_hint_index(uint32_t id);
void filed_remember_vnode_slot(filed_vfs_t *vfs, const filed_vnode_t *vnode);
void filed_remember_file_slot(filed_vfs_t *vfs, const filed_file_t *file);
void filed_remember_handle_slot(filed_vfs_t *vfs, const filed_handle_t *handle);

bool filed_mount_id_exists(const filed_vfs_t *vfs, filed_mount_id_t id);
bool filed_vnode_id_exists(const filed_vfs_t *vfs, filed_vnode_id_t id);
filed_mount_t *filed_find_mount(filed_vfs_t *vfs, filed_mount_id_t id);
filed_vnode_t *filed_find_vnode(filed_vfs_t *vfs, filed_vnode_id_t id);
const filed_vnode_t *filed_find_vnode_const(const filed_vfs_t *vfs, filed_vnode_id_t id);
filed_file_t *filed_find_file(filed_vfs_t *vfs, filed_file_id_t id);
const filed_file_t *filed_find_file_const(const filed_vfs_t *vfs, filed_file_id_t id);
filed_handle_t *filed_find_handle(filed_vfs_t *vfs, filed_handle_id_t id);
const filed_handle_t *filed_find_handle_const(const filed_vfs_t *vfs, filed_handle_id_t id);
filed_vnode_t *filed_find_backend_vnode(
    filed_vfs_t *vfs,
    filed_mount_id_t mount_id,
    filed_backend_object_id_t backend_object,
    filed_vnode_id_t parent,
    const char *name);
filed_vnode_t *filed_find_backend_object_vnode(
    filed_vfs_t *vfs,
    filed_backend_object_id_t backend_object);
uint32_t filed_vnode_mount_pins(const filed_vfs_t *vfs, filed_vnode_id_t vnode_id);
bool filed_vnode_is_dead(const filed_vfs_t *vfs, const filed_vnode_t *vnode);
