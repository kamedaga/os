/* End-to-end fd_info lifetime snapshot. No FileD mocks or Linux personality. */
#include <stddef.h>
#include <stdint.h>
#include "pacha/ipc.h"

static uint64_t call(uint64_t n, uint64_t a, uint64_t b, uint64_t c,
    uint64_t d, uint64_t e, uint64_t f)
{
    register uint64_t r10 __asm__("r10") = d;
    register uint64_t r8 __asm__("r8") = e;
    register uint64_t r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "+a"(n) : "D"(a), "S"(b), "d"(c),
        "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return n;
}

static void log_text(const char *text)
{
    size_t n = 0;
    while (text[n]) ++n;
    (void)call(1, (uintptr_t)text, n, 0, 0, 0, 0);
}

__attribute__((noreturn)) static void fail(unsigned line)
{
    char text[] = "NATIVE_VMO_LIFETIME=FAIL line=0000\n";
    for (unsigned i = 0; i < 4; ++i) {
        text[sizeof(text) - 3 - i] = (char)('0' + line % 10);
        line /= 10;
    }
    log_text(text);
    (void)call(PACHA_PROCESS_SYSCALL_EXIT, 1, 0, 0, 0, 0, 0);
    __builtin_trap();
}
#define CHECK(c) do { if (!(c)) fail(__LINE__); } while (0)

static void refs(uint64_t fd, uint32_t objects, uint32_t native)
{
    struct pacha_fd_info info = {0};
    CHECK(call(PACHA_FD_SYSCALL_GET_INFO, fd, (uintptr_t)&info, 0, 0, 0, 0) == 0);
    CHECK(info.kind == PACHA_FD_KIND_VMO);
    CHECK(info.extra == ((uint64_t)objects | ((uint64_t)native << 32)));
}

static void close_fd(uint64_t fd)
{
    CHECK(call(PACHA_FD_SYSCALL_CLOSE, fd, 0, 0, 0, 0, 0) == 0);
}

static uint64_t dirty_bits(uint64_t view)
{
    uint64_t bits = UINT64_MAX;
    CHECK(call(PACHA_FD_SYSCALL_VMO_COLLECT_DIRTY, view, (uintptr_t)&bits, 1, 0, 0, 0) == 0);
    return bits;
}

static volatile unsigned dirty_permit, dirty_done;
static uint64_t dirty_writer_alias;
static unsigned char dirty_writer_stack[65536] __attribute__((aligned(16)));
__attribute__((used,noreturn)) static void dirty_writer_body(void)
{
    for (unsigned round = 1; round <= 64; ++round) {
        while (__atomic_load_n(&dirty_permit, __ATOMIC_ACQUIRE) != round)
            __asm__ volatile("pause");
        volatile uint64_t *word = (void *)(uintptr_t)(dirty_writer_alias + (round % 3) * 4096);
        for (unsigned i = 0; i < 4096; ++i) *word = ((uint64_t)round << 32) | i;
        __atomic_store_n(&dirty_done, round, __ATOMIC_RELEASE);
    }
    (void)call(PACHA_THREAD_SYSCALL_EXIT, 0, 0, 0, 0, 0, 0);
    __builtin_trap();
}
extern void dirty_writer_entry(void);
__asm__(".text\n.global dirty_writer_entry\ndirty_writer_entry: and $-16,%rsp; call dirty_writer_body; ud2\n");

static void concurrent_dirty(uint64_t view, uint64_t alias)
{
    dirty_writer_alias = alias;
    const uint64_t thread = call(PACHA_THREAD_SYSCALL_CREATE, PACHA_PROCESS_SELF_FD,
        (uintptr_t)dirty_writer_entry, (uintptr_t)(dirty_writer_stack + sizeof(dirty_writer_stack)),
        0, 0, PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE);
    CHECK(thread >= 16 && thread < PACHA_THREAD_SELF_FD);
    CHECK(call(PACHA_THREAD_SYSCALL_START, thread, 0, 0, 0, 0, 0) == 0);
    for (unsigned round = 1; round <= 64; ++round) {
        __atomic_store_n(&dirty_permit, round, __ATOMIC_RELEASE);
        uint64_t seen = dirty_bits(view);
        while (__atomic_load_n(&dirty_done, __ATOMIC_ACQUIRE) != round)
            (void)call(PACHA_RUNTIME_SYSCALL_YIELD, 0, 0, 0, 0, 0, 0);
        seen |= dirty_bits(view);
        CHECK(seen & (UINT64_C(1) << (round % 3)));
    }
    uint64_t status[4] = {0};
    while (call(PACHA_THREAD_SYSCALL_WAIT, thread, (uintptr_t)status, 0, 0, 0, 0))
        (void)call(PACHA_RUNTIME_SYSCALL_YIELD, 0, 0, 0, 0, 0, 0);
    CHECK(status[0] == 2 && status[1] == 0);
    close_fd(thread);
    (void)dirty_bits(view); // late D may conservatively repeat a reported page
    CHECK(dirty_bits(view) == 0);
    log_text("NATIVE_VMO_DIRTY_SMP=PASS concurrent-writer/collect/late-dirty/clean\n");
}

