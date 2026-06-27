#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "__dirent.h"

#ifdef __pachaos__
#include "atomic.h"

#define PACHAOS_DIR_POOL_CAP 16

extern struct __dirstream __pachaos_dir_pool[PACHAOS_DIR_POOL_CAP];
extern volatile int __pachaos_dir_pool_used[PACHAOS_DIR_POOL_CAP];

static int __pachaos_dir_release(DIR *dir)
{
	for (int i = 0; i < PACHAOS_DIR_POOL_CAP; i++) {
		if (dir == (DIR *)&__pachaos_dir_pool[i]) {
			memset(&__pachaos_dir_pool[i], 0, sizeof __pachaos_dir_pool[i]);
			a_store(&__pachaos_dir_pool_used[i], 0);
			return 1;
		}
	}
	return 0;
}
#endif

int closedir(DIR *dir)
{
	int ret = close(dir->fd);
#ifdef __pachaos__
	if (!__pachaos_dir_release(dir))
		free(dir);
#else
	free(dir);
#endif
	return ret;
}
