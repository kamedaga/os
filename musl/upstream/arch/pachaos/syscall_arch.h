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
#define PACHAOS_SYSCALL_FD_FCNTL 29
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
#define PACHAOS_SYSCALL_MREMAP 42
#define PACHAOS_SYSCALL_MADVISE 43
#define PACHAOS_SYSCALL_IPC_CHANNEL_CREATE 45
#define PACHAOS_SYSCALL_IPC_SEND 46
#define PACHAOS_SYSCALL_IPC_RECV 47
#define PACHAOS_SYSCALL_IPC_CALL 48
#define PACHAOS_SYSCALL_IPC_RECV_WAIT 50

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
#define LINUX_MREMAP_MAYMOVE 1
#define LINUX_MREMAP_FIXED 2
#define LINUX_MREMAP_DONTUNMAP 4
#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1
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
#define PACHAOS_MAX_POLLFDS 64
#define PACHAOS_BRK_RESERVE_BYTES (256ULL * 1024ULL * 1024ULL)

extern unsigned char *__pachaos_brk_base;
extern unsigned char *__pachaos_brk_cur;
extern unsigned char *__pachaos_brk_limit;
extern int __pachaos_brk_lock;

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
#define PACHAOS_FILED_OP_DUP 11
#define PACHAOS_FILED_OP_GET_FLAGS 12
#define PACHAOS_FILED_OP_SET_FLAGS 13
#define PACHAOS_FILED_OP_PWRITE 14
#define PACHAOS_FILED_OP_WRITE 15
#define PACHAOS_FILED_OP_TRUNCATE 17
#define PACHAOS_FILED_OP_UNLINK 18
#define PACHAOS_FILED_OP_RENAME 19
#define PACHAOS_FILED_OP_SEEK 22
#define PACHAOS_FILED_OP_CONNECT 25
#define PACHAOS_FILED_OP_FAST_DOORBELL 27
#define PACHAOS_FILED_OP_VALIDATE_OPEN_CACHE 28
#define PACHAOS_FILED_OP_PWRITE_BATCH 29
#define PACHAOS_FILED_OP_WRITE_BATCH 30
#define PACHAOS_FILED_RIGHT_LOOKUP (1U << 0)
#define PACHAOS_FILED_RIGHT_READ (1U << 1)
#define PACHAOS_FILED_RIGHT_WRITE (1U << 2)
#define PACHAOS_FILED_RIGHT_EXEC (1U << 3)
#define PACHAOS_FILED_RIGHT_STAT (1U << 4)
#define PACHAOS_FILED_RIGHT_GETDENTS (1U << 5)
#define PACHAOS_FILED_RIGHT_CREATE (1U << 6)
#define PACHAOS_FILED_RIGHT_REMOVE (1U << 7)
#define PACHAOS_FILED_RIGHT_RENAME (1U << 8)
#define PACHAOS_FILED_OPEN_CREATE (1U << 0)
#define PACHAOS_FILED_OPEN_TRUNCATE (1U << 2)
#define PACHAOS_FILED_OPEN_DIRECTORY (1U << 3)
#define PACHAOS_FILED_OPEN_CLOEXEC (1U << 5)
#define PACHAOS_FILED_OPEN_APPEND (1U << 6)
#define PACHAOS_FILED_OPEN_NONBLOCK (1U << 7)
#define PACHAOS_FILED_FD_CLOEXEC (1U << 0)
#define PACHAOS_FILED_FILE_APPEND (1U << 0)
#define PACHAOS_FILED_FILE_NONBLOCK (1U << 1)
#define PACHAOS_FILED_FILE_SYNC (1U << 2)
#define PACHAOS_FILED_FILE_NEEDS_REWIND (1U << 30)
#define PACHAOS_FILED_FILE_DIR_EOF (1U << 31)
#define PACHAOS_FILED_EXEC_MAX_ARGS 8
#define PACHAOS_FILED_EXEC_MAX_ENVS 8
#define PACHAOS_FILED_EXEC_ARG_BYTES 128
#define PACHAOS_FILED_EXEC_ENV_BYTES 128
#define PACHAOS_FILED_SESSION_PAGE_BYTES 40960
#define PACHAOS_FILED_FAST_MAGIC 0x31545341464c4446ULL
#define PACHAOS_FILED_FAST_VERSION 1
#define PACHAOS_FILED_FAST_REQUEST_CAPACITY 8
#define PACHAOS_FILED_FAST_COMPLETION_CAPACITY 8
#define PACHAOS_FILED_FAST_PAYLOAD_SLOT_COUNT 4
#define PACHAOS_FILED_FAST_PAYLOAD_OFFSET 4096
#define PACHAOS_FILED_FAST_GENERATION_OFFSET (PACHAOS_FILED_FAST_PAYLOAD_OFFSET + PACHAOS_FILED_FAST_PAYLOAD_SLOT_COUNT * PACHAOS_FILED_PAGE_BYTES)
#define PACHAOS_FILED_FAST_GENERATION_CAPACITY 64
#define PACHAOS_FILED_OPEN_CACHE_CAP 8
#ifndef PACHAOS_FILED_READ_CACHE_BYTES
#define PACHAOS_FILED_READ_CACHE_BYTES 4096
#endif
#if PACHAOS_FILED_READ_CACHE_BYTES > 0
#define PACHAOS_FILED_READ_CACHE_STORAGE_BYTES PACHAOS_FILED_READ_CACHE_BYTES
#else
#define PACHAOS_FILED_READ_CACHE_STORAGE_BYTES 1
#endif

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
	unsigned long long object_generation;
	unsigned long long dir_generation;
	unsigned long long reserved0;
	char name[PACHAOS_FILED_NAME_BYTES];
};

struct __pachaos_filed_validate_open_cache {
	unsigned long long cached_handle;
	unsigned long long dir_handle;
	unsigned long long rights;
	unsigned long long open_flags;
	unsigned long long object_generation;
	unsigned long long dir_generation;
	unsigned long long reserved0;
	unsigned long long reserved1;
	char name[PACHAOS_FILED_NAME_BYTES];
};

struct __pachaos_filed_io {
	unsigned long long handle;
	unsigned long long offset;
	unsigned long long length;
	unsigned char data[PACHAOS_FILED_IO_BYTES];
};

struct __pachaos_filed_fast_header {
	unsigned long long magic;
	unsigned long long version;
	unsigned long long flags;
	unsigned long long request_capacity;
	unsigned long long completion_capacity;
	unsigned long long payload_slot_count;
	unsigned long long payload_slot_size;
	unsigned long long payload_offset;
	unsigned long long request_head;
	unsigned long long request_tail;
	unsigned long long completion_head;
	unsigned long long completion_tail;
	unsigned long long doorbell_seq;
	unsigned long long completion_seq;
	unsigned long long generation_offset;
	unsigned long long generation_capacity;
};

struct __pachaos_filed_fast_request {
	unsigned long long request_id;
	unsigned long long opcode;
	unsigned long long flags;
	unsigned long long handle;
	unsigned long long word2;
	unsigned long long offset;
	unsigned long long length;
	unsigned long long payload_slot;
	unsigned long long payload_length;
	unsigned long long timeout_ns;
};

struct __pachaos_filed_fast_completion {
	unsigned long long request_id;
	long long status;
	unsigned long long result;
	unsigned long long bytes;
	unsigned long long flags;
};

struct __pachaos_filed_generation_entry {
	unsigned long long seq;
	unsigned long long handle;
	unsigned long long object_generation;
	unsigned long long dir_generation;
};

struct __pachaos_filed_statx {
	unsigned long long handle;
	unsigned long long mode;
	unsigned long long size;
	unsigned long long blocks;
	unsigned long long nlink;
	unsigned long long kind;
	unsigned long long object_generation;
	unsigned long long dir_generation;
};

struct __pachaos_filed_seek {
	unsigned long long handle;
	long long offset;
	unsigned long long whence;
	unsigned long long reserved0;
};

struct __pachaos_filed_truncate {
	unsigned long long handle;
	unsigned long long size;
	unsigned long long reserved0;
	unsigned long long reserved1;
};

struct __pachaos_filed_unlink {
	unsigned long long dir_handle;
	unsigned long long reserved0;
	char name[PACHAOS_FILED_NAME_BYTES];
};

struct __pachaos_filed_rename {
	unsigned long long old_dir_handle;
	unsigned long long new_dir_handle;
	char old_name[PACHAOS_FILED_NAME_BYTES];
	char new_name[PACHAOS_FILED_NAME_BYTES];
};

struct __pachaos_filed_handle_flags {
	unsigned long long handle;
	unsigned long long fd_flags;
	unsigned long long status_flags;
	unsigned long long reserved0;
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
	unsigned long long dir_generation;
	unsigned long long reserved0;
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
	unsigned char stat_valid;
	unsigned char read_cache_valid;
	unsigned char read_cache_eof;
	unsigned char open_cacheable;
	unsigned short read_cache_len;
	unsigned int fd_flags;
	unsigned int status_flags;
	unsigned int open_cache_rights;
	unsigned int open_cache_open_flags;
	unsigned long long offset;
	unsigned long long handle;
	unsigned long long stat_mode;
	unsigned long long stat_size;
	unsigned long long stat_blocks;
	unsigned long long stat_nlink;
	unsigned long long stat_kind;
	unsigned long long read_cache_offset;
	unsigned long long object_generation;
	unsigned long long dir_generation;
	unsigned char read_cache[PACHAOS_FILED_READ_CACHE_STORAGE_BYTES];
	char open_cache_path[PACHAOS_FILED_NAME_BYTES];
};

struct __pachaos_filed_open_cache_entry {
	unsigned char used;
	unsigned char stat_valid;
	unsigned char read_cache_valid;
	unsigned char read_cache_eof;
	unsigned char slot_index;
	unsigned short read_cache_len;
	unsigned int rights;
	unsigned int open_flags;
	unsigned long long handle;
	unsigned long long object_generation;
	unsigned long long dir_generation;
	unsigned long long stat_mode;
	unsigned long long stat_size;
	unsigned long long stat_blocks;
	unsigned long long stat_nlink;
	unsigned long long stat_kind;
	unsigned long long read_cache_offset;
	char path[PACHAOS_FILED_NAME_BYTES];
};

struct __pachaos_iovec {
	void *base;
	unsigned long len;
};

extern struct __pachaos_filed_fd_entry __pachaos_filed_fds[PACHAOS_FILED_FD_CAP];
extern unsigned char __pachaos_filed_fd_used[PACHAOS_FILED_FD_CAP];
extern struct __pachaos_filed_open_cache_entry __pachaos_filed_open_cache[PACHAOS_FILED_OPEN_CACHE_CAP];
extern unsigned char __pachaos_filed_open_cache_read_data[PACHAOS_FILED_OPEN_CACHE_CAP][PACHAOS_FILED_READ_CACHE_STORAGE_BYTES];
extern unsigned long long __pachaos_filed_request_id;
extern long __pachaos_filed_page_fd;
extern unsigned char *__pachaos_filed_page_addr;
extern unsigned char *__pachaos_filed_session_page_addr;
extern int __pachaos_filed_page_lock;
extern long __pachaos_filed_session_fd;

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

