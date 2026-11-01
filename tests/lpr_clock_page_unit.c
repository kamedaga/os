#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#include <stdint.h>
static uint64_t counter;
static unsigned mutation;
static unsigned char page_bytes[4096] __attribute__((aligned(4096)));
static uint64_t test_counter(void);
#define LPR_CLOCK_COUNTER() test_counter()
#include "../userland/personality/linux/runtime/lpr_clock.c"

static unsigned opens, maps, closes;
static int map_failure, open_failure;
void lpr_state_lock(volatile uint32_t *word) { assert(!*word); *word = 1; }
void lpr_state_unlock(volatile uint32_t *word) { assert(*word); *word = 0; }
int64_t lpr_pacha_syscall0(uint64_t nr)
{ assert(nr == PACHAOS_SYSCALL_CLOCK_PAGE_OPEN); ++opens; return open_failure ? -1 : 16; }
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd)
{ assert(nr == PACHAOS_SYSCALL_FD_CLOSE && fd == 16); ++closes; return 0; }
int64_t lpr_pacha_syscall6(uint64_t nr, uint64_t fd, uint64_t at,
    uint64_t bytes, uint64_t prot, uint64_t flags, uint64_t off)
{
    assert(nr == PACHAOS_SYSCALL_MMAP && fd == 16 && !at && bytes == 4096 &&
        prot == PACHAOS_PROT_READ && flags == PACHAOS_MMAP_SHARED && !off);
    ++maps; return map_failure ? 4 : (intptr_t)page_bytes;
}
static void u32(size_t off, uint32_t v) { memcpy(page_bytes+off, &v, 4); }
static void u64(size_t off, uint64_t v) { memcpy(page_bytes+off, &v, 8); }
static uint64_t test_counter(void)
{
    if (mutation == 1) u32(PV(VERSION), 4);
    if (mutation == 2) u32(PACHA_CLOCK_PAGE_FAST_OFFSET, 0);
    if (mutation == 3) {
        uint32_t version; memcpy(&version, page_bytes+PV(VERSION), 4);
        u32(PV(VERSION), version + 2);
    }
    return counter;
}
static void setup(void)
{
    memset(page_bytes, 0, sizeof(page_bytes));
    clock_page = page_bytes; clock_mapping_state = 2; mutation = 0;
    u32(0, 1); u32(PV(VERSION), 2); u64(PV(TSC_TIMESTAMP), 1000);
    u64(PV(SYSTEM_TIME), 100); u32(PV(MULTIPLIER), 1u<<31);
    page_bytes[PV(FLAGS)] = PACHA_CLOCK_PAGE_PV_STABLE_FLAG;
    u64(PACHA_CLOCK_PAGE_PV_ORIGIN_OFFSET, 100);
    u64(PACHA_CLOCK_PAGE_NS_ORIGIN_OFFSET, 10000);
    u64(PACHA_CLOCK_PAGE_FLOOR_NS_OFFSET, 10000);
    u64(PACHA_CLOCK_PAGE_CEILING_NS_OFFSET, 11000);
    counter = 1200;
}
static void reject(void)
{ uint64_t out=0xdead; assert(lpr_clock_monotonic(&out) == 1 && out == 0xdead); }
static void segv(int sig, siginfo_t *info, void *raw)
{
    (void)info; assert(sig == SIGSEGV);
    ucontext_t *uc=raw;
    uintptr_t fix=lpr_clock_copy_fault_target(14, uc->uc_mcontext.gregs[REG_ERR],
        uc->uc_mcontext.gregs[REG_RIP]);
    assert(fix); uc->uc_mcontext.gregs[REG_RIP] = fix;
}
int main(void)
{
    uint64_t out;
    setup(); assert(!lpr_clock_monotonic(&out) && out==10100);
    u64(PACHA_CLOCK_PAGE_FLOOR_NS_OFFSET, 10200);
    assert(!lpr_clock_monotonic(&out) && out==10200);
    setup(); counter=3000; assert(!lpr_clock_monotonic(&out) && out==11000);
    counter=3002; reject();
    setup(); u32(0,0); reject();
    setup(); u32(PV(VERSION),3); reject();
    setup(); mutation=1; assert(!lpr_clock_monotonic(&out) && out==10100);
    setup(); mutation=3; reject();
    setup(); mutation=2; reject();
    setup(); page_bytes[PV(FLAGS)]=0; reject();
    setup(); counter=999; reject();
    setup(); u32(PV(MULTIPLIER),0); reject();
    setup(); page_bytes[PV(SHIFT)]=64; reject();
    setup(); page_bytes[PV(SHIFT)]=(uint8_t)-64; reject();
    setup(); page_bytes[PV(SHIFT)]=63; counter=UINT64_MAX; reject();
    setup(); u64(PV(SYSTEM_TIME),UINT64_MAX); reject();
    setup(); u64(PACHA_CLOCK_PAGE_PV_ORIGIN_OFFSET,1000); reject();
    setup(); u64(PACHA_CLOCK_PAGE_NS_ORIGIN_OFFSET,UINT64_MAX); reject();
    setup(); page_bytes[PV(SHIFT)]=(uint8_t)-1;
    assert(!lpr_clock_monotonic(&out) && out==10050);
    setup(); page_bytes[PV(SHIFT)]=1;
    assert(!lpr_clock_monotonic(&out) && out==10200);
    /* Compare production arithmetic to a 128-bit reference for every valid
     * shift. Keep delta small when shifting left so the product fits. */
    for (int shift=-63; shift<=63; ++shift) {
        setup(); counter=1001; page_bytes[PV(SHIFT)]=(uint8_t)shift;
        u64(PACHA_CLOCK_PAGE_CEILING_NS_OFFSET, UINT64_MAX);
        uint64_t expected=10000+(shift<0 ? 0 : (uint64_t)(((unsigned __int128)1<<shift)/2));
        assert(!lpr_clock_monotonic(&out) && out==expected);
    }
    setup(); clock_page=0; clock_mapping_state=0;
    assert(!lpr_clock_monotonic(&out) && opens==1 && maps==1 && closes==1);
    assert(!lpr_clock_monotonic(&out) && opens==1);
    lpr_clock_after_fork_child(); assert(clock_mapping_state==2);
    clock_mapping_state=1; lpr_clock_after_fork_child(); assert(!clock_mapping_state && !clock_page);
    map_failure=1; reject(); assert(opens==2 && maps==2 && closes==2);
    reject(); assert(opens==2); /* failed map is attempted only once */
    clock_mapping_state=0; open_failure=1; reject(); assert(opens==3 && maps==2);
    clock_mapping_state=1; clock_mapping_lock=1;
    reject(); assert(opens==3); /* another initializer: no wait */
    lpr_clock_after_fork_child(); assert(!clock_mapping_state && !clock_mapping_lock);
    lpr_clock_fork_lock(); assert(clock_mapping_lock==1);
    lpr_clock_fork_unlock(); assert(!clock_mapping_lock);
    assert(!lpr_clock_copy_fault_target(14,0,(uintptr_t)lpr_clock_copy_seconds));
    assert(!lpr_clock_copy_fault_target(14,18,(uintptr_t)lpr_clock_copy_seconds));
    assert(!lpr_clock_copy_fault_target(13,2,(uintptr_t)lpr_clock_copy_seconds));
    assert(!lpr_clock_copy_fault_target(14,2,0));
    struct sigaction sa={.sa_sigaction=segv,.sa_flags=SA_SIGINFO};
    sigemptyset(&sa.sa_mask); assert(!sigaction(SIGSEGV,&sa,0));
    unsigned char *memory=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(memory!=MAP_FAILED);
    assert(!lpr_clock_copy_timespec(memory+4093,123,456));
    uint64_t values[2]; memcpy(values,memory+4093,16); assert(values[0]==123 && values[1]==456);
    assert(!mprotect(memory+4096,4096,PROT_NONE));
    assert(lpr_clock_copy_timespec(memory+4088,7,8)==-14);
    assert(!mprotect(memory,4096,PROT_READ));
    assert(lpr_clock_copy_timespec(memory,7,8)==-14);
    assert(!munmap(memory,8192));
    puts("LPR clock page model, map/fork/failure and guarded copyout: PASS");
}
