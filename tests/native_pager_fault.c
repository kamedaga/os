/* SPDX-License-Identifier: MIT */
/* A real user #PF must park one process and restart its load after supply. */
#include <stdint.h>
#include <stddef.h>
#include "pacha/ipc.h"
#include "pacha/syscall.h"

#define PAGE 4096ul
#define CONTROL 0x42000000ul
#define TARGET 0x43000000ul
#define CODE 0x44000000ul
#define STACK 0x45000000ul
#define RW (PACHA_PROT_READ | PACHA_PROT_WRITE)
#define VMO_RIGHTS (PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE | PACHA_FD_RIGHT_MAP_EXEC | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP)
#define PROCESS_RIGHTS (PACHA_FD_RIGHT_MAP_INTO | PACHA_FD_RIGHT_SPAWN | PACHA_FD_RIGHT_SET_CONTEXT | PACHA_FD_RIGHT_KILL | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP)
#define STR1(x) #x
#define STR(x) STR1(x)

extern unsigned char pager_child[], pager_child_end[];
void *memset(void *p, int value, size_t n) { unsigned char *b=p; for (size_t i=0;i<n;i++) b[i]=(unsigned char)value; return p; }
void *memcpy(void *p, const void *q, size_t n) { unsigned char *b=p; const unsigned char *s=q; for(size_t i=0;i<n;i++) b[i]=s[i]; return p; }
static void log_line(const char *s) { size_t n=0; while(s[n])n++; (void)pacha_syscall2(1,(uintptr_t)s,n); }
__attribute__((noreturn)) static void fail(unsigned line) {
    char b[20]; unsigned n=0; do { b[n++]='0'+line%10; line/=10; } while(line);
    for(unsigned i=0;i<n/2;i++){ char c=b[i];b[i]=b[n-1-i];b[n-1-i]=c; }
    b[n++]='\n';b[n]=0;log_line(b);log_line("NATIVE_PAGER=FAIL\n");
    (void)pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT,1); __builtin_trap();
}
#define CHECK(x) do { if(!(x)) fail(__LINE__); } while(0)
static long vmo(size_t size, uint64_t rights, uint64_t flags) {
    long fd=pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE,size,rights,flags); CHECK(fd>=16); return fd;
}
static void *alias(long fd) {
    long va=pacha_syscall6(PACHA_VM_SYSCALL_MMAP,fd,0,PAGE,RW,PACHA_MMAP_SHARED,0);
    CHECK(va>=(long)PAGE); return (void *)(uintptr_t)va;
}
static long map(long process,long fd,uintptr_t va,size_t size,uint64_t prot,uint64_t flags) {
    return pacha_syscall6(PACHA_PROCESS_SYSCALL_MAP,process,fd,va,size,prot,flags);
}
__asm__(".pushsection pager_code,\"ax\",@progbits\n"
    ".global pager_child,pager_child_end\npager_child:\n"
    "mov $0x42000000,%rbx\n"
    /* A different port cannot use this pager VMO as WAIT's output buffer. */
    "xor %edi,%edi\nmov $" STR(PACHA_PAGER_SYSCALL_PORT_CREATE) ",%eax\nsyscall\n"
    "mov %rax,%r12\nmov %rax,%rdi\nmov $0x43000000,%esi\n"
    "xor %edx,%edx\nmov $" STR(PACHA_PAGER_SYSCALL_WAIT) ",%eax\nsyscall\n"
    "mov %rax,24(%rbx)\n"
    "mov %r12,%rdi\nmov $" STR(PACHA_FD_SYSCALL_CLOSE) ",%eax\nsyscall\n"
    /* Kernel copyin must queue demand and return VA before any log output. */
    "mov $0x43000000,%edi\nmov $8,%esi\nmov $1,%eax\nsyscall\n"
    "mov %rax,16(%rbx)\n"
    "mov %rdx,32(%rbx)\n"
    "mov $0x43000000,%rax\n"
    "mov (%rax),%rcx\n"
    "mov %rcx,8(%rbx)\n"
    "movq $1,0(%rbx)\n"
    "xor %edi,%edi\nmov $" STR(PACHA_PROCESS_SYSCALL_EXIT) ",%eax\nsyscall\nud2\n"
    "pager_child_end:\n.popsection\n");

