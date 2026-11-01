/* SPDX-License-Identifier: GPL-2.0-only */
/* Actual lease engine + actual early_boot hooks. Only Linux CPA, memtype,
 * allocation, PTE read and native host syscall boundaries are modeled. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include "../kobox2/linux-sandbox/kobox/memory/cache_lease.h"
typedef uint64_t u64;
#define PAGE_SHIFT 12
#define PAGE_SIZE 4096UL
#define PAGE_MASK (~(PAGE_SIZE-1))
#define PAGE_ALIGN(a) (((a)+PAGE_SIZE-1)&PAGE_MASK)
#define TLB_FLUSH_ALL ULONG_MAX
#define GFP_KERNEL 0
#define _PAGE_CACHE_MODE_WC 3
#define _PAGE_PRESENT 1u
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define EXPORT_SYMBOL_GPL(name)
#define DEFINE_MUTEX(name) pthread_mutex_t name=PTHREAD_MUTEX_INITIALIZER
#define DEFINE_RAW_SPINLOCK(name) pthread_mutex_t name=PTHREAD_MUTEX_INITIALIZER
#define DEFINE_PER_CPU(type,name) _Thread_local type name
#define this_cpu_write(name,value) ((name)=(value))
#define this_cpu_xchg(name,value) ({ __typeof__(name) old=(name);(name)=(value);old; })
#define mutex_lock(p) assert(!pthread_mutex_lock(p))
#define mutex_unlock(p) assert(!pthread_mutex_unlock(p))
#define raw_spin_lock_irqsave(p,flags) do { (flags)=0;test_raw_lock(p); } while(0)
#define raw_spin_unlock_irqrestore(p,flags) do { (void)(flags);mutex_unlock(p); } while(0)
#define kobox_host_call(call) (call)
#define KOBOX_BOOT_RUNTIME 1
#define BUG() abort()
#define panic(...) do { fprintf(stderr,"panic: %s:%d\n",__FILE__,__LINE__); abort(); } while(0)
#define pr_err(...) fprintf(stderr,__VA_ARGS__)
#define RAM_PAGES 32u
#define BASE 0x100000UL
static DEFINE_RAW_SPINLOCK(kernel_alias_lock);
static pthread_mutex_t race_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t race_condition=PTHREAD_COND_INITIALIZER;
static bool race_mode,creator_waiting,mutate_at_owner_get;
static _Thread_local bool is_creator;
static void test_raw_lock(pthread_mutex_t *lock) {
    if(race_mode && is_creator && lock==&kernel_alias_lock) {
        mutex_lock(&race_gate);creator_waiting=true;
        assert(!pthread_cond_signal(&race_condition));mutex_unlock(&race_gate);
    }
    mutex_lock(lock);
}
struct page { unsigned long pfn;unsigned refs;bool reserved,high,device; };
static struct page ram[RAM_PAGES];
static unsigned native_cache[RAM_PAGES],shadow_cache[RAM_PAGES],shadow_prot[RAM_PAGES];
static bool reservations[RAM_PAGES];
static unsigned reserve_calls,free_calls,free_per_page[RAM_PAGES],owner_gets,owner_puts;
static unsigned alloc_calls,alloc_fail,live_allocs,owner_fail;
static int native_fail_mode=-1,partial_fail_mode=-1,reserve_fail_page=-1,free_fail_page=-1;
static unsigned native_failures_left,partial_failures_left,wb_failures_left;
static unsigned native_attempts[4],explicit_wb_calls,mirror_calls,remote_calls;
static unsigned mirror_failures_left,mirror_fail_after,mirror_fail_cache;
static bool remote_mirror,break_shadow_check,arm_wb_fail_on_partial;
static pthread_t owner_thread;
static bool target[RAM_PAGES];
static void *kzalloc(size_t bytes,unsigned flags) {
    assert(flags==GFP_KERNEL);if(++alloc_calls==alloc_fail)return NULL;
    void *p=calloc(1,bytes);assert(p);++live_allocs;return p;
}
static void *kvmalloc_array(size_t n,size_t bytes,unsigned flags) {
    assert(n && n<=SIZE_MAX/bytes);return kzalloc(n*bytes,flags);
}
static void kfree(void *p) { if(p) { assert(live_allocs);--live_allocs;free(p); } }
static void kvfree(void *p) { kfree(p); }
static bool PageHighMem(struct page *p) { return p->high; }
static bool PageReserved(struct page *p) { return p->reserved; }
static bool is_zone_device_page(struct page *p) { return p->device; }
static int page_count(struct page *p) { return (int)p->refs; }
static unsigned long page_to_pfn(struct page *p) { return p->pfn; }
static void *page_address(struct page *p) { return (void *)(BASE+(p->pfn<<PAGE_SHIFT)); }
static void get_page(struct page *p) { ++p->refs; }
static void put_page(struct page *p) { assert(p->refs>1);--p->refs; }
static void sort(void *p,size_t count,size_t size,int (*cmp)(const void *,const void *),void *swap) {
    assert(!swap);qsort(p,count,size,cmp);
}
static int memtype_reserve(u64 start,u64 end,unsigned mode,void *actual) {
    unsigned p=(unsigned)(start>>PAGE_SHIFT);
    assert(end==start+PAGE_SIZE && p<RAM_PAGES && mode==_PAGE_CACHE_MODE_WC && !actual);
    ++reserve_calls;
    if((int)p==reserve_fail_page)return -EBUSY;
    if(reservations[p])return -EBUSY;
    reservations[p]=true;return 0;
}
static int memtype_free(u64 start,u64 end) {
    unsigned p=(unsigned)(start>>PAGE_SHIFT);
    assert(end==start+PAGE_SIZE && p<RAM_PAGES && reservations[p]);
    if((int)p==free_fail_page)return -EIO;
    reservations[p]=false;++free_per_page[p];++free_calls;return 0;
}
static int owner_get(void *p) {
    assert(p==&ram);++owner_gets;
    if(mutate_at_owner_get)shadow_cache[4]=3;
    return owner_fail?-EACCES:0;
}
static void owner_put(void *p) { assert(p==&ram);++owner_puts; }
static const struct kobox_cache_owner_ops owner_ops={owner_get,owner_put};
static int _set_memory_wc(unsigned long address,int pages);
static int _set_memory_wb(unsigned long address,int pages);
#include "cache_lease_actual.inc"

typedef struct { unsigned long pfn;unsigned cache,prot; } pte_t;
static const unsigned long page_offset_base=BASE,vmalloc_base=0x200000;
static const unsigned long kobox_memory_vmalloc_end=0x1fffff;
static bool direct_tables_ready=true;
static DEFINE_PER_CPU(bool,kobox_cpa_drain_ready);
static pte_t kernel_pte(unsigned long address) {
    unsigned long pfn=(address-BASE)>>PAGE_SHIFT;assert(pfn<RAM_PAGES);
    return (pte_t){pfn,shadow_cache[pfn],shadow_prot[pfn]};
}
static unsigned long pte_flags(pte_t p) { return p.prot?_PAGE_PRESENT:0; }
static unsigned long pte_pfn(pte_t p) { return p.pfn; }
static unsigned host_pte_cache(pte_t p) { return p.cache; }
static unsigned host_pte_protection(pte_t p) { return p.prot; }
static int host_set_cache(void *backing,size_t offset,size_t bytes,unsigned cache) {
    assert(backing==ram && !(offset%PAGE_SIZE) && !(bytes%PAGE_SIZE) && bytes);
    size_t first=offset>>PAGE_SHIFT,count=bytes>>PAGE_SHIFT;assert(first+count<=RAM_PAGES && cache<4);
    bool owned=false;for(size_t i=first;i<first+count;++i)owned|=target[i];
    if(owned) { ++native_attempts[cache];if(cache==0)++explicit_wb_calls; }
    if(owned && cache==0 && wb_failures_left) { --wb_failures_left;return -ENOMEM; }
    if(cache==(unsigned)native_fail_mode && native_failures_left) {
        --native_failures_left;return -EIO;
    }
    if(owned && cache==(unsigned)partial_fail_mode && partial_failures_left) {
        --partial_failures_left;
        /* A callback can contain several native syscalls: the first succeeds,
         * the later one fails. Nonzero does not mean this span is unchanged. */
        native_cache[first]=cache;
        if(arm_wb_fail_on_partial)wb_failures_left=1;
        return -EAGAIN;
    }
    for(size_t i=first;i<first+count;++i)native_cache[i]=cache;
    return 0;
}
static int host_protect(void *window,size_t offset,size_t bytes,unsigned prot) {
    assert(window==ram && offset+bytes<=RAM_PAGES*PAGE_SIZE && prot==3);return 0;
}
static int host_cache_flush(void) { return 0; } /* Deliberately stale-success host. */
static int mapped_range(unsigned long a,unsigned long b,void *w,unsigned long base,bool publish) {
    (void)a;(void)b;(void)w;(void)base;(void)publish;assert(0);return -EIO;
}
static int reset_kernel_window(void *context,unsigned long a,unsigned long b) {
    (void)context;(void)a;(void)b;assert(0);return -EIO;
}
static int kobox_mmio_reset(unsigned long a,unsigned long b,void *context,
    int (*reset)(void *,unsigned long,unsigned long)) {
    return reset(context,a,b);
}
static const struct kobox_linux_memory_host_operations host_ops={
    .set_cache=host_set_cache,.protect=host_protect,.cache_flush=host_cache_flush};
