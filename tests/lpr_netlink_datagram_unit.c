#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pacha/abi.h>
#include <netd/ipc_protocol.h>
#define LPR_LINUX_UIO_MAXIOV 1024
#define LPR_LINUX_EMSGSIZE EMSGSIZE
#define LPR_LINUX_EFAULT EFAULT
#define LPR_LINUX_EINVAL EINVAL
#define LPR_LINUX_EAFNOSUPPORT EAFNOSUPPORT
#define LPR_LINUX_EOPNOTSUPP EOPNOTSUPP
#define LPR_LINUX_EAGAIN EAGAIN
#define LPR_LINUX_EIO EIO
#define LPR_LINUX_AF_NETLINK 16
#define LPR_LINUX_NETLINK_KOBJECT_UEVENT 15
#define LPR_LINUX_MSG_PEEK 2
#define LPR_LINUX_MSG_TRUNC 32
#define LPR_LINUX_POLLOUT 4
#define LPR_LINUX_POLLIN 1
#define lpr_memset memset
#define lpr_memcpy memcpy
typedef struct {uint64_t base,len;} lpr_linux_iovec_t;
typedef struct {uint16_t family,pad;uint32_t pid,groups;} lpr_linux_sockaddr_nl_t;
typedef struct {int dummy;} lpr_wait_deadline_t;
struct backend_state {uint64_t handle;int rcvtimeo_ms,sndtimeo_ms;uint16_t protocol;uint32_t peer_addr_be,local_addr_be;};
static struct backend_state backend={.handle=3};
static union {uint64_t align;unsigned char bytes[NETD_PAGE_BYTES];} page;
static int calls,queued,readable,binds;
static unsigned char sent[64];static size_t sent_size;
static struct backend_state *lpr_socket_backend(uint64_t fd){(void)fd;return &backend;}
static pacha_fd_t lpr_netd_create_page(void **p){*p=page.bytes;return 20;}
static void lpr_netd_destroy_page(pacha_fd_t fd,void*p){(void)fd;(void)p;}
static int lpr_socket_op_nonblocking(uint64_t fd,uint64_t f){(void)fd;(void)f;return 1;}
static int64_t lpr_linux_socket_wait_events_until(uint64_t f,uint64_t e,lpr_wait_deadline_t*d,unsigned*w){(void)f;(void)e;(void)d;(void)w;return -EAGAIN;}
static int lpr_wait_deadline_init(lpr_wait_deadline_t*d,int64_t t){(void)d;(void)t;return 0;}
static void lpr_linux_socket_mark_readable(uint64_t fd){(void)fd;readable++;}
static int64_t lpr_netd_call(uint64_t op,pacha_fd_t fd,uint64_t zero,uint64_t*out)
{
 (void)fd;(void)zero;calls++;netd_io_t*r=(void*)page.bytes;
 if(op==NETD_OP_BIND){netd_netlink_bind_t*b=(void*)page.bytes;b->pid=77;binds++;return 0;}
 if(op==NETD_OP_SEND){assert(r->length<=sizeof(sent));memcpy(sent,r->data,r->length);sent_size=r->length;*out=r->length;return 0;}
 assert(op==NETD_OP_RECV);if(!queued)return -EAGAIN;
 size_t n=r->length<8?r->length:8;uint64_t flags=r->flags;
 memcpy(r->data,"abcdefgh",n);r->flags=n<8?32:0;*out=(flags&32)?8:n;
 if(!(flags&2))queued=0;return 0;
}
#include "../userland/personality/linux/runtime/lpr_netlink.h"
int main(void)
{
 char a[]="abc",b[]="def";lpr_linux_iovec_t out[]={{(uintptr_t)a,3},{(uintptr_t)b,3}};
 lpr_linux_sockaddr_nl_t kernel={.family=16};
 assert(lpr_netlink_send(4,out,2,0,(uintptr_t)&kernel,sizeof(kernel),0)==6);
 assert(calls==2&&binds==1&&backend.local_addr_be==77&&sent_size==6&&!memcmp(sent,"abcdef",6));
 assert(lpr_netlink_send(4,out,2,0,0,0,0)==6&&binds==1);
 kernel.pid=1;assert(lpr_netlink_send(4,out,2,0,(uintptr_t)&kernel,sizeof(kernel),0)==-EOPNOTSUPP);kernel.pid=0;
 assert(lpr_netlink_send(4,out,2,0,(uintptr_t)&kernel,11,0)==-EINVAL);
 unsigned char x[4]={0,0,0,0xcc},y[4]={0,0,0,0xdd};
 lpr_linux_iovec_t in[]={{(uintptr_t)x,3},{(uintptr_t)y,2}};uint32_t flags=0;
 queued=1;assert(lpr_netlink_receive(4,in,2,2|32,0,0,&flags,0)==8);
 assert(queued&&readable&&flags==32&&!memcmp(x,"abc",3)&&!memcmp(y,"de",2)&&x[3]==0xcc&&y[3]==0xdd);
 assert(lpr_netlink_receive(4,in,2,0,0,0,&flags,0)==5&&!queued&&flags==32);
 queued=1;assert(lpr_netlink_receive(4,0,0,32,0,0,&flags,0)==8&&!queued&&flags==32);
 queued=1;assert(lpr_netlink_receive(4,0,0,0,0,0,&flags,0)==0&&!queued&&flags==32);
 unsigned char name[16];memset(name,0xa5,sizeof(name));uint32_t length=3;
 lpr_netlink_name((uintptr_t)name,(uintptr_t)&length,9,0);assert(length==12&&name[0]==16&&name[3]==0xa5);
 puts("LPR netlink: datagram gather/scatter, kernel address, PEEK/TRUNC bounds, zero receive and short names PASS");
}
