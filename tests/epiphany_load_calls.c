#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef PAGE_FONT_STAGE_DIAG
#define PAGE_FONT_STAGE_DIAG 0
#endif
#if PAGE_FONT_STAGE_DIAG && !PAGE_CYCLE_DIAG
#error PAGE_FONT_STAGE_DIAG requires PAGE_CYCLE_DIAG
#endif

/* Opt-in elapsed-call diagnostic for real page loads. No defaults changed.
 * Times include scheduling and nested calls; never sum them as CPU time.
 * Only slow calls are logged, bounded per process, with recursion suppressed. */
static _Thread_local unsigned depth;
static _Atomic unsigned reports;
struct metric { const char *name; _Atomic unsigned long calls, elapsed; };
static struct metric metrics[] = {
    { .name="mmap" }, { .name="munmap" }, { .name="mprotect" }, { .name="madvise" },
    { .name="FcFontMatch" }, { .name="FcFontSort" }, { .name="FcFontSetMatch" },
    { .name="FcFontSetSort" }, { .name="FcInitLoadConfigAndFonts" },
    { .name="FcConfigBuildFonts" }, { .name="FcConfigSubstitute" },
    { .name="pthread_cond_wait" }, { .name="pthread_cond_timedwait" },
};
static _Atomic uint64_t next_summary;
static _Atomic unsigned summary_reports;
static long main_tid;
static unsigned main_traces;
static _Atomic unsigned long time_calls, time_cycles;
static uint64_t calibration_ms, calibration_cycles;
#if PAGE_CYCLE_DIAG
static struct {
    const char *name;
    _Atomic unsigned long calls, cycles;
} font_cycles[] = {
    { .name="FcConfigCreate" }, { .name="FcFontSetMatch" },
    { .name="FcConfigDestroy" },
#if PAGE_FONT_STAGE_DIAG
    { .name="FcPatternCreate" }, { .name="FcPatternDuplicate" },
    { .name="FcPatternDestroy" }, { .name="FcPatternAddString" },
    { .name="FcPatternAddLangSet" }, { .name="FcFontSetCreate" },
    { .name="FcFontSetDestroy" }, { .name="FcFontSetAdd" },
    { .name="FcLangSetCreate" }, { .name="FcLangSetDestroy" },
    { .name="FcLangSetAdd" }, { .name="FcDefaultSubstitute" },
    { .name="FcConfigSubstitute" },
    { .name="FcPatternFilter" },
    { .name="FcPatternRemoveSampled" },
#endif
};
static _Atomic unsigned cycle_reports;
#endif
static uint64_t cycles(void)
{
    unsigned lo,hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi<<32)|lo;
}
struct unwind_context;
struct trace_state { unsigned count; unsigned long (*ip)(struct unwind_context *); };
static int trace_frame(struct unwind_context *context, void *data)
{
    struct trace_state *state=data;
    if(state->count++ >= 16) return 5;
    uintptr_t ip=state->ip(context);
    Dl_info info={0};
    dladdr((void *)ip,&info);
    char line[512];
    int n=snprintf(line,sizeof(line),"PAGE_WAIT_FRAME pid=%ld n=%u module=%s offset=%#lx symbol=%s\n",
        (long)getpid(),state->count,info.dli_fname?info.dli_fname:"?",
        (unsigned long)(ip-(uintptr_t)info.dli_fbase),info.dli_sname?info.dli_sname:"?");
    if(n>0) (void)write(2,line,(size_t)n<sizeof(line)?(size_t)n:sizeof(line)-1);
    return 0;
}
static uint64_t milliseconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void end_call(const char *name, uint64_t start, size_t size, void *caller)
{
    int saved = errno;
    uint64_t now = milliseconds();
    if (depth == 1) {
        for (size_t i=0; i<sizeof(metrics)/sizeof(*metrics); ++i)
            if (!strcmp(metrics[i].name, name)) {
                atomic_fetch_add(&metrics[i].calls, 1);
                atomic_fetch_add(&metrics[i].elapsed, now-start);
                break;
            }
        uint64_t next = atomic_load(&next_summary);
        if (now >= next && atomic_compare_exchange_strong(&next_summary, &next, now+2000)) {
#if PAGE_CYCLE_DIAG
            for (unsigned i=0; i<sizeof(font_cycles)/sizeof(*font_cycles); ++i) {
                unsigned long count=atomic_exchange(&font_cycles[i].calls,0);
                unsigned long ticks=atomic_exchange(&font_cycles[i].cycles,0);
                if(count && atomic_fetch_add(&cycle_reports,1)<(PAGE_FONT_STAGE_DIAG ? 450u : 150u)) {
                    char line[320];
                    int n=snprintf(line,sizeof(line),"PAGE_CYCLE_SUM ms=%llu pid=%ld call=%s calls=%lu cycles=%lu calibration_ms=%llu calibration_cycles=%llu\n",
                        (unsigned long long)now,(long)getpid(),font_cycles[i].name,count,ticks,
                        (unsigned long long)(now-calibration_ms),(unsigned long long)(cycles()-calibration_cycles));
                    if(n>0) (void)write(2,line,(size_t)n<sizeof(line)?(size_t)n:sizeof(line)-1);
                }
            }
#endif
            unsigned long clock_calls=atomic_exchange(&time_calls,0);
            unsigned long clock_cycles=atomic_exchange(&time_cycles,0);
            if(clock_calls && atomic_fetch_add(&summary_reports,1)<100) {
                char line[256];
                int n=snprintf(line,sizeof(line),"PAGE_TIME_SUM ms=%llu pid=%ld calls=%lu cycles=%lu calibration_ms=%llu calibration_cycles=%llu\n",
                    (unsigned long long)now,(long)getpid(),clock_calls,clock_cycles,
                    (unsigned long long)(now-calibration_ms),(unsigned long long)(cycles()-calibration_cycles));
                if(n>0) (void)write(2,line,(size_t)n<sizeof(line)?(size_t)n:sizeof(line)-1);
            }
            for (size_t i=0; i<sizeof(metrics)/sizeof(*metrics); ++i) {
                unsigned long calls=atomic_exchange(&metrics[i].calls, 0);
                unsigned long elapsed=atomic_exchange(&metrics[i].elapsed, 0);
                if (calls && (elapsed >= 20 || i == 6) && atomic_fetch_add(&summary_reports, 1) < 100) {
                    char line[256];
                    int n=snprintf(line,sizeof(line),"PAGE_CALL_SUM ms=%llu pid=%ld call=%s calls=%lu total_ms=%lu\n",
                        (unsigned long long)now,(long)getpid(),metrics[i].name,calls,elapsed);
                    if(n>0) (void)write(2,line,(size_t)n<sizeof(line)?(size_t)n:sizeof(line)-1);
                }
            }
        }
    }
    uint64_t threshold = !strncmp(name, "pthread_cond_", 13) ? 1000 : 50;
    if (depth == 1 && now - start >= threshold && atomic_fetch_add(&reports, 1) < 100) {
        Dl_info info = {0};
        dladdr(caller, &info);
        char line[512];
        int n=snprintf(line, sizeof(line), "PAGE_CALL ms=%llu pid=%ld tid=%ld call=%s elapsed=%llu size=%zu caller=%s+%#lx\n",
            (unsigned long long)now, (long)getpid(), (long)syscall(SYS_gettid),
            name, (unsigned long long)(now-start), size,
            info.dli_fname ? info.dli_fname : "?",
            (unsigned long)((uintptr_t)caller-(uintptr_t)info.dli_fbase));
        if(n>0) (void)write(2,line,(size_t)n<sizeof(line)?(size_t)n:sizeof(line)-1);
    }
    if(depth == 1 && now-start >= 1000 && (long)syscall(SYS_gettid) == main_tid && main_traces++ < 4) {
        int (*backtrace_fn)(int (*)(struct unwind_context *,void *),void *)=dlsym(RTLD_DEFAULT,"_Unwind_Backtrace");
        struct trace_state state={.ip=dlsym(RTLD_DEFAULT,"_Unwind_GetIP")};
        if(backtrace_fn && state.ip) backtrace_fn(trace_frame,&state);
    }
    --depth;
    errno = saved;
}
static int module(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size; (void)data;
    const char *name = info->dlpi_name;
    if (strstr(name, "webkit") || strstr(name, "javascript") ||
        strstr(name, "fontconfig") || strstr(name, "freetype") ||
        strstr(name, "pango") || strstr(name, "cairo") || strstr(name, "musl"))
        dprintf(2, "PAGE_MODULE pid=%ld base=%#lx name=%s\n",
            (long)getpid(), (unsigned long)info->dlpi_addr, name);
    return 0;
}
__attribute__((constructor)) static void loaded(void)
{
    int saved = errno;
    ++depth;
    main_tid=(long)syscall(SYS_gettid);
    calibration_ms=milliseconds();
    calibration_cycles=cycles();
    dprintf(2, "PAGE_PROCESS ms=%llu pid=%ld main_tid=%ld\n",
        (unsigned long long)milliseconds(), (long)getpid(), (long)syscall(SYS_gettid));
    dl_iterate_phdr(module, NULL);
    --depth;
    errno = saved;
}

