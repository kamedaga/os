#ifndef LPR_CLOCK_ENTRY_H
#define LPR_CLOCK_ENTRY_H
/* Private offsets, checked against the C types in lpr_clock.c. No wire ABI. */
#define LPR_CLOCK_HINT_OFFSET 0
#define LPR_CLOCK_HINT_ACTIVE_OFFSET 4
#define LPR_CLOCK_PENDING_OFFSET 32
/* A diagnostic build must retain the ordinary dispatch/profile boundaries. */
#if (defined(LPR_SYSCALL_PROFILE) && LPR_SYSCALL_PROFILE) || \
    (defined(LPR_STARTUP_PROFILE) && LPR_STARTUP_PROFILE) || \
    (defined(LPR_GUI_PROFILE) && LPR_GUI_PROFILE) || \
    (defined(LPR_EPHY_PROFILE) && LPR_EPHY_PROFILE) || \
    (defined(LPR_DRAW_PROFILE) && LPR_DRAW_PROFILE) || \
    (defined(LPR_WAIT_TRACE) && LPR_WAIT_TRACE) || \
    (defined(LPR_GLYCIN_DIAG) && LPR_GLYCIN_DIAG) || \
    (defined(LPR_VM_ORIGIN_DIAG) && LPR_VM_ORIGIN_DIAG)
#define LPR_CLOCK_LEAF_ENABLED 0
#else
#define LPR_CLOCK_LEAF_ENABLED 1
#endif
#endif
