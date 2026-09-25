/* SPDX-License-Identifier: MIT */
/* Two native processes; children never receive a backing VMO capability. */
#include <stdint.h>
#include <stddef.h>
#include "pacha/ipc.h"
#include "pacha/syscall.h"

#define STR1(x) #x
#define STR(x) STR1(x)
#define PAGE 4096ul
#define CONTROL 0x42000000ul
#define TARGET 0x43000000ul
#define CODE 0x44000000ul
#define STACK 0x45000000ul
#define RW (PACHA_PROT_READ | PACHA_PROT_WRITE)
#define VMO_RIGHTS (PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE | PACHA_FD_RIGHT_MAP_EXEC | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP)
#define PROCESS_RIGHTS (PACHA_FD_RIGHT_MAP_INTO | PACHA_FD_RIGHT_SPAWN | PACHA_FD_RIGHT_SET_CONTEXT | PACHA_FD_RIGHT_KILL | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_DUP)
#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x,v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
struct control {
    uint64_t command, done, value, write_value, vector, address, access_va, ready;
    uint64_t fd_capacity, fd_maximum, fd_free;
    uint64_t saved_r10, saved_r15, saved_fs, saved_gs, saved_xmm15[2];
};
_Static_assert(sizeof(struct pacha_thread_context) == PACHA_THREAD_CONTEXT_SIZE, "context ABI size");
_Static_assert(offsetof(struct pacha_thread_context, registers) == 64, "context GPR offset");
_Static_assert(offsetof(struct pacha_thread_context, xstate) == 224, "context XSAVE offset");
struct client { long process, thread, control_fd; struct control *control; };
extern unsigned char remote_blob[], remote_blob_end[];

