#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "filed/tmpfs_backend.h"
#include "filed/vfs.h"

enum {
    /* /tmp, /run, /dev/shm and memfd share this bounded metadata pool.
     * Desktop + WebKit exhausted 128 inodes with data pages still free. */
    FILED_TMPFS_MAX_INODES = 256,
    FILED_TMPFS_MAX_DENTRIES = 512,
    /* Bounded backing budget, allocated only for nonzero written pages.
     * Browser temporary files alone exceeded the old 16 MiB global pool. */
    FILED_TMPFS_PAGE_POOL_PAGES = 16384,
    FILED_TMPFS_CHILD_HASH_BUCKETS = 256,
    FILED_TMPFS_NO_SLOT = 0xffffu,
};

typedef struct filed_tmpfs_page {
    bool used;
    uint16_t next_inode_page;
    uint32_t file_page_index;
    uint8_t *data;
} filed_tmpfs_page_t;

typedef struct filed_tmpfs_inode {
    bool used;
    uint16_t slot_index;
    uint16_t primary_dentry_slot;
    uint16_t first_child_dentry_slot;
    uint64_t object_id;
    uint64_t mode;
    uint64_t size;
    uint64_t generation;
    uint64_t rdev;
    uint32_t nlink;
    filed_vnode_kind_t kind;
    uint16_t allocated_page_count;
    /* Page ownership metadata belongs to the global page pool, not a large
     * fixed array per inode. A single file may use the existing 64 MiB pool. */
    uint16_t first_allocated_page;
} filed_tmpfs_inode_t;

typedef struct filed_tmpfs_dentry {
    bool used;
    bool linked;
    uint16_t slot_index;
    uint16_t parent_inode_slot;
    uint16_t inode_slot;
    uint16_t next_sibling_slot;
    uint16_t hash_next_slot;
    char name[FILED_TMPFS_NAME_BYTES];
} filed_tmpfs_dentry_t;

struct filed_tmpfs_backend {
    filed_lock_t lock;
    uint64_t root_object_id;
    uint64_t next_object_generation;
    uint32_t free_inode_count;
    uint32_t free_dentry_count;
    uint32_t free_page_count;
    uint16_t free_inode_stack[FILED_TMPFS_MAX_INODES];
    uint16_t free_dentry_stack[FILED_TMPFS_MAX_DENTRIES];
    uint16_t free_page_stack[FILED_TMPFS_PAGE_POOL_PAGES];
    uint16_t child_hash_buckets[FILED_TMPFS_CHILD_HASH_BUCKETS];
    filed_tmpfs_inode_t inodes[FILED_TMPFS_MAX_INODES];
    filed_tmpfs_dentry_t dentries[FILED_TMPFS_MAX_DENTRIES];
    filed_tmpfs_page_t pages[FILED_TMPFS_PAGE_POOL_PAGES];
};
