/* SPDX-License-Identifier: GPL-2.0-only */
/* The actual context callback, owner checks and ioctl wrapper are extracted.
 * Only Linux allocation/fd/task/syscall boundaries are modeled. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <drm/i915_drm.h>
#include <kobox2/i915_context_shape.h>
#include "../kobox2/linux-sandbox/kobox/boot/drm_file.h"
typedef uint64_t u64;
typedef uint32_t u32;
#undef static_assert
#define static_assert(c) _Static_assert(c,#c)
#define GFP_KERNEL 0
#define PF_KTHREAD 1u
#define CREATE KB2_GPU_DRM_I915_COMMAND_CONTEXT_CREATE
#define SET KB2_GPU_DRM_I915_COMMAND_CONTEXT_SETPARAM
#define DESTROY KB2_GPU_DRM_I915_COMMAND_CONTEXT_DESTROY
#define AUX_MAX (64u*32u+4096u*4u)
#define SENTINEL UINT32_C(0xdecafbad)
struct file { int tag; };
struct files_struct { int tag; };
struct task_struct { void *mm; unsigned flags,pid; struct files_struct *files; };
struct pt_regs { unsigned long di,si,dx; };
struct kobox_linux_drm_file { struct file *guard; struct task_struct *owner;
    struct files_struct *files; int fd; };
static struct file installed, other_guard;
static struct files_struct files, other_files;
static struct task_struct task={.pid=1,.files=&files}, stranger;
static struct task_struct *current=&task;
static struct kobox_linux_drm_file owner={.guard=&installed,.owner=&task,.files=&files,.fd=71};
static bool irq_context,irq_disabled,missing_fd,mutate_peer;
static unsigned calls,gets,puts_count,task_work_calls,allocation_attempts,allocations,frees,fail_at;
static int ioctl_error;
static struct allocation { void *p;size_t bytes;bool freed,zero; } allocated[2];
static unsigned char header_storage[18],aux_storage[AUX_MAX+2];
static unsigned char golden_header[16],golden_aux[AUX_MAX];
static unsigned char *header=header_storage+1,*aux=aux_storage+1;
static size_t bytes,aux_bytes;
static u32 command,output;
static int in_interrupt(void) { return irq_context; }
static int irqs_disabled(void) { return irq_disabled; }
static struct file *fget(int fd) { assert(fd==71);++gets;return missing_fd?NULL:&installed; }
static void fput(struct file *f) { assert(f==&installed);++puts_count; }
static void task_work_run(void) { ++task_work_calls;assert(task_work_calls==calls); }
static void *allocate(size_t size,unsigned flags,bool zero) {
    assert(flags==GFP_KERNEL && size && allocations<2);++allocation_attempts;
    if(allocation_attempts==fail_at)return NULL;
    void *p=zero?calloc(1,size):malloc(size);assert(p);
    allocated[allocations++]=(struct allocation){p,size,false,zero};return p;
}
static void *kvmalloc(size_t size,unsigned flags) { return allocate(size,flags,false); }
static void *kvzalloc(size_t size,unsigned flags) { return allocate(size,flags,true); }
static void kvfree(void *p) {
    if(!p)return;
    for(unsigned i=0;i<allocations;++i)if(allocated[i].p==p) {
        assert(!allocated[i].freed);allocated[i].freed=true;++frees;free(p);return;
    }
    assert(0);
}
static u32 get_unaligned_le32(const void *p) { return kb2_i915_context_u32(p); }
static u64 get_unaligned_le64(const void *p) { return kb2_i915_context_u64(p); }
static void put32(void *p,u32 value) {
    unsigned char *q=p;for(unsigned i=0;i<4;++i)q[i]=(unsigned char)(value>>(i*8));
}
static void put64(void *p,u64 value) { put32(p,(u32)value);put32((unsigned char *)p+4,(u32)(value>>32)); }
static bool frame_contains(const void *p,size_t size) {
    uintptr_t address=(uintptr_t)p;
    for(unsigned i=0;i<allocations;++i) {
        const struct allocation *a=&allocated[i];uintptr_t start=(uintptr_t)a->p;
        if(a->zero && !a->freed && address>=start && address-start<=a->bytes &&
            size<=a->bytes-(address-start))return true;
    }
    return false;
}
static void validate_param(const struct drm_i915_gem_context_param *p,unsigned index,u32 context_id) {
    assert(frame_contains(p,sizeof(*p)));
    const unsigned char *wire=golden_aux+index*32;
    assert(p->ctx_id==context_id && p->param==get_unaligned_le64(wire));
    assert(p->size==get_unaligned_le32(wire+16));
    if(p->param==I915_CONTEXT_PARAM_ENGINES && p->size) {
        const struct i915_context_param_engines *engines=(void *)(uintptr_t)p->value;
        assert(frame_contains(engines,p->size));
        assert(!engines->extensions);
        u32 first=get_unaligned_le32(wire+20),count=get_unaligned_le32(wire+24);
        u32 params=get_unaligned_le32(golden_header+4);
        assert(p->size==8+count*4);
        assert(!memcmp(engines->engines,golden_aux+params*32+first*4,count*4));
    } else {
        assert(p->value==get_unaligned_le64(wire+8));
        if(p->param==I915_CONTEXT_PARAM_ENGINES)assert(!p->value && !p->size);
    }
}
static long __x64_sys_ioctl(struct pt_regs *regs) {
    assert(regs && regs->di==71);++calls;assert(task_work_calls+1==calls);
    void *p=(void *)(uintptr_t)regs->dx;
    assert(p && p!=header && p!=aux && p!=&output);
    /* Destroy the peer's entire request before traversing the private graph.
     * Every subsequent comparison uses the pre-call immutable golden bytes. */
    if(mutate_peer) { memset(header,0xcc,bytes);memset(aux,0xdd,aux_bytes); }
    if(command==DESTROY) {
        assert(regs->si==DRM_IOCTL_I915_GEM_CONTEXT_DESTROY);
        struct drm_i915_gem_context_destroy *a=p;
        assert(a->ctx_id==get_unaligned_le32(golden_header) && !a->pad);
        a->ctx_id=0;
    } else if(command==SET) {
        assert(regs->si==DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM);
        validate_param(p,0,get_unaligned_le32(golden_header));
        ((struct drm_i915_gem_context_param *)p)->value=0;
    } else {
        assert(command==CREATE && regs->si==DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT);
        struct drm_i915_gem_context_create_ext *a=p;
        assert(!a->ctx_id && a->flags==get_unaligned_le32(golden_header));
        u32 count=get_unaligned_le32(golden_header+4);
        uintptr_t next=(uintptr_t)a->extensions;
        const void *seen[64];
        for(u32 i=0;i<count;++i) {
            struct drm_i915_gem_context_create_ext_setparam *ext=(void *)next;
            assert(frame_contains(ext,sizeof(*ext)));
            for(u32 j=0;j<i;++j)assert(seen[j]!=ext);
            seen[i]=ext;
            assert(ext->base.name==I915_CONTEXT_CREATE_EXT_SETPARAM && !ext->base.flags);
            for(unsigned j=0;j<4;++j)assert(!ext->base.rsvd[j]);
            validate_param(&ext->param,i,0);next=(uintptr_t)ext->base.next_extension;
        }
        assert(!next);a->ctx_id=UINT32_MAX;
    }
    return ioctl_error;
}
#include "drm_i915_context_actual.inc"
static void reset(u32 cmd,u32 count,u32 engines) {
    assert(allocations==frees);
    calls=gets=puts_count=task_work_calls=allocation_attempts=allocations=frees=fail_at=0;
    memset(allocated,0,sizeof(allocated));ioctl_error=0;mutate_peer=false;
    irq_context=irq_disabled=missing_fd=false;current=&task;
    task.mm=NULL;task.flags=0;task.pid=1;task.files=&files;
    owner.guard=&installed;owner.owner=&task;owner.files=&files;
    memset(header_storage,0,sizeof(header_storage));memset(aux_storage,0,sizeof(aux_storage));
    command=cmd;bytes=cmd==DESTROY?8:16;aux_bytes=cmd==DESTROY?0:(size_t)count*32+engines*4;
    assert(aux_bytes<=AUX_MAX);output=SENTINEL;
    put32(header,cmd==CREATE?(count?1:0):UINT32_C(0xfedcba98));
    if(cmd!=DESTROY) { put32(header+4,count);put32(header+8,engines); }
}
static void parameter(unsigned index,u64 id,u64 value,u32 size,u32 first,u32 count) {
    unsigned char *p=aux+index*32;put64(p,id);put64(p+8,value);
    put32(p+16,size);put32(p+20,first);put32(p+24,count);
}
static void engine_values(u32 params,u32 count) {
    for(u32 i=0;i<count;++i) {
        /* Preserve both u16 fields, including INVALID class/instance values
         * for the real driver to validate, without pointer reinterpretation. */
        put32(aux+params*32+i*4,UINT32_C(0xffff0000)+i);
    }
}
static int invoke(struct kobox_linux_drm_file *file,const void *in,size_t n,
    const void *data,size_t length,u32 *id) {
    memcpy(golden_header,header,bytes);memcpy(golden_aux,aux,aux_bytes);
    int result=kobox_linux_drm_i915_context(file,command,in,n,data,length,id);
    assert(allocations==frees);return result;
}
static void success(bool mutation) {
    mutate_peer=mutation;
    assert(!invoke(&owner,header,bytes,aux_bytes?aux:NULL,aux_bytes,&output));
    assert(output==(command==CREATE?UINT32_MAX:SENTINEL));
    assert(calls==1 && task_work_calls==1 && gets==1 && puts_count==1);
    if(!mutation) { assert(!memcmp(header,golden_header,bytes));assert(!memcmp(aux,golden_aux,aux_bytes)); }
}
static void graph_cases(void) {
    reset(CREATE,0,0);success(true);assert(!allocation_attempts);
    reset(CREATE,0,0);put32(header,3);success(false);
    reset(DESTROY,0,0);success(true);assert(!allocation_attempts);
    const u64 scalar_ids[]={4,5,6,8,9,11,12,13,14};
    for(unsigned i=0;i<sizeof(scalar_ids)/sizeof(scalar_ids[0]);++i) {
        reset(SET,1,0);parameter(0,scalar_ids[i],UINT64_C(0xfedcba9876543210)+i,0,0,0);
        success(true);assert(allocation_attempts==2);
    }
    reset(SET,1,0);parameter(0,10,0,0,0,0);success(true); /* ENGINES reset */
    reset(SET,1,0);parameter(0,10,0,8,0,0);success(false); /* non-NULL empty array */
    reset(CREATE,64,4096);
    for(unsigned i=0;i<64;++i)parameter(i,10,0,264,i*64,64);
    engine_values(64,4096);success(true);assert(allocation_attempts==2);
    reset(CREATE,64,128);
    unsigned next=0;
    for(unsigned i=0;i<64;++i) {
        if(i%2) { parameter(i,10,0,24,next,4);next+=4; }
        else parameter(i,scalar_ids[(i/2)%9],UINT64_C(0x123456789abcdef0)+i,0,0,0);
    }
    engine_values(64,128);success(true);
    reset(SET,1,64);parameter(0,10,0,264,0,64);engine_values(1,64);success(true);
}
static void refusal(const void *in,size_t n,const void *data,size_t length) {
    assert(invoke(&owner,in,n,data,length,&output)==-EINVAL);
    assert(output==SENTINEL && !calls && !task_work_calls);
}
static void invalid_cases(void) {
    reset(CREATE,0,0);refusal(NULL,16,NULL,0);refusal(header,17,NULL,0);
    refusal(header,15,NULL,0);refusal(header,0,NULL,0);
    assert(invoke(&owner,header,16,NULL,0,NULL)==-EINVAL);
    reset(CREATE,0,0);put32(header,4);refusal(header,16,NULL,0);
    reset(CREATE,0,0);put32(header+12,1);refusal(header,16,NULL,0);
    reset(CREATE,0,0);put32(header+4,65);refusal(header,16,NULL,0);
    reset(CREATE,0,0);put32(header+8,4097);refusal(header,16,NULL,0);
    reset(CREATE,0,0);put32(header+8,1);refusal(header,16,NULL,0);
    reset(CREATE,1,0);put32(header,0);refusal(header,16,aux,32);
    reset(SET,0,0);refusal(header,16,NULL,0);
    reset(SET,2,0);refusal(header,16,aux,64);
    reset(DESTROY,0,0);put32(header+4,1);refusal(header,8,NULL,0);
    reset(DESTROY,0,0);refusal(header,16,NULL,0);refusal(header,8,aux,1);
    reset(SET,1,0);parameter(0,4,1,0,0,0);
    refusal(header,16,NULL,32);refusal(header,16,aux,31);refusal(header,16,aux,33);
    refusal(header,16,aux,SIZE_MAX);
    const u64 forbidden[]={0,1,2,3,7,15,UINT64_MAX,UINT64_C(0x100000004)};
    for(unsigned i=0;i<sizeof(forbidden)/sizeof(forbidden[0]);++i) {
        reset(SET,1,0);parameter(0,forbidden[i],0,0,0,0);refusal(header,16,aux,32);
    }
    for(unsigned field=16;field<=28;field+=4) {
        reset(SET,1,0);parameter(0,4,1,0,0,0);put32(aux+field,1);refusal(header,16,aux,32);
    }
    for(unsigned field=8;field<=28;field+=4) {
        reset(SET,1,0);parameter(0,10,0,0,0,0);put32(aux+field,1);refusal(header,16,aux,32);
    }
    reset(SET,1,64);parameter(0,10,0,268,0,65);refusal(header,16,aux,aux_bytes);
    reset(SET,1,1);parameter(0,10,0,12,1,1);refusal(header,16,aux,aux_bytes);
    reset(SET,1,1);parameter(0,10,0,8,0,0);refusal(header,16,aux,aux_bytes);
    reset(SET,1,1);parameter(0,10,0,16,0,1);refusal(header,16,aux,aux_bytes);
    reset(CREATE,2,2);parameter(0,10,0,12,0,1);parameter(1,10,0,12,0,1);
    refusal(header,16,aux,aux_bytes); /* overlapping lists */
    reset(SET,1,1);parameter(0,10,0,12,UINT32_MAX,1);refusal(header,16,aux,aux_bytes);
    reset(CREATE,0,0);command=UINT32_MAX;refusal(header,16,NULL,0);
}
static void failures(void) {
    const int errors[]={-EIO,-EINTR,-EAGAIN,-EINVAL,-ENOMEM};
    for(u32 cmd=CREATE;cmd<=DESTROY;++cmd)for(unsigned e=0;e<5;++e) {
        reset(cmd,cmd==DESTROY?0:1,0);
        if(cmd!=DESTROY)parameter(0,4,UINT64_MAX,0,0,0);
        ioctl_error=errors[e];mutate_peer=true;
        assert(invoke(&owner,header,bytes,aux_bytes?aux:NULL,aux_bytes,&output)==errors[e]);
        assert(output==SENTINEL && calls==1 && task_work_calls==1);
    }
    for(u32 cmd=CREATE;cmd<=SET;++cmd)for(unsigned site=1;site<=2;++site) {
        reset(cmd,1,0);parameter(0,4,1,0,0,0);fail_at=site;
        assert(invoke(&owner,header,16,aux,32,&output)==-ENOMEM);
        assert(output==SENTINEL && !calls && !task_work_calls && allocation_attempts==site);
        assert(allocations==site-1 && frees==allocations);
    }
}
static void authority(void) {
    for(u32 cmd=CREATE;cmd<=DESTROY;++cmd)for(unsigned which=0;which<12;++which) {
        reset(cmd,cmd==DESTROY?0:1,0);if(cmd!=DESTROY)parameter(0,4,1,0,0,0);
        struct kobox_linux_drm_file *file=&owner;int error=-EPERM;
        switch(which) {
        case 0:file=NULL;break;case 1:current=&stranger;break;case 2:task.mm=&task;break;
        case 3:irq_context=true;break;case 4:irq_disabled=true;break;case 5:task.pid=42;break;
        case 6:task.files=NULL;break;case 7:task.files=&other_files;break;
        case 8:owner.owner=&stranger;break;case 9:owner.files=&other_files;break;
        case 10:missing_fd=true;error=-EBADF;break;case 11:owner.guard=&other_guard;error=-EBADF;break;
        }
        assert(invoke(file,header,bytes,aux_bytes?aux:NULL,aux_bytes,&output)==error);
        assert(output==SENTINEL && !calls && !task_work_calls && !allocation_attempts);
        assert(gets==(which>=10) && puts_count==(which==11));
    }
}
int main(void) {
    graph_cases();invalid_cases();failures();authority();
    puts("I915 context actual callback/private heap graph/64 parameters/4096 engines/reset/scalars: PASS");
    puts("I915 context owner/two allocation failures/input mutation/output preservation/limits: PASS");
    return 0;
}
