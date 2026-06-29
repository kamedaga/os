#define _BSD_SOURCE
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include "syscall.h"

void *sbrk(intptr_t inc)
{
	uintptr_t old = __syscall(SYS_brk, 0);
	uintptr_t new;

	if (!inc)
		return (void *)old;
	if (inc > 0) {
		if (old > UINTPTR_MAX - (uintptr_t)inc)
			return (void *)__syscall_ret(-ENOMEM);
		new = old + (uintptr_t)inc;
	} else {
		uintptr_t dec = (uintptr_t)(-(inc + 1)) + 1;
		if (old < dec)
			return (void *)__syscall_ret(-ENOMEM);
		new = old - dec;
	}
	if (__syscall(SYS_brk, new) != new)
		return (void *)__syscall_ret(-ENOMEM);
	return (void *)old;
}
