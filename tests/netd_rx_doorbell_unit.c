/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <poll.h>
#include "../userland/kobox2_adapter/net_frame_service.c"
#include "../userland/netd/src/kobox2_frame_client.c"

static struct ph_net_frame_service server;
static union { struct ph_net_frame_shared view; unsigned char bytes[PH_NET_FRAME_SHARED_BYTES]; } storage;
#define arena storage.view
static struct ph_ipc_packet reply;
static int event, closes, mapped, unmaps, reject_map, wrong_kind, bind_error;
static int (*port_wake)(void *); static void *port_context;
static unsigned queued;
static int fake_info(void *p,struct kobox_linux_net_info *i) { assert(p==&server);memset(i,0,sizeof(*i));i->mtu=1500;i->carrier=1;return 0; }
static int fake_read(void *p,struct kobox_linux_net_frame *f,size_t cap,size_t *n) { assert(p==&server);*n=queued<cap?queued:cap;queued-=*n;for(size_t k=0;k<*n;k++){f[k].length=60;memset(f[k].bytes,(int)k,60);}return 0; }
static int fake_write(void *p,const void *b,size_t n){assert(p==&server&&b&&n==60);return 0;}
static int fake_bind(void *p,int (*w)(void *),void *c){assert(p==&server);if(bind_error)return -EIO;port_wake=w;port_context=c;return 0;}
static int fake_stats(void *p,struct kobox_linux_net_rx_stats *s){assert(p==&server);memset(s,0,sizeof(*s));s->queued=queued;return 0;}
long pacha_syscall1(uint64_t nr,uint64_t fd){assert(nr==PACHA_FD_SYSCALL_CLOSE&&fd==103);closes++;return 0;}
long pacha_syscall2(uint64_t nr,uint64_t a,uint64_t b){
 if(nr==PACHA_VM_SYSCALL_MUNMAP){assert(a==(uintptr_t)&arena&&b==PH_NET_FRAME_SHARED_BYTES);unmaps++;return 0;}
 assert(nr==PACHA_FD_SYSCALL_GET_INFO);struct pacha_fd_info *i=(void*)(uintptr_t)b;memset(i,0,sizeof(*i));
 if(a==102){i->kind=PACHA_FD_KIND_VMO;i->size=PH_NET_FRAME_SHARED_BYTES;i->rights=PACHA_FD_RIGHT_MAP_READ|PACHA_FD_RIGHT_MAP_WRITE;}
 else{assert(a==103);i->kind=wrong_kind?PACHA_FD_KIND_VMO:PACHA_FD_KIND_EVENT;i->rights=PACHA_FD_RIGHT_WRITE|PACHA_FD_RIGHT_CLOSE;}
 return 0;
}
long pacha_syscall3(uint64_t nr,uint64_t fd,uint64_t p,uint64_t n){assert(nr==PACHA_FD_SYSCALL_WRITE&&fd==103&&n==8);return write(event,(void*)(uintptr_t)p,(size_t)n);}
long pacha_syscall6(uint64_t nr,uint64_t fd,uint64_t off,uint64_t size,uint64_t prot,uint64_t flags,uint64_t extra){(void)prot;(void)flags;(void)extra;assert(nr==PACHA_VM_SYSCALL_MMAP&&fd==102&&!off&&size==PH_NET_FRAME_SHARED_BYTES);if(reject_map)return -4;mapped++;return (long)(uintptr_t)&arena;}
pacha_fd_t pacha_vmo_create(uint64_t n,uint64_t r,uint32_t f){(void)r;(void)f;assert(n==PH_NET_FRAME_SHARED_BYTES);return 100;}
void *pacha_mmap(pacha_fd_t fd,uint64_t n,uint64_t p,uint64_t f,uint64_t o){(void)p;(void)f;assert(fd==100&&n==PH_NET_FRAME_SHARED_BYTES&&!o);return &arena;}
int pacha_munmap(void *p,uint64_t n){assert(p==&arena&&n==PH_NET_FRAME_SHARED_BYTES);unmaps++;return 0;}
pacha_fd_t pacha_eventfd_create(uint64_t v,uint64_t r,uint32_t f){(void)r;(void)f;assert(!v);event=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);assert(event>=0);return 101;}
int pacha_fd_close(pacha_fd_t fd){assert(fd==100||fd==101);if(fd==101){close(event);event=-1;}closes++;return 0;}
int ph_ipc_packet_release(struct ph_ipc_packet *p){for(size_t i=0;i<p->fd_count;i++)p->fds[i].fd=PH_IPC_NO_FD;return 0;}
int ph_ipc_send(struct ph_ipc *ipc,struct ph_ipc_packet *p){
 (void)ipc;
 if(server.operation){reply=*p;return 0;}
 struct ph_ipc_packet request=*p;
 if(p->operation==PH_NET_FRAME_ATTACH){assert(p->fd_count==2&&p->fds[1].fd==101);request.fds[0].fd=102;request.fds[1].fd=103;}
 int rc=prepare(&server,&request);
 if(rc)return rc;
 assert(!ph_ipc_packet_release(&request));assert(!dispatch(&server,&server));assert(!complete(&server,ipc));assert(!release(&server));return 0;
}
int ph_ipc_receive_wait(struct ph_ipc *i,uint64_t g,struct ph_ipc_packet *p,uint64_t t){(void)i;(void)g;(void)t;*p=reply;return 0;}
long pacha_fd_poll(struct pacha_pollfd *p,uint64_t n){(void)p;(void)n;return 0;}
static void reset(void){memset(&server,0,sizeof(server));server.generation=1;server.rx_wake_fd=-1;server.info=fake_info;server.read=fake_read;server.write=fake_write;server.bind_wake=fake_bind;server.rx_stats=fake_stats;}
int main(void){
 reset();struct netd_kobox2_frame_client client={0};struct ph_ipc ipc={.generation=1};
 assert(!netd_kobox2_frame_open(&client,&ipc,20,1));assert(mapped==1&&server.rx_wake_fd==103&&port_wake);
 queued=80;assert(!port_wake(port_context));struct pollfd fd={event,POLLIN,0};assert(poll(&fd,1,0)==1);uint64_t edge;assert(read(event,&edge,8)==8&&edge==1);
 size_t total=0;for(unsigned batch=0;batch<5;batch++){const struct kobox_linux_net_frame *frames;size_t count;assert(!netd_kobox2_frame_read(&client,&ipc,20,&frames,&count));assert(count==16&&frames[0].length==60);total+=count;}
 assert(total==80&&!queued); /* Full quantum must continue without needing another hint. */
 queued=1;assert(!port_wake(port_context)); /* Arrival before the retained wait is persistent. */
 fd.revents=0;assert(poll(&fd,1,0)==1&&fd.revents&POLLIN);
 struct kobox_linux_net_info info;assert(!netd_kobox2_frame_info(&client,&ipc,20,&info));assert(arena.rx_stats.queued==1);
 assert(!stop(&server));int old=closes;assert(server.rx_wake_fd==103&&closes==old);assert(!port_wake(port_context));
 port_wake=NULL;port_context=NULL; /* Linux port_close synchronizes work here. */
 assert(!ph_net_frame_service_finish(&server)&&closes==old+1);assert(!netd_kobox2_frame_close(&client));
 reset();wrong_kind=1;assert(netd_kobox2_frame_open(&client,&ipc,20,1)==-EPROTO);assert(!client.shared&&server.rx_wake_fd==-1);wrong_kind=0;
 reset();reject_map=1;assert(netd_kobox2_frame_open(&client,&ipc,20,1)==-ENOMEM);assert(!client.shared&&server.rx_wake_fd==-1);reject_map=0;
 reset();bind_error=1;assert(netd_kobox2_frame_open(&client,&ipc,20,1)==-EIO);assert(!client.shared);assert(!stop(&server));assert(!ph_net_frame_service_finish(&server));
 puts("PASS actual client/service: paired-FD attach, persistent event, 80-frame continuation, INFO counters, close ordering, invalid capability, mapping/bind failure cleanup");
}
