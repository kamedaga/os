/* SPDX-License-Identifier: MIT */
#include "lpr_filed_internal.h"
#include "lpr_thread_page.h"
const volatile unsigned char *lpr_thread_fast_page;
static uint32_t mapping_done;
static volatile uint32_t mapping_lock;
#define WORD(page, offset) __atomic_load_n((const volatile uint64_t *)((page)+(offset)), __ATOMIC_ACQUIRE)
static int valid_header(const volatile unsigned char *p, uint64_t *bytes)
{
    const uint64_t capacity = WORD(p,16), total = WORD(p,8);
    if (WORD(p,0) != PACHA_THREAD_PAGE_MAGIC || !capacity || capacity > UINT32_MAX ||
        WORD(p,24) != 64 || WORD(p,32) != 1 || !WORD(p,40) || WORD(p,48) || WORD(p,56) ||
        total != ((64 + capacity*64 + 4095) & ~UINT64_C(4095))) return 0;
    *bytes = total; return 1;
}
void lpr_thread_page_initialize(void)
{
    if (__atomic_load_n(&mapping_done, __ATOMIC_ACQUIRE)) return;
    uint32_t expected = 0;
    /* Reentrant/sibling entry uses GETTID until publication; never waits on
     * a potentially interrupted initializer from an arbitrary user stack. */
    if (!__atomic_compare_exchange_n(&mapping_lock,&expected,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return;
    if (mapping_done) { lpr_state_unlock(&mapping_lock); return; }
    int64_t fd = lpr_pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE,0,0,
        PACHA_VMO_CREATE_CURRENT_THREAD_PAGE | PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC);
    if (fd >= 16) {
        uint64_t bytes = 0;
        int64_t address = lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP,fd,0,4096,PACHAOS_PROT_READ,PACHAOS_MMAP_SHARED,0);
        if (address >= 4096) {
            const volatile unsigned char *p = (void *)(uintptr_t)address;
            if (!valid_header(p,&bytes)) bytes = 0;
            if (bytes != 4096) {
                (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,address,4096);
                address = bytes ? lpr_pacha_syscall6(PACHAOS_SYSCALL_MMAP,fd,0,bytes,PACHAOS_PROT_READ,PACHAOS_MMAP_SHARED,0) : -1;
                if (address >= 4096 && !valid_header((void *)(uintptr_t)address,&bytes)) {
                    (void)lpr_pacha_syscall2(PACHAOS_SYSCALL_MUNMAP,address,bytes); address = -1;
                }
            }
            if (address >= 4096) __atomic_store_n(&lpr_thread_fast_page,(void *)(uintptr_t)address,__ATOMIC_RELEASE);
        }
        (void)lpr_pacha_syscall1(PACHAOS_SYSCALL_FD_CLOSE,fd);
    }
    __atomic_store_n(&mapping_done,1,__ATOMIC_RELEASE);
    lpr_state_unlock(&mapping_lock);
}
void lpr_thread_page_fork_lock(void) { lpr_state_lock(&mapping_lock); }
void lpr_thread_page_fork_unlock(void) { lpr_state_unlock(&mapping_lock); }
void lpr_thread_page_after_fork_child(void)
{
    /* Kernel deliberately omits this mapping and its private FD on fork.
     * Clear BEFORE any helper can reenter entry assembly; never unmap or close
     * inherited numbers, which may now name unrelated child resources. */
    __atomic_store_n(&lpr_thread_fast_page,0,__ATOMIC_RELEASE);
    mapping_lock = mapping_done = 0;
}
