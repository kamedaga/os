/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <time.h>
#include "../_kobox/src/linux_personality/linux_memory.c"

/* Host backing for the allocator's page path, not a kernel/module substitute. */
void *kb_alloc_pages_exact(size_t size, unsigned int flags)
{
    return flags & KB_LINUX___GFP_ZERO ? calloc(1, size) : malloc(size);
}

void kb_free_pages_exact(void *ptr, size_t size)
{
    (void)size;
    free(ptr);
}

static double seconds(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec + now.tv_nsec / 1e9;
}

static void index_lifecycle(size_t count, unsigned order)
{
    kb_heap_allocation_t *records = calloc(count, sizeof(*records));
    assert(records);
    for (size_t i = 0; i < count; ++i) {
        records[i].ptr = (void *)(uintptr_t)(0x100000 + i * 4096);
        records[i].size = i + 1;
        track_heap_allocation(&records[i]);
    }
    for (size_t i = 0; i < count; ++i)
        assert(kb_kmalloc_usable_size(records[i].ptr) == i + 1);
    assert(take_heap_allocation((void *)(uintptr_t)0x123456) == NULL);
    kb_memory_hotpath_profile_reset();
    double begin = seconds();
    for (size_t i = 0; i < count; ++i) {
        size_t j = order == 0 ? i : order == 1 ? count - 1 - i : (i * 4051) % count;
        kb_heap_allocation_t *record = take_heap_allocation(records[j].ptr);
        assert(record == &records[j] && record->next == NULL);
        assert(kb_kmalloc_usable_size(records[j].ptr) == 0);
    }
    double elapsed = seconds() - begin;
    kb_memory_hotpath_profile_t profile;
    kb_memory_hotpath_profile_snapshot(&profile);
    printf("HEAP_INDEX count=%zu order=%u steps=%llu seconds=%.6f\n",
        count, order, (unsigned long long)profile.allocation_search_steps, elapsed);
    for (size_t i = 0; i < count; ++i) {
        /* Reusing exactly the same address must not leave stale membership. */
        track_heap_allocation(&records[i]);
        assert(take_heap_allocation(records[i].ptr) == &records[i]);
        assert(take_heap_allocation(records[i].ptr) == NULL);
    }
    free(records);
}

static void allocator_lifecycle(const char *arena, const char *mapped)
{
    assert(setenv("KOBOX_KMALLOC_ARENA", arena, 1) == 0);
    assert(setenv("KOBOX_KMALLOC_MMAP", mapped, 1) == 0);
    void *pointers[128];
    for (size_t i = 0; i < 128; ++i) {
        size_t size = i & 1 ? 8192 : 17 + i;
        pointers[i] = kb_kmalloc(size, KB_LINUX___GFP_ZERO);
        assert(pointers[i] && kb_kmalloc_usable_size(pointers[i]) == size);
        for (size_t j = 0; j < size; ++j)
            assert(((unsigned char *)pointers[i])[j] == 0);
        memset(pointers[i], (int)i, size);
    }
    for (size_t i = 0; i < 128; ++i) {
        size_t j = (i * 53) % 128;
        size_t size = j & 1 ? 8192 : 17 + j;
        void *replacement = kb_krealloc_managed(pointers[j], size + 31, 0);
        assert(replacement && kb_kmalloc_usable_size(replacement) == size + 31);
        for (size_t k = 0; k < size; ++k)
            assert(((unsigned char *)replacement)[k] == j);
        kb_kfree(replacement);
    }
    void *zero = kb_kzalloc(73, 0);
    assert(zero && kb_kmalloc_usable_size(zero) == 73);
    for (size_t i = 0; i < 73; ++i) assert(((unsigned char *)zero)[i] == 0);
    kb_kfree(zero);
    kb_kfree(NULL);
    kb_kfree((void *)(uintptr_t)16);
    kb_kfree((void *)(uintptr_t)-1);
    kb_kfree((void *)(uintptr_t)0x123456);
    for (size_t i = 0; i < (1u << KB_KMALLOC_INDEX_BITS); ++i)
        assert(heap_allocations[i] == NULL);
    /* Preserve the production arena lifetime policy, but release test backing. */
    heap_arena_free_blocks = NULL;
    while (heap_arena_chunks) {
        kb_heap_arena_chunk_t *chunk = heap_arena_chunks;
        heap_arena_chunks = chunk->next;
        size_t header = align_up_size(sizeof(*chunk), KB_KMALLOC_RECORD_ALIGN);
        assert(munmap(chunk, header + chunk->capacity) == 0);
    }
}

static void collisions(void)
{
    kb_heap_allocation_t records[5] = {0};
    uintptr_t address = 0x100000;
    size_t bucket = heap_allocation_bucket((void *)address);
    for (size_t i = 0; i < 5; ++i) {
        while (heap_allocation_bucket((void *)address) != bucket) address += 16;
        records[i].ptr = (void *)address;
        records[i].size = i + 7;
        address += 16;
        if (i < 4) track_heap_allocation(&records[i]);
    }
    assert(take_heap_allocation(records[4].ptr) == NULL);
    const size_t order[] = {1, 3, 0, 2}; /* middle, head, tail, last */
    for (size_t i = 0; i < 4; ++i) {
        size_t j = order[i];
        assert(kb_kmalloc_usable_size(records[j].ptr) == j + 7);
        assert(take_heap_allocation(records[j].ptr) == &records[j]);
        assert(take_heap_allocation(records[j].ptr) == NULL);
    }
}

int main(void)
{
    collisions();
    for (size_t count = 1024; count <= 8192; count *= 8)
        for (unsigned order = 0; order < 3; ++order)
            index_lifecycle(count, order);
    allocator_lifecycle("0", "0");
    allocator_lifecycle("0", "1");
    allocator_lifecycle("1", "0");
    puts("HEAP_INDEX_PASS");
    return 0;
}
