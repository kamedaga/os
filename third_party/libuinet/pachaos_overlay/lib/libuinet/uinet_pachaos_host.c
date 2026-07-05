#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "uinet_api.h"
#include "uinet_host_interface.h"

extern void *malloc(size_t size);
extern void *calloc(size_t number, size_t size);
extern void *realloc(void *ptr, size_t size);
extern void free(void *ptr);
extern void *memset(void *s, int c, size_t n);
extern void *memmove(void *dest, const void *src, size_t n);
extern void *memcpy(void *dest, const void *src, size_t n);
extern size_t strlen(const char *s);
extern char *strchr(const char *s, int c);
extern char *strrchr(const char *s, int c);
extern long write(int fd, const void *buf, size_t count);
extern int clock_gettime(int clock_id, void *tp);
extern int nanosleep(const void *req, void *rem);
extern int printf(const char *fmt, ...);
extern int snprintf(char *str, size_t size, const char *fmt, ...);

struct pachaos_timespec {
	long tv_sec;
	long tv_nsec;
};

static unsigned int g_num_cpus = 1;
static void *g_tls_value;
static uhi_thread_hook_t g_thread_hooks[UHI_THREAD_NUM_HOOK_TYPES][8];
static void *g_thread_hook_args[UHI_THREAD_NUM_HOOK_TYPES][8];
static uint32_t g_rand_state = 0x12345678u;
uintptr_t __stack_chk_guard = 0x70616368616f7355ull;
const char *panicstr;
int if_netmap_num_extra_bufs;

void __stack_chk_fail(void)
{
	panicstr = "stack check failed";
	for (;;) {
	}
}

void panic(const char *fmt, ...)
{
	panicstr = fmt != NULL ? fmt : "panic";
	printf("panic: %s\n", panicstr);
	for (;;) {
	}
}

void bzero(void *s, size_t n)
{
	(void)memset(s, 0, n);
}

void bcopy(const void *src, void *dest, size_t n)
{
	(void)memmove(dest, src, n);
}

char *index(const char *s, int c)
{
	return strchr(s, c);
}

char *rindex(const char *s, int c)
{
	return strrchr(s, c);
}

char *strcat(char *dest, const char *src)
{
	char *out = dest + strlen(dest);

	while ((*out++ = *src++) != '\0') {
	}
	return dest;
}

int putchar(int c)
{
	unsigned char ch = (unsigned char)c;

	(void)write(1, &ch, 1);
	return c;
}

void syslog(int priority, const char *fmt, ...)
{
	(void)priority;
	(void)fmt;
}

long random(void)
{
	g_rand_state ^= g_rand_state << 13;
	g_rand_state ^= g_rand_state >> 17;
	g_rand_state ^= g_rand_state << 5;
	return (long)(g_rand_state & 0x7fffffffu);
}

void srandom(unsigned int seed)
{
	g_rand_state = seed != 0 ? seed : 0x12345678u;
}

long long strtoq(const char *nptr, char **endptr, int base)
{
	const char *p = nptr;
	int neg = 0;
	unsigned long long value = 0;

	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
		p++;
	}
	if (*p == '-' || *p == '+') {
		neg = (*p == '-');
		p++;
	}
	if (base == 0) {
		base = 10;
	}
	while (*p != '\0') {
		unsigned digit;
		if (*p >= '0' && *p <= '9') {
			digit = (unsigned)(*p - '0');
		} else if (*p >= 'a' && *p <= 'z') {
			digit = (unsigned)(*p - 'a') + 10u;
		} else if (*p >= 'A' && *p <= 'Z') {
			digit = (unsigned)(*p - 'A') + 10u;
		} else {
			break;
		}
		if (digit >= (unsigned)base) {
			break;
		}
		value = value * (unsigned)base + digit;
		p++;
	}
	if (endptr != NULL) {
		*endptr = (char *)p;
	}
	return neg ? -(long long)value : (long long)value;
}

int sscanf(const char *str, const char *fmt, ...)
{
	(void)str;
	(void)fmt;
	return 0;
}

static const char *pachaos_ipv4_ntop(const unsigned char *src, char *dst, size_t size)
{
	if (size < 16) {
		return NULL;
	}
	(void)snprintf(dst, size, "%u.%u.%u.%u", src[0], src[1], src[2], src[3]);
	return dst;
}