static __inline int __pachaos_str_equal(const char *a, const char *b)
{
	if (!a || !b) return 0;
	for (long i = 0; i < PACHAOS_FILED_NAME_BYTES; i++) {
		if (a[i] != b[i]) return 0;
		if (a[i] == 0) return 1;
	}
	return 1;
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

static __inline long __pachaos_filed_next_request_id(void);

static __inline long __pachaos_filed_session_connect(long page_fd)
{
	if (__pachaos_filed_session_fd >= 16) return 0;
	if (page_fd < 16) return -22;

	unsigned long long pair[2] = {0, 0};
	unsigned long long channel_rights =
		PACHAOS_FD_RIGHT_INSPECT |
		PACHAOS_FD_RIGHT_WAIT |
		PACHAOS_FD_RIGHT_POLL |
		PACHAOS_FD_RIGHT_CLOSE |
		PACHAOS_FD_RIGHT_SEND |
		PACHAOS_FD_RIGHT_RECV |
		PACHAOS_FD_RIGHT_TRANSFER;
	long status = __pachaos_raw3(PACHAOS_SYSCALL_IPC_CHANNEL_CREATE, (long)pair, channel_rights, PACHAOS_FD_FLAG_CLOEXEC);
	if (status != 0 || pair[0] < 16 || pair[1] < 16) return status != 0 ? status : -23;

	struct __pachaos_ipc_fd request_fds[2];
	struct __pachaos_ipc_msg request;
	struct __pachaos_ipc_msg reply;
	__pachaos_bzero(request_fds, sizeof request_fds);
	__pachaos_bzero(&request, sizeof request);
	__pachaos_bzero(&reply, sizeof reply);

	request_fds[0].fd = pair[1];
	request_fds[0].rights =
		PACHAOS_FD_RIGHT_CLOSE |
		PACHAOS_FD_RIGHT_WAIT |
		PACHAOS_FD_RIGHT_POLL |
		PACHAOS_FD_RIGHT_SEND |
		PACHAOS_FD_RIGHT_RECV;
	request_fds[1].fd = page_fd;
	request_fds[1].rights =
		PACHAOS_FD_RIGHT_CLOSE |
		PACHAOS_FD_RIGHT_MAP_READ |
		PACHAOS_FD_RIGHT_MAP_WRITE;
	request.word0 = PACHAOS_FILED_REQUEST_MAGIC;
	request.word1 = PACHAOS_FILED_OP_CONNECT;
	request.word3 = __pachaos_filed_next_request_id();
	request.fds = request_fds;
	request.fd_count = 2;

	long reply_fd = __pachaos_raw2(PACHAOS_SYSCALL_IPC_CALL, PACHAOS_FILED_ENDPOINT_FD, (long)&request);
	(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, (long)pair[1]);
	if (reply_fd < 16) {
		(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, (long)pair[0]);
		return -9;
	}
	for (;;) {
		status = __pachaos_raw4(PACHAOS_SYSCALL_IPC_RECV_WAIT, reply_fd, (long)&reply, (long)~0ULL, 0);
		if (status == 0) break;
		if (status != 2 && status != -2 && status != 5 && status != -5) break;
	}
	(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, reply_fd);
	if (status != 0 ||
		reply.word0 != PACHAOS_FILED_REPLY_MAGIC ||
		reply.word1 != 0 ||
		reply.word3 != request.word3)
	{
		(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, (long)pair[0]);
		return status != 0 ? (status > 0 ? -status : status) : -71;
	}

	__pachaos_filed_session_fd = (long)pair[0];
	return 0;
}

static __inline long __pachaos_filed_fast_layout(
	struct __pachaos_filed_fast_header **out_header,
	struct __pachaos_filed_fast_request **out_requests,
	struct __pachaos_filed_fast_completion **out_completions)
{
	if ((unsigned long)__pachaos_filed_session_page_addr < 4096) return -14;
	struct __pachaos_filed_fast_header *header =
		(struct __pachaos_filed_fast_header *)__pachaos_filed_session_page_addr;
	if (header->magic != PACHAOS_FILED_FAST_MAGIC ||
		header->version != PACHAOS_FILED_FAST_VERSION ||
		header->request_capacity != PACHAOS_FILED_FAST_REQUEST_CAPACITY ||
		header->completion_capacity != PACHAOS_FILED_FAST_COMPLETION_CAPACITY ||
		header->payload_slot_count == 0 ||
		header->payload_slot_size != PACHAOS_FILED_PAGE_BYTES ||
		header->payload_offset != PACHAOS_FILED_FAST_PAYLOAD_OFFSET ||
		header->generation_offset != PACHAOS_FILED_FAST_GENERATION_OFFSET ||
		header->generation_capacity != PACHAOS_FILED_FAST_GENERATION_CAPACITY)
	{
		return -71;
	}
	struct __pachaos_filed_fast_request *requests =
		(struct __pachaos_filed_fast_request *)(__pachaos_filed_session_page_addr + sizeof(struct __pachaos_filed_fast_header));
	struct __pachaos_filed_fast_completion *completions =
		(struct __pachaos_filed_fast_completion *)((unsigned char *)requests +
			sizeof(struct __pachaos_filed_fast_request) * PACHAOS_FILED_FAST_REQUEST_CAPACITY);
	if (out_header) *out_header = header;
	if (out_requests) *out_requests = requests;
	if (out_completions) *out_completions = completions;
	return 0;
}

static __inline unsigned char *__pachaos_filed_fast_payload_slot(unsigned long long slot)
{
	struct __pachaos_filed_fast_header *header = 0;
	if (__pachaos_filed_fast_layout(&header, 0, 0) != 0) return 0;
	if (slot >= header->payload_slot_count) return 0;
	return __pachaos_filed_session_page_addr + header->payload_offset + slot * header->payload_slot_size;
}

static __inline long __pachaos_filed_shared_generation_lookup(
	unsigned long long handle,
	unsigned long long *object_generation,
	unsigned long long *dir_generation)
{
	if (!handle || !object_generation || !dir_generation) return 0;
	struct __pachaos_filed_fast_header *header = 0;
	if (__pachaos_filed_fast_layout(&header, 0, 0) != 0) return 0;
	struct __pachaos_filed_generation_entry *entries =
		(struct __pachaos_filed_generation_entry *)(__pachaos_filed_session_page_addr + header->generation_offset);
	for (unsigned long long i = 0; i < header->generation_capacity; i++) {
		struct __pachaos_filed_generation_entry *entry = &entries[i];
		unsigned long long seq0 = entry->seq;
		__sync_synchronize();
		unsigned long long entry_handle = entry->handle;
		unsigned long long entry_object_generation = entry->object_generation;
		unsigned long long entry_dir_generation = entry->dir_generation;
		__sync_synchronize();
		unsigned long long seq1 = entry->seq;
		if (seq0 == seq1 &&
			(seq0 & 1ULL) == 0 &&
			entry_handle == handle &&
			entry_object_generation != 0)
		{
			*object_generation = entry_object_generation;
			*dir_generation = entry_dir_generation;
			return 1;
		}
	}
	return 0;
}

static __inline long __pachaos_filed_fast_enqueue(
	long op,
	long word2,
	long request_id,
	unsigned long long payload_slot,
	unsigned long long payload_length)
{
	struct __pachaos_filed_fast_header *header = 0;
	struct __pachaos_filed_fast_request *requests = 0;
	long status = __pachaos_filed_fast_layout(&header, &requests, 0);
	if (status != 0) return status;
	if (payload_slot >= header->payload_slot_count) return -22;
	if (payload_length > header->payload_slot_size) return -22;
	if (header->request_tail - header->request_head >= header->request_capacity) return -11;

	unsigned long long tail = header->request_tail;
	struct __pachaos_filed_fast_request *slot =
		&requests[tail % header->request_capacity];
	__pachaos_bzero(slot, sizeof *slot);
	slot->request_id = (unsigned long long)request_id;
	slot->opcode = (unsigned long long)op;
	slot->word2 = (unsigned long long)word2;
	slot->payload_slot = payload_slot;
	slot->payload_length = payload_length;
	__sync_synchronize();
	header->request_tail = tail + 1;
	return 0;
}

static __inline long __pachaos_filed_fast_doorbell(long request_id, struct __pachaos_ipc_msg *reply)
{
	struct __pachaos_filed_fast_header *header = 0;
	long status = __pachaos_filed_fast_layout(&header, 0, 0);
	if (status != 0) return status;

	struct __pachaos_ipc_msg request;
	__pachaos_bzero(&request, sizeof request);
	__pachaos_bzero(reply, sizeof *reply);
	request.word0 = PACHAOS_FILED_REQUEST_MAGIC;
	request.word1 = PACHAOS_FILED_OP_FAST_DOORBELL;
	request.word2 = ++header->doorbell_seq;
	request.word3 = (unsigned long long)request_id;

	for (;;) {
		status = __pachaos_raw2(PACHAOS_SYSCALL_IPC_SEND, __pachaos_filed_session_fd, (long)&request);
		if (status == 0) break;
		if (status != 2 && status != -2 && status != 5 && status != -5) return status > 0 ? -status : status;
		struct __pachaos_pollfd wait_fd;
		wait_fd.fd = __pachaos_filed_session_fd;
		wait_fd.events = PACHAOS_POLL_WRITABLE;
		wait_fd.revents = 0;
		(void)__pachaos_raw4(PACHAOS_SYSCALL_FD_WAIT_MANY, (long)&wait_fd, 1, 1, 0);
	}
	for (;;) {
		status = __pachaos_raw4(PACHAOS_SYSCALL_IPC_RECV_WAIT, __pachaos_filed_session_fd, (long)reply, (long)~0ULL, 0);
		if (status == 0) break;
		if (status != 2 && status != -2 && status != 5 && status != -5) return status > 0 ? -status : status;
	}
	if (reply->word0 != PACHAOS_FILED_REPLY_MAGIC) return -71;
	return (long)reply->word1;
}

static __inline long __pachaos_filed_fast_wait_completion(long request_id, struct __pachaos_ipc_msg *reply)
{
	struct __pachaos_filed_fast_header *header = 0;
	struct __pachaos_filed_fast_completion *completions = 0;
	long status = __pachaos_filed_fast_layout(&header, 0, &completions);
	if (status != 0) return status;
	while (header->completion_head == header->completion_tail) {
		status = __pachaos_raw4(PACHAOS_SYSCALL_IPC_RECV_WAIT, __pachaos_filed_session_fd, (long)reply, (long)~0ULL, 0);
		if (status != 0 && status != 2 && status != -2 && status != 5 && status != -5) return status > 0 ? -status : status;
	}
	__sync_synchronize();
	struct __pachaos_filed_fast_completion *completion =
		&completions[header->completion_head % header->completion_capacity];
	if (completion->request_id != (unsigned long long)request_id) return -71;
	reply->word0 = PACHAOS_FILED_REPLY_MAGIC;
	reply->word1 = (unsigned long long)completion->status;
	reply->word2 = completion->result;
	reply->word3 = completion->request_id;
	header->completion_head++;
	return (long)completion->status;
}

static __inline long __pachaos_filed_session_call(long op, long word2, long request_id, long page_fd, struct __pachaos_ipc_msg *reply)
{
	long status = __pachaos_filed_session_connect(page_fd);
	if (status != 0) return status;

	struct __pachaos_filed_fast_header *header = 0;
	status = __pachaos_filed_fast_layout(&header, 0, 0);
	if (status != 0) return status;
	while (header->request_tail - header->request_head >= header->request_capacity) {
		status = __pachaos_filed_fast_doorbell(request_id, reply);
		if (status != 0) return status;
	}

	status = __pachaos_filed_fast_enqueue(op, word2, request_id, 0, PACHAOS_FILED_PAGE_BYTES);
	if (status != 0) return status;
	status = __pachaos_filed_fast_doorbell(request_id, reply);
	if (status != 0) return status;
	return __pachaos_filed_fast_wait_completion(request_id, reply);
}

static __inline long __pachaos_filed_call_with_fds(
	long op,
	long word2,
	long request_id,
	long page_fd,
	struct __pachaos_ipc_msg *reply,
	struct __pachaos_ipc_fd *reply_fds,
	unsigned long long reply_fd_capacity)
{
	if (reply_fd_capacity == 0 && op != PACHAOS_FILED_OP_CONNECT) {
		return __pachaos_filed_session_call(op, word2, request_id, page_fd, reply);
	}

	struct __pachaos_ipc_fd request_fd;
	struct __pachaos_ipc_msg request;
	__pachaos_bzero(&request_fd, sizeof request_fd);
	__pachaos_bzero(&request, sizeof request);
	__pachaos_bzero(reply, sizeof *reply);
	reply->fds = reply_fds;
	reply->fd_capacity = reply_fd_capacity;

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
		status = __pachaos_raw4(PACHAOS_SYSCALL_IPC_RECV_WAIT, reply_fd, (long)reply, (long)~0ULL, 0);
		if (status == 0) break;
		if (status != 2 && status != -2 && status != 5 && status != -5) break;
	}
	(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, reply_fd);
	if (status != 0) return status > 0 ? -status : status;
	if (reply->word0 != PACHAOS_FILED_REPLY_MAGIC || reply->word3 != (unsigned long long)request_id) return -71;
	return (long)reply->word1;
}

static __inline long __pachaos_filed_call(long op, long word2, long request_id, long page_fd, struct __pachaos_ipc_msg *reply)
{
	return __pachaos_filed_call_with_fds(op, word2, request_id, page_fd, reply, 0, 0);
}

static __inline long __pachaos_filed_next_request_id(void)
{
	unsigned long long id = __pachaos_filed_request_id++;
	if (__pachaos_filed_request_id == 0) __pachaos_filed_request_id = 100;
	return (long)id;
}

static __inline long __pachaos_filed_page_create(long *out_fd, unsigned char **out_page)
{
	if (!out_fd || !out_page) return -22;

	while (__sync_lock_test_and_set(&__pachaos_filed_page_lock, 1)) {
		__asm__ __volatile__("pause");
	}

	if (__pachaos_filed_page_fd < 16 || (unsigned long)__pachaos_filed_session_page_addr < 4096) {
		long page_fd = __pachaos_raw3(
			PACHAOS_SYSCALL_VMO_CREATE,
			PACHAOS_FILED_SESSION_PAGE_BYTES,
			PACHAOS_FD_RIGHT_TRANSFER|PACHAOS_FD_RIGHT_CLOSE|PACHAOS_FD_RIGHT_MAP_READ|PACHAOS_FD_RIGHT_MAP_WRITE,
			0);
		if (page_fd < 16) {
			__sync_lock_release(&__pachaos_filed_page_lock);
			return -23;
		}
		long page_addr = __pachaos_raw6(
			PACHAOS_SYSCALL_MMAP,
			page_fd,
			0,
			PACHAOS_FILED_SESSION_PAGE_BYTES,
			PACHAOS_PROT_READ|PACHAOS_PROT_WRITE,
			PACHAOS_MMAP_SHARED,
			0);
		if (page_addr < 4096) {
			(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, page_fd);
			__sync_lock_release(&__pachaos_filed_page_lock);
			return -14;
		}
		__pachaos_filed_page_fd = page_fd;
		__pachaos_filed_session_page_addr = (unsigned char *)page_addr;
		__pachaos_bzero(__pachaos_filed_session_page_addr, PACHAOS_FILED_SESSION_PAGE_BYTES);
		struct __pachaos_filed_fast_header *header =
			(struct __pachaos_filed_fast_header *)__pachaos_filed_session_page_addr;
		header->magic = PACHAOS_FILED_FAST_MAGIC;
		header->version = PACHAOS_FILED_FAST_VERSION;
		header->request_capacity = PACHAOS_FILED_FAST_REQUEST_CAPACITY;
		header->completion_capacity = PACHAOS_FILED_FAST_COMPLETION_CAPACITY;
		header->payload_slot_count = PACHAOS_FILED_FAST_PAYLOAD_SLOT_COUNT;
		header->payload_slot_size = PACHAOS_FILED_PAGE_BYTES;
		header->payload_offset = PACHAOS_FILED_FAST_PAYLOAD_OFFSET;
		header->generation_offset = PACHAOS_FILED_FAST_GENERATION_OFFSET;
		header->generation_capacity = PACHAOS_FILED_FAST_GENERATION_CAPACITY;
		__pachaos_filed_page_addr = __pachaos_filed_session_page_addr + PACHAOS_FILED_FAST_PAYLOAD_OFFSET;
	}

	*out_fd = __pachaos_filed_page_fd;
	*out_page = __pachaos_filed_page_addr;
	return 0;
}

static __inline void __pachaos_filed_page_destroy(long page_fd, unsigned char *page)
{
	(void)page_fd;
	(void)page;
	__sync_lock_release(&__pachaos_filed_page_lock);
}

static __inline int __pachaos_filed_fd_index(long fd)
{
	if (fd < PACHAOS_FILED_FD_BASE || fd >= PACHAOS_FILED_FD_BASE + PACHAOS_FILED_FD_CAP) return -1;
	int idx = (int)(fd - PACHAOS_FILED_FD_BASE);
	return __pachaos_filed_fd_used[idx] ? idx : -1;
}

static __inline unsigned int __pachaos_filed_fd_flags_from_open(long flags)
{
	return (flags & LINUX_O_CLOEXEC) ? PACHAOS_FILED_FD_CLOEXEC : 0;
}

static __inline unsigned int __pachaos_filed_status_flags_from_open(long flags)
{
	unsigned int out = 0;
	if (flags & LINUX_O_APPEND) out |= PACHAOS_FILED_FILE_APPEND;
	if (flags & LINUX_O_NONBLOCK) out |= PACHAOS_FILED_FILE_NONBLOCK;
	return out;
}

static __inline long __pachaos_filed_fd_alloc_from(
	unsigned long long handle,
	long min_fd,
	unsigned int fd_flags,
	unsigned int status_flags)
{
	if (!handle) return -22;
	if (min_fd < 0) return -22;
	int start = 0;
	if (min_fd > PACHAOS_FILED_FD_BASE) {
		start = (int)(min_fd - PACHAOS_FILED_FD_BASE);
	}
	if (start >= PACHAOS_FILED_FD_CAP) return -24;
	for (int i = start; i < PACHAOS_FILED_FD_CAP; i++) {
		if (!__pachaos_filed_fd_used[i]) {
			__pachaos_filed_fd_used[i] = 1;
			__pachaos_filed_fds[i].used = 1;
			__pachaos_filed_fds[i].fd_flags = fd_flags;
			__pachaos_filed_fds[i].status_flags = status_flags;
			__pachaos_filed_fds[i].open_cache_rights = 0;
			__pachaos_filed_fds[i].open_cache_open_flags = 0;
			__pachaos_filed_fds[i].offset = 0;
			__pachaos_filed_fds[i].handle = handle;
			__pachaos_filed_fds[i].stat_valid = 0;
			__pachaos_filed_fds[i].read_cache_valid = 0;
			__pachaos_filed_fds[i].read_cache_eof = 0;
			__pachaos_filed_fds[i].open_cacheable = 0;
			__pachaos_filed_fds[i].read_cache_len = 0;
			__pachaos_filed_fds[i].stat_mode = 0;
			__pachaos_filed_fds[i].stat_size = 0;
			__pachaos_filed_fds[i].stat_blocks = 0;
			__pachaos_filed_fds[i].stat_nlink = 0;
			__pachaos_filed_fds[i].stat_kind = 0;
			__pachaos_filed_fds[i].read_cache_offset = 0;
			__pachaos_filed_fds[i].object_generation = 0;
			__pachaos_filed_fds[i].dir_generation = 0;
			__pachaos_bzero(__pachaos_filed_fds[i].open_cache_path, PACHAOS_FILED_NAME_BYTES);
			return PACHAOS_FILED_FD_BASE + i;
		}
	}
	return -24;
}

