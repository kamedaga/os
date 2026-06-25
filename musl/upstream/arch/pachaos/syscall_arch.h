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
#define PACHAOS_SYSCALL_VMO_CREATE 38
#define PACHAOS_SYSCALL_MMAP 39
#define PACHAOS_SYSCALL_MUNMAP 40
#define PACHAOS_SYSCALL_MPROTECT 41
#define PACHAOS_SYSCALL_IPC_RECV 45
#define PACHAOS_SYSCALL_IPC_CALL 46

#define PACHAOS_FD_FLAG_CLOEXEC 1
#define PACHAOS_FD_FLAG_NONBLOCK 2
#define PACHAOS_FD_RIGHT_INSPECT (1ULL << 0)
#define PACHAOS_FD_RIGHT_TRANSFER (1ULL << 2)
#define PACHAOS_FD_RIGHT_SET_FLAGS (1ULL << 5)
#define PACHAOS_FD_RIGHT_WAIT (1ULL << 3)
#define PACHAOS_FD_RIGHT_POLL (1ULL << 4)
#define PACHAOS_FD_RIGHT_CLOSE (1ULL << 6)
#define PACHAOS_FD_RIGHT_CALL (1ULL << 9)
#define PACHAOS_FD_RIGHT_READ (1ULL << 42)
#define PACHAOS_FD_RIGHT_WRITE (1ULL << 43)
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
#define LINUX_AT_FDCWD (-100)
#define LINUX_O_ACCMODE 03
#define LINUX_O_WRONLY 01
#define LINUX_O_RDWR 02
#define LINUX_O_CREAT 0100
#define LINUX_O_TRUNC 01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000
#define LINUX_O_DIRECTORY 0200000
#define LINUX_O_CLOEXEC 02000000

#define PACHAOS_POLL_READABLE 1
#define PACHAOS_POLL_WRITABLE 2
#define PACHAOS_POLL_ERROR 4
#define PACHAOS_POLL_HANGUP 8
#define PACHAOS_MAX_POLLFDS 64
#define PACHAOS_FILED_ENDPOINT_FD 240
#define PACHAOS_FILED_FD_BASE 1024
#define PACHAOS_FILED_FD_CAP 128
#define PACHAOS_FILED_PAGE_BYTES 8192
#define PACHAOS_FILED_NAME_BYTES 96
#define PACHAOS_FILED_IO_BYTES 7680
#define PACHAOS_FILED_DIRENT_NAME_BYTES 96
#define PACHAOS_FILED_DIRENT_CAPACITY 16
#define PACHAOS_FILED_REQUEST_MAGIC 0x31465152444c4946ULL
#define PACHAOS_FILED_REPLY_MAGIC 0x31595052444c4946ULL
#define PACHAOS_FILED_OP_OPENAT 4
#define PACHAOS_FILED_OP_STAT 5
#define PACHAOS_FILED_OP_PREAD 6
#define PACHAOS_FILED_OP_GETDENTS 7
#define PACHAOS_FILED_OP_CLOSE 8
#define PACHAOS_FILED_OP_EXEC_PATH 9
#define PACHAOS_FILED_OP_READ 10
#define PACHAOS_FILED_OP_WRITE 15
#define PACHAOS_FILED_RIGHT_LOOKUP (1U << 0)
#define PACHAOS_FILED_RIGHT_READ (1U << 1)
#define PACHAOS_FILED_RIGHT_WRITE (1U << 2)
#define PACHAOS_FILED_RIGHT_EXEC (1U << 3)
#define PACHAOS_FILED_RIGHT_STAT (1U << 4)
#define PACHAOS_FILED_RIGHT_GETDENTS (1U << 5)
#define PACHAOS_FILED_RIGHT_CREATE (1U << 6)
#define PACHAOS_FILED_OPEN_CREATE (1U << 0)
#define PACHAOS_FILED_OPEN_TRUNCATE (1U << 2)
#define PACHAOS_FILED_OPEN_DIRECTORY (1U << 3)
#define PACHAOS_FILED_OPEN_CLOEXEC (1U << 5)
#define PACHAOS_FILED_OPEN_APPEND (1U << 6)
#define PACHAOS_FILED_OPEN_NONBLOCK (1U << 7)
#define PACHAOS_FILED_EXEC_MAX_ARGS 8
#define PACHAOS_FILED_EXEC_MAX_ENVS 8
#define PACHAOS_FILED_EXEC_ARG_BYTES 128
#define PACHAOS_FILED_EXEC_ENV_BYTES 128

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

