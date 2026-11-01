/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <drm/i915_drm.h>
#include <drm/drm_mode.h>
typedef uint32_t u32;typedef uint64_t u64;
#define PF_KTHREAD 1u
#define KOBOX_I915_MAP_CURRENT 0u
#define KOBOX_I915_MAP_WB 1u
#define KOBOX_I915_MAP_WC 2u
struct drm_driver {const char *name;};
struct drm_device {struct drm_driver *driver;};
struct drm_minor {struct drm_device *dev;};
struct drm_file {struct drm_minor *minor;};
struct file {struct drm_file *private_data;};
struct files_struct {int tag;};
struct task_struct {void *mm;unsigned flags,pid;struct files_struct *files;};
struct pt_regs {unsigned long di,si,dx;};
struct kobox_linux_drm_file {struct file *guard;struct task_struct *owner;
 struct files_struct *files;int fd;};
struct kobox_linux_drm_map_pages {u64 length;};
struct kobox_linux_drm_mapping {int tag;};
static struct drm_driver driver={"i915"};
static struct drm_device dev={&driver};
static struct drm_minor minor={&dev};
static struct drm_file drm_file={&minor};
static struct file installed={&drm_file},wrong;
static struct files_struct files;
static struct task_struct task={.pid=1,.files=&files},other;
static struct task_struct *current=&task;
static struct kobox_linux_drm_file file={&installed,&task,&files,71};
static bool irq_context,disabled,missing;
static int syscall_error,map_error,calls,map_calls,work;
static u32 kind;
static u64 real_offset;
static int in_interrupt(void) {return irq_context;}
static int irqs_disabled(void) {return disabled;}
static struct file *fget(int fd) {assert(fd==71);return missing?NULL:&installed;}
static void fput(struct file *f) {assert(f==&installed);}
static void task_work_run(void) {++work;}
static long __x64_sys_ioctl(struct pt_regs *regs) {
 assert(regs->di==71);
 if(kind==0) {
  assert(regs->si==DRM_IOCTL_MODE_MAP_DUMB);
  struct drm_mode_map_dumb *arg=(void *)regs->dx;
  assert(arg->handle==27 && !arg->pad && !arg->offset);
  ++calls;arg->offset=real_offset;return syscall_error;
 }
 assert(regs->si==DRM_IOCTL_I915_GEM_MMAP_OFFSET);
 struct drm_i915_gem_mmap_offset *arg=(void *)regs->dx;
 assert(arg->handle==27 && !arg->pad && !arg->offset && !arg->extensions);
 assert(arg->flags==(kind==KOBOX_I915_MAP_WB ? I915_MMAP_OFFSET_WB:I915_MMAP_OFFSET_WC));
 ++calls;arg->offset=real_offset;return syscall_error;
}
static int map_pages(struct kobox_linux_drm_file *f,u32 handle,u32 rights,
 u32 cache_kind,u64 offset,u64 *pages,size_t capacity,
 struct kobox_linux_drm_map_pages *result,struct kobox_linux_drm_mapping **owner) {
 assert(f==&file && handle==27 && rights==3 && cache_kind==(kind ? kind : KOBOX_I915_MAP_WC));
 assert(offset==real_offset && pages && capacity==1 && result && owner && !*owner);
 ++map_calls;return map_error;
}
#include "mmap_actual.inc"
int main(void) {
 u64 pages[1];struct kobox_linux_drm_map_pages result;
 struct kobox_linux_drm_mapping *owner=NULL;
 for(kind=0;kind<=2;++kind) for(unsigned e=0;e<3;++e) {
  syscall_error=e==1?-EIO:0;map_error=e==2?-EBUSY:0;
  real_offset=UINT64_C(0x12345678000);calls=map_calls=work=0;
  assert(kobox_linux_drm_i915_map_pages(&file,27,3,kind,pages,1,&result,&owner)==
   (e==1 ? -EIO : e==2 ? -EBUSY:0));
  assert(calls==1 && work==1 && map_calls==(e==1?0:1));
 }
 int before=calls;
 assert(kobox_linux_drm_i915_map_pages(&file,27,3,3,pages,1,&result,&owner)==-EINVAL);
 assert(kobox_linux_drm_i915_map_pages(&file,27,8,1,pages,1,&result,&owner)==-EINVAL);
 assert(kobox_linux_drm_i915_map_pages(&file,27,3,1,pages,0,&result,&owner)==-EINVAL);
 current=&other;
 assert(kobox_linux_drm_i915_map_pages(&file,27,3,1,pages,1,&result,&owner)==-EPERM);
 current=&task;file.guard=&wrong;
 assert(kobox_linux_drm_i915_map_pages(&file,27,3,1,pages,1,&result,&owner)==-EBADF);
 file.guard=&installed;driver.name="amdgpu";
 assert(kobox_linux_drm_i915_map_pages(&file,27,3,1,pages,1,&result,&owner)==-ENODEV);
 assert(calls==before);
 puts("I915 actual MMAP_OFFSET wrapper: private UAPI, real driver offset, WB/WC, ownership, errno and no lease on ioctl failure PASS");
}
