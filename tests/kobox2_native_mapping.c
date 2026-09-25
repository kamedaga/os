/* SPDX-License-Identifier: MIT */
/* Native PTE/VMO checks supplement, but never replace, the unchanged core Gates. */
#include "host.h"

#define FIXED UINT64_C(1)
struct mapping_probe {
    volatile uint64_t *target;
    atomic_uint command, done;
    uint64_t expected, written;
};
static void *mapping_peer(void *opaque) {
    struct mapping_probe *p = opaque;
    for (unsigned step = 1; ; ++step) {
        unsigned command;
        while ((command = atomic_load_explicit(&p->command, memory_order_acquire)) < step)
            ph_wait(&p->command, command);
        if (command == UINT32_MAX) return NULL;
        PH_CHECK(*p->target == p->expected);
        *p->target = p->written;
        atomic_store_explicit(&p->done, step, memory_order_release); ph_wake(&p->done);
    }
}
void ph_verify_native_mapping(void) {
    const uint64_t rights = PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_CLOSE;
    long fd = pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, 3 * PH_PAGE_SIZE, rights, 0);
    PH_CHECK(fd >= 16);
    long alias = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, fd, 0, 3 * PH_PAGE_SIZE, 3, PACHA_MMAP_SHARED, 0);
    PH_CHECK(alias >= (long)PH_PAGE_SIZE);
    volatile uint64_t *source = (void *)(uintptr_t)alias;
    source[0] = 11; source[PH_PAGE_SIZE / 8] = 22; source[2 * PH_PAGE_SIZE / 8] = 33;
    volatile uint64_t *view = ph_alloc(3 * PH_PAGE_SIZE);
    view[0] = 101; view[PH_PAGE_SIZE / 8] = 202; view[2 * PH_PAGE_SIZE / 8] = 303;
    uintptr_t middle = (uintptr_t)view + PH_PAGE_SIZE;
    long read_only = pacha_syscall4(PACHA_FD_SYSCALL_DUP, fd, 16,
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_CLOSE, 0);
    PH_CHECK(read_only >= 16);
    PH_CHECK(pacha_syscall6(PACHA_VM_SYSCALL_MMAP, read_only, middle, PH_PAGE_SIZE, 3,
        FIXED | PACHA_MMAP_SHARED, 0) == PACHA_SYSCALL_ERR_INVALID);
    PH_CHECK(pacha_syscall6(PACHA_VM_SYSCALL_MMAP, fd, middle, PH_PAGE_SIZE, 3,
        FIXED | PACHA_MMAP_SHARED, 3 * PH_PAGE_SIZE) == PACHA_SYSCALL_ERR_INVALID);
    PH_CHECK(pacha_syscall6(PACHA_VM_SYSCALL_MMAP, fd, middle, PH_PAGE_SIZE, 3,
        FIXED | PACHA_MMAP_SHARED | PACHA_MMAP_PRIVATE, 0) == PACHA_SYSCALL_ERR_INVALID);
    PH_CHECK(view[0] == 101 && view[PH_PAGE_SIZE / 8] == 202 && view[2 * PH_PAGE_SIZE / 8] == 303);
    PH_OK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, read_only));
    struct mapping_probe probe = {.target = (void *)middle, .expected = 202, .written = 203};
    struct ph_task *peer = ph_thread_create(mapping_peer, &probe);
    /* Populate the peer's old PTE before replacements. Both explicit shared
     * and default native VMO mappings must expose writes, not private COW. */
    for (unsigned step = 1; step <= 17; ++step) {
        size_t offset = (step % 3) * PH_PAGE_SIZE;
        if (step > 1) {
            source[offset / 8] = 1000 + step;
            PH_CHECK((uintptr_t)pacha_syscall6(PACHA_VM_SYSCALL_MMAP, fd, middle, PH_PAGE_SIZE,
                3, FIXED | (step & 1 ? PACHA_MMAP_SHARED : 0), offset) == middle);
            probe.expected = 1000 + step; probe.written = 2000 + step;
        }
        atomic_store_explicit(&probe.command, step, memory_order_release); ph_wake(&probe.command);
        unsigned done;
        while ((done = atomic_load_explicit(&probe.done, memory_order_acquire)) != step)
            ph_wait(&probe.done, done);
        PH_CHECK(*probe.target == probe.written);
        if (step > 1) PH_CHECK(source[offset / 8] == probe.written);
        PH_CHECK(view[0] == 101 && view[2 * PH_PAGE_SIZE / 8] == 303);
    }
    atomic_store_explicit(&probe.command, UINT32_MAX, memory_order_release); ph_wake(&probe.command);
    PH_OK(ph_thread_join(peer));
    PH_OK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, fd));
    *probe.target = 9999;
    PH_CHECK(source[2 * PH_PAGE_SIZE / 8] == 9999);
    ph_free((void *)view, 3 * PH_PAGE_SIZE); ph_free((void *)(uintptr_t)alias, 3 * PH_PAGE_SIZE);
    ph_log("PACHA_KOBOX_NATIVE_MAPPING=PASS\n");
}
