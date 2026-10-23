/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <pacha/abi.h>
#include <pacha/syscall.h>

typedef struct filed_mutex { uint32_t state; } filed_mutex_t;
static inline void filed_mutex_lock(filed_mutex_t *mutex)
{
    while (__atomic_exchange_n(&mutex->state,1,__ATOMIC_ACQUIRE)) {
#ifdef __pachaos__
        (void)pacha_syscall4(PACHA_RUNTIME_SYSCALL_FUTEX_WAIT,
            (uintptr_t)&mutex->state,1,0,0);
#else
        __builtin_ia32_pause();
#endif
    }
}
static inline void filed_mutex_unlock(filed_mutex_t *mutex)
{
    __atomic_store_n(&mutex->state,0,__ATOMIC_RELEASE);
#ifdef __pachaos__
    (void)pacha_syscall2(PACHA_RUNTIME_SYSCALL_FUTEX_WAKE,
        (uintptr_t)&mutex->state,1);
#endif
}
