/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "gpud/drm_protocol.h"
typedef int64_t pacha_fd_t;
#define LPR_LINUX_EINVAL 22
#define LPR_LINUX_EFAULT 14
#define LPR_LINUX_EOPNOTSUPP 95
#define LPR_DRM_ERR_PROTOCOL 71
#define lpr_memcpy memcpy
#define lpr_memset memset
static unsigned char page[4096];
static void *allocation;
static unsigned pages, frees, calls;
static int fail_aux, call_error, bad_reply;
static uint32_t expected_count;
static pacha_fd_t lpr_create_tty_wire_page(void **out)
{ assert(!allocation); ++pages; *out=page; return 20; }
static void lpr_destroy_tty_wire_page(pacha_fd_t fd,void *p)
{ assert(fd==20 && p==page); ++frees; }
static void *lpr_gpud_drm_payload(void *p) { return p; }
static int lpr_drm_aux_create(uint64_t bytes,int reusable,pacha_fd_t *fd,void **out,uint64_t *size)
{
    assert(!reusable && bytes && bytes<=18432);
    if(fail_aux) return -12;
    allocation=malloc(bytes);assert(allocation);memset(allocation,0xcc,bytes);
    *fd=21;*out=allocation;*size=bytes;return 0;
}
static void lpr_drm_aux_destroy(pacha_fd_t fd,void *p,uint64_t bytes)
{
    if(p) {assert(fd==21 && p==allocation && bytes);free(p);allocation=NULL;}
    else assert(fd==-1 && !bytes);
}
static int64_t lpr_gpud_drm_call_transfer(uint64_t op,pacha_fd_t fd,void *p,
 size_t bytes,uint64_t in,uint64_t out,pacha_fd_t aux,int temporary) {
 assert(op==GPUD_DRM_OP_HANDLE_IOCTL && fd==20 && p==page && bytes==sizeof(gpud_drm_ioctl_request_t));
 assert(!in && !out && temporary);
 gpud_drm_ioctl_request_t *r=p;uint32_t count;uint64_t reserved;
 memcpy(&count,r->data+12,4);memcpy(&reserved,r->data+16,8);
 assert(r->handle==42 && r->request==GPUD_DRM_IOCTL_I915_GEM_CREATE_EXT && r->arg_size==24 && r->data_size==24);
 assert(count==expected_count && !reserved && r->aux_size==count*4);
 if(count)assert(aux==21 && allocation);else assert(aux==-1);
 ++calls;if(call_error)return call_error;
 uint64_t size=2101248;uint32_t handle=bad_reply==2?0:37,pad=0;
 memcpy(r->data,&size,8);memcpy(r->data+8,&handle,4);memcpy(r->data+12,&pad,4);r->data_size=bad_reply==1?24:16;return 0;
}
#include "../userland/personality/linux/runtime/lpr_drm/i915_create.h"
int main(void) {
 struct extension {gpud_i915_user_extension_t base;uint32_t pad,count;uint64_t pointer;} e={0};
 uint32_t region=0;e.count=1;e.pointer=(uintptr_t)&region;
 for(unsigned n=0;n<2;++n)for(unsigned fail=0;fail<4;++fail) {
  gpud_i915_create_ext_t input={.size=2097152,.handle=0xfeed,.extensions=n?(uintptr_t)&e:0};
  expected_count=n;call_error=fail==1?-12:0;bad_reply=fail>=2?fail-1:0;
  int64_t result=lpr_i915_create_ext_ioctl(42,(uintptr_t)&input);
  assert(result==(fail==1?-12:fail>=2?-71:0));
  assert(input.handle==(fail?0xfeedu:37u) && input.extensions==(n?(uintptr_t)&e:0));
  assert(!allocation && pages==frees);
 }
 call_error=bad_reply=0;expected_count=1;
 gpud_i915_create_ext_t input={.size=2097152,.extensions=(uintptr_t)&e};unsigned before=calls;
 e.base.next=(uintptr_t)&e;assert(lpr_i915_create_ext_ioctl(42,(uintptr_t)&input)==-22);e.base.next=0;
 e.base.name=2;assert(lpr_i915_create_ext_ioctl(42,(uintptr_t)&input)==-95);e.base.name=0;
 e.base.reserved[0]=1;assert(lpr_i915_create_ext_ioctl(42,(uintptr_t)&input)==-22);e.base.reserved[0]=0;
 e.count=65;assert(lpr_i915_create_ext_ioctl(42,(uintptr_t)&input)==-22);e.count=1;
 fail_aux=1;assert(lpr_i915_create_ext_ioctl(42,(uintptr_t)&input)==-12);fail_aux=0;
 assert(calls==before && !allocation && pages==frees);
 puts("LPR CREATE_EXT actual marshaler: pointer-free regions, cycle/reserved/unknown/bounds refusal, copyout and failure cleanup PASS");
}