void *memset(void *p, int value, size_t n) {
    unsigned char *b = p; for (size_t i = 0; i < n; ++i) b[i] = (unsigned char)value; return p;
}
void *memcpy(void *p, const void *q, size_t n) {
    unsigned char *b = p; const unsigned char *s = q;
    for (size_t i = 0; i < n; ++i) b[i] = s[i]; return p;
}
static void log_line(const char *s) {
    size_t n = 0; while (s[n]) ++n; (void)pacha_syscall2(1, (uintptr_t)s, n);
}
__attribute__((noreturn)) static void fail(unsigned line) {
    char buf[24]; unsigned n = 0; do {buf[n++] = '0' + line % 10; line /= 10;} while (line);
    for (unsigned i = 0; i < n / 2; ++i) {char c = buf[i]; buf[i] = buf[n-i-1]; buf[n-i-1] = c;}
    buf[n++] = '\n'; buf[n] = 0; log_line(buf); log_line("NATIVE_REMOTE_MAPPING=FAIL\n");
    (void)pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT, 1); __builtin_trap();
}
#define CHECK(x) do {if (!(x)) fail(__LINE__);} while (0)
static uint64_t now(void) {
    uint64_t ts[2]; CHECK(!pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME, 1, (uintptr_t)ts));
    return ts[0] * 1000000000ull + ts[1];
}
static void await(uint64_t *value, uint64_t expected) {
    uint64_t until = now() + 3000000000ull;
    while (LOAD(*value) != expected) CHECK(now() < until);
}
static long vmo(size_t size) {
    long fd = pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, size, VMO_RIGHTS, 0); CHECK(fd >= 16); return fd;
}
static void *alias(long fd, size_t size) {
    long va = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, fd, 0, size, RW, PACHA_MMAP_SHARED, 0);
    CHECK(va >= (long)PAGE); return (void *)(uintptr_t)va;
}
static long map(long process, long fd, uintptr_t va, size_t size, uint64_t prot, uint64_t offset, uint64_t flags) {
    return pacha_syscall6(PACHA_PROCESS_SYSCALL_MAP, process, fd, va, size, prot, offset | flags);
}
static long unmap(long process, uintptr_t va, size_t size) {
    return pacha_syscall4(PACHA_PROCESS_SYSCALL_UNMAP, process, va, size, 0);
}
static long context(long fd, unsigned op, struct pacha_thread_context *image) {
    return pacha_syscall4(PACHA_THREAD_SYSCALL_CONTEXT, fd, op, (uintptr_t)image, sizeof(*image));
}
static int equal(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; ++i) if (x[i] != y[i]) return 0;
    return 1;
}
static void invalid_context(long fd, struct pacha_thread_context *image,
                            const struct pacha_thread_context *before) {
    struct pacha_thread_context after;
    CHECK(context(fd, PACHA_THREAD_CONTEXT_SET, image) == PACHA_SYSCALL_ERR_INVALID);
    CHECK(!context(fd, PACHA_THREAD_CONTEXT_GET, &after));
    CHECK(equal(before, &after, sizeof(after)));
}
static void probe(struct client *c, uintptr_t va, uint64_t write_value, uint64_t expected, uint64_t vector) {
    struct control *p = c->control;
    p->access_va = va; p->write_value = write_value; p->value = UINT64_MAX; p->vector = 0; p->address = 0;
    uint64_t command = LOAD(p->command) + 1; STORE(p->command, command); await(&p->done, command);
    CHECK(p->vector == vector);
    if (vector) CHECK(p->address == va); else CHECK(p->value == expected);
}
static struct client create(long code_fd, long data_fd) {
    struct client c = {0};
    c.process = pacha_syscall5(PACHA_PROCESS_SYSCALL_CREATE, 0, PROCESS_RIGHTS, 0, 0, 0); CHECK(c.process >= 16);
    c.control_fd = vmo(PAGE); c.control = alias(c.control_fd, PAGE); memset(c.control, 0, PAGE);
    CHECK(map(c.process, code_fd, CODE, PAGE, PACHA_PROT_READ | PACHA_PROT_EXEC, 0, PACHA_PROCESS_MAP_SHARED) == CODE);
    CHECK(map(c.process, c.control_fd, CONTROL, PAGE, RW, 0, PACHA_PROCESS_MAP_SHARED) == CONTROL);
    CHECK(map(c.process, data_fd, TARGET, 3 * PAGE, RW, 0, PACHA_PROCESS_MAP_SHARED) == TARGET);
    CHECK(map(c.process, 0, STACK, 8 * PAGE, RW, 0, PACHA_PROCESS_MAP_PRIVATE | PACHA_PROCESS_MAP_ANONYMOUS) == STACK);
    c.thread = pacha_syscall6(PACHA_THREAD_SYSCALL_CREATE, c.process, CODE, STACK + 4 * PAGE,
        0, 0, PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_SET_CONTEXT | PACHA_FD_RIGHT_DUP); CHECK(c.thread >= 16);
    struct pacha_thread_context initial;
    CHECK(!context(c.thread, PACHA_THREAD_CONTEXT_GET, &initial));
    CHECK(initial.size == sizeof(initial) && initial.xstate_features == 7);
    CHECK(initial.registers.rip == CODE && initial.registers.rsp == STACK + 4 * PAGE);
    CHECK(!context(c.thread, PACHA_THREAD_CONTEXT_SET, &initial));
    CHECK(!pacha_syscall1(PACHA_THREAD_SYSCALL_START, c.thread)); await(&c.control->ready, 1);
    CHECK(c.control->fd_free == c.control->fd_capacity - 16);
    return c;
}

/* Position independent child image, copied into a parent-owned code VMO.
 * Between commands the child makes no syscall and retains its old TLB state.
 * The fault handler resumes after the deliberate access, never remaps pages. */
