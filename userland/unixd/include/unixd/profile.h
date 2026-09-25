#pragma once
#include <stdint.h>

/* Measurement-only build. Elapsed TSC ticks include descheduling/waits;
 * nested rows are inclusive and must not be summed with their parents. */
enum unix_profile_group { UP_IO, UP_RPC, UP_CONTROL, UP_GROUPS };
enum unix_profile_stage {
    UP_TOTAL, UP_SETUP, UP_RESERVE, UP_COPY, UP_COMMIT, UP_NOTIFY,
    UP_WATCH, UP_WAIT, UP_CLEANUP, UP_ROUTE, UP_MAP,
    UP_SERIALIZE, UP_CALL, UP_RECEIVE, UP_VALIDATE, UP_CLOSE, UP_STAGES
};
#if defined(LPR_UNIX_PROFILE) && LPR_UNIX_PROFILE
struct unix_profile_scope { uint64_t start; unsigned group, key, stage; };
static inline uint64_t unix_profile_clock(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
void unix_profile_end(struct unix_profile_scope *scope);
void unix_profile_dump(void);
#define UP_BEGIN(name, g, k, s) \
    struct unix_profile_scope name __attribute__((cleanup(unix_profile_end))) = \
        { unix_profile_clock(), (g), (k), (s) }
#define UP_END(name) unix_profile_end(&(name))
#else
#define UP_BEGIN(name, g, k, s) ((void)0)
#define UP_END(name) ((void)0)
static inline void unix_profile_dump(void) {}
#endif
