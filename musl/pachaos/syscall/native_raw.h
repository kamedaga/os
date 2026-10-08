/* SPDX-License-Identifier: MIT */
#ifndef PACHAOS_NATIVE_RAW_H
#define PACHAOS_NATIVE_RAW_H
#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

#define PACHAOS_SYSCALL_LOG 1
#define PACHAOS_SYSCALL_PROCESS_EXIT 6
#define PACHAOS_SYSCALL_THREAD_CONTEXT 9
#define PACHAOS_SYSCALL_PROCESS_SIGNAL 14
#define PACHAOS_SYSCALL_PROCESS_SIGNAL_CTL 15
#define PACHAOS_SYSCALL_PROCESS_STOP 16
#define PACHAOS_SYSCALL_PROCESS_CONTINUE 17
#define PACHAOS_SYSCALL_THREAD_SET_FS_BASE 18
#define PACHAOS_SYSCALL_THREAD_SET_GS_BASE 19
#define PACHAOS_SYSCALL_PROCESS_CLONE 20
#define PACHAOS_SYSCALL_PROCESS_MAP 21
#define PACHAOS_SYSCALL_PROCESS_MAP_BATCH 22
#define PACHAOS_SYSCALL_PROCESS_UNMAP 23
#define PACHAOS_SYSCALL_PROCESS_EXEC_FROM 24
#define PACHAOS_SYSCALL_PROCESS_MEMORY_BARRIER 25
#define PACHAOS_PROCESS_MAP_ANYWHERE 0xffffffffffffffffULL
#define PACHAOS_SYSCALL_GETPID 26
#define PACHAOS_SYSCALL_GETTID 27
#define PACHAOS_SYSCALL_SYSTEM_INFO 28
#define PACHAOS_SYSCALL_CLOCK_READ 29
#define PACHAOS_SYSCALL_CLOCK_GETTIME 30
#define PACHAOS_SYSCALL_CLOCK_GETRES 31
#define PACHAOS_SYSCALL_CLOCK_PAGE_OPEN 32
#define PACHAOS_SYSCALL_YIELD 33
#define PACHAOS_SYSCALL_NANOSLEEP 34
#define PACHAOS_SYSCALL_FUTEX_WAIT 35
#define PACHAOS_SYSCALL_FUTEX_WAKE 36
#define PACHAOS_SYSCALL_FUTEX_WAKE_WAIT 37
#define PACHAOS_SYSCALL_FUTEX_REQUEUE 38
#define PACHAOS_SYSCALL_GETRANDOM 39
#define PACHAOS_SYSCALL_POWER_CONTROL 40
#define PACHAOS_SYSCALL_FD_TABLE 41
#define PACHAOS_SYSCALL_FD_CLOSE 42
#define PACHAOS_SYSCALL_FD_DUP 43
#define PACHAOS_SYSCALL_FD_GET_INFO 44
#define PACHAOS_SYSCALL_FD_READ 46
#define PACHAOS_SYSCALL_FD_WRITE 47
#define PACHAOS_SYSCALL_FD_READV 48
#define PACHAOS_SYSCALL_FD_WRITEV 49
#define PACHAOS_SYSCALL_FD_FCNTL 50
#define PACHAOS_SYSCALL_FD_POLL 51
#define PACHAOS_SYSCALL_FD_WAIT_MANY 52
#define PACHAOS_SYSCALL_FD_IOCTL 53
#define PACHAOS_SYSCALL_FD_STAT 54
#define PACHAOS_SYSCALL_EVENTFD_CREATE 55
#define PACHAOS_SYSCALL_PIPE_CREATE 56
#define PACHAOS_SYSCALL_TIMERFD_CREATE 57
#define PACHAOS_SYSCALL_TIMERFD_SETTIME 58
#define PACHAOS_SYSCALL_TIMERFD_GETTIME 59
#define PACHAOS_SYSCALL_VMO_CREATE 60
#define PACHAOS_SYSCALL_VMO_GROW 61
#define PACHAOS_SYSCALL_VMO_CACHE_POLICY 62
#define PACHAOS_SYSCALL_VMO_CREATE_PAGE_VIEW 63
#define PACHAOS_SYSCALL_VMO_REVOKE 64
#define PACHAOS_SYSCALL_MMAP 65
#define PACHAOS_SYSCALL_MUNMAP 66
#define PACHAOS_SYSCALL_MPROTECT 67
#define PACHAOS_SYSCALL_MREMAP 68
#define PACHAOS_SYSCALL_MADVISE 69
#define PACHAOS_SYSCALL_IPC_ENDPOINT_CREATE 70
#define PACHAOS_SYSCALL_IPC_CHANNEL_CREATE 71
#define PACHAOS_SYSCALL_IPC_SEND 72
#define PACHAOS_SYSCALL_IPC_RECV 73
#define PACHAOS_SYSCALL_IPC_CALL 74
#define PACHAOS_SYSCALL_IPC_REPLY 75
#define PACHAOS_SYSCALL_IPC_RECV_WAIT 76

