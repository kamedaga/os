/* Host model: no native instruction executes. */
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#define main worker_main
#include "../src/main.c"
#undef main
static netd_io_t backing;
static jmp_buf finished;
static unsigned sleeps,reads,heartbeats,raw_packets,markers,mode;
static uint64_t request_sequence;
pacha_fd_t pacha_vmo_create(uint64_t n,uint64_t r,uint32_t f)
{ (void)r;(void)f; assert(n==NETD_PAGE_BYTES); return 300; }
void *pacha_mmap(pacha_fd_t fd,uint64_t n,uint64_t p,uint64_t f,uint64_t o)
{ (void)n;(void)p;(void)f;(void)o;assert(fd==300);return &backing; }
int pacha_fd_close(pacha_fd_t fd) { assert(fd>=16);return 0; }
int pacha_ipc_channel_create(struct pacha_ipc_channel_pair *p,uint64_t r,uint32_t f)
{ (void)r;(void)f; static int fd=400;*p=(struct pacha_ipc_channel_pair){fd++,fd++};return 0; }
int pacha_ipc_recv(pacha_fd_t fd,struct pacha_ipc_msg *msg)
{ (void)msg;assert(fd>=16);return PACHA_ERR_EMPTY; }
int pacha_ipc_call(pacha_fd_t fd,const struct pacha_ipc_msg *q,
    struct pacha_ipc_msg *r,uint64_t timeout,int *committed)
{
    assert(fd==241 && timeout==PACHA_FD_WAIT_FOREVER);
    assert(q->word0==PACHA_SERVICE_REQUEST_MAGIC && q->word3==++request_sequence);
    *committed=1;
    *r=(struct pacha_ipc_msg){.word0=PACHA_SERVICE_REPLY_MAGIC,.word3=q->word3};
    if(q->word1==NETD_OP_PAGE_ATTACH) {
        assert(q->fd_count==2 && !q->word2);r->word2=42;
    } else if(q->word1==NETD_OP_SOCKET) {
        assert(q->fd_count==1 && q->word2==42);
        assert(((netd_socket_t *)&backing)->type==NETD_SOCK_DGRAM);r->word2=73;
    } else {
        assert(q->word1==NETD_OP_SEND && q->word2==42 && !q->fd_count);
        assert(backing.handle==73 && backing.length<=1024);
        const unsigned char *ip=(void *)&backing.addr.addr_be;
        assert(ip[0]==192 && ip[1]==168 && ip[2]==3 && ip[3]==8);
        assert(backing.addr.port_be==0x8813); /* 5000 in network order */
        if(!memcmp(backing.data,"[klogudp alive",14)) heartbeats++;
        else if(!memcmp(backing.data,"[klogudp overrun",16)) markers++;
        else { assert(backing.length==3 && !memcmp(backing.data,"abc",3));raw_packets++; }
        r->word2=backing.length;
        if(mode==2 && raw_packets==1 && backing.length==3) r->word1=(uint64_t)-11;
    }
    return 0;
}
long pacha_clock_read(uint64_t id,uint64_t *n)
{ assert(id==1);*n=123000000+sleeps*250000000ull;return 0; }
long pacha_syscall2(uint64_t nr,uint64_t a,uint64_t b)
{
    if(nr==PACHA_RUNTIME_SYSCALL_NANOSLEEP) {
        struct timespec *t=(void *)(uintptr_t)a;
        assert(t->tv_sec==0 && t->tv_nsec==250000000 && !b);
        if(++sleeps==3) longjmp(finished,1);
        return 0;
    }
    assert(nr==PACHA_KERNEL_LOG_SYSCALL_READ && b==sizeof(struct pacha_kernel_log_read));
    struct pacha_kernel_log_read *r=(void *)(uintptr_t)a;
    if(!reads++) assert(r->cursor==(mode==1 ? 0 : UINT64_MAX));
    else assert(r->cursor==((mode!=1 && reads==2) || (mode==2 && raw_packets==1) ? 100 : 103));
    r->next_cursor=103;r->oldest_cursor=99;r->dropped=2;
    if(reads==1 && mode!=1) {r->next_cursor=100;return 0;}
    if(reads==1 || reads==2 || (mode==2 && reads==3)) {
        r->written=3;r->overrun=reads==2;memcpy(r->bytes,"abc",3);
    }
    return 0;
}
int main(int argc,char **argv)
{
    assert(argc==2);mode=(unsigned)atoi(argv[1]);
    char *args[]={"klogudp","192.168.3.8","5000","--from-start"};
    assert(parse_destination("256.0.0.1","5000"));
    assert(parse_destination("1.2.3","5000"));
    assert(parse_destination("1.2.3.4","0"));
    assert(parse_destination("1.2.3.4","65536"));
    if(!setjmp(finished)) worker_main(mode==1 ? 4 : 3,args);
    assert(sleeps==3 && heartbeats==3 && raw_packets>=1 && markers>=1);
    assert(mode==2 ? failures==1 : failures==0);
    puts("worker model: heartbeat/cursor/overrun/send-error/250ms/IPv4 passed");
    return 0;
}
