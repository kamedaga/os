#include <assert.h>
#include <stdio.h>
#include "pacha/ipc.h"
void *klogudp_gate;
extern void raw_gate(void), ipc_gate(void);
__asm__(".text\n"
"raw_gate:\n add %rdi,%rax\n add %rsi,%rax\n add %rdx,%rax\n"
"add %r10,%rax\n add %r8,%rax\n add %r9,%rax\n ret\n"
"ipc_gate:\n mov $111,%r10\n mov $222,%r8\n mov $333,%r9\n"
"mov $444,%r12\n xor %r15,%r15\n mov $1,%rdx\n xor %eax,%eax\n ret\n");
int main(void)
{
    /* Mock gate only: not a native syscall number. */
    const uint64_t mock_number=1001;
    klogudp_gate=raw_gate;
    assert(pacha_syscall6(mock_number,1,2,3,4,5,6)==1022);
    klogudp_gate=ipc_gate;
    uint64_t words[4]={1,2,3,4};int committed=0;
    struct pacha_ipc_extension ext={0};
    assert(!pacha_ipc_registers(1002,241,0,55,words,&ext,&committed));
    assert(words[0]==111 && words[1]==222 && words[2]==333 && words[3]==444);
    assert(committed==1 && !ext.received_count);
    puts("bridge register model: raw arguments / four-word IPC / commit passed");
}
