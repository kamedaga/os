#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

char *__randname(char *template)
{
	struct timespec ts;
	unsigned long r;

	clock_gettime(CLOCK_REALTIME, &ts);
	r = (unsigned long)ts.tv_nsec ^ ((unsigned long)ts.tv_sec << 7) ^ (unsigned long)(uintptr_t)template;
	for (int i = 0; i < 6; i++, r = r * 1103515245u + 12345u) {
		unsigned v = (unsigned)((r >> 16) & 31);
		template[i] = (char)(v < 26 ? 'A' + v : 'a' + (v - 26));
	}
	return template;
}

pid_t fork(void)
{
	errno = ENOSYS;
	return -1;
}

int execvp(const char *file, char *const argv[])
{
	(void)file;
	(void)argv;
	errno = ENOSYS;
	return -1;
}

pid_t wait(int *status)
{
	(void)status;
	errno = ENOSYS;
	return -1;
}

uid_t getuid(void)
{
	return 0;
}

int getpwnam_r(const char *name, struct passwd *pw, char *buf, size_t size, struct passwd **res)
{
	(void)name;
	(void)pw;
	(void)buf;
	(void)size;
	*res = NULL;
	return ENOENT;
}

int getpwuid_r(uid_t uid, struct passwd *pw, char *buf, size_t size, struct passwd **res)
{
	(void)uid;
	(void)pw;
	(void)buf;
	(void)size;
	*res = NULL;
	return ENOENT;
}

int system(const char *command)
{
	(void)command;
	errno = ENOSYS;
	return -1;
}

void (*signal(int sig, void (*func)(int)))(int)
{
	(void)sig;
	(void)func;
	return SIG_DFL;
}

void abort(void)
{
	_Exit(134);
}
