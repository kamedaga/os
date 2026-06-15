#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_brk
#define SYS_brk 12
#endif
#ifndef SYS_futex
#define SYS_futex 202
#endif
#ifndef SYS_gettid
#define SYS_gettid 186
#endif
#ifndef SYS_exit_group
#define SYS_exit_group 231
#endif
#ifndef SYS_arch_prctl
#define SYS_arch_prctl 158
#endif

#define ARCH_SET_GS 0x1001
#define ARCH_GET_GS 0x1004

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128

enum {
    DEFAULT_ROUNDS = 8,
    DEFAULT_THREADS = 4,
    MAX_THREADS = 16,
    MALLOC_SLOTS = 96,
    FD_PRESSURE_SLOTS = 96,
    STRESS_MAX_THREADS = 8,
};

static __thread int tls_probe_word = 0;
static volatile int thread_start_gate = 0;
static volatile int thread_ready_mask = 0;
static volatile int thread_done_mask = 0;
static volatile int thread_tls_sum = 0;
static volatile int thread_futex_eagain_count = 0;
static volatile int thread_tid_seen[MAX_THREADS];
static volatile int exit_group_child_ready = 0;
static volatile int exit_group_wait_word = 0;
static volatile int stress_start_gate = 0;
static volatile int stress_ready_mask = 0;
static volatile int stress_done_mask = 0;
static volatile int stress_tls_sum = 0;
static volatile int postmmap_stage = 0;
static volatile int postmmap_done = 0;
static volatile unsigned char *postmmap_main_page = NULL;
static volatile unsigned char *postmmap_thread_page = NULL;

static uint64_t gs_probe_words[2] = {
    UINT64_C(0x7061636861677331),
    UINT64_C(0x7061636861677332),
};

static int write_all_fd(int fd, const char *text)
{
    size_t len = strlen(text);
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, text + done, len - done);
        if (n <= 0) {
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static void write_dec_fd(int fd, long value)
{
    char buf[40];
    char tmp[32];
    size_t pos = 0;
    size_t used = 0;
    unsigned long magnitude;
    if (value < 0) {
        buf[pos++] = '-';
        magnitude = (unsigned long)(-(value + 1)) + 1ul;
    } else {
        magnitude = (unsigned long)value;
    }
    do {
        tmp[used++] = (char)('0' + (magnitude % 10ul));
        magnitude /= 10ul;
    } while (magnitude != 0 && used < sizeof(tmp));
    while (used != 0) {
        buf[pos++] = tmp[--used];
    }
    (void)write(fd, buf, pos);
}

static void write_hex_fd(int fd, uintptr_t value)
{
    static const char digits[] = "0123456789abcdef";
    char buf[2 + sizeof(uintptr_t) * 2];
    buf[0] = '0';
    buf[1] = 'x';
    for (size_t i = 0; i < sizeof(uintptr_t) * 2; i++) {
        unsigned int shift = (unsigned int)((sizeof(uintptr_t) * 2 - 1 - i) * 4);
        buf[2 + i] = digits[(value >> shift) & 0xfu];
    }
    (void)write(fd, buf, sizeof(buf));
}

static int fail_errno(const char *name, int code)
{
    (void)write_all_fd(2, "linux_abi_probe: ");
    (void)write_all_fd(2, name);
    (void)write_all_fd(2, " failed errno=");
    write_dec_fd(2, errno);
    (void)write_all_fd(2, "\n");
    return code;
}

static int fail_value(const char *name, const char *field, long value, int code)
{
    (void)write_all_fd(2, "linux_abi_probe: ");
    (void)write_all_fd(2, name);
    (void)write_all_fd(2, " failed ");
    (void)write_all_fd(2, field);
    (void)write_all_fd(2, "=");
    write_dec_fd(2, value);
    (void)write_all_fd(2, "\n");
    return code;
}

static int ok(const char *name)
{
    (void)write_all_fd(1, "linux_abi_probe: ");
    (void)write_all_fd(1, name);
    (void)write_all_fd(1, " ok\n");
    return 0;
}

static int trace_enabled(void)
{
    const char *value = getenv("LINUX_ABI_PROBE_TRACE");
    return value != NULL && value[0] != '\0' && value[0] != '0';
}

static void trace_text(const char *text)
{
    if (!trace_enabled()) {
        return;
    }
    (void)write_all_fd(1, "linux_abi_probe.trace: ");
    (void)write_all_fd(1, text);
    (void)write_all_fd(1, "\n");
}

static void trace_index(const char *text, int index)
{
    if (!trace_enabled()) {
        return;
    }
    (void)write_all_fd(1, "linux_abi_probe.trace: ");
    (void)write_all_fd(1, text);
    (void)write_all_fd(1, " index=");
    write_dec_fd(1, index);
    (void)write_all_fd(1, "\n");
}

static unsigned env_u32(const char *name, unsigned fallback, unsigned maximum)
{
    const char *text = getenv(name);
    if (text == NULL || *text == '\0') {
        return fallback;
    }
    unsigned value = 0;
    while (*text >= '0' && *text <= '9') {
        value = value * 10u + (unsigned)(*text - '0');
        text++;
    }
    if (*text != '\0' || value == 0) {
        return fallback;
    }
    return value > maximum ? maximum : value;
}

static size_t page_size(void)
{
    long value = sysconf(_SC_PAGESIZE);
    return value > 0 ? (size_t)value : 4096u;
}

static int mmap_probe(unsigned rounds)
{
    const size_t page = page_size();
    for (unsigned round = 0; round < rounds; round++) {
        size_t pages = 1u + (round % 8u);
        size_t length = page * pages;
        errno = 0;
        unsigned char *mem = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) {
            return fail_errno("mmap", 10);
        }
        uintptr_t addr = (uintptr_t)mem;
        if (addr < 0x10000u || (addr % page) != 0) {
            (void)write_all_fd(2, "linux_abi_probe: mmap returned bad user VA ");
            write_hex_fd(2, addr);
            (void)write_all_fd(2, "\n");
            return 11;
        }
        for (size_t i = 0; i < length; i += 97u) {
            mem[i] = (unsigned char)(round + i);
        }
        mem[length - 1u] = (unsigned char)(0xa0u + round);
        for (size_t i = 0; i < length; i += 97u) {
            if (mem[i] != (unsigned char)(round + i)) {
                return fail_value("mmap pattern", "round", (long)round, 12);
            }
        }
        if (munmap(mem, length) != 0) {
            return fail_errno("munmap", 13);
        }
    }
    return ok("mmap");
}