__asm__(".pushsection remote_code,\"ax\",@progbits\n"
    ".global remote_blob,remote_blob_end\nremote_blob:\n"
    "mov $0x42000000,%r13\n"
    "mov $0,%edi; lea 64(%r13),%rsi; mov $" STR(PACHA_FD_SYSCALL_TABLE) ",%eax; syscall\n"
    "mov $8,%edi; lea remote_fault(%rip),%rsi; mov $0x45004000,%edx; mov $16384,%r10d; "
        "mov $" STR(PACHA_PROCESS_SYSCALL_SIGNAL_CTL) ",%eax; syscall\n"
    "test %rax,%rax; jnz remote_bad\nmovq $1,56(%r13)\n"
    "remote_loop: mov 0(%r13),%r12; cmp 8(%r13),%r12; je remote_pause\n"
    "cmp $-1,%r12; je remote_exit\n"
    "cmp $-2,%r12; je remote_capture\n"
    "mov 48(%r13),%rbx; test %rbx,%rbx; jz remote_zero; mov (%rbx),%rax; mov %rax,16(%r13)\n"
    "mov 24(%r13),%rax; test %rax,%rax; jz remote_after; mov %rax,(%rbx)\n"
    "remote_after: mov %r12,8(%r13); jmp remote_loop\n"
    "remote_pause: pause; jmp remote_loop\n"
    "remote_zero: xor %eax,%eax; call *%rax; mov %rax,16(%r13); jmp remote_after\n"
    "remote_capture: mov %r10,88(%r13); mov %r15,96(%r13); "
        "mov %fs:0,%rax; mov %rax,104(%r13); mov %gs:0,%rax; mov %rax,112(%r13); "
        "movdqu %xmm15,120(%r13); jmp remote_after\n"
    "remote_fault: mov %rdi,%rsi; mov 1024(%rsi),%rax; mov %rax,32(%r13); "
        "mov 1040(%rsi),%rax; mov %rax,40(%r13); lea remote_after(%rip),%rax; mov %rax,152(%rsi); "
        "mov $3,%edi; mov $" STR(PACHA_PROCESS_SYSCALL_SIGNAL_CTL) ",%eax; syscall; ud2\n"
    "remote_exit: xor %edi,%edi; mov $" STR(PACHA_PROCESS_SYSCALL_EXIT) ",%eax; syscall; ud2\n"
    "remote_bad: movq $2,56(%r13); jmp remote_pause\n"
    "remote_blob_end:\n.popsection\n");