static __inline long __pachaos_filed_fd_alloc(unsigned long long handle)
{
	return __pachaos_filed_fd_alloc_from(handle, 0, 0, 0);
}

static __inline unsigned long long __pachaos_filed_fd_handle(long fd)
{
	int idx = __pachaos_filed_fd_index(fd);
	return idx < 0 ? 0 : __pachaos_filed_fds[idx].handle;
}

static __inline unsigned int __pachaos_filed_fd_flags(long fd)
{
	int idx = __pachaos_filed_fd_index(fd);
	return idx < 0 ? 0 : __pachaos_filed_fds[idx].fd_flags;
}

static __inline void __pachaos_filed_fd_set_flags(long fd, unsigned int fd_flags)
{
	int idx = __pachaos_filed_fd_index(fd);
	if (idx >= 0) __pachaos_filed_fds[idx].fd_flags = fd_flags;
}

static __inline unsigned int __pachaos_filed_status_flags(long fd)
{
	int idx = __pachaos_filed_fd_index(fd);
	return idx < 0 ? 0 : __pachaos_filed_fds[idx].status_flags;
}

static __inline void __pachaos_filed_fd_set_status_flags(long fd, unsigned int status_flags)
{
	int idx = __pachaos_filed_fd_index(fd);
	if (idx >= 0) __pachaos_filed_fds[idx].status_flags = status_flags;
}

static __inline unsigned long long __pachaos_filed_fd_offset(long fd)
{
	int idx = __pachaos_filed_fd_index(fd);
	return idx < 0 ? 0 : __pachaos_filed_fds[idx].offset;
}

static __inline void __pachaos_filed_fd_set_offset(long fd, unsigned long long offset)
{
	int idx = __pachaos_filed_fd_index(fd);
	if (idx >= 0) {
		__pachaos_filed_fds[idx].offset = offset;
		__pachaos_filed_fds[idx].status_flags &= ~PACHAOS_FILED_FILE_DIR_EOF;
	}
}

static __inline void __pachaos_filed_fd_advance_offset(long fd, unsigned long long amount)
{
	int idx = __pachaos_filed_fd_index(fd);
	if (idx < 0) return;
	if (amount > ~0ULL - __pachaos_filed_fds[idx].offset) {
		__pachaos_filed_fds[idx].offset = ~0ULL;
		return;
	}
	__pachaos_filed_fds[idx].offset += amount;
}

static __inline void __pachaos_filed_fd_invalidate_cached_state_idx(int idx)
{
	if (idx < 0) return;
	__pachaos_filed_fds[idx].stat_valid = 0;
	__pachaos_filed_fds[idx].read_cache_valid = 0;
	__pachaos_filed_fds[idx].read_cache_eof = 0;
	__pachaos_filed_fds[idx].read_cache_len = 0;
	__pachaos_filed_fds[idx].read_cache_offset = 0;
	__pachaos_filed_fds[idx].status_flags &= ~PACHAOS_FILED_FILE_DIR_EOF;
}

static __inline void __pachaos_filed_fd_invalidate_stat_idx(int idx)
{
	if (idx >= 0) __pachaos_filed_fds[idx].stat_valid = 0;
}

static __inline void __pachaos_filed_fd_invalidate_stat(long fd)
{
	__pachaos_filed_fd_invalidate_stat_idx(__pachaos_filed_fd_index(fd));
}

static __inline void __pachaos_filed_fd_note_write_stat_idx(
	int idx,
	unsigned long long offset,
	unsigned long long len)
{
	if (idx < 0 || !__pachaos_filed_fds[idx].stat_valid) return;
	if (offset > ~0ULL - len) {
		__pachaos_filed_fds[idx].stat_valid = 0;
		return;
	}
	unsigned long long end = offset + len;
	if (end > __pachaos_filed_fds[idx].stat_size) {
		__pachaos_filed_fds[idx].stat_size = end;
	}
}

static __inline void __pachaos_filed_fd_invalidate_cached_state(long fd)
{
	__pachaos_filed_fd_invalidate_cached_state_idx(__pachaos_filed_fd_index(fd));
}

static __inline unsigned long long __pachaos_min_ull(unsigned long long a, unsigned long long b)
{
	return a < b ? a : b;
}

static __inline unsigned long long __pachaos_max_ull(unsigned long long a, unsigned long long b)
{
	return a > b ? a : b;
}

static __inline long __pachaos_filed_fd_read_cache_copy_to_linear(
	int idx,
	unsigned long long offset,
	unsigned long long len,
	unsigned char *dst)
{
#if PACHAOS_FILED_READ_CACHE_BYTES > 0
	if (idx < 0 || !dst || !__pachaos_filed_fds[idx].read_cache_valid) return -1;
	unsigned long long cache_start = __pachaos_filed_fds[idx].read_cache_offset;
	unsigned long long cache_len = __pachaos_filed_fds[idx].read_cache_len;
	if (offset < cache_start || offset - cache_start > cache_len) return -1;
	unsigned long long rel = offset - cache_start;
	unsigned long long avail = cache_len - rel;
	if (len > avail) {
		if (!__pachaos_filed_fds[idx].read_cache_eof) return -1;
		len = avail;
	}
	for (unsigned long long i = 0; i < len; i++) {
		dst[i] = __pachaos_filed_fds[idx].read_cache[rel + i];
	}
	return (long)len;
#else
	(void)idx;
	(void)offset;
	(void)len;
	(void)dst;
	return -1;
#endif
}

static __inline int __pachaos_filed_fd_read_cache_equals_linear(
	int idx,
	unsigned long long offset,
	unsigned long long len,
	const unsigned char *src)
{
#if PACHAOS_FILED_READ_CACHE_BYTES > 0
	if (idx < 0 || (len > 0 && !src) || !__pachaos_filed_fds[idx].read_cache_valid) return 0;
	unsigned long long cache_start = __pachaos_filed_fds[idx].read_cache_offset;
	unsigned long long cache_len = __pachaos_filed_fds[idx].read_cache_len;
	if (offset < cache_start || offset - cache_start > cache_len) return 0;
	unsigned long long rel = offset - cache_start;
	unsigned long long avail = cache_len - rel;
	if (len > avail) return 0;
	for (unsigned long long i = 0; i < len; i++) {
		if (__pachaos_filed_fds[idx].read_cache[rel + i] != src[i]) return 0;
	}
	return 1;
#else
	(void)idx;
	(void)offset;
	(void)len;
	(void)src;
	return 0;
#endif
}

static __inline long __pachaos_filed_fd_read_cache_copy_to_iov(
	int idx,
	unsigned long long offset,
	const struct __pachaos_iovec *iov,
	long count)
{
#if PACHAOS_FILED_READ_CACHE_BYTES > 0
	if (idx < 0 || count < 0 || (count > 0 && !iov)) return -1;
	unsigned long long len = 0;
	for (long i = 0; i < count; i++) {
		if (iov[i].len != 0 && !iov[i].base) return -1;
		if (iov[i].len > ~0ULL - len) return -1;
		len += iov[i].len;
	}
	if (!__pachaos_filed_fds[idx].read_cache_valid) return -1;
	unsigned long long cache_start = __pachaos_filed_fds[idx].read_cache_offset;
	unsigned long long cache_len = __pachaos_filed_fds[idx].read_cache_len;
	if (offset < cache_start || offset - cache_start > cache_len) return -1;
	unsigned long long rel = offset - cache_start;
	unsigned long long avail = cache_len - rel;
	if (len > avail) {
		if (!__pachaos_filed_fds[idx].read_cache_eof) return -1;
		len = avail;
	}
	unsigned long long copied = 0;
	for (long i = 0; i < count && copied < len; i++) {
		unsigned long long take = iov[i].len;
		if (take > len - copied) take = len - copied;
		for (unsigned long long j = 0; j < take; j++) {
			((unsigned char *)iov[i].base)[j] = __pachaos_filed_fds[idx].read_cache[rel + copied + j];
		}
		copied += take;
	}
	return (long)copied;
#else
	(void)idx;
	(void)offset;
	(void)iov;
	(void)count;
	return -1;
#endif
}

static __inline void __pachaos_filed_fd_store_read_cache_idx(
	int idx,
	unsigned long long offset,
	const unsigned char *src,
	unsigned long long len,
	int eof)
{
#if PACHAOS_FILED_READ_CACHE_BYTES > 0
	if (idx < 0 || !src) return;
	if (len > PACHAOS_FILED_READ_CACHE_BYTES) {
		__pachaos_filed_fds[idx].read_cache_valid = 0;
		__pachaos_filed_fds[idx].read_cache_eof = 0;
		__pachaos_filed_fds[idx].read_cache_len = 0;
		return;
	}
	if (offset > ~0ULL - len) return;
	unsigned long long write_start = offset;
	unsigned long long write_end = offset + len;
	unsigned long long new_start = write_start;
	unsigned long long new_end = write_end;
	int new_eof = eof ? 1 : 0;
	if (__pachaos_filed_fds[idx].read_cache_valid) {
		unsigned long long cache_start = __pachaos_filed_fds[idx].read_cache_offset;
		unsigned long long cache_len = __pachaos_filed_fds[idx].read_cache_len;
		if (cache_start <= ~0ULL - cache_len) {
			unsigned long long cache_end = cache_start + cache_len;
			if (write_start <= cache_end && cache_start <= write_end) {
				new_start = __pachaos_min_ull(cache_start, write_start);
				new_end = __pachaos_max_ull(cache_end, write_end);
				if (new_end >= new_start && new_end - new_start <= PACHAOS_FILED_READ_CACHE_BYTES) {
					if (new_start < cache_start) {
						unsigned long long shift = cache_start - new_start;
						for (unsigned long long i = cache_len; i > 0; i--) {
							__pachaos_filed_fds[idx].read_cache[shift + i - 1] =
								__pachaos_filed_fds[idx].read_cache[i - 1];
						}
					}
					new_eof = eof || (__pachaos_filed_fds[idx].read_cache_eof && write_start <= cache_end);
				} else {
					new_start = write_start;
					new_end = write_end;
				}
			}
		}
	}
	__pachaos_filed_fds[idx].read_cache_valid = 1;
	__pachaos_filed_fds[idx].read_cache_eof = (unsigned char)new_eof;
	__pachaos_filed_fds[idx].read_cache_offset = new_start;
	__pachaos_filed_fds[idx].read_cache_len = (unsigned short)(new_end - new_start);
	for (unsigned long long i = 0; i < len; i++) {
		__pachaos_filed_fds[idx].read_cache[write_start - new_start + i] = src[i];
	}
#else
	(void)idx;
	(void)offset;
	(void)src;
	(void)len;
	(void)eof;
#endif
}

static __inline void __pachaos_filed_fd_store_read_cache_from_iov_idx(
	int idx,
	unsigned long long offset,
	const struct __pachaos_iovec *iov,
	long count,
	unsigned long long len,
	int eof)
{
#if PACHAOS_FILED_READ_CACHE_BYTES > 0
	if (idx < 0 || !iov || len > PACHAOS_FILED_READ_CACHE_BYTES) {
		if (idx >= 0 && len > PACHAOS_FILED_READ_CACHE_BYTES) {
			__pachaos_filed_fds[idx].read_cache_valid = 0;
			__pachaos_filed_fds[idx].read_cache_eof = 0;
			__pachaos_filed_fds[idx].read_cache_len = 0;
		}
		return;
	}
	unsigned char tmp[PACHAOS_FILED_READ_CACHE_STORAGE_BYTES];
	unsigned long long copied = 0;
	for (long i = 0; i < count && copied < len; i++) {
		unsigned long long take = iov[i].len;
		if (take > len - copied) take = len - copied;
		for (unsigned long long j = 0; j < take; j++) {
			tmp[copied + j] = ((const unsigned char *)iov[i].base)[j];
		}
		copied += take;
	}
	if (copied == len) __pachaos_filed_fd_store_read_cache_idx(idx, offset, tmp, len, eof);
#else
	(void)idx;
	(void)offset;
	(void)iov;
	(void)count;
	(void)len;
	(void)eof;
#endif
}

static __inline int __pachaos_filed_fd_should_update_read_cache_for_write_idx(
	int idx,
	unsigned long long offset,
	unsigned long long len)
{
#if PACHAOS_FILED_READ_CACHE_BYTES > 0
	if (idx < 0 || len == 0) return 0;
	if (!__pachaos_filed_fds[idx].read_cache_valid) return 1;
	if (offset > ~0ULL - len) {
		__pachaos_filed_fds[idx].read_cache_valid = 0;
		__pachaos_filed_fds[idx].read_cache_eof = 0;
		__pachaos_filed_fds[idx].read_cache_len = 0;
		__pachaos_filed_fds[idx].read_cache_offset = 0;
		return 0;
	}
	unsigned long long write_start = offset;
	unsigned long long write_end = offset + len;
	unsigned long long cache_start = __pachaos_filed_fds[idx].read_cache_offset;
	unsigned long long cache_len = __pachaos_filed_fds[idx].read_cache_len;
	if (cache_start > ~0ULL - cache_len) {
		__pachaos_filed_fds[idx].read_cache_valid = 0;
		__pachaos_filed_fds[idx].read_cache_eof = 0;
		__pachaos_filed_fds[idx].read_cache_len = 0;
		__pachaos_filed_fds[idx].read_cache_offset = 0;
		return 0;
	}
	unsigned long long cache_end = cache_start + cache_len;
	if (write_end < cache_start || cache_end < write_start) return 0;
	unsigned long long new_start = __pachaos_min_ull(cache_start, write_start);
	unsigned long long new_end = __pachaos_max_ull(cache_end, write_end);
	if (new_end < new_start || new_end - new_start > PACHAOS_FILED_READ_CACHE_BYTES) {
		__pachaos_filed_fds[idx].read_cache_valid = 0;
		__pachaos_filed_fds[idx].read_cache_eof = 0;
		__pachaos_filed_fds[idx].read_cache_len = 0;
		__pachaos_filed_fds[idx].read_cache_offset = 0;
		return 0;
	}
	return 1;
#else
	(void)idx;
	(void)offset;
	(void)len;
	return 0;
#endif
}

