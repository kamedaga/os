#include <unixd/profile.h>
#if defined(LPR_UNIX_PROFILE) && LPR_UNIX_PROFILE
#include "../lpr_filed_internal.h"

enum { PROFILE_KEYS = 40 };
struct metric { uint64_t count, ticks; };
static struct metric metrics[UP_GROUPS][PROFILE_KEYS][UP_STAGES];
static unsigned dumped;

void unix_profile_end(struct unix_profile_scope *scope)
{
    if (!scope->start) return;
    const uint64_t end = unix_profile_clock(), start = scope->start;
    scope->start = 0;
    if (end < start || scope->group >= UP_GROUPS || scope->key >= PROFILE_KEYS ||
        scope->stage >= UP_STAGES) return;
    struct metric *m = &metrics[scope->group][scope->key][scope->stage];
    __atomic_fetch_add(&m->count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&m->ticks, end - start, __ATOMIC_RELAXED);
}

static void output(unsigned group, unsigned key, unsigned stage, uint64_t count, uint64_t ticks)
{
    char line[128] = "UNIX_PROFILE";
    unsigned n = 12;
    const uint64_t values[] = { lpr_linux_current_pid, group, key, stage, count, ticks };
    for (unsigned i = 0; i < 6; i++) {
        line[n++] = ' ';
        for (int shift = 60; shift >= 0; shift -= 4)
            line[n++] = "0123456789abcdef"[(values[i] >> shift) & 15];
    }
    line[n++] = '\n';
#if defined(LPR_UNIX_PROFILE_STDERR) && LPR_UNIX_PROFILE_STDERR
    (void)lpr_linux_write(2, (uint64_t)(uintptr_t)line, n, NULL);
#else
    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_LOG, (uint64_t)(uintptr_t)line, n);
#endif
}

void unix_profile_fd_inventory(unsigned phase)
{
    static unsigned started;
    if(!phase && __atomic_exchange_n(&started,1,__ATOMIC_RELAXED))return;
    struct pacha_fd_table_info info={0};
    if(!lpr_pacha_syscall2(PACHAOS_SYSCALL_FD_TABLE,0,(uintptr_t)&info))
        output(UP_FD_TRANSFER,39,phase,info.capacity-info.free_slots,info.capacity);
}

void unix_profile_dump(void)
{
    if (__atomic_exchange_n(&dumped, 1, __ATOMIC_RELAXED)) return;
#if defined(LPR_UNIX_PROFILE_KERNEL_LOG) && LPR_UNIX_PROFILE_KERNEL_LOG
    uint64_t cursor=0;
    for(unsigned i=0;i<64;i++) {
        struct pacha_kernel_log_read log={.cursor=cursor,.limit=PACHA_KERNEL_LOG_READ_MAX_BYTES};
        int64_t status=lpr_pacha_syscall2(PACHA_KERNEL_LOG_SYSCALL_READ,(uintptr_t)&log,sizeof(log));
        if(status) { output(UP_GROUPS,1,0,0,(uint64_t)status);break; }
        if(log.written) (void)lpr_linux_write(2,(uintptr_t)log.bytes,log.written,NULL);
        if(!log.written||log.next_cursor==cursor)break;
        cursor=log.next_cursor;
    }
#endif
    unix_profile_fd_inventory(1);
    int any = 0;
    for (unsigned g = 0; g < UP_GROUPS; g++)
        for (unsigned k = 0; k < PROFILE_KEYS; k++)
            for (unsigned s = 0; s < UP_STAGES; s++) {
                struct metric *m = &metrics[g][k][s];
                const uint64_t count = __atomic_load_n(&m->count, __ATOMIC_RELAXED);
                if (count) { output(g, k, s, count,
                    __atomic_load_n(&m->ticks, __ATOMIC_RELAXED)); any = 1; }
            }
    if (any) {
        uint64_t overhead = 0;
        for (unsigned i = 0; i < 1024; i++) {
            const uint64_t start = unix_profile_clock();
            overhead += unix_profile_clock() - start;
        }
        output(UP_GROUPS, 0, 0, 1024, overhead);
    }
}
#endif
