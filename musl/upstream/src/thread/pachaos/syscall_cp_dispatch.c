#include "syscall.h"

hidden long __pachaos_syscall_cp_dispatch(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	return __syscall(n, a1, a2, a3, a4, a5, a6);
}
