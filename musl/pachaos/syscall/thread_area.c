#include <stdint.h>
#include "pachaos/abi.h"

static long pachaos_raw_syscall1(uint64_t nr, uint64_t a0)
{
	uint64_t ret;
	__asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a0) : "rcx", "r11", "memory");
	return (long)ret;
}

int __set_thread_area(void *p)
{
	long ret = pachaos_raw_syscall1(PACHAOS_SYSCALL_THREAD_SET_FS_BASE, (uint64_t)(uintptr_t)p);
	return ret == PACHAOS_SYSCALL_OK ? 0 : -1;
}
