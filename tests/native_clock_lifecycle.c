/* Exercise scalar clock admission while another CPU retires its process. */
#include <stdint.h>
#include <stddef.h>
#include "pacha/ipc.h"
#include "pacha/syscall.h"

#define STR1(x) #x
#define STR(x) STR1(x)
#define PAGE 4096ul
#define CODE 0x46000000ul
#define CONTROL 0x47000000ul
#define STACK 0x48000000ul
#define RW (PACHA_PROT_READ | PACHA_PROT_WRITE)
#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)

struct clock_control { uint64_t count, last, error, cpus; };
extern unsigned char clock_remote_blob[], clock_remote_end[];

/* The copied image has no references to parent text or private data. A shared
 * VMO lets the parent detect writes after a lifecycle ACK without executing
 * another syscall in the child or assuming a local FD close means retirement. */
__asm__(".pushsection clock_remote_code,\"ax\",@progbits\n"
    ".global clock_remote_blob,clock_remote_end\nclock_remote_blob:\n"
    "mov $0x47000000,%r12; xor %r13d,%r13d\n"
    "clock_remote_loop: mov $1,%edi; mov $" STR(PACHA_RUNTIME_SYSCALL_CLOCK_READ) ",%eax; syscall\n"
    "test %rax,%rax; jnz clock_remote_status\n"
    "cmp %r13,%rdx; jb clock_remote_bad\n"
    "mov %rdx,%r13; mov %rdx,8(%r12); addq $1,0(%r12)\n"
    "mov $1,%eax; cpuid; shr $24,%ebx; bts %rbx,24(%r12); jmp clock_remote_loop\n"
    "clock_remote_status: cmp $" STR(PACHA_SYSCALL_ERR_NOT_READY) ",%rax; jne clock_remote_bad\n"
    "test %rdx,%rdx; jnz clock_remote_bad; jmp clock_remote_loop\n"
    "clock_remote_bad: movq $1,16(%r12); pause; jmp clock_remote_bad\n"
    "clock_remote_end:\n.popsection\n");

static int failure(unsigned line)
{
    static const char prefix[] = "NATIVE_CLOCK_LIFECYCLE=FAIL line=";
    pacha_syscall2(1, (uintptr_t)prefix, sizeof(prefix) - 1);
    char digits[16]; unsigned n = 0;
    do { digits[n++] = '0' + line % 10; line /= 10; } while (line);
    for (unsigned i = 0; i < n / 2; ++i) {
        char c = digits[i]; digits[i] = digits[n - i - 1]; digits[n - i - 1] = c;
    }
    digits[n++] = '\n'; pacha_syscall2(1, (uintptr_t)digits, n);
    return 1;
}
#define CHECK(x) do { if (!(x)) return failure(__LINE__); } while (0)

static uint64_t now(void)
{
    uint64_t value;
    return pacha_clock_read(1, &value) ? UINT64_MAX : value;
}

static int await_progress(struct clock_control *control, uint64_t count)
{
    const uint64_t start = now(); CHECK(start != UINT64_MAX);
    while (LOAD(control->count) <= count + 128) {
        CHECK(!LOAD(control->error) && now() - start < 3000000000ull);
        __asm__ volatile("pause");
    }
    return 0;
}

static int unchanged(struct clock_control *control, uint64_t count, uint64_t last)
{
    const uint64_t start = now(); CHECK(start != UINT64_MAX);
    do {
        const uint64_t current = now(); CHECK(current != UINT64_MAX && current >= last);
        CHECK(LOAD(control->count) == count && LOAD(control->last) == last && !LOAD(control->error));
        if (current - start >= 3000000) break;
    } while (1);
    return 0;
}