static int brk_probe(void)
{
    uintptr_t current = (uintptr_t)syscall(SYS_brk, 0);
    if (current < 0x10000u) {
        return fail_value("brk current", "value", (long)current, 20);
    }
    uintptr_t target = (current + 8191u) & ~(uintptr_t)4095u;
    target += 8192u;
    uintptr_t grown = (uintptr_t)syscall(SYS_brk, target);
    if (grown < target) {
        return fail_value("brk grow", "value", (long)grown, 21);
    }
    unsigned char *start = (unsigned char *)current;
    unsigned char *end = (unsigned char *)target;
    for (unsigned char *p = start; p < end; p += 257u) {
        *p = (unsigned char)((uintptr_t)p >> 4);
    }
    end[-1] = 0x5a;
    uintptr_t restored = (uintptr_t)syscall(SYS_brk, current);
    if (restored != current) {
        return fail_value("brk restore", "value", (long)restored, 22);
    }
    return ok("brk");
}

static int malloc_probe(unsigned rounds)
{
    void *slots[MALLOC_SLOTS];
    size_t sizes[MALLOC_SLOTS];
    memset(slots, 0, sizeof(slots));
    memset(sizes, 0, sizeof(sizes));

    for (unsigned round = 0; round < rounds; round++) {
        for (unsigned i = 0; i < MALLOC_SLOTS; i++) {
            size_t size = 1u + ((round * 131u + i * 197u) % 16384u);
            unsigned char *ptr = malloc(size);
            if (ptr == NULL) {
                return fail_errno("malloc", 30);
            }
            memset(ptr, (int)(0x31u + ((round + i) & 0x3fu)), size);
            if (slots[i] != NULL) {
                unsigned char *old = slots[i];
                if (old[0] != (unsigned char)(0x31u + (((round - 1u) + i) & 0x3fu)) ||
                    old[sizes[i] - 1u] != (unsigned char)(0x31u + (((round - 1u) + i) & 0x3fu)))
                {
                    return fail_value("malloc pattern", "slot", (long)i, 31);
                }
                free(slots[i]);
            }
            slots[i] = ptr;
            sizes[i] = size;
        }
    }

    for (unsigned i = 0; i < MALLOC_SLOTS; i++) {
        free(slots[i]);
    }
    return ok("malloc");
}

