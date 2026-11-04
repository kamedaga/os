#include <assert.h>
#include <stdio.h>
#ifndef MADVISE_IMPLEMENTATION
#define MADVISE_IMPLEMENTATION "../userland/personality/linux/runtime/lpr_dispatch.c"
#endif
#include MADVISE_IMPLEMENTATION
static uint64_t last_advice;
static int locked;
int64_t lpr_mlock_madvise_begin(uint64_t a,uint64_t n){(void)a;(void)n;locked=1;return 0;}
void lpr_mlock_vm_end(void){locked=0;}
int64_t lpr_pacha_syscall3(uint64_t nr,uint64_t a,uint64_t n,uint64_t advice){
 assert(nr==PACHA_VM_SYSCALL_MADVISE);last_advice=advice;
 if((a&4095)||n>UINT64_MAX-a||a<4096)return PACHA_SYSCALL_ERR_INVALID;
 return advice==17?PACHA_SYSCALL_ERR_INVALID:0;
}
int main(void){unsigned w=0;assert(!lpr_sys_madvise(4096,4096,17,0,0,0,&w)&&last_advice==0);assert(lpr_sys_madvise(4097,4096,17,0,0,0,&w)==-LPR_LINUX_EINVAL);assert(!lpr_sys_madvise(4096,4096,4,0,0,0,&w)&&last_advice==4&&!locked);puts("MADVISE_DISPATCH_PASS DODUMP/native coverage/alignment/DONTNEED lock");}
