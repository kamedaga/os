#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Opt-in libdrm diagnostic, not a browser or Mesa patch. Only the validated
 * Mesa caller's EXECBUFFER header is observed; command contents stay private.
 * Per-thread aggregates avoid adding contention to the path being measured. */
static uint64_t stamp(void)
{
    uint32_t low, high;
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high) :: "memory");
    return ((uint64_t)high << 32) | low;
}

int drmIoctl(int fd, unsigned long request, void *argument)
{
    static _Thread_local int (*next)(int, unsigned long, void *);
    static _Thread_local struct {
        uint64_t calls, cycles, maximum, bytes, errors;
    } buckets[9];
    static _Thread_local unsigned reports;
    const int incoming_errno = errno;
    if (!next) next = dlsym(RTLD_NEXT, "drmIoctl");
    if (!next) { errno = ENOSYS; return -1; }
    if (request != UINT32_C(0xc0406442) || !argument) {
        errno = incoming_errno;
        return next(fd, request, argument);
    }
    uint32_t header[2];
    memcpy(header, argument, sizeof(header));
    const unsigned bucket = header[0] < 8 ? header[0] : 8;
    errno = incoming_errno;
    const uint64_t start = stamp();
    const int result = next(fd, request, argument);
    const uint64_t elapsed = stamp() - start;
    const int saved_errno = errno;
    ++buckets[bucket].calls;
    buckets[bucket].cycles += elapsed;
    if (elapsed > buckets[bucket].maximum) buckets[bucket].maximum = elapsed;
    buckets[bucket].bytes += header[1];
    buckets[bucket].errors += result < 0;
    const uint64_t count = buckets[bucket].calls;
    if (reports < 64 && (count == 1 || (count >= 32 && !(count & (count - 1))))) {
        ++reports;
        char line[256];
        int length = snprintf(line, sizeof(line),
            "VIRGL_EXEC_TIMING pid=%ld stream=%llx flags_bucket=%u calls=%llu cycles=%llu max_cycles=%llu bytes=%llu errors=%llu\n",
            (long)getpid(), (unsigned long long)(uintptr_t)buckets,
            bucket, (unsigned long long)count,
            (unsigned long long)buckets[bucket].cycles,
            (unsigned long long)buckets[bucket].maximum,
            (unsigned long long)buckets[bucket].bytes,
            (unsigned long long)buckets[bucket].errors);
        if (length > 0 && (size_t)length < sizeof(line))
            (void)write(2, line, (size_t)length);
    }
    errno = saved_errno;
    return result;
}