static void test_context(struct client *c) {
    struct pacha_thread_context before, image, after;
    log_line("NATIVE_THREAD_CONTEXT=START\n");
    CHECK(context(c->thread, PACHA_THREAD_CONTEXT_GET, &before) == PACHA_SYSCALL_ERR_NOT_READY);
    CHECK(!pacha_syscall2(PACHA_PROCESS_SYSCALL_STOP, c->process, 19));
    CHECK(!context(c->thread, PACHA_THREAD_CONTEXT_GET, &before));
    long inspect = pacha_syscall4(PACHA_FD_SYSCALL_DUP, c->thread, 16,
        PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE, 0);
    long change = pacha_syscall4(PACHA_FD_SYSCALL_DUP, c->thread, 16,
        PACHA_FD_RIGHT_SET_CONTEXT | PACHA_FD_RIGHT_CLOSE, 0);
    CHECK(inspect >= 16 && change >= 16);
    CHECK(context(inspect, PACHA_THREAD_CONTEXT_SET, &before) == PACHA_SYSCALL_ERR_INVALID);
    CHECK(context(change, PACHA_THREAD_CONTEXT_GET, &before) == PACHA_SYSCALL_ERR_INVALID);
    CHECK(context(c->thread, 2, &before) == PACHA_SYSCALL_ERR_INVALID);
    CHECK(pacha_syscall4(PACHA_THREAD_SYSCALL_CONTEXT, c->thread, 0,
        (uintptr_t)&before, sizeof(before) - 1) == PACHA_SYSCALL_ERR_INVALID);
    CHECK(pacha_syscall4(PACHA_THREAD_SYSCALL_CONTEXT, c->thread, 0,
        0, sizeof(before)) == PACHA_SYSCALL_ERR_INVALID);
    image = before; image.registers.cs = 8; invalid_context(c->thread, &image, &before);
    image = before; image.registers.rip = UINT64_MAX; invalid_context(c->thread, &image, &before);
    image = before; image.fs_base = 0xffff800000000000ull; invalid_context(c->thread, &image, &before);
    image = before; image.gs_base = 0x800000000000ull; invalid_context(c->thread, &image, &before);
    image = before; image.pkru = 1ull << 32; invalid_context(c->thread, &image, &before);
    image = before; image.reserved[1] = 1; invalid_context(c->thread, &image, &before);
    image = before; image.size--; invalid_context(c->thread, &image, &before);
    image = before; image.xstate_features = 0; invalid_context(c->thread, &image, &before);
    image = before; image.xstate[527] = 0x80; invalid_context(c->thread, &image, &before);
    image = before; image.xstate[27] |= 0x80; invalid_context(c->thread, &image, &before);
    image = before; image.registers.rflags |= 0x3000 | 0x4000 | 0x20000;
    image.registers.rflags &= ~0x200ull;
    CHECK(!context(change, PACHA_THREAD_CONTEXT_SET, &image));
    CHECK(!context(inspect, PACHA_THREAD_CONTEXT_GET, &after));
    CHECK((after.registers.rflags & (0x3000 | 0x4000 | 0x20000 | 0x200)) == 0x200);
    for (unsigned i = 0; i < 32; ++i) {
        if (i) {
            CHECK(!pacha_syscall2(PACHA_PROCESS_SYSCALL_STOP, c->process, 19));
            CHECK(!context(c->thread, PACHA_THREAD_CONTEXT_GET, &image));
        } else image = after;
        image.registers.r10 = 0xabc000 + i; image.registers.r15 = 0xdef000 + i;
        image.fs_base = CONTROL + offsetof(struct control, write_value);
        image.gs_base = CONTROL + offsetof(struct control, access_va);
        image.xstate[512] |= 2; /* SSE component is present in standard XSAVE. */
        uint64_t pattern[2] = {0x1122334455667700ull + i, 0x9988776655443300ull + i};
        memcpy(image.xstate + 400, pattern, sizeof(pattern));
        CHECK(!context(c->thread, PACHA_THREAD_CONTEXT_SET, &image));
        CHECK(!context(c->thread, PACHA_THREAD_CONTEXT_GET, &after));
        CHECK(equal(&image, &after, sizeof(image)));
        c->control->write_value = 0x111000 + i; c->control->access_va = 0x222000 + i;
        STORE(c->control->done, 0); STORE(c->control->command, UINT64_MAX - 1);
        CHECK(!pacha_syscall2(PACHA_PROCESS_SYSCALL_CONTINUE, c->process, 0));
        await(&c->control->done, UINT64_MAX - 1);
        CHECK(c->control->saved_r10 == 0xabc000 + i && c->control->saved_r15 == 0xdef000 + i);
        CHECK(c->control->saved_fs == 0x111000 + i && c->control->saved_gs == 0x222000 + i);
        CHECK(equal(c->control->saved_xmm15, pattern, sizeof(pattern)));
    }
    CHECK(!pacha_syscall2(PACHA_PROCESS_SYSCALL_STOP, c->process, 19));
    CHECK(!context(c->thread, PACHA_THREAD_CONTEXT_SET, &before));
    STORE(c->control->command, 0); STORE(c->control->done, 0);
    CHECK(!pacha_syscall2(PACHA_PROCESS_SYSCALL_CONTINUE, c->process, 0));
    CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, inspect));
    CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, change));
    log_line("NATIVE_THREAD_CONTEXT=PASS\n");
}

