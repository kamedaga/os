#define _GNU_SOURCE

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static uint64_t now_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
}

static uint64_t read_tsc(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

int main(void)
{
    const uint64_t start_ns = now_ns();
    const uint64_t start_cycles = read_tsc();
    uint64_t end_ns = start_ns;
    uint64_t end_cycles = start_cycles;
    while (end_ns - start_ns < 100000000ull) {
        end_ns = now_ns();
        end_cycles = read_tsc();
    }
    if (start_ns == 0 || end_ns <= start_ns || end_cycles <= start_cycles) {
        return 1;
    }
    const uint64_t elapsed_ns = end_ns - start_ns;
    const uint64_t elapsed_cycles = end_cycles - start_cycles;
    /* The calibration window is 100 ms, so the product remains within u64
     * even for TSC rates far above the virtual CPU used by this benchmark. */
    const uint64_t hz = elapsed_cycles * 1000000000ull / elapsed_ns;
    printf("KEY_PHASE_TSC_CALIBRATION cycles=%" PRIu64 " ns=%" PRIu64
        " hz=%" PRIu64 "\n", elapsed_cycles, elapsed_ns, hz);
    return 0;
}
