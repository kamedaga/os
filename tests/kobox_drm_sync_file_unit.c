/* SPDX-License-Identifier: GPL-2.0-only */
/* Actual typed wrappers and service lifetime; Linux FDs/fences/syscalls are
 * modeled at their boundary. The separate fence-queue suite runs its actual
 * callback implementation. These models never replace production fences. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <drm/drm.h>
#include <linux/dma-buf.h>
#include "../kobox2/linux-sandbox/kobox/boot/drm_limits.h"
typedef uint32_t u32;typedef uint64_t u64;
#define U64_MAX UINT64_MAX
#define PF_KTHREAD 1u
#define DRM_PRIME_LIMIT KOBOX_DRM_PRIME_LIMIT
#define DRM_MAPPING_LIMIT KOBOX_DRM_MAPPING_LIMIT
struct file {int tag;};
struct files_struct {int tag;};
struct task_struct {void *mm;unsigned flags,pid;struct files_struct *files;};
struct pt_regs {unsigned long di,si,dx;};
struct kobox_linux_drm_file {struct file *guard;struct task_struct *owner;
 struct files_struct *files;int fd;};
struct dma_fence {unsigned refs;bool signaled;};
struct kobox_drm_fence_entry {struct dma_fence *fence;};
struct kobox_drm_fences {u64 last;};
struct drm_owned_sync_file {u64 id;int fd;};
struct drm_owned_prime {u64 id;int dma_fd;};
struct drm_owned_mapping {void *owner;};
struct drm_owned_file {u64 cookie;struct kobox_linux_drm_file *file;int close_error;};
struct kobox_linux_drm_service_report {int close_error;};
struct kobox_linux_drm_service {
 struct task_struct *owner;struct files_struct *files;
 struct drm_owned_file *entries;unsigned limit;
 struct drm_owned_prime primes[DRM_PRIME_LIMIT];
 struct drm_owned_sync_file sync_files[KOBOX_DRM_SYNC_FILE_LIMIT];
 struct drm_owned_mapping mappings[DRM_MAPPING_LIMIT];
 struct kobox_drm_fences fences;u64 sync_file_sequence;
 struct kobox_linux_drm_service_report report;bool stopping;
};
static struct file installed,other_guard;
static struct files_struct files,other_files;
static struct task_struct task={.pid=1,.files=&files},stranger;
static struct task_struct *current=&task;
static struct kobox_linux_drm_file drm={&installed,&task,&files,71};
static struct dma_fence sources[2]={{1,false},{1,false}};
static struct dma_fence *fd_fence[1024],*imported_syncobj,*imported_prime;
static struct kobox_drm_fence_entry *watches[1024];
static unsigned next_fd=100,watch_count,calls,export_calls,import_calls,closed,work;
static unsigned reserve_calls,cancel_calls,arm_calls;
static int syscall_error,reserve_error,arm_error,close_error;
static bool irq_context,irq_disabled,missing_fd,invalid_sync_file;
static int in_interrupt(void) {return irq_context;}
static int irqs_disabled(void) {return irq_disabled;}
static struct file *fget(int fd) {assert(fd==71);return missing_fd?NULL:&installed;}
static void fput(struct file *f) {assert(f==&installed);}
static void task_work_run(void) {++work;}
static void dma_fence_put(struct dma_fence *f) {assert(f && f->refs);--f->refs;}
static struct dma_fence *sync_file_get_fence(int fd) {
 assert(fd>=100 && fd<1024 && fd_fence[fd]);
 if(invalid_sync_file)return NULL;
 ++fd_fence[fd]->refs;return fd_fence[fd];
}
static int close_fd(int fd) {
 assert(fd>=100 && fd<1024 && fd_fence[fd]);
 dma_fence_put(fd_fence[fd]);fd_fence[fd]=NULL;++closed;return close_error;
}
static void import_fence(struct dma_fence **target,int fd) {
 assert(fd>=100 && fd<1024 && fd_fence[fd]);
 if(*target)dma_fence_put(*target);
 *target=fd_fence[fd];++(*target)->refs;
}
static long __x64_sys_ioctl(struct pt_regs *regs) {
 assert(regs && (regs->di==71 || regs->di==61));++calls;
 bool drm_fd=regs->di==71,export=false;
 int *fd;
 if(drm_fd) {
  struct drm_syncobj_handle *a=(void *)regs->dx;
  assert(a->handle==27 && !a->pad && !a->point);
  if(regs->si==DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD) {
   assert(a->flags==DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE);export=true;
  } else assert(regs->si==DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE &&
   a->flags==DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE);
  fd=&a->fd;
 } else {
  struct dma_buf_export_sync_file *a=(void *)regs->dx;
  assert(a->flags && !(a->flags & ~DMA_BUF_SYNC_RW));
  assert(regs->si==DMA_BUF_IOCTL_EXPORT_SYNC_FILE || regs->si==DMA_BUF_IOCTL_IMPORT_SYNC_FILE);
  export=regs->si==DMA_BUF_IOCTL_EXPORT_SYNC_FILE;fd=&a->fd;
 }
 if(export) {
  ++export_calls;assert(*fd==-1);
  if(syscall_error)return syscall_error;
  assert(next_fd<1024);*fd=(int)next_fd++;
  fd_fence[*fd]=&sources[drm_fd?0:1];++fd_fence[*fd]->refs;
 } else {
  ++import_calls;
  if(syscall_error)return syscall_error;
  import_fence(drm_fd?&imported_syncobj:&imported_prime,*fd);
 }
 return 0;
}
static int kobox_drm_fence_reserve(struct kobox_drm_fences *q,u64 session,u64 correlation,
 struct kobox_drm_fence_entry **out) {
 ++reserve_calls;assert(out && !*out);
 if(reserve_error)return reserve_error;
 if(!session || !correlation || correlation<=q->last)return -EINVAL;
 q->last=correlation;*out=calloc(1,sizeof(**out));assert(*out);return 0;
}
static int kobox_drm_fence_arm(struct kobox_drm_fence_entry *entry,struct dma_fence *fence) {
 ++arm_calls;assert(entry && fence);
 if(arm_error)return arm_error;
 assert(watch_count<1024);entry->fence=fence;watches[watch_count++]=entry;return 0;
}
static int kobox_drm_fence_cancel(struct kobox_drm_fence_entry **entry) {
 assert(entry && *entry && !(*entry)->fence);free(*entry);*entry=NULL;++cancel_calls;return 0;
}
static int kobox_drm_fences_quiesce(struct kobox_drm_fences *q) {
 (void)q;
 for(unsigned i=0;i<watch_count;++i) {dma_fence_put(watches[i]->fence);free(watches[i]);watches[i]=NULL;}
 watch_count=0;return 0;
}
static int kobox_linux_drm_mapping_quiesce(void) {return 0;}
static int release_mapping(struct drm_owned_mapping *m) {assert(!m->owner);return 0;}
static int release_prime(struct drm_owned_prime *p) {p->id=0;p->dma_fd=-1;return 0;}
static int close_entry(struct kobox_linux_drm_service *s,struct drm_owned_file *f) {
 (void)s;f->file=NULL;return 0;
}
#include "sync_file_actual.inc"
static struct drm_owned_file entry={3,&drm,0};
static void init_service(struct kobox_linux_drm_service *s) {
 *s=(struct kobox_linux_drm_service){.owner=&task,.files=&files,.entries=&entry,.limit=1};
 entry.file=&drm;s->primes[0]=(struct drm_owned_prime){4097,61};
}
static void drop_imports(void) {
 if(imported_syncobj)dma_fence_put(imported_syncobj);
 if(imported_prime)dma_fence_put(imported_prime);
 imported_syncobj=imported_prime=NULL;
}
static void transfer_without_wait(void) {
 struct kobox_linux_drm_service s;init_service(&s);
 u64 sync_token=0,prime_token=0;
 assert(!kobox_linux_drm_service_syncobj_export_file(&s,3,27,9,1,&sync_token));
 assert(!kobox_linux_drm_service_prime_export_file(&s,4097,3,9,2,&prime_token));
 assert(sync_token && prime_token && sync_token!=prime_token && watch_count==2);
 assert(watches[0]->fence==&sources[0] && watches[1]->fence==&sources[1]);
 assert(!sources[0].signaled && !sources[1].signaled);
 assert(!kobox_linux_drm_service_syncobj_import_file(&s,3,27,prime_token));
 assert(!kobox_linux_drm_service_prime_import_file(&s,4097,2,sync_token));
 assert(imported_syncobj==&sources[1] && imported_prime==&sources[0]);
 assert(!sources[0].signaled && !sources[1].signaled); /* No CPU wait or fabricated completion. */
 assert(!kobox_linux_drm_service_release_sync_file(&s,sync_token));
 assert(kobox_linux_drm_service_release_sync_file(&s,sync_token)==-ENOENT);
 assert(watch_count==2 && imported_prime==&sources[0] && !sources[0].signaled);
 entry.file=NULL; /* Export remains valid after its originating DRM file closes. */
 assert(!kobox_linux_drm_service_prime_import_file(&s,4097,1,prime_token));
 assert(kobox_linux_drm_service_syncobj_import_file(&s,3,27,prime_token)==-ENOENT);
 entry.file=&drm;
 struct kobox_linux_drm_service_report report;
 assert(!kobox_linux_drm_service_quiesce(&s,&report));
 assert(s.stopping && !watch_count && !report.close_error);
 for(unsigned i=0;i<KOBOX_DRM_SYNC_FILE_LIMIT;++i)assert(!s.sync_files[i].id);
 assert(kobox_linux_drm_service_prime_import_file(&s,4097,1,prime_token)==-ESHUTDOWN);
 drop_imports();assert(sources[0].refs==1 && sources[1].refs==1);
}
static void failures_preserve_ownership(void) {
 struct kobox_linux_drm_service s;init_service(&s);
 u64 token=0xfeed;unsigned before=calls;
 current=&stranger;
 assert(kobox_linux_drm_service_prime_export_file(&s,4097,3,9,1,&token)==-EPERM);
 current=&task;irq_context=true;
 assert(kobox_linux_drm_service_syncobj_export_file(&s,3,27,9,1,&token)==-EPERM);
 irq_context=false;
 assert(kobox_linux_drm_service_prime_export_file(&s,4097,4,9,1,&token)==-EINVAL);
 assert(kobox_linux_drm_service_prime_export_file(&s,4098,3,9,1,&token)==-ENOENT);
 reserve_error=-ENOMEM;
 assert(kobox_linux_drm_service_prime_export_file(&s,4097,3,9,1,&token)==-ENOMEM);
 reserve_error=0;assert(calls==before && token==0xfeed);
 for(unsigned i=0;i<KOBOX_DRM_SYNC_FILE_LIMIT;++i)s.sync_files[i].id=i+1;
 assert(kobox_linux_drm_service_prime_export_file(&s,4097,3,9,1,&token)==-ENOSPC);
 memset(s.sync_files,0,sizeof(s.sync_files));s.sync_file_sequence=U64_MAX;
 assert(kobox_linux_drm_service_prime_export_file(&s,4097,3,9,1,&token)==-EOVERFLOW);
 s.sync_file_sequence=0;assert(calls==before);
 syscall_error=-EIO;unsigned canceled=cancel_calls;
 assert(kobox_linux_drm_service_prime_export_file(&s,4097,3,9,1,&token)==-EIO);
 assert(cancel_calls==canceled+1 && !watch_count && token==0xfeed);
 syscall_error=0;invalid_sync_file=true;unsigned closes=closed;
 assert(kobox_linux_drm_service_prime_export_file(&s,4097,3,9,2,&token)==-EBADF);
 assert(closed==closes+1 && !watch_count && token==0xfeed);
 invalid_sync_file=false;arm_error=-EIO;
 assert(kobox_linux_drm_service_syncobj_export_file(&s,3,27,9,3,&token)==-EIO);
 arm_error=0;
 assert(sources[0].refs==1 && sources[1].refs==1 && !watch_count);
 assert(!kobox_linux_drm_service_prime_export_file(&s,4097,3,9,4,&token));
 syscall_error=-ENOMEM;
 assert(kobox_linux_drm_service_syncobj_import_file(&s,3,27,token)==-ENOMEM);
 syscall_error=0;
 assert(!kobox_linux_drm_service_syncobj_import_file(&s,3,27,token));
 assert(kobox_linux_drm_service_syncobj_import_file(&s,3,27,0)==-ENOENT);
 assert(kobox_linux_drm_service_prime_import_file(&s,4097,0,token)==-EINVAL);
 close_error=-EIO;
 assert(kobox_linux_drm_service_release_sync_file(&s,token)==-EIO);
 close_error=0;
 assert(s.report.close_error==-EIO && !find_sync_file(&s,token));
 assert(kobox_linux_drm_service_syncobj_import_file(&s,3,27,token)==-ESHUTDOWN);
 assert(!kobox_drm_fences_quiesce(&s.fences));drop_imports();
 assert(sources[0].refs==1 && sources[1].refs==1);
}
static void wrapper_admission(void) {
 int fd=0xfeed;unsigned before=calls;
 assert(kobox_linux_drm_syncobj_export_file(&drm,0,&fd)==-EINVAL);
 assert(kobox_linux_drm_syncobj_import_file(&drm,27,-1)==-EINVAL);
 assert(kobox_linux_drm_dmabuf_export_file(61,0,&fd)==-EINVAL);
 assert(kobox_linux_drm_dmabuf_import_file(61,4,100)==-EINVAL);
 current=&stranger;
 assert(kobox_linux_drm_syncobj_export_file(&drm,27,&fd)==-EPERM);
 current=&task;task.files=&other_files;
 assert(kobox_linux_drm_syncobj_export_file(&drm,27,&fd)==-EPERM);
 task.files=&files;irq_disabled=true;
 assert(kobox_linux_drm_dmabuf_export_file(61,1,&fd)==-EPERM);
 irq_disabled=false;drm.guard=&other_guard;
 assert(kobox_linux_drm_syncobj_export_file(&drm,27,&fd)==-EBADF);
 drm.guard=&installed;
 assert(calls==before && fd==0xfeed);
 syscall_error=-ENOMEM;
 assert(kobox_linux_drm_syncobj_export_file(&drm,27,&fd)==-ENOMEM && fd==0xfeed);
 assert(kobox_linux_drm_dmabuf_export_file(61,3,&fd)==-ENOMEM && fd==0xfeed);
 syscall_error=0;
}
int main(void) {
 wrapper_admission();transfer_without_wait();failures_preserve_ownership();
 assert(export_calls && import_calls && reserve_calls && arm_calls && work && closed);
 for(unsigned i=100;i<1024;++i)assert(!fd_fence[i]);
 puts("Actual sync_file wrappers+ledger: private Linux UAPI/FDs, original unsignaled fence identity, nonblocking import and close-after-DRM-close PASS");
 puts("Actual sync_file failure cleanup: reserve/full/overflow/ioctl/invalid-fence/arm/import/close, retained imports, quiesce and reference balance PASS");
}
