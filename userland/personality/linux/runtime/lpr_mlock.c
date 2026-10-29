#include "lpr_mlock.h"
#include "lpr_filed_internal.h"

typedef struct { uint64_t first,end; } locked_range_t;
static struct {
    volatile uint32_t lock;
    locked_range_t *ranges;
    uint64_t count,capacity,bytes;
} locked;

static int range(uint64_t address,uint64_t length,locked_range_t *out)
{
    if (address>UINT64_MAX-length || address+length>UINT64_MAX-4095) return 0;
    out->first=address&~4095ull;
    out->end=(address+length+4095)&~4095ull;
    return 1;
}

static int64_t reserve(uint64_t count)
{
    if (count<=locked.capacity) return 0;
    if (count>UINT64_MAX/sizeof(locked_range_t)/2) return -LPR_LINUX_ENOMEM;
    uint64_t bytes=count*2*sizeof(locked_range_t);
    if (bytes>UINT64_MAX-4095) return -LPR_LINUX_ENOMEM;
    bytes=(bytes+4095)&~4095ull;
    const int64_t mapped=lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP,0,0,bytes,
        PACHAOS_PROT_READ|PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_PRIVATE|PACHAOS_MMAP_ANONYMOUS,0);
    if (mapped<4096) return -LPR_LINUX_ENOMEM;
    if (locked.count) lpr_memcpy((void *)(uintptr_t)mapped,locked.ranges,
        locked.count*sizeof(locked_range_t));
    if (locked.ranges && lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
        (uintptr_t)locked.ranges,locked.bytes)) {
        /* Keep the old allocation and interval ownership if replacement fails. */
        if (lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,(uint64_t)mapped,bytes)) {
            /* Neither mapping can be abandoned without losing ownership. */
            (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_PROCESS_EXIT,127);
            __builtin_trap();
        }
        return -LPR_LINUX_ENOMEM;
    }
    locked.ranges=(void *)(uintptr_t)mapped;
    locked.capacity=bytes/sizeof(locked_range_t);locked.bytes=bytes;
    return 0;
}

static void release_empty(void)
{
    if (locked.count || !locked.ranges) return;
    if (lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,
        (uintptr_t)locked.ranges,locked.bytes)) return;
    locked.ranges=0;locked.capacity=0;locked.bytes=0;
}

int64_t lpr_mlock_vm_begin(void)
{
    lpr_state_lock(&locked.lock);
    const int64_t status=locked.count ? reserve(locked.count+1) : 0;
    if (status) lpr_state_unlock(&locked.lock);
    return status;
}

void lpr_mlock_vm_end(void) { lpr_state_unlock(&locked.lock); }
volatile uint32_t *lpr_mlock_lock_address(void) { return &locked.lock; }
void lpr_mlock_fork_lock(void) { lpr_state_lock(&locked.lock); }

void lpr_mlock_after_fork_child(void)
{
    /* Fork snapshots while this lock is held. Memory locks are not inherited;
     * the child owns its copied metadata mapping and can reclaim it directly. */
    locked.count=0;release_empty();locked.lock=0;
}

int lpr_mlock_overlaps(uint64_t address,uint64_t length)
{
    locked_range_t target;
    if (!range(address,length,&target)) return locked.count!=0;
    for (uint64_t i=0;i<locked.count;++i)
        if (locked.ranges[i].first<target.end && target.first<locked.ranges[i].end) return 1;
    return 0;
}

void lpr_mlock_forget(uint64_t address,uint64_t length)
{
    locked_range_t target;
    if (!range(address,length,&target) || target.first==target.end) return;
    for (uint64_t i=0;i<locked.count;) {
        locked_range_t old=locked.ranges[i];
        if (old.end<=target.first) { ++i;continue; }
        if (old.first>=target.end) break;
        if (old.first<target.first && old.end>target.end) {
            lpr_memmove(&locked.ranges[i+2],&locked.ranges[i+1],
                (locked.count-i-1)*sizeof(old));
            locked.ranges[i].end=target.first;
            locked.ranges[i+1]=(locked_range_t){target.end,old.end};
            ++locked.count;break;
        }
        if (old.first<target.first) { locked.ranges[i].end=target.first;++i;continue; }
        if (old.end>target.end) { locked.ranges[i].first=target.end;break; }
        lpr_memmove(&locked.ranges[i],&locked.ranges[i+1],
            (locked.count-i-1)*sizeof(old));
        --locked.count;
    }
    release_empty();
}

int64_t lpr_linux_mlock(uint64_t address,uint64_t length,uint64_t limit)
{
    locked_range_t target;
    if (!range(address,length,&target)) return -LPR_LINUX_EINVAL;
    if (target.first==target.end) return 0;
    lpr_state_lock(&locked.lock);
    uint64_t used=0,added=target.end-target.first;
    for (uint64_t i=0;i<locked.count;++i) {
        locked_range_t old=locked.ranges[i];
        used+=old.end-old.first;
        uint64_t first=old.first>target.first ? old.first : target.first;
        uint64_t end=old.end<target.end ? old.end : target.end;
        if (first<end) added-=end-first;
    }
    if (!limit || used>limit || added>limit-used) {
        lpr_state_unlock(&locked.lock);
        return limit ? -LPR_LINUX_ENOMEM : -LPR_LINUX_EPERM;
    }
    int64_t status=reserve(locked.count+1);
    if (!status) {
        /* Native populated backing is non-pageable while its mapping/VMO
         * reference lives. WRITE prefault also commits private COW now; a
         * READ fallback would hide failed allocation or unsupported access.
         * This initial implementation rejects non-writable ranges. */
        status=lpr_pacha_syscall3(PACHA_VM_SYSCALL_MADVISE,target.first,
            target.end-target.first,PACHA_VM_MADV_POPULATE_WRITE);
        if (status) status=-LPR_LINUX_ENOMEM;
    }
    if (!status) {
        uint64_t i=0;
        while (i<locked.count && locked.ranges[i].end<target.first) ++i;
        uint64_t end=i;
        while (end<locked.count && locked.ranges[end].first<=target.end) {
            if (locked.ranges[end].first<target.first) target.first=locked.ranges[end].first;
            if (locked.ranges[end].end>target.end) target.end=locked.ranges[end].end;
            ++end;
        }
        lpr_memmove(&locked.ranges[i+1],&locked.ranges[end],
            (locked.count-end)*sizeof(target));
        locked.ranges[i]=target;
        locked.count=locked.count-(end-i)+1;
    }
    release_empty();lpr_state_unlock(&locked.lock);
    return status;
}

int64_t lpr_linux_munlock(uint64_t address,uint64_t length)
{
    locked_range_t target;
    if (!range(address,length,&target)) return -LPR_LINUX_EINVAL;
    if (target.first==target.end) return 0;
    int64_t status=lpr_mlock_vm_begin();
    if (status) return status;
    status=lpr_pacha_syscall3(PACHA_VM_SYSCALL_MADVISE,target.first,
        target.end-target.first,0); /* Coverage check, including PROT_NONE. */
    if (!status) lpr_mlock_forget(address,length);
    lpr_mlock_vm_end();
    return status ? -LPR_LINUX_ENOMEM : 0;
}
