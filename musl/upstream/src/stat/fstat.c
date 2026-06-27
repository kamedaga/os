#define _BSD_SOURCE
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include "syscall.h"

int __fstat(int fd, struct stat *st)
{
	if (fd<0) return __syscall_ret(-EBADF);
#ifdef PACHAOS_SYSCALL_FD_READ
	return __syscall_ret(__syscall(SYS_fstat, fd, st));
#else
	return __fstatat(fd, "", st, AT_EMPTY_PATH);
#endif
}

weak_alias(__fstat, fstat);