static void dirty_views(void)
{
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_SHARE | PACHA_FD_RIGHT_REVOKE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    const uint64_t parent = call(PACHA_FD_SYSCALL_VMO_CREATE, 12288, rights, 0, 0, 0, 0);
    CHECK(parent >= 16 && parent < PACHA_THREAD_SELF_FD);
    const uint64_t owner = call(PACHA_VM_SYSCALL_MMAP, parent, 0, 12288,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    CHECK(owner >= 4096);
    uint64_t indices[3] = {0, 1, 2};
    const uint64_t view = call(PACHA_FD_SYSCALL_VMO_CREATE_PAGE_VIEW, parent,
        (uintptr_t)indices, 3, rights & ~PACHA_FD_RIGHT_SHARE, 0, 0);
    CHECK(view >= 16 && view < PACHA_THREAD_SELF_FD);
    const uint64_t a = call(PACHA_VM_SYSCALL_MMAP, view, 0, 12288,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    const uint64_t b = call(PACHA_VM_SYSCALL_MMAP, view, 0, 12288,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    CHECK(a >= 4096 && b >= 4096 && a != b);
    CHECK(dirty_bits(view) == 0);
    *(volatile uint64_t *)(uintptr_t)a = 11;
    *(volatile uint64_t *)(uintptr_t)(b + 8192) = 22;
    CHECK(dirty_bits(view) == 5);
    CHECK(dirty_bits(view) == 0);
    concurrent_dirty(view, b);
    *(volatile uint64_t *)(uintptr_t)(a + 4096) = 33;
    uint64_t bits = UINT64_MAX;
    CHECK(call(PACHA_FD_SYSCALL_VMO_COLLECT_DIRTY, view, (uintptr_t)&bits, 1, 1, 0, 0) != 0);
    CHECK(call(PACHA_FD_SYSCALL_VMO_COLLECT_DIRTY, view, a, 1, 0, 0, 0) != 0);
    CHECK(call(PACHA_FD_SYSCALL_VMO_COLLECT_DIRTY, view, owner, 1, 0, 0, 0) != 0);
    const uint64_t attenuated = call(PACHA_FD_SYSCALL_DUP, view, 16,
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ, 0, 0, 0);
    CHECK(attenuated >= 16 && attenuated < PACHA_THREAD_SELF_FD);
    CHECK(call(PACHA_FD_SYSCALL_VMO_COLLECT_DIRTY, attenuated, (uintptr_t)&bits, 1, 0, 0, 0) != 0);
    CHECK(dirty_bits(view) == 2);
    close_fd(attenuated);
    *(volatile uint64_t *)(uintptr_t)(a + 8192) = 44;
    CHECK(call(PACHA_VM_SYSCALL_MPROTECT, a, 12288, PACHA_PROT_READ, 0, 0, 0) == 0);
    CHECK(dirty_bits(view) == 7); // mprotect retires old PTEs conservatively
    *(volatile uint64_t *)(uintptr_t)b = 55;
    CHECK(call(PACHA_VM_SYSCALL_MUNMAP, b, 12288, 0, 0, 0, 0) == 0);
    CHECK(dirty_bits(view) == 7);
    CHECK(dirty_bits(view) == 0);
    CHECK(call(PACHA_VM_SYSCALL_MPROTECT, a, 12288, PACHA_PROT_READ | PACHA_PROT_WRITE, 0, 0, 0) == 0);
    const uint64_t event = call(PACHA_FD_SYSCALL_EVENTFD_CREATE, 77,
        PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_CLOSE, 0, 0, 0, 0);
    CHECK(event >= 16 && event < PACHA_THREAD_SELF_FD);
    CHECK(call(PACHA_FD_SYSCALL_READ, event, a, 8, 0, 0, 0) == 8);
    CHECK(*(volatile uint64_t *)(uintptr_t)a == 77);
    CHECK(dirty_bits(view) == 7); // physical copy-window write
    close_fd(event);
    struct pacha_futex_wake_op operation = {.operation = PACHA_FUTEX_OP_ADD,
        .operand = 1, .comparison = PACHA_FUTEX_CMP_EQ, .compare_operand = 77};
    CHECK(call(PACHA_RUNTIME_SYSCALL_FUTEX_WAKE_OP, a, a, 0, 0,
        (uintptr_t)&operation, 0) == 0);
    CHECK(*(volatile uint32_t *)(uintptr_t)a == 78);
    CHECK(dirty_bits(view) == 7); // physical alias RMW
    indices[0] = 2; indices[2] = 0;
    CHECK(call(PACHA_FD_SYSCALL_VMO_REFRESH_PAGE_VIEW, view, parent,
        (uintptr_t)indices, 3, 0, 0) == 0);
    CHECK(dirty_bits(view) == 7);
    CHECK(dirty_bits(view) == 0);
    *(volatile uint64_t *)(uintptr_t)(a + 4096) = 66;
    CHECK(dirty_bits(view) == 2);
    const uint64_t target = call(PACHA_VM_SYSCALL_MMAP, 0, 0, 12288,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS, 0);
    CHECK(target >= 4096);
    CHECK(call(PACHA_VM_SYSCALL_MREMAP, a, 12288, 12288, 3, target, 0) == target); // MAYMOVE|FIXED
    CHECK(dirty_bits(view) == 7);
    CHECK(call(PACHA_VM_SYSCALL_MREMAP, target, 12288, 8192, 0, 0, 0) == target);
    CHECK(dirty_bits(view) == 7);
    *(volatile uint64_t *)(uintptr_t)(target + 4096) = 88;
    CHECK(dirty_bits(view) == 2);
    CHECK(call(PACHA_VM_SYSCALL_MMAP, 0, target + 4096, 4096,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS | PACHA_MMAP_FIXED, 0) == target + 4096);
    CHECK(dirty_bits(view) == 7);
    CHECK(call(PACHA_FD_SYSCALL_VMO_REVOKE, view, 0, 0, 0, 0, 0) == 0);
    CHECK(call(PACHA_VM_SYSCALL_MUNMAP, target + 4096, 4096, 0, 0, 0, 0) == 0);
    CHECK(call(PACHA_VM_SYSCALL_MUNMAP, owner, 12288, 0, 0, 0, 0) == 0);
    close_fd(parent);
    log_text("NATIVE_VMO_DIRTY=PASS aliases/clean/authority/physical-alias/readonly/munmap/copyout/futex/refresh/mremap/shrink/fixed\n");
}

static void chunk_views(uint64_t parent)
{
    uint64_t indices[65];
    const uint64_t rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_REVOKE;
    for (unsigned count = 64; count <= 65; ++count) {
        for (unsigned i = 0; i < count; ++i) indices[i] = i % 3;
        uint64_t view = call(PACHA_FD_SYSCALL_VMO_CREATE_PAGE_VIEW, parent,
            (uintptr_t)indices, count, rights, 0, 0);
        CHECK(view >= 16 && view < PACHA_THREAD_SELF_FD);
        uint64_t mapping = call(PACHA_VM_SYSCALL_MMAP, view, 0, count * 4096,
            PACHA_PROT_READ, PACHA_MMAP_SHARED, 0);
        CHECK(mapping >= 4096);
        CHECK(*(volatile unsigned char *)(uintptr_t)mapping == 73);
        if (count == 65) {
            /* A bad second chunk must not replace the valid first chunk of
             * an existing alias or retain the failed staging parent's ref. */
            for (unsigned i = 0; i < count; ++i) indices[i] = 2;
            indices[64] = 3;
            CHECK(call(PACHA_FD_SYSCALL_VMO_REFRESH_PAGE_VIEW, view, parent,
                (uintptr_t)indices, count, 0, 0) != 0);
            CHECK(*(volatile unsigned char *)(uintptr_t)mapping == 73);
            CHECK(*(volatile unsigned char *)(uintptr_t)(mapping + 64 * 4096) == 127);
            refs(parent, 1, 3);
            CHECK(call(PACHA_FD_SYSCALL_VMO_CREATE_PAGE_VIEW, parent,
                (uintptr_t)indices, count, rights, 0, 0) < 16);
            refs(parent, 1, 3);
        }
        CHECK(call(PACHA_VM_SYSCALL_MUNMAP, mapping, count * 4096, 0, 0, 0, 0) == 0);
        close_fd(view);
        refs(parent, 1, 2);
    }
    log_text("NATIVE_VMO_CHUNKS=PASS boundary/late-failure/atomic-refresh/refs\n");
}

__asm__(".global _start\n_start: and $-16, %rsp; call main; ud2\n");

__attribute__((noreturn)) void main(void)
{
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_SHARE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE | PACHA_FD_RIGHT_REVOKE;
    const uint64_t fd = call(PACHA_FD_SYSCALL_VMO_CREATE, 12288, rights, 0, 0, 0, 0);
    CHECK(fd >= 16 && fd < PACHA_THREAD_SELF_FD);
    refs(fd, 1, 1);
    const uint64_t owner = call(PACHA_VM_SYSCALL_MMAP, fd, 0, 12288,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    CHECK(owner >= 4096);
    *(volatile unsigned char *)(uintptr_t)owner = 73;
    refs(fd, 1, 2);
    const uint64_t dup = call(PACHA_FD_SYSCALL_DUP, fd, 16,
        rights & ~PACHA_FD_RIGHT_INSPECT, 0, 0, 0);
    CHECK(dup >= 16 && dup < PACHA_THREAD_SELF_FD);
    refs(dup, 0, 0);
    refs(fd, 2, 2);
    const uint64_t borrower = call(PACHA_VM_SYSCALL_MMAP, dup, 0, 12288,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    CHECK(borrower >= 4096 && borrower != owner);
    close_fd(dup);
    refs(fd, 1, 3);
    CHECK(*(volatile unsigned char *)(uintptr_t)borrower == 73);
    CHECK(call(PACHA_VM_SYSCALL_MPROTECT, borrower + 4096, 4096,
        PACHA_PROT_READ, 0, 0, 0) == 0);
    refs(fd, 1, 5);
    CHECK(call(PACHA_VM_SYSCALL_MUNMAP, borrower, 12288, 0, 0, 0, 0) == 0);
    refs(fd, 1, 2);
    const uint64_t endpoint = call(PACHA_IPC_SYSCALL_ENDPOINT_CREATE,
        PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_CLOSE, 0, 0, 0, 0, 0);
    CHECK(endpoint >= 16 && endpoint < PACHA_THREAD_SELF_FD);
    struct pacha_ipc_fd transferred = { .fd = fd, .rights = PACHA_FD_RIGHT_CLOSE };
    struct pacha_ipc_msg msg = { .fds = &transferred, .fd_count = 1, .fd_capacity = 1 };
    CHECK(pacha_ipc_message_syscall(PACHA_IPC_SYSCALL_SEND, endpoint, (struct pacha_ipc_msg *)(uintptr_t)((uintptr_t)&msg), 0, 0, 0, 0) == 0);
    refs(fd, 2, 2);
    CHECK(pacha_ipc_message_syscall(PACHA_IPC_SYSCALL_RECV, endpoint, 0, (struct pacha_ipc_msg *)(uintptr_t)((uintptr_t)&msg), 0, 0, 0) == 0);
    CHECK(msg.fd_count == 1 && transferred.fd != fd);
    refs(fd, 2, 2);
    close_fd(transferred.fd);
    refs(fd, 1, 2);
    close_fd(endpoint);
    const uint64_t index = 0;
    const uint64_t view = call(PACHA_FD_SYSCALL_VMO_CREATE_PAGE_VIEW,
        fd, (uintptr_t)&index, 1, PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ, 0, 0);
    CHECK(view >= 16 && view < PACHA_THREAD_SELF_FD);
    refs(fd, 1, 3);
    const uint64_t view_map = call(PACHA_VM_SYSCALL_MMAP, view, 0, 4096,
        PACHA_PROT_READ, PACHA_MMAP_SHARED, 0);
    CHECK(view_map >= 4096);
    close_fd(view);
    refs(fd, 1, 3);
    CHECK(*(volatile unsigned char *)(uintptr_t)view_map == 73);
    CHECK(call(PACHA_VM_SYSCALL_MUNMAP, view_map, 4096, 0, 0, 0, 0) == 0);
    refs(fd, 1, 2);
    *(volatile unsigned char *)(uintptr_t)(owner + 4096) = 89;
    *(volatile unsigned char *)(uintptr_t)(owner + 8192) = 107;
    uint64_t indices[2] = {0, PACHA_VMO_PAGE_VIEW_HOLE};
    const uint64_t mutable = call(PACHA_FD_SYSCALL_VMO_CREATE_PAGE_VIEW,
        fd, (uintptr_t)indices, 2, (rights & ~PACHA_FD_RIGHT_SHARE) | PACHA_FD_RIGHT_REVOKE,
        PACHA_VMO_PAGE_VIEW_SPARSE, 0);
    CHECK(mutable >= 16 && mutable < PACHA_THREAD_SELF_FD);
    const uint64_t alias_a = call(PACHA_VM_SYSCALL_MMAP, mutable, 0, 8192,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    const uint64_t alias_b = call(PACHA_VM_SYSCALL_MMAP, mutable, 0, 8192,
        PACHA_PROT_READ, PACHA_MMAP_SHARED, 0);
    CHECK(alias_a >= 4096 && alias_b >= 4096 && alias_a != alias_b);
    CHECK(*(volatile unsigned char *)(uintptr_t)alias_a == 73);
    CHECK(*(volatile unsigned char *)(uintptr_t)alias_b == 73);
    indices[0] = 1; indices[1] = 2;
    const uint64_t reader = call(PACHA_FD_SYSCALL_DUP, mutable, 16,
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ, 0, 0, 0);
    CHECK(reader >= 16 && reader < PACHA_THREAD_SELF_FD);
    CHECK(call(PACHA_FD_SYSCALL_VMO_REFRESH_PAGE_VIEW, reader, fd,
        (uintptr_t)indices, 2, 0, 0) != 0);
    CHECK(*(volatile unsigned char *)(uintptr_t)alias_a == 73);
    CHECK(call(PACHA_FD_SYSCALL_VMO_REFRESH_PAGE_VIEW, mutable, fd,
        (uintptr_t)indices, 2, 0, 0) == 0);
    CHECK(*(volatile unsigned char *)(uintptr_t)alias_a == 89);
    CHECK(*(volatile unsigned char *)(uintptr_t)alias_b == 89);
    CHECK(*(volatile unsigned char *)(uintptr_t)(alias_a + 4096) == 107);
    *(volatile unsigned char *)(uintptr_t)alias_a = 127;
    CHECK(*(volatile unsigned char *)(uintptr_t)(owner + 4096) == 127);
    CHECK(*(volatile unsigned char *)(uintptr_t)alias_b == 127);
    indices[1] = PACHA_VMO_PAGE_VIEW_HOLE;
    CHECK(call(PACHA_FD_SYSCALL_VMO_REFRESH_PAGE_VIEW, mutable, fd,
        (uintptr_t)indices, 2, 0, 0) == 0);
    CHECK(*(volatile unsigned char *)(uintptr_t)alias_b == 127);
    close_fd(reader);
    CHECK(call(PACHA_VM_SYSCALL_MUNMAP, alias_a, 8192, 0, 0, 0, 0) == 0);
    CHECK(call(PACHA_VM_SYSCALL_MUNMAP, alias_b, 8192, 0, 0, 0, 0) == 0);
    close_fd(mutable);
    refs(fd, 1, 2);
    log_text("NATIVE_VMO_REFRESH=PASS aliases/hole-fill/clear/rights/parent-write\n");
    chunk_views(fd);
    CHECK(call(PACHA_VM_SYSCALL_MUNMAP, owner, 12288, 0, 0, 0, 0) == 0);
    refs(fd, 1, 1);
    close_fd(fd);
    dirty_views();
    log_text("NATIVE_VMO_LIFETIME=PASS\n");
    (void)call(PACHA_PROCESS_SYSCALL_EXIT, 0, 0, 0, 0, 0, 0);
    __builtin_trap();
}