#define PACHAOS_FD_FLAG_CLOEXEC 1
#define PACHAOS_FD_FLAG_NONBLOCK 2
#define PACHAOS_FD_RIGHT_INSPECT (1ULL << 0)
#define PACHAOS_FD_RIGHT_TRANSFER (1ULL << 2)
#define PACHAOS_FD_RIGHT_SET_FLAGS (1ULL << 5)
#define PACHAOS_FD_RIGHT_WAIT (1ULL << 3)
#define PACHAOS_FD_RIGHT_POLL (1ULL << 4)
#define PACHAOS_FD_RIGHT_CLOSE (1ULL << 6)
#define PACHAOS_FD_RIGHT_SEND (1ULL << 7)
#define PACHAOS_FD_RIGHT_RECV (1ULL << 8)
#define PACHAOS_FD_RIGHT_CALL (1ULL << 9)
#define PACHAOS_FD_RIGHT_READ (1ULL << 43)
#define PACHAOS_FD_RIGHT_WRITE (1ULL << 44)
#define PACHAOS_FD_RIGHT_MAP_READ (1ULL << 13)
#define PACHAOS_FD_RIGHT_MAP_WRITE (1ULL << 14)
#define PACHAOS_FD_RIGHT_MAP_EXEC (1ULL << 15)

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
#define LINUX_MREMAP_MAYMOVE 1
#define LINUX_MREMAP_FIXED 2
#define LINUX_MREMAP_DONTUNMAP 4
#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1
#define LINUX_FUTEX_REQUEUE 3
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100
#define LINUX_AT_REMOVEDIR 0x200
#define LINUX_AT_EMPTY_PATH 0x1000
#define LINUX_F_OK 0
#define LINUX_X_OK 1
#define LINUX_W_OK 2
#define LINUX_R_OK 4
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
#define LINUX_AT_FDCWD (-100)
#define LINUX_O_ACCMODE 03
#define LINUX_O_WRONLY 01
#define LINUX_O_RDWR 02
#define LINUX_O_CREAT 0100
#define LINUX_O_EXCL 0200
#define LINUX_O_TRUNC 01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000
#define LINUX_O_LARGEFILE 0100000
#define LINUX_O_DIRECTORY 0200000
#define LINUX_O_CLOEXEC 02000000
#define LINUX_F_DUPFD 0
#define LINUX_F_GETFD 1
#define LINUX_F_SETFD 2
#define LINUX_F_GETFL 3
#define LINUX_F_SETFL 4
#define LINUX_F_DUPFD_CLOEXEC 1030
#define LINUX_FD_CLOEXEC 1

#define PACHAOS_POLL_READABLE 1
#define PACHAOS_POLL_WRITABLE 2
#define PACHAOS_POLL_ERROR 4
#define PACHAOS_POLL_HANGUP 8
#define PACHAOS_BRK_RESERVE_BYTES (256ULL * 1024ULL * 1024ULL)

extern unsigned char *__pachaos_brk_base;
extern unsigned char *__pachaos_brk_cur;
extern unsigned char *__pachaos_brk_limit;
extern int __pachaos_brk_lock;

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

struct __pachaos_ipc_fd {
	unsigned long long fd;
	unsigned long long rights;
	unsigned long long flags;
	unsigned long long transfer_flags;
};

