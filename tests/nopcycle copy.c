#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(__rdtsc)
#pragma intrinsic(__rdtscp)
#define NOINLINE __declspec(noinline)
#else
#include <x86intrin.h>
#define NOINLINE __attribute__((noinline))
#endif

#if defined(_WIN32)
#include <windows.h>
static void pin_to_cpu0(void) {
    SetThreadAffinityMask(GetCurrentThread(), 1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}
#else
#include <sched.h>
#include <unistd.h>
static void pin_to_cpu0(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    sched_setaffinity(0, sizeof(set), &set);
}
#endif

#define NOP10 "nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t""nop\n\t"
#define NOP50 NOP10 NOP10 NOP10 NOP10 NOP10
#define NOP100 NOP50 NOP50
#define NOP500 NOP100 NOP100 NOP100 NOP100 NOP100
#define NOP1000 NOP500 NOP500
#define NOP5000 NOP1000 NOP1000 NOP1000 NOP1000 NOP1000

static inline uint64_t rdtsc_start(void) {
#if defined(_MSC_VER)
    int regs[4];
    __cpuid(regs, 0);
    return __rdtsc();
#else
    unsigned int lo, hi;
    __asm__ __volatile__("cpuid\n\t"
                         "rdtsc\n\t"
                         : "=a"(lo), "=d"(hi)
                         : "a"(0)
                         : "rbx", "rcx", "memory");
    return ((uint64_t)hi << 32) | lo;
#endif
}

static inline uint64_t rdtsc_stop(void) {
    unsigned int aux;
#if defined(_MSC_VER)
    uint64_t t = __rdtscp(&aux);
    int regs[4];
    __cpuid(regs, 0);
    return t;
#else
    unsigned int lo, hi;
    __asm__ __volatile__("rdtscp\n\t"
                         : "=a"(lo), "=d"(hi), "=c"(aux)
                         :
                         : "memory");
    __asm__ __volatile__("cpuid\n\t"
                         :
                         : "a"(0)
                         : "rbx", "rcx", "rdx", "memory");
    return ((uint64_t)hi << 32) | lo;
#endif
}

NOINLINE void nop_500(void) {
#if defined(_MSC_VER)
    /* MSVC x64 does not support inline assembly. Build this file with clang/gcc. */
#else
    __asm__ __volatile__(NOP500 ::: "memory");
#endif
}

NOINLINE void nop_5000(void) {
#if defined(_MSC_VER)
    /* MSVC x64 does not support inline assembly. Build this file with clang/gcc. */
#else
    __asm__ __volatile__(NOP5000 ::: "memory");
#endif
}

NOINLINE void nop_16000(void) {
#if defined(_MSC_VER)
    /* MSVC x64 does not support inline assembly. Build this file with clang/gcc. */
#else
    __asm__ __volatile__(".rept 16000\n\t"
                         "nop\n\t"
                         ".endr\n\t"
                         ::: "memory");
#endif
}

NOINLINE void empty_block(void) {
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void measure(void (*fn)(void), uint64_t *out, int samples) {
    for (int i = 0; i < samples; i++) {
        uint64_t t0 = rdtsc_start();
        fn();
        uint64_t t1 = rdtsc_stop();
        out[i] = t1 - t0;
    }
}

typedef struct {
    uint64_t min;
    uint64_t p50;
    uint64_t p90;
    uint64_t p99;
    double trimmed_mean;
} stat_result;

static stat_result stats(const char *name, uint64_t *v, int n) {
    qsort(v, n, sizeof(v[0]), cmp_u64);
    unsigned long long sum = 0;
    for (int i = n / 20; i < n - n / 20; i++) {
        sum += v[i];
    }
    int kept = n - 2 * (n / 20);
    stat_result r = {
        v[0],
        v[n / 2],
        v[(n * 90) / 100],
        v[(n * 99) / 100],
        (double)sum / kept,
    };
    printf("%-12s min=%5llu p50=%5llu p90=%5llu p99=%5llu trimmed_mean=%.2f\n",
           name,
           (unsigned long long)r.min,
           (unsigned long long)r.p50,
           (unsigned long long)r.p90,
           (unsigned long long)r.p99,
           r.trimmed_mean);
    return r;
}

int main(int argc, char **argv) {
    int samples = 200000;
    if (argc > 1) {
        samples = atoi(argv[1]);
    }
    if (samples < 1000) {
        samples = 1000;
    }

    pin_to_cpu0();

    uint64_t *empty = (uint64_t *)malloc((size_t)samples * sizeof(uint64_t));
    uint64_t *nop500 = (uint64_t *)malloc((size_t)samples * sizeof(uint64_t));
    uint64_t *nop5000 = (uint64_t *)malloc((size_t)samples * sizeof(uint64_t));
    uint64_t *nop16000 = (uint64_t *)malloc((size_t)samples * sizeof(uint64_t));
    if (!empty || !nop500 || !nop5000 || !nop16000) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    for (int i = 0; i < 10000; i++) {
        empty_block();
        nop_500();
        nop_5000();
        nop_16000();
    }

    measure(empty_block, empty, samples);
    measure(nop_500, nop500, samples);
    measure(nop_5000, nop5000, samples);
    measure(nop_16000, nop16000, samples);

    printf("samples=%d, 1-byte nop blocks\n", samples);
    stat_result empty_s = stats("empty", empty, samples);
    stat_result nop500_s = stats("nop500", nop500, samples);
    stat_result nop5000_s = stats("nop5000", nop5000, samples);
    stat_result nop16000_s = stats("nop16000", nop16000, samples);
    printf("estimate cycles per 500 nops, using trimmed_mean-empty:\n");
    printf("  nop500:   %.2f\n", nop500_s.trimmed_mean - empty_s.trimmed_mean);
    printf("  nop5000:  %.2f\n", (nop5000_s.trimmed_mean - empty_s.trimmed_mean) / 10.0);
    printf("  nop16000: %.2f\n", (nop16000_s.trimmed_mean - empty_s.trimmed_mean) / 32.0);

    free(empty);
    free(nop500);
    free(nop5000);
    free(nop16000);
    return 0;
}
