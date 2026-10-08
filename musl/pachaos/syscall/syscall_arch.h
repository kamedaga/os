/* SPDX-License-Identifier: MIT */
#ifndef PACHAOS_NATIVE_SYSCALL_ARCH_H
#define PACHAOS_NATIVE_SYSCALL_ARCH_H
#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)
long __pachaos_native_syscall(long, long, long, long, long, long, long)
    __attribute__((visibility("hidden")));
static __inline long __syscall0(long n) { return __pachaos_native_syscall(n,0,0,0,0,0,0); }
static __inline long __syscall1(long n,long a) { return __pachaos_native_syscall(n,a,0,0,0,0,0); }
static __inline long __syscall2(long n,long a,long b) { return __pachaos_native_syscall(n,a,b,0,0,0,0); }
static __inline long __syscall3(long n,long a,long b,long c) { return __pachaos_native_syscall(n,a,b,c,0,0,0); }
static __inline long __syscall4(long n,long a,long b,long c,long d) { return __pachaos_native_syscall(n,a,b,c,d,0,0); }
static __inline long __syscall5(long n,long a,long b,long c,long d,long e) { return __pachaos_native_syscall(n,a,b,c,d,e,0); }
static __inline long __syscall6(long n,long a,long b,long c,long d,long e,long f) { return __pachaos_native_syscall(n,a,b,c,d,e,f); }
static __inline long __syscall7(long n,long a,long b,long c,long d,long e,long f,long g) { (void)g; return __pachaos_native_syscall(n,a,b,c,d,e,f); }
#define IPC_64 0
#endif
