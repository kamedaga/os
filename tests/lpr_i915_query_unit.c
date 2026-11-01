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
#define LPR_DRM_ERR_PROTOCOL 71
#define lpr_memcpy memcpy
#define lpr_memset memset
static unsigned char page[4096];
static void *allocation;
static unsigned creates, destroys, calls;
static int failure, corruption, mode;
static pacha_fd_t lpr_create_tty_wire_page(void **out)
{ ++creates; *out=page; memset(page,0xcc,sizeof(page)); return 20; }
static void lpr_destroy_tty_wire_page(pacha_fd_t fd,void *p)
{ assert(fd==20 && p==page); ++destroys; }
static void *lpr_gpud_drm_payload(void *p) { return p; }
static int lpr_drm_aux_create(uint64_t bytes,int reusable,pacha_fd_t *fd,void **out,uint64_t *size)
{
    assert(!reusable && bytes && bytes<=KB2_GPU_DRM_I915_MAX_QUERY_BYTES);
    if (failure==1) return -12;
    allocation=malloc(bytes); assert(allocation);
    *fd=21; *out=allocation; *size=bytes; return 0;
}
static void lpr_drm_aux_destroy(pacha_fd_t fd,void *p,uint64_t bytes)
{
    if(p) { assert(fd==21 && p==allocation && bytes); free(p); allocation=NULL; }
    else assert(fd==-1 && !bytes);
}
static int64_t lpr_gpud_drm_call_transfer(uint64_t op,pacha_fd_t fd,void *p,
    size_t size,uint64_t in,uint64_t out,pacha_fd_t aux,int temporary)
{
    assert(op==GPUD_DRM_OP_HANDLE_IOCTL && fd==20 && p==page);
    assert(size==sizeof(gpud_drm_ioctl_request_t) && !in && !out && temporary);
    gpud_drm_ioctl_request_t *r=p;
    gpud_drm_i915_query_wire_t q;
    memcpy(&q,r->data,sizeof(q));
    assert(r->handle==42 && r->request==GPUD_DRM_IOCTL_I915_QUERY);
    assert(r->arg_size==16 && r->data_size==16 && !q.flags);
    assert(r->aux_size==q.count*24+q.blob_bytes); ++calls;
    if(failure==2) return -4;
    if(!q.count) { assert(aux==-1 && !r->aux_size); return 0; }
    assert(aux==21 && q.count==2);
    gpud_drm_i915_query_item_wire_t *items=allocation;
    assert(items[0].query_id==1 && items[1].query_id==999);
    if(mode==1) {
        assert(items[0].data_offset==48 && items[0].length==17);
        assert(items[1].data_offset==72 && items[1].length==8);
        unsigned char *b=allocation;
        for(unsigned i=48;i<65;++i) assert(b[i]==0xa1);
        for(unsigned i=65;i<72;++i) assert(!b[i]);
        for(unsigned i=72;i<80;++i) assert(b[i]==0xb2);
        memset(b+48,0x5a,16);
    } else { assert(!items[0].data_offset && !items[1].data_offset); }
    items[0].length=16; items[1].length=-22;
    if(corruption==1) items[1].query_id=1;
    if(corruption==2) items[0].data_offset++;
    if(corruption==3) items[0].length=18;
    if(corruption==4) items[1].flags++;
    return 0;
}
#include "../userland/personality/linux/runtime/lpr_drm/i915_query.h"
static void wait_copyout(void)
{
    const int errors[] = {0, -62, -4, -11, -12, -2};
    for(unsigned e=0;e<sizeof(errors)/sizeof(errors[0]);++e) {
        unsigned char user[16]; memset(user,0xa5,sizeof(user));
        gpud_drm_ioctl_request_t r={.data_size=24};
        const int32_t result=errors[e]; const int64_t remaining=123456;
        memcpy(r.data+8,&remaining,8); memcpy(r.data+16,&result,4);
        assert(lpr_i915_wait_copyout(&r,result,(uintptr_t)user)==result);
        int64_t actual; memcpy(&actual,user+8,8); assert(actual==remaining);
        for(unsigned i=0;i<8;++i) assert(user[i]==0xa5);
        memset(user,0xa5,sizeof(user)); r.data[20]=1;
        assert(lpr_i915_wait_copyout(&r,result,(uintptr_t)user)==-71);
        for(unsigned i=0;i<16;++i) assert(user[i]==0xa5);
        r.data[20]=0;
        assert(lpr_i915_wait_copyout(&r,result-1,(uintptr_t)user)==-71);
        r.data_size=16;
        assert(lpr_i915_wait_copyout(&r,-5,(uintptr_t)user)==-5);
        assert(lpr_i915_wait_copyout(&r,0,(uintptr_t)user)==-71);
    }
}
int main(void)
{
    wait_copyout();
    unsigned char a[17],b[8];
    for(int trial=0;trial<10;++trial) {
        memset(a,0xa1,sizeof(a)); memset(b,0xb2,sizeof(b));
        mode=trial!=0 && trial!=9;
        corruption=trial>=2 && trial<=5 ? trial-1 : 0;
        failure=trial==6?1:trial==7?2:0;
        gpud_drm_i915_query_item_t items[2]={
            {.query_id=1,.length=mode?17:0,.data_pointer=mode?(uintptr_t)a:0},
            {.query_id=999,.length=mode?8:0,.data_pointer=mode?(uintptr_t)b:0}};
        if(trial==9) { items[0].length=17; items[1].length=-9; }
        gpud_drm_i915_query_t q={.count=2,.items_pointer=(uintptr_t)items};
        int64_t result=lpr_i915_query_ioctl(42,(uintptr_t)&q);
        assert(!allocation && creates==destroys);
        if(corruption || failure) {
            assert(result==(corruption?-71:failure==1?-12:-4));
            assert(items[0].length==17 && items[1].length==8);
            for(unsigned i=0;i<sizeof(a);++i) assert(a[i]==0xa1);
        } else {
            assert(!result && items[0].length==16 && items[1].length==-22);
            if(mode) { for(unsigned i=0;i<16;++i) assert(a[i]==0x5a); assert(a[16]==0xa1); }
        }
        for(unsigned i=0;i<sizeof(b);++i) assert(b[i]==0xb2);
        assert(items[0].data_pointer==(mode?(uintptr_t)a:0));
    }
    mode=0; failure=corruption=0;
    gpud_drm_i915_query_t q={0};
    assert(!lpr_i915_query_ioctl(42,(uintptr_t)&q));
    unsigned before=calls;
    q.count=1; assert(lpr_i915_query_ioctl(42,(uintptr_t)&q)==-14);
    q.count=65; assert(lpr_i915_query_ioctl(42,(uintptr_t)&q)==-22);
    q.count=0; q.flags=1; assert(lpr_i915_query_ioctl(42,(uintptr_t)&q)==-22);
    gpud_drm_i915_query_item_t item={.query_id=1,.length=INT32_MAX,.data_pointer=(uintptr_t)a};
    q=(gpud_drm_i915_query_t){.count=1,.items_pointer=(uintptr_t)&item};
    assert(lpr_i915_query_ioctl(42,(uintptr_t)&q)==-22);
    assert(calls==before && creates==destroys);
    puts("LPR i915 QUERY: private offsets, probe/fill, per-item errno, null buffers, corrupt replies and cleanup; WAIT timeout/error copyout PASS");
}