static const struct kobox_linux_memory_layout layout={.operations=&host_ops,
    .ram_backing=ram,.direct_window=ram,.ram_size=RAM_PAGES*PAGE_SIZE};
static const struct kobox_linux_memory_layout *memory_layout=&layout;
#include "cache_bridge_actual.inc"
static void *flush_remote(void *unused) {
    (void)unused;assert(!pthread_equal(pthread_self(),owner_thread));++remote_calls;
    flush_tlb_kernel_range(0,TLB_FLUSH_ALL);
    kobox_cpa_wbinvd();
    assert(!kobox_cache_lease_flush_ack() || cache_leases);
    return NULL;
}
static void flush_shadow(void) {
    if(remote_mirror) {
        pthread_t thread;assert(!pthread_create(&thread,NULL,flush_remote,NULL));
        assert(!pthread_join(thread,NULL));
    } else {
        flush_tlb_kernel_range(0,TLB_FLUSH_ALL);kobox_cpa_wbinvd();
    }
}
static int shadow_phase(unsigned long address,int pages,unsigned cache) {
    unsigned first=(unsigned)((address-BASE)>>PAGE_SHIFT);bool changed=false;
    assert(first+(unsigned)pages<=RAM_PAGES);
    unsigned limit=(unsigned)pages;
    bool fail=mirror_failures_left && cache==mirror_fail_cache;
    if(fail) { --mirror_failures_left;limit=min(limit,mirror_fail_after); }
    for(unsigned i=first;i<first+limit;++i) {
        changed|=shadow_cache[i]!=cache;shadow_cache[i]=cache;
    }
    if(changed || fail)flush_shadow();
    if(break_shadow_check) { shadow_cache[first]=KOBOX_LINUX_MEMORY_CACHE_UC;break_shadow_check=false; }
    return fail?-ENOMEM:0;
}
static int _set_memory_wc(unsigned long address,int pages) {
    ++mirror_calls;
    int error=shadow_phase(address,pages,KOBOX_LINUX_MEMORY_CACHE_UC_MINUS);
    return error?error:shadow_phase(address,pages,KOBOX_LINUX_MEMORY_CACHE_WC);
}
static int _set_memory_wb(unsigned long address,int pages) {
    ++mirror_calls;return shadow_phase(address,pages,KOBOX_LINUX_MEMORY_CACHE_WB);
}
static void reset(void) {
    assert(!cache_leases && !live_allocs);
    for(unsigned i=0;i<RAM_PAGES;++i) {
        ram[i]=(struct page){.pfn=i,.refs=1};
        native_cache[i]=shadow_cache[i]=0;shadow_prot[i]=3;reservations[i]=false;
        target[i]=false;free_per_page[i]=0;
    }
    reserve_calls=free_calls=owner_gets=owner_puts=alloc_calls=alloc_fail=owner_fail=0;
    native_fail_mode=partial_fail_mode=reserve_fail_page=free_fail_page=-1;
    native_failures_left=partial_failures_left=wb_failures_left=0;
    memset(native_attempts,0,sizeof(native_attempts));explicit_wb_calls=mirror_calls=remote_calls=0;
    mirror_failures_left=mirror_fail_after=0;mirror_fail_cache=3;
    remote_mirror=break_shadow_check=arm_wb_fail_on_partial=false;
    owner_thread=pthread_self();kobox_cpa_drain_ready=false;
    race_mode=creator_waiting=mutate_at_owner_get=false;
}
static struct kobox_cache_lease *make(bool scattered) {
    struct page *pages[]={&ram[6],&ram[4],&ram[scattered?10:5]};
    for(unsigned i=0;i<3;++i)target[pages[i]->pfn]=true;
    struct kobox_cache_lease *lease=NULL;
    assert(!kobox_cache_lease_create(pages,3,&owner_ops,&ram,&lease));assert(lease);
    assert(owner_gets==1 && !owner_puts);
    return lease;
}
static struct kobox_cache_lease_status status(struct kobox_cache_lease *lease) {
    struct kobox_cache_lease_status s;assert(!kobox_cache_lease_status(lease,&s));return s;
}
static void all_wb(void) {
    for(unsigned i=0;i<RAM_PAGES;++i)if(target[i]) {
        assert(native_cache[i]==0 && shadow_cache[i]==0 && !reservations[i]);
    }
}
static void destroy(struct kobox_cache_lease **lease) {
    assert(!kobox_cache_lease_destroy(lease) && !*lease);
    assert(owner_gets==owner_puts && !live_allocs);
    for(unsigned i=0;i<RAM_PAGES;++i)assert(ram[i].refs==1);
}
static void normal(void) {
    for(unsigned mode=0;mode<=3;mode+=3)for(unsigned scattered=0;scattered<2;++scattered) {
        reset();struct kobox_cache_lease *lease=make(scattered);remote_mirror=true;
        assert(!kobox_cache_lease_acquire(lease,mode));
        assert(status(lease).state==KOBOX_CACHE_ACTIVE && !kobox_cache_lease_flush_ack());
        for(unsigned i=0;i<RAM_PAGES;++i)if(target[i])assert(native_cache[i]==mode && shadow_cache[i]==mode);
        assert(kobox_cache_lease_destroy(&lease)==-EBUSY && lease && !owner_puts);
        assert(!kobox_cache_lease_restore_wb(lease));all_wb();
        unsigned frees_before=free_calls;
        assert(!kobox_cache_lease_restore_wb(lease) && free_calls==frees_before);
        if(mode) { assert(remote_calls && reserve_calls==3 && free_calls==3); }
        destroy(&lease);
    }
}
static void refusal_and_allocation(void) {
    reset();struct page *pages[]={&ram[4],&ram[4]};struct kobox_cache_lease *lease=NULL;
    assert(kobox_cache_lease_create(pages,2,&owner_ops,&ram,&lease)==-EINVAL && !lease && !live_allocs);
    for(unsigned site=1;site<=2;++site) {
        reset();alloc_fail=site;
        assert(kobox_cache_lease_create(pages,1,&owner_ops,&ram,&lease)==-ENOMEM && !lease && !live_allocs);
    }
    reset();owner_fail=1;
    assert(kobox_cache_lease_create(pages,1,&owner_ops,&ram,&lease)==-EACCES && !lease && !live_allocs && !owner_puts);
    reset();ram[4].reserved=true;
    assert(kobox_cache_lease_create(pages,1,&owner_ops,&ram,&lease)==-EINVAL && !lease);
    reset();shadow_cache[4]=3;
    assert(kobox_cache_lease_create(pages,1,&owner_ops,&ram,&lease)==-EBUSY && !lease);
    reset();lease=make(false);struct kobox_cache_lease *other=NULL;
    assert(kobox_cache_lease_create(pages,1,&owner_ops,&ram,&other)==-EBUSY && !other);
    assert(kobox_cache_lease_acquire(lease,2)==-EINVAL);
    assert(!kobox_cache_lease_restore_wb(lease));destroy(&lease);
}
static void partial_and_phase_failures(void) {
    for(unsigned mode=1;mode<=3;mode+=2) {
        reset();struct kobox_cache_lease *lease=make(false);
        partial_fail_mode=(int)mode;partial_failures_left=1;
        assert(kobox_cache_lease_acquire(lease,3)==-EAGAIN);
        assert(status(lease).state==KOBOX_CACHE_RESTORED && status(lease).native_error==-EAGAIN);
        assert(mirror_calls==1); /* Only WB rollback; its PTE edit is a no-op. */
        all_wb();assert(explicit_wb_calls>=2);destroy(&lease);
    }
    reset();struct kobox_cache_lease *lease=make(true);reserve_fail_page=6;
    assert(kobox_cache_lease_acquire(lease,3)==-EBUSY);all_wb();
    assert(free_per_page[4]==1 && free_per_page[6]==0 && free_per_page[10]==0);destroy(&lease);
    reset();lease=make(false);mirror_failures_left=1;mirror_fail_after=1;
    assert(kobox_cache_lease_acquire(lease,3)==-ENOMEM);
    assert(status(lease).cpa_error==-ENOMEM);all_wb();assert(free_calls==3);destroy(&lease);
    reset();lease=make(false);wb_failures_left=1;
    assert(kobox_cache_lease_acquire(lease,3)==-ENOMEM);
    assert(status(lease).state==KOBOX_CACHE_RESTORED && !reserve_calls);
    all_wb();destroy(&lease);
}
static void retained_retry(void) {
    reset();struct kobox_cache_lease *lease=make(false);
    assert(!kobox_cache_lease_acquire(lease,3));
    wb_failures_left=1;
    assert(kobox_cache_lease_restore_wb(lease)==-EIO);
    assert(status(lease).state==KOBOX_CACHE_RETAINED && status(lease).restore_error==-ENOMEM);
    assert(!free_calls && !owner_puts && live_allocs==2);
    assert(kobox_cache_lease_destroy(&lease)==-EBUSY);
    /* An unrelated full flush must not revert retained native policy from
     * shadow PTEs, nor consume the prior operation's mirror acknowledgement. */
    unsigned attempts=native_attempts[0];flush_tlb_kernel_range(0,TLB_FLUSH_ALL);
    assert(native_attempts[0]==attempts && !kobox_cache_lease_flush_ack());
    assert(!kobox_cache_lease_restore_wb(lease));all_wb();destroy(&lease);

    reset();lease=make(false);assert(!kobox_cache_lease_acquire(lease,3));free_fail_page=5;
    assert(kobox_cache_lease_restore_wb(lease)==-EIO);
    assert(free_per_page[4]==1 && !free_per_page[5] && !free_per_page[6] && !owner_puts);
    free_fail_page=-1;assert(!kobox_cache_lease_restore_wb(lease));
    for(unsigned i=4;i<=6;++i)assert(free_per_page[i]==1);
    all_wb();destroy(&lease);

    reset();lease=make(false);partial_fail_mode=3;partial_failures_left=1;
    arm_wb_fail_on_partial=true;
    assert(kobox_cache_lease_acquire(lease,3)==-EIO);
    assert(status(lease).state==KOBOX_CACHE_RETAINED && !owner_puts && !free_calls);
    for(unsigned i=4;i<=6;++i)assert(shadow_cache[i]==0);
    assert(native_cache[4]==3 && native_cache[5]==1);
    assert(!kobox_cache_lease_restore_wb(lease));all_wb();destroy(&lease);

    reset();lease=make(false);assert(!kobox_cache_lease_acquire(lease,3));
    mirror_fail_cache=0;mirror_failures_left=1;mirror_fail_after=1;
    assert(kobox_cache_lease_restore_wb(lease)==-EIO);
    assert(status(lease).state==KOBOX_CACHE_RETAINED && !free_calls && !owner_puts);
    for(unsigned i=4;i<=6;++i)assert(native_cache[i]==0);
    assert(!kobox_cache_lease_restore_wb(lease));all_wb();destroy(&lease);
}
static void expect_abort(void (*fn)(void)) {
    pid_t pid=fork();assert(pid>=0);
    if(!pid) { (void)freopen("/dev/null","w",stderr);fn();_exit(99); }
    int result;assert(waitpid(pid,&result,0)==pid);
    assert(WIFSIGNALED(result) && WTERMSIG(result)==SIGABRT);
}
static void outside_failure(void) {
    native_fail_mode=0;native_failures_left=1;
    flush_tlb_kernel_range(BASE,BASE+PAGE_SIZE);
}
static void stale_ack(void) { kobox_cpa_drain_ready=false;kobox_cpa_wbinvd(); }
static void repeated_ack(void) {
    flush_tlb_kernel_range(0,TLB_FLUSH_ALL);kobox_cpa_wbinvd();kobox_cpa_wbinvd();
}
static void conflicting_shadow(void) {
    shadow_cache[4]=2;flush_tlb_kernel_range(BASE+4*PAGE_SIZE,BASE+5*PAGE_SIZE);
}
static void boundaries_and_fatal(void) {
    reset();struct kobox_cache_lease *lease=make(true);
    assert(kobox_cache_lease_boundary(0,32)==4);
    assert(kobox_cache_lease_boundary(4,32)==5);
    assert(kobox_cache_lease_boundary(5,32)==6);
    assert(kobox_cache_lease_boundary(6,32)==7);
    int result=123;assert(!kobox_cache_lease_shadow(0,1,0,3,&result) && result==123);
    assert(!kobox_cache_lease_flush_ack());
    expect_abort(outside_failure);expect_abort(stale_ack);expect_abort(repeated_ack);expect_abort(conflicting_shadow);
    assert(!kobox_cache_lease_restore_wb(lease));destroy(&lease);
}
static void *create_while_publisher_locked(void *out) {
    struct page *pages[]={&ram[4],&ram[5],&ram[6]};is_creator=true;
    assert(!kobox_cache_lease_create(pages,3,&owner_ops,&ram,out));return NULL;
}
static void registry_publication(void) {
    reset();struct kobox_cache_lease *lease=NULL;pthread_t creator;
    target[4]=target[5]=target[6]=true;race_mode=true;
    /* Model an early_boot publisher which already computed an unmanaged
     * span. The actual create path must wait for that alias transaction. */
    mutex_lock(&kernel_alias_lock);
    assert(kobox_cache_lease_boundary(0,32)==32);
    assert(!pthread_create(&creator,NULL,create_while_publisher_locked,&lease));
    mutex_lock(&race_gate);
    struct timespec until;assert(!clock_gettime(CLOCK_REALTIME,&until));until.tv_sec+=2;
    while(!creator_waiting)assert(!pthread_cond_timedwait(&race_condition,&race_gate,&until));
    mutex_unlock(&race_gate);
    mutex_lock(&cache_journal_lock);assert(!cache_leases);mutex_unlock(&cache_journal_lock);
    protect_direct_range(BASE,BASE+RAM_PAGES*PAGE_SIZE);
    mutex_unlock(&kernel_alias_lock);
    assert(!pthread_join(creator,NULL));race_mode=false;
    assert(lease && !kobox_cache_lease_acquire(lease,3));
    assert(!kobox_cache_lease_restore_wb(lease));all_wb();destroy(&lease);

    reset();mutate_at_owner_get=true;struct page *page=&ram[4];
    assert(kobox_cache_lease_create(&page,1,&owner_ops,&ram,&lease)==-EBUSY);
    assert(!lease && owner_gets==1 && owner_puts==1 && ram[4].refs==1 && !live_allocs);
}

