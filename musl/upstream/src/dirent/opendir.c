#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include "__dirent.h"
#include "syscall.h"

#ifdef __pachaos__
#include "atomic.h"

#define PACHAOS_DIR_POOL_CAP 16

struct __dirstream __pachaos_dir_pool[PACHAOS_DIR_POOL_CAP];
volatile int __pachaos_dir_pool_used[PACHAOS_DIR_POOL_CAP];

static DIR *__pachaos_dir_alloc(void)
{
	for (int i = 0; i < PACHAOS_DIR_POOL_CAP; i++) {
		if (a_cas(&__pachaos_dir_pool_used[i], 0, 1) == 0) {
			memset(&__pachaos_dir_pool[i], 0, sizeof __pachaos_dir_pool[i]);
			return (DIR *)&__pachaos_dir_pool[i];
		}
	}
	return 0;
}
#endif

DIR *opendir(const char *name)
{
	int fd;
	DIR *dir;

	if ((fd = open(name, O_RDONLY|O_DIRECTORY|O_CLOEXEC)) < 0)
		return 0;
#ifdef __pachaos__
	if (!(dir = __pachaos_dir_alloc()) && !(dir = calloc(1, sizeof *dir))) {
#else
	if (!(dir = calloc(1, sizeof *dir))) {
#endif
		__syscall(SYS_close, fd);
		return 0;
	}
	dir->fd = fd;
	return dir;
}
