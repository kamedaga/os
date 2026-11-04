#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"
#include "../userland/personality/linux/runtime/lpr_entry_stack.h"
struct context {struct context *next;uint64_t tid;uintptr_t top;};
static unsigned char stacks[2][131072] __attribute__((aligned(16)));
unsigned char lpr_entry_bootstrap_stack[LPR_ENTRY_BOOTSTRAP_BYTES] __attribute__((aligned(16)));
struct context *lpr_entry_context_head;
volatile uint32_t lpr_entry_context_lock,lpr_entry_bootstrap_lock;
const volatile unsigned char *lpr_thread_fast_page;
uintptr_t lpr_entry_single_stack_top;
static uint32_t thread_count=1;
const volatile uint32_t *const lpr_clock_thread_count=&thread_count;
const volatile unsigned char *lpr_clock_fast_page;
void *lpr_single_thread_signal_state;
uint64_t entry_test_tid=11;
unsigned entry_test_tid_calls;
static unsigned expected_stack;
uint32_t lpr_thread_test_cpu;
static uint64_t identity_page[1024] __attribute__((aligned(4096)));
extern long entry_test_call(void);
struct lpr_entry_stack_result lpr_entry_stack_prepare(uintptr_t captured){return (struct lpr_entry_stack_result){captured,0};}
int64_t lpr_dispatch_syscall_frame(struct lpr_linux_user_frame *frame,uint64_t nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f){
 (void)b;(void)c;(void)d;(void)e;(void)f;
 assert(nr==39&&a==123&&c==555);
 uintptr_t here=(uintptr_t)&nr;
 assert(here>=(uintptr_t)stacks[expected_stack]&&here<(uintptr_t)stacks[expected_stack]+sizeof stacks[0]);
 assert(frame->rax==39&&frame->rdi==123);return 0x1234;
}
int main(void){
 struct context first={0,11,(uintptr_t)stacks[0]+sizeof stacks[0]},second={0,22,(uintptr_t)stacks[1]+sizeof stacks[1]};first.next=&second;lpr_entry_context_head=&first;
 assert(entry_test_call()==0x1234&&entry_test_tid_calls==1);
 assert(lpr_entry_single_stack_top==first.top);
 assert(entry_test_call()==0x1234&&entry_test_tid_calls==1); // No native identity lookup.
 thread_count=2;entry_test_tid=22;expected_stack=1;
 assert(entry_test_call()==0x1234&&entry_test_tid_calls==2);
 assert(lpr_entry_single_stack_top==first.top); // Multithread lookup cannot publish.
 lpr_entry_single_stack_top=0;thread_count=1;
 assert(entry_test_call()==0x1234&&entry_test_tid_calls==3);
 assert(lpr_entry_single_stack_top==second.top);
 assert(entry_test_call()==0x1234&&entry_test_tid_calls==3);
 thread_count=2; lpr_thread_fast_page=(void *)identity_page;
 identity_page[2]=4;identity_page[4]=1;identity_page[8]=2;identity_page[9]=22;
 unsigned before=entry_test_tid_calls;
 for(unsigned i=0;i<1000;i++)assert(entry_test_call()==0x1234);
 assert(entry_test_tid_calls==before);
 identity_page[8]=3;assert(entry_test_call()==0x1234&&entry_test_tid_calls==before+1);
 identity_page[8]=2;identity_page[4]=0;assert(entry_test_call()==0x1234&&entry_test_tid_calls==before+2);
 lpr_thread_fast_page=0;
 puts("ENTRY_IDENTITY_PASS single_skips_GETTID multi_uses_GETTID survivor_owns_private_stack");return 0;
}
