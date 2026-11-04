#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
uint64_t lpr_clock_test_counter;
unsigned lpr_clock_test_mutation;
#define LPR_CLOCK_COUNTER() lpr_clock_test_counter
#include "../userland/personality/linux/runtime/lpr_clock.c"
lpr_state_t lpr_state;
lpr_signal_thread_state_t *lpr_single_thread_signal_state;
static lpr_signal_thread_state_t signal_state;
unsigned clock_test_fallbacks;
unsigned char clock_test_before[512] __attribute__((aligned(16)));
unsigned char clock_test_after[512] __attribute__((aligned(16)));
static unsigned char bytes[4096] __attribute__((aligned(4096)));
extern int64_t clock_test_call(uint64_t,uint64_t,void *);
static void u32(unsigned n,uint32_t v){memcpy(bytes+n,&v,4);}
static void u64(unsigned n,uint64_t v){memcpy(bytes+n,&v,8);}
void lpr_state_lock(volatile uint32_t *p){assert(!*p);*p=1;}
void lpr_state_unlock(volatile uint32_t *p){assert(*p);*p=0;}
int64_t lpr_pacha_syscall0(uint64_t nr){(void)nr;assert(0);return -1;}
int64_t lpr_pacha_syscall1(uint64_t nr,uint64_t a){(void)nr;(void)a;assert(0);return -1;}
int64_t lpr_pacha_syscall6(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f){(void)n;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;assert(0);return -1;}
static void setup(void){
 memset(bytes,0,sizeof bytes);memset(&signal_state,0,sizeof signal_state);
 lpr_single_thread_signal_state=&signal_state;signal_state.native_pending_hint_active=1;
 lpr_state.thread_count=1;clock_page=lpr_clock_fast_page=bytes;clock_mapping_state=2;
 lpr_clock_test_counter=1000000000ULL;clock_test_fallbacks=0;
 lpr_clock_test_mutation=0;
 u32(0,1);u32(64,2);u64(8,1000000000ULL);u64(16,2000000000ULL);
 u64(32,2504000000ULL);u64(80,1000000000ULL);u32(88,1u<<31);bytes[93]=1;
}
static int64_t invoke(uint64_t id,void *p){
 int64_t result=clock_test_call(228,id,p);
 assert(!memcmp(clock_test_before,clock_test_after,512));return result;
}
static void miss(void){uint64_t ts[2]={11,22};assert(invoke(1,ts)==-38);assert(clock_test_fallbacks==1);assert(ts[0]==11&&ts[1]==22);}
static void fault(int sig,siginfo_t *info,void *opaque){
 (void)sig;(void)info;ucontext_t *ctx=opaque;
 uintptr_t target=lpr_clock_copy_fault_target(14,(uint64_t)ctx->uc_mcontext.gregs[REG_ERR],(uint64_t)ctx->uc_mcontext.gregs[REG_RIP]);
 assert(target);ctx->uc_mcontext.gregs[REG_RIP]=(greg_t)target;
}
int main(void){
 struct sigaction action={.sa_sigaction=fault,.sa_flags=SA_SIGINFO};sigemptyset(&action.sa_mask);assert(!sigaction(SIGSEGV,&action,0));
 setup();uint64_t ts[2];assert(!invoke(1,ts));assert(ts[0]==2&&ts[1]==500000000);assert(!clock_test_fallbacks);
 // Compare the assembly leaf against the general production reader over
 // actual normalized shifts and nonzero epochs, not a second formula.
 for(int shift=-12;shift<=0;shift++)for(unsigned i=0;i<200;i++){
  setup();bytes[92]=(unsigned char)shift;lpr_clock_test_counter=(uint64_t)i*123456789ULL;
  u64(8,0);u64(16,37);u64(32,UINT64_MAX);
  uint64_t expected;assert(!lpr_clock_monotonic(&expected));assert(!invoke(1,ts));assert(ts[0]*1000000000ULL+ts[1]==expected);
 }
 setup();u64(24,2500000010ULL);assert(!invoke(1,ts)&&ts[1]==500000010);
 setup();u64(24,2504000001ULL);miss();
 setup();lpr_state.thread_count=2;miss();
 setup();lpr_single_thread_signal_state=0;miss();
 setup();signal_state.native_pending_hint_active=0;miss();
 setup();signal_state.native_pending_hint=1;miss();
 setup();signal_state.pending_mask=1;miss();
 setup();lpr_clock_fast_page=0;miss();
 setup();u32(0,0);miss();
 setup();u32(64,3);miss();
 setup();u32(64,4);miss(); // General reader still supports this even version.
 setup();lpr_clock_test_mutation=1;miss();
 setup();lpr_clock_test_mutation=2;miss();
 setup();bytes[93]=0;miss();
 setup();bytes[92]=1;miss();
 setup();bytes[92]=(unsigned char)-64;miss();
 setup();u32(88,0);miss();
 setup();u64(72,lpr_clock_test_counter+1);miss();
 setup();u64(80,UINT64_MAX);miss();
 setup();u64(8,UINT64_MAX);miss();
 setup();u64(16,UINT64_MAX);miss();
 setup();u64(32,2499999999ULL);miss();
 setup();assert(invoke(0,ts)==-38&&clock_test_fallbacks==1);
 setup();assert(clock_test_call(39,1,ts)==-38&&clock_test_fallbacks==1);
 setup();assert(invoke(1,0)==-14);assert(invoke(1,(void *)0x7ffffffffff8ULL)==-14);assert(invoke(1,(void *)UINT64_MAX)==-14);
 unsigned char *p=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED);
 assert(!mprotect(p+4096,4096,PROT_READ));assert(invoke(1,p+4096)==-14);assert(invoke(1,p+4088)==-14);assert(*(uint64_t *)(void *)(p+4088)==2);
 assert(!munmap(p,8192));
 assert(!lpr_clock_copy_fault_target(13,2,(uintptr_t)lpr_clock_entry_copy_seconds));
 assert(!lpr_clock_copy_fault_target(14,16,(uintptr_t)lpr_clock_entry_copy_seconds));
 assert(!lpr_clock_copy_fault_target(14,2,(uintptr_t)main));
 puts("CLOCK_ENTRY_UNIT_PASS cases=2600+bounds GP/FP=preserved EFAULT=guarded fallbacks=preserved");
}
