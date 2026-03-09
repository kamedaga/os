#ifndef CAP_ABILITY_OS_CAPC_H
#define CAP_ABILITY_OS_CAPC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

long cap_write(int fd, const void *buf, size_t len);
long cap_log(const char *buf, size_t len);
void *cap_malloc(size_t len);
void cap_free(void *ptr);
int cap_untyped_alloc(size_t bytes, size_t align, uint64_t flags, uint64_t *out_token);
int cap_untyped_retype_pages(
    uint64_t token,
    void *base_va,
    size_t page_count,
    uint64_t flags,
    uint64_t *out_paddrs
);
int cap_untyped_alloc_map_pages(
    void *base_va,
    size_t page_count,
    uint64_t flags,
    uint64_t *out_paddrs
);
int cap_untyped_reset(uint64_t token);

enum cap_untyped_alloc_flag {
    CAP_UNTYPED_ALLOC_CONTIGUOUS = 1ull << 0,
    CAP_UNTYPED_ALLOC_DMA_OK = 1ull << 1
};

enum cap_untyped_retype_flag {
    CAP_UNTYPED_RETYPE_WRITABLE = 1ull << 0,
    CAP_UNTYPED_RETYPE_DROP_CAP_AFTER_MAP = 1ull << 1,
    CAP_UNTYPED_RETYPE_CONTIGUOUS = 1ull << 2
};

enum cap_untyped_alloc_map_flag {
    CAP_UNTYPED_ALLOC_MAP_WRITABLE = 1ull << 0,
    CAP_UNTYPED_ALLOC_MAP_DROP_CAP_AFTER_MAP = 1ull << 1,
    CAP_UNTYPED_ALLOC_MAP_CONTIGUOUS = 1ull << 2,
    CAP_UNTYPED_ALLOC_MAP_DMA_OK = 1ull << 3
};

#ifdef __cplusplus
}
#endif

#endif /* CAP_ABILITY_OS_CAPC_H */