#define WRAP(result_type, name, signature, arguments, bytes) \
result_type name signature \
{ \
    int saved = errno; \
    typedef result_type (*fn_type) signature; \
    static _Atomic(fn_type) cached; \
    fn_type next = atomic_load(&cached); \
    if (!next) { next = dlsym(RTLD_NEXT, #name); atomic_store(&cached,next); } \
    ++depth; \
    uint64_t start = milliseconds(); \
    errno = saved; \
    result_type call_result = next arguments; \
    end_call(#name, start, bytes, __builtin_return_address(0)); \
    return call_result; \
}

#if PAGE_MMAP_DETAIL
/* Optional classification of slow mappings, never a production preload.
 * Resolve the descriptor only after the measured call; a concurrent close may
 * make the path unavailable, so flags/offset and the return value are primary.
 * Keep a separate bound: idle condition waits can exhaust the generic log. */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t off)
{
    typedef void *(*fn_type)(void *, size_t, int, int, int, off_t);
    static _Atomic(fn_type) cached;
    static _Atomic unsigned details;
    int saved = errno;
    fn_type next = atomic_load(&cached);
    if (!next) { next = dlsym(RTLD_NEXT, "mmap"); atomic_store(&cached, next); }
    ++depth;
    uint64_t start = milliseconds();
    errno = saved;
    void *result = next(addr, length, prot, flags, fd, off);
    int result_errno = errno;
    uint64_t end = milliseconds();
    int outer = depth == 1;
    errno = result_errno;
    end_call("mmap", start, length, __builtin_return_address(0));
    ++depth;
    if (outer && end >= start && end - start >= 50 &&
        atomic_fetch_add(&details, 1) < 48) {
        char descriptor[64], path[256] = "-", line[768];
        if (fd >= 0 && !(flags & MAP_ANONYMOUS)) {
            snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
            ssize_t n = readlink(descriptor, path, sizeof(path) - 1);
            if (n >= 0) path[n] = '\0';
            else strcpy(path, "?");
            for (char *p = path; *p; ++p)
                if ((unsigned char)*p < 32 || *p == 127) *p = '?';
        }
        Dl_info info = {0};
        void *caller = __builtin_return_address(0);
        dladdr(caller, &info);
        int n = snprintf(line, sizeof(line),
            "PAGE_MMAP ms=%llu pid=%ld elapsed=%llu len=%zu prot=%x flags=%x fd=%d offset=%lld result=%p errno=%d caller=%s+%#lx path=%s\n",
            (unsigned long long)end, (long)getpid(),
            (unsigned long long)(end - start), length, prot, flags, fd,
            (long long)off, result, result == MAP_FAILED ? result_errno : 0,
            info.dli_fname ? info.dli_fname : "?",
            (unsigned long)((uintptr_t)caller - (uintptr_t)info.dli_fbase), path);
        if (n > 0) (void)write(2, line,
            (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    }
    --depth;
    errno = result_errno;
    return result;
}
#else
WRAP(void *, mmap, (void *addr, size_t length, int prot, int flags, int fd, off_t off),
     (addr, length, prot, flags, fd, off), length)
#endif
WRAP(int, munmap, (void *addr, size_t length), (addr, length), length)
WRAP(int, mprotect, (void *addr, size_t length, int prot), (addr, length, prot), length)
WRAP(int, madvise, (void *addr, size_t length, int advice), (addr, length, advice), length)
WRAP(void *, FcFontMatch, (void *config, void *pattern, int *result),
     (config, pattern, result), 0)
WRAP(void *, FcFontSort, (void *config, void *pattern, int trim, void **charset, int *result),
     (config, pattern, trim, charset, result), 0)
void *FcFontSetMatch(void *config, void **sets, int nsets, void *pattern, int *result)
{
    typedef void *(*fn_type)(void *,void **,int,void *,int *);
    static _Atomic(fn_type) cached;
    static _Atomic unsigned traced;
    int saved=errno;
    fn_type next=atomic_load(&cached);
    if(!next) { next=dlsym(RTLD_NEXT,"FcFontSetMatch"); atomic_store(&cached,next); }
    atomic_fetch_add(&metrics[6].calls,1); /* Count only: no per-match clocks. */
    if(!atomic_exchange(&traced,1)) {
        ++depth;
        Dl_info caller={0};
        void *address=__builtin_return_address(0);
        dladdr(address,&caller);
        dprintf(2,"PAGE_FONT_CALLER pid=%ld module=%s offset=%#lx symbol=%s\n",
            (long)getpid(),caller.dli_fname?caller.dli_fname:"?",
            (unsigned long)((uintptr_t)address-(uintptr_t)caller.dli_fbase),
            caller.dli_sname?caller.dli_sname:"?");
        int (*backtrace_fn)(int (*)(struct unwind_context *,void *),void *)=dlsym(RTLD_DEFAULT,"_Unwind_Backtrace");
        struct trace_state state={.ip=dlsym(RTLD_DEFAULT,"_Unwind_GetIP")};
        if(backtrace_fn && state.ip) backtrace_fn(trace_frame,&state);
        --depth;
    }
    errno=saved;
#if PAGE_CYCLE_DIAG
    uint64_t start=cycles();
    void *matched=next(config,sets,nsets,pattern,result);
    uint64_t elapsed=cycles()-start;
    atomic_fetch_add(&font_cycles[1].calls,1);
    atomic_fetch_add(&font_cycles[1].cycles,elapsed);
    return matched;
#else
    return next(config,sets,nsets,pattern,result);
#endif
}
#if PAGE_CYCLE_DIAG
/* Attribution only. No replacement results, caches, or font settings. TSC
 * measurements add no syscalls per call; nested/scheduling time is included. */
void *FcConfigCreate(void)
{
    typedef void *(*fn_type)(void);
    static _Atomic(fn_type) cached;
    int saved=errno;
    fn_type next=atomic_load(&cached);
    if(!next) { next=dlsym(RTLD_NEXT,"FcConfigCreate"); atomic_store(&cached,next); }
    errno=saved;
    uint64_t start=cycles();
    void *result=next();
    uint64_t elapsed=cycles()-start;
    atomic_fetch_add(&font_cycles[0].calls,1);
    atomic_fetch_add(&font_cycles[0].cycles,elapsed);
    return result;
}
void FcConfigDestroy(void *config)
{
    typedef void (*fn_type)(void *);
    static _Atomic(fn_type) cached;
    int saved=errno;
    fn_type next=atomic_load(&cached);
    if(!next) { next=dlsym(RTLD_NEXT,"FcConfigDestroy"); atomic_store(&cached,next); }
    errno=saved;
    uint64_t start=cycles();
    next(config);
    uint64_t elapsed=cycles()-start;
    atomic_fetch_add(&font_cycles[2].calls,1);
    atomic_fetch_add(&font_cycles[2].cycles,elapsed);
}
#endif
WRAP(void *, FcFontSetSort, (void *config, void **sets, int nsets, void *pattern, int trim, void **charset, int *result),
     (config, sets, nsets, pattern, trim, charset, result), 0)
WRAP(void *, FcInitLoadConfigAndFonts, (void), (), 0)
WRAP(int, FcConfigBuildFonts, (void *config), (config), 0)
#if PAGE_FONT_STAGE_DIAG && PAGE_CYCLE_DIAG
/* Public fontconfig ABI signatures were checked against the packaged header.
 * These optional wrappers only measure existing calls: no font configuration,
 * results, allocation policy or production launcher changes. Intervals overlap
 * with child calls and include descheduling. No clock syscall per call. */
#define FONT_STAGE_HEAD(name) \
    static _Atomic(fn_type) cached; \
    int saved = errno; \
    fn_type next = atomic_load(&cached); \
    if (!next) { next = dlsym(RTLD_NEXT, #name); atomic_store(&cached, next); } \
    errno = saved; \
    uint64_t start = cycles();
#define FONT_STAGE_TAIL(index) \
    uint64_t elapsed = cycles() - start; \
    atomic_fetch_add(&font_cycles[index].calls, 1); \
    atomic_fetch_add(&font_cycles[index].cycles, elapsed);
#define FONT_STAGE_RET(type, name, signature, arguments, index) \
type name signature { \
    typedef type (*fn_type) signature; \
    FONT_STAGE_HEAD(name) \
    type result = next arguments; \
    FONT_STAGE_TAIL(index) \
    return result; \
}
#define FONT_STAGE_VOID(name, signature, arguments, index) \
void name signature { \
    typedef void (*fn_type) signature; \
    FONT_STAGE_HEAD(name) \
    next arguments; \
    FONT_STAGE_TAIL(index) \
}
FONT_STAGE_RET(void *, FcPatternCreate, (void), (), 3)
FONT_STAGE_RET(void *, FcPatternDuplicate, (const void *p), (p), 4)
FONT_STAGE_VOID(FcPatternDestroy, (void *p), (p), 5)
FONT_STAGE_RET(int, FcPatternAddString,
    (void *p, const char *object, const unsigned char *s), (p, object, s), 6)
FONT_STAGE_RET(int, FcPatternAddLangSet,
    (void *p, const char *object, const void *ls), (p, object, ls), 7)
FONT_STAGE_RET(void *, FcFontSetCreate, (void), (), 8)
FONT_STAGE_VOID(FcFontSetDestroy, (void *s), (s), 9)
FONT_STAGE_RET(int, FcFontSetAdd, (void *s, void *p), (s, p), 10)
FONT_STAGE_RET(void *, FcLangSetCreate, (void), (), 11)
FONT_STAGE_VOID(FcLangSetDestroy, (void *ls), (ls), 12)
FONT_STAGE_RET(int, FcLangSetAdd, (void *ls, const unsigned char *s), (ls, s), 13)
FONT_STAGE_VOID(FcDefaultSubstitute, (void *p), (p), 14)
FONT_STAGE_RET(int, FcConfigSubstitute,
    (void *config, void *pattern, int kind), (config, pattern, kind), 15)
FONT_STAGE_RET(void *, FcPatternFilter,
    (void *pattern, const void *objects), (pattern, objects), 16)
/* This linked-list operation can execute millions of times. Sample one in
 * 256 per thread; the reported calls/cycles are samples, NOT full totals.
 * The other 255 calls add no TSC reads, syscalls or shared counter writes. */
int FcPatternRemove(void *pattern, const char *object, int id)
{
    typedef int (*fn_type)(void *, const char *, int);
    static _Atomic(fn_type) cached;
    static _Thread_local unsigned ordinal;
    int saved = errno;
    fn_type next = atomic_load(&cached);
    if (!next) { next = dlsym(RTLD_NEXT, "FcPatternRemove"); atomic_store(&cached, next); }
    errno = saved;
    if (ordinal++ & 255u) return next(pattern, object, id);
    uint64_t start = cycles();
    int result = next(pattern, object, id);
    FONT_STAGE_TAIL(17)
    return result;
}
#undef FONT_STAGE_VOID
#undef FONT_STAGE_RET
#undef FONT_STAGE_TAIL
#undef FONT_STAGE_HEAD
#else
WRAP(int, FcConfigSubstitute, (void *config, void *pattern, int kind), (config, pattern, kind), 0)
#endif
WRAP(int, pthread_cond_wait, (pthread_cond_t *cond, pthread_mutex_t *mutex), (cond, mutex), 0)
WRAP(int, pthread_cond_timedwait, (pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline), (cond, mutex, deadline), 0)

/* Count the actual fontconfig time() path without adding a clock syscall per
 * call. Cycles include scheduling; calibration is diagnostic, not a new clock. */
time_t time(time_t *out)
{
    typedef time_t (*fn_type)(time_t *);
    static _Atomic(fn_type) cached;
    int saved=errno;
    fn_type next=atomic_load(&cached);
    if(!next) { next=dlsym(RTLD_NEXT,"time"); atomic_store(&cached,next); }
    errno=saved;
    uint64_t start=cycles();
    time_t result=next(out);
    uint64_t elapsed=cycles()-start;
    atomic_fetch_add(&time_calls,1);
    atomic_fetch_add(&time_cycles,elapsed);
    return result;
}
