/* SPDX-License-Identifier: GPL-2.0-only */
/* Compile the actual backend and owner checks. Only the Linux ioctl, fd and
 * allocator boundaries are modeled; the reconstructed items use real UAPI. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <drm/i915_drm.h>
#include "../kobox2/linux-sandbox/kobox/boot/drm_file.h"
typedef uint64_t u64;
typedef uint32_t u32;
typedef int32_t s32;
#define SZ_16M (16u * 1024u * 1024u)
#define ALIGN(v,a) (((v)+(a)-1)&~((size_t)(a)-1))
#define GFP_KERNEL 0
#define PF_KTHREAD 1
struct file { int tag; };
struct files_struct { int tag; };
struct task_struct { void *mm; unsigned flags, pid; struct files_struct *files; };
struct kobox_linux_drm_file { struct file *guard; struct task_struct *owner;
    struct files_struct *files; int fd; };
static struct file installed;
static struct files_struct files;
static struct task_struct task={.pid=1,.files=&files}, stranger;
static struct task_struct *current=&task;
static bool irq_context, irq_disabled, missing_fd, allocation_fail;
static unsigned calls, allocations, frees, gets, puts_count, fail_call;
static int ioctl_error;
static unsigned char *aux;
static size_t aux_bytes;
static uint64_t observed_flags;
static int in_interrupt(void) { return irq_context; }
static int irqs_disabled(void) { return irq_disabled; }
static struct file *fget(int fd) { assert(fd==71);++gets;return missing_fd?NULL:&installed; }
static void fput(struct file *f) { assert(f==&installed);++puts_count; }
static void *kcalloc(size_t n,size_t bytes,unsigned flags) {
    assert(flags==GFP_KERNEL && n<=64 && bytes==sizeof(struct drm_i915_query_item));
    if(allocation_fail)return NULL;
    void *p=calloc(n,bytes);assert(p);++allocations;return p;
}
static void kfree(void *p) { assert(p);++frees;free(p); }
static u32 get_unaligned_le32(const void *p) {
    const unsigned char *b=p;return b[0]|(u32)b[1]<<8|(u32)b[2]<<16|(u32)b[3]<<24;
}
static u64 get_unaligned_le64(const void *p) {
    const unsigned char *b=p;return get_unaligned_le32(b)|(u64)get_unaligned_le32(b+4)<<32;
}
static void put_unaligned_le32(u32 v,void *p) {
    unsigned char *b=p;for(unsigned i=0;i<4;++i)b[i]=(unsigned char)(v>>(i*8));
}
static void put64(void *p,u64 v) {
    unsigned char *b=p;put_unaligned_le32((u32)v,b);put_unaligned_le32((u32)(v>>32),b+4);
}
static size_t required(u64 id) { return id==2?24:id==4?32:id==5?7:16; }
static int private_ioctl(struct kobox_linux_drm_file *file,unsigned command,void *pointer) {
    assert(file && command==DRM_IOCTL_I915_QUERY);
    struct drm_i915_query *q=pointer;
    assert(!q->flags && q->num_items<=1);
    ++calls;
    if(fail_call==calls)return ioctl_error;
    if(!q->num_items) { assert(!q->items_ptr);return 0; }
    struct drm_i915_query_item *item=(void *)(uintptr_t)q->items_ptr;
    assert(item && ((uintptr_t)item%_Alignof(struct drm_i915_query_item))==0);
    assert((uintptr_t)item < (uintptr_t)aux || (uintptr_t)item >= (uintptr_t)aux+aux_bytes);
    assert(item->query_id && item->query_id<=7 && item->query_id!=3);
    observed_flags=item->flags;
    if(item->flags && item->query_id!=6) { item->length=-EINVAL;return 0; }
    size_t size=required(item->query_id);
    if(!item->length) { assert(!item->data_ptr);item->length=(s32)size;return 0; }
    if(item->length<0 || (size_t)item->length<size) { item->length=-EINVAL;return 0; }
    if(!item->data_ptr) { item->length=-EFAULT;return 0; }
    unsigned char *blob=(void *)(uintptr_t)item->data_ptr;
    assert(blob>=aux && blob<=aux+aux_bytes && size<=(size_t)(aux+aux_bytes-blob));
    /* The top-level wire offset never becomes a Linux pointer. */
    memset(blob,(int)item->query_id,size);
    item->length=item->query_id==7?0:(s32)size;
    return 0;
}
#include "drm_i915_query_actual.inc"
static struct kobox_linux_drm_file owner={.guard=&installed,.owner=&task,.files=&files,.fd=71};
static unsigned char storage[1024];
static void reset(size_t bytes) {
    memset(storage,0,sizeof(storage));aux=storage;aux_bytes=bytes;
    calls=allocations=frees=gets=puts_count=fail_call=0;
    ioctl_error=0;allocation_fail=missing_fd=irq_context=irq_disabled=false;
    current=&task;task.mm=NULL;task.pid=1;task.flags=0;task.files=&files;
}
static void item(unsigned i,u64 id,s32 length,u32 flags,u64 offset) {
    unsigned char *p=aux+i*24;put64(p,id);put_unaligned_le32((u32)length,p+8);
    put_unaligned_le32(flags,p+12);put64(p+16,offset);
}
static s32 length(unsigned i) { return (s32)get_unaligned_le32(aux+i*24+8); }
static void reject(u32 count,size_t bytes) {
    unsigned char before[sizeof(storage)];memcpy(before,storage,sizeof(storage));
    assert(kobox_linux_drm_i915_query(&owner,0,count,aux,bytes)==-EINVAL);
    assert(!calls && !allocations && !memcmp(before,storage,sizeof(storage)));
}
static void probes(void) {
    reset(6*24);unsigned ids[]={1,2,4,5,6,7};
    for(unsigned i=0;i<6;++i)item(i,ids[i],0,0,0);
    assert(!kobox_linux_drm_i915_query(&owner,0,6,aux,aux_bytes));
    for(unsigned i=0;i<6;++i)assert(length(i)==(s32)required(ids[i]));
    assert(calls==6 && allocations==1 && frees==1);
    /* All blobs are eight-byte aligned, but the final blob may end between
     * boundaries. Padding is outside every exported driver destination. */
    reset(3*24+16+7+1+16);
    item(0,1,16,0,72);item(1,5,7,0,88);item(2,7,16,0,96);
    assert(!kobox_linux_drm_i915_query(&owner,0,3,aux,aux_bytes));
    assert(length(0)==16 && length(1)==7 && length(2)==0 && aux[95]==0);
    assert(aux[72]==1 && aux[88]==5 && aux[96]==7 && frees==1);
    reset(40);item(0,6,16,0x10002,24);
    assert(!kobox_linux_drm_i915_query(&owner,0,1,aux,40) && observed_flags==0x10002);
    reset(24);item(0,1,16,0,0);
    assert(!kobox_linux_drm_i915_query(&owner,0,1,aux,24) && length(0)==-EFAULT);
    reset(24);item(0,1,-EIO,0,0);
    assert(!kobox_linux_drm_i915_query(&owner,0,1,aux,24) && length(0)==-EINVAL);
    reset(40);item(0,1,16,1,24);
    assert(!kobox_linux_drm_i915_query(&owner,0,1,aux,40) && length(0)==-EINVAL);
    reset(25);item(0,1,1,0,24);
    assert(!kobox_linux_drm_i915_query(&owner,0,1,aux,25) && length(0)==-EINVAL);
}
static void refusal(void) {
    reset(40);item(0,1,16,0,16);reject(1,40); /* prefix overlap */
    reset(48);item(0,1,16,0,32);reject(1,48); /* leading hole */
    reset(96);item(0,1,24,0,48);item(1,2,24,0,48);reject(2,96);
    reset(65);item(0,1,16,0,49);reject(1,65); /* unaligned */
    reset(40);item(0,1,17,0,24);reject(1,40); /* short region */
    reset(40);item(0,1,16,0,UINT64_MAX-7);reject(1,40);
    reset(24);item(0,1,0,0,24);reject(1,24);
    reset(24);item(0,1,-1,0,24);reject(1,24);
    reset(48);item(0,1,16,0,24);reject(1,48); /* trailing capacity */
    reset(24);reject(1,23);reject(65,24);reject(1,SZ_16M+1);
    assert(kobox_linux_drm_i915_query(&owner,0,1,NULL,24)==-EINVAL);
    assert(kobox_linux_drm_i915_query(&owner,1,1,aux,24)==-EINVAL && !calls);
    reset(0);assert(!kobox_linux_drm_i915_query(&owner,0,0,NULL,0) && calls==1 && !allocations);
    reset(1);reject(0,1);
}
static void blocked(void) {
    reset(3*24);item(0,3,0,3,0);item(1,8,0,0,0);item(2,UINT64_MAX,-EIO,0,0);
    assert(!kobox_linux_drm_i915_query(&owner,0,3,aux,aux_bytes));
    assert(length(0)==-EOPNOTSUPP && length(1)==-EINVAL && length(2)==-EINVAL && !calls && frees==1);
    reset(40);item(0,3,16,3,24);memset(aux+24,0xff,16);
    assert(!kobox_linux_drm_i915_query(&owner,0,1,aux,40) && length(0)==-EOPNOTSUPP && !calls);
    for(unsigned i=24;i<40;++i)assert(aux[i]==0xff);
    reset(3*24);item(0,1,0,0,0);item(1,0,0,0,0);item(2,2,0,0,0);
    assert(kobox_linux_drm_i915_query(&owner,0,3,aux,aux_bytes)==-EINVAL);
    assert(length(0)==16 && length(1)==0 && length(2)==0 && calls==1 && frees==1);
    reset(24);item(0,0,0,0,0);
    assert(kobox_linux_drm_i915_query(&owner,0,1,aux,24)==-EINVAL && !calls && frees==1);
}
static void authority_and_failure(void) {
    reset(24);item(0,1,0,0,0);current=&stranger;
    assert(kobox_linux_drm_i915_query(&owner,0,1,aux,24)==-EPERM && !allocations);
    current=&task;task.mm=&files;
    assert(kobox_linux_drm_i915_query(&owner,0,1,aux,24)==-EPERM);
    task.mm=NULL;irq_context=true;assert(kobox_linux_drm_i915_query(&owner,0,1,aux,24)==-EPERM);
    irq_context=false;irq_disabled=true;assert(kobox_linux_drm_i915_query(&owner,0,1,aux,24)==-EPERM);
    irq_disabled=false;missing_fd=true;
    assert(kobox_linux_drm_i915_query(&owner,0,1,aux,24)==-EBADF && !allocations);
    missing_fd=false;allocation_fail=true;
    assert(kobox_linux_drm_i915_query(&owner,0,1,aux,24)==-ENOMEM && !calls && length(0)==0);
    const int errors[]={-EIO,-EINTR,-EAGAIN};
    for(unsigned i=0;i<3;++i) {
        reset(48);item(0,1,0,0,0);item(1,2,0,0,0);fail_call=2;ioctl_error=errors[i];
        assert(kobox_linux_drm_i915_query(&owner,0,2,aux,48)==errors[i]);
        assert(length(0)==16 && length(1)==0 && calls==2 && frees==1);
    }
}
static void limits(void) {
    reset(0);aux_bytes=64*24;aux=calloc(1,aux_bytes);assert(aux);
    for(unsigned i=0;i<64;++i)item(i,1,0,0,0);
    assert(!kobox_linux_drm_i915_query(&owner,0,64,aux,aux_bytes) && calls==64 && frees==1);free(aux);
    reset(0);aux_bytes=SZ_16M;aux=calloc(1,aux_bytes);assert(aux);
    item(0,1,(s32)(SZ_16M-24),0,24);
    assert(!kobox_linux_drm_i915_query(&owner,0,1,aux,aux_bytes) && length(0)==16 && frees==1);free(aux);
    reset(41);aux=storage+1;aux_bytes=40;item(0,1,16,0,24);
    assert(!kobox_linux_drm_i915_query(&owner,0,1,aux,40) && length(0)==16);
}
int main(void) {
    probes();refusal();blocked();authority_and_failure();limits();
    puts("I915_QUERY backend: private pointers, size/probe/NULL, per-item errors, blocked PERF/future IDs, packing/bounds, owner/cleanup and limits PASS");
}
