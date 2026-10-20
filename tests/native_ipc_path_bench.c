/* SPDX-License-Identifier: MIT */
/* B3 diagnostic-only fixture. Uses the opt-in kernel trace tag and empty number.
 * Raw stamps are retained: CPU endpoint classification is not proof that no
 * migration occurred between stamps. Output and allocation stay outside timing. */
#include <stdint.h>
#include <stddef.h>
#include <pacha/abi.h>
#include <pacha/ipc.h>
#include <pacha/syscall.h>
#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x,v) __atomic_store_n(&(x),(v),__ATOMIC_RELEASE)
#define N 256
#define CR (PACHA_FD_RIGHT_SEND|PACHA_FD_RIGHT_RECV|PACHA_FD_RIGHT_CALL|PACHA_FD_RIGHT_WAIT|PACHA_FD_RIGHT_POLL|PACHA_FD_RIGHT_CLOSE|PACHA_FD_RIGHT_TRANSFER)
#define TR (PACHA_FD_RIGHT_START|PACHA_FD_RIGHT_WAIT|PACHA_FD_RIGHT_POLL|PACHA_FD_RIGHT_CLOSE)
struct stamp { uint64_t t; uint32_t cpu; };
struct row { struct stamp a,b,c,d; uint64_t valid, client_waits, server_waits; };
struct shared { struct row r[N]; unsigned ready,stop,seq; struct stamp last; };
static struct shared *s;
static uint64_t pair[2];
static unsigned mode, count, payload, separate;
static unsigned char stack[65536] __attribute__((aligned(4096)));
static struct stamp fault_enter, fault_leave;
static char output[512]; static unsigned used;
void *memset(void *p,int c,size_t n) { unsigned char *b=p;for(size_t i=0;i<n;i++)b[i]=c;return p; }
void *memcpy(void *d,const void *p,size_t n) { unsigned char *a=d;const unsigned char*b=p;for(size_t i=0;i<n;i++)a[i]=b[i];return d; }
static void add(const char *p) { while(*p && used<sizeof(output)-1)output[used++]=*p++; }
static void num(uint64_t v) { char a[24];unsigned n=0;do{a[n++]='0'+v%10;v/=10;}while(v);while(n)output[used++]=a[--n]; }
static void flush(void) { pacha_syscall2(1,(uintptr_t)output,used);used=0; }
__attribute__((noreturn)) static void fail(unsigned site,long rc) { add("S10S_FAIL site=");num(site);add(" rc=");num((uint64_t)rc);add("\n");flush();pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT,1);__builtin_trap(); }
static void ok(long rc,unsigned site) { if(rc)fail(site,rc); }
static uint64_t fd(long rc,unsigned site) { if(rc<16)fail(site,rc);return (uint64_t)rc; }
/* Lightweight boundary stamps avoid CPUID VM exits inside the measured path. */
static struct stamp tick(void) {
 unsigned lo,hi; __asm__ volatile("lfence; rdtsc":"=a"(lo),"=d"(hi)::"memory");
 return (struct stamp){((uint64_t)hi<<32)|lo,0};
}
static long measured(uint64_t nr,uint64_t endpoint,uint64_t send_va,uint64_t receive_va,
 uint64_t timeout,uint64_t flags,unsigned i,struct stamp *after) {
 const struct pacha_ipc_msg *send=(const void *)(uintptr_t)send_va;
 struct pacha_ipc_msg *receive=(void *)(uintptr_t)receive_va;
 register uint64_t r10 __asm__("r10")=send ? send->word0 : 0;
 register uint64_t r8 __asm__("r8")=send ? send->word1 : 0;
 register uint64_t r9 __asm__("r9")=send ? send->word2 : 0;
 register uint64_t r12 __asm__("r12")=send ? send->word3 : 0;
 register uint64_t r13 __asm__("r13")=0;
 register uint64_t rbx __asm__("rbx")=0;
 register uint64_t r15 __asm__("r15")=0;
 /* Profiling tag uses a preserved register; R9 now carries payload. */
 register uint64_t r14 __asm__("r14")=0x4233000000000000ull|((uint64_t)separate<<24)|((uint64_t)payload<<16)|i;
 uint64_t rc;
 __asm__ volatile("syscall; mov %%rax,%[rc]; lfence; rdtsc"
 : [rc] "=&r"(rc),"+a"(nr),"+d"(timeout),"+r"(r10),"+r"(r8),"+r"(r9),"+r"(r12),"+r"(r15)
 : "D"(endpoint),"S"(flags),"r"(r13),"r"(r14),"r"(rbx)
 : "rcx","r11","memory");
 *after=(struct stamp){(timeout<<32)|(uint32_t)nr,0};
 if(receive) {receive->word0=r10;receive->word1=r8;receive->word2=r9;receive->word3=r12;receive->fd_count=0;}
 return (long)rc;
}
static void closefd(uint64_t f) { ok(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE,f),10); }
static unsigned receive(uint64_t f,struct pacha_ipc_msg *m) { unsigned n=0;long rc;do {++n;rc=pacha_ipc_message_syscall(PACHA_IPC_SYSCALL_RECV_WAIT, f, 0, (struct pacha_ipc_msg *)(uintptr_t)((uintptr_t)m), UINT64_MAX, PACHA_WAIT_UNINTERRUPTIBLE, 0);}while(rc==PACHA_SYSCALL_ERR_NOT_READY);ok(rc,11);return n; }
static unsigned waitnotify(uint64_t f) { unsigned n=0;long rc;do {++n;rc=pacha_syscall3(PACHA_IPC_SYSCALL_NOTIFY_WAIT,f,UINT64_MAX,PACHA_WAIT_UNINTERRUPTIBLE);}while(rc==PACHA_SYSCALL_ERR_NOT_READY);ok(rc,12);return n; }
static void notify(uint64_t f) { ok(pacha_syscall1(PACHA_IPC_SYSCALL_NOTIFY,f),13); }
__attribute__((used,noreturn)) static void peer(void) {
    STORE(s->ready,1);
    if(mode==2) {
        while(!LOAD(s->stop)) { struct stamp t=tick(); __atomic_add_fetch(&s->seq,1,__ATOMIC_ACQ_REL); STORE(s->last.t,t.t); STORE(s->last.cpu,t.cpu); __atomic_add_fetch(&s->seq,1,__ATOMIC_RELEASE);pacha_syscall0(PACHA_RUNTIME_SYSCALL_YIELD); }
    } else if(mode==0) {
        struct pacha_ipc_msg m={0};
        s->r[0].server_waits=receive(pair[1],&m);
        for(unsigned i=0;i<count;i++) {
            if(i==0)s->r[i].b=tick();
            if(m.fd_count || m.word0!=(payload?0x1234u:0) || m.word3!=(payload?0x5678u:0))fail(14,m.fd_count);
            struct pacha_ipc_msg reply={.word0=m.word0,.word1=m.word1,.word2=m.word2,.word3=m.word3};
            s->r[i].c=tick();
            if(i+1==count) ok(pacha_ipc_message_syscall(PACHA_IPC_SYSCALL_REPLY, UINT64_MAX, (struct pacha_ipc_msg *)(uintptr_t)((uintptr_t)&reply), 0, 0, 0, 0),15);
            else {
                /* A successful reply is never replayed after recv failure.
                 * This fixture treats either half failing as a failed run. */
                ok(measured(PACHA_IPC_SYSCALL_REPLY_RECV,pair[1],(uintptr_t)&reply,(uintptr_t)&m,UINT64_MAX,PACHA_WAIT_UNINTERRUPTIBLE,i,&s->r[i+1].b),15);
                s->r[i+1].server_waits=1;
            }
        }
    } else for(unsigned i=0;i<count;i++) {
        s->r[i].server_waits=waitnotify(pair[1]);s->r[i].b=tick();s->r[i].c=tick();notify(pair[1]);
    }
    if(separate) {closefd(pair[0]);closefd(pair[1]);pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT,0);}
    else pacha_syscall3(PACHA_THREAD_SYSCALL_EXIT,0,0,0);
    __builtin_trap();
}
extern void peer_entry(void),fault_entry(void);
__asm__(".text\n.global peer_entry\npeer_entry: call peer; ud2\n.global fault_entry\nfault_entry: call fault_handler; ud2\n");
__attribute__((used,noreturn)) static void fault_handler(struct pacha_native_fault_frame *f) {
    fault_enter=tick();
    if(f->vector!=6 || f->signal.size!=sizeof(*f))fail(16,f->vector);
    f->signal.rip+=2;
    fault_leave=tick();
    pacha_syscall2(PACHA_PROCESS_SYSCALL_SIGNAL_CTL,PACHA_PROCESS_SIGNAL_CTL_RETURN,(uintptr_t)f);
    fail(17,0);
}
static uint64_t spawn(void) {
    if(separate) {
        long r=pacha_syscall3(PACHA_PROCESS_SYSCALL_CLONE,TR,PACHA_PROCESS_CLONE_CURRENT_THREAD,0);
        if(!r)peer();return fd(r,18);
    }
    uint64_t t=fd(pacha_syscall6(PACHA_THREAD_SYSCALL_CREATE,PACHA_PROCESS_SELF_FD,(uintptr_t)peer_entry,(uintptr_t)(stack+sizeof(stack)),0,0,TR),19);
    ok(pacha_syscall1(PACHA_THREAD_SYSCALL_START,t),20);return t;
}
static void join(uint64_t t) {
    struct pacha_pollfd p={.fd=t,.events=PACHA_FD_EVENT_READABLE};
    long r=pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,(uintptr_t)&p,1,UINT64_MAX,0);
    if(r!=1)fail(21,r);
    uint64_t st[4]={0};ok(pacha_syscall2(separate?PACHA_PROCESS_SYSCALL_WAIT:PACHA_THREAD_SYSCALL_WAIT,t,(uintptr_t)st),22);
    if(st[0]!=2 || st[1])fail(23,st[1]);closefd(t);
}
static void emit(const char *name,unsigned n) {
    for(unsigned i=0;i<n;i++) {
        struct row *r=&s->r[i];add("S10S_ROW test=");add(name);add(" as=");num(separate);add(" payload=");num(payload);add(" i=");num(i);add(" valid=");num(r->valid);add(" client_waits=");num(r->client_waits);add(" server_waits=");num(r->server_waits);
        struct stamp *a[4]={&r->a,&r->b,&r->c,&r->d};
        for(unsigned j=0;j<4;j++){add(" t");num(j);add("=");num(a[j]->t);add(" cpu");num(j);add("=");num(a[j]->cpu);}add("\n");flush();
    }
}
static void paired(unsigned kind,unsigned as,unsigned words) {
    mode=kind;separate=as;payload=words;count=kind==0?64:110;memset(s,0,sizeof(*s));
    ok(pacha_syscall3(PACHA_IPC_SYSCALL_CHANNEL_CREATE,(uintptr_t)pair,CR,0),24);
    uint64_t child=spawn();while(!LOAD(s->ready))pacha_syscall0(PACHA_RUNTIME_SYSCALL_YIELD);
    /* Parent blocks on IPC, so the receiver gets a chance to arm its wait.
     * Warm-up samples are kept, not silently filtered by latency. */
    for(unsigned i=0;i<count;i++) {
        struct row *r=&s->r[i];
        if(kind==0) {
            struct pacha_ipc_msg m={.word0=words?0x1234:0,.word1=words?1:0,.word2=words?2:0,.word3=words?0x5678:0};
            struct pacha_ipc_msg answer={0};
            r->a=tick();ok(measured(PACHA_IPC_SYSCALL_CALL,pair[0],(uintptr_t)&m,(uintptr_t)&answer,UINT64_MAX,PACHA_WAIT_UNINTERRUPTIBLE,i,&r->d),25);
            r->client_waits=1;
            if(answer.fd_count || answer.word0!=m.word0 || answer.word3!=m.word3)fail(26,answer.word0);
            r->valid=1;
        } else if(kind==1) {r->a=tick();notify(pair[0]);r->client_waits=waitnotify(pair[0]);r->d=tick();r->valid=1;}
        else { unsigned before=LOAD(s->seq);r->a=tick();pacha_syscall0(PACHA_RUNTIME_SYSCALL_YIELD);r->d=tick();unsigned after; do { after=LOAD(s->seq); r->b.t=LOAD(s->last.t);r->b.cpu=LOAD(s->last.cpu); } while((after&1) || after!=LOAD(s->seq));r->c=r->b;r->valid=after!=before && r->b.t>=r->a.t && r->b.t<=r->d.t; }
    }
    STORE(s->stop,1);emit(kind==0?"ipc":kind==1?"signal":"yield",count);join(child);closefd(pair[0]);closefd(pair[1]);
}
#ifdef S10B1_FD4200
static uint64_t held[4200];
static void fd4200(void) {
    struct pacha_fd_table_info table={0};
    ok(pacha_syscall2(PACHA_FD_SYSCALL_TABLE,0,(uintptr_t)&table),30);
    ok(pacha_syscall2(PACHA_FD_SYSCALL_TABLE,table.capacity-table.free_slots+4200,(uintptr_t)&table),31);
    if(table.free_slots<4200)fail(32,table.free_slots);
    for(unsigned i=0;i<4200;i++) held[i]=fd(pacha_syscall3(PACHA_FD_SYSCALL_EVENTFD_CREATE,0,
        PACHA_FD_RIGHT_INSPECT|PACHA_FD_RIGHT_CLOSE|PACHA_FD_RIGHT_READ|PACHA_FD_RIGHT_WRITE,0),33);
    for(unsigned i=0;i<4200;i++)closefd(held[i]);
    add("FDLIMIT_NATIVE_4200_PASS opened=4200\n");flush();
}
#endif
int main(void) {
#ifdef S10B1_FD4200
    fd4200();
#endif
    /* Fail before using RDTSCP when the host does not expose it. */
    unsigned a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000001u),"c"(0));
    if(!(d&(1u<<27)))fail(27,d);
    uint64_t v=fd(pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE,65536,PACHA_FD_RIGHT_MAP_READ|PACHA_FD_RIGHT_MAP_WRITE|PACHA_FD_RIGHT_CLOSE,0),28);
    long address=pacha_syscall6(PACHA_VM_SYSCALL_MMAP,v,0,65536,PACHA_PROT_READ|PACHA_PROT_WRITE,PACHA_MMAP_SHARED,0);
    if(address<4096)fail(29,address);s=(void*)(uintptr_t)address;memset(s,0,sizeof(*s));
    add("S10S_BEGIN\n");flush();
    for(unsigned i=0;i<110;i++){s->r[i].a=tick();s->r[i].d=tick();s->r[i].valid=1;}emit("overhead",110);
    for(unsigned i=0;i<110;i++){s->r[i].a=tick();ok(measured(0x42330000,0,0,0,0,0,i,&s->r[i].d),40);s->r[i].valid=1;}emit("empty",110);
    for(unsigned as=0;as<2;as++){paired(0,as,0);paired(0,as,4);}
    ok(pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP,(uintptr_t)s,65536),32);closefd(v);
    add("S10S_PASS\n");flush();return 0;
}
