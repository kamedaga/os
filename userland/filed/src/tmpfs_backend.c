#include "filed/tmpfs_backend.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum {
    FILED_TMPFS_OBJECT_TAG = 0x8000000000000000ull,
    FILED_TMPFS_OBJECT_SLOT_MASK = 0xffffu,
    FILED_TMPFS_ROOT_OBJECT_GENERATION = 1u,
    FILED_TMPFS_MODE_TYPE_MASK = 0170000u,
    FILED_TMPFS_MODE_REGULAR = 0100000u,
    FILED_TMPFS_MODE_DIRECTORY = 0040000u,
};

static void filed_tmpfs_lock_acquire(filed_lock_t *lock)
{
    if (lock == NULL) {
        return;
    }
    while (atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire)) {
    }
}

static void filed_tmpfs_lock_release(filed_lock_t *lock)
{
    if (lock == NULL) {
        return;
    }
    atomic_flag_clear_explicit(&lock->flag, memory_order_release);
}

bool filed_tmpfs_backend_is_object(uint64_t object_id)
{
    return (object_id & FILED_TMPFS_OBJECT_TAG) != 0;
}

static uint64_t filed_tmpfs_make_object_id(uint16_t slot, uint64_t object_generation)
{
    if (slot >= FILED_TMPFS_MAX_NODES) {
        return 0;
    }
    if (object_generation == 0) {
        object_generation = 1;
    }
    return FILED_TMPFS_OBJECT_TAG |
        ((object_generation & 0x00007fffffffffffull) << 16) |
        ((uint64_t)slot + 1u);
}

static uint64_t filed_tmpfs_mode_for_kind(filed_vnode_kind_t kind, uint64_t mode)
{
    const uint64_t perms = mode & 07777u;
    if (kind == FILED_VNODE_DIRECTORY) {
        return FILED_TMPFS_MODE_DIRECTORY | (perms == 0 ? 0755u : perms);
    }
    return FILED_TMPFS_MODE_REGULAR | (perms == 0 ? 0644u : perms);
}

static int filed_tmpfs_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    for (size_t i = 0; name[i] != '\0'; ++i) {
        if (name[i] == '/' || i + 1u >= FILED_TMPFS_NAME_BYTES) {
            return 0;
        }
    }
    return 1;
}

static uint16_t filed_tmpfs_alloc_page(filed_tmpfs_backend_t *backend)
{
    if (backend == NULL || backend->free_page_count == 0) {
        return 0;
    }
    const uint16_t page_id = backend->free_page_stack[--backend->free_page_count];
    if (page_id == 0 || page_id > FILED_TMPFS_PAGE_POOL_PAGES) {
        return 0;
    }
    filed_tmpfs_page_t *page = &backend->pages[(size_t)page_id - 1u];
    if (page->used) {
        return 0;
    }
    memset(page, 0, sizeof(*page));
    page->used = true;
    return page_id;
}

static filed_tmpfs_page_t *filed_tmpfs_page_by_id(filed_tmpfs_backend_t *backend, uint16_t page_id)
{
    if (backend == NULL || page_id == 0 || page_id > FILED_TMPFS_PAGE_POOL_PAGES) {
        return NULL;
    }
    filed_tmpfs_page_t *page = &backend->pages[(size_t)page_id - 1u];
    return page->used ? page : NULL;
}

static void filed_tmpfs_free_page(filed_tmpfs_backend_t *backend, uint16_t page_id)
{
    if (backend == NULL || page_id == 0 || page_id > FILED_TMPFS_PAGE_POOL_PAGES) {
        return;
    }
    const uint32_t index = (uint32_t)page_id - 1u;
    if (!backend->pages[index].used) {
        return;
    }
    memset(&backend->pages[index], 0, sizeof(backend->pages[0]));
    if (backend->free_page_count < FILED_TMPFS_PAGE_POOL_PAGES) {
        backend->free_page_stack[backend->free_page_count++] = page_id;
    }
}