static int futex_probe(void)
{
    int word = 1;
    errno = 0;
    long ret = syscall(SYS_futex, &word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, NULL, NULL, 0);
    if (ret != -1 || errno != EAGAIN) {
        return fail_value("futex wait eagain", "ret", ret, 40);
    }

    errno = 0;
    ret = syscall(SYS_futex, &word, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, NULL, NULL, 0);
    if (ret != 0) {
        return fail_errno("futex wake", 41);
    }

    struct timespec ts = { 0, 0 };
    word = 0;
    errno = 0;
    ret = syscall(SYS_futex, &word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, &ts, NULL, 0);
    if (ret != -1 || errno != ETIMEDOUT) {
        return fail_value("futex timeout", "ret", ret, 42);
    }
    return ok("futex");
}

typedef struct thread_arg {
    int index;
    unsigned rounds;
} thread_arg_t;

typedef struct stress_thread_arg {
    int index;
    unsigned rounds;
    unsigned wave;
} stress_thread_arg_t;

static void *thread_entry(void *arg)
{
    thread_arg_t *thread = arg;
    int index = thread->index;
    trace_index("thread entry", index);
    tls_probe_word = 1000 + index;
    thread_tid_seen[index] = (int)syscall(SYS_gettid);
    trace_index("thread tid", index);

    int local_word = 1;
    errno = 0;
    long ret = syscall(SYS_futex, &local_word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, NULL, NULL, 0);
    if (ret == -1 && errno == EAGAIN) {
        __sync_fetch_and_add(&thread_futex_eagain_count, 1);
    }
    trace_index("thread futex-eagain", index);

    __sync_fetch_and_or(&thread_ready_mask, 1 << index);
    trace_index("thread ready", index);
    while (!thread_start_gate) {
        syscall(SYS_futex, (int *)&thread_start_gate, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, NULL, NULL, 0);
    }
    trace_index("thread started", index);

    for (unsigned round = 0; round < thread->rounds; round++) {
        size_t length = 4096u * (1u + ((round + (unsigned)index) % 4u));
        unsigned char *mapped = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            mapped[0] = (unsigned char)(index + round);
            mapped[length - 1u] = (unsigned char)(index + round + 1u);
            (void)munmap(mapped, length);
        }
        trace_index("thread mmap", index);
        size_t size = 64u + ((round * 257u + (unsigned)index * 911u) % 8192u);
        unsigned char *heap = malloc(size);
        if (heap != NULL) {
            memset(heap, 0x51 + index, size);
            free(heap);
        }
        trace_index("thread malloc", index);
    }

    __sync_fetch_and_add(&thread_tls_sum, tls_probe_word);
    __sync_fetch_and_or(&thread_done_mask, 1 << index);
    trace_index("thread done", index);
    return (void *)(long)(index + 17);
}

static int pthread_probe(unsigned threads, unsigned rounds)
{
    pthread_t ids[MAX_THREADS];
    thread_arg_t args[MAX_THREADS];
    if (threads > MAX_THREADS) {
        threads = MAX_THREADS;
    }
    thread_start_gate = 0;
    thread_ready_mask = 0;
    thread_done_mask = 0;
    thread_tls_sum = 0;
    thread_futex_eagain_count = 0;
    memset((void *)thread_tid_seen, 0, sizeof(thread_tid_seen));

    int expected_mask = (1 << threads) - 1;
    int expected_tls = 0;
    for (unsigned i = 0; i < threads; i++) {
        args[i].index = (int)i;
        args[i].rounds = rounds;
        expected_tls += 1000 + (int)i;
        trace_index("pthread create begin", (int)i);
        int status = pthread_create(&ids[i], NULL, thread_entry, &args[i]);
        if (status != 0) {
            errno = status;
            return fail_errno("pthread_create", 50);
        }
        trace_index("pthread create ok", (int)i);
    }

    trace_text("pthread wait ready");
    while (thread_ready_mask != expected_mask) {
        __asm__ volatile("pause");
    }
    trace_text("pthread wake");
    thread_start_gate = 1;
    (void)syscall(SYS_futex, (int *)&thread_start_gate, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, (int)threads, NULL, NULL, 0);

    for (unsigned i = 0; i < threads; i++) {
        void *joined = NULL;
        trace_index("pthread join begin", (int)i);
        int status = pthread_join(ids[i], &joined);
        if (status != 0) {
            errno = status;
            return fail_errno("pthread_join", 51);
        }
        trace_index("pthread join ok", (int)i);
        if ((long)joined != (long)i + 17) {
            return fail_value("pthread return", "index", (long)i, 52);
        }
    }

    if (thread_done_mask != expected_mask) {
        return fail_value("pthread done mask", "mask", thread_done_mask, 53);
    }
    if (thread_tls_sum != expected_tls) {
        return fail_value("pthread tls", "sum", thread_tls_sum, 54);
    }
    if (thread_futex_eagain_count != (int)threads) {
        return fail_value("pthread futex", "count", thread_futex_eagain_count, 55);
    }
    int main_tid = (int)syscall(SYS_gettid);
    for (unsigned i = 0; i < threads; i++) {
        if (thread_tid_seen[i] == 0 || thread_tid_seen[i] == main_tid) {
            return fail_value("pthread tid", "index", (long)i, 56);
        }
        for (unsigned j = i + 1; j < threads; j++) {
            if (thread_tid_seen[i] == thread_tid_seen[j]) {
                return fail_value("pthread duplicate tid", "index", (long)i, 57);
            }
        }
    }
    return ok("pthread");
}

