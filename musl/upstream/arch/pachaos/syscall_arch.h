#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

#define PACHAOS_SYSCALL_LOG 1
#define PACHAOS_SYSCALL_PROCESS_EXIT 5
#define PACHAOS_SYSCALL_THREAD_SET_FS_BASE 11
#define PACHAOS_SYSCALL_THREAD_SET_GS_BASE 12
#define PACHAOS_SYSCALL_PROCESS_MAP 13
#define PACHAOS_SYSCALL_GETPID 14
#define PACHAOS_SYSCALL_GETTID 15
#define PACHAOS_SYSCALL_CLOCK_GETTIME 16
#define PACHAOS_SYSCALL_NANOSLEEP 17
#define PACHAOS_SYSCALL_FUTEX_WAIT 18
#define PACHAOS_SYSCALL_FUTEX_WAKE 19
#define PACHAOS_SYSCALL_GETRANDOM 20
#define PACHAOS_SYSCALL_FD_CLOSE 21
#define PACHAOS_SYSCALL_FD_READ 25
#define PACHAOS_SYSCALL_FD_WRITE 26
#define PACHAOS_SYSCALL_FD_READV 27
#define PACHAOS_SYSCALL_FD_WRITEV 28
#define PACHAOS_SYSCALL_FD_POLL 30
#define PACHAOS_SYSCALL_FD_WAIT_MANY 31
#define PACHAOS_SYSCALL_FD_IOCTL 32
#define PACHAOS_SYSCALL_FD_STAT 33
#define PACHAOS_SYSCALL_EVENTFD_CREATE 34
#define PACHAOS_SYSCALL_TIMERFD_CREATE 35
#define PACHAOS_SYSCALL_TIMERFD_SETTIME 36
#define PACHAOS_SYSCALL_TIMERFD_GETTIME 37
#define PACHAOS_SYSCALL_MMAP 39
#define PACHAOS_SYSCALL_MUNMAP 40
#define PACHAOS_SYSCALL_MPROTECT 41

#define PACHAOS_FD_FLAG_CLOEXEC 1
#define PACHAOS_FD_FLAG_NONBLOCK 2
#define PACHAOS_FD_RIGHT_INSPECT (1ULL << 0)
#define PACHAOS_FD_RIGHT_WAIT (1ULL << 3)
#define PACHAOS_FD_RIGHT_POLL (1ULL << 4)
#define PACHAOS_FD_RIGHT_CLOSE (1ULL << 6)
#define PACHAOS_FD_RIGHT_READ (1ULL << 42)
#define PACHAOS_FD_RIGHT_WRITE (1ULL << 43)

#define PACHAOS_PROT_READ 1
#define PACHAOS_PROT_WRITE 2
#define PACHAOS_PROT_EXEC 4
#define PACHAOS_MMAP_FIXED 1
#define PACHAOS_MMAP_FIXED_NOREPLACE 2
#define PACHAOS_MMAP_PRIVATE 4
#define PACHAOS_MMAP_SHARED 8
#define PACHAOS_MMAP_ANONYMOUS 16
#define PACHAOS_MMAP_NORESERVE 32

#define LINUX_PROT_READ 1
#define LINUX_PROT_WRITE 2
#define LINUX_PROT_EXEC 4
#define LINUX_MAP_SHARED 1
#define LINUX_MAP_PRIVATE 2
#define LINUX_MAP_FIXED 16
#define LINUX_MAP_ANONYMOUS 32
#define LINUX_MAP_NORESERVE 0x4000
#define LINUX_MAP_FIXED_NOREPLACE 0x100000
#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1
#define LINUX_AT_EMPTY_PATH 0x1000
#define LINUX_CLOCK_MONOTONIC 1
#define LINUX_EFD_SEMAPHORE 1
#define LINUX_EFD_CLOEXEC 0x80000
#define LINUX_EFD_NONBLOCK 0x800
#define LINUX_TFD_CLOEXEC 0x80000
#define LINUX_TFD_NONBLOCK 0x800
#define LINUX_TFD_TIMER_ABSTIME 1
#define LINUX_POLLIN 0x001
#define LINUX_POLLOUT 0x004
#define LINUX_POLLERR 0x008
#define LINUX_POLLHUP 0x010

