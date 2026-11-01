#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"
#include <unixd/transfer.h>
/* Compile exact DMABUF branches from ops.c. Model only FD refcounts and the
 * surrounding allocator/native RPC boundaries; no transfer logic is copied. */
struct native { unsigned object; uint64_t rights; };
static struct native fds[128];
static unsigned refs[3], owner_closed, collected, duplicate_fail;
static unsigned next_fd=16;
void *lpr_memset(void *p,int c,size_t n) { return memset(p,c,n); }
int lpr_native_fd_info(uint64_t fd,struct pacha_fd_info *out) {
 if(fd>=128 || !fds[fd].object)return 0;
 *out=(struct pacha_fd_info){.kind=fds[fd].object==1?PACHA_FD_KIND_VMO:PACHA_FD_KIND_CHANNEL,
  .size=fds[fd].object==1?4096:0,.rights=fds[fd].rights};return 1;
}
int64_t lpr_pacha_status_to_errno(int64_t value) { return value>0?-value:value; }
int64_t lpr_pacha_syscall4(uint64_t n,uint64_t fd,uint64_t cmd,uint64_t min,uint64_t rights) {
 assert(n==PACHA_FD_SYSCALL_FCNTL && cmd==PACHA_FD_FCNTL_DUP && min==16 && fds[fd].object);
 assert(rights==fds[fd].rights);
 if(duplicate_fail && fds[fd].object==duplicate_fail)return PACHA_SYSCALL_ERR_ALLOC;
 unsigned dst=next_fd++; assert(dst<90);
 fds[dst]=fds[fd];++refs[fds[dst].object];return dst;
}
int64_t lpr_close_native_fd_if_open(uint64_t fd) {
 assert(fd<128 && fds[fd].object);unsigned obj=fds[fd].object;fds[fd]=(struct native){0};
 assert(refs[obj]);--refs[obj];
 if(obj==2 && !refs[obj] && owner_closed)++collected;
 return 0;
}
int64_t lpr_pacha_syscall1(uint64_t op,uint64_t fd) {
 assert(op==PACHAOS_SYSCALL_FD_CLOSE);return lpr_close_native_fd_if_open(fd);
}
int64_t lpr_close_ignore_badfd(int64_t value) { return value; }
int64_t lpr_drm_prime_ref(uint32_t op,uint64_t token) {
 assert(op==GPUD_DRM_OP_PRIME_RELEASE && token==4097);++owner_closed;return 0;
}
int64_t lpr_backend_state_free(void *p,uint64_t n) { (void)p;(void)n;assert(0);return -1; }
#define LPR_LINUX_EMSGSIZE 90
#include "dmabuf_transfer_actual.inc"
static void run(unsigned writable) {
 memset(fds,0,sizeof(fds));memset(refs,0,sizeof(refs));
 next_fd=16;owner_closed=collected=duplicate_fail=0;
 uint64_t view=PACHA_FD_RIGHT_DUP|PACHA_FD_RIGHT_TRANSFER|PACHA_FD_RIGHT_CLOSE|PACHA_FD_RIGHT_MAP_READ|
  (writable?PACHA_FD_RIGHT_MAP_WRITE:0);
 fds[90]=(struct native){1,view};fds[91]=(struct native){2,GPUD_DRM_PRIME_OWNER_RIGHTS};
 refs[1]=refs[2]=1;
 lpr_dmabuf_backend_t source={.active=1,.writable=writable,.token=4097,.size=4096,
  .flags=writable?LPR_LINUX_O_RDWR:LPR_LINUX_O_RDONLY,.native.raw=90,.lease_fd.raw=91};
 lpr_fd_pin_t pin={.ops_id=LPR_FD_OPS_DMABUF,.state=&source,.effective_rights=UINT32_MAX};
 struct unix_transfer_item item; pacha_fd_t caps[2];uint32_t count=0;
 duplicate_fail=2;
 assert(prepare(&pin,&item,caps,2,&count)<0 && refs[1]==1 && refs[2]==1 && !count);
 duplicate_fail=0;
 assert(!prepare(&pin,&item,caps,2,&count) && count==2 && refs[1]==2 && refs[2]==2);
 assert(fds[caps[1]].rights==GPUD_DRM_PRIME_OWNER_RIGHTS && fds[caps[1]].object==2);
 lpr_dmabuf_backend_t received={0};
 assert(!stage(&item,caps,&received) && received.writable==writable && received.token==4097);
 assert(received.lease_fd.raw==caps[1] && (received.reserved0&LPR_BACKEND_TRANSFER_LEASE));
 struct unix_transfer_item forged=item; forged.flags=writable?LPR_LINUX_O_RDONLY:LPR_LINUX_O_RDWR;
 lpr_dmabuf_backend_t untouched={0};
 assert(stage(&forged,caps,&untouched)==-LPR_LINUX_EINVAL && !untouched.active);
 uint64_t rights=fds[caps[1]].rights;fds[caps[1]].rights&=~PACHA_FD_RIGHT_CALL;
 assert(stage(&item,caps,&untouched)==-LPR_LINUX_EINVAL);fds[caps[1]].rights=rights;
 /* A dup/SCM holder keeps the same authenticated peer alive after the
  * original export closes; closing that holder must not release by number. */
 assert(!lpr_ops_dmabuf_close(&source) && owner_closed==1 && !collected && refs[2]==1);
 assert(!lpr_ops_dmabuf_close(&received) && owner_closed==1 && collected==1 && !refs[1] && !refs[2]);
}
int main(void) {
 run(0);run(1);
 puts("LPR DMA-BUF SCM/dup: same owner capability, RO/RW, forged-mode/CALL refusal, rollback and last-close lifetime PASS");
}