/* Actual i915 vmap wrapper, using the same real lease engine above. */
typedef unsigned pgprot_t;
#define pgprot_val(value) (value)
#define _PAGE_CACHE_MASK 0xffu
#define cachemode2protval(value) (value)
#define VM_MAP_PUT_PAGES 2ul
#define THIS_MODULE ((void *)1)
static unsigned module_gets, module_puts, vmap_calls, vunmap_calls;
static bool vmap_fail, module_fail, vmap_rollback_fail;
static bool try_module_get(void *module) {
    assert(module==THIS_MODULE);if(module_fail)return false;++module_gets;return true;
}
static void module_put(void *module) { assert(module==THIS_MODULE);++module_puts; }
void *__real_vmap(struct page **pages,unsigned count,unsigned long flags,pgprot_t protection) {
    (void)flags;++vmap_calls;
    if(protection==3)for(unsigned i=0;i<count;++i) {
        assert(native_cache[pages[i]->pfn]==3 && shadow_cache[pages[i]->pfn]==3);
        assert(pages[i]->refs==(pages[i]->pfn==6?0u:1u));
    }
    if(vmap_fail && vmap_rollback_fail)wb_failures_left=1;
    return vmap_fail?NULL:(void *)0xdead000;
}
void __real_vunmap(const void *address) {
    assert(address==(void *)0xdead000);++vunmap_calls;
    if(cache_leases)assert(native_cache[4]==3 && shadow_cache[4]==3);
}
#include "i915_vmap_cache_actual.inc"
static void borrowed_restore_failure(void) {
    reset();vmap_fail=module_fail=vmap_rollback_fail=false;
    struct page *pages[]={&ram[4],&ram[6]};target[4]=target[6]=true;ram[6].refs=0;
    void *address=__wrap_vmap(pages,2,0,3);assert(address);
    wb_failures_left=1;
    __wrap_vunmap(address);
    _exit(0);
}
static void borrowed_rollback_failure(void) {
    reset();vmap_fail=vmap_rollback_fail=true;module_fail=false;
    struct page *pages[]={&ram[4],&ram[6]};target[4]=target[6]=true;ram[6].refs=0;
    (void)__wrap_vmap(pages,2,0,3);
    _exit(0);
}
static void vmap_cache_cases(void) {
    struct page *pages[]={&ram[4],&ram[6]};
    for(unsigned failure=0;failure<4;++failure) {
        reset();module_gets=module_puts=vmap_calls=vunmap_calls=0;
        vmap_fail=failure==1;module_fail=failure==2;
        target[4]=target[6]=true;ram[6].refs=0;
        if(failure==3) {native_fail_mode=3;native_failures_left=1;}
        void *address=__wrap_vmap(pages,2,0,3);
        if(failure)assert(!address);
        else {
            assert(address==(void *)0xdead000 && ram[4].refs==1 && ram[6].refs==0);
            assert(native_cache[4]==3 && module_gets==1 && !module_puts);
            __wrap_vunmap(address);
            assert(vunmap_calls==1);
        }
        assert(!cache_aliases && !cache_leases && !live_allocs);
        assert(module_gets==module_puts && ram[4].refs==1 && ram[6].refs==0);
        all_wb();
    }
    reset();vmap_fail=module_fail=false;module_gets=module_puts=0;
    struct page *duplicate[]={&ram[4],&ram[4]};
    assert(!__wrap_vmap(duplicate,2,0,3));
    assert(!__wrap_vmap(pages,2,VM_MAP_PUT_PAGES,3));
    assert(!cache_aliases && !cache_leases && !live_allocs && !module_gets);
    void *plain=__wrap_vmap(pages,2,0,0);
    assert(plain && !module_gets && !native_cache[4]);__wrap_vunmap(plain);
    reset();ram[6].refs=0;struct kobox_cache_lease *unsafe=NULL;
    assert(kobox_cache_lease_create(pages,2,&owner_ops,&ram,&unsafe)==-EINVAL);
    assert(!unsafe && !live_allocs && !owner_gets);
    expect_abort(borrowed_restore_failure);expect_abort(borrowed_rollback_failure);
    puts("I915 native WC vmap: borrowed higher-order allocation, no tail refs, revoke-before-WB restoration/fail-stop PASS");
}

int main(void) {
    normal();refusal_and_allocation();partial_and_phase_failures();retained_retry();boundaries_and_fatal();
    registry_publication();vmap_cache_cases();
    puts("CACHE_LEASE actual engine+early_boot hooks WB/WC/partial-host/UC-minus/CPA rollback: PASS");
    puts("CACHE_LEASE retained retry/owner refs/exactly-once memtype/remote CPU/outside fatal/fresh drain: PASS");
    puts("CACHE_LEASE old unmanaged publisher exclusion/WB recheck/reference rollback: PASS");
    return 0;
}