#define PACHAOS_POLL_READABLE 1
#define PACHAOS_POLL_WRITABLE 2
#define PACHAOS_POLL_ERROR 4
#define PACHAOS_POLL_HANGUP 8
#define PACHAOS_MAX_POLLFDS 64

struct __pachaos_linux_pollfd {
	int fd;
	short events;
	short revents;
};

struct __pachaos_pollfd {
	long fd;
	long events;
	long revents;
};

static __inline long __pachaos_raw0(long n)
{
	unsigned long ret;
	__asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n) : "rcx", "r11", "memory");
	return ret;
}

static __inline long __pachaos_raw1(long n, long a1)
{
	unsigned long ret;
	__asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n), "D"(a1) : "rcx", "r11", "memory");
	return ret;
}

static __inline long __pachaos_raw2(long n, long a1, long a2)
{
	unsigned long ret;
	__asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
	return ret;
}

static __inline long __pachaos_raw3(long n, long a1, long a2, long a3)
{
	unsigned long ret;
	__asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
	return ret;
}

static __inline long __pachaos_raw4(long n, long a1, long a2, long a3, long a4)
{
	unsigned long ret;
	register long r10 __asm__("r10") = a4;
	__asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx", "r11", "memory");
	return ret;
}

static __inline long __pachaos_raw6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	unsigned long ret;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	register long r9 __asm__("r9") = a6;
	__asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
	return ret;
}

static __inline long __pachaos_status(long status)
{
	return status == 0 ? 0 : -22;
}

static __inline long __pachaos_mmap_result(long result)
{
	return result >= 4096 ? result : -12;
}

static __inline long __pachaos_prot(long prot)
{
	long out = 0;
	if (prot & LINUX_PROT_READ) out |= PACHAOS_PROT_READ;
	if (prot & LINUX_PROT_WRITE) out |= PACHAOS_PROT_WRITE;
	if (prot & LINUX_PROT_EXEC) out |= PACHAOS_PROT_EXEC;
	return out;
}

static __inline long __pachaos_mmap_flags(long flags)
{
	long out = 0;
	if (flags & LINUX_MAP_PRIVATE) out |= PACHAOS_MMAP_PRIVATE;
	if (flags & LINUX_MAP_SHARED) out |= PACHAOS_MMAP_SHARED;
	if (flags & LINUX_MAP_ANONYMOUS) out |= PACHAOS_MMAP_ANONYMOUS;
	if (flags & LINUX_MAP_FIXED) out |= PACHAOS_MMAP_FIXED;
	if (flags & LINUX_MAP_FIXED_NOREPLACE) out |= PACHAOS_MMAP_FIXED_NOREPLACE;
	if (flags & LINUX_MAP_NORESERVE) out |= PACHAOS_MMAP_NORESERVE;
	return out;
}

static __inline long __pachaos_poll_events_from_linux(long events)
{
	long out = 0;
	if (events & LINUX_POLLIN) out |= PACHAOS_POLL_READABLE;
	if (events & LINUX_POLLOUT) out |= PACHAOS_POLL_WRITABLE;
	if (events & LINUX_POLLERR) out |= PACHAOS_POLL_ERROR;
	if (events & LINUX_POLLHUP) out |= PACHAOS_POLL_HANGUP;
	return out;
}

static __inline short __pachaos_poll_events_to_linux(long events)
{
	short out = 0;
	if (events & PACHAOS_POLL_READABLE) out |= LINUX_POLLIN;
	if (events & PACHAOS_POLL_WRITABLE) out |= LINUX_POLLOUT;
	if (events & PACHAOS_POLL_ERROR) out |= LINUX_POLLERR;
	if (events & PACHAOS_POLL_HANGUP) out |= LINUX_POLLHUP;
	return out;
}

static __inline long __pachaos_fd_flags_from_linux(long flags, long cloexec_bit, long nonblock_bit)
{
	long out = 0;
	if (flags & cloexec_bit) out |= PACHAOS_FD_FLAG_CLOEXEC;
	if (flags & nonblock_bit) out |= PACHAOS_FD_FLAG_NONBLOCK;
	return out;
}

