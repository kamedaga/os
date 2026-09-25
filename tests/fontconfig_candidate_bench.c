#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Isolated diagnostic for repeated candidate-list copies/removals, not a
 * browser replacement or optimization. Use the same unmodified fontconfig
 * binary under host Linux and LPR. No installed fonts/configuration required. */
static double seconds(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}
static uint64_t cycles(void)
{
    unsigned lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
int main(int argc, char **argv)
{
    /* A dependency-chain control distinguishes generic execution slowdown
     * from candidate allocation/list work. No syscalls inside the loop. */
    if (argc == 2 && strcmp(argv[1], "--cpu") == 0) {
        volatile uint64_t value = 1;
        puts("FONT_CPU_BEGIN");
        fflush(stdout);
        double start = seconds();
        for (unsigned i = 0; i < 100000000; ++i)
            value = value * UINT64_C(6364136223846793005) + 1;
        double elapsed = seconds() - start;
        printf("FONT_CPU_CONTROL value=%llu elapsed_ms=%.3f\n",
               (unsigned long long)value, elapsed * 1000);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--alloc") == 0) {
        unsigned char *slots[105];
        unsigned long sum = 0;
        puts("FONT_ALLOC_BEGIN");
        fflush(stdout);
        double start = seconds();
        for (unsigned r = 0; r < 30000; ++r) {
            for (unsigned i = 0; i < 105; ++i) {
                slots[i] = malloc(32);
                assert(slots[i]);
                memset(slots[i], (int)i, 32);
            }
            for (unsigned i = 0; i < 105; ++i) {
                sum += slots[i][0];
                free(slots[i]);
            }
        }
        double elapsed = seconds() - start;
        assert(sum == 163800000);
        printf("FONT_ALLOC_CONTROL allocations=3150000 checksum=%lu elapsed_ms=%.3f\n",
               sum, elapsed * 1000);
        return 0;
    }
    int stages = argc == 2 && strcmp(argv[1], "--stages") == 0;
    assert(argc == 1 || stages);
    void *lib = dlopen("libfontconfig.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "fontconfig load: %s\n", dlerror()); return 1; }
#define FN(type, name, args) type (*name) args = dlsym(lib, #name); assert(name)
    FN(void *, FcPatternCreate, (void));
    FN(void, FcPatternDestroy, (void *));
    FN(int, FcPatternAddString, (void *, const char *, const unsigned char *));
    FN(void *, FcPatternFilter, (void *, const void *));
    FN(int, FcPatternRemove, (void *, const char *, int));
    FN(void *, FcObjectSetCreate, (void));
    FN(int, FcObjectSetAdd, (void *, const char *));
    FN(void, FcObjectSetDestroy, (void *));
#undef FN
    void *objects = FcObjectSetCreate();
    assert(objects && FcObjectSetAdd(objects, "family"));
    const unsigned rounds = 3000, width = 105;
    unsigned long copies = 0, removed = 0;
    Dl_info module;
    assert(dladdr((void *)FcPatternFilter, &module));
    printf("FONT_CANDIDATE_BEGIN base=%p module=%s\n", module.dli_fbase,
           module.dli_fname);
    assert(dladdr((void *)malloc, &module));
    printf("FONT_LIBC base=%p malloc=%p module=%s\n", module.dli_fbase,
           (void *)malloc, module.dli_fname);
    fflush(stdout);
    double start = seconds();
    uint64_t total_start = cycles(), add_cycles = 0, copy_cycles = 0, remove_cycles = 0;
    uint64_t destroy_cycles = 0, head_cycles = 0;
    for (unsigned r = 0; r < rounds; ++r) {
        uint64_t mark = stages ? cycles() : 0;
        void *pattern = FcPatternCreate();
        assert(pattern);
        for (unsigned i = 0; i < width; ++i) {
            char family[40];
            snprintf(family, sizeof(family), "candidate-%u", i);
            assert(FcPatternAddString(pattern, "family", (const unsigned char *)family));
        }
        if (stages) add_cycles += cycles() - mark;
        for (unsigned i = 0; i < width; ++i) {
            if (stages) mark = cycles();
            void *copy = FcPatternFilter(pattern, objects);
            assert(copy);
            ++copies;
            if (stages) { uint64_t end = cycles(); copy_cycles += end - mark; mark = end; }
            while (FcPatternRemove(copy, "family", 1)) ++removed;
            if (stages) { uint64_t end = cycles(); remove_cycles += end - mark; mark = end; }
            FcPatternDestroy(copy);
            if (stages) { uint64_t end = cycles(); destroy_cycles += end - mark; mark = end; }
            assert(FcPatternRemove(pattern, "family", 0));
            if (stages) head_cycles += cycles() - mark;
        }
        FcPatternDestroy(pattern);
    }
    uint64_t total_cycles = cycles() - total_start;
    double elapsed = seconds() - start;
    FcObjectSetDestroy(objects);
    assert(copies == rounds * width);
    assert(removed == (unsigned long)rounds * width * (width - 1) / 2);
    printf("FONT_CANDIDATE_BENCH copies=%lu removed=%lu elapsed_ms=%.3f\n",
           copies, removed, elapsed * 1000);
    if (stages)
        printf("FONT_CANDIDATE_STAGES add_ms=%.3f copy_ms=%.3f remove_ms=%.3f destroy_ms=%.3f head_ms=%.3f total_cycles=%llu\n",
               add_cycles * elapsed * 1000 / total_cycles,
               copy_cycles * elapsed * 1000 / total_cycles,
               remove_cycles * elapsed * 1000 / total_cycles,
               destroy_cycles * elapsed * 1000 / total_cycles,
               head_cycles * elapsed * 1000 / total_cycles,
               (unsigned long long)total_cycles);
    dlclose(lib);
    return 0;
}