static __inline void __pachaos_filed_fd_note_write_read_cache_idx(
	int idx,
	unsigned long long offset,
	const unsigned char *src,
	unsigned long long len)
{
#if PACHAOS_FILED_READ_CACHE_BYTES > 0
	if (__pachaos_filed_fd_should_update_read_cache_for_write_idx(idx, offset, len)) {
		__pachaos_filed_fd_store_read_cache_idx(idx, offset, src, len, 0);
	}
#else
	(void)idx;
	(void)offset;
	(void)src;
	(void)len;
#endif
}

static __inline void __pachaos_filed_fd_note_write_read_cache_from_iov_idx(
	int idx,
	unsigned long long offset,
	const struct __pachaos_iovec *iov,
	long count,
	unsigned long long len)
{
#if PACHAOS_FILED_READ_CACHE_BYTES > 0
	if (__pachaos_filed_fd_should_update_read_cache_for_write_idx(idx, offset, len)) {
		__pachaos_filed_fd_store_read_cache_from_iov_idx(idx, offset, iov, count, len, 0);
	}
#else
	(void)idx;
	(void)offset;
	(void)iov;
	(void)count;
	(void)len;
#endif
}

static __inline long __pachaos_filed_refresh_generation_from_stat_idx(int idx)
{
	if (idx < 0 || !__pachaos_filed_fd_used[idx]) return 0;
	unsigned long long handle = __pachaos_filed_fds[idx].handle;
	unsigned long long old_generation = __pachaos_filed_fds[idx].object_generation;
	if (!handle || old_generation == 0) return 0;

	unsigned long long shared_object_generation = 0;
	unsigned long long shared_dir_generation = 0;
	if (__pachaos_filed_shared_generation_lookup(
			handle,
			&shared_object_generation,
			&shared_dir_generation))
	{
		if (shared_object_generation == old_generation) return 1;
		__pachaos_filed_fds[idx].object_generation = shared_object_generation;
		__pachaos_filed_fds[idx].dir_generation = shared_dir_generation;
		return 2;
	}

	long page_fd = -1;
	unsigned char *page = 0;
	long status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return 0;
	__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
	struct __pachaos_filed_statx *st = (struct __pachaos_filed_statx *)page;
	st->handle = handle;
	struct __pachaos_ipc_msg reply;
	status = __pachaos_filed_call(PACHAOS_FILED_OP_STAT, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
	unsigned long long new_generation = 0;
	if (status == 0) {
		new_generation = st->object_generation;
		__pachaos_filed_fds[idx].stat_valid = 1;
		__pachaos_filed_fds[idx].stat_mode = st->mode;
		__pachaos_filed_fds[idx].stat_size = st->size;
		__pachaos_filed_fds[idx].stat_blocks = st->blocks;
		__pachaos_filed_fds[idx].stat_nlink = st->nlink ? st->nlink : 1;
		__pachaos_filed_fds[idx].stat_kind = st->kind;
		__pachaos_filed_fds[idx].object_generation = st->object_generation;
		__pachaos_filed_fds[idx].dir_generation = st->dir_generation;
	}
	__pachaos_filed_page_destroy(page_fd, page);
	if (status != 0) return 0;
	return new_generation == old_generation ? 1 : 2;
}

static __inline void __pachaos_filed_fd_validate_read_cache_idx(int idx)
{
#if PACHAOS_FILED_READ_CACHE_BYTES > 0
	if (idx < 0 || !__pachaos_filed_fds[idx].read_cache_valid) return;
	if (__pachaos_filed_refresh_generation_from_stat_idx(idx) != 1) {
		__pachaos_filed_fds[idx].read_cache_valid = 0;
		__pachaos_filed_fds[idx].read_cache_eof = 0;
		__pachaos_filed_fds[idx].read_cache_len = 0;
		__pachaos_filed_fds[idx].read_cache_offset = 0;
	}
#else
	(void)idx;
#endif
}

static __inline void __pachaos_filed_close_handle(unsigned long long handle, long page_fd);

static __inline int __pachaos_filed_open_can_use_cache(long dirfd, const char *path, long flags)
{
	if (!path || path[0] != '/') return 0;
	if (dirfd != LINUX_AT_FDCWD) return 0;
	if (flags & (LINUX_O_EXCL|LINUX_O_TRUNC)) return 0;
	return 1;
}

static __inline int __pachaos_filed_relative_open_name_ok(const char *path)
{
	if (!path || !path[0]) return 0;
	if (path[0] == '.' && (!path[1] || (path[1] == '.' && !path[2]))) return 0;
	for (long i = 0; i < PACHAOS_FILED_NAME_BYTES; i++) {
		char c = path[i];
		if (!c) return 1;
		if (c == '/') return 0;
	}
	return 0;
}

static __inline int __pachaos_filed_make_open_cache_key(
	long dirfd,
	const char *path,
	long flags,
	char *out)
{
	if (!out) return -22;
	out[0] = 0;
	if (!path) return 0;
	if (flags & (LINUX_O_EXCL|LINUX_O_TRUNC)) return 0;
	if (path[0] == '/') {
		if (dirfd != LINUX_AT_FDCWD) return 0;
		long status = __pachaos_copy_path(out, path, PACHAOS_FILED_NAME_BYTES);
		return status == 0 ? 1 : (int)status;
	}
	if (dirfd == LINUX_AT_FDCWD) return 0;
	if (!__pachaos_filed_relative_open_name_ok(path)) return 0;
	int dir_idx = __pachaos_filed_fd_index(dirfd);
	if (dir_idx < 0) return 0;
	const char *base = __pachaos_filed_fds[dir_idx].open_cache_path;
	if (!base || base[0] != '/') return 0;
	long pos = 0;
	for (; pos < PACHAOS_FILED_NAME_BYTES - 1 && base[pos]; pos++) {
		out[pos] = base[pos];
	}
	if (pos == 0 || pos >= PACHAOS_FILED_NAME_BYTES - 1) {
		out[0] = 0;
		return 0;
	}
	if (out[pos - 1] != '/') {
		if (pos >= PACHAOS_FILED_NAME_BYTES - 1) {
			out[0] = 0;
			return 0;
		}
		out[pos++] = '/';
	}
	for (long i = 0; i < PACHAOS_FILED_NAME_BYTES; i++) {
		char c = path[i];
		if (pos >= PACHAOS_FILED_NAME_BYTES - 1) {
			out[0] = 0;
			return -36;
		}
		out[pos++] = c;
		if (!c) return 1;
	}
	out[0] = 0;
	return -36;
}

static __inline long __pachaos_filed_open_cache_take(
	const char *path,
	unsigned int rights,
	unsigned int open_flags,
	struct __pachaos_filed_open_cache_entry *out_entry)
{
	if (out_entry) __pachaos_bzero(out_entry, sizeof(*out_entry));
	for (int i = 0; i < PACHAOS_FILED_OPEN_CACHE_CAP; i++) {
		if (__pachaos_filed_open_cache[i].used &&
			__pachaos_filed_open_cache[i].rights == rights &&
			__pachaos_filed_open_cache[i].open_flags == open_flags &&
			__pachaos_str_equal(__pachaos_filed_open_cache[i].path, path))
		{
			unsigned long long handle = __pachaos_filed_open_cache[i].handle;
			if (out_entry) {
				*out_entry = __pachaos_filed_open_cache[i];
				out_entry->slot_index = (unsigned char)i;
			}
			__pachaos_bzero(&__pachaos_filed_open_cache[i], sizeof(__pachaos_filed_open_cache[i]));
			return (long)handle;
		}
	}
	return 0;
}

static __inline void __pachaos_filed_open_cache_store(
	unsigned long long handle,
	const char *path,
	unsigned int rights,
	unsigned int open_flags,
	unsigned long long object_generation,
	unsigned long long dir_generation,
	unsigned char stat_valid,
	unsigned long long stat_mode,
	unsigned long long stat_size,
	unsigned long long stat_blocks,
	unsigned long long stat_nlink,
	unsigned long long stat_kind,
	unsigned char read_cache_valid,
	unsigned char read_cache_eof,
	unsigned short read_cache_len,
	unsigned long long read_cache_offset,
	const unsigned char *read_cache)
{
	if (!handle || !path) return;
	int slot = -1;
	for (int i = 0; i < PACHAOS_FILED_OPEN_CACHE_CAP; i++) {
		if (!__pachaos_filed_open_cache[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		slot = 0;
		__pachaos_filed_close_handle(__pachaos_filed_open_cache[slot].handle, -1);
	}
	__pachaos_bzero(&__pachaos_filed_open_cache[slot], sizeof(__pachaos_filed_open_cache[slot]));
	__pachaos_filed_open_cache[slot].used = 1;
	__pachaos_filed_open_cache[slot].rights = rights;
	__pachaos_filed_open_cache[slot].open_flags = open_flags;
	__pachaos_filed_open_cache[slot].handle = handle;
	__pachaos_filed_open_cache[slot].object_generation = object_generation;
	__pachaos_filed_open_cache[slot].dir_generation = dir_generation;
	__pachaos_filed_open_cache[slot].stat_valid = stat_valid;
	__pachaos_filed_open_cache[slot].stat_mode = stat_mode;
	__pachaos_filed_open_cache[slot].stat_size = stat_size;
	__pachaos_filed_open_cache[slot].stat_blocks = stat_blocks;
	__pachaos_filed_open_cache[slot].stat_nlink = stat_nlink;
	__pachaos_filed_open_cache[slot].stat_kind = stat_kind;
	if (read_cache_valid && read_cache && read_cache_len <= PACHAOS_FILED_READ_CACHE_BYTES) {
		__pachaos_filed_open_cache[slot].read_cache_valid = 1;
		__pachaos_filed_open_cache[slot].read_cache_eof = read_cache_eof;
		__pachaos_filed_open_cache[slot].read_cache_len = read_cache_len;
		__pachaos_filed_open_cache[slot].read_cache_offset = read_cache_offset;
		for (unsigned long long i = 0; i < read_cache_len; i++) {
			__pachaos_filed_open_cache_read_data[slot][i] = read_cache[i];
		}
	}
	(void)__pachaos_copy_path(__pachaos_filed_open_cache[slot].path, path, PACHAOS_FILED_NAME_BYTES);
}

static __inline void __pachaos_filed_open_cache_clear_all(void)
{
	for (int i = 0; i < PACHAOS_FILED_OPEN_CACHE_CAP; i++) {
		if (__pachaos_filed_open_cache[i].used) {
			__pachaos_filed_close_handle(__pachaos_filed_open_cache[i].handle, -1);
			__pachaos_bzero(&__pachaos_filed_open_cache[i], sizeof(__pachaos_filed_open_cache[i]));
		}
	}
}

static __inline void __pachaos_filed_fd_mark_open_cacheable(
	long fd,
	const char *path,
	unsigned int rights,
	unsigned int open_flags)
{
	int idx = __pachaos_filed_fd_index(fd);
	if (idx < 0 || !path) return;
	__pachaos_filed_fds[idx].open_cacheable = 1;
	__pachaos_filed_fds[idx].open_cache_rights = rights;
	__pachaos_filed_fds[idx].open_cache_open_flags = open_flags;
	(void)__pachaos_copy_path(__pachaos_filed_fds[idx].open_cache_path, path, PACHAOS_FILED_NAME_BYTES);
}

static __inline void __pachaos_filed_write_kstat_from_fd_cache(int idx, struct __pachaos_kstat *out)
{
	__pachaos_bzero(out, sizeof(*out));
	out->st_ino = __pachaos_filed_fds[idx].handle;
	out->st_nlink = __pachaos_filed_fds[idx].stat_nlink ? __pachaos_filed_fds[idx].stat_nlink : 1;
	out->st_mode = (unsigned int)__pachaos_filed_fds[idx].stat_mode;
	out->st_size = (long long)__pachaos_filed_fds[idx].stat_size;
	out->st_blksize = 4096;
	out->st_blocks = (long long)__pachaos_filed_fds[idx].stat_blocks;
}

static __inline void __pachaos_filed_close_handle(unsigned long long handle, long page_fd)
{
	if (!handle) return;
	unsigned char *page = 0;
	int owned_page = 0;
	if (page_fd < 16 && __pachaos_filed_session_fd < 16) {
		if (__pachaos_filed_page_create(&page_fd, &page) != 0) return;
		owned_page = 1;
	}
	struct __pachaos_ipc_msg reply;
	(void)__pachaos_filed_call(PACHAOS_FILED_OP_CLOSE, (long)handle, __pachaos_filed_next_request_id(), page_fd, &reply);
	if (owned_page) __pachaos_filed_page_destroy(page_fd, page);
}

static __inline long __pachaos_filed_seek_handle(unsigned long long handle, long offset, long whence)
{
	if (!handle) return -22;
	long page_fd = -1;
	unsigned char *page = 0;
	long status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;
	__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
	struct __pachaos_filed_seek *seek = (struct __pachaos_filed_seek *)page;
	seek->handle = handle;
	seek->offset = offset;
	seek->whence = (unsigned long long)whence;
	struct __pachaos_ipc_msg reply;
	status = __pachaos_filed_call(PACHAOS_FILED_OP_SEEK, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
	__pachaos_filed_page_destroy(page_fd, page);
	return status == 0 ? (long)reply.word2 : status;
}

static __inline long __pachaos_filed_validate_open_cache(
	unsigned long long cached_handle,
	const char *path,
	unsigned int rights,
	unsigned int open_flags,
	unsigned long long object_generation,
	unsigned long long dir_generation)
{
	if (!cached_handle || !path || object_generation == 0) return 0;
	long page_fd = -1;
	unsigned char *page = 0;
	long status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;
	__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
	struct __pachaos_filed_validate_open_cache *validate =
		(struct __pachaos_filed_validate_open_cache *)page;
	validate->cached_handle = cached_handle;
	validate->dir_handle = 0;
	validate->rights = rights;
	validate->open_flags = open_flags;
	validate->object_generation = object_generation;
	validate->dir_generation = dir_generation;
	status = __pachaos_copy_path(validate->name, path, PACHAOS_FILED_NAME_BYTES);
	if (status != 0) {
		__pachaos_filed_page_destroy(page_fd, page);
		return status;
	}
	struct __pachaos_ipc_msg reply;
	status = __pachaos_filed_call(
		PACHAOS_FILED_OP_VALIDATE_OPEN_CACHE,
		0,
		__pachaos_filed_next_request_id(),
		page_fd,
		&reply);
	__pachaos_filed_page_destroy(page_fd, page);
	if (status != 0) return status;
	return reply.word2 == 1 ? 1 : 0;
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
	const unsigned int rights = __pachaos_filed_rights_from_linux(flags);
	const unsigned int open_flags = __pachaos_filed_open_flags_from_linux(flags);
	char path_key[PACHAOS_FILED_NAME_BYTES];
	int cacheable = __pachaos_filed_make_open_cache_key(dirfd, path, flags, path_key);
	if (cacheable < 0) return cacheable;
	long status;
	if (cacheable) {
		struct __pachaos_filed_open_cache_entry cached_entry;
		long cached_handle = __pachaos_filed_open_cache_take(
			path_key,
			rights,
			open_flags,
			&cached_entry);
		if (cached_handle > 0) {
			unsigned long long shared_object_generation = 0;
			unsigned long long shared_dir_generation = 0;
			long valid = 0;
			if (__pachaos_filed_shared_generation_lookup(
					(unsigned long long)cached_handle,
					&shared_object_generation,
					&shared_dir_generation) &&
				shared_object_generation == cached_entry.object_generation &&
				shared_dir_generation == cached_entry.dir_generation)
			{
				valid = 1;
			} else {
				valid = __pachaos_filed_validate_open_cache(
					(unsigned long long)cached_handle,
					path_key,
					rights,
					open_flags,
					cached_entry.object_generation,
					cached_entry.dir_generation);
			}
			if (valid != 1) {
				__pachaos_filed_close_handle((unsigned long long)cached_handle, -1);
			} else {
			unsigned int status_flags = __pachaos_filed_status_flags_from_open(flags);
			if (open_flags & PACHAOS_FILED_OPEN_DIRECTORY) {
				status_flags |= PACHAOS_FILED_FILE_NEEDS_REWIND;
			}
			long fd = __pachaos_filed_fd_alloc_from(
				(unsigned long long)cached_handle,
				0,
				__pachaos_filed_fd_flags_from_open(flags),
				status_flags);
			if (fd >= 0) {
				int fd_idx = __pachaos_filed_fd_index(fd);
				if (fd_idx >= 0) {
					__pachaos_filed_fds[fd_idx].object_generation = cached_entry.object_generation;
					__pachaos_filed_fds[fd_idx].dir_generation = cached_entry.dir_generation;
					__pachaos_filed_fds[fd_idx].stat_valid = cached_entry.stat_valid;
					__pachaos_filed_fds[fd_idx].stat_mode = cached_entry.stat_mode;
					__pachaos_filed_fds[fd_idx].stat_size = cached_entry.stat_size;
					__pachaos_filed_fds[fd_idx].stat_blocks = cached_entry.stat_blocks;
					__pachaos_filed_fds[fd_idx].stat_nlink = cached_entry.stat_nlink;
					__pachaos_filed_fds[fd_idx].stat_kind = cached_entry.stat_kind;
					if (cached_entry.read_cache_valid &&
						cached_entry.read_cache_len <= PACHAOS_FILED_READ_CACHE_BYTES &&
						cached_entry.slot_index < PACHAOS_FILED_OPEN_CACHE_CAP)
					{
						__pachaos_filed_fds[fd_idx].read_cache_valid = 1;
						__pachaos_filed_fds[fd_idx].read_cache_eof = cached_entry.read_cache_eof;
						__pachaos_filed_fds[fd_idx].read_cache_len = cached_entry.read_cache_len;
						__pachaos_filed_fds[fd_idx].read_cache_offset = cached_entry.read_cache_offset;
						for (unsigned long long i = 0; i < cached_entry.read_cache_len; i++) {
							__pachaos_filed_fds[fd_idx].read_cache[i] =
								__pachaos_filed_open_cache_read_data[cached_entry.slot_index][i];
						}
					}
				}
				__pachaos_filed_fd_mark_open_cacheable(fd, path_key, rights, open_flags);
				return fd;
			}
			__pachaos_filed_open_cache_store(
				(unsigned long long)cached_handle,
				path_key,
				rights,
				open_flags,
				cached_entry.object_generation,
				cached_entry.dir_generation,
				cached_entry.stat_valid,
				cached_entry.stat_mode,
				cached_entry.stat_size,
				cached_entry.stat_blocks,
				cached_entry.stat_nlink,
				cached_entry.stat_kind,
				cached_entry.read_cache_valid,
				cached_entry.read_cache_eof,
				cached_entry.read_cache_len,
				cached_entry.read_cache_offset,
				cached_entry.slot_index < PACHAOS_FILED_OPEN_CACHE_CAP ?
					__pachaos_filed_open_cache_read_data[cached_entry.slot_index] : 0);
			return fd;
			}
		}
	}
	long page_fd = -1;
	unsigned char *page = 0;
	status = __pachaos_filed_page_create(&page_fd, &page);
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
	openat->rights = rights;
	openat->open_flags = open_flags;
	status = __pachaos_copy_path(openat->name, path, PACHAOS_FILED_NAME_BYTES);
	if (status != 0) {
		__pachaos_filed_page_destroy(page_fd, page);
		return status;
	}

	struct __pachaos_ipc_msg reply;
	status = __pachaos_filed_call(PACHAOS_FILED_OP_OPENAT, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
	__pachaos_filed_page_destroy(page_fd, page);
	if (status != 0) return status;
	status = __pachaos_filed_fd_alloc_from(
		reply.word2,
		0,
		__pachaos_filed_fd_flags_from_open(flags),
		__pachaos_filed_status_flags_from_open(flags));
	if (status < 0) {
		__pachaos_filed_close_handle(reply.word2, -1);
	} else {
		int fd_idx = __pachaos_filed_fd_index(status);
		if (fd_idx >= 0) {
			__pachaos_filed_fds[fd_idx].object_generation = openat->object_generation;
			__pachaos_filed_fds[fd_idx].dir_generation = openat->dir_generation;
		}
	}
	if (status >= 0 && cacheable) {
		__pachaos_filed_fd_mark_open_cacheable(status, path_key, rights, open_flags);
	}
	return status;
}

static __inline long __pachaos_filed_read(long fd, void *buf, long len)
{
	unsigned long long handle = __pachaos_filed_fd_handle(fd);
	if (!handle) return __pachaos_raw3(PACHAOS_SYSCALL_FD_READ, fd, (long)buf, len);
	if (len < 0 || (len > 0 && !buf)) return -22;
	if (len == 0) return 0;
	int idx = __pachaos_filed_fd_index(fd);
	unsigned long long original_offset = __pachaos_filed_fd_offset(fd);
	__pachaos_filed_fd_validate_read_cache_idx(idx);
	long cached = __pachaos_filed_fd_read_cache_copy_to_linear(
		idx,
		original_offset,
		(unsigned long long)len,
		(unsigned char *)buf);
	if (cached >= 0) {
		__pachaos_filed_fd_advance_offset(fd, (unsigned long long)cached);
		return cached;
	}
	long page_fd = -1;
	unsigned char *page = 0;
	long status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;
#if PACHAOS_FILED_READ_CACHE_BYTES > 0
	if ((unsigned long long)len <= PACHAOS_FILED_READ_CACHE_BYTES) {
		unsigned long long want = PACHAOS_FILED_READ_CACHE_BYTES;
		if (want > PACHAOS_FILED_IO_BYTES) want = PACHAOS_FILED_IO_BYTES;
		__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
		struct __pachaos_filed_io *io = (struct __pachaos_filed_io *)page;
		io->handle = handle;
		io->offset = original_offset;
		io->length = want;
		struct __pachaos_ipc_msg reply;
		status = __pachaos_filed_call(PACHAOS_FILED_OP_PREAD, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
		if (status == 0) {
			long got = (long)reply.word2;
			if (got < 0) got = 0;
			if ((unsigned long long)got > want) got = (long)want;
			long user_got = got;
			if (user_got > len) user_got = len;
			for (long i = 0; i < user_got; i++) ((unsigned char *)buf)[i] = io->data[i];
			if (got > 0) {
				__pachaos_filed_fd_store_read_cache_idx(
					idx,
					original_offset,
					io->data,
					(unsigned long long)got,
					(unsigned long long)got < want);
			}
			__pachaos_filed_page_destroy(page_fd, page);
			if (user_got > 0) {
				__pachaos_filed_fd_advance_offset(fd, (unsigned long long)user_got);
			}
			return user_got;
		}
		__pachaos_filed_page_destroy(page_fd, page);
		return status;
	}
#endif
	long done = 0;
	while (done < len) {
		long want = len - done;
		if (want > PACHAOS_FILED_IO_BYTES) want = PACHAOS_FILED_IO_BYTES;
		__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
		struct __pachaos_filed_io *io = (struct __pachaos_filed_io *)page;
		io->handle = handle;
		io->offset = __pachaos_filed_fd_offset(fd) + (unsigned long long)done;
		io->length = (unsigned long long)want;
		struct __pachaos_ipc_msg reply;
		status = __pachaos_filed_call(PACHAOS_FILED_OP_PREAD, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
		if (status != 0) break;
		long got = (long)reply.word2;
		if (got <= 0) break;
		if (got > want) got = want;
		for (long i = 0; i < got; i++) ((unsigned char *)buf)[done + i] = io->data[i];
		done += got;
		if (got < want) break;
	}
	__pachaos_filed_page_destroy(page_fd, page);
	if (done > 0) {
		__pachaos_filed_fd_advance_offset(fd, (unsigned long long)done);
		__pachaos_filed_fd_store_read_cache_idx(
			idx,
			original_offset,
			(const unsigned char *)buf,
			(unsigned long long)done,
			done < len);
	}
	return done > 0 ? done : status;
}

static __inline long __pachaos_filed_write(long fd, const void *buf, long len)
{
	unsigned long long handle = __pachaos_filed_fd_handle(fd);
	if (!handle) return __pachaos_raw3(PACHAOS_SYSCALL_FD_WRITE, fd, (long)buf, len);
	if (len < 0 || (len > 0 && !buf)) return -22;
	if (len == 0) return 0;
	int idx = __pachaos_filed_fd_index(fd);
	unsigned long long original_offset = __pachaos_filed_fd_offset(fd);
	int append = (__pachaos_filed_status_flags(fd) & PACHAOS_FILED_FILE_APPEND) != 0;
	if (!append) {
		__pachaos_filed_fd_validate_read_cache_idx(idx);
	}
	if (!append &&
		original_offset <= ~0ULL - (unsigned long long)len &&
		__pachaos_filed_fd_read_cache_equals_linear(
			idx,
			original_offset,
			(unsigned long long)len,
			(const unsigned char *)buf))
	{
		__pachaos_filed_fd_advance_offset(fd, (unsigned long long)len);
		return len;
	}
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
		if (!append) {
			io->offset = __pachaos_filed_fd_offset(fd) + (unsigned long long)done;
		}
		io->length = (unsigned long long)want;
		for (long i = 0; i < want; i++) io->data[i] = ((const unsigned char *)buf)[done + i];
		struct __pachaos_ipc_msg reply;
		status = __pachaos_filed_call(
			append ? PACHAOS_FILED_OP_WRITE : PACHAOS_FILED_OP_PWRITE,
			0,
			__pachaos_filed_next_request_id(),
			page_fd,
			&reply);
		if (status != 0) break;
		long wrote = (long)reply.word2;
		if (wrote <= 0) break;
		if (wrote > want) wrote = want;
		done += wrote;
		if (wrote < want) break;
	}
	__pachaos_filed_page_destroy(page_fd, page);
	if (done > 0) {
		__pachaos_filed_fd_advance_offset(fd, (unsigned long long)done);
		if (append) {
			__pachaos_filed_fd_invalidate_cached_state_idx(idx);
		} else {
			__pachaos_filed_fd_note_write_stat_idx(
				idx,
				original_offset,
				(unsigned long long)done);
			__pachaos_filed_fd_note_write_read_cache_idx(
				idx,
				original_offset,
				(const unsigned char *)buf,
				(unsigned long long)done);
		}
		if (idx >= 0 && __pachaos_filed_fds[idx].object_generation != 0) {
			++__pachaos_filed_fds[idx].object_generation;
		}
	}
	return done > 0 ? done : status;
}

static __inline long __pachaos_filed_readv(long fd, const struct __pachaos_iovec *iov, long count)
{
	unsigned long long handle = __pachaos_filed_fd_handle(fd);
	if (!handle) {
		return __pachaos_raw3(PACHAOS_SYSCALL_FD_READV, fd, (long)iov, count);
	}
	if (count < 0 || (count > 0 && !iov)) return -22;
	for (long i = 0; i < count; i++) {
		if (iov[i].len != 0 && !iov[i].base) return -22;
	}
	int idx = __pachaos_filed_fd_index(fd);
	unsigned long long original_offset = __pachaos_filed_fd_offset(fd);
	unsigned long long requested = 0;
	for (long j = 0; j < count; j++) {
		if (iov[j].len > ~0ULL - requested) return -22;
		requested += iov[j].len;
	}
	__pachaos_filed_fd_validate_read_cache_idx(idx);
	long cached = __pachaos_filed_fd_read_cache_copy_to_iov(idx, original_offset, iov, count);
	if (cached >= 0) {
		__pachaos_filed_fd_advance_offset(fd, (unsigned long long)cached);
		return cached;
	}

	long page_fd = -1;
	unsigned char *page = 0;
	long status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;
	status = __pachaos_filed_session_connect(page_fd);
	if (status != 0) {
		__pachaos_filed_page_destroy(page_fd, page);
		return status;
	}

	long done = 0;
	long i = 0;
	unsigned long iov_off = 0;
	while (i < count) {
		while (i < count && iov[i].len == iov_off) {
			i++;
			iov_off = 0;
		}
		if (i >= count) break;

		unsigned long want = 0;
		for (long j = i; j < count && want < PACHAOS_FILED_IO_BYTES; j++) {
			unsigned long off = j == i ? iov_off : 0;
			unsigned long avail = iov[j].len - off;
			unsigned long room = PACHAOS_FILED_IO_BYTES - want;
			want += avail < room ? avail : room;
			if (avail >= room) break;
		}
		if (want == 0) break;

		__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
		struct __pachaos_filed_io *io = (struct __pachaos_filed_io *)page;
		io->handle = handle;
		io->offset = __pachaos_filed_fd_offset(fd) + (unsigned long long)done;
		io->length = want;
		struct __pachaos_ipc_msg reply;
		status = __pachaos_filed_call(PACHAOS_FILED_OP_PREAD, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
		if (status != 0) break;

		long got = (long)reply.word2;
		if (got <= 0) break;
		if ((unsigned long)got > want) got = (long)want;

		long copied = 0;
		while (copied < got && i < count) {
			unsigned long avail = iov[i].len - iov_off;
			unsigned long take = (unsigned long)(got - copied);
			if (take > avail) take = avail;
			for (unsigned long k = 0; k < take; k++) {
				((unsigned char *)iov[i].base)[iov_off + k] = io->data[copied + (long)k];
			}
			copied += (long)take;
			iov_off += take;
			if (iov_off == iov[i].len) {
				i++;
				iov_off = 0;
			}
		}
		done += got;
		if ((unsigned long)got < want) break;
	}
	__pachaos_filed_page_destroy(page_fd, page);
	if (done > 0) {
		__pachaos_filed_fd_advance_offset(fd, (unsigned long long)done);
		__pachaos_filed_fd_store_read_cache_from_iov_idx(
			idx,
			original_offset,
			iov,
			count,
			(unsigned long long)done,
			(unsigned long long)done < requested);
	}
	return done > 0 ? done : status;
}

static __inline long __pachaos_filed_writev(long fd, const struct __pachaos_iovec *iov, long count)
{
	unsigned long long handle = __pachaos_filed_fd_handle(fd);
	if (!handle) {
		return __pachaos_raw3(PACHAOS_SYSCALL_FD_WRITEV, fd, (long)iov, count);
	}
	if (count < 0 || (count > 0 && !iov)) return -22;
	for (long i = 0; i < count; i++) {
		if (iov[i].len != 0 && !iov[i].base) return -22;
	}
	int idx = __pachaos_filed_fd_index(fd);
	unsigned long long original_offset = __pachaos_filed_fd_offset(fd);
	int append = (__pachaos_filed_status_flags(fd) & PACHAOS_FILED_FILE_APPEND) != 0;

	long page_fd = -1;
	unsigned char *page = 0;
	long status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;

	long done = 0;
	long i = 0;
	unsigned long iov_off = 0;
	while (i < count) {
		while (i < count && iov[i].len == iov_off) {
			i++;
			iov_off = 0;
		}
		if (i >= count) break;

		long gather_i = i;
		unsigned long gather_off = iov_off;
		unsigned long long batch_count = 0;
		unsigned long long batch_total = 0;
		while (batch_count < PACHAOS_FILED_FAST_PAYLOAD_SLOT_COUNT) {
			while (gather_i < count && iov[gather_i].len == gather_off) {
				gather_i++;
				gather_off = 0;
			}
			if (gather_i >= count) break;

			unsigned char *slot_page = __pachaos_filed_fast_payload_slot(batch_count);
			if (!slot_page) {
				status = -14;
				break;
			}
			__pachaos_bzero(slot_page, PACHAOS_FILED_PAGE_BYTES);
			struct __pachaos_filed_io *io = (struct __pachaos_filed_io *)slot_page;
			io->handle = handle;
			if (!append) {
				io->offset =
					__pachaos_filed_fd_offset(fd) +
					(unsigned long long)done +
					batch_total;
			}

			unsigned long want = 0;
			while (gather_i < count && want < PACHAOS_FILED_IO_BYTES) {
				unsigned long avail = iov[gather_i].len - gather_off;
				unsigned long room = PACHAOS_FILED_IO_BYTES - want;
				unsigned long take = avail < room ? avail : room;
				for (unsigned long k = 0; k < take; k++) {
					io->data[want + k] =
						((const unsigned char *)iov[gather_i].base)[gather_off + k];
				}
				want += take;
				gather_off += take;
				if (gather_off == iov[gather_i].len) {
					gather_i++;
					gather_off = 0;
				}
				if (take < avail) break;
			}
			if (want == 0) break;
			io->length = want;
			batch_total += want;
			batch_count++;
		}
		if (status != 0 || batch_count == 0) break;

		struct __pachaos_ipc_msg reply;
		if (batch_count == 1) {
			status = __pachaos_filed_call(
				append ? PACHAOS_FILED_OP_WRITE : PACHAOS_FILED_OP_PWRITE,
				0,
				__pachaos_filed_next_request_id(),
				page_fd,
				&reply);
			if (status != 0) break;
		} else {
			long request_id = __pachaos_filed_next_request_id();
			status = __pachaos_filed_fast_enqueue(
				append ? PACHAOS_FILED_OP_WRITE_BATCH : PACHAOS_FILED_OP_PWRITE_BATCH,
				(long)batch_count,
				request_id,
				0,
				PACHAOS_FILED_PAGE_BYTES);
			if (status != 0) break;
			status = __pachaos_filed_fast_doorbell(request_id, &reply);
			if (status != 0) break;
			status = __pachaos_filed_fast_wait_completion(request_id, &reply);
			if (status != 0) break;
		}

		long wrote = (long)reply.word2;
		if (wrote <= 0) break;
		if ((unsigned long long)wrote > batch_total) wrote = (long)batch_total;
		done += wrote;
		long advance = wrote;
		while (advance > 0 && i < count) {
			unsigned long avail = iov[i].len - iov_off;
			unsigned long take = (unsigned long)advance;
			if (take > avail) take = avail;
			iov_off += take;
			advance -= (long)take;
			if (iov_off == iov[i].len) {
				i++;
				iov_off = 0;
			}
		}
		if ((unsigned long long)wrote < batch_total) break;
	}
	__pachaos_filed_page_destroy(page_fd, page);
	if (done > 0) {
		__pachaos_filed_fd_advance_offset(fd, (unsigned long long)done);
		if (append) {
			__pachaos_filed_fd_invalidate_cached_state_idx(idx);
		} else {
			__pachaos_filed_fd_note_write_stat_idx(
				idx,
				original_offset,
				(unsigned long long)done);
			__pachaos_filed_fd_note_write_read_cache_from_iov_idx(
				idx,
				original_offset,
				iov,
				count,
				(unsigned long long)done);
		}
		if (idx >= 0 && __pachaos_filed_fds[idx].object_generation != 0) {
			++__pachaos_filed_fds[idx].object_generation;
		}
	}
	return done > 0 ? done : status;
}

static __inline long __pachaos_filed_pread(long fd, void *buf, long len, long offset)
{
	unsigned long long handle = __pachaos_filed_fd_handle(fd);
	if (!handle) return -29;
	if (len < 0 || offset < 0 || (len > 0 && !buf)) return -22;
	if (len == 0) return 0;
	int idx = __pachaos_filed_fd_index(fd);
	__pachaos_filed_fd_validate_read_cache_idx(idx);
	long cached = __pachaos_filed_fd_read_cache_copy_to_linear(
		idx,
		(unsigned long long)offset,
		(unsigned long long)len,
		(unsigned char *)buf);
	if (cached >= 0) {
		return cached;
	}
	long original_len = len;
	long original_offset = offset;
	long page_fd = -1;
	unsigned char *page = 0;
	long status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;
#if PACHAOS_FILED_READ_CACHE_BYTES > 0
	if ((unsigned long long)len <= PACHAOS_FILED_READ_CACHE_BYTES) {
		unsigned long long want = PACHAOS_FILED_READ_CACHE_BYTES;
		if (want > PACHAOS_FILED_IO_BYTES) want = PACHAOS_FILED_IO_BYTES;
		__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
		struct __pachaos_filed_io *io = (struct __pachaos_filed_io *)page;
		io->handle = handle;
		io->offset = (unsigned long long)offset;
		io->length = want;
		struct __pachaos_ipc_msg reply;
		status = __pachaos_filed_call(PACHAOS_FILED_OP_PREAD, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
		if (status == 0) {
			long got = (long)reply.word2;
			if (got < 0) got = 0;
			if ((unsigned long long)got > want) got = (long)want;
			long user_got = got;
			if (user_got > len) user_got = len;
			for (long i = 0; i < user_got; i++) ((unsigned char *)buf)[i] = io->data[i];
			if (got > 0) {
				__pachaos_filed_fd_store_read_cache_idx(
					idx,
					(unsigned long long)original_offset,
					io->data,
					(unsigned long long)got,
					(unsigned long long)got < want);
			}
			__pachaos_filed_page_destroy(page_fd, page);
			return user_got;
		}
		__pachaos_filed_page_destroy(page_fd, page);
		return status;
	}
#endif
	long done = 0;
	while (done < len) {
		long want = len - done;
		if (want > PACHAOS_FILED_IO_BYTES) want = PACHAOS_FILED_IO_BYTES;
		__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
		struct __pachaos_filed_io *io = (struct __pachaos_filed_io *)page;
		io->handle = handle;
		io->offset = (unsigned long long)(offset + done);
		io->length = (unsigned long long)want;
		struct __pachaos_ipc_msg reply;
		status = __pachaos_filed_call(PACHAOS_FILED_OP_PREAD, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
		if (status != 0) break;
		long got = (long)reply.word2;
		if (got <= 0) break;
		if (got > want) got = want;
		for (long i = 0; i < got; i++) ((unsigned char *)buf)[done + i] = io->data[i];
		done += got;
		if (got < want) break;
	}
	__pachaos_filed_page_destroy(page_fd, page);
#if PACHAOS_FILED_READ_CACHE_BYTES > 0
	if (done > 0) {
		__pachaos_filed_fd_store_read_cache_idx(
			idx,
			(unsigned long long)original_offset,
			(const unsigned char *)buf,
			(unsigned long long)done,
			done < original_len);
	}
#endif
	return done > 0 ? done : status;
}

static __inline long __pachaos_filed_pwrite(long fd, const void *buf, long len, long offset)
{
	unsigned long long handle = __pachaos_filed_fd_handle(fd);
	if (!handle) return -29;
	if (len < 0 || offset < 0 || (len > 0 && !buf)) return -22;
	if (len == 0) return 0;
	int idx = __pachaos_filed_fd_index(fd);
	long original_offset = offset;
	__pachaos_filed_fd_validate_read_cache_idx(idx);
	if ((unsigned long long)offset <= ~0ULL - (unsigned long long)len &&
		__pachaos_filed_fd_read_cache_equals_linear(
			idx,
			(unsigned long long)offset,
			(unsigned long long)len,
			(const unsigned char *)buf))
	{
		return len;
	}
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
		io->offset = (unsigned long long)(offset + done);
		io->length = (unsigned long long)want;
		for (long i = 0; i < want; i++) io->data[i] = ((const unsigned char *)buf)[done + i];
		struct __pachaos_ipc_msg reply;
		status = __pachaos_filed_call(PACHAOS_FILED_OP_PWRITE, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
		if (status != 0) break;
		long wrote = (long)reply.word2;
		if (wrote <= 0) break;
		if (wrote > want) wrote = want;
		done += wrote;
		if (wrote < want) break;
	}
	__pachaos_filed_page_destroy(page_fd, page);
	if (done > 0) {
		__pachaos_filed_fd_note_write_stat_idx(
			idx,
			(unsigned long long)original_offset,
			(unsigned long long)done);
		__pachaos_filed_fd_note_write_read_cache_idx(
			idx,
			(unsigned long long)original_offset,
			(const unsigned char *)buf,
			(unsigned long long)done);
		if (idx >= 0 && __pachaos_filed_fds[idx].object_generation != 0) {
			++__pachaos_filed_fds[idx].object_generation;
		}
	}
	return done > 0 ? done : status;
}

static __inline long __pachaos_filed_ftruncate(long fd, long size)
{
	unsigned long long handle = __pachaos_filed_fd_handle(fd);
	if (!handle) return -29;
	if (size < 0) return -22;
	int idx = __pachaos_filed_fd_index(fd);

	long page_fd = -1;
	unsigned char *page = 0;
	long status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;
	__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
	struct __pachaos_filed_truncate *truncate = (struct __pachaos_filed_truncate *)page;
	truncate->handle = handle;
	truncate->size = (unsigned long long)size;
	struct __pachaos_ipc_msg reply;
	status = __pachaos_filed_call(PACHAOS_FILED_OP_TRUNCATE, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
	__pachaos_filed_page_destroy(page_fd, page);
	if (status != 0) return status;

	__pachaos_filed_fd_invalidate_cached_state_idx(idx);
	if (idx >= 0) {
		__pachaos_filed_fds[idx].stat_valid = 1;
		__pachaos_filed_fds[idx].stat_size = (unsigned long long)size;
		if (__pachaos_filed_fds[idx].object_generation != 0) {
			++__pachaos_filed_fds[idx].object_generation;
		}
	}
	return 0;
}

static __inline void __pachaos_filed_note_path_mutation(void)
{
	__pachaos_filed_open_cache_clear_all();
	for (int i = 0; i < PACHAOS_FILED_FD_CAP; i++) {
		if (__pachaos_filed_fd_used[i]) {
			__pachaos_filed_fd_invalidate_cached_state_idx(i);
		}
	}
}

static __inline long __pachaos_filed_dir_handle_for_path(
	long dirfd,
	const char *path,
	unsigned long long *out_handle)
{
	if (!path || !out_handle) return -22;
	if (path[0] == '/' || dirfd == LINUX_AT_FDCWD) {
		*out_handle = 0;
		return 0;
	}
	*out_handle = __pachaos_filed_fd_handle(dirfd);
	return *out_handle ? 0 : -95;
}

static __inline long __pachaos_filed_unlinkat(long dirfd, const char *path, long flags)
{
	if (!path) return -22;
	if (flags & LINUX_AT_REMOVEDIR) return -22;
	if (flags & ~LINUX_AT_REMOVEDIR) return -22;
	unsigned long long dir_handle = 0;
	long status = __pachaos_filed_dir_handle_for_path(dirfd, path, &dir_handle);
	if (status != 0) return status;

	long page_fd = -1;
	unsigned char *page = 0;
	status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;
	__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
	struct __pachaos_filed_unlink *unlink = (struct __pachaos_filed_unlink *)page;
	unlink->dir_handle = dir_handle;
	status = __pachaos_copy_path(unlink->name, path, PACHAOS_FILED_NAME_BYTES);
	if (status == 0) {
		struct __pachaos_ipc_msg reply;
		status = __pachaos_filed_call(
			PACHAOS_FILED_OP_UNLINK,
			0,
			__pachaos_filed_next_request_id(),
			page_fd,
			&reply);
	}
	__pachaos_filed_page_destroy(page_fd, page);
	if (status == 0) __pachaos_filed_note_path_mutation();
	return status;
}

static __inline long __pachaos_filed_renameat(
	long old_dirfd,
	const char *old_path,
	long new_dirfd,
	const char *new_path)
{
	if (!old_path || !new_path) return -22;
	unsigned long long old_dir_handle = 0;
	unsigned long long new_dir_handle = 0;
	long status = __pachaos_filed_dir_handle_for_path(old_dirfd, old_path, &old_dir_handle);
	if (status != 0) return status;
	status = __pachaos_filed_dir_handle_for_path(new_dirfd, new_path, &new_dir_handle);
	if (status != 0) return status;

	long page_fd = -1;
	unsigned char *page = 0;
	status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;
	__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
	struct __pachaos_filed_rename *rename = (struct __pachaos_filed_rename *)page;
	rename->old_dir_handle = old_dir_handle;
	rename->new_dir_handle = new_dir_handle;
	status = __pachaos_copy_path(rename->old_name, old_path, PACHAOS_FILED_NAME_BYTES);
	if (status == 0) {
		status = __pachaos_copy_path(rename->new_name, new_path, PACHAOS_FILED_NAME_BYTES);
	}
	if (status == 0) {
		struct __pachaos_ipc_msg reply;
		status = __pachaos_filed_call(
			PACHAOS_FILED_OP_RENAME,
			0,
			__pachaos_filed_next_request_id(),
			page_fd,
			&reply);
	}
	__pachaos_filed_page_destroy(page_fd, page);
	if (status == 0) __pachaos_filed_note_path_mutation();
	return status;
}

static __inline long __pachaos_filed_close_fd(long fd);

static __inline long __pachaos_filed_truncate_path(const char *path, long size)
{
	if (!path) return -22;
	if (size < 0) return -22;
	long fd = __pachaos_openat_filed_fd(
		LINUX_AT_FDCWD,
		path,
		LINUX_O_RDWR | LINUX_O_CLOEXEC);
	if (fd < 0) return fd;
	long status = __pachaos_filed_ftruncate(fd, size);
	long close_status = __pachaos_filed_close_fd(fd);
	return status != 0 ? status : close_status;
}

static __inline int __pachaos_filed_handle_has_other_fd(unsigned long long handle)
{
	if (!handle) return 0;
	for (int i = 0; i < PACHAOS_FILED_FD_CAP; i++) {
		if (__pachaos_filed_fd_used[i] && __pachaos_filed_fds[i].handle == handle) return 1;
	}
	return 0;
}

static __inline long __pachaos_filed_close_fd(long fd)
{
	int idx = __pachaos_filed_fd_index(fd);
	if (idx < 0) {
		long status = __pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, fd);
		return status == 0 ? 0 : -22;
	}
	unsigned long long handle = __pachaos_filed_fds[idx].handle;
	unsigned char cacheable = __pachaos_filed_fds[idx].open_cacheable;
	unsigned int cache_rights = __pachaos_filed_fds[idx].open_cache_rights;
	unsigned int cache_open_flags = __pachaos_filed_fds[idx].open_cache_open_flags;
	unsigned long long cache_object_generation = __pachaos_filed_fds[idx].object_generation;
	unsigned long long cache_dir_generation = __pachaos_filed_fds[idx].dir_generation;
	unsigned char cache_stat_valid = __pachaos_filed_fds[idx].stat_valid;
	unsigned long long cache_stat_mode = __pachaos_filed_fds[idx].stat_mode;
	unsigned long long cache_stat_size = __pachaos_filed_fds[idx].stat_size;
	unsigned long long cache_stat_blocks = __pachaos_filed_fds[idx].stat_blocks;
	unsigned long long cache_stat_nlink = __pachaos_filed_fds[idx].stat_nlink;
	unsigned long long cache_stat_kind = __pachaos_filed_fds[idx].stat_kind;
	unsigned char cache_read_cache_valid = __pachaos_filed_fds[idx].read_cache_valid;
	unsigned char cache_read_cache_eof = __pachaos_filed_fds[idx].read_cache_eof;
	unsigned short cache_read_cache_len = __pachaos_filed_fds[idx].read_cache_len;
	unsigned long long cache_read_cache_offset = __pachaos_filed_fds[idx].read_cache_offset;
	const unsigned char *cache_read_cache = __pachaos_filed_fds[idx].read_cache;
	char cache_path[PACHAOS_FILED_NAME_BYTES];
	for (long i = 0; i < PACHAOS_FILED_NAME_BYTES; i++) {
		cache_path[i] = __pachaos_filed_fds[idx].open_cache_path[i];
	}
	__pachaos_filed_fd_used[idx] = 0;
	__pachaos_filed_fds[idx].used = 0;
	__pachaos_filed_fds[idx].open_cacheable = 0;
	__pachaos_filed_fds[idx].fd_flags = 0;
	__pachaos_filed_fds[idx].status_flags = 0;
	__pachaos_filed_fds[idx].open_cache_rights = 0;
	__pachaos_filed_fds[idx].open_cache_open_flags = 0;
	__pachaos_filed_fds[idx].offset = 0;
	__pachaos_filed_fds[idx].handle = 0;
	__pachaos_filed_fds[idx].object_generation = 0;
	__pachaos_filed_fds[idx].dir_generation = 0;
	__pachaos_filed_fd_invalidate_cached_state_idx(idx);
	__pachaos_bzero(__pachaos_filed_fds[idx].open_cache_path, PACHAOS_FILED_NAME_BYTES);
	if (__pachaos_filed_handle_has_other_fd(handle)) {
		return 0;
	}
	if (cacheable && cache_path[0] == '/') {
		__pachaos_filed_open_cache_store(
			handle,
			cache_path,
			cache_rights,
			cache_open_flags,
			cache_object_generation,
			cache_dir_generation,
			cache_stat_valid,
			cache_stat_mode,
			cache_stat_size,
			cache_stat_blocks,
			cache_stat_nlink,
			cache_stat_kind,
			cache_read_cache_valid,
			cache_read_cache_eof,
			cache_read_cache_len,
			cache_read_cache_offset,
			cache_read_cache);
	} else {
		__pachaos_filed_close_handle(handle, -1);
	}
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
	int idx = __pachaos_filed_fd_index(fd);
	if (idx >= 0 && __pachaos_filed_fds[idx].stat_valid) {
		if (__pachaos_filed_refresh_generation_from_stat_idx(idx) == 1 &&
			__pachaos_filed_fds[idx].stat_valid)
		{
			__pachaos_filed_write_kstat_from_fd_cache(idx, (struct __pachaos_kstat *)kst);
			return 0;
		}
	}
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
			if (idx >= 0) {
				__pachaos_filed_fds[idx].stat_valid = 1;
				__pachaos_filed_fds[idx].stat_mode = st->mode;
				__pachaos_filed_fds[idx].stat_size = st->size;
				__pachaos_filed_fds[idx].stat_blocks = st->blocks;
				__pachaos_filed_fds[idx].stat_nlink = st->nlink ? st->nlink : 1;
				__pachaos_filed_fds[idx].stat_kind = st->kind;
				__pachaos_filed_fds[idx].object_generation = st->object_generation;
				__pachaos_filed_fds[idx].dir_generation = st->dir_generation;
			}
		}
		__pachaos_filed_page_destroy(page_fd, page);
		return status;
	}

static __inline long __pachaos_filed_fstatat_path(long dirfd, const char *path, void *kst, long flags)
{
	if (!kst || !path) return -22;
	if ((flags & LINUX_AT_EMPTY_PATH) && path[0] == 0) {
		return __pachaos_filed_fstat(dirfd, kst);
	}
	if (flags & ~(LINUX_AT_EMPTY_PATH|LINUX_AT_SYMLINK_NOFOLLOW)) return -22;
	long fd = __pachaos_openat_filed_fd(dirfd, path, LINUX_O_CLOEXEC);
	if (fd < 0) {
		fd = __pachaos_openat_filed_fd(dirfd, path, LINUX_O_CLOEXEC|LINUX_O_DIRECTORY);
	}
	if (fd < 0) return fd;
	long status = __pachaos_filed_fstat(fd, kst);
	long close_status = __pachaos_filed_close_fd(fd);
	return status != 0 ? status : close_status;
}

static __inline long __pachaos_filed_accessat(long dirfd, const char *path, long mode, long flags)
{
	if (!path) return -22;
	if (flags & ~(LINUX_AT_SYMLINK_NOFOLLOW)) return -22;
	if (mode & ~(LINUX_R_OK|LINUX_W_OK|LINUX_X_OK)) return -22;
	struct __pachaos_kstat st;
	long status = __pachaos_filed_fstatat_path(dirfd, path, &st, flags);
	if (status != 0) return status;
	if (mode == LINUX_F_OK) return 0;
	unsigned int bits = st.st_mode & 0777U;
	if ((mode & LINUX_R_OK) && !(bits & 0444U)) return -13;
	if ((mode & LINUX_W_OK) && !(bits & 0222U)) return -13;
	if ((mode & LINUX_X_OK) && !(bits & 0111U)) return -13;
	return 0;
}

static __inline long __pachaos_getcwd(char *buf, long size)
{
	if (!buf || size <= 0) return -22;
	if (size < 2) return -34;
	buf[0] = '/';
	buf[1] = 0;
	return 2;
}

static __inline long __pachaos_readlinkat(long dirfd, const char *path, char *buf, long bufsize)
{
	if (!path || (bufsize > 0 && !buf)) return -22;
	if (bufsize < 0) return -22;
	struct __pachaos_kstat st;
	long status = __pachaos_filed_fstatat_path(dirfd, path, &st, LINUX_AT_SYMLINK_NOFOLLOW);
	if (status != 0) return status;
	return -22;
}

static __inline long __pachaos_filed_getdents(long fd, void *buf, long len)
{
	unsigned long long handle = __pachaos_filed_fd_handle(fd);
	if (!handle) return -9;
	if (len < (long)sizeof(struct __pachaos_dirent) || !buf) return -22;
	if ((__pachaos_filed_status_flags(fd) & PACHAOS_FILED_FILE_DIR_EOF) != 0) return 0;
	long status = 0;
	if (__pachaos_filed_fd_offset(fd) == 0 &&
		(__pachaos_filed_status_flags(fd) & PACHAOS_FILED_FILE_NEEDS_REWIND) != 0)
	{
		status = __pachaos_filed_seek_handle(handle, 0, 0);
		if (status != 0) return status;
		__pachaos_filed_fd_set_status_flags(
			fd,
			__pachaos_filed_status_flags(fd) & ~PACHAOS_FILED_FILE_NEEDS_REWIND);
	}
	long page_fd = -1;
	unsigned char *page = 0;
	status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;
	__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
	struct __pachaos_filed_getdents *gd = (struct __pachaos_filed_getdents *)page;
	gd->dir_handle = handle;
	gd->offset = __pachaos_filed_fd_offset(fd);
	gd->capacity = PACHAOS_FILED_DIRENT_CAPACITY;
	struct __pachaos_ipc_msg reply;
	status = __pachaos_filed_call(PACHAOS_FILED_OP_GETDENTS, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
	long out_bytes = 0;
	unsigned long long emitted = 0;
	if (status == 0) {
		int idx = __pachaos_filed_fd_index(fd);
		if (idx >= 0 && gd->dir_generation != 0) {
			__pachaos_filed_fds[idx].dir_generation = gd->dir_generation;
		}
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
			emitted++;
		}
		if (emitted == count && count < PACHAOS_FILED_DIRENT_CAPACITY) {
			__pachaos_filed_fd_set_status_flags(
				fd,
				__pachaos_filed_status_flags(fd) | PACHAOS_FILED_FILE_DIR_EOF);
		}
	}
	__pachaos_filed_page_destroy(page_fd, page);
	if (status == 0 && emitted != 0) {
		__pachaos_filed_fd_advance_offset(fd, emitted);
	}
	return status == 0 ? out_bytes : status;
}

static __inline long __pachaos_filed_lseek(long fd, long offset, long whence)
{
	unsigned long long handle = __pachaos_filed_fd_handle(fd);
	if (!handle) return -29;
	long long base = 0;
	switch (whence) {
	case 0:
		base = 0;
		break;
	case 1:
		base = (long long)__pachaos_filed_fd_offset(fd);
		break;
	case 2: {
		struct __pachaos_kstat st;
		long status = __pachaos_filed_fstat(fd, &st);
		if (status != 0) return status;
		base = st.st_size;
		break;
	}
	default:
		return -22;
	}
	if ((offset > 0 && base > 0x7fffffffffffffffLL - offset) ||
		(offset < 0 && base < (-0x7fffffffffffffffLL - 1LL) - offset))
	{
		return -22;
	}
	long long next = base + offset;
	if (next < 0) return -22;
	__pachaos_filed_fd_set_offset(fd, (unsigned long long)next);
	(void)handle;
	return (long)next;
}

static __inline unsigned long long __pachaos_filed_fd_flags_from_linux_fcntl(long flags)
{
	return (flags & LINUX_FD_CLOEXEC) ? PACHAOS_FILED_FD_CLOEXEC : 0;
}

static __inline unsigned long long __pachaos_filed_status_flags_from_linux_fcntl(long flags)
{
	unsigned long long out = 0;
	if (flags & LINUX_O_APPEND) out |= PACHAOS_FILED_FILE_APPEND;
	if (flags & LINUX_O_NONBLOCK) out |= PACHAOS_FILED_FILE_NONBLOCK;
	return out;
}

static __inline long __pachaos_filed_set_flags(const struct __pachaos_filed_handle_flags *flags)
{
	long page_fd = -1;
	unsigned char *page = 0;
	long status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;
	__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
	*(struct __pachaos_filed_handle_flags *)page = *flags;
	struct __pachaos_ipc_msg reply;
	status = __pachaos_filed_call(PACHAOS_FILED_OP_SET_FLAGS, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
	__pachaos_filed_page_destroy(page_fd, page);
	return status;
}

static __inline long __pachaos_filed_dup_fd(long fd, long min_fd, long flags)
{
	unsigned long long handle = __pachaos_filed_fd_handle(fd);
	if (!handle) return -9;
	long status = __pachaos_filed_fd_alloc_from(
		handle,
		min_fd,
		(unsigned int)__pachaos_filed_fd_flags_from_linux_fcntl(flags),
		__pachaos_filed_status_flags(fd));
	if (status >= 0) {
		__pachaos_filed_fd_set_offset(status, __pachaos_filed_fd_offset(fd));
		int src_idx = __pachaos_filed_fd_index(fd);
		int dst_idx = __pachaos_filed_fd_index(status);
		if (src_idx >= 0 && dst_idx >= 0) {
			__pachaos_filed_fds[dst_idx].stat_valid = __pachaos_filed_fds[src_idx].stat_valid;
			__pachaos_filed_fds[dst_idx].read_cache_valid = __pachaos_filed_fds[src_idx].read_cache_valid;
			__pachaos_filed_fds[dst_idx].read_cache_eof = __pachaos_filed_fds[src_idx].read_cache_eof;
			__pachaos_filed_fds[dst_idx].read_cache_len = __pachaos_filed_fds[src_idx].read_cache_len;
			__pachaos_filed_fds[dst_idx].stat_mode = __pachaos_filed_fds[src_idx].stat_mode;
			__pachaos_filed_fds[dst_idx].stat_size = __pachaos_filed_fds[src_idx].stat_size;
			__pachaos_filed_fds[dst_idx].stat_blocks = __pachaos_filed_fds[src_idx].stat_blocks;
			__pachaos_filed_fds[dst_idx].stat_nlink = __pachaos_filed_fds[src_idx].stat_nlink;
			__pachaos_filed_fds[dst_idx].stat_kind = __pachaos_filed_fds[src_idx].stat_kind;
			__pachaos_filed_fds[dst_idx].read_cache_offset = __pachaos_filed_fds[src_idx].read_cache_offset;
			__pachaos_filed_fds[dst_idx].object_generation = __pachaos_filed_fds[src_idx].object_generation;
			__pachaos_filed_fds[dst_idx].dir_generation = __pachaos_filed_fds[src_idx].dir_generation;
			__pachaos_filed_fds[dst_idx].open_cacheable = __pachaos_filed_fds[src_idx].open_cacheable;
			__pachaos_filed_fds[dst_idx].open_cache_rights = __pachaos_filed_fds[src_idx].open_cache_rights;
			__pachaos_filed_fds[dst_idx].open_cache_open_flags = __pachaos_filed_fds[src_idx].open_cache_open_flags;
			for (long i = 0; i < PACHAOS_FILED_NAME_BYTES; i++) {
				__pachaos_filed_fds[dst_idx].open_cache_path[i] =
					__pachaos_filed_fds[src_idx].open_cache_path[i];
			}
#if PACHAOS_FILED_READ_CACHE_BYTES > 0
			if (__pachaos_filed_fds[dst_idx].read_cache_len > PACHAOS_FILED_READ_CACHE_BYTES) {
				__pachaos_filed_fds[dst_idx].read_cache_valid = 0;
				__pachaos_filed_fds[dst_idx].read_cache_len = 0;
			}
			for (unsigned int i = 0; i < __pachaos_filed_fds[dst_idx].read_cache_len; i++) {
				__pachaos_filed_fds[dst_idx].read_cache[i] = __pachaos_filed_fds[src_idx].read_cache[i];
			}
#else
			__pachaos_filed_fds[dst_idx].read_cache_valid = 0;
			__pachaos_filed_fds[dst_idx].read_cache_len = 0;
#endif
		}
	}
	return status;
}

static __inline long __pachaos_filed_fcntl(long fd, long cmd, long arg)
{
	unsigned long long handle = __pachaos_filed_fd_handle(fd);
	if (!handle) return __pachaos_raw3(PACHAOS_SYSCALL_FD_FCNTL, fd, cmd, arg);

	struct __pachaos_filed_handle_flags flags;
	long status;
	__pachaos_bzero(&flags, sizeof flags);
	switch (cmd) {
	case LINUX_F_DUPFD:
		return __pachaos_filed_dup_fd(fd, arg, 0);
	case LINUX_F_DUPFD_CLOEXEC:
		return __pachaos_filed_dup_fd(fd, arg, LINUX_FD_CLOEXEC);
	case LINUX_F_GETFD:
		return (__pachaos_filed_fd_flags(fd) & PACHAOS_FILED_FD_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;
	case LINUX_F_SETFD:
		flags.handle = handle;
		flags.fd_flags = __pachaos_filed_fd_flags_from_linux_fcntl(arg);
		if ((unsigned int)flags.fd_flags == __pachaos_filed_fd_flags(fd)) return 0;
		flags.status_flags = __pachaos_filed_status_flags(fd) &
			(PACHAOS_FILED_FILE_APPEND|PACHAOS_FILED_FILE_NONBLOCK|PACHAOS_FILED_FILE_SYNC);
		status = __pachaos_filed_set_flags(&flags);
		if (status == 0) __pachaos_filed_fd_set_flags(fd, (unsigned int)flags.fd_flags);
		return status;
	case LINUX_F_GETFL:
		return ((__pachaos_filed_status_flags(fd) & PACHAOS_FILED_FILE_APPEND) ? LINUX_O_APPEND : 0) |
			((__pachaos_filed_status_flags(fd) & PACHAOS_FILED_FILE_NONBLOCK) ? LINUX_O_NONBLOCK : 0) |
			LINUX_O_LARGEFILE;
	case LINUX_F_SETFL:
		flags.handle = handle;
		flags.fd_flags = __pachaos_filed_fd_flags(fd);
		flags.status_flags = __pachaos_filed_status_flags_from_linux_fcntl(arg);
		status = __pachaos_filed_set_flags(&flags);
		if (status == 0) __pachaos_filed_fd_set_status_flags(fd, (unsigned int)flags.status_flags);
		return status;
	default:
		return -38;
	}
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
	status = __pachaos_filed_call(
		PACHAOS_FILED_OP_EXEC_PATH,
		0,
		__pachaos_filed_next_request_id(),
		page_fd,
		&reply);
	__pachaos_filed_page_destroy(page_fd, page);
	if (status != 0) return status;
	(void)__pachaos_raw1(PACHAOS_SYSCALL_PROCESS_EXIT, 0);
	for (;;) __asm__ __volatile__("pause");
}

static __inline long __pachaos_filed_handle_vmo(unsigned long long handle)
{
	if (!handle) return -22;

	long page_fd = -1;
	unsigned char *page = 0;
	long status = __pachaos_filed_page_create(&page_fd, &page);
	if (status != 0) return status;

	struct __pachaos_ipc_msg reply;
	__pachaos_bzero(page, PACHAOS_FILED_PAGE_BYTES);
	struct __pachaos_filed_statx *st = (struct __pachaos_filed_statx *)page;
	st->handle = handle;
	status = __pachaos_filed_call(PACHAOS_FILED_OP_STAT, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
	if (status != 0) {
		__pachaos_filed_page_destroy(page_fd, page);
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
		status = __pachaos_filed_call(PACHAOS_FILED_OP_PREAD, 0, __pachaos_filed_next_request_id(), page_fd, &reply);
		if (status != 0 || reply.word2 < header_bytes) {
			__pachaos_filed_page_destroy(page_fd, page);
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
		__pachaos_filed_page_destroy(page_fd, page);
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
			__pachaos_filed_page_destroy(page_fd, page);
			return -12;
		}
	}

	unsigned long long offset = 0;
	while (offset < file_size) {
		unsigned long long batch_count = 0;
		unsigned long long batch_ids[PACHAOS_FILED_FAST_PAYLOAD_SLOT_COUNT];
		unsigned long long batch_offsets[PACHAOS_FILED_FAST_PAYLOAD_SLOT_COUNT];
		unsigned long long batch_lengths[PACHAOS_FILED_FAST_PAYLOAD_SLOT_COUNT];

		while (offset < file_size && batch_count < PACHAOS_FILED_FAST_PAYLOAD_SLOT_COUNT) {
			unsigned long long want = file_size - offset;
			if (want > PACHAOS_FILED_IO_BYTES) want = PACHAOS_FILED_IO_BYTES;
			unsigned char *slot_page = __pachaos_filed_fast_payload_slot(batch_count);
			if (!slot_page) {
				status = -14;
				break;
			}
			__pachaos_bzero(slot_page, PACHAOS_FILED_PAGE_BYTES);
			struct __pachaos_filed_io *io = (struct __pachaos_filed_io *)slot_page;
			io->handle = handle;
			io->offset = offset;
			io->length = want;
			const unsigned long long request_id = (unsigned long long)__pachaos_filed_next_request_id();
			status = __pachaos_filed_fast_enqueue(
				PACHAOS_FILED_OP_PREAD,
				0,
				(long)request_id,
				batch_count,
				PACHAOS_FILED_PAGE_BYTES);
			if (status != 0) break;
			batch_ids[batch_count] = request_id;
			batch_offsets[batch_count] = offset;
			batch_lengths[batch_count] = want;
			offset += want;
			batch_count++;
		}
		if (status != 0 || batch_count == 0) {
			if (map_size != 0) (void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, file_addr, (long)map_size);
			(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, file_fd);
			__pachaos_filed_page_destroy(page_fd, page);
			return status != 0 ? status : -14;
		}
		status = __pachaos_filed_fast_doorbell((long)batch_ids[batch_count - 1], &reply);
		if (status != 0) {
			if (map_size != 0) (void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, file_addr, (long)map_size);
			(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, file_fd);
			__pachaos_filed_page_destroy(page_fd, page);
			return status;
		}
		for (unsigned long long batch_index = 0; batch_index < batch_count; batch_index++) {
			status = __pachaos_filed_fast_wait_completion((long)batch_ids[batch_index], &reply);
			if (status != 0 || reply.word2 < batch_lengths[batch_index]) {
				if (map_size != 0) (void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, file_addr, (long)map_size);
				(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, file_fd);
				__pachaos_filed_page_destroy(page_fd, page);
				return status != 0 ? status : -5;
			}
			unsigned char *slot_page = __pachaos_filed_fast_payload_slot(batch_index);
			struct __pachaos_filed_io *io = (struct __pachaos_filed_io *)slot_page;
			for (unsigned long long i = 0; i < batch_lengths[batch_index]; i++) {
				((unsigned char *)file_addr)[batch_offsets[batch_index] + i] = io->data[i];
			}
		}
	}

	if (map_size != 0) (void)__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, file_addr, (long)map_size);
	__pachaos_filed_page_destroy(page_fd, page);
	return file_fd;
}

static __inline long __pachaos_openat_dispatch(long dirfd, const char *path, long flags)
{
	return __pachaos_openat_filed_fd(dirfd, path, flags);
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

static __inline long __pachaos_mmap_fd(long addr, long len, long prot, long flags, long fd, long offset)
{
	unsigned long long handle = fd >= 0 ? __pachaos_filed_fd_handle(fd) : 0;
	if (!handle) {
		return __pachaos_mmap_result(__pachaos_raw6(
			PACHAOS_SYSCALL_MMAP,
			fd < 0 ? 0 : fd,
			addr,
			len,
			__pachaos_prot(prot),
			__pachaos_mmap_flags(flags),
			offset));
	}

	long vmo_fd = __pachaos_filed_handle_vmo(handle);
	if (vmo_fd < 16) return vmo_fd;
	long result = __pachaos_mmap_result(__pachaos_raw6(
		PACHAOS_SYSCALL_MMAP,
		vmo_fd,
		addr,
		len,
		__pachaos_prot(prot),
		__pachaos_mmap_flags(flags),
		offset));
	(void)__pachaos_raw1(PACHAOS_SYSCALL_FD_CLOSE, vmo_fd);
	return result;
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
	case __NR_brk: return __pachaos_brk(a1);
	case __NR_unlink: return __pachaos_filed_unlinkat(LINUX_AT_FDCWD, (const char *)a1, 0);
	case __NR_exit:
	case __NR_exit_group: return __pachaos_raw1(PACHAOS_SYSCALL_PROCESS_EXIT, a1);
	case __NR_set_tid_address: return __pachaos_raw0(PACHAOS_SYSCALL_GETTID);
	default: return -38;
	}
}

static __inline long __syscall2(long n, long a1, long a2)
{
	switch (n) {
	case __NR_getcwd: return __pachaos_getcwd((char *)a1, a2);
	case __NR_brk: return __pachaos_brk(a1);
	case __NR_munmap: return __pachaos_status(__pachaos_raw2(PACHAOS_SYSCALL_MUNMAP, a1, a2));
	case __NR_madvise: return __pachaos_madvise(a1, a2, 0);
	case __NR_clock_gettime: return __pachaos_status(__pachaos_raw2(PACHAOS_SYSCALL_CLOCK_GETTIME, a1, a2));
	case __NR_nanosleep: return __pachaos_status(__pachaos_raw2(PACHAOS_SYSCALL_NANOSLEEP, a1, a2));
	case __NR_fstat: return __pachaos_filed_fstat(a1, (void *)a2);
	case __NR_ftruncate: return __pachaos_filed_ftruncate(a1, a2);
	case __NR_truncate: return __pachaos_filed_truncate_path((const char *)a1, a2);
	case __NR_rename: return __pachaos_filed_renameat(
		LINUX_AT_FDCWD,
		(const char *)a1,
		LINUX_AT_FDCWD,
		(const char *)a2);
	case __NR_stat: return __pachaos_filed_fstatat_path(LINUX_AT_FDCWD, (const char *)a1, (void *)a2, 0);
	case __NR_lstat: return __pachaos_filed_fstatat_path(LINUX_AT_FDCWD, (const char *)a1, (void *)a2, LINUX_AT_SYMLINK_NOFOLLOW);
	case __NR_access: return __pachaos_filed_accessat(LINUX_AT_FDCWD, (const char *)a1, a2, 0);
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
	case __NR_readv: return __pachaos_filed_readv(a1, (const struct __pachaos_iovec *)a2, a3);
	case __NR_writev: return __pachaos_filed_writev(a1, (const struct __pachaos_iovec *)a2, a3);
	case __NR_ioctl: return __pachaos_status(__pachaos_raw3(PACHAOS_SYSCALL_FD_IOCTL, a1, a2, a3));
	case __NR_poll: return __pachaos_poll((struct __pachaos_linux_pollfd *)a1, a2, a3);
	case __NR_mprotect: return __pachaos_status(__pachaos_raw3(PACHAOS_SYSCALL_MPROTECT, a1, a2, __pachaos_prot(a3)));
	case __NR_madvise: return __pachaos_madvise(a1, a2, a3);
	case __NR_open: return __pachaos_openat_dispatch(LINUX_AT_FDCWD, (const char *)a1, a2);
	case __NR_lseek: return __pachaos_filed_lseek(a1, a2, a3);
	case __NR_fcntl: return __pachaos_filed_fcntl(a1, a2, a3);
	case __NR_readlink: return __pachaos_readlinkat(LINUX_AT_FDCWD, (const char *)a1, (char *)a2, a3);
	case __NR_getdents:
	case __NR_getdents64: return __pachaos_filed_getdents(a1, (void *)a2, a3);
	case __NR_execve: return __pachaos_filed_execve((const char *)a1, (char *const *)a2, (char *const *)a3);
	case __NR_unlinkat: return __pachaos_filed_unlinkat(a1, (const char *)a2, a3);
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
		return __pachaos_filed_fstatat_path(a1, (const char *)a2, (void *)a3, a4);
	case __NR_faccessat:
	case __NR_faccessat2:
		return __pachaos_filed_accessat(a1, (const char *)a2, a3, a4);
	case __NR_readlinkat:
		return __pachaos_readlinkat(a1, (const char *)a2, (char *)a3, a4);
	case __NR_timerfd_settime:
		if (a2 & ~LINUX_TFD_TIMER_ABSTIME) return -22;
		return __pachaos_status(__pachaos_raw4(PACHAOS_SYSCALL_TIMERFD_SETTIME, a1, a2, a3, a4));
	case __NR_openat:
		return __pachaos_openat_dispatch(a1, (const char *)a2, a3);
	case __NR_renameat:
		return __pachaos_filed_renameat(a1, (const char *)a2, a3, (const char *)a4);
	case __NR_pread64:
		return __pachaos_filed_pread(a1, (void *)a2, a3, a4);
	case __NR_pwrite64:
		return __pachaos_filed_pwrite(a1, (const void *)a2, a3, a4);
	default: return -38;
	}
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	switch (n) {
	case __NR_mremap:
		return __pachaos_mremap(a1, a2, a3, a4, a5);
	case __NR_renameat2:
		if (a5 != 0) return -22;
		return __pachaos_filed_renameat(a1, (const char *)a2, a3, (const char *)a4);
	default:
		break;
	}
	return __syscall4(n, a1, a2, a3, a4);
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	switch (n) {
	case __NR_brk:
		return __pachaos_brk(a1);
	case __NR_mmap:
		return __pachaos_mmap_fd(a1, a2, a3, a4, a5, a6);
	case __NR_close:
	case __NR_unlink:
		return __syscall1(n, a1);
	case __NR_munmap:
	case __NR_clock_gettime:
	case __NR_nanosleep:
	case __NR_fstat:
	case __NR_ftruncate:
	case __NR_truncate:
	case __NR_rename:
	case __NR_getcwd:
	case __NR_stat:
	case __NR_lstat:
	case __NR_access:
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
	case __NR_lseek:
	case __NR_fcntl:
	case __NR_readlink:
	case __NR_getdents:
	case __NR_getdents64:
	case __NR_execve:
	case __NR_unlinkat:
		return __syscall3(n, a1, a2, a3);
	case __NR_futex:
	case __NR_newfstatat:
	case __NR_faccessat:
	case __NR_faccessat2:
	case __NR_readlinkat:
	case __NR_clock_nanosleep:
	case __NR_timerfd_settime:
	case __NR_openat:
	case __NR_renameat:
	case __NR_pread64:
	case __NR_pwrite64:
		return __syscall4(n, a1, a2, a3, a4);
	case __NR_renameat2:
		return __syscall5(n, a1, a2, a3, a4, a5);
	default: return -38;
	}
}

static __inline long __syscall7(long n, long a1, long a2, long a3, long a4, long a5, long a6, long a7)
{
	(void)a7;
	return __syscall6(n, a1, a2, a3, a4, a5, a6);
}

#define IPC_64 0