static void test_low_region(struct client *client, long data_fd, volatile uint64_t *data) {
    const uintptr_t low = 0x10000, boundary = 0x1ff000;
    const uint64_t zero_value = 0x12345678;
    const unsigned char zero_code[] = {0xb8, 0x78, 0x56, 0x34, 0x12, 0xc3};
    long zero_fd = vmo(PAGE);
    void *zero_alias = alias(zero_fd, PAGE);

    memcpy(zero_alias, zero_code, sizeof(zero_code));
    long executable = pacha_syscall4(PACHA_FD_SYSCALL_DUP, zero_fd, 16,
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_EXEC | PACHA_FD_RIGHT_CLOSE, 0);

    CHECK(executable >= 16);
    CHECK(map(client->process, executable, 0, PAGE, PACHA_PROT_READ | PACHA_PROT_EXEC,
        0, PACHA_PROCESS_MAP_SHARED) == 0);
    /* Address zero selects an actual CALL in the native client, not a read.
     * This must keep working while sibling low PTEs are replaced/removed. */
    probe(client, 0, 0, zero_value, 0);
    CHECK(map(client->process, data_fd, low, PAGE, 0, 0, PACHA_PROCESS_MAP_REPLACE) == low);
    probe(client, low, 0, 0, 14);
    CHECK(map(client->process, data_fd, low, PAGE, RW, 0, PACHA_PROCESS_MAP_REPLACE) == low);
    probe(client, low, 71, data[0], 0);
    CHECK(data[0] == 71);
    CHECK(map(client->process, data_fd, low + 2 * PAGE, PAGE, RW, 2 * PAGE,
        PACHA_PROCESS_MAP_REPLACE) == low + 2 * PAGE);
    CHECK(map(client->process, data_fd, low, PAGE, PACHA_PROT_READ, 0,
        PACHA_PROCESS_MAP_REPLACE) == low);
    probe(client, low, 72, 0, 14);
    CHECK(data[0] == 71);
    CHECK(map(client->process, data_fd, low, PAGE, RW, 3 * PAGE,
        PACHA_PROCESS_MAP_REPLACE) == PACHA_SYSCALL_ERR_INVALID);
    probe(client, low, 0, 71, 0);
    CHECK(!unmap(client->process, low, 2 * PAGE));
    probe(client, low, 0, 0, 14);
    probe(client, low + 2 * PAGE, 0, data[2 * PAGE / 8], 0);
    probe(client, 0, 0, zero_value, 0);
    CHECK(!unmap(client->process, low, 3 * PAGE));

    CHECK(map(client->process, data_fd, boundary, 3 * PAGE, RW, 0,
        PACHA_PROCESS_MAP_REPLACE) == boundary);
    probe(client, boundary + PAGE, 0, data[PAGE / 8], 0);
    CHECK(!unmap(client->process, boundary, 3 * PAGE));
    probe(client, boundary + PAGE, 0, 0, 14);
    probe(client, 0, 0, zero_value, 0);
    CHECK(map(client->process, data_fd, low - PAGE, PAGE, RW, 0,
        PACHA_PROCESS_MAP_REPLACE) != low - PAGE);
    CHECK(unmap(client->process, low - PAGE, PAGE) != 0);
    probe(client, 0, 0, zero_value, 0);

    /* An unstarted process permits a full guest-window reset without also
     * removing this fixture's own running code/control/stack mappings. */
    long empty = pacha_syscall5(PACHA_PROCESS_SYSCALL_CREATE, 0, PROCESS_RIGHTS, 0, 0, 0);
    const uintptr_t upper = UINT64_C(0x7fff00000000), high = upper - 3 * PAGE;

    CHECK(empty >= 16);
    CHECK(map(empty, data_fd, low, PAGE, RW, 0, PACHA_PROCESS_MAP_REPLACE) == low);
    CHECK(map(empty, data_fd, high, PAGE, RW, 0, PACHA_PROCESS_MAP_REPLACE) == high);
    uint64_t started = now();

    CHECK(!unmap(empty, low, upper - low));
    CHECK(now() - started < UINT64_C(3000000000));
    CHECK(!unmap(empty, low, upper - low));
    CHECK(map(empty, data_fd, low, PAGE, RW, 0, PACHA_PROCESS_MAP_SHARED) == low);
    CHECK(map(empty, data_fd, high, PAGE, RW, 0, PACHA_PROCESS_MAP_SHARED) == high);
    CHECK(!pacha_syscall2(PACHA_PROCESS_SYSCALL_KILL, empty, 0));
    uint64_t status[4], deadline = now() + UINT64_C(3000000000);
    long waited;

    while ((waited = pacha_syscall2(PACHA_PROCESS_SYSCALL_WAIT, empty,
        (uintptr_t)status)) == PACHA_SYSCALL_ERR_NOT_READY) CHECK(now() < deadline);
    CHECK(!waited && status[0] == 3);
    CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, empty));
    CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, executable));
    CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, zero_fd));
    CHECK(!pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, (uintptr_t)zero_alias, PAGE));
    data[0] = 11;
    log_line("NATIVE_REMOTE_LOW_RANGE=PASS\n");
}