const char *inet_ntop(int af, const void *src, char *dst, size_t size)
{
	if (af == 2) {
		return pachaos_ipv4_ntop(src, dst, size);
	}
	if (dst != NULL && size != 0) {
		dst[0] = '\0';
	}
	return NULL;
}

int inet_pton(int af, const char *src, void *dst)
{
	unsigned values[4] = {0, 0, 0, 0};
	unsigned index = 0;
	const char *p = src;

	if (af != 2 || src == NULL || dst == NULL) {
		return -1;
	}
	while (*p != '\0') {
		if (*p >= '0' && *p <= '9') {
			values[index] = values[index] * 10u + (unsigned)(*p - '0');
			if (values[index] > 255u) {
				return 0;
			}
		} else if (*p == '.' && index < 3) {
			index++;
		} else {
			return 0;
		}
		p++;
	}
	if (index != 3) {
		return 0;
	}
	for (unsigned i = 0; i < 4; i++) {
		((unsigned char *)dst)[i] = (unsigned char)values[i];
	}
	return 1;
}

void uhi_lock_log_init(void) {}
void uhi_lock_log_set_file(const char *file) { (void)file; }
void uhi_lock_log_enable(void) {}
void uhi_lock_log_disable(void) {}

void uhi_init(void) {}

void uhi_set_num_cpus(unsigned int n)
{
	g_num_cpus = n == 0 ? 1 : n;
}

void *uhi_malloc(uint64_t size)
{
	return malloc((size_t)size);
}

void *uhi_calloc(uint64_t number, uint64_t size)
{
	return calloc((size_t)number, (size_t)size);
}

void *uhi_realloc(void *p, uint64_t size)
{
	return realloc(p, (size_t)size);
}

void uhi_free(void *p)
{
	free(p);
}

void uhi_clock_gettime(int id, int64_t *sec, long *nsec)
{
	struct pachaos_timespec ts;
	int clock_id = id == UHI_CLOCK_REALTIME ? 0 : 1;

	if (clock_gettime(clock_id, &ts) != 0) {
		ts.tv_sec = 0;
		ts.tv_nsec = 0;
	}
	*sec = ts.tv_sec;
	*nsec = ts.tv_nsec;
}

uint64_t uhi_clock_gettime_ns(int id)
{
	int64_t sec;
	long nsec;

	uhi_clock_gettime(id, &sec, &nsec);
	return ((uint64_t)sec * UHI_NSEC_PER_SEC) + (uint64_t)nsec;
}

int uhi_nanosleep(uint64_t nsecs)
{
	struct pachaos_timespec ts;

	ts.tv_sec = (long)(nsecs / UHI_NSEC_PER_SEC);
	ts.tv_nsec = (long)(nsecs % UHI_NSEC_PER_SEC);
	return nanosleep(&ts, NULL);
}

int uhi_open(const char *path, int flags)
{
	(void)path;
	(void)flags;
	return -1;
}

int uhi_close(int d)
{
	(void)d;
	return -1;
}

int uhi_mkdir(const char *path, unsigned int mode)
{
	(void)path;
	(void)mode;
	return -1;
}

void *uhi_mmap(void *addr, uint64_t len, int prot, int flags, int fd, uint64_t offset)
{
	uintptr_t raw;
	uintptr_t aligned;
	void **slot;

	(void)addr;
	(void)prot;
	(void)flags;
	(void)fd;
	(void)offset;
	if (len == 0) {
		return UHI_MAP_FAILED;
	}

	raw = (uintptr_t)malloc((size_t)len + 4095u + sizeof(void *));
	if (raw == 0) {
		return UHI_MAP_FAILED;
	}
	aligned = (raw + sizeof(void *) + 4095u) & ~(uintptr_t)4095u;
	slot = (void **)(aligned - sizeof(void *));
	*slot = (void *)raw;
	return (void *)aligned;
}

int uhi_munmap(void *addr, uint64_t len)
{
	(void)len;
	if (addr != NULL && addr != UHI_MAP_FAILED) {
		void **slot = (void **)((uintptr_t)addr - sizeof(void *));
		free(*slot);
	}
	return 0;
}

