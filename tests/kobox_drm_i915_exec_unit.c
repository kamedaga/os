/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <drm/i915_drm.h>
#include <kobox2/i915_exec_shape.h>
#include "../kobox2/linux-sandbox/kobox/boot/drm_file.h"
typedef uint32_t u32;typedef uint64_t u64;
#define GFP_KERNEL 0
#define PF_KTHREAD 1
#define ERESTARTSYS 512
#undef static_assert
#define static_assert(c) _Static_assert(c,#c)
struct file {int tag;};struct files_struct {int tag;};
struct task_struct {void *mm;unsigned flags,pid;struct files_struct *files;};
struct pt_regs {unsigned long di,si,dx;};
struct kobox_linux_drm_file {struct file *guard;struct task_struct *owner;struct files_struct *files;int fd;};
static struct file installed;static struct files_struct files;
static struct task_struct task={.pid=1,.files=&files},stranger;
static struct task_struct *current=&task;
static struct kobox_linux_drm_file owner={.guard=&installed,.owner=&task,.files=&files,.fd=71};
static bool irq_context,irq_disabled,missing_fd,mutate_peer;
static unsigned calls,gets,puts_count,work_calls,attempts,allocations,frees,fail_at;
static int ioctl_error;
struct allocation {void *p;size_t bytes;bool freed;};
static struct allocation memory[3];
static unsigned char header_storage[34],golden_header[32];
static unsigned char *header=header_storage+1,*aux,*storage,*golden_aux;
static size_t aux_bytes;static u32 count,fence_count;
static int in_interrupt(void) {return irq_context;}
static int irqs_disabled(void) {return irq_disabled;}
static struct file *fget(int fd) {assert(fd==71);++gets;return missing_fd?NULL:&installed;}
static void fput(struct file *p) {assert(p==&installed);++puts_count;}
static void task_work_run(void) {++work_calls;assert(work_calls==calls);}
static void *allocate(size_t bytes,bool zero)
{
    assert(bytes && bytes<=1048576 && allocations<3);++attempts;
    if(attempts==fail_at) return NULL;
    void *p=zero?calloc(1,bytes):malloc(bytes);assert(p);
    memory[allocations++]=(struct allocation){p,bytes,false};return p;
}
static void *kvmalloc(size_t bytes,unsigned flags) {assert(flags==GFP_KERNEL);return allocate(bytes,false);}
static void *kvcalloc(size_t n,size_t bytes,unsigned flags) {assert(flags==GFP_KERNEL && n<=65536 && bytes<=56);return allocate(n*bytes,true);}
static void kvfree(void *p)
{
    if(!p) return;
    for(unsigned i=0;i<allocations;++i) if(memory[i].p==p) {assert(!memory[i].freed);memory[i].freed=true;++frees;free(p);return;}
    assert(0);
}
static bool contains(const void *p,size_t bytes)
{
    const uintptr_t a=(uintptr_t)p;
    for(unsigned i=0;i<allocations;++i) {
        const uintptr_t begin=(uintptr_t)memory[i].p;
        if(!memory[i].freed && a>=begin && a-begin<=memory[i].bytes && bytes<=memory[i].bytes-(a-begin)) return true;
    }
    return false;
}
static u32 get_unaligned_le32(const void *p) {return kb2_i915_context_u32(p);}
static u64 get_unaligned_le64(const void *p) {return kb2_i915_context_u64(p);}
static void put_unaligned_le64(u64 value,void *p) {unsigned char *q=p;for(unsigned i=0;i<8;++i)q[i]=(unsigned char)(value>>(i*8));}
static void put32(void *p,u32 value) {unsigned char *q=p;for(unsigned i=0;i<4;++i)q[i]=(unsigned char)(value>>(i*8));}
static long __x64_sys_ioctl(struct pt_regs *regs)
{
    assert(regs->di==71 && regs->si==DRM_IOCTL_I915_GEM_EXECBUFFER2);++calls;
    struct drm_i915_gem_execbuffer2 *a=(void *)(uintptr_t)regs->dx;
    assert(a && (void *)a!=header && a->buffer_count==count && a->num_cliprects==fence_count);
    assert(a->batch_start_offset==0 && a->batch_len==256 && !a->DR1 && !a->DR4);
    assert(a->flags==get_unaligned_le64(golden_header+16) && a->rsvd1==91 && !a->rsvd2);
    struct drm_i915_gem_exec_object2 *objects=(void *)(uintptr_t)a->buffers_ptr;
    assert(contains(objects,(size_t)count*sizeof(*objects)) && !((uintptr_t)objects%8));
    struct drm_i915_gem_exec_fence *fences=(void *)(uintptr_t)a->cliprects_ptr;
    if(fence_count) assert(contains(fences,(size_t)fence_count*sizeof(*fences)) && !((uintptr_t)fences%8));
    else assert(!fences);
    if(mutate_peer) {memset(header,0xcc,32);memset(aux,0xcc,aux_bytes);}
    for(u32 i=0;i<count;++i) {
        const unsigned char *wire=golden_aux+(size_t)i*40;
        assert(objects[i].handle==get_unaligned_le32(wire) && !objects[i].relocation_count && !objects[i].relocs_ptr);
        assert(objects[i].alignment==get_unaligned_le64(wire+8) && objects[i].offset==get_unaligned_le64(wire+16));
        assert(objects[i].flags==get_unaligned_le64(wire+24) && objects[i].pad_to_size==get_unaligned_le64(wire+32) && !objects[i].rsvd2);
        objects[i].offset=UINT64_C(0x100000000)+(u64)i*4096;
        /* Only the offset may be copied back, even if other private fields change. */
        objects[i].handle=0;objects[i].flags=UINT64_MAX;
    }
    for(u32 i=0;i<fence_count;++i) {
        const unsigned char *wire=golden_aux+(size_t)count*40+(size_t)i*8;
        assert(fences[i].handle==get_unaligned_le32(wire) && fences[i].flags==get_unaligned_le32(wire+4));
    }
    return ioctl_error;
}
#include "drm_i915_exec_actual.inc"
static void reset(u32 objects,u32 fences)
{
    assert(allocations==frees);free(storage);free(golden_aux);
    memset(memory,0,sizeof(memory));attempts=allocations=frees=calls=gets=puts_count=work_calls=fail_at=0;
    ioctl_error=0;irq_context=irq_disabled=missing_fd=mutate_peer=false;current=&task;
    task.mm=NULL;task.flags=0;task.pid=1;task.files=&files;
    count=objects;fence_count=fences;aux_bytes=(size_t)objects*40+(size_t)fences*8;
    storage=malloc(aux_bytes+2);golden_aux=malloc(aux_bytes);assert(storage && golden_aux);aux=storage+1;
    memset(storage,0xa5,aux_bytes+2);memset(aux,0,aux_bytes);memset(header_storage,0xa5,sizeof(header_storage));memset(header,0,32);
    put32(header,objects);put32(header+4,fences);put32(header+12,256);
    put_unaligned_le64((1u<<11)|(1u<<18)|(fences?(1u<<19):0),header+16);put32(header+24,91);
    for(u32 i=0;i<objects;++i) {put32(aux+(size_t)i*40,i+1);put_unaligned_le64(4096,aux+(size_t)i*40+8);put_unaligned_le64((u64)i*4096,aux+(size_t)i*40+16);}
    for(u32 i=0;i<fences;++i) {put32(aux+(size_t)objects*40+(size_t)i*8,i+1);put32(aux+(size_t)objects*40+(size_t)i*8+4,(i&1)?2:1);}
    memcpy(golden_header,header,32);memcpy(golden_aux,aux,aux_bytes);
}
static void success_and_errors(void)
{
    const int errors[]={0,-ENOMEM,-EINTR,-ERESTARTSYS,-EAGAIN,-EIO};
    for(unsigned e=0;e<6;++e) for(unsigned f=0;f<2;++f) {
        reset(2,f?2:0);ioctl_error=errors[e];
        assert(kobox_linux_drm_i915_execbuffer(&owner,header,32,aux,aux_bytes)==(errors[e]==-ERESTARTSYS?-EINTR:errors[e]));
        for(u32 i=0;i<count;++i) {
            assert(get_unaligned_le64(aux+(size_t)i*40+16)==UINT64_C(0x100000000)+(u64)i*4096);
            assert(get_unaligned_le32(aux+(size_t)i*40)==i+1 && !get_unaligned_le64(aux+(size_t)i*40+24));
        }
        assert(calls==1 && gets==1 && puts_count==1 && work_calls==1 && allocations==frees);
        assert(storage[0]==0xa5 && storage[aux_bytes+1]==0xa5 && header_storage[0]==0xa5 && header_storage[33]==0xa5);
    }
    reset(8192,65536);assert(!kobox_linux_drm_i915_execbuffer(&owner,header,32,aux,aux_bytes));assert(allocations==frees);
    reset(2,2);mutate_peer=true;assert(!kobox_linux_drm_i915_execbuffer(&owner,header,32,aux,aux_bytes));
    assert(calls==1 && allocations==frees);
}
static void failures(void)
{
    for(unsigned i=1;i<=3;++i) {
        reset(2,2);fail_at=i;
        assert(kobox_linux_drm_i915_execbuffer(&owner,header,32,aux,aux_bytes)==-ENOMEM);
        assert(!calls && allocations==frees && !memcmp(aux,golden_aux,aux_bytes));
    }
    reset(2,2);current=&stranger;
    assert(kobox_linux_drm_i915_execbuffer(&owner,header,32,aux,aux_bytes)==-EPERM && !calls && !attempts);
    reset(2,2);missing_fd=true;
    assert(kobox_linux_drm_i915_execbuffer(&owner,header,32,aux,aux_bytes)==-EBADF && !calls && !attempts);
    for(unsigned which=0;which<8;++which) {
        reset(2,2);
        switch(which) {
        case 0:put32(header,8193);break;
        case 1:put32(header+4,65537);break;
        case 2:put_unaligned_le64(1u<<9,header+16);break;
        case 3:put32(header+28,1);break;
        case 4:put32(aux+4,1);break;
        case 5:put_unaligned_le64(1u<<8,aux+24);break;
        case 6:put_unaligned_le64(4096,aux+32);break;
        case 7:put32(aux+80+4,4);break;
        }
        assert(kobox_linux_drm_i915_execbuffer(&owner,header,32,aux,aux_bytes)==-EINVAL);
        assert(!calls && allocations==frees);
    }
    reset(2,2);
    assert(kobox_linux_drm_i915_execbuffer(&owner,header,31,aux,aux_bytes)==-EINVAL);
    assert(kobox_linux_drm_i915_execbuffer(&owner,header,32,aux,aux_bytes-1)==-EINVAL);
    assert(!calls && !attempts);
}
int main(void)
{
    success_and_errors();failures();assert(allocations==frees);free(storage);free(golden_aux);
    puts("I915 EXEC actual callback: private BO/fence graph, limits, real ioctl/errno, offset writes, owner and allocation cleanup PASS");
}
