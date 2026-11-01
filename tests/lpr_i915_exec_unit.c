/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "gpud/drm_protocol.h"
#include <kobox2/i915_exec_shape.h>
typedef int64_t pacha_fd_t;
#define LPR_LINUX_EINVAL 22
#define LPR_LINUX_EFAULT 14
#define LPR_LINUX_EOPNOTSUPP 95
#define LPR_LINUX_EACCES 13
#define LPR_DRM_ERR_PROTOCOL 71
#define lpr_memcpy memcpy
#define lpr_memset memset
static unsigned char page[4096];static void *allocation;
static unsigned pages,frees,calls;static int fail_aux,transport_error,driver_error,bad_reply;
static pacha_fd_t lpr_create_tty_wire_page(void **out) {*out=page;++pages;return 20;}
static void lpr_destroy_tty_wire_page(pacha_fd_t fd,void *p) {assert(fd==20 && p==page);++frees;}
static void *lpr_gpud_drm_payload(void *p) {return p;}
static int lpr_drm_aux_create(uint64_t bytes,int reusable,pacha_fd_t *fd,void **out,uint64_t *size)
{
    assert(!reusable && bytes==96 && !allocation);
    if(fail_aux) return -12;
    allocation=malloc(bytes);assert(allocation);*fd=21;*out=allocation;*size=bytes;return 0;
}
static void lpr_drm_aux_destroy(pacha_fd_t fd,void *p,uint64_t bytes)
{
    if(p) {assert(fd==21 && p==allocation && bytes==96);free(p);allocation=NULL;}
    else assert(fd==-1 && !bytes);
}
static int64_t lpr_gpud_drm_call_transfer(uint64_t op,pacha_fd_t fd,void *p,
    size_t size,uint64_t in,uint64_t out,pacha_fd_t aux,int temporary)
{
    assert(op==GPUD_DRM_OP_HANDLE_IOCTL && fd==20 && p==page && size==sizeof(gpud_drm_ioctl_request_t));
    assert(!in && !out && aux==21 && temporary);++calls;
    gpud_drm_ioctl_request_t *r=p;
    assert(r->handle==42 && r->arg_size==64 && r->data_size==32 && r->aux_size==96);
    uint32_t header[8];memcpy(header,r->data,32);
    assert(header[0]==2 && header[1]==2 && !header[2] && header[3]==256 && header[6]==91 && !header[7]);
    assert(kb2_i915_exec_payload(allocation,96,2,2));
    gpud_i915_exec_object_wire_t objects[2];memcpy(objects,allocation,80);
    assert(objects[0].handle==1 && objects[1].handle==2 && objects[1].flags==4);
    const uint32_t fences[4]={7,1,8,2};assert(!memcmp((unsigned char *)allocation+80,fences,16));
    if(transport_error) return transport_error;
    for(unsigned i=0;i<2;++i) {objects[i].offset=UINT64_C(0x12345000)+(uint64_t)i*4096;}
    memcpy(allocation,objects,80);
    const int32_t result=driver_error;memcpy(r->data,&result,4);memset(r->data+4,0,4);r->data_size=8;
    if(bad_reply==1) r->data[4]=1;
    if(bad_reply==2) r->data_size=32;
    if(bad_reply==3) ((unsigned char *)allocation)[4]=1;
    return driver_error;
}
#include "../userland/personality/linux/runtime/lpr_drm/i915_exec.h"
static void clean(void) {assert(!allocation && pages==frees);}
static void roundtrip(void)
{
    const int errors[]={0,-12,-4,-11,-5};
    for(unsigned e=0;e<5;++e) for(unsigned wr=0;wr<2;++wr) {
        gpud_i915_exec_object_t objects[2]={{.handle=1,.relocations_pointer=1,.offset=0xaaa},
            {.handle=2,.relocations_pointer=UINT64_MAX,.offset=0xbbb,.flags=4}};
        const uint32_t fences[4]={7,1,8,2};
        gpud_i915_execbuffer_t input={.objects_pointer=(uintptr_t)objects,.object_count=2,.batch_length=256,
            .fence_count=2,.fences_pointer=(uintptr_t)fences,.flags=(1u<<11)|(1u<<18)|(1u<<19),.context_id=91};
        const gpud_i915_execbuffer_t original=input;driver_error=errors[e];
        assert(lpr_i915_exec_ioctl(42,wr?GPUD_DRM_IOCTL_I915_EXECBUFFER_WR:GPUD_DRM_IOCTL_I915_EXECBUFFER,(uintptr_t)&input)==driver_error);
        assert(!memcmp(&input,&original,sizeof(input)));
        for(unsigned i=0;i<2;++i) assert(objects[i].offset==UINT64_C(0x12345000)+(uint64_t)i*4096);
        assert(objects[0].relocations_pointer==1 && objects[1].relocations_pointer==UINT64_MAX && objects[1].flags==4);
        clean();
    }
    driver_error=0;
}
static void failures(void)
{
    gpud_i915_exec_object_t objects[2]={{.handle=1,.offset=0xaaa},{.handle=2,.offset=0xbbb,.flags=4}};
    const uint32_t fences[4]={7,1,8,2};
    gpud_i915_execbuffer_t input={.objects_pointer=(uintptr_t)objects,.object_count=2,.batch_length=256,
        .fence_count=2,.fences_pointer=(uintptr_t)fences,.flags=(1u<<11)|(1u<<18)|(1u<<19),.context_id=91};
    fail_aux=1;assert(lpr_i915_exec_ioctl(42,GPUD_DRM_IOCTL_I915_EXECBUFFER,(uintptr_t)&input)==-12);fail_aux=0;clean();
    transport_error=-5;assert(lpr_i915_exec_ioctl(42,GPUD_DRM_IOCTL_I915_EXECBUFFER,(uintptr_t)&input)==-5);
    assert(objects[0].offset==0xaaa && objects[1].offset==0xbbb);transport_error=0;clean();
    for(bad_reply=1;bad_reply<=3;++bad_reply) {
        assert(lpr_i915_exec_ioctl(42,GPUD_DRM_IOCTL_I915_EXECBUFFER,(uintptr_t)&input)==-71);
        assert(objects[0].offset==0xaaa && objects[1].offset==0xbbb);clean();
    }
    bad_reply=0;unsigned before=calls;
    objects[0].relocation_count=1;
    assert(lpr_i915_exec_ioctl(42,GPUD_DRM_IOCTL_I915_EXECBUFFER,(uintptr_t)&input)==-95);
    objects[0].relocation_count=0;objects[1].reserved=1;
    assert(lpr_i915_exec_ioctl(42,GPUD_DRM_IOCTL_I915_EXECBUFFER,(uintptr_t)&input)==-22);
    objects[1].reserved=0;input.flags|=1u<<9;
    assert(lpr_i915_exec_ioctl(42,GPUD_DRM_IOCTL_I915_EXECBUFFER,(uintptr_t)&input)==-13);
    input.flags&=~(1u<<9);input.flags|=1u<<17;
    assert(lpr_i915_exec_ioctl(42,GPUD_DRM_IOCTL_I915_EXECBUFFER,(uintptr_t)&input)==-95);
    input.flags&=~(1u<<17);input.object_count=8193;
    assert(lpr_i915_exec_ioctl(42,GPUD_DRM_IOCTL_I915_EXECBUFFER,(uintptr_t)&input)==-22);
    assert(calls==before);clean();
    assert(lpr_i915_exec_ioctl(42,0,1)==-22);
}
int main(void)
{
    roundtrip();failures();puts("LPR i915 EXEC: private BO/fence snapshots, actual errno/offset copyout, descriptor/pointer refusal and cleanup PASS");
}