static __inline long __pachaos_poll(struct __pachaos_linux_pollfd *fds, long n, long timeout)
{
	if (n < 0 || n > PACHAOS_MAX_POLLFDS) return -22;
	struct __pachaos_pollfd native[PACHAOS_MAX_POLLFDS];
	for (long i = 0; i < n; i++) {
		native[i].fd = fds[i].fd;
		native[i].events = __pachaos_poll_events_from_linux(fds[i].events);
		native[i].revents = 0;
	}
	long ret = __pachaos_raw2(PACHAOS_SYSCALL_FD_POLL, (long)native, n);
	if (ret < 0) return ret;
	for (long i = 0; i < n; i++) fds[i].revents = __pachaos_poll_events_to_linux(native[i].revents);
	if (ret != 0 || timeout == 0) return ret;
	if (timeout < 0) {
		for (;;) {
			ret = __pachaos_raw4(PACHAOS_SYSCALL_FD_WAIT_MANY, (long)native, n, ~0ULL, 0);
			if (ret != 2) break;
			ret = __pachaos_raw2(PACHAOS_SYSCALL_FD_POLL, (long)native, n);
			if (ret != 0) break;
		}
	} else {
		ret = __pachaos_raw4(PACHAOS_SYSCALL_FD_WAIT_MANY, (long)native, n, timeout, 0);
		if (ret == 2) ret = __pachaos_raw2(PACHAOS_SYSCALL_FD_POLL, (long)native, n);
		if (ret == 0 || ret == 2) ret = 0;
	}
	if (ret < 0) return ret;
	for (long i = 0; i < n; i++) fds[i].revents = __pachaos_poll_events_to_linux(native[i].revents);
	return ret;
}

static __inline long __syscall0(long n)
{
	switch (n) {
	case __NR_getpid: return __pachaos_raw0(PACHAOS_SYSCALL_GETPID);
	case __NR_gettid: return __pachaos_raw0(PACHAOS_SYSCALL_GETTID);
	default: return -38;
	}
}

static __inline long __syscall1(long n, long a1)
{
	switch (n) {
	case __NR_close: return __pachaos_status(__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, a1));
	case __NR_exit:
	case __NR_exit_group: return __pachaos_raw1(PACHAOS_SYSCALL_PROCESS_EXIT, a1);
	case __NR_set_tid_address: return __pachaos_raw0(PACHAOS_SYSCALL_GETTID);
	default: return -38;
	}
}

static __inline long __syscall2(long n, long a1, long a2)
{
	switch (n) {
	case __NR_munmap: return __pachaos_status(__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, a1, a2));
	case __NR_clock_gettime: return __pachaos_status(__pachaos_raw2(PACHAOS_SYSCALL_CLOCK_GETTIME, a1, a2));
	case __NR_nanosleep: return __pachaos_status(__pachaos_raw2(PACHAOS_SYSCALL_NANOSLEEP, a1, a2));
	case __NR_fstat: return __pachaos_status(__pachaos_raw2(PACHAOS_SYSCALL_FD_STAT, a1, a2));
	case __NR_timerfd_gettime: return __pachaos_status(__pachaos_raw2(PACHAOS_SYSCALL_TIMERFD_GETTIME, a1, a2));
	case __NR_eventfd2:
		if (a2 & ~(LINUX_EFD_SEMAPHORE|LINUX_EFD_CLOEXEC|LINUX_EFD_NONBLOCK)) return -22;
		if (a2 & LINUX_EFD_SEMAPHORE) return -38;
		return __pachaos_raw3(PACHAOS_SYSCALL_EVENTFD_CREATE, a1,
			PACHAOS_FD_RIGHT_INSPECT|PACHAOS_FD_RIGHT_WAIT|PACHAOS_FD_RIGHT_POLL|PACHAOS_FD_RIGHT_CLOSE|PACHAOS_FD_RIGHT_READ|PACHAOS_FD_RIGHT_WRITE,
			__pachaos_fd_flags_from_linux(a2, LINUX_EFD_CLOEXEC, LINUX_EFD_NONBLOCK));
	case __NR_timerfd_create:
		if (a1 != LINUX_CLOCK_MONOTONIC) return -22;
		if (a2 & ~(LINUX_TFD_CLOEXEC|LINUX_TFD_NONBLOCK)) return -22;
		return __pachaos_raw6(PACHAOS_SYSCALL_TIMERFD_CREATE, a1, 0, 0, 0,
			PACHAOS_FD_RIGHT_INSPECT|PACHAOS_FD_RIGHT_WAIT|PACHAOS_FD_RIGHT_POLL|PACHAOS_FD_RIGHT_CLOSE|PACHAOS_FD_RIGHT_READ|PACHAOS_FD_RIGHT_WRITE,
			__pachaos_fd_flags_from_linux(a2, LINUX_TFD_CLOEXEC, LINUX_TFD_NONBLOCK));
	default: return -38;
	}
}

