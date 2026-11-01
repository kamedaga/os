#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <drm/i915_drm.h>
typedef uint32_t u32;typedef uint64_t u64;
#define GFP_KERNEL 0
struct kobox_linux_drm_file {int tag;};
static int owner_error,ioctl_error,calls,allocs,frees;
static bool fail_alloc;
static size_t expected_count;
static const void *original;
static int check_owner(struct kobox_linux_drm_file *file) {assert(file);return owner_error;}
static void *kmemdup(const void *p,size_t n,int flags) {
 (void)flags;if(fail_alloc)return NULL;void *copy=malloc(n);assert(copy);memcpy(copy,p,n);++allocs;return copy;
}
static void kfree(void *p) {if(p){++frees;free(p);}}
static int private_ioctl(struct kobox_linux_drm_file *file,unsigned long command,void *raw) {
 assert(file && command==DRM_IOCTL_I915_GEM_CREATE_EXT);++calls;
 struct drm_i915_gem_create_ext *a=raw;
 assert(a->size==2097152 && !a->handle && !a->flags);
 if(expected_count) {
  const struct drm_i915_gem_create_ext_memory_regions *e=(void *)(uintptr_t)a->extensions;
  assert(e && !e->base.next_extension && !e->base.name && !e->base.flags && !e->pad);
  for(unsigned i=0;i<4;++i)assert(!e->base.rsvd[i]);
  assert(e->num_regions==expected_count && (void *)(uintptr_t)e->regions!=original);
  assert(!memcmp((void *)(uintptr_t)e->regions,original,expected_count*4));
 } else assert(!a->extensions);
 a->size=2101248;a->handle=37;return ioctl_error;
}
#include "create_actual.inc"
int main(void) {
 struct kobox_linux_drm_file file={0};
 struct drm_i915_gem_memory_class_instance regions[65];
 for(unsigned i=0;i<65;++i)regions[i]=(struct drm_i915_gem_memory_class_instance){0,i};
 original=regions;
 for(unsigned n=0;n<=64;n+=n?63:1) for(unsigned error=0;error<2;++error) {
  expected_count=n;ioctl_error=error?-ENOMEM:0;u64 size=123;u32 handle=456;
  assert(kobox_linux_drm_i915_create_ext(&file,2097152,0,regions,n,&size,&handle)==ioctl_error);
  assert(size==(error?123:2101248) && handle==(error?456:37));
 }
 u64 size=123;u32 handle=456;int before=calls;
 assert(kobox_linux_drm_i915_create_ext(&file,0,0,regions,1,&size,&handle)==-EINVAL);
 assert(kobox_linux_drm_i915_create_ext(&file,2097152,2,regions,1,&size,&handle)==-EINVAL);
 assert(kobox_linux_drm_i915_create_ext(&file,2097152,0,regions,65,&size,&handle)==-EINVAL);
 regions[1]=regions[0];
 assert(kobox_linux_drm_i915_create_ext(&file,2097152,0,regions,2,&size,&handle)==-EINVAL);
 fail_alloc=true;
 assert(kobox_linux_drm_i915_create_ext(&file,2097152,0,regions,1,&size,&handle)==-ENOMEM);
 fail_alloc=false;owner_error=-EPERM;
 assert(kobox_linux_drm_i915_create_ext(&file,2097152,0,regions,1,&size,&handle)==-EPERM);
 assert(calls==before && size==123 && handle==456 && allocs==frees);
 puts("I915 CREATE_EXT actual wrapper: real UAPI/private region graph, bounds, duplicates, allocation/owner/driver errors and output preservation PASS");
}