static int filed_tmpfs_note_node_page(filed_tmpfs_node_t *node, uint64_t page_index)
{
    if (node == NULL || page_index >= FILED_TMPFS_NODE_MAX_PAGES) {
        return 0;
    }
    if (node->allocated_page_count >= FILED_TMPFS_NODE_MAX_PAGES) {
        return 0;
    }
    node->allocated_page_indices[node->allocated_page_count++] = (uint16_t)page_index;
    return 1;
}

static void filed_tmpfs_free_node_pages(filed_tmpfs_backend_t *backend, filed_tmpfs_node_t *node, uint64_t first_page)
{
    if (node == NULL) {
        return;
    }
    if (first_page > FILED_TMPFS_NODE_MAX_PAGES) {
        first_page = FILED_TMPFS_NODE_MAX_PAGES;
    }
    uint16_t i = 0;
    while (i < node->allocated_page_count) {
        const uint16_t page_index = node->allocated_page_indices[i];
        if (page_index >= first_page && page_index < FILED_TMPFS_NODE_MAX_PAGES) {
            filed_tmpfs_free_page(backend, node->pages[page_index]);
            node->pages[page_index] = 0;
            node->allocated_page_indices[i] = node->allocated_page_indices[--node->allocated_page_count];
        } else {
            ++i;
        }
    }
}

static filed_tmpfs_node_t *filed_tmpfs_node_by_slot(filed_tmpfs_backend_t *backend, uint16_t slot);

static filed_tmpfs_node_t *filed_tmpfs_find_node(filed_tmpfs_backend_t *backend, uint64_t object_id)
{
    if (backend == NULL || object_id == 0 || !filed_tmpfs_backend_is_object(object_id)) {
        return NULL;
    }
    const uint16_t encoded_slot = (uint16_t)(object_id & FILED_TMPFS_OBJECT_SLOT_MASK);
    if (encoded_slot == 0) {
        return NULL;
    }
    const uint16_t slot = (uint16_t)(encoded_slot - 1u);
    filed_tmpfs_node_t *node = filed_tmpfs_node_by_slot(backend, slot);
    if (node == NULL || node->object_id != object_id) {
        return NULL;
    }
    return node;
}

static uint16_t filed_tmpfs_node_slot(const filed_tmpfs_backend_t *backend, const filed_tmpfs_node_t *node)
{
    if (backend == NULL || node == NULL) {
        return FILED_TMPFS_NO_NODE;
    }
    const uintptr_t base = (uintptr_t)&backend->nodes[0];
    const uintptr_t ptr = (uintptr_t)node;
    const uintptr_t end = (uintptr_t)&backend->nodes[FILED_TMPFS_MAX_NODES];
    if (ptr < base || ptr >= end) {
        return FILED_TMPFS_NO_NODE;
    }
    return (uint16_t)((ptr - base) / sizeof(backend->nodes[0]));
}

static filed_tmpfs_node_t *filed_tmpfs_node_by_slot(filed_tmpfs_backend_t *backend, uint16_t slot)
{
    if (backend == NULL || slot >= FILED_TMPFS_MAX_NODES) {
        return NULL;
    }
    filed_tmpfs_node_t *node = &backend->nodes[slot];
    return node->used ? node : NULL;
}

static uint32_t filed_tmpfs_child_hash(uint16_t parent_slot, const char *name)
{
    uint32_t hash = 2166136261u ^ parent_slot;
    if (name != NULL) {
        for (size_t i = 0; name[i] != '\0'; ++i) {
            hash ^= (uint8_t)name[i];
            hash *= 16777619u;
        }
    }
    return hash % FILED_TMPFS_CHILD_HASH_BUCKETS;
}