int main(void) {
    log_line("NATIVE_PAGER=START\n");
    long port=pacha_syscall1(PACHA_PAGER_SYSCALL_PORT_CREATE,0); CHECK(port>=16);
    long pager=vmo(PAGE,0,PACHA_VMO_CREATE_PAGER);
    uint64_t attached_generation=0;
    CHECK(pacha_pager_attach(port,pager,0x1234,0,&attached_generation)==0);
    CHECK(attached_generation!=0);
    long source=vmo(PAGE,VMO_RIGHTS,0);
    *(volatile uint64_t *)alias(source)=0x123456789abcdef0ull;
    long code=vmo(PAGE,VMO_RIGHTS,0);
    memcpy(alias(code),pager_child,(size_t)(pager_child_end-pager_child));
    long control=vmo(PAGE,VMO_RIGHTS,0);
    volatile uint64_t *state=alias(control); state[0]=0;state[1]=0;state[2]=UINT64_MAX;state[3]=UINT64_MAX;state[4]=UINT64_MAX;
    long process=pacha_syscall5(PACHA_PROCESS_SYSCALL_CREATE,0,PROCESS_RIGHTS,0,0,0);CHECK(process>=16);
    CHECK(map(process,code,CODE,PAGE,PACHA_PROT_READ|PACHA_PROT_EXEC,PACHA_PROCESS_MAP_SHARED)==CODE);
    CHECK(map(process,control,CONTROL,PAGE,RW,PACHA_PROCESS_MAP_SHARED)==CONTROL);
    CHECK(map(process,pager,TARGET,PAGE,PACHA_PROT_READ,PACHA_PROCESS_MAP_PRIVATE)==TARGET);
    CHECK(map(process,0,STACK,8*PAGE,RW,PACHA_PROCESS_MAP_PRIVATE|PACHA_PROCESS_MAP_ANONYMOUS)==STACK);
    long thread=pacha_syscall6(PACHA_THREAD_SYSCALL_CREATE,process,CODE,STACK+4*PAGE,0,0,
        PACHA_FD_RIGHT_START|PACHA_FD_RIGHT_WAIT|PACHA_FD_RIGHT_CLOSE|PACHA_FD_RIGHT_INSPECT);
    CHECK(thread>=16);
    CHECK(!pacha_syscall1(PACHA_THREAD_SYSCALL_START,thread));
    log_line("NATIVE_PAGER=WAIT\n");
    long sparse=vmo(PAGE,VMO_RIGHTS,PACHA_VMO_CREATE_ZERO_ON_DEMAND);
    void *absent_output=alias(sparse);
    CHECK(pacha_syscall3(PACHA_PAGER_SYSCALL_WAIT,port,(uintptr_t)absent_output,0)==PACHA_SYSCALL_ERR_INVALID);
    struct pacha_pager_event event;
    long rc;
    do { rc=pacha_syscall3(PACHA_PAGER_SYSCALL_WAIT,port,(uintptr_t)&event,0); }
    while (rc==PACHA_SYSCALL_ERR_NOT_READY);
    CHECK(rc==0 && event.kind==PACHA_PAGER_EVENT_FAULT && event.key==0x1234 &&
          event.page_offset==0 && event.page_length==PAGE && event.request_id!=0 &&
          event.vmo_generation==attached_generation);
    CHECK(state[0]==0 && (int64_t)state[2]==PACHA_SYSCALL_ERR_PAGER_ABSENT &&
          state[3]==PACHA_SYSCALL_ERR_INVALID && state[4]==TARGET);
    log_line("NATIVE_PAGER=SUPPLY\n");
    struct pacha_pager_supply supply={event.key,event.vmo_generation,event.request_id,(uint64_t)source,0,0,PAGE};
    long blind_port=pacha_syscall4(PACHA_FD_SYSCALL_DUP,port,16,PACHA_FD_RIGHT_CLOSE|PACHA_FD_RIGHT_WAIT,0);
    CHECK(blind_port>=16);
    CHECK(pacha_syscall2(PACHA_PAGER_SYSCALL_SUPPLY,blind_port,(uintptr_t)&supply)==PACHA_SYSCALL_ERR_INVALID);
    CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE,blind_port));
    supply.source_fd=(uint64_t)pager;
    CHECK(pacha_syscall2(PACHA_PAGER_SYSCALL_SUPPLY,port,(uintptr_t)&supply)==PACHA_SYSCALL_ERR_INVALID);
    supply.source_fd=(uint64_t)source;
    CHECK(!pacha_syscall2(PACHA_PAGER_SYSCALL_SUPPLY,port,(uintptr_t)&supply));
    CHECK(pacha_syscall2(PACHA_PAGER_SYSCALL_SUPPLY,port,(uintptr_t)&supply)==PACHA_SYSCALL_ERR_INVALID);
    uint64_t ts[2];
    CHECK(!pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME,1,(uintptr_t)ts));
    uint64_t until=ts[0]+5;
    while (__atomic_load_n(&state[0],__ATOMIC_ACQUIRE)==0) {
        CHECK(!pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME,1,(uintptr_t)ts));
        CHECK(ts[0]<until);
    }
    CHECK(state[1]==0x123456789abcdef0ull);
    log_line("NATIVE_PAGER=PASS\n");
    return 0;
}