int main(void) {
    log_line("NATIVE_REMOTE_MAPPING=START\n");
    long code_fd = vmo(PAGE); void *code = alias(code_fd, PAGE);
    CHECK((size_t)(remote_blob_end - remote_blob) < PAGE); memcpy(code, remote_blob, remote_blob_end - remote_blob);
    long data_fd = vmo(3 * PAGE); volatile uint64_t *data = alias(data_fd, 3 * PAGE);
    data[0] = 11; data[PAGE / 8] = 22; data[2 * PAGE / 8] = 33;
    struct client a = create(code_fd, data_fd), b = create(code_fd, data_fd);
    test_context(&a);
    test_low_region(&a, data_fd, data);
    probe(&a, TARGET + PAGE, 0, 22, 0); probe(&b, TARGET + PAGE, 0, 22, 0);
    CHECK(!pacha_syscall2(PACHA_PROCESS_SYSCALL_STOP, a.process, 19));
    CHECK(!unmap(a.process, TARGET + PAGE, PAGE));
    CHECK(map(a.process, data_fd, TARGET + PAGE, PAGE, RW, 2 * PAGE,
        PACHA_PROCESS_MAP_REPLACE | PACHA_PROCESS_MAP_SHARED) == TARGET + PAGE);
    CHECK(!pacha_syscall2(PACHA_PROCESS_SYSCALL_CONTINUE, a.process, 0));
    probe(&a, TARGET + PAGE, 0, 33, 0);
    for (unsigned i = 0; i < 32; ++i) {
        size_t offset = (i % 3) * PAGE; uint64_t value = 1000 + i;
        data[offset / 8] = value;
        CHECK(map(a.process, data_fd, TARGET + PAGE, PAGE, RW, offset,
            PACHA_PROCESS_MAP_REPLACE | (i & 1 ? PACHA_PROCESS_MAP_SHARED : 0)) == TARGET + PAGE);
        probe(&a, TARGET + PAGE, value + 100, value, 0); CHECK(data[offset / 8] == value + 100);
        probe(&b, TARGET + PAGE, 0, data[PAGE / 8], 0);
        probe(&a, TARGET, 0, data[0], 0); probe(&a, TARGET + 2 * PAGE, 0, data[2 * PAGE / 8], 0);
    }
    long restricted = pacha_syscall4(PACHA_FD_SYSCALL_DUP, a.process, 16, PACHA_FD_RIGHT_CLOSE, 0); CHECK(restricted >= 16);
    CHECK(unmap(restricted, TARGET + PAGE, PAGE) == PACHA_SYSCALL_ERR_INVALID);
    CHECK(map(restricted, data_fd, TARGET + PAGE, PAGE, RW, 0, PACHA_PROCESS_MAP_REPLACE) == PACHA_SYSCALL_ERR_INVALID);
    CHECK(map(a.process, data_fd, TARGET + PAGE, PAGE, RW, 3 * PAGE, PACHA_PROCESS_MAP_REPLACE) == PACHA_SYSCALL_ERR_INVALID);
    CHECK(map(a.process, data_fd, PACHA_PROCESS_MAP_ANYWHERE, PAGE, RW, 0, PACHA_PROCESS_MAP_REPLACE) == PACHA_SYSCALL_ERR_INVALID);
    long read_only = pacha_syscall4(PACHA_FD_SYSCALL_DUP, data_fd, 16,
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_CLOSE, 0); CHECK(read_only >= 16);
    CHECK(map(a.process, read_only, TARGET + PAGE, PAGE, RW, 0,
        PACHA_PROCESS_MAP_REPLACE | PACHA_PROCESS_MAP_SHARED) == PACHA_SYSCALL_ERR_INVALID);
    CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, read_only));
    struct pacha_process_map_batch_entry batch = {.vmo_fd = data_fd, .target_va = TARGET + PAGE,
        .size = PAGE, .prot = RW, .flags = PACHA_PROCESS_MAP_REPLACE};
    CHECK(pacha_syscall3(PACHA_PROCESS_SYSCALL_MAP_BATCH, a.process, (uintptr_t)&batch, 1) == PACHA_SYSCALL_ERR_INVALID);
    CHECK(pacha_syscall4(PACHA_PROCESS_SYSCALL_UNMAP, a.process, TARGET + PAGE, PAGE, 1) == PACHA_SYSCALL_ERR_INVALID);
    CHECK(unmap(a.process, TARGET + PAGE + 1, PAGE) == PACHA_SYSCALL_ERR_INVALID);
    CHECK(unmap(a.process, UINT64_MAX - PAGE + 1, 2 * PAGE) != 0);
    probe(&a, TARGET + PAGE, 0, data[PAGE / 8], 0);
    CHECK(map(a.process, data_fd, TARGET + PAGE, PAGE, PACHA_PROT_READ, 0, PACHA_PROCESS_MAP_REPLACE) == TARGET + PAGE);
    probe(&a, TARGET + PAGE, 888, 0, 14); CHECK(data[0] != 888);
    uint64_t done = LOAD(a.control->done);
    CHECK(!unmap(a.process, TARGET + PAGE, PAGE)); CHECK(LOAD(a.control->done) == done);
    CHECK(!unmap(a.process, TARGET + PAGE, PAGE)); /* Idempotent holes. */
    probe(&a, TARGET + PAGE, 0, 0, 14);
    probe(&a, TARGET, 0, data[0], 0); probe(&a, TARGET + 2 * PAGE, 0, data[2 * PAGE / 8], 0);
    probe(&b, TARGET + PAGE, 0, data[PAGE / 8], 0);
    CHECK(map(a.process, data_fd, TARGET + PAGE, PAGE, RW, 2 * PAGE, PACHA_PROCESS_MAP_REPLACE) == TARGET + PAGE);
    CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, data_fd));
    probe(&a, TARGET + PAGE, 7777, data[2 * PAGE / 8], 0); CHECK(data[2 * PAGE / 8] == 7777);
    CHECK(!unmap(a.process, TARGET, 3 * PAGE)); CHECK(!unmap(b.process, TARGET, 3 * PAGE));
    for (unsigned i = 0; i < 2; ++i) {
        struct client *c = i ? &b : &a; STORE(c->control->command, UINT64_MAX);
        uint64_t status[4], deadline = now() + 3000000000ull; long rc;
        while ((rc = pacha_syscall2(PACHA_PROCESS_SYSCALL_WAIT, c->process, (uintptr_t)status)) == PACHA_SYSCALL_ERR_NOT_READY) CHECK(now() < deadline);
        CHECK(!rc && status[0] == 2 && status[1] == 0);
        struct pacha_thread_context dead;
        CHECK(context(c->thread, PACHA_THREAD_CONTEXT_GET, &dead) == PACHA_SYSCALL_ERR_INVALID);
        CHECK(unmap(c->process, TARGET, PAGE) == PACHA_SYSCALL_ERR_INVALID);
        CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, c->thread));
        CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, c->process));
        CHECK(!pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, (uintptr_t)c->control, PAGE));
        CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, c->control_fd));
    }
    CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, restricted));
    CHECK(!pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, (uintptr_t)data, 3 * PAGE));
    CHECK(!pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, (uintptr_t)code, PAGE));
    CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, code_fd));
    log_line("NATIVE_REMOTE_MAPPING=PASS\n"); return 0;
}