static void short_sleep_ns(long ns)
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = ns;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static void *stress_thread_entry(void *arg)
{
    stress_thread_arg_t *thread = arg;
    int index = thread->index;
    tls_probe_word = 3000 + (int)thread->wave * 100 + index;
    __sync_fetch_and_add(&stress_tls_sum, tls_probe_word);
    __sync_fetch_and_or(&stress_ready_mask, 1 << index);
    while (!stress_start_gate) {
        syscall(SYS_futex, (int *)&stress_start_gate, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, NULL, NULL, 0);
    }

    for (unsigned round = 0; round < thread->rounds; round++) {
        int futex_word = 1;
        errno = 0;
        long ret = syscall(SYS_futex, &futex_word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, NULL, NULL, 0);
        if (ret != -1 || errno != EAGAIN) {
            return (void *)101;
        }

        futex_word = 0;
        struct timespec zero = { 0, 0 };
        errno = 0;
        ret = syscall(SYS_futex, &futex_word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, &zero, NULL, 0);
        if (ret != -1 || errno != ETIMEDOUT) {
            return (void *)102;
        }

        size_t length = 4096u * (1u + ((round + (unsigned)index + thread->wave) % 6u));
        unsigned char *mapped = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped == MAP_FAILED) {
            return (void *)103;
        }
        mapped[0] = (unsigned char)(0x40u + index);
        mapped[length - 1u] = (unsigned char)(0x70u + round);
        if (munmap(mapped, length) != 0) {
            return (void *)104;
        }

        size_t size = 32u + ((round * 149u + (unsigned)index * 997u + thread->wave * 17u) % 12000u);
        unsigned char *heap = malloc(size);
        if (heap == NULL) {
            return (void *)105;
        }
        memset(heap, 0x20 + index, size);
        free(heap);

        short_sleep_ns(1000000L + (long)((round + (unsigned)index) % 3u) * 1000000L);
    }

    __sync_fetch_and_or(&stress_done_mask, 1 << index);
    return (void *)(long)(index + 200);
}

static int stress_probe(unsigned threads, unsigned rounds)
{
    if (threads > STRESS_MAX_THREADS) {
        threads = STRESS_MAX_THREADS;
    }
    if (threads == 0) {
        threads = DEFAULT_THREADS;
    }
    unsigned waves = env_u32("LINUX_ABI_PROBE_STRESS_WAVES", 8, 64);
    int expected_mask = (1 << threads) - 1;

    for (unsigned wave = 0; wave < waves; wave++) {
        pthread_t ids[STRESS_MAX_THREADS];
        stress_thread_arg_t args[STRESS_MAX_THREADS];
        stress_start_gate = 0;
        stress_ready_mask = 0;
        stress_done_mask = 0;
        stress_tls_sum = 0;

        for (unsigned i = 0; i < threads; i++) {
            args[i].index = (int)i;
            args[i].rounds = rounds;
            args[i].wave = wave;
            trace_index("stress create begin", (int)i);
            int status = pthread_create(&ids[i], NULL, stress_thread_entry, &args[i]);
            if (status != 0) {
                errno = status;
                return fail_errno("stress pthread_create", 80);
            }
        }

        while (stress_ready_mask != expected_mask) {
            __asm__ volatile("pause");
        }
        stress_start_gate = 1;
        (void)syscall(SYS_futex, (int *)&stress_start_gate, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, (int)threads, NULL, NULL, 0);

        int expected_tls = 0;
        for (unsigned i = 0; i < threads; i++) {
            expected_tls += 3000 + (int)wave * 100 + (int)i;
        }
        for (unsigned i = 0; i < threads; i++) {
            void *joined = NULL;
            int status = pthread_join(ids[i], &joined);
            if (status != 0) {
                errno = status;
                return fail_errno("stress pthread_join", 81);
            }
            if ((long)joined != (long)i + 200) {
                return fail_value("stress thread return", "value", (long)joined, 82);
            }
        }

        if (stress_done_mask != expected_mask) {
            return fail_value("stress done mask", "mask", stress_done_mask, 83);
        }
        if (stress_tls_sum != expected_tls) {
            return fail_value("stress tls", "sum", stress_tls_sum, 84);
        }
        short_sleep_ns(1000000L);
    }

    return ok("stress");
}

