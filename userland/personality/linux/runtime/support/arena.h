#ifndef LPR_SUPPORT_ARENA_H
#define LPR_SUPPORT_ARENA_H

#include <stddef.h>
#include <stdint.h>

struct lpr_arena {
    uint8_t *base;
    uint64_t size;
    uint64_t used;
};

void lpr_arena_init(struct lpr_arena *arena, void *memory, uint64_t size);
uint64_t lpr_arena_remaining(const struct lpr_arena *arena);
void *lpr_arena_alloc(struct lpr_arena *arena, uint64_t size, uint64_t align);
char *lpr_arena_strdup(struct lpr_arena *arena, const char *s);
char *lpr_arena_strndup(struct lpr_arena *arena, const char *s, size_t max_len);

#endif
