/* SPDX-License-Identifier: GPL-2.0-only */
/* The runner includes the actual service function bodies, not a second
 * implementation. Only Linux ownership/fd/dma-buf boundaries are modeled. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef uint64_t u64;
enum dma_data_direction { DMA_BIDIRECTIONAL, DMA_TO_DEVICE, DMA_FROM_DEVICE };
#define DRM_PRIME_LIMIT 2
#define FMODE_READ 1
#define FMODE_WRITE 2
struct files_struct { int unused; };
struct task_struct { struct files_struct *files; void *mm; };
struct drm_owned_prime { u64 id; int dma_fd; };
struct kobox_linux_drm_service { struct task_struct *owner; struct files_struct *files;
    struct drm_owned_prime primes[DRM_PRIME_LIMIT]; bool stopping;
    struct { int close_error; } report; };
struct file { int f_mode; };
struct fd { struct file *file; };
struct dma_buf { struct file *file; };
static struct task_struct task, other, *current = &task;
static struct files_struct files;
static struct file exported = {3};
static struct dma_buf buffer = {&exported};
static int irq, disabled, get_error, access_error;
static unsigned gets, put_count, begins, ends;
static enum dma_data_direction last_direction;
static int in_interrupt(void) { return irq; }
static int irqs_disabled(void) { return disabled; }
static struct dma_buf *dma_buf_get(int fd) {
    assert(fd==71); ++gets;
    return get_error ? (void *)(intptr_t)get_error : &buffer;
}
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
static void dma_buf_put(struct dma_buf *b) { assert(b==&buffer); ++put_count; }
static int dma_buf_begin_cpu_access(struct dma_buf *b, enum dma_data_direction d) {
    assert(b==&buffer); ++begins; last_direction=d; return access_error;
}
static int dma_buf_end_cpu_access(struct dma_buf *b, enum dma_data_direction d) {
    assert(b==&buffer); ++ends; last_direction=d; return access_error;
}
#include "drm_cpu_sync_actual.inc"
int main(void) {
    task.files=&files;
    struct kobox_linux_drm_service s={.owner=&task,.files=&files,
        .primes={{4097,71},{0,-1}}};
    const uint64_t flags[]={1,2,3,5,6,7};
    const enum dma_data_direction directions[]={DMA_FROM_DEVICE,DMA_TO_DEVICE,DMA_BIDIRECTIONAL};
    for(unsigned i=0;i<6;++i) {
        assert(!kobox_linux_drm_service_prime_cpu_sync(&s,4097,flags[i]));
        assert(last_direction==directions[i%3]);
    }
    assert(gets==6 && put_count==6 && begins==3 && ends==3);
    const uint64_t invalid[]={0,4,8,UINT64_C(1)<<32|1,UINT64_MAX};
    for(unsigned i=0;i<5;++i) assert(kobox_linux_drm_service_prime_cpu_sync(&s,4097,invalid[i])==-EINVAL);
    assert(kobox_linux_drm_service_prime_cpu_sync(&s,0,1)==-EOPNOTSUPP);
    assert(kobox_linux_drm_service_prime_cpu_sync(&s,8193,1)==-ENOENT);
    s.stopping=true; assert(kobox_linux_drm_service_prime_cpu_sync(&s,4097,1)==-ESHUTDOWN); s.stopping=false;
    s.report.close_error=-EIO; assert(kobox_linux_drm_service_prime_cpu_sync(&s,4097,1)==-ESHUTDOWN); s.report.close_error=0;
    current=&other; assert(kobox_linux_drm_service_prime_cpu_sync(&s,4097,1)==-EPERM); current=&task;
    task.mm=&files; assert(kobox_linux_drm_service_prime_cpu_sync(&s,4097,1)==-EPERM); task.mm=NULL;
    irq=1; assert(kobox_linux_drm_service_prime_cpu_sync(&s,4097,1)==-EPERM); irq=0;
    disabled=1; assert(kobox_linux_drm_service_prime_cpu_sync(&s,4097,1)==-EPERM); disabled=0;
    assert(gets==6 && put_count==6);
    exported.f_mode=FMODE_READ; assert(kobox_linux_drm_service_prime_cpu_sync(&s,4097,2)==-EACCES);
    exported.f_mode=FMODE_WRITE; assert(kobox_linux_drm_service_prime_cpu_sync(&s,4097,1)==-EACCES);
    exported.f_mode=3;
    get_error=-EBADF; assert(kobox_linux_drm_service_prime_cpu_sync(&s,4097,1)==-EBADF); get_error=0;
    assert(gets==9 && put_count==8);
    const int errors[]={-EINTR,-EAGAIN,-EIO};
    for(unsigned i=0;i<3;++i) {
        access_error=errors[i];
        assert(kobox_linux_drm_service_prime_cpu_sync(&s,4097,3)==errors[i]);
        assert(kobox_linux_drm_service_prime_cpu_sync(&s,4097,7)==errors[i]);
    }
    assert(gets==15 && put_count==14);
    puts("DMA-BUF backend: owner/token/flags/access mode, directions, get/put and exporter errno PASS");
}
