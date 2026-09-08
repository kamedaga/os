#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include "../userland/filed/src/exec/linux_lpr/map.c"

static unsigned allocations, maps, closes;
static uint64_t buffer_size;
static void *buffer;
static uint64_t expected_pointer;
int pacha_vmo_create(uint64_t size, uint64_t rights, uint32_t flags)
{
    (void)rights; (void)flags;
    assert(!buffer); buffer = calloc(1, size); assert(buffer);
    buffer_size = size; ++allocations; return 20;
}
void *pacha_mmap(int fd, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset)
{
    (void)prot; (void)flags;
    assert(fd == 20 && size == buffer_size && !offset); return buffer;
}
int pacha_munmap(void *addr, uint64_t size)
{ assert(addr == buffer && size == buffer_size); return 0; }
int pacha_fd_close(int fd)
{ assert(fd == 20); ++closes; free(buffer); buffer = NULL; return 0; }
int pacha_process_map_batch(int fd, const struct pacha_process_map_batch_entry *entries, uint64_t count)
{
    assert(fd >= 30 && count == 1);
    assert(entries[0].vmo_fd == 20 && entries[0].flags == PACHA_PROCESS_MAP_PRIVATE);
    assert(entries[0].size == 5 * 1024 * 1024);
    assert(memcmp(buffer, "runtime", 7) == 0);
    if (expected_pointer)
        assert(lpr_reloc_u64((unsigned char *)buffer + 512) == expected_pointer);
    ++maps; return 0;
}
int lpr_exec_image_find_text_section(const lpr_exec_image_t *image, uint64_t *offset, uint64_t *size)
{ (void)image; (void)offset; (void)size; abort(); }
int lpr_exec_patch_syscalls(unsigned char *bytes, uint64_t size, uint64_t *patched)
{ (void)bytes; (void)size; (void)patched; abort(); }

int main(void)
{
    unsigned char ph[56] = {0};
    uint32_t type = LPR_EXEC_PT_LOAD, flags = LPR_EXEC_PF_R | LPR_EXEC_PF_W;
    uint64_t size = 5 * 1024 * 1024, filesz = 7;
    memcpy(ph, &type, 4); memcpy(ph + 4, &flags, 4);
    memcpy(ph + 32, &filesz, 8); memcpy(ph + 40, &size, 8);
    lpr_exec_image_t image = {.bytes=(unsigned char *)"runtime", .size=7,
        .backend_object=42, .object_generation=1};
    for (int process = 30; process < 62; ++process) {
        assert(lpr_exec_load_memory_image_into_process(process, &image, ph, 1, 56,
            0x4000000, 0, 0, NULL, 0, NULL) == 0);
    }
    assert(allocations == 1 && maps == 32 && closes == 0);
    for (unsigned i = 0; i < LPR_EXEC_MEMORY_SPAN_VMO_CACHE_SLOTS; ++i)
        lpr_exec_memory_span_vmo_cache_clear_slot(&lpr_exec_memory_span_vmo_cache[i]);
    assert(closes == 1 && !buffer && lpr_exec_memory_span_vmo_cache_bytes == 0);
    /* Relocate once before publishing; subsequent children share the template.
     * Ordinary Linux images and different biases must not reuse this template. */
    unsigned char runtime[1024] = "runtime", headers[112] = {0};
    memcpy(headers, ph, sizeof(ph));
    filesz = sizeof(runtime); memcpy(headers + 32, &filesz, 8);
    uint64_t dynamic_header[] = {2, 128, 128, 0, 64, 64, 8};
    uint64_t dynamic[] = {7, 256, 8, 24, 9, 24, 0, 0};
    uint64_t rela[] = {512, 8, 768};
    memcpy(headers + 56, dynamic_header, sizeof(dynamic_header));
    memcpy(runtime + 128, dynamic, sizeof(dynamic));
    memcpy(runtime + 256, rela, sizeof(rela));
    image.bytes = runtime; image.size = sizeof(runtime);
    expected_pointer = 0x4000300;
    for (int process = 30; process < 62; ++process)
        assert(lpr_exec_load_memory_image_into_process(process, &image, headers, 2, 56,
            0x4000000, 0, 1, NULL, 0, NULL) == 0);
    assert(allocations == 2 && maps == 64 && closes == 1);
    assert(lpr_reloc_u64(runtime + 512) == 0); /* source ELF unchanged */
    assert(lpr_exec_memory_span_vmo_cache_find(42, 1, sizeof(runtime),
        0x4000000, 0x4000000, size, 0, 1) != NULL);
    assert(lpr_exec_memory_span_vmo_cache_find(42, 1, sizeof(runtime),
        0x4000000, 0x4000000, size, 0, 0) == NULL);
    assert(lpr_exec_memory_span_vmo_cache_find(42, 1, sizeof(runtime),
        0x8000000, 0x8000000, size, 0, 1) == NULL);
    for (unsigned i = 0; i < LPR_EXEC_MEMORY_SPAN_VMO_CACHE_SLOTS; ++i)
        lpr_exec_memory_span_vmo_cache_clear_slot(&lpr_exec_memory_span_vmo_cache[i]);
    assert(closes == 2 && !buffer && lpr_exec_memory_span_vmo_cache_bytes == 0);
    puts("FILED_RUNTIME_SHARING_UNIT=OK");
    return 0;
}
