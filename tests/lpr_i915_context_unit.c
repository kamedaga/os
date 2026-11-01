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
static uint32_t expected_count, expected_engines, expected_first;
static gpud_i915_context_param_wire_t expected_params[64];
static unsigned char expected_engine_data[256];
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
    size_t size,uint64_t in,uint64_t out,pacha_fd_t aux,int temporary)
{
    assert(op==GPUD_DRM_OP_HANDLE_IOCTL && fd==20 && p==page && size==sizeof(gpud_drm_ioctl_request_t));
    assert(!in && !out && temporary);
    gpud_drm_ioctl_request_t *r=p;uint32_t header[4];memcpy(header,r->data,16);
    assert(r->handle==42 && r->data_size==16);
    assert(header[0]==expected_first && header[1]==expected_count && header[2]==expected_engines && !header[3]);
    assert(r->aux_size==(uint64_t)expected_count*32+(uint64_t)expected_engines*4);
    if(expected_count) {
        assert(aux==21 && allocation);
        assert(!memcmp(allocation,expected_params,expected_count*32));
        assert(!memcmp((unsigned char *)allocation+expected_count*32,expected_engine_data,expected_engines*4));
    } else assert(aux==-1 && !allocation);
    ++calls;
    if(call_error) return call_error;
    if(r->request!=GPUD_DRM_IOCTL_I915_CONTEXT_SETPARAM) {
        uint32_t result[2]={bad_reply==2?0:91,bad_reply==3?1:0};
        memcpy(r->data,result,8);r->data_size=bad_reply==1?16:8;
    }
    return 0;
}
#include "../userland/personality/linux/runtime/lpr_drm/i915_context.h"
static void clean(void) {assert(!allocation && pages==frees);}
static void basic(void)
{
    expected_first=expected_count=expected_engines=0;
    uint32_t legacy[2]={0xaabbccdd,0};
    assert(!lpr_i915_context_ioctl(42,GPUD_DRM_IOCTL_I915_CONTEXT_CREATE,(uintptr_t)legacy));
    assert(legacy[0]==91 && !legacy[1]);clean();
    gpud_i915_context_create_t create={.context_id=0xaabbccdd,.extensions=1};
    /* Without USE_EXTENSIONS Linux ignores the pointer entirely. */
    assert(!lpr_i915_context_ioctl(42,GPUD_DRM_IOCTL_I915_CONTEXT_CREATE_EXT,(uintptr_t)&create));
    assert(create.context_id==91 && create.extensions==1);clean();
    for(bad_reply=1;bad_reply<=3;++bad_reply) {
        create.context_id=0xaabbccdd;
        assert(lpr_i915_context_ioctl(42,GPUD_DRM_IOCTL_I915_CONTEXT_CREATE_EXT,(uintptr_t)&create)==-71);
        assert(create.context_id==0xaabbccdd);clean();
    }
    bad_reply=0;call_error=-4;create.context_id=0xaabbccdd;
    assert(lpr_i915_context_ioctl(42,GPUD_DRM_IOCTL_I915_CONTEXT_CREATE_EXT,(uintptr_t)&create)==-4);
    assert(create.context_id==0xaabbccdd);call_error=0;clean();
}
static void graph(void)
{
    struct {uint64_t extensions;uint16_t engines[4];} engines={.engines={0,0,1,0}};
    gpud_i915_context_setparam_ext_t ext[3]={
        {.param={.parameter=9,.value=23}},
        {.param={.parameter=10,.size=16,.value=(uintptr_t)&engines}},
        {.param={.parameter=8,.value=0}}};
    ext[0].base.next=(uintptr_t)&ext[1];ext[1].base.next=(uintptr_t)&ext[2];
    expected_first=1;expected_count=3;expected_engines=2;
    expected_params[0]=(gpud_i915_context_param_wire_t){.parameter=9,.value=23};
    expected_params[1]=(gpud_i915_context_param_wire_t){.parameter=10,.size=16,.engine_count=2};
    expected_params[2]=(gpud_i915_context_param_wire_t){.parameter=8};
    memcpy(expected_engine_data,engines.engines,8);
    gpud_i915_context_create_t create={.flags=1,.extensions=(uintptr_t)&ext[0]};
    assert(!lpr_i915_context_ioctl(42,GPUD_DRM_IOCTL_I915_CONTEXT_CREATE_EXT,(uintptr_t)&create));
    assert(create.context_id==91 && create.flags==1 && create.extensions==(uintptr_t)&ext[0]);clean();
    fail_aux=1;create.context_id=0xabcdef;
    assert(lpr_i915_context_ioctl(42,GPUD_DRM_IOCTL_I915_CONTEXT_CREATE_EXT,(uintptr_t)&create)==-12);
    assert(create.context_id==0xabcdef);fail_aux=0;clean();
    const unsigned before=calls;
    ext[2].base.next=(uintptr_t)&ext[0];
    assert(lpr_i915_context_ioctl(42,GPUD_DRM_IOCTL_I915_CONTEXT_CREATE_EXT,(uintptr_t)&create)==-22);
    ext[2].base.next=0;engines.extensions=1;
    assert(lpr_i915_context_ioctl(42,GPUD_DRM_IOCTL_I915_CONTEXT_CREATE_EXT,(uintptr_t)&create)==-95);
    engines.extensions=0;ext[0].param.parameter=7;ext[0].param.value=1;
    assert(lpr_i915_context_ioctl(42,GPUD_DRM_IOCTL_I915_CONTEXT_CREATE_EXT,(uintptr_t)&create)==-95);
    assert(calls==before);clean();
    /* Unknown extensions need only a base header; do not overread a param. */
    gpud_i915_user_extension_t unknown={.name=1};create.extensions=(uintptr_t)&unknown;
    assert(lpr_i915_context_ioctl(42,GPUD_DRM_IOCTL_I915_CONTEXT_CREATE_EXT,(uintptr_t)&create)==-22);
    assert(calls==before);clean();
}
static void setparam_and_limits(void)
{
    gpud_i915_context_param_t p={.context_id=91,.parameter=6,.value=UINT64_MAX};
    expected_first=91;expected_count=1;expected_engines=0;
    expected_params[0]=(gpud_i915_context_param_wire_t){.parameter=6,.value=UINT64_MAX};
    gpud_i915_context_param_t before=p;
    assert(!lpr_i915_context_ioctl(42,GPUD_DRM_IOCTL_I915_CONTEXT_SETPARAM,(uintptr_t)&p));
    assert(!memcmp(&p,&before,sizeof(p)));clean();
    gpud_i915_context_setparam_ext_t ext[65]={0};
    for(unsigned i=0;i<65;++i) {
        ext[i].param.parameter=8;
        if(i<64) ext[i].base.next=(uintptr_t)&ext[i+1];
        if(i<64) expected_params[i]=(gpud_i915_context_param_wire_t){.parameter=8};
    }
    gpud_i915_context_create_t create={.flags=1,.extensions=(uintptr_t)ext};
    expected_first=1;expected_count=64;
    ext[63].base.next=0;
    assert(!lpr_i915_context_ioctl(42,GPUD_DRM_IOCTL_I915_CONTEXT_CREATE_EXT,(uintptr_t)&create));clean();
    ext[63].base.next=(uintptr_t)&ext[64];unsigned calls_before=calls;
    assert(lpr_i915_context_ioctl(42,GPUD_DRM_IOCTL_I915_CONTEXT_CREATE_EXT,(uintptr_t)&create)==-22);
    assert(calls==calls_before);clean();
}
int main(void)
{
    basic();graph();setparam_and_limits();
    puts("LPR i915 context: private chain/engine snapshots, legacy/ext/setparam, cycles/limits, unsupported nested pointers and cleanup PASS");
}