static filed_tmpfs_node_t *filed_tmpfs_find_child(
    filed_tmpfs_backend_t *backend,
    uint64_t parent_object_id,
    const char *name)
{
    if (backend == NULL || parent_object_id == 0 || name == NULL) {
        return NULL;
    }
    filed_tmpfs_node_t *parent = filed_tmpfs_find_node(backend, parent_object_id);
    if (parent == NULL || parent->kind != FILED_VNODE_DIRECTORY) {
        return NULL;
    }
    const uint16_t parent_slot = filed_tmpfs_node_slot(backend, parent);
    if (parent_slot == FILED_TMPFS_NO_NODE) {
        return NULL;
    }
    uint16_t slot = backend->child_hash_buckets[filed_tmpfs_child_hash(parent_slot, name)];
    while (slot != FILED_TMPFS_NO_NODE) {
        filed_tmpfs_node_t *node = filed_tmpfs_node_by_slot(backend, slot);
        if (node == NULL) {
            return NULL;
        }
        if (node->linked && node->parent_slot == parent_slot && strcmp(node->name, name) == 0) {
            return node;
        }
        slot = node->hash_next_slot;
    }
    return NULL;
}

static filed_tmpfs_node_t *filed_tmpfs_alloc_node(filed_tmpfs_backend_t *backend)
{
    if (backend == NULL || backend->free_node_count == 0) {
        return NULL;
    }
    const uint16_t slot = backend->free_node_stack[--backend->free_node_count];
    if (slot == 0 || slot >= FILED_TMPFS_MAX_NODES || backend->nodes[slot].used) {
        return NULL;
    }
    return &backend->nodes[slot];
}

static void filed_tmpfs_link_child_locked(filed_tmpfs_backend_t *backend, filed_tmpfs_node_t *parent, filed_tmpfs_node_t *child)
{
    if (backend == NULL || parent == NULL || child == NULL) {
        return;
    }
    child->parent_slot = filed_tmpfs_node_slot(backend, parent);
    child->parent_object_id = parent->object_id;
    child->next_sibling_slot = parent->first_child_slot;
    parent->first_child_slot = child->slot_index;
    const uint32_t bucket = filed_tmpfs_child_hash(child->parent_slot, child->name);
    child->hash_next_slot = backend->child_hash_buckets[bucket];
    backend->child_hash_buckets[bucket] = child->slot_index;
}

static void filed_tmpfs_unlink_child_locked(filed_tmpfs_backend_t *backend, filed_tmpfs_node_t *parent, filed_tmpfs_node_t *child)
{
    if (backend == NULL || parent == NULL || child == NULL) {
        return;
    }
    uint16_t *link = &parent->first_child_slot;
    while (*link != FILED_TMPFS_NO_NODE) {
        filed_tmpfs_node_t *node = filed_tmpfs_node_by_slot(backend, *link);
        if (node == NULL) {
            *link = FILED_TMPFS_NO_NODE;
            break;
        }
        if (node == child) {
            *link = child->next_sibling_slot;
            child->next_sibling_slot = FILED_TMPFS_NO_NODE;
            child->parent_slot = FILED_TMPFS_NO_NODE;
            child->parent_object_id = 0;
            break;
        }
        link = &node->next_sibling_slot;
    }
    const uint16_t parent_slot = filed_tmpfs_node_slot(backend, parent);
    if (parent_slot != FILED_TMPFS_NO_NODE) {
        uint16_t *hash_link = &backend->child_hash_buckets[filed_tmpfs_child_hash(parent_slot, child->name)];
        while (*hash_link != FILED_TMPFS_NO_NODE) {
            filed_tmpfs_node_t *node = filed_tmpfs_node_by_slot(backend, *hash_link);
            if (node == NULL) {
                *hash_link = FILED_TMPFS_NO_NODE;
                break;
            }
            if (node == child) {
                *hash_link = child->hash_next_slot;
                child->hash_next_slot = FILED_TMPFS_NO_NODE;
                break;
            }
            hash_link = &node->hash_next_slot;
        }
    }
}

static int filed_tmpfs_directory_empty_locked(
    const filed_tmpfs_backend_t *backend,
    uint64_t object_id)
{
    filed_tmpfs_node_t *dir = filed_tmpfs_find_node((filed_tmpfs_backend_t *)backend, object_id);
    if (dir == NULL || dir->kind != FILED_VNODE_DIRECTORY) {
        return 0;
    }
    return dir->first_child_slot == FILED_TMPFS_NO_NODE;
}