static __inline long __syscall3(long n, long a1, long a2, long a3)
{
	switch (n) {
	case __NR_read: return __pachaos_raw3(PACHAOS_SYSCALL_FD_READ, a1, a2, a3);
	case __NR_write: return __pachaos_raw3(PACHAOS_SYSCALL_FD_WRITE, a1, a2, a3);
	case __NR_readv: return __pachaos_raw3(PACHAOS_SYSCALL_FD_READV, a1, a2, a3);
	case __NR_writev: return __pachaos_raw3(PACHAOS_SYSCALL_FD_WRITEV, a1, a2, a3);
	case __NR_ioctl: return __pachaos_status(__pachaos_raw3(PACHAOS_SYSCALL_FD_IOCTL, a1, a2, a3));
	case __NR_poll: return __pachaos_poll((struct __pachaos_linux_pollfd *)a1, a2, a3);
	case __NR_mprotect: return __pachaos_status(__pachaos_raw3(PACHAOS_SYSCALL_MPROTECT, a1, a2, __pachaos_prot(a3)));
	case __NR_open: return -38;
	default: return -38;
	}
}

static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
	switch (n) {
	case __NR_clock_nanosleep:
		if (a2 != 0) return -38;
		return __pachaos_status(__pachaos_raw2(PACHAOS_SYSCALL_NANOSLEEP, a3, a4));
	case __NR_futex:
		if ((a2 & 127) == LINUX_FUTEX_WAIT) return __pachaos_status(__pachaos_raw4(PACHAOS_SYSCALL_FUTEX_WAIT, a1, a3, a4, 0));
		if ((a2 & 127) == LINUX_FUTEX_WAKE) return __pachaos_raw2(PACHAOS_SYSCALL_FUTEX_WAKE, a1, a3);
		return -38;
	case __NR_fstatat:
		if ((a4 & LINUX_AT_EMPTY_PATH) && a2 && *(const char *)a2 == 0) {
			return __pachaos_status(__pachaos_raw2(PACHAOS_SYSCALL_FD_STAT, a1, a3));
		}
		return -38;
	case __NR_timerfd_settime:
		if (a2 & ~LINUX_TFD_TIMER_ABSTIME) return -22;
		return __pachaos_status(__pachaos_raw4(PACHAOS_SYSCALL_TIMERFD_SETTIME, a1, a2, a3, a4));
	default: return -38;
	}
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	return __syscall4(n, a1, a2, a3, a4);
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	switch (n) {
	case __NR_mmap:
		return __pachaos_mmap_result(__pachaos_raw6(PACHAOS_SYSCALL_MMAP, a5 < 0 ? 0 : a5, a1, a2, __pachaos_prot(a3), __pachaos_mmap_flags(a4), a6));
	case __NR_close:
		return __syscall1(n, a1);
	case __NR_munmap:
	case __NR_clock_gettime:
	case __NR_nanosleep:
	case __NR_fstat:
	case __NR_timerfd_gettime:
	case __NR_eventfd2:
	case __NR_timerfd_create:
		return __syscall2(n, a1, a2);
	case __NR_read:
	case __NR_write:
	case __NR_readv:
	case __NR_writev:
	case __NR_ioctl:
	case __NR_poll:
	case __NR_open:
	case __NR_mprotect:
		return __syscall3(n, a1, a2, a3);
	case __NR_futex:
	case __NR_fstatat:
	case __NR_clock_nanosleep:
	case __NR_timerfd_settime:
		return __syscall4(n, a1, a2, a3, a4);
	default: return -38;
	}
}

static __inline long __syscall7(long n, long a1, long a2, long a3, long a4, long a5, long a6, long a7)
{
	(void)a7;
	return __syscall6(n, a1, a2, a3, a4, a5, a6);
}

#define IPC_64 0