static void *postmmap_thread_entry(void *arg)
{
    (void)arg;
    const size_t page = page_size();
    while (postmmap_stage == 0) {
        syscall(SYS_futex, (int *)&postmmap_stage, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, NULL, NULL, 0);
    }
    if (postmmap_main_page == NULL || postmmap_main_page[0] != 0x41) {
        return (void *)301;
    }
    postmmap_main_page[page - 1u] = 0x72;

    unsigned char *mapped = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) {
        return (void *)302;
    }
    mapped[0] = 0x53;
    mapped[page - 1u] = 0x54;
    postmmap_thread_page = mapped;
    __sync_synchronize();
    postmmap_done = 1;
    (void)syscall(SYS_futex, (int *)&postmmap_done, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, NULL, NULL, 0);
    return (void *)300;
}

static int postmmap_probe(void)
{
    const size_t page = page_size();
    pthread_t id;
    postmmap_stage = 0;
    postmmap_done = 0;
    postmmap_main_page = NULL;
    postmmap_thread_page = NULL;

    int status = pthread_create(&id, NULL, postmmap_thread_entry, NULL);
    if (status != 0) {
        errno = status;
        return fail_errno("postmmap pthread_create", 90);
    }

    unsigned char *mapped = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) {
        return fail_errno("postmmap main mmap", 91);
    }
    mapped[0] = 0x41;
    postmmap_main_page = mapped;
    __sync_synchronize();
    postmmap_stage = 1;
    (void)syscall(SYS_futex, (int *)&postmmap_stage, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, NULL, NULL, 0);

    while (!postmmap_done) {
        syscall(SYS_futex, (int *)&postmmap_done, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, NULL, NULL, 0);
    }

    void *joined = NULL;
    status = pthread_join(id, &joined);
    if (status != 0) {
        errno = status;
        return fail_errno("postmmap pthread_join", 92);
    }
    if ((long)joined != 300) {
        return fail_value("postmmap thread return", "value", (long)joined, 93);
    }
    if (mapped[page - 1u] != 0x72) {
        return fail_value("postmmap main page", "value", mapped[page - 1u], 94);
    }
    if (postmmap_thread_page == NULL || postmmap_thread_page[0] != 0x53 || postmmap_thread_page[page - 1u] != 0x54) {
        return fail_value("postmmap thread page", "value", postmmap_thread_page == NULL ? -1 : postmmap_thread_page[0], 95);
    }
    (void)munmap(mapped, page);
    (void)munmap((void *)postmmap_thread_page, page);
    return ok("postmmap");
}