static uint64_t filed_tmpfs_dir_nlink_locked(
    const filed_tmpfs_backend_t *backend,
    uint64_t object_id)
{
    uint64_t nlink = 2;
    filed_tmpfs_node_t *dir = filed_tmpfs_find_node((filed_tmpfs_backend_t *)backend, object_id);
    if (dir == NULL || dir->kind != FILED_VNODE_DIRECTORY) {
        return 0;
    }
    uint16_t slot = dir->first_child_slot;
    while (slot != FILED_TMPFS_NO_NODE) {
        const filed_tmpfs_node_t *node = filed_tmpfs_node_by_slot((filed_tmpfs_backend_t *)backend, slot);
        if (node == NULL) {
            break;
        }
        if (node->linked && node->kind == FILED_VNODE_DIRECTORY) {
            ++nlink;
        }
        slot = node->next_sibling_slot;
    }
    return nlink;
}

void filed_tmpfs_backend_init(filed_tmpfs_backend_t *backend)
{
    if (backend == NULL) {
        return;
    }
    memset(backend, 0, sizeof(*backend));
    atomic_flag_clear(&backend->lock.flag);
    backend->root_object_id = filed_tmpfs_make_object_id(0, FILED_TMPFS_ROOT_OBJECT_GENERATION);
    backend->next_object_generation = FILED_TMPFS_ROOT_OBJECT_GENERATION + 1u;
    backend->free_node_count = 0;
    backend->free_page_count = FILED_TMPFS_PAGE_POOL_PAGES;
    for (size_t i = 1; i < FILED_TMPFS_MAX_NODES; ++i) {
        backend->free_node_stack[backend->free_node_count++] = (uint16_t)(FILED_TMPFS_MAX_NODES - i);
    }
    for (size_t i = 0; i < FILED_TMPFS_PAGE_POOL_PAGES; ++i) {
        backend->free_page_stack[i] = (uint16_t)(FILED_TMPFS_PAGE_POOL_PAGES - i);
    }
    for (size_t i = 0; i < FILED_TMPFS_CHILD_HASH_BUCKETS; ++i) {
        backend->child_hash_buckets[i] = FILED_TMPFS_NO_NODE;
    }

    filed_tmpfs_node_t *root = &backend->nodes[0];
    root->used = true;
    root->linked = true;
    root->slot_index = 0;
    root->parent_slot = FILED_TMPFS_NO_NODE;
    root->first_child_slot = FILED_TMPFS_NO_NODE;
    root->next_sibling_slot = FILED_TMPFS_NO_NODE;
    root->hash_next_slot = FILED_TMPFS_NO_NODE;
    root->object_id = backend->root_object_id;
    root->parent_object_id = 0;
    root->mode = FILED_TMPFS_MODE_DIRECTORY | 0755u;
    root->generation = 1;
    root->kind = FILED_VNODE_DIRECTORY;
    snprintf(root->name, sizeof(root->name), "%s", "/");
}

int filed_tmpfs_backend_mount_root(filed_tmpfs_backend_t *backend, uint64_t *out_root_object_id)
{
    if (backend == NULL || out_root_object_id == NULL) {
        return -22;
    }
    *out_root_object_id = backend->root_object_id;
    return 0;
}