struct __pachaos_ipc_msg {
	unsigned long long word0;
	unsigned long long word1;
	unsigned long long word2;
	unsigned long long word3;
	struct __pachaos_ipc_fd *fds;
	unsigned long long fd_count;
	unsigned long long fd_capacity;
	unsigned long long flags;
};

struct __pachaos_kstat {
	unsigned long long st_dev;
	unsigned long long st_ino;
	unsigned long long st_nlink;
	unsigned int st_mode;
	unsigned int st_uid;
	unsigned int st_gid;
	unsigned int __pad0;
	unsigned long long st_rdev;
	long long st_size;
	long long st_blksize;
	long long st_blocks;
	long st_atime_sec;
	long st_atime_nsec;
	long st_mtime_sec;
	long st_mtime_nsec;
	long st_ctime_sec;
	long st_ctime_nsec;
	long __unused[3];
};

struct __pachaos_iovec {
	void *base;
	unsigned long len;
};

#if defined(PACHAOS_NATIVE_TEST) || defined(PACHAOS_NATIVE_RAW_TEST)
extern long native_test_raw(long,long,long,long,long,long,long);
static __inline long __pachaos_raw0(long n) { return native_test_raw(n,0,0,0,0,0,0); }
static __inline long __pachaos_raw1(long n,long a) { return native_test_raw(n,a,0,0,0,0,0); }
static __inline long __pachaos_raw2(long n,long a,long b) { return native_test_raw(n,a,b,0,0,0,0); }
static __inline long __pachaos_raw3(long n,long a,long b,long c) { return native_test_raw(n,a,b,c,0,0,0); }
static __inline long __pachaos_raw4(long n,long a,long b,long c,long d) { return native_test_raw(n,a,b,c,d,0,0); }
static __inline long __pachaos_raw5(long n,long a,long b,long c,long d,long e) { return native_test_raw(n,a,b,c,d,e,0); }
static __inline long __pachaos_raw6(long n,long a,long b,long c,long d,long e,long f) { return native_test_raw(n,a,b,c,d,e,f); }
#else
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

