/* SPDX-License-Identifier: MIT */
/* LPR machine entry is native bootstrap state, never a Linux RAM grant. */
#include "vm_internal.h"
#include <personality/linux_lpr.h>
#include <personality/zpoline.h>

void ph_vm_install_lpr_entry(struct ph_vm_space *space) {
    uint64_t entry = space->control->syscall_entry;
    uint64_t rights = PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_MAP_EXEC | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP;
    long backing = pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, PH_PAGE_SIZE, rights, 0);

    PH_CHECK(entry >= PH_VM_CLIENT_IMAGE_BASE && entry < PH_VM_CLIENT_IMAGE_LIMIT);
    PH_CHECK(backing >= 16);
    long alias = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, backing, 0, PH_PAGE_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);

    PH_CHECK(alias >= (long)PH_PAGE_SIZE);
    PH_OK(lpr_build_zpoline_page((void *)(uintptr_t)alias, entry));
    /* The target's protection ceiling must be RX, even though preparation
     * required a temporary writable parent alias of this private VMO. */
    long executable = pacha_syscall4(PACHA_FD_SYSCALL_DUP, backing, 16,
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_EXEC | PACHA_FD_RIGHT_CLOSE, 0);

    if (executable < 16) {
        ph_fail(__FILE__, __LINE__, executable);
    }
    PH_CHECK(pacha_syscall6(PACHA_PROCESS_SYSCALL_MAP, space->process_fd, executable,
        LPR_ZPOLINE_PAGE_VA, LPR_ZPOLINE_PAGE_SIZE, PACHA_PROT_READ | PACHA_PROT_EXEC,
        PACHA_PROCESS_MAP_SHARED) == LPR_ZPOLINE_PAGE_VA);
    PH_OK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, executable));
    PH_OK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, backing));
    ph_free((void *)(uintptr_t)alias, PH_PAGE_SIZE);
}
