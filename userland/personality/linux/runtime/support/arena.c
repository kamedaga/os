#include "arena.h"
#include "string.h"

static int lpr_is_power_of_two(uint64_t value)
{
    return value != 0 && (value & (value - 1u)) == 0;
}

static int lpr_align_up(uint64_t value, uint64_t align, uint64_t *out)
{
    if (out == 0 || !lpr_is_power_of_two(align)) {
        return -1;
    }
    const uint64_t mask = align - 1u;
    if (value > UINT64_MAX - mask) {
        return -1;
    }
    *out = (value + mask) & ~mask;
    return 0;
}

void lpr_arena_init(struct lpr_arena *arena, void *memory, uint64_t size)
{
    if (arena == 0) {
        return;
    }
    arena->base = (uint8_t *)memory;
    arena->size = memory == 0 ? 0 : size;
    arena->used = 0;
}

uint64_t lpr_arena_remaining(const struct lpr_arena *arena)
{
    if (arena == 0 || arena->used > arena->size) {
        return 0;
    }
    return arena->size - arena->used;
}

void *lpr_arena_alloc(struct lpr_arena *arena, uint64_t size, uint64_t align)
{
    uint64_t start;
    uint64_t end;
    if (arena == 0 || arena->base == 0 || align == 0) {
        return 0;
    }
    if (lpr_align_up(arena->used, align, &start) != 0) {
        return 0;
    }
    if (size > UINT64_MAX - start) {
        return 0;
    }
    end = start + size;
    if (end > arena->size) {
        return 0;
    }
    arena->used = end;
    return arena->base + start;
}

char *lpr_arena_strdup(struct lpr_arena *arena, const char *s)
{
    if (s == 0) {
        return 0;
    }
    const size_t len = lpr_strlen(s);
    if (len == (size_t)UINT64_MAX) {
        return 0;
    }
    char *copy = (char *)lpr_arena_alloc(arena, (uint64_t)len + 1u, 1);
    if (copy == 0) {
        return 0;
    }
    lpr_memcpy(copy, s, len + 1u);
    return copy;
}

char *lpr_arena_strndup(struct lpr_arena *arena, const char *s, size_t max_len)
{
    if (s == 0) {
        return 0;
    }
    const size_t len = lpr_strnlen(s, max_len);
    char *copy = (char *)lpr_arena_alloc(arena, (uint64_t)len + 1u, 1);
    if (copy == 0) {
        return 0;
    }
    lpr_memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}
