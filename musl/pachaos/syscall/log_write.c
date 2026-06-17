#include <stdint.h>
#include "pachaos/abi.h"

long pachaos_log_write(const char *data, uint64_t len)
{
	uint64_t ret;
	__asm__ volatile (
		"syscall"
		: "=a"(ret)
		: "a"((uint64_t)PACHAOS_SYSCALL_LOG), "D"((uint64_t)(uintptr_t)data), "S"(len)
		: "rcx", "r11", "memory");
	return (long)ret;
}