int native_clock_lifecycle_contract(void)
{
    unsigned remote_cpus = 0;
    const uint64_t vmo_rights = PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_MAP_EXEC | PACHA_FD_RIGHT_CLOSE;
    const long code = pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, PAGE, vmo_rights, 0);
    CHECK(code >= 16);
    const long code_alias = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, code, 0, PAGE, RW, PACHA_MMAP_SHARED, 0);
    CHECK(code_alias >= (long)PAGE);
    const size_t bytes = (uintptr_t)clock_remote_end - (uintptr_t)clock_remote_blob;
    CHECK(bytes > 0 && bytes <= PAGE);
    for (size_t i = 0; i < bytes; ++i) ((unsigned char *)(uintptr_t)code_alias)[i] = clock_remote_blob[i];
    const uint64_t process_rights = PACHA_FD_RIGHT_MAP_INTO | PACHA_FD_RIGHT_SPAWN |
        PACHA_FD_RIGHT_SET_CONTEXT | PACHA_FD_RIGHT_KILL | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE;
    for (unsigned generation = 0; generation < 8; ++generation) {
        const long process = pacha_syscall5(PACHA_PROCESS_SYSCALL_CREATE, 0, process_rights, 0, 0, 0);
        CHECK(process >= 16);
        const long control_fd = pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, PAGE, vmo_rights, 0);
        CHECK(control_fd >= 16);
        const long alias = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, control_fd, 0, PAGE, RW, PACHA_MMAP_SHARED, 0);
        CHECK(alias >= (long)PAGE);
        struct clock_control *control = (void *)(uintptr_t)alias;
        control->count = control->last = control->error = control->cpus = 0;
        CHECK(pacha_syscall6(PACHA_PROCESS_SYSCALL_MAP, process, code, CODE, PAGE,
            PACHA_PROT_READ | PACHA_PROT_EXEC, PACHA_PROCESS_MAP_SHARED) == CODE);
        CHECK(pacha_syscall6(PACHA_PROCESS_SYSCALL_MAP, process, control_fd, CONTROL, PAGE,
            RW, PACHA_PROCESS_MAP_SHARED) == CONTROL);
        CHECK(pacha_syscall6(PACHA_PROCESS_SYSCALL_MAP, process, 0, STACK, PAGE,
            RW, PACHA_PROCESS_MAP_PRIVATE | PACHA_PROCESS_MAP_ANONYMOUS) == STACK);
        const long thread = pacha_syscall6(PACHA_THREAD_SYSCALL_CREATE, process,
            CODE, STACK + PAGE, 0, 0, PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_CLOSE);
        CHECK(thread >= 16 && pacha_syscall1(PACHA_THREAD_SYSCALL_START, thread) == 0);
        CHECK(await_progress(control, 0) == 0);
        unsigned eax = 1, ebx, ecx, edx;
        __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
        remote_cpus |= (unsigned)LOAD(control->cpus) & ~(1u << (ebx >> 24));
        for (unsigned cycle = 0; cycle < 16; ++cycle) {
            CHECK(pacha_syscall2(PACHA_PROCESS_SYSCALL_STOP, process, 19) == 0);
            const uint64_t count = LOAD(control->count), last = LOAD(control->last);
            CHECK(unchanged(control, count, last) == 0);
            CHECK(pacha_syscall2(PACHA_PROCESS_SYSCALL_CONTINUE, process, 0) == 0);
            CHECK(await_progress(control, count) == 0);
        }
        CHECK(pacha_syscall2(PACHA_PROCESS_SYSCALL_KILL, process, 73) == 0);
        const uint64_t count = LOAD(control->count), last = LOAD(control->last);
        CHECK(unchanged(control, count, last) == 0);
        uint64_t status[4];
        CHECK(pacha_syscall2(PACHA_PROCESS_SYSCALL_WAIT, process, (uintptr_t)status) == 0);
        CHECK(status[0] == 3 && status[1] == 73);
        CHECK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, thread) == 0);
        CHECK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, process) == 0);
        CHECK(pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, alias, PAGE) == 0);
        CHECK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, control_fd) == 0);
    }
    CHECK(pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, code_alias, PAGE) == 0);
    CHECK(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, code) == 0);
    CHECK(remote_cpus != 0);
    static const char passed[] = "NATIVE_CLOCK_LIFECYCLE=PASS\n";
    pacha_syscall2(1, (uintptr_t)passed, sizeof(passed) - 1);
    return 0;
}