static int fd_probe(void)
{
    const char *path = "/tmp/linux_abi_probe.tmp";
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        return fail_errno("open", 60);
    }
    const char payload[] = "linux-abi-probe";
    if (write(fd, payload, sizeof(payload) - 1u) != (ssize_t)(sizeof(payload) - 1u)) {
        int saved = errno;
        (void)close(fd);
        (void)unlink(path);
        errno = saved;
        return fail_errno("write", 61);
    }
    if (lseek(fd, 0, SEEK_SET) != 0) {
        int saved = errno;
        (void)close(fd);
        (void)unlink(path);
        errno = saved;
        return fail_errno("lseek", 62);
    }
    char got[32];
    memset(got, 0, sizeof(got));
    if (read(fd, got, sizeof(payload) - 1u) != (ssize_t)(sizeof(payload) - 1u) ||
        memcmp(got, payload, sizeof(payload) - 1u) != 0)
    {
        (void)close(fd);
        (void)unlink(path);
        return fail_value("read", "mismatch", 1, 63);
    }
    if (close(fd) != 0) {
        return fail_errno("close", 64);
    }
    if (unlink(path) != 0) {
        return fail_errno("unlink", 65);
    }

    int fds[FD_PRESSURE_SLOTS];
    for (unsigned i = 0; i < FD_PRESSURE_SLOTS; i++) {
        fds[i] = -1;
    }
    for (unsigned i = 0; i < FD_PRESSURE_SLOTS; i++) {
        fds[i] = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (fds[i] < 0) {
            int saved = errno;
            for (unsigned j = 0; j < i; j++) {
                if (fds[j] >= 0) {
                    (void)close(fds[j]);
                }
            }
            errno = saved;
            return fail_errno("fd pressure open", 66);
        }
    }
    if (fds[FD_PRESSURE_SLOTS - 1] < 32) {
        int last = fds[FD_PRESSURE_SLOTS - 1];
        for (unsigned i = 0; i < FD_PRESSURE_SLOTS; i++) {
            if (fds[i] >= 0) {
                (void)close(fds[i]);
            }
        }
        return fail_value("fd pressure high fd", "fd", last, 67);
    }
    int dup_fd = fcntl(fds[0], F_DUPFD_CLOEXEC, 64);
    if (dup_fd < 64) {
        int saved = errno;
        for (unsigned i = 0; i < FD_PRESSURE_SLOTS; i++) {
            if (fds[i] >= 0) {
                (void)close(fds[i]);
            }
        }
        errno = saved;
        return fail_errno("fd pressure dup", 68);
    }
    if (close(dup_fd) != 0) {
        int saved = errno;
        for (unsigned i = 0; i < FD_PRESSURE_SLOTS; i++) {
            if (fds[i] >= 0) {
                (void)close(fds[i]);
            }
        }
        errno = saved;
        return fail_errno("fd pressure dup close", 69);
    }
    for (unsigned i = 0; i < FD_PRESSURE_SLOTS; i++) {
        if (close(fds[i]) != 0) {
            return fail_errno("fd pressure close", 72);
        }
    }
    return ok("fd");
}

static void *exit_group_waiter(void *arg)
{
    (void)arg;
    exit_group_child_ready = 1;
    while (exit_group_wait_word == 0) {
        syscall(SYS_futex, (int *)&exit_group_wait_word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, NULL, NULL, 0);
    }
    return (void *)23;
}

static int exit_group_probe(void)
{
    pthread_t waiter;
    exit_group_child_ready = 0;
    exit_group_wait_word = 0;
    int status = pthread_create(&waiter, NULL, exit_group_waiter, NULL);
    if (status != 0) {
        errno = status;
        return fail_errno("exit_group pthread_create", 70);
    }
    while (!exit_group_child_ready) {
        __asm__ volatile("pause");
    }
    (void)write_all_fd(1, "linux_abi_probe: exit_group child ready\n");
    syscall(SYS_exit_group, 0);
    return fail_value("exit_group returned", "value", 1, 71);
}

static int exit_group_big_anon_probe(void)
{
    pthread_t waiter;
    exit_group_child_ready = 0;
    exit_group_wait_word = 0;
    int status = pthread_create(&waiter, NULL, exit_group_waiter, NULL);
    if (status != 0) {
        errno = status;
        return fail_errno("exit_group_big_anon pthread_create", 72);
    }
    while (!exit_group_child_ready) {
        __asm__ volatile("pause");
    }

    unsigned mb = env_u32("LINUX_ABI_PROBE_BIG_MB", 16, 256);
    if (mb == 0) mb = 16;
    size_t length = (size_t)mb * 1024u * 1024u;
    unsigned char *mapped = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) {
        return fail_errno("exit_group_big_anon mmap", 73);
    }
    const size_t page = page_size();
    for (size_t offset = 0; offset < length; offset += page) {
        mapped[offset] = (unsigned char)(0x31u + ((offset / page) & 0x0fu));
    }
    mapped[length - 1u] = 0x7a;
    (void)write_all_fd(1, "linux_abi_probe: exit_group big anon ready\n");
    syscall(SYS_exit_group, 0);
    return fail_value("exit_group_big_anon returned", "value", 1, 74);
}

