#pragma once

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "filed/tmpfs_internal.h"

enum {
    FILED_TMPFS_OBJECT_TAG = 0x8000000000000000ull,
    FILED_TMPFS_OBJECT_SLOT_MASK = 0xffffu,
    FILED_TMPFS_ROOT_OBJECT_GENERATION = 1u,
    FILED_TMPFS_MODE_TYPE_MASK = 0170000u,
    FILED_TMPFS_MODE_REGULAR = 0100000u,
    FILED_TMPFS_MODE_DIRECTORY = 0040000u,
    FILED_TMPFS_MODE_SYMLINK = 0120000u,
};

void filed_tmpfs_lock_acquire(filed_lock_t *lock);
void filed_tmpfs_lock_release(filed_lock_t *lock);

uint64_t filed_tmpfs_make_object_id(uint16_t slot, uint64_t object_generation);
uint64_t filed_tmpfs_mode_for_kind(filed_vnode_kind_t kind, uint64_t mode);
int filed_tmpfs_name_valid(const char *name);

uint16_t filed_tmpfs_alloc_page(filed_tmpfs_backend_t *backend);
filed_tmpfs_page_t *filed_tmpfs_page_by_id(filed_tmpfs_backend_t *backend, uint16_t page_id);
void filed_tmpfs_free_page(filed_tmpfs_backend_t *backend, uint16_t page_id);
uint16_t filed_tmpfs_inode_page_id(const filed_tmpfs_inode_t *inode, uint64_t page_index);
int filed_tmpfs_note_inode_page(
    filed_tmpfs_inode_t *inode,
    uint64_t page_index,
    uint16_t page_id);
void filed_tmpfs_free_inode_pages(filed_tmpfs_backend_t *backend, filed_tmpfs_inode_t *inode, uint64_t first_page);

filed_tmpfs_inode_t *filed_tmpfs_find_inode(filed_tmpfs_backend_t *backend, uint64_t object_id);
uint16_t filed_tmpfs_inode_slot(const filed_tmpfs_backend_t *backend, const filed_tmpfs_inode_t *inode);
filed_tmpfs_inode_t *filed_tmpfs_inode_by_slot(filed_tmpfs_backend_t *backend, uint16_t slot);
filed_tmpfs_inode_t *filed_tmpfs_alloc_inode(filed_tmpfs_backend_t *backend);
void filed_tmpfs_free_inode_if_dead(filed_tmpfs_backend_t *backend, filed_tmpfs_inode_t *inode);

filed_tmpfs_dentry_t *filed_tmpfs_dentry_by_slot(filed_tmpfs_backend_t *backend, uint16_t slot);
filed_tmpfs_dentry_t *filed_tmpfs_alloc_dentry(filed_tmpfs_backend_t *backend);
filed_tmpfs_dentry_t *filed_tmpfs_find_child_dentry(
    filed_tmpfs_backend_t *backend,
    filed_tmpfs_inode_t *parent,
    const char *name);
filed_tmpfs_inode_t *filed_tmpfs_find_child_inode(
    filed_tmpfs_backend_t *backend,
    filed_tmpfs_inode_t *parent,
    const char *name);
void filed_tmpfs_link_dentry_locked(
    filed_tmpfs_backend_t *backend,
    filed_tmpfs_inode_t *parent,
    filed_tmpfs_inode_t *inode,
    filed_tmpfs_dentry_t *dentry,
    const char *name);
void filed_tmpfs_unlink_dentry_locked(
    filed_tmpfs_backend_t *backend,
    filed_tmpfs_inode_t *parent,
    filed_tmpfs_dentry_t *dentry);
int filed_tmpfs_directory_empty_locked(
    const filed_tmpfs_backend_t *backend,
    uint64_t object_id);
uint64_t filed_tmpfs_dir_nlink_locked(
    const filed_tmpfs_backend_t *backend,
    uint64_t object_id);