int filed_tmpfs_backend_lookup(filed_tmpfs_backend_t *backend, uint64_t parent_object_id, const char *name, uint64_t *out_object_id)
{
    if (backend == NULL || name == NULL || out_object_id == NULL) {
        return -22;
    }
    *out_object_id = 0;
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_node_t *parent = filed_tmpfs_find_node(backend, parent_object_id);
    if (parent == NULL || !parent->linked) {
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
        *out_object_id = parent->parent_object_id != 0 ? parent->parent_object_id : parent->object_id;
        filed_tmpfs_lock_release(&backend->lock);
        return 0;
    }
    filed_tmpfs_node_t *child = filed_tmpfs_find_child(backend, parent_object_id, name);
    if (child == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    *out_object_id = child->object_id;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_statx(filed_tmpfs_backend_t *backend, uint64_t object_id, koboxd_wire_fs_statx_t *out_stat)
{
    if (backend == NULL || out_stat == NULL) {
        return -22;
    }
    memset(out_stat, 0, sizeof(*out_stat));
    filed_tmpfs_lock_acquire(&backend->lock);
    const filed_tmpfs_node_t *node = filed_tmpfs_find_node(backend, object_id);
    if (node == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    out_stat->object_id = node->object_id;
    out_stat->mode = node->mode;
    out_stat->size = node->size;
    out_stat->blocks = (uint64_t)node->allocated_page_count * (FILED_TMPFS_PAGE_BYTES / 512u);
    out_stat->nlink = node->kind == FILED_VNODE_DIRECTORY ?
        filed_tmpfs_dir_nlink_locked(backend, node->object_id) :
        (node->linked ? 1u : 0u);
    out_stat->kind = node->mode & FILED_TMPFS_MODE_TYPE_MASK;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_pread(filed_tmpfs_backend_t *backend, uint64_t object_id, uint64_t offset, void *buffer, uint64_t length, uint64_t *out_bytes)
{
    if (backend == NULL || buffer == NULL || out_bytes == NULL) {
        return -22;
    }
    *out_bytes = 0;
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_node_t *node = filed_tmpfs_find_node(backend, object_id);
    if (node == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (node->kind == FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -21;
    }
    if (node->kind != FILED_VNODE_REGULAR) {
        filed_tmpfs_lock_release(&backend->lock);
        return -22;
    }
    if (offset >= node->size) {
        filed_tmpfs_lock_release(&backend->lock);
        return 0;
    }
    uint64_t available = node->size - offset;
    if (length > available) {
        length = available;
    }
    uint64_t total = 0;
    while (total < length) {
        const uint64_t absolute = offset + total;
        const uint64_t page_index = absolute / FILED_TMPFS_PAGE_BYTES;
        const uint64_t page_offset = absolute % FILED_TMPFS_PAGE_BYTES;
        uint64_t chunk = length - total;
        if (chunk > FILED_TMPFS_PAGE_BYTES - page_offset) {
            chunk = FILED_TMPFS_PAGE_BYTES - page_offset;
        }
        const uint16_t page_id = page_index < FILED_TMPFS_NODE_MAX_PAGES ? node->pages[page_index] : 0;
        filed_tmpfs_page_t *page = filed_tmpfs_page_by_id(backend, page_id);
        if (page != NULL) {
            memcpy((uint8_t *)buffer + total, page->data + page_offset, (size_t)chunk);
        } else {
            memset((uint8_t *)buffer + total, 0, (size_t)chunk);
        }
        total += chunk;
    }
    *out_bytes = total;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_pwrite(filed_tmpfs_backend_t *backend, uint64_t object_id, uint64_t offset, const void *buffer, uint64_t length, uint64_t *out_bytes)
{
    if (backend == NULL || buffer == NULL || out_bytes == NULL) {
        return -22;
    }
    *out_bytes = 0;
    if (offset > FILED_TMPFS_MAX_FILE_BYTES || length > FILED_TMPFS_MAX_FILE_BYTES - offset) {
        return -27;
    }
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_node_t *node = filed_tmpfs_find_node(backend, object_id);
    if (node == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (node->kind == FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -21;
    }
    if (node->kind != FILED_VNODE_REGULAR) {
        filed_tmpfs_lock_release(&backend->lock);
        return -22;
    }
    uint64_t total = 0;
    while (total < length) {
        const uint64_t absolute = offset + total;
        const uint64_t page_index = absolute / FILED_TMPFS_PAGE_BYTES;
        const uint64_t page_offset = absolute % FILED_TMPFS_PAGE_BYTES;
        uint64_t chunk = length - total;
        if (chunk > FILED_TMPFS_PAGE_BYTES - page_offset) {
            chunk = FILED_TMPFS_PAGE_BYTES - page_offset;
        }
        if (page_index >= FILED_TMPFS_NODE_MAX_PAGES) {
            filed_tmpfs_lock_release(&backend->lock);
            return -27;
        }
        if (node->pages[page_index] == 0) {
            const uint16_t page_id = filed_tmpfs_alloc_page(backend);
            if (page_id == 0) {
                filed_tmpfs_lock_release(&backend->lock);
                return -28;
            }
            if (!filed_tmpfs_note_node_page(node, page_index)) {
                filed_tmpfs_free_page(backend, page_id);
                filed_tmpfs_lock_release(&backend->lock);
                return -28;
            }
            node->pages[page_index] = page_id;
        }
        filed_tmpfs_page_t *page = filed_tmpfs_page_by_id(backend, node->pages[page_index]);
        if (page == NULL) {
            filed_tmpfs_lock_release(&backend->lock);
            return -5;
        }
        memcpy(page->data + page_offset, (const uint8_t *)buffer + total, (size_t)chunk);
        total += chunk;
    }
    if (offset + length > node->size) {
        node->size = offset + length;
    }
    ++node->generation;
    *out_bytes = length;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_fsync(filed_tmpfs_backend_t *backend, uint64_t object_id)
{
    if (backend == NULL) {
        return -22;
    }
    filed_tmpfs_lock_acquire(&backend->lock);
    const filed_tmpfs_node_t *node = filed_tmpfs_find_node(backend, object_id);
    filed_tmpfs_lock_release(&backend->lock);
    return node == NULL ? -2 : 0;
}

static int filed_tmpfs_create_kind(filed_tmpfs_backend_t *backend, uint64_t parent_object_id, const char *name, uint64_t mode, filed_vnode_kind_t kind, uint64_t *out_object_id)
{
    if (backend == NULL || out_object_id == NULL || !filed_tmpfs_name_valid(name)) {
        return -22;
    }
    *out_object_id = 0;
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_node_t *parent = filed_tmpfs_find_node(backend, parent_object_id);
    if (parent == NULL || !parent->linked) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (parent->kind != FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -20;
    }
    if (filed_tmpfs_find_child(backend, parent_object_id, name) != NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -17;
    }
    filed_tmpfs_node_t *node = filed_tmpfs_alloc_node(backend);
    if (node == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -28;
    }
    memset(node, 0, sizeof(*node));
    node->slot_index = filed_tmpfs_node_slot(backend, node);
    if (node->slot_index == FILED_TMPFS_NO_NODE) {
        filed_tmpfs_lock_release(&backend->lock);
        return -5;
    }
    node->used = true;
    node->linked = true;
    node->object_id = filed_tmpfs_make_object_id(node->slot_index, backend->next_object_generation++);
    if (node->object_id == 0) {
        memset(node, 0, sizeof(*node));
        filed_tmpfs_lock_release(&backend->lock);
        return -5;
    }
    node->parent_slot = FILED_TMPFS_NO_NODE;
    node->first_child_slot = FILED_TMPFS_NO_NODE;
    node->next_sibling_slot = FILED_TMPFS_NO_NODE;
    node->hash_next_slot = FILED_TMPFS_NO_NODE;
    node->mode = filed_tmpfs_mode_for_kind(kind, mode);
    node->generation = 1;
    node->kind = kind;
    snprintf(node->name, sizeof(node->name), "%s", name);
    filed_tmpfs_link_child_locked(backend, parent, node);
    ++parent->generation;
    *out_object_id = node->object_id;
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

int filed_tmpfs_backend_truncate(filed_tmpfs_backend_t *backend, uint64_t object_id, uint64_t size)
{
    if (backend == NULL || size > FILED_TMPFS_MAX_FILE_BYTES) {
        return backend == NULL ? -22 : -27;
    }
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_node_t *node = filed_tmpfs_find_node(backend, object_id);
    if (node == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (node->kind == FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -21;
    }
    if (node->kind != FILED_VNODE_REGULAR) {
        filed_tmpfs_lock_release(&backend->lock);
        return -22;
    }
    const uint64_t old_size = node->size;
    const uint64_t keep_pages = size == 0 ? 0 : ((size - 1u) / FILED_TMPFS_PAGE_BYTES) + 1u;
    if (size < old_size) {
        if ((size % FILED_TMPFS_PAGE_BYTES) != 0 && keep_pages <= FILED_TMPFS_NODE_MAX_PAGES) {
            filed_tmpfs_page_t *last = filed_tmpfs_page_by_id(backend, node->pages[keep_pages - 1u]);
            if (last != NULL) {
                memset(
                    last->data + (size % FILED_TMPFS_PAGE_BYTES),
                    0,
                    (size_t)(FILED_TMPFS_PAGE_BYTES - (size % FILED_TMPFS_PAGE_BYTES)));
            }
        }
        filed_tmpfs_free_node_pages(backend, node, keep_pages);
    }
    node->size = size;
    ++node->generation;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_utimens(filed_tmpfs_backend_t *backend, uint64_t object_id, uint32_t mask, int64_t atime_sec, int64_t atime_nsec, int64_t mtime_sec, int64_t mtime_nsec)
{
    (void)mask;
    (void)atime_sec;
    (void)atime_nsec;
    (void)mtime_sec;
    (void)mtime_nsec;
    if (backend == NULL) {
        return -22;
    }
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_node_t *node = filed_tmpfs_find_node(backend, object_id);
    if (node == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    ++node->generation;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_chmod(filed_tmpfs_backend_t *backend, uint64_t object_id, uint64_t mode)
{
    if (backend == NULL) {
        return -22;
    }
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_node_t *node = filed_tmpfs_find_node(backend, object_id);
    if (node == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    node->mode = filed_tmpfs_mode_for_kind(node->kind, mode);
    ++node->generation;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_unlink(filed_tmpfs_backend_t *backend, uint64_t parent_object_id, const char *name)
{
    if (backend == NULL || !filed_tmpfs_name_valid(name)) {
        return -22;
    }
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_node_t *parent = filed_tmpfs_find_node(backend, parent_object_id);
    filed_tmpfs_node_t *child = filed_tmpfs_find_child(backend, parent_object_id, name);
    if (parent == NULL || child == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (parent->kind != FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -20;
    }
    if (child->kind == FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -21;
    }
    filed_tmpfs_unlink_child_locked(backend, parent, child);
    child->linked = false;
    ++child->generation;
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
    filed_tmpfs_node_t *parent = filed_tmpfs_find_node(backend, parent_object_id);
    filed_tmpfs_node_t *child = filed_tmpfs_find_child(backend, parent_object_id, name);
    if (parent == NULL || child == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (parent->kind != FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -20;
    }
    if (child->kind != FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -20;
    }
    if (!filed_tmpfs_directory_empty_locked(backend, child->object_id)) {
        filed_tmpfs_lock_release(&backend->lock);
        return -39;
    }
    filed_tmpfs_unlink_child_locked(backend, parent, child);
    child->linked = false;
    ++child->generation;
    ++parent->generation;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

static int filed_tmpfs_node_is_ancestor_locked(
    filed_tmpfs_backend_t *backend,
    uint64_t maybe_ancestor,
    uint64_t child)
{
    while (child != 0) {
        if (child == maybe_ancestor) {
            return 1;
        }
        filed_tmpfs_node_t *node = filed_tmpfs_find_node(backend, child);
        if (node == NULL) {
            return 0;
        }
        child = node->parent_object_id;
    }
    return 0;
}

int filed_tmpfs_backend_rename(filed_tmpfs_backend_t *backend, uint64_t old_parent_object_id, const char *old_name, uint64_t new_parent_object_id, const char *new_name, uint64_t *out_object_id)
{
    if (backend == NULL || out_object_id == NULL || !filed_tmpfs_name_valid(old_name) || !filed_tmpfs_name_valid(new_name)) {
        return -22;
    }
    *out_object_id = 0;
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_node_t *old_parent = filed_tmpfs_find_node(backend, old_parent_object_id);
    filed_tmpfs_node_t *new_parent = filed_tmpfs_find_node(backend, new_parent_object_id);
    filed_tmpfs_node_t *source = filed_tmpfs_find_child(backend, old_parent_object_id, old_name);
    filed_tmpfs_node_t *target = filed_tmpfs_find_child(backend, new_parent_object_id, new_name);
    if (old_parent == NULL || new_parent == NULL || source == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (old_parent->kind != FILED_VNODE_DIRECTORY || new_parent->kind != FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -20;
    }
    if (source->kind == FILED_VNODE_DIRECTORY &&
        filed_tmpfs_node_is_ancestor_locked(backend, source->object_id, new_parent->object_id))
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
        filed_tmpfs_unlink_child_locked(backend, new_parent, target);
        target->linked = false;
        ++target->generation;
    }
    filed_tmpfs_unlink_child_locked(backend, old_parent, source);
    snprintf(source->name, sizeof(source->name), "%s", new_name);
    filed_tmpfs_link_child_locked(backend, new_parent, source);
    ++source->generation;
    ++old_parent->generation;
    if (new_parent->object_id != old_parent->object_id) {
        ++new_parent->generation;
    }
    *out_object_id = source->object_id;
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_release_object(filed_tmpfs_backend_t *backend, uint64_t object_id)
{
    if (backend == NULL) {
        return -22;
    }
    if (object_id == backend->root_object_id) {
        return 0;
    }
    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_node_t *node = filed_tmpfs_find_node(backend, object_id);
    if (node == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (!node->linked) {
        const uint16_t slot = node->slot_index;
        filed_tmpfs_free_node_pages(backend, node, 0);
        memset(node, 0, sizeof(*node));
        if (slot != 0 && slot < FILED_TMPFS_MAX_NODES && backend->free_node_count < FILED_TMPFS_MAX_NODES) {
            backend->free_node_stack[backend->free_node_count++] = slot;
        }
    }
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}

int filed_tmpfs_backend_getdents(filed_tmpfs_backend_t *backend, uint64_t dir_object_id, uint64_t offset, koboxd_wire_fs_getdents_t *out_entries)
{
    if (backend == NULL || out_entries == NULL) {
        return -22;
    }
    memset(out_entries, 0, sizeof(*out_entries));
    out_entries->dir_object_id = dir_object_id;
    out_entries->offset = offset;
    out_entries->capacity = KOBOXD_WIRE_FS_DIRENT_CAPACITY;

    filed_tmpfs_lock_acquire(&backend->lock);
    filed_tmpfs_node_t *dir = filed_tmpfs_find_node(backend, dir_object_id);
    if (dir == NULL) {
        filed_tmpfs_lock_release(&backend->lock);
        return -2;
    }
    if (dir->kind != FILED_VNODE_DIRECTORY) {
        filed_tmpfs_lock_release(&backend->lock);
        return -20;
    }
    uint64_t skipped = 0;
    uint16_t slot = dir->first_child_slot;
    while (slot != FILED_TMPFS_NO_NODE && out_entries->count < KOBOXD_WIRE_FS_DIRENT_CAPACITY) {
        filed_tmpfs_node_t *node = filed_tmpfs_node_by_slot(backend, slot);
        if (node == NULL) {
            break;
        }
        slot = node->next_sibling_slot;
        if (!node->linked) {
            continue;
        }
        if (skipped < offset) {
            ++skipped;
            continue;
        }
        koboxd_wire_fs_dirent_t *entry = &out_entries->entries[out_entries->count++];
        entry->object_id = node->object_id;
        entry->kind = node->mode & FILED_TMPFS_MODE_TYPE_MASK;
        entry->name_len = strlen(node->name);
        snprintf(entry->name, sizeof(entry->name), "%s", node->name);
    }
    filed_tmpfs_lock_release(&backend->lock);
    return 0;
}