static uint64_t read_gs_u64(unsigned offset)
{
    uint64_t value = 0;
#if defined(__x86_64__)
    if (offset == 0) {
        __asm__ volatile("movq %%gs:0, %0" : "=r"(value));
    } else if (offset == 8) {
        __asm__ volatile("movq %%gs:8, %0" : "=r"(value));
    }
#else
    (void)offset;
#endif
    return value;
}

static int gs_probe(void)
{
    unsigned long old_gs = 0;
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, &old_gs) != 0) {
        return fail_errno("arch_prctl get_gs initial", 90);
    }
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)(uintptr_t)gs_probe_words) != 0) {
        return fail_errno("arch_prctl set_gs", 91);
    }

    unsigned long current_gs = 0;
    int status = 0;
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, &current_gs) != 0) {
        status = fail_errno("arch_prctl get_gs after set", 92);
        goto restore;
    }
    if (current_gs != (unsigned long)(uintptr_t)gs_probe_words) {
        status = fail_value("arch_prctl get_gs value", "value", (long)current_gs, 93);
        goto restore;
    }
    if (read_gs_u64(0) != gs_probe_words[0] || read_gs_u64(8) != gs_probe_words[1]) {
        status = fail_value("gs direct read", "value", (long)read_gs_u64(0), 94);
        goto restore;
    }

    (void)getpid();
    struct timespec ts = { 0, 0 };
    (void)nanosleep(&ts, NULL);
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, &current_gs) != 0) {
        status = fail_errno("arch_prctl get_gs after syscall", 95);
        goto restore;
    }
    if (current_gs != (unsigned long)(uintptr_t)gs_probe_words) {
        status = fail_value("arch_prctl get_gs after syscall", "value", (long)current_gs, 96);
        goto restore;
    }
    if (read_gs_u64(0) != gs_probe_words[0] || read_gs_u64(8) != gs_probe_words[1]) {
        status = fail_value("gs direct read after syscall", "value", (long)read_gs_u64(0), 97);
        goto restore;
    }

restore:
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, old_gs) != 0 && status == 0) {
        return fail_errno("arch_prctl restore_gs", 98);
    }
    return status != 0 ? status : ok("gs");
}

static int run_all(unsigned rounds, unsigned threads)
{
    int status = mmap_probe(rounds);
    if (status != 0) return status;
    status = brk_probe();
    if (status != 0) return status;
    status = malloc_probe(rounds);
    if (status != 0) return status;
    status = futex_probe();
    if (status != 0) return status;
    status = pthread_probe(threads, rounds);
    if (status != 0) return status;
    status = stress_probe(threads, rounds);
    if (status != 0) return status;
    status = postmmap_probe();
    if (status != 0) return status;
    status = fd_probe();
    if (status != 0) return status;
    return ok("all");
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "all";
    unsigned rounds = env_u32("LINUX_ABI_PROBE_ROUNDS", DEFAULT_ROUNDS, 256);
    unsigned threads = env_u32("LINUX_ABI_PROBE_THREADS", DEFAULT_THREADS, MAX_THREADS);
    if (threads == 0) {
        threads = DEFAULT_THREADS;
    }

    if (strcmp(mode, "mmap") == 0) return mmap_probe(rounds);
    if (strcmp(mode, "brk") == 0) return brk_probe();
    if (strcmp(mode, "malloc") == 0) return malloc_probe(rounds);
    if (strcmp(mode, "futex") == 0) return futex_probe();
    if (strcmp(mode, "pthread") == 0) return pthread_probe(threads, rounds);
    if (strcmp(mode, "stress") == 0) return stress_probe(threads, rounds);
    if (strcmp(mode, "postmmap") == 0) return postmmap_probe();
    if (strcmp(mode, "fd") == 0) return fd_probe();
    if (strcmp(mode, "exit-group") == 0) return exit_group_probe();
    if (strcmp(mode, "exit-group-big-anon") == 0) return exit_group_big_anon_probe();
    if (strcmp(mode, "gs") == 0) return gs_probe();
    if (strcmp(mode, "all") == 0) return run_all(rounds, threads);

    (void)write_all_fd(2, "usage: linux_abi_probe.elf [all|mmap|brk|malloc|futex|pthread|stress|postmmap|fd|exit-group|exit-group-big-anon|gs]\n");
    return 2;
}