static __inline long __pachaos_raw5(long n, long a1, long a2, long a3, long a4, long a5)
{
	unsigned long ret;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	__asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
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

#endif
static __inline void __pachaos_bzero(void *ptr, long len)
{
	unsigned char *p = (unsigned char *)ptr;
	for (long i = 0; i < len; i++) p[i] = 0;
}

static __inline unsigned short __pachaos_rd16(const unsigned char *p)
{
	return (unsigned short)p[0] | ((unsigned short)p[1] << 8);
}

static __inline unsigned int __pachaos_rd32(const unsigned char *p)
{
	return (unsigned int)p[0] |
		((unsigned int)p[1] << 8) |
		((unsigned int)p[2] << 16) |
		((unsigned int)p[3] << 24);
}

static __inline unsigned long long __pachaos_rd64(const unsigned char *p)
{
	unsigned long long lo = __pachaos_rd32(p);
	unsigned long long hi = __pachaos_rd32(p + 4);
	return lo | (hi << 32);
}

static __inline unsigned long long __pachaos_page_align_up(unsigned long long value)
{
	return (value + 4095ULL) & ~4095ULL;
}

static __inline unsigned long long __pachaos_elf_mapping_size(const unsigned char *bytes, unsigned long long got, unsigned long long file_size)
{
	if (got < 64) return __pachaos_page_align_up(file_size);
	if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
		return __pachaos_page_align_up(file_size);
	}
	if (bytes[4] != 2 || bytes[5] != 1) return __pachaos_page_align_up(file_size);
	unsigned long long phoff = __pachaos_rd64(bytes + 32);
	unsigned short phentsize = __pachaos_rd16(bytes + 54);
	unsigned short phnum = __pachaos_rd16(bytes + 56);
	if (phentsize < 56 || phnum == 0) return __pachaos_page_align_up(file_size);
	if (phoff > got || (unsigned long long)phentsize * phnum > got - phoff) {
		return __pachaos_page_align_up(file_size);
	}
	unsigned long long max_end = file_size;
	for (unsigned int i = 0; i < phnum; i++) {
		const unsigned char *ph = bytes + phoff + (unsigned long long)i * phentsize;
		if (__pachaos_rd32(ph) != 1) continue;
		unsigned long long vaddr = __pachaos_rd64(ph + 16);
		unsigned long long memsz = __pachaos_rd64(ph + 40);
		if (vaddr + memsz > max_end) max_end = vaddr + memsz;
	}
	return __pachaos_page_align_up(max_end);
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

static __inline long __pachaos_brk(long requested)
{
	while (__sync_lock_test_and_set(&__pachaos_brk_lock, 1)) {
		__asm__ __volatile__("pause");
	}

	if (!__pachaos_brk_base) {
		long mapped = __pachaos_raw6(
			PACHAOS_SYSCALL_MMAP,
			0,
			0,
			(long)PACHAOS_BRK_RESERVE_BYTES,
			0,
			PACHAOS_MMAP_PRIVATE|PACHAOS_MMAP_ANONYMOUS|PACHAOS_MMAP_NORESERVE,
			0);
		if (mapped >= 4096) {
			__pachaos_brk_base = (unsigned char *)mapped;
			__pachaos_brk_cur = __pachaos_brk_base;
			__pachaos_brk_limit = __pachaos_brk_base + PACHAOS_BRK_RESERVE_BYTES;
		}
	}

	long result = (long)__pachaos_brk_cur;
	if (__pachaos_brk_base && requested != 0) {
		unsigned char *want = (unsigned char *)requested;
		if (want >= __pachaos_brk_base && want <= __pachaos_brk_limit) {
			unsigned long old_page = __pachaos_page_align_up((unsigned long)__pachaos_brk_cur);
			unsigned long new_page = __pachaos_page_align_up((unsigned long)want);
			if (want > __pachaos_brk_cur) {
				if (new_page > old_page) {
					long status = __pachaos_raw3(
						PACHAOS_SYSCALL_MPROTECT,
						(long)old_page,
						(long)(new_page - old_page),
						PACHAOS_PROT_READ|PACHAOS_PROT_WRITE);
					if (status != 0) {
						__sync_lock_release(&__pachaos_brk_lock);
						return result;
					}
				}
			} else if (want < __pachaos_brk_cur) {
				if (old_page > new_page) {
					long status = __pachaos_raw3(
						PACHAOS_SYSCALL_MPROTECT,
						(long)new_page,
						(long)(old_page - new_page),
						0);
					if (status != 0) {
						__sync_lock_release(&__pachaos_brk_lock);
						return result;
					}
				}
			}
			__pachaos_brk_cur = want;
			result = requested;
		}
	}

	__sync_lock_release(&__pachaos_brk_lock);
	return result;
}

static __inline long __pachaos_madvise(long addr, long len, long advice)
{
	switch (advice) {
	case 0:  /* MADV_NORMAL */
	case 1:  /* MADV_RANDOM */
	case 2:  /* MADV_SEQUENTIAL */
	case 3:  /* MADV_WILLNEED */
	case 4:  /* MADV_DONTNEED */
	case 8:  /* MADV_FREE */
	case 14: /* MADV_HUGEPAGE */
	case 15: /* MADV_NOHUGEPAGE */
	case 16: /* MADV_DONTDUMP */
	case 17: /* MADV_DODUMP */
	case 20: /* MADV_COLD */
	case 21: /* MADV_PAGEOUT */
		return __pachaos_status(__pachaos_raw3(PACHAOS_SYSCALL_MADVISE, addr, len, advice));
	default:
		return -22;
	}
}

static __inline long __pachaos_mremap(long old_addr, long old_len, long new_len, long flags, long new_addr)
{
	if (flags & ~(LINUX_MREMAP_MAYMOVE|LINUX_MREMAP_FIXED)) return -22;
	if ((flags & LINUX_MREMAP_FIXED) && !(flags & LINUX_MREMAP_MAYMOVE)) return -22;
	if (!(flags & LINUX_MREMAP_FIXED) && new_addr != 0) return -22;
	return __pachaos_mmap_result(__pachaos_raw5(PACHAOS_SYSCALL_MREMAP, old_addr, old_len, new_len, flags, new_addr));
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

#endif
