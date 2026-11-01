/* SPDX-License-Identifier: GPL-2.0-only */
/* Compile actual BO admission, owner checks and private_ioctl; model only
 * Linux task/fd/syscall boundaries. The native records are real UAPI types. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <drm/i915_drm.h>
#include <kobox2/i915_bo_shape.h>
#include "../kobox2/linux-sandbox/kobox/boot/drm_file.h"
typedef uint32_t u32;
#define static_assert _Static_assert
#define PF_KTHREAD 1u
#define ERESTARTSYS 512
struct file { int tag; };
struct files_struct { int tag; };
struct task_struct { void *mm; unsigned flags, pid; struct files_struct *files; };
struct pt_regs { unsigned long di, si, dx; };
struct kobox_linux_drm_file { struct file *guard; struct task_struct *owner;
    struct files_struct *files; int fd; };
static struct file installed, other_guard;
static struct files_struct files, other_files;
static struct task_struct task={.pid=1,.files=&files}, stranger;
static struct task_struct *current=&task;
static struct kobox_linux_drm_file owner={.guard=&installed,.owner=&task,.files=&files,.fd=71};
static bool irq_context, irq_disabled, missing_fd;
static unsigned calls, gets, puts_count, task_work_calls;
static int syscall_error;
static struct kb2_i915_bo_shape active;
static unsigned char expected[16], returned[16];
static const void *caller_input;
static void *caller_output;
static int in_interrupt(void) { return irq_context; }
static int irqs_disabled(void) { return irq_disabled; }
static struct file *fget(int fd) { assert(fd==71);++gets;return missing_fd?NULL:&installed; }
static void fput(struct file *file) { assert(file==&installed);++puts_count; }
static void task_work_run(void) { ++task_work_calls;assert(task_work_calls==calls); }
static long __x64_sys_ioctl(struct pt_regs *regs) {
    assert(regs && regs->di==71 && regs->si==active.ioctl);
    void *p=(void *)(uintptr_t)regs->dx;
    assert(p && p!=caller_input && p!=caller_output);
    assert(!((uintptr_t)p%8) && !memcmp(p,expected,active.bytes));
    ++calls;assert(task_work_calls+1==calls);
    switch(active.command) {
    case KB2_GPU_DRM_I915_COMMAND_GEM_CREATE: {
        struct drm_i915_gem_create *a=p;
        assert(a->size==UINT64_C(0x123456789000) && !a->handle && !a->pad);
        a->size+=4096;a->handle=UINT32_MAX;break;
    }
    case KB2_GPU_DRM_I915_COMMAND_GEM_BUSY:
        ((struct drm_i915_gem_busy *)p)->busy=0xa5a50001;break;
    case KB2_GPU_DRM_I915_COMMAND_GEM_MADVISE:
        ((struct drm_i915_gem_madvise *)p)->retained=1;break;
    case KB2_GPU_DRM_I915_COMMAND_GEM_WAIT:
        ((struct drm_i915_gem_wait *)p)->timeout_ns=INT64_C(0x123456789);break;
    case KB2_GPU_DRM_I915_COMMAND_GEM_SET_DOMAIN:
    case KB2_GPU_DRM_I915_COMMAND_GEM_SET_CACHING:
        memset(p,0xdd,active.bytes);break;
    default:assert(0);
    }
    memcpy(returned,p,active.bytes);
    /* Errors can modify arguments. WAIT retains its remaining timeout;
     * other failed operations must not copy out. */
    return syscall_error;
}
#include "drm_i915_bo_actual.inc"
static void reset(u32 command) {
    calls=gets=puts_count=task_work_calls=0;syscall_error=0;
    irq_context=irq_disabled=missing_fd=false;current=&task;
    task.mm=NULL;task.pid=1;task.flags=0;task.files=&files;
    owner.guard=&installed;owner.owner=&task;owner.files=&files;
    assert(kb2_i915_bo_shape_command(command,&active));
    memset(expected,0,sizeof(expected));memset(returned,0,sizeof(returned));
    switch(command) {
    case KB2_GPU_DRM_I915_COMMAND_GEM_CREATE: {
        struct drm_i915_gem_create a={.size=UINT64_C(0x123456789000)};
        memcpy(expected,&a,sizeof(a));break;
    }
    case KB2_GPU_DRM_I915_COMMAND_GEM_BUSY: {
        struct drm_i915_gem_busy a={.handle=0xfedcba98};memcpy(expected,&a,sizeof(a));break;
    }
    case KB2_GPU_DRM_I915_COMMAND_GEM_MADVISE: {
        struct drm_i915_gem_madvise a={.handle=0xfedcba98,.madv=I915_MADV_DONTNEED};
        memcpy(expected,&a,sizeof(a));break;
    }
    case KB2_GPU_DRM_I915_COMMAND_GEM_WAIT: {
        struct drm_i915_gem_wait a={.bo_handle=0xfedcba98,.timeout_ns=-1};
        memcpy(expected,&a,sizeof(a));break;
    }
    case KB2_GPU_DRM_I915_COMMAND_GEM_SET_DOMAIN: {
        struct drm_i915_gem_set_domain a={.handle=0xfedcba98,
            .read_domains=I915_GEM_DOMAIN_CPU,.write_domain=I915_GEM_DOMAIN_CPU};
        memcpy(expected,&a,sizeof(a));break;
    }
    case KB2_GPU_DRM_I915_COMMAND_GEM_SET_CACHING: {
        struct drm_i915_gem_caching a={.handle=0xfedcba98,.caching=I915_CACHING_CACHED};
        memcpy(expected,&a,sizeof(a));break;
    }
    default:assert(0);
    }
}
static int call(struct kobox_linux_drm_file *file,u32 command,
    const void *input,size_t bytes,void *output,size_t capacity) {
    caller_input=input;caller_output=output;
    return kobox_linux_drm_i915_bo(file,command,input,bytes,output,capacity);
}
static void successes_and_errors(void) {
    const int errors[]={0,-EIO,-EINVAL,-ENOENT,-ETIME,-EINTR,-EAGAIN,-ENOMEM,-ERESTARTSYS};
    for(u32 command=KB2_GPU_DRM_I915_COMMAND_GEM_CREATE;command<=KB2_GPU_DRM_I915_COMMAND_GEM_SET_CACHING;++command)
    for(unsigned e=0;e<sizeof(errors)/sizeof(errors[0]);++e)
    for(unsigned inplace=0;inplace<2;++inplace) {
        if(command==KB2_GPU_DRM_I915_COMMAND_GEM_CREATE_EXT)continue;
        reset(command);syscall_error=errors[e];
        /* Unaligned transport records must not become unaligned UAPI args. */
        unsigned char input[18],output[34],before[34];
        memset(input,0xa5,sizeof(input));memcpy(input+1,expected,active.bytes);
        memset(output,0x5a,sizeof(output));
        if(inplace)memcpy(output+1,expected,active.bytes);
        memcpy(before,output,sizeof(output));
        const void *request=inplace?output+1:input+1;
        int error=command==KB2_GPU_DRM_I915_COMMAND_GEM_WAIT && errors[e]==-ERESTARTSYS?-EINTR:errors[e];
        assert(call(&owner,command,request,active.bytes,output+1,
            inplace?active.result_bytes:32)==error);
        assert(calls==1 && task_work_calls==1 && gets==1 && puts_count==1);
        if(command==KB2_GPU_DRM_I915_COMMAND_GEM_WAIT || (!errors[e] && active.result_bytes))
            memcpy(before+1,returned,active.bytes);
        assert(!memcmp(before,output,sizeof(output)));
        assert(!memcmp(input+1,expected,active.bytes));
    }
    for(u32 command=KB2_GPU_DRM_I915_COMMAND_GEM_SET_DOMAIN;command<=KB2_GPU_DRM_I915_COMMAND_GEM_SET_CACHING;++command) {
        if(command==KB2_GPU_DRM_I915_COMMAND_GEM_CREATE_EXT)continue;
        reset(command);task.pid=42;task.flags=PF_KTHREAD;
        assert(!call(&owner,command,expected,active.bytes,NULL,0));
        assert(calls==1 && task_work_calls==1);
    }
}
static void reject(u32 command,const void *input,size_t bytes,void *output,
    size_t capacity,unsigned char *storage,size_t storage_size) {
    unsigned char before[64];assert(storage_size<=sizeof(before));
    memcpy(before,storage,storage_size);
    assert(call(&owner,command,input,bytes,output,capacity)==-EINVAL);
    assert(!calls && !gets && !puts_count && !task_work_calls);
    assert(!memcmp(before,storage,storage_size));
}
static void validation(void) {
    for(u32 command=KB2_GPU_DRM_I915_COMMAND_GEM_CREATE;command<=KB2_GPU_DRM_I915_COMMAND_GEM_SET_CACHING;++command) {
        if(command==KB2_GPU_DRM_I915_COMMAND_GEM_CREATE_EXT)continue;
        reset(command);unsigned char output[32];memset(output,0x5a,sizeof(output));
        reject(command,NULL,active.bytes,output,sizeof(output),output,sizeof(output));
        reject(command,expected,active.bytes-1,output,sizeof(output),output,sizeof(output));
        reject(command,expected,active.bytes+1,output,sizeof(output),output,sizeof(output));
        reject(command,expected,SIZE_MAX,output,sizeof(output),output,sizeof(output));
        if(active.result_bytes) {
            reject(command,expected,active.bytes,NULL,active.result_bytes,output,sizeof(output));
            reject(command,expected,active.bytes,output,active.result_bytes-1,output,sizeof(output));
        }
        unsigned char malformed[16];memcpy(malformed,expected,sizeof(malformed));
        unsigned offset=command==KB2_GPU_DRM_I915_COMMAND_GEM_CREATE?8:command==KB2_GPU_DRM_I915_COMMAND_GEM_MADVISE?8:command==KB2_GPU_DRM_I915_COMMAND_GEM_SET_DOMAIN?5:4;
        malformed[offset]=0xff;
        reject(command,malformed,active.bytes,output,sizeof(output),output,sizeof(output));
        if(command==KB2_GPU_DRM_I915_COMMAND_GEM_CREATE) {
            memcpy(malformed,expected,sizeof(malformed));malformed[12]=1;
            reject(command,malformed,active.bytes,output,sizeof(output),output,sizeof(output));
        }
        if(command==KB2_GPU_DRM_I915_COMMAND_GEM_MADVISE) {
            memcpy(malformed,expected,sizeof(malformed));malformed[4]=2;
            reject(command,malformed,active.bytes,output,sizeof(output),output,sizeof(output));
        }
        if(command==KB2_GPU_DRM_I915_COMMAND_GEM_SET_DOMAIN) {
            /* 0xff is within the domain mask; use the first unsupported bit. */
            memcpy(malformed,expected,sizeof(malformed));malformed[5]=1;
            reject(command,malformed,active.bytes,output,sizeof(output),output,sizeof(output));
            memcpy(malformed,expected,sizeof(malformed));malformed[9]=1;
            reject(command,malformed,active.bytes,output,sizeof(output),output,sizeof(output));
        }
    }
    const u32 unknown[]={0,1,4,11,UINT32_MAX,DRM_IOCTL_I915_GEM_CREATE};
    for(unsigned i=0;i<sizeof(unknown)/sizeof(unknown[0]);++i) {
        reset(5);unsigned char output[32]={0};
        reject(unknown[i],expected,16,output,sizeof(output),output,sizeof(output));
    }
}
static void authority(void) {
    for(u32 command=KB2_GPU_DRM_I915_COMMAND_GEM_CREATE;command<=KB2_GPU_DRM_I915_COMMAND_GEM_SET_CACHING;++command) for(unsigned which=0;which<12;++which) {
        if(command==KB2_GPU_DRM_I915_COMMAND_GEM_CREATE_EXT)continue;
        reset(command);struct kobox_linux_drm_file *file=&owner;int error=-EPERM;
        switch(which) {
        case 0:file=NULL;break;
        case 1:current=&stranger;break;
        case 2:task.mm=&task;break;
        case 3:irq_context=true;break;
        case 4:irq_disabled=true;break;
        case 5:task.pid=42;break;
        case 6:task.files=NULL;break;
        case 7:task.files=&other_files;break;
        case 8:owner.owner=&stranger;break;
        case 9:owner.files=&other_files;break;
        case 10:missing_fd=true;error=-EBADF;break;
        case 11:owner.guard=&other_guard;error=-EBADF;break;
        }
        unsigned char output[32],before[32];memset(output,0x5a,sizeof(output));memcpy(before,output,sizeof(output));
        assert(call(file,command,expected,active.bytes,output,sizeof(output))==error);
        assert(!memcmp(before,output,sizeof(output)) && !calls && !task_work_calls);
        assert(gets==(which>=10) && puts_count==(which==11));
    }
}
int main(void) {
    successes_and_errors();validation();authority();
    puts("I915 BO six actual UAPI/private_ioctl/owner/errno/copyout: PASS");
    puts("I915 BO normalization/bounds/unaligned/in-place/no-result/canaries: PASS");
    return 0;
}
