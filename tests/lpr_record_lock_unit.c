#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"
#ifndef LOCK_IMPLEMENTATION
#define LOCK_IMPLEMENTATION "../userland/personality/linux/runtime/lpr_fd/record_lock.c"
#endif
#include LOCK_IMPLEMENTATION
volatile uint32_t lpr_posix_locks_used;
static lpr_filed_backend_t backend={.active=1,.handle=99};
static kb2_fs_record_lock_t reply={.type=2};
static unsigned calls;
static int malformed,transport_error;
static kb2_fs_request_t seen;
int lpr_user_range_plausible(uint64_t p,uint64_t n){return p>=4096&&p<=0x7fffffffffffULL&&n<=0x800000000000ULL-p;}
void *lpr_memcpy(void*d,const void*s,size_t n){return memcpy(d,s,n);}
int64_t lpr_filed_fd_binding(uint64_t fd){return fd==9 ? 0 : -LPR_LINUX_EBADF;}
lpr_filed_backend_t *lpr_filed_backend(uint64_t fd){return fd==9?&backend:0;}
int64_t lpr_linux_lseek(uint64_t fd,uint64_t off,uint64_t whence){assert(fd==9&&!off&&whence==1);return 100;}
int64_t lpr_linux_fstat(uint64_t fd,uint64_t ptr,unsigned *w){(void)w;assert(fd==9);((lpr_linux_stat_t*)(uintptr_t)ptr)->st_size=200;return 0;}
int64_t lpr_linux_nanosleep(uint64_t a,uint64_t b,unsigned*c){(void)a;(void)b;(void)c;assert(0);return 0;}
int64_t lpr_fs_call(kb2_fs_request_t*r,const char*a,const char*b,const void*c,uint64_t d,void*e,uint64_t n,kb2_fs_response_t*out,pacha_fd_t f,pacha_fd_t*g){
 (void)a;(void)b;(void)c;(void)d;(void)f;(void)g;calls++;seen=*r;
 if(transport_error)return -LPR_LINUX_EAGAIN;
 if(r->flags!=5 || !e || n!=KB2_FILESYSTEM_RECORD_LOCK_SIZE)return -LPR_LINUX_EOPNOTSUPP;
 assert(!kb2_fs_record_lock_encode(e,n,&reply));
 out->data_length=malformed==2?1:n;
 if(malformed==1)((unsigned char*)e)[4]=1;
 return 0;
}
/* Only used by the archived filed-branch negative build. */
int lpr_fd_is_filed(uint64_t f){return f==9;}
int64_t lpr_filed_endpoint_ready(void){return 0;}
int64_t lpr_control_get_fd_flags(uint64_t f){(void)f;assert(0);return 0;}
int lpr_control_set_fd_flags(uint64_t f,uint64_t a){(void)f;(void)a;assert(0);return 0;}
int64_t lpr_linux_dup(uint64_t f,uint64_t a,uint64_t c){(void)f;(void)a;(void)c;assert(0);return 0;}
struct lock {int16_t type,whence;int32_t pad;int64_t start,length;int32_t pid,tail;};
static int64_t query(struct lock*l){return lpr_filed_record_lock(9,5,(uintptr_t)l);}
int main(void){
 struct lock l={.type=1,.whence=1,.start=-20,.length=10,.pid=777,.pad=123,.tail=456},before=l;
 assert(!query(&l));assert(seen.offset==80&&seen.length==10&&seen.handle==99&&seen.flags==5);
 before.type=2;assert(!memcmp(&l,&before,sizeof l)); /* no-conflict preserves relative input */
 reply=(kb2_fs_record_lock_t){.type=1,.start=88,.length=4,.pid=1234};l.type=1;assert(!query(&l));assert(l.type==1&&!l.whence&&l.start==88&&l.length==4&&l.pid==1234);
 l=(struct lock){.type=0,.whence=2,.start=-10,.length=-5};reply.type=2;assert(!query(&l));assert(seen.offset==190&&(int64_t)seen.length==-5);
 l=(struct lock){.type=1,.start=8,.length=0};reply=(kb2_fs_record_lock_t){.type=0,.start=4,.length=0,.pid=17};assert(!query(&l));assert(!l.length&&l.pid==17);
 before=l;malformed=1;assert(query(&l)==-LPR_LINUX_EIO&&!memcmp(&l,&before,sizeof l));malformed=2;assert(query(&l)==-LPR_LINUX_EIO&&!memcmp(&l,&before,sizeof l));malformed=0;transport_error=1;assert(query(&l)==-LPR_LINUX_EAGAIN&&!memcmp(&l,&before,sizeof l));transport_error=0;
 unsigned n=calls;l.whence=3;assert(query(&l)==-LPR_LINUX_EINVAL&&calls==n);l.whence=0;l.start=-1;assert(query(&l)==-LPR_LINUX_EINVAL&&calls==n);l.whence=1;l.start=INT64_MAX;assert(query(&l)==-LPR_LINUX_EOVERFLOW&&calls==n);assert(lpr_filed_record_lock(9,5,0)==-LPR_LINUX_EFAULT);
 uint8_t payload[KB2_FILESYSTEM_RECORD_LOCK_SIZE];kb2_fs_record_lock_t v={.type=1,.start=INT64_MAX,.length=2,.pid=4};assert(kb2_fs_record_lock_encode(payload,sizeof payload,&v));v.length=0;assert(!kb2_fs_record_lock_encode(payload,sizeof payload,&v));payload[28]=1;assert(kb2_fs_record_lock_decode(payload,sizeof payload,&v));
 puts("LPR_RECORD_LOCK_PASS relative/no-conflict/copyout/conflict/PID/EOF/negative/malformed/transport/overflow");
}
