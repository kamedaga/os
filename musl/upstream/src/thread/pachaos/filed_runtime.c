#ifndef PACHAOS_FILED_READ_CACHE_BYTES
#define PACHAOS_FILED_READ_CACHE_BYTES 4096
#endif
#define PACHAOS_FILED_OPEN_CACHE_CAP 8
#define PACHAOS_FILED_NAME_BYTES 96
#if PACHAOS_FILED_READ_CACHE_BYTES > 0
#define PACHAOS_FILED_READ_CACHE_STORAGE_BYTES PACHAOS_FILED_READ_CACHE_BYTES
#else
#define PACHAOS_FILED_READ_CACHE_STORAGE_BYTES 1
#endif

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
	unsigned int rights;
	unsigned int open_flags;
	unsigned long long handle;
	unsigned long long object_generation;
	unsigned long long dir_generation;
	char path[PACHAOS_FILED_NAME_BYTES];
};

struct __pachaos_filed_fd_entry __pachaos_filed_fds[128];
unsigned char __pachaos_filed_fd_used[128];
struct __pachaos_filed_open_cache_entry __pachaos_filed_open_cache[PACHAOS_FILED_OPEN_CACHE_CAP];
unsigned long long __pachaos_filed_request_id = 100;
long __pachaos_filed_page_fd = -1;
unsigned char *__pachaos_filed_page_addr;
unsigned char *__pachaos_filed_session_page_addr;
int __pachaos_filed_page_lock;
long __pachaos_filed_session_fd = -1;