int uhi_poll(struct uhi_pollfd *fds, unsigned int nfds, int timeout)
{
	(void)fds;
	(void)nfds;
	if (timeout > 0) {
		(void)uhi_nanosleep((uint64_t)timeout * 1000000ull);
	}
	return 0;
}

void uhi_thread_bind(unsigned int cpu)
{
	(void)cpu;
}

int uhi_thread_bound_cpu(void)
{
	return 0;
}

int uhi_thread_create(uhi_thread_t *new_thread, struct uhi_thread_start_args *start_args, unsigned int stack_bytes)
{
	(void)stack_bytes;
	if (new_thread != NULL) {
		*new_thread = 1;
	}
	if (start_args != NULL) {
		if (start_args->set_tls) {
			g_tls_value = start_args->tls_data;
		}
		if (start_args->start_notify_routine != NULL) {
			start_args->start_notify_routine(start_args->start_notify_routine_arg);
		}
	}
	return 0;
}

void uhi_thread_exit(void)
{
	for (;;) {
		(void)uhi_nanosleep(UHI_NSEC_PER_SEC);
	}
}

int uhi_thread_hook_add(int which, uhi_thread_hook_t hook, void *arg)
{
	if (which < 0 || which >= UHI_THREAD_NUM_HOOK_TYPES || hook == NULL) {
		return -1;
	}
	for (unsigned i = 0; i < 8; i++) {
		if (g_thread_hooks[which][i] == NULL) {
			g_thread_hooks[which][i] = hook;
			g_thread_hook_args[which][i] = arg;
			return (int)i;
		}
	}
	return -1;
}

void uhi_thread_hook_remove(int which, int id)
{
	if (which >= 0 && which < UHI_THREAD_NUM_HOOK_TYPES && id >= 0 && id < 8) {
		g_thread_hooks[which][id] = NULL;
		g_thread_hook_args[which][id] = NULL;
	}
}

void uhi_thread_run_hooks(int which)
{
	if (which < 0 || which >= UHI_THREAD_NUM_HOOK_TYPES) {
		return;
	}
	for (unsigned i = 0; i < 8; i++) {
		if (g_thread_hooks[which][i] != NULL) {
			g_thread_hooks[which][i](g_thread_hook_args[which][i]);
		}
	}
}

void uhi_thread_set_name(const char *name)
{
	(void)name;
}

int uhi_tls_key_create(uhi_tls_key_t *key, void (*destructor)(void *))
{
	(void)destructor;
	*key = 1;
	return 0;
}

int uhi_tls_key_delete(uhi_tls_key_t key)
{
	(void)key;
	g_tls_value = NULL;
	return 0;
}

void *uhi_tls_get(uhi_tls_key_t key)
{
	(void)key;
	return g_tls_value;
}

int uhi_tls_set(uhi_tls_key_t key, void *data)
{
	(void)key;
	g_tls_value = data;
	return 0;
}

uhi_thread_t uhi_thread_self(void)
{
	return 1;
}

uint64_t uhi_thread_self_id(void)
{
	return 1;
}

void uhi_thread_yield(void) {}
int uhi_thread_setprio(unsigned int prio) { (void)prio; return 0; }
int uhi_thread_setprio_rt(unsigned int prio) { (void)prio; return 0; }

int uhi_cond_init(uhi_cond_t *c) { *c = NULL; return 0; }
void uhi_cond_destroy(uhi_cond_t *c) { (void)c; }
void uhi_cond_wait(uhi_cond_t *c, uhi_mutex_t *m) { (void)c; (void)m; }
int uhi_cond_timedwait(uhi_cond_t *c, uhi_mutex_t *m, uint64_t nsecs)
{
	(void)c;
	(void)m;
	return uhi_nanosleep(nsecs);
}
void uhi_cond_signal(uhi_cond_t *c) { (void)c; }
void uhi_cond_broadcast(uhi_cond_t *c) { (void)c; }

