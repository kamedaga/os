#define _BSD_SOURCE
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include "syscall.h"

int brk(void *end)
{
	return __syscall(SYS_brk, end) == (uintptr_t)end ? 0 : __syscall_ret(-ENOMEM);
}