struct __pachaos_filed_openat {
	unsigned long long dir_handle;
	unsigned long long rights;
	unsigned long long open_flags;
	char name[PACHAOS_FILED_NAME_BYTES];
};

struct __pachaos_filed_io {
	unsigned long long handle;
	unsigned long long offset;
	unsigned long long length;
	unsigned char data[PACHAOS_FILED_IO_BYTES];
};

struct __pachaos_filed_statx {
	unsigned long long handle;
	unsigned long long mode;
	unsigned long long size;
	unsigned long long blocks;
	unsigned long long nlink;
	unsigned long long kind;
};

struct __pachaos_filed_dirent {
	unsigned long long handle;
	unsigned long long kind;
	unsigned long long name_len;
	char name[PACHAOS_FILED_DIRENT_NAME_BYTES];
};

struct __pachaos_filed_getdents {
	unsigned long long dir_handle;
	unsigned long long offset;
	unsigned long long capacity;
	unsigned long long count;
	struct __pachaos_filed_dirent entries[PACHAOS_FILED_DIRENT_CAPACITY];
};

struct __pachaos_filed_exec_fd_patch {
	unsigned long long kind;
	unsigned long long index;
	unsigned long long offset;
	unsigned long long reserved0;
};

struct __pachaos_filed_exec_path {
	unsigned long long dir_handle;
	unsigned long long flags;
	unsigned long long inherit_fd_count;
	unsigned long long fd_patch_count;
	unsigned long long inherit_handle_count;
	unsigned long long reserved1;
	unsigned long long argc;
	unsigned long long envc;
	unsigned long long inherit_handles[4];
	struct __pachaos_filed_exec_fd_patch fd_patches[4];
	char path[PACHAOS_FILED_NAME_BYTES];
	char argv0[PACHAOS_FILED_NAME_BYTES];
	char argv[PACHAOS_FILED_EXEC_MAX_ARGS][PACHAOS_FILED_EXEC_ARG_BYTES];
	char envp[PACHAOS_FILED_EXEC_MAX_ENVS][PACHAOS_FILED_EXEC_ENV_BYTES];
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

struct __pachaos_dirent {
	unsigned long long d_ino;
	long long d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[256];
};

struct __pachaos_filed_fd_entry {
	unsigned char used;
	unsigned long long handle;
};

extern struct __pachaos_filed_fd_entry __pachaos_filed_fds[PACHAOS_FILED_FD_CAP];
extern unsigned long long __pachaos_filed_request_id;

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

static __inline void __pachaos_bzero(void *ptr, long len)
{
	unsigned char *p = (unsigned char *)ptr;
	for (long i = 0; i < len; i++) p[i] = 0;
}

static __inline long __pachaos_copy_path(char *dst, const char *src, long cap)
{
	if (!dst || !src || cap <= 0) return -22;
	long i = 0;
	for (; i < cap - 1; i++) {
		char c = src[i];
		dst[i] = c;
		if (!c) return 0;
	}
	dst[i] = 0;
	return src[i] ? -36 : 0;
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

static __inline long __pachaos_filed_call(long op, long word2, long request_id, long page_fd, struct __pachaos_ipc_msg *reply)
{
	struct __pachaos_ipc_fd request_fd;
	struct __pachaos_ipc_msg request;
	__pachaos_bzero(&request_fd, sizeof request_fd);
	__pachaos_bzero(&request, sizeof request);
	__pachaos_bzero(reply, sizeof *reply);

	request_fd.fd = page_fd;
	request_fd.rights =
		PACHAOS_FD_RIGHT_CLOSE |
		PACHAOS_FD_RIGHT_MAP_READ |
		PACHAOS_FD_RIGHT_MAP_WRITE;
	request.word0 = PACHAOS_FILED_REQUEST_MAGIC;
	request.word1 = op;
	request.word2 = word2;
	request.word3 = request_id;
	request.fds = page_fd >= 16 ? &request_fd : 0;
	request.fd_count = page_fd >= 16 ? 1 : 0;

	long reply_fd = __pachaos_raw2(PACHAOS_SYSCALL_IPC_CALL, PACHAOS_FILED_ENDPOINT_FD, (long)&request);
	if (reply_fd < 16) return -9;
	long status = -22;
	for (;;) {
		status = __pachaos_raw2(PACHAOS_SYSCALL_IPC_RECV, reply_fd, (long)reply);
		if (status == 0) break;
		if (status != 2 && status != -2 && status != 5 && status != -5) break;
		struct __pachaos_pollfd wait_fd;
		wait_fd.fd = reply_fd;
		wait_fd.events = PACHAOS_POLL_READABLE;
		wait_fd.revents = 0;
		long wait_status = __pachaos_raw4(PACHAOS_SYSCALL_FD_WAIT_MANY, (long)&wait_fd, 1, 1, 0);
		if (wait_status != 0 && wait_status != 2 && wait_status != -2 && wait_status != 5 && wait_status != -5) {
			status = wait_status;
			break;
		}
	}
	(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, reply_fd);
	if (status != 0) return -5;
	if (reply->word0 != PACHAOS_FILED_REPLY_MAGIC || reply->word3 != (unsigned long long)request_id) return -71;
	return (long)reply->word1;
}

static __inline long __pachaos_filed_next_request_id(void)
{
	unsigned long long id = __pachaos_filed_request_id++;
	if (__pachaos_filed_request_id == 0) __pachaos_filed_request_id = 100;
	return (long)id;
}

static __inline long __pachaos_filed_page_create(long *out_fd, unsigned char **out_page)
{
	long page_fd = __pachaos_raw3(
		PACHAOS_SYSCALL_VMO_CREATE,
		PACHAOS_FILED_PAGE_BYTES,
		PACHAOS_FD_RIGHT_TRANSFER|PACHAOS_FD_RIGHT_CLOSE|PACHAOS_FD_RIGHT_MAP_READ|PACHAOS_FD_RIGHT_MAP_WRITE,
		0);
	if (page_fd < 16) return -23;
	long page_addr = __pachaos_raw6(
		PACHAOS_SYSCALL_MMAP,
		page_fd,
		0,
		PACHAOS_FILED_PAGE_BYTES,
		PACHAOS_PROT_READ|PACHAOS_PROT_WRITE,
		PACHAOS_MMAP_SHARED,
		0);
	if (page_addr < 4096) {
		(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, page_fd);
		return -14;
	}
	*out_fd = page_fd;
	*out_page = (unsigned char *)page_addr;
	return 0;
}

static __inline void __pachaos_filed_page_destroy(long page_fd, unsigned char *page)
{
	if (page && (unsigned long)page >= 4096) {
		(void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, (long)page, PACHAOS_FILED_PAGE_BYTES);
	}
	if (page_fd >= 16) {
		(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, page_fd);
	}
}

static __inline int __pachaos_filed_fd_index(long fd)
{
	if (fd < PACHAOS_FILED_FD_BASE || fd >= PACHAOS_FILED_FD_BASE + PACHAOS_FILED_FD_CAP) return -1;
	int idx = (int)(fd - PACHAOS_FILED_FD_BASE);
	return __pachaos_filed_fds[idx].used ? idx : -1;
}

static __inline long __pachaos_filed_fd_alloc(unsigned long long handle)
{
	if (!handle) return -22;
	for (int i = 0; i < PACHAOS_FILED_FD_CAP; i++) {
		if (!__pachaos_filed_fds[i].used) {
			__pachaos_filed_fds[i].used = 1;
			__pachaos_filed_fds[i].handle = handle;
			return PACHAOS_FILED_FD_BASE + i;
		}
	}
	return -24;
}

static __inline unsigned long long __pachaos_filed_fd_handle(long fd)
{
	int idx = __pachaos_filed_fd_index(fd);
	return idx < 0 ? 0 : __pachaos_filed_fds[idx].handle;
}

static __inline void __pachaos_filed_close_handle(unsigned long long handle, long page_fd)
{
	if (!handle) return;
	struct __pachaos_ipc_msg reply;
	(void)__pachaos_filed_call(PACHAOS_FILED_OP_CLOSE, (long)handle, __pachaos_filed_next_request_id(), -1, &reply);
	(void)page_fd;
}

static __inline unsigned int __pachaos_filed_rights_from_linux(long flags)
{
	unsigned int rights = PACHAOS_FILED_RIGHT_STAT;
	switch (flags & LINUX_O_ACCMODE) {
	case LINUX_O_WRONLY:
		rights |= PACHAOS_FILED_RIGHT_WRITE|PACHAOS_FILED_RIGHT_CREATE;
		break;
	case LINUX_O_RDWR:
		rights |= PACHAOS_FILED_RIGHT_READ|PACHAOS_FILED_RIGHT_WRITE|PACHAOS_FILED_RIGHT_CREATE;
		break;
	default:
		rights |= PACHAOS_FILED_RIGHT_READ|PACHAOS_FILED_RIGHT_EXEC;
		break;
	}
	if (flags & LINUX_O_DIRECTORY) rights |= PACHAOS_FILED_RIGHT_GETDENTS|PACHAOS_FILED_RIGHT_READ;
	if (flags & LINUX_O_DIRECTORY) rights |= PACHAOS_FILED_RIGHT_LOOKUP;
	if (flags & LINUX_O_CREAT) rights |= PACHAOS_FILED_RIGHT_CREATE|PACHAOS_FILED_RIGHT_WRITE;
	return rights;
}

static __inline unsigned int __pachaos_filed_open_flags_from_linux(long flags)
{
	unsigned int out = 0;
	if (flags & LINUX_O_CREAT) out |= PACHAOS_FILED_OPEN_CREATE;
	if (flags & LINUX_O_TRUNC) out |= PACHAOS_FILED_OPEN_TRUNCATE;
	if (flags & LINUX_O_DIRECTORY) out |= PACHAOS_FILED_OPEN_DIRECTORY;
	if (flags & LINUX_O_APPEND) out |= PACHAOS_FILED_OPEN_APPEND;
	if (flags & LINUX_O_NONBLOCK) out |= PACHAOS_FILED_OPEN_NONBLOCK;
	if (flags & LINUX_O_CLOEXEC) out |= PACHAOS_FILED_OPEN_CLOEXEC;
	return out;
}

static __inline long __pachaos_openat_filed_fd(long dirfd, const char *path, long flags)
{
	if (!path) return -22;
	long page_fd = -1;
	unsigned char *page = 0;
	long status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;

	__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
	struct __pachaos_filed_openat *openat = (struct __pachaos_filed_openat *)page;
	if (path[0] == '/') {
		openat->dir_handle = 0;
	} else if (dirfd == LINUX_AT_FDCWD) {
		openat->dir_handle = 0;
	} else {
		openat->dir_handle = __pachaos_filed_fd_handle(dirfd);
		if (!openat->dir_handle) {
			__pachaos_filed_page_destroy(page_fd, page);
			return -95;
		}
	}
	openat->rights = __pachaos_filed_rights_from_linux(flags);
	openat->open_flags = __pachaos_filed_open_flags_from_linux(flags);
	status = __pachaos_copy_path(openat->name, path, PACHAOS_FILED_NAME_BYTES);
	if (status != 0) {
		__pachaos_filed_page_destroy(page_fd, page);
		return status;
	}

	struct __pachaos_ipc_msg reply;
	status = __pachaos_filed_call(PACHAOS_FILED_OP_OPENAT, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
	__pachaos_filed_page_destroy(page_fd, page);
	if (status != 0) return status;
	return __pachaos_filed_fd_alloc(reply.word2);
}

static __inline long __pachaos_filed_read(long fd, void *buf, long len)
{
	unsigned long long handle = __pachaos_filed_fd_handle(fd);
	if (!handle) return __pachaos_raw3(PACHAOS_SYSCALL_FD_READ, fd, (long)buf, len);
	if (len < 0 || (len > 0 && !buf)) return -22;
	long page_fd = -1;
	unsigned char *page = 0;
	long status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;
	long done = 0;
	while (done < len) {
		long want = len - done;
		if (want > PACHAOS_FILED_IO_BYTES) want = PACHAOS_FILED_IO_BYTES;
		__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
		struct __pachaos_filed_io *io = (struct __pachaos_filed_io *)page;
		io->handle = handle;
		io->length = (unsigned long long)want;
		struct __pachaos_ipc_msg reply;
		status = __pachaos_filed_call(PACHAOS_FILED_OP_READ, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
		if (status != 0) break;
		long got = (long)reply.word2;
		if (got <= 0) break;
		if (got > want) got = want;
		for (long i = 0; i < got; i++) ((unsigned char *)buf)[done + i] = io->data[i];
		done += got;
		if (got < want) break;
	}
	__pachaos_filed_page_destroy(page_fd, page);
	return done > 0 ? done : status;
}

static __inline long __pachaos_filed_write(long fd, const void *buf, long len)
{
	unsigned long long handle = __pachaos_filed_fd_handle(fd);
	if (!handle) return __pachaos_raw3(PACHAOS_SYSCALL_FD_WRITE, fd, (long)buf, len);
	if (len < 0 || (len > 0 && !buf)) return -22;
	long page_fd = -1;
	unsigned char *page = 0;
	long status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;
	long done = 0;
	while (done < len) {
		long want = len - done;
		if (want > PACHAOS_FILED_IO_BYTES) want = PACHAOS_FILED_IO_BYTES;
		__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
		struct __pachaos_filed_io *io = (struct __pachaos_filed_io *)page;
		io->handle = handle;
		io->length = (unsigned long long)want;
		for (long i = 0; i < want; i++) io->data[i] = ((const unsigned char *)buf)[done + i];
		struct __pachaos_ipc_msg reply;
		status = __pachaos_filed_call(PACHAOS_FILED_OP_WRITE, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
		if (status != 0) break;
		long wrote = (long)reply.word2;
		if (wrote <= 0) break;
		if (wrote > want) wrote = want;
		done += wrote;
		if (wrote < want) break;
	}
	__pachaos_filed_page_destroy(page_fd, page);
	return done > 0 ? done : status;
}

static __inline long __pachaos_filed_close_fd(long fd)
{
	int idx = __pachaos_filed_fd_index(fd);
	if (idx < 0) {
		long status = __pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, fd);
		return status == 0 ? 0 : -22;
	}
	unsigned long long handle = __pachaos_filed_fds[idx].handle;
	__pachaos_filed_fds[idx].used = 0;
	__pachaos_filed_fds[idx].handle = 0;
	__pachaos_filed_close_handle(handle, -1);
	return 0;
}

static __inline unsigned char __pachaos_dtype_from_kind(unsigned long long kind)
{
	switch (kind & 0170000ULL) {
	case 0040000ULL: return 4;
	case 0100000ULL: return 8;
	case 0120000ULL: return 10;
	case 0010000ULL: return 1;
	case 0020000ULL: return 2;
	case 0060000ULL: return 6;
	default: return 0;
	}
}

static __inline long __pachaos_filed_fstat(long fd, void *kst)
{
	unsigned long long handle = __pachaos_filed_fd_handle(fd);
	if (!handle) {
		long status = __pachaos_raw2(PACHAOS_SYSCALL_FD_STAT, fd, (long)kst);
		return status == 0 ? 0 : -22;
	}
	if (!kst) return -22;
	long page_fd = -1;
	unsigned char *page = 0;
	long status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;
	__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
	struct __pachaos_filed_statx *st = (struct __pachaos_filed_statx *)page;
	st->handle = handle;
	struct __pachaos_ipc_msg reply;
	status = __pachaos_filed_call(PACHAOS_FILED_OP_STAT, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
	if (status == 0) {
		struct __pachaos_kstat *out = (struct __pachaos_kstat *)kst;
		__pachaos_bzero(out, sizeof(*out));
		out->st_ino = handle;
		out->st_nlink = st->nlink ? st->nlink : 1;
		out->st_mode = (unsigned int)st->mode;
		out->st_size = (long long)st->size;
		out->st_blksize = 4096;
		out->st_blocks = (long long)st->blocks;
	}
	__pachaos_filed_page_destroy(page_fd, page);
	return status;
}

static __inline long __pachaos_filed_getdents(long fd, void *buf, long len)
{
	unsigned long long handle = __pachaos_filed_fd_handle(fd);
	if (!handle) return -9;
	if (len < (long)sizeof(struct __pachaos_dirent) || !buf) return -22;
	long page_fd = -1;
	unsigned char *page = 0;
	long status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;
	__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
	struct __pachaos_filed_getdents *gd = (struct __pachaos_filed_getdents *)page;
	gd->dir_handle = handle;
	gd->capacity = PACHAOS_FILED_DIRENT_CAPACITY;
	struct __pachaos_ipc_msg reply;
	status = __pachaos_filed_call(PACHAOS_FILED_OP_GETDENTS, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
	long out_bytes = 0;
	if (status == 0) {
		unsigned long long count = gd->count;
		if (count > PACHAOS_FILED_DIRENT_CAPACITY) count = PACHAOS_FILED_DIRENT_CAPACITY;
		for (unsigned long long i = 0; i < count; i++) {
			if (out_bytes + (long)sizeof(struct __pachaos_dirent) > len) break;
			struct __pachaos_dirent *de = (struct __pachaos_dirent *)((unsigned char *)buf + out_bytes);
			__pachaos_bzero(de, sizeof(*de));
			de->d_ino = gd->entries[i].handle ? gd->entries[i].handle : (unsigned long long)(i + 1);
			de->d_off = (long long)(gd->offset + i + 1);
			de->d_reclen = (unsigned short)sizeof(*de);
			de->d_type = __pachaos_dtype_from_kind(gd->entries[i].kind);
			unsigned long long n = gd->entries[i].name_len;
			if (n >= sizeof(de->d_name)) n = sizeof(de->d_name) - 1;
			for (unsigned long long j = 0; j < n; j++) de->d_name[j] = gd->entries[i].name[j];
			de->d_name[n] = 0;
			out_bytes += (long)sizeof(*de);
		}
	}
	__pachaos_filed_page_destroy(page_fd, page);
	return status == 0 ? out_bytes : status;
}

static __inline long __pachaos_filed_copy_exec_string(char *dst, const char *src, long cap)
{
	return __pachaos_copy_path(dst, src ? src : "", cap);
}

static __inline long __pachaos_filed_execve(const char *path, char *const argv[], char *const envp[])
{
	if (!path) return -22;
	long page_fd = -1;
	unsigned char *page = 0;
	long status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;
	__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
	struct __pachaos_filed_exec_path *exec = (struct __pachaos_filed_exec_path *)page;
	status = __pachaos_copy_path(exec->path, path, PACHAOS_FILED_NAME_BYTES);
	if (status != 0) {
		__pachaos_filed_page_destroy(page_fd, page);
		return status;
	}
	const char *argv0 = (argv && argv[0]) ? argv[0] : path;
	status = __pachaos_filed_copy_exec_string(exec->argv0, argv0, PACHAOS_FILED_NAME_BYTES);
	if (status != 0) {
		__pachaos_filed_page_destroy(page_fd, page);
		return status;
	}
	unsigned long long argc = 0;
	if (argv) {
		while (argv[argc]) {
			if (argc >= PACHAOS_FILED_EXEC_MAX_ARGS) {
				__pachaos_filed_page_destroy(page_fd, page);
				return -7;
			}
			status = __pachaos_filed_copy_exec_string(exec->argv[argc], argv[argc], PACHAOS_FILED_EXEC_ARG_BYTES);
			if (status != 0) {
				__pachaos_filed_page_destroy(page_fd, page);
				return status;
			}
			argc++;
		}
	} else {
		status = __pachaos_filed_copy_exec_string(exec->argv[0], argv0, PACHAOS_FILED_EXEC_ARG_BYTES);
		if (status != 0) {
			__pachaos_filed_page_destroy(page_fd, page);
			return status;
		}
		argc = 1;
	}
	unsigned long long envc = 0;
	if (envp) {
		while (envp[envc]) {
			if (envc >= PACHAOS_FILED_EXEC_MAX_ENVS) {
				__pachaos_filed_page_destroy(page_fd, page);
				return -7;
			}
			status = __pachaos_filed_copy_exec_string(exec->envp[envc], envp[envc], PACHAOS_FILED_EXEC_ENV_BYTES);
			if (status != 0) {
				__pachaos_filed_page_destroy(page_fd, page);
				return status;
			}
			envc++;
		}
	}
	exec->argc = argc;
	exec->envc = envc;

	struct __pachaos_ipc_msg reply;
	status = __pachaos_filed_call(PACHAOS_FILED_OP_EXEC_PATH, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
	__pachaos_filed_page_destroy(page_fd, page);
	if (status != 0) return status;
	(void)__pachaos_raw1(PACHAOS_SYSCALL_PROCESS_EXIT, 0);
	for (;;) __asm__ __volatile__("pause");
}

static __inline long __pachaos_openat_file_vmo(long dirfd, const char *path, long flags)
{
	if (!path) return -22;
	if ((flags & LINUX_O_ACCMODE) != 0) return -95;
	if (flags & (LINUX_O_CREAT|LINUX_O_TRUNC|LINUX_O_APPEND|LINUX_O_DIRECTORY)) return -95;
	if (dirfd != LINUX_AT_FDCWD && path[0] != '/') return -95;

	long page_fd = __pachaos_raw3(
		PACHAOS_SYSCALL_VMO_CREATE,
		PACHAOS_FILED_PAGE_BYTES,
		PACHAOS_FD_RIGHT_TRANSFER|PACHAOS_FD_RIGHT_CLOSE|PACHAOS_FD_RIGHT_MAP_READ|PACHAOS_FD_RIGHT_MAP_WRITE,
		0);
	if (page_fd < 16) return -23;

	long page_addr = __pachaos_raw6(
		PACHAOS_SYSCALL_MMAP,
		page_fd,
		0,
		PACHAOS_FILED_PAGE_BYTES,
		PACHAOS_PROT_READ|PACHAOS_PROT_WRITE,
		PACHAOS_MMAP_SHARED,
		0);
	if (page_addr < 4096) {
		(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, page_fd);
		return -14;
	}

	unsigned char *page = (unsigned char *)page_addr;
	__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
	struct __pachaos_filed_openat *openat = (struct __pachaos_filed_openat *)page;
	openat->dir_handle = 0;
	openat->rights = PACHAOS_FILED_RIGHT_READ|PACHAOS_FILED_RIGHT_EXEC|PACHAOS_FILED_RIGHT_STAT;
	long status = __pachaos_copy_path(openat->name, path, PACHAOS_FILED_NAME_BYTES);
	if (status != 0) {
		(void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, page_addr, PACHAOS_FILED_PAGE_BYTES);
		(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, page_fd);
		return status;
	}

	struct __pachaos_ipc_msg reply;
	status = __pachaos_filed_call(PACHAOS_FILED_OP_OPENAT, 0, 1, page_fd, &reply);
	if (status != 0) {
		(void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, page_addr, PACHAOS_FILED_PAGE_BYTES);
		(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, page_fd);
		return status;
	}
	unsigned long long handle = reply.word2;
	if (!handle) {
		(void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, page_addr, PACHAOS_FILED_PAGE_BYTES);
		(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, page_fd);
		return -2;
	}

	__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
	struct __pachaos_filed_statx *st = (struct __pachaos_filed_statx *)page;
	st->handle = handle;
	status = __pachaos_filed_call(PACHAOS_FILED_OP_STAT, 0, 2, page_fd, &reply);
	if (status != 0) {
		__pachaos_filed_close_handle(handle, page_fd);
		(void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, page_addr, PACHAOS_FILED_PAGE_BYTES);
		(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, page_fd);
		return status;
	}
	unsigned long long file_size = reply.word2;
	unsigned long long header_bytes = file_size;
	if (header_bytes > PACHAOS_FILED_IO_BYTES) header_bytes = PACHAOS_FILED_IO_BYTES;
	if (header_bytes != 0) {
		__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
		struct __pachaos_filed_io *header_io = (struct __pachaos_filed_io *)page;
		header_io->handle = handle;
		header_io->offset = 0;
		header_io->length = header_bytes;
		status = __pachaos_filed_call(PACHAOS_FILED_OP_PREAD, 0, 6, page_fd, &reply);
		if (status != 0 || reply.word2 < header_bytes) {
			__pachaos_filed_close_handle(handle, page_fd);
			(void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, page_addr, PACHAOS_FILED_PAGE_BYTES);
			(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, page_fd);
			return status != 0 ? status : -5;
		}
	}
	unsigned long long map_size = __pachaos_elf_mapping_size(((struct __pachaos_filed_io *)page)->data, header_bytes, file_size);
	long file_fd = __pachaos_raw3(
		PACHAOS_SYSCALL_VMO_CREATE,
		(long)map_size,
		PACHAOS_FD_RIGHT_CLOSE|PACHAOS_FD_RIGHT_READ|PACHAOS_FD_RIGHT_MAP_READ|PACHAOS_FD_RIGHT_MAP_WRITE|PACHAOS_FD_RIGHT_MAP_EXEC,
		0);
	if (file_fd < 16) {
		__pachaos_filed_close_handle(handle, page_fd);
		(void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, page_addr, PACHAOS_FILED_PAGE_BYTES);
		(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, page_fd);
		return -28;
	}

	long file_addr = 0;
	if (map_size != 0) {
		file_addr = __pachaos_raw6(
			PACHAOS_SYSCALL_MMAP,
			file_fd,
			0,
			(long)map_size,
			PACHAOS_PROT_READ|PACHAOS_PROT_WRITE,
			PACHAOS_MMAP_SHARED,
			0);
		if (file_addr < 4096) {
			(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, file_fd);
			__pachaos_filed_close_handle(handle, page_fd);
			(void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, page_addr, PACHAOS_FILED_PAGE_BYTES);
			(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, page_fd);
			return -12;
		}
	}

	unsigned long long offset = 0;
	while (offset < file_size) {
		unsigned long long want = file_size - offset;
		if (want > PACHAOS_FILED_IO_BYTES) want = PACHAOS_FILED_IO_BYTES;
		__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
		struct __pachaos_filed_io *io = (struct __pachaos_filed_io *)page;
		io->handle = handle;
		io->offset = offset;
		io->length = want;
		status = __pachaos_filed_call(PACHAOS_FILED_OP_PREAD, 0, 3, page_fd, &reply);
		if (status != 0 || reply.word2 == 0) {
			if (map_size != 0) (void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, file_addr, (long)map_size);
			(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, file_fd);
			__pachaos_filed_close_handle(handle, page_fd);
			(void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, page_addr, PACHAOS_FILED_PAGE_BYTES);
			(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, page_fd);
			return status != 0 ? status : -5;
		}
		unsigned long long got = reply.word2;
		if (got > want) got = want;
		for (unsigned long long i = 0; i < got; i++) {
			((unsigned char *)file_addr)[offset + i] = io->data[i];
		}
		offset += got;
	}

	if (map_size != 0) (void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, file_addr, (long)map_size);
	__pachaos_filed_close_handle(handle, page_fd);
	(void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, page_addr, PACHAOS_FILED_PAGE_BYTES);
	(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, page_fd);
	return file_fd;
}

static __inline long __pachaos_openat_dispatch(long dirfd, const char *path, long flags)
{
	if ((flags & (LINUX_O_WRONLY|LINUX_O_RDWR|LINUX_O_CREAT|LINUX_O_TRUNC|LINUX_O_APPEND|LINUX_O_DIRECTORY)) ||
		__pachaos_filed_fd_handle(dirfd))
	{
		return __pachaos_openat_filed_fd(dirfd, path, flags);
	}
	return __pachaos_openat_file_vmo(dirfd, path, flags);
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
	case __NR_close: return __pachaos_filed_close_fd(a1);
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
	case __NR_fstat: return __pachaos_filed_fstat(a1, (void *)a2);
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
	case __NR_read: return __pachaos_filed_read(a1, (void *)a2, a3);
	case __NR_write: return __pachaos_filed_write(a1, (const void *)a2, a3);
	case __NR_readv: return __pachaos_raw3(PACHAOS_SYSCALL_FD_READV, a1, a2, a3);
	case __NR_writev: return __pachaos_raw3(PACHAOS_SYSCALL_FD_WRITEV, a1, a2, a3);
	case __NR_ioctl: return __pachaos_status(__pachaos_raw3(PACHAOS_SYSCALL_FD_IOCTL, a1, a2, a3));
	case __NR_poll: return __pachaos_poll((struct __pachaos_linux_pollfd *)a1, a2, a3);
	case __NR_mprotect: return __pachaos_status(__pachaos_raw3(PACHAOS_SYSCALL_MPROTECT, a1, a2, __pachaos_prot(a3)));
	case __NR_open: return __pachaos_openat_dispatch(LINUX_AT_FDCWD, (const char *)a1, a2);
	case __NR_getdents:
	case __NR_getdents64: return __pachaos_filed_getdents(a1, (void *)a2, a3);
	case __NR_execve: return __pachaos_filed_execve((const char *)a1, (char *const *)a2, (char *const *)a3);
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
	case __NR_newfstatat:
		if ((a4 & LINUX_AT_EMPTY_PATH) && a2 && *(const char *)a2 == 0) {
			return __pachaos_filed_fstat(a1, (void *)a3);
		}
		return -38;
	case __NR_timerfd_settime:
		if (a2 & ~LINUX_TFD_TIMER_ABSTIME) return -22;
		return __pachaos_status(__pachaos_raw4(PACHAOS_SYSCALL_TIMERFD_SETTIME, a1, a2, a3, a4));
	case __NR_openat:
		return __pachaos_openat_dispatch(a1, (const char *)a2, a3);
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
	case __NR_getdents:
	case __NR_getdents64:
	case __NR_execve:
		return __syscall3(n, a1, a2, a3);
	case __NR_futex:
	case __NR_newfstatat:
	case __NR_clock_nanosleep:
	case __NR_timerfd_settime:
	case __NR_openat:
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