int uhi_mutex_init(uhi_mutex_t *m, int opts) { (void)opts; *m = NULL; return 0; }
void uhi_mutex_destroy(uhi_mutex_t *m) { (void)m; }
void _uhi_mutex_lock(uhi_mutex_t *m, void *l, const char *file, int line)
{
	(void)m;
	(void)l;
	(void)file;
	(void)line;
}
int _uhi_mutex_trylock(uhi_mutex_t *m, void *l, const char *file, int line)
{
	(void)m;
	(void)l;
	(void)file;
	(void)line;
	return 1;
}
void _uhi_mutex_unlock(uhi_mutex_t *m, void *l, const char *file, int line)
{
	(void)m;
	(void)l;
	(void)file;
	(void)line;
}

int uhi_rwlock_init(uhi_rwlock_t *rw, int opts) { (void)opts; *rw = NULL; return 0; }
void uhi_rwlock_destroy(uhi_rwlock_t *rw) { (void)rw; }
void _uhi_rwlock_wlock(uhi_rwlock_t *rw, void *l, const char *file, int line)
{
	(void)rw;
	(void)l;
	(void)file;
	(void)line;
}
int _uhi_rwlock_trywlock(uhi_rwlock_t *rw, void *l, const char *file, int line)
{
	(void)rw;
	(void)l;
	(void)file;
	(void)line;
	return 1;
}
void _uhi_rwlock_wunlock(uhi_rwlock_t *rw, void *l, const char *file, int line)
{
	(void)rw;
	(void)l;
	(void)file;
	(void)line;
}
void _uhi_rwlock_rlock(uhi_rwlock_t *rw, void *l, const char *file, int line)
{
	(void)rw;
	(void)l;
	(void)file;
	(void)line;
}
int _uhi_rwlock_tryrlock(uhi_rwlock_t *rw, void *l, const char *file, int line)
{
	(void)rw;
	(void)l;
	(void)file;
	(void)line;
	return 1;
}
void _uhi_rwlock_runlock(uhi_rwlock_t *rw, void *l, const char *file, int line)
{
	(void)rw;
	(void)l;
	(void)file;
	(void)line;
}
int _uhi_rwlock_tryupgrade(uhi_rwlock_t *rw, void *l, const char *file, int line)
{
	(void)rw;
	(void)l;
	(void)file;
	(void)line;
	return 1;
}
void _uhi_rwlock_downgrade(uhi_rwlock_t *rw, void *l, const char *file, int line)
{
	(void)rw;
	(void)l;
	(void)file;
	(void)line;
}

int uhi_get_ifaddr(const char *ifname, uint8_t *ethaddr)
{
	static const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

	(void)ifname;
	for (unsigned i = 0; i < sizeof(mac); i++) {
		ethaddr[i] = mac[i];
	}
	return 0;
}

void uhi_arc4rand(void *ptr, unsigned int len, int reseed)
{
	uint8_t *out = ptr;

	if (reseed) {
		g_rand_state ^= (uint32_t)uhi_clock_gettime_ns(UHI_CLOCK_MONOTONIC);
	}
	for (unsigned i = 0; i < len; i++) {
		g_rand_state ^= g_rand_state << 13;
		g_rand_state ^= g_rand_state >> 17;
		g_rand_state ^= g_rand_state << 5;
		out[i] = (uint8_t)g_rand_state;
	}
}

uint32_t uhi_arc4random(void)
{
	uint32_t value;

	uhi_arc4rand(&value, sizeof(value), 0);
	return value;
}

void uhi_install_sighandlers(void) {}
void uhi_mask_all_signals(void) {}
void uhi_unmask_all_signals(void) {}

int uhi_msg_init(struct uhi_msg *msg, unsigned int size, unsigned int rsp_size)
{
	(void)msg;
	(void)size;
	(void)rsp_size;
	return -1;
}
void uhi_msg_destroy(struct uhi_msg *msg) { (void)msg; }
int uhi_msg_send(struct uhi_msg *msg, void *payload) { (void)msg; (void)payload; return -1; }
int uhi_msg_wait(struct uhi_msg *msg, void *payload) { (void)msg; (void)payload; return -1; }
int uhi_msg_rsp_send(struct uhi_msg *msg, void *payload) { (void)msg; (void)payload; return -1; }
int uhi_msg_rsp_wait(struct uhi_msg *msg, void *payload) { (void)msg; (void)payload; return -1; }

int uhi_get_stacktrace(uintptr_t *pcs, int npcs)
{
	(void)pcs;
	(void)npcs;
	return 0;
}
