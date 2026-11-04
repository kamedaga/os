/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_thread_page.c"
static unsigned char data[8192] __attribute__((aligned(4096)));
uint32_t lpr_thread_test_cpu;
static int unsupported, opens, maps, closes, fail_map_at, map_attempts, corrupt_remap;
void lpr_state_lock(volatile uint32_t *p) { assert(!*p); *p=1; }
void lpr_state_unlock(volatile uint32_t *p) { *p=0; }
int64_t lpr_pacha_syscall1(uint64_t n,uint64_t fd) { assert(n==PACHAOS_SYSCALL_FD_CLOSE&&fd==16);closes++;return 0; }
int64_t lpr_pacha_syscall2(uint64_t n,uint64_t p,uint64_t size) { assert(n==PACHAOS_SYSCALL_MUNMAP&&p==(uintptr_t)data&&(size==4096||size==8192));return 0; }
int64_t lpr_pacha_syscall3(uint64_t n,uint64_t size,uint64_t rights,uint64_t flags) {
 assert(n==PACHA_FD_SYSCALL_VMO_CREATE&&!size&&!rights);
 assert(flags==(PACHA_VMO_CREATE_CURRENT_THREAD_PAGE|PACHA_FD_FLAG_PRIVATE|PACHA_FD_FLAG_CLOEXEC));
 opens++;return unsupported?-2:16;
}
int64_t lpr_pacha_syscall6(uint64_t n,uint64_t fd,uint64_t addr,uint64_t bytes,uint64_t prot,uint64_t flags,uint64_t offset) {
 assert(n==PACHAOS_SYSCALL_MMAP&&fd==16&&!addr&&!offset&&prot==PACHAOS_PROT_READ&&flags==PACHAOS_MMAP_SHARED);
 assert(bytes==4096||bytes==8192); maps++;map_attempts++;
 if(map_attempts==fail_map_at)return 1;
 if(corrupt_remap&&map_attempts==2){uint64_t zero=0;memcpy(data+32,&zero,8);}
 return (uintptr_t)data;
}
static void put(unsigned off,uint64_t v) { memcpy(data+off,&v,8); }
static void reset(unsigned capacity) {
 lpr_thread_page_after_fork_child();memset(data,0,sizeof data);
 put(0,PACHA_THREAD_PAGE_MAGIC);put(8,capacity>63?8192:4096);put(16,capacity);put(24,64);put(32,1);put(40,11);
 put(64,2);put(72,123);unsupported=fail_map_at=map_attempts=corrupt_remap=0;lpr_thread_test_cpu=0;
}
int main(void) {
 reset(4);unsupported=1;lpr_thread_page_initialize();assert(!lpr_thread_fast_page&&!lpr_thread_page_read());
 int old=opens;lpr_thread_page_initialize();assert(opens==old); /* old kernel: one failed open per image */
 reset(4);lpr_thread_page_initialize();assert(lpr_thread_page_read()==123);
 put(64,0);assert(!lpr_thread_page_read());put(64,1);assert(!lpr_thread_page_read());
 put(64,UINT64_MAX);assert(!lpr_thread_page_read());put(64,2);put(32,0);assert(!lpr_thread_page_read());
 put(32,1);lpr_thread_test_cpu=4;assert(!lpr_thread_page_read());lpr_thread_test_cpu=0;
 put(72,0);assert(!lpr_thread_page_read());put(72,999);assert(lpr_thread_page_read()==999);
 lpr_thread_page_after_fork_child();assert(!lpr_thread_fast_page&&!lpr_thread_page_read());
 reset(64);lpr_thread_page_initialize();assert(lpr_thread_page_read()==123);assert(maps>=3);
 for(unsigned off=0;off<48;off+=8) {reset(4);put(off,0);lpr_thread_page_initialize();assert(!lpr_thread_fast_page);}
 reset(4);put(48,1);lpr_thread_page_initialize();assert(!lpr_thread_fast_page);
 reset(4);put(16,UINT64_MAX);lpr_thread_page_initialize();assert(!lpr_thread_fast_page);
 reset(4);fail_map_at=1;lpr_thread_page_initialize();assert(!lpr_thread_fast_page&&!lpr_thread_page_read());
 old=opens;lpr_thread_page_initialize();assert(opens==old);
 reset(64);fail_map_at=2;lpr_thread_page_initialize();assert(!lpr_thread_fast_page&&!lpr_thread_page_read());assert(map_attempts==2);
 reset(64);corrupt_remap=1;lpr_thread_page_initialize();assert(!lpr_thread_fast_page);
 reset(4);mapping_lock=1;old=opens;lpr_thread_page_initialize();assert(opens==old&&!lpr_thread_fast_page&&!lpr_thread_page_read());
 mapping_lock=0;lpr_thread_page_initialize();assert(lpr_thread_page_read()==123);
 printf("THREAD_PAGE_UNIT_PASS old-kernel map header multi-page seq-zero odd overflow closed capacity fork tid-reuse map-failure remap-failure reentrant opens=%d closes=%d\n",opens,closes);
}
