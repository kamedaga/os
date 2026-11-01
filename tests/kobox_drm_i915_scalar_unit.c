/* SPDX-License-Identifier: GPL-2.0-only */
/* Extract real backend callbacks, owner checks and private ioctl wrapper.
 * Model only the Linux fd/task/syscall boundary, using actual DRM UAPI. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <drm/i915_drm.h>
#include "../kobox2/linux-sandbox/kobox/boot/drm_file.h"
typedef uint64_t u64;
typedef uint32_t u32;
#define PF_KTHREAD 1u
struct file { unsigned tag; };
struct files_struct { unsigned tag; };
struct task_struct { void *mm; unsigned flags, pid; struct files_struct *files; };
struct pt_regs { unsigned long di, si, dx; };
struct kobox_linux_drm_file { struct file *guard; struct task_struct *owner;
    struct files_struct *files; int fd; };
static struct file installed, other_guard;
static struct files_struct files, other_files;
static struct task_struct task = {.pid=1, .files=&files}, stranger;
static struct task_struct *current = &task;
static struct kobox_linux_drm_file owner = {
    .guard=&installed, .owner=&task, .files=&files, .fd=71};
static bool irq_context, irq_disabled, missing_fd;
static unsigned calls, gets, puts_count, task_work_calls;
static int syscall_error;
static u32 expected_context;
static u64 expected_parameter;
static void *caller_output1, *caller_output2;
static const u64 total_result = UINT64_C(0xfedcba9876543210);
static const u64 available_result = UINT64_C(0x123456789abcdef0);
static const u64 value_result = UINT64_C(0xabcdeffedcba1234);
static const u64 sentinel = UINT64_C(0xfeedfacebeefcafe);
static int in_interrupt(void) { return irq_context; }
static int irqs_disabled(void) { return irq_disabled; }
static struct file *fget(int fd) {
    assert(fd==71); ++gets; return missing_fd ? NULL : &installed;
}
static void fput(struct file *file) { assert(file==&installed); ++puts_count; }
static void task_work_run(void) { ++task_work_calls; assert(task_work_calls==calls); }
static long __x64_sys_ioctl(struct pt_regs *regs) {
    assert(regs && regs->di==71);
    void *argument=(void *)(uintptr_t)regs->dx;
    assert(argument && argument!=caller_output1 && argument!=caller_output2);
    ++calls;
    assert(task_work_calls+1==calls);
    if (regs->si==DRM_IOCTL_I915_GEM_GET_APERTURE) {
        struct drm_i915_gem_get_aperture *a=argument;
        assert(!a->aper_size && !a->aper_available_size);
        a->aper_size=total_result; a->aper_available_size=available_result;
    } else {
        assert(regs->si==DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM);
        struct drm_i915_gem_context_param *a=argument;
        assert(a->ctx_id==expected_context && a->param==expected_parameter);
        assert(!a->size && !a->value);
        assert(a->param==I915_CONTEXT_PARAM_GTT_SIZE || a->param==I915_CONTEXT_PARAM_VM);
        a->value=value_result;
    }
    /* Mutate even on error, so output preservation tests detect premature
     * copyout. The wrapper must also drain task work on these errors. */
    return syscall_error;
}
#include "drm_i915_scalar_actual.inc"
static void reset(void) {
    calls=gets=puts_count=task_work_calls=0; syscall_error=0;
    irq_context=irq_disabled=missing_fd=false;
    current=&task; task.mm=NULL; task.flags=0; task.pid=1; task.files=&files;
    owner.guard=&installed; owner.owner=&task; owner.files=&files;
    expected_context=0; expected_parameter=I915_CONTEXT_PARAM_GTT_SIZE;
    caller_output1=caller_output2=NULL;
}
static int aperture(struct kobox_linux_drm_file *file,u64 *total,u64 *available) {
    caller_output1=total; caller_output2=available;
    return kobox_linux_drm_i915_get_aperture(file,total,available);
}
static int context(struct kobox_linux_drm_file *file,u64 parameter,u64 *value) {
    expected_parameter=parameter; caller_output1=value; caller_output2=NULL;
    return kobox_linux_drm_i915_context_getparam(file,expected_context,parameter,value);
}
static void success(void) {
    u64 a=sentinel,b=sentinel;
    reset(); assert(!aperture(&owner,&a,&b));
    assert(a==total_result && b==available_result);
    assert(calls==1 && gets==1 && puts_count==1 && task_work_calls==1);
    const u64 parameters[]={I915_CONTEXT_PARAM_GTT_SIZE,I915_CONTEXT_PARAM_VM};
    const u32 contexts[]={0,1,UINT32_MAX};
    for(unsigned p=0;p<2;++p) for(unsigned c=0;c<3;++c) {
        reset(); expected_context=contexts[c]; a=sentinel;
        assert(!context(&owner,parameters[p],&a) && a==value_result);
        assert(calls==1 && gets==1 && puts_count==1 && task_work_calls==1);
    }
    reset(); task.pid=42; task.flags=PF_KTHREAD;
    assert(!context(&owner,I915_CONTEXT_PARAM_VM,&a));
}
static void invalid_arguments(void) {
    u64 a=sentinel,b=sentinel;
    reset(); assert(aperture(&owner,NULL,&b)==-EINVAL);
    assert(aperture(&owner,&a,NULL)==-EINVAL);
    assert(context(&owner,I915_CONTEXT_PARAM_VM,NULL)==-EINVAL);
    const u64 refused[]={0,1,2,4,5,6,7,8,10,11,UINT64_MAX,
                         UINT64_C(0x100000003),UINT64_C(0x100000009)};
    for(unsigned i=0;i<sizeof(refused)/sizeof(refused[0]);++i)
        assert(context(&owner,refused[i],&a)==-EOPNOTSUPP);
    assert(a==sentinel && b==sentinel);
    assert(!calls && !gets && !puts_count && !task_work_calls);
}
static void errors(void) {
    const int errors[]={-EIO,-EINTR,-EAGAIN,-ENOENT,-EINVAL,-ETIME,-EFAULT};
    for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);++i) {
        u64 a=sentinel,b=sentinel;
        reset(); syscall_error=errors[i];
        assert(aperture(&owner,&a,&b)==errors[i]);
        assert(a==sentinel && b==sentinel && calls==1 && task_work_calls==1);
        assert(gets==1 && puts_count==1);
        for(unsigned p=0;p<2;++p) {
            reset(); syscall_error=errors[i];
            assert(context(&owner,p?I915_CONTEXT_PARAM_VM:I915_CONTEXT_PARAM_GTT_SIZE,&a)==errors[i]);
            assert(a==sentinel && calls==1 && task_work_calls==1);
            assert(gets==1 && puts_count==1);
        }
    }
}
static void denied_owner(void) {
    for(unsigned which=0;which<12;++which) for(unsigned callback=0;callback<2;++callback) {
        reset(); struct kobox_linux_drm_file *file=&owner;
        int expected=-EPERM;
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
        case 10:missing_fd=true;expected=-EBADF;break;
        case 11:owner.guard=&other_guard;expected=-EBADF;break;
        }
        u64 a=sentinel,b=sentinel;
        int result=callback?context(file,I915_CONTEXT_PARAM_VM,&a):aperture(file,&a,&b);
        assert(result==expected && a==sentinel && b==sentinel);
        assert(!calls && !task_work_calls);
        assert(gets==(which>=10) && puts_count==(which==11));
    }
}
int main(void) {
    success(); invalid_arguments(); errors(); denied_owner();
    puts("I915 scalar actual callbacks/private_ioctl/owner/output-preservation: PASS");
    puts("I915 scalar pointer parameters 7/10 and unknown parameters: no-dispatch PASS");
    return 0;
}
