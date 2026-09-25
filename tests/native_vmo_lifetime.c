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

__asm__(".global _start\n_start: and $-16, %rsp; call main; ud2\n");

__attribute__((noreturn)) void main(void)
{
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_SHARE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    const uint64_t fd = call(PACHA_FD_SYSCALL_VMO_CREATE, 12288, rights, 0, 0, 0, 0);
    CHECK(fd >= 16 && fd < PACHA_FD_TABLE_LIMIT);
    refs(fd, 1, 1);
    const uint64_t owner = call(PACHA_VM_SYSCALL_MMAP, fd, 0, 12288,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    CHECK(owner >= 4096);
    *(volatile unsigned char *)(uintptr_t)owner = 73;
    refs(fd, 1, 2);
    const uint64_t dup = call(PACHA_FD_SYSCALL_DUP, fd, 16,
        rights & ~PACHA_FD_RIGHT_INSPECT, 0, 0, 0);
    CHECK(dup >= 16 && dup < PACHA_FD_TABLE_LIMIT);
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
    CHECK(endpoint >= 16 && endpoint < PACHA_FD_TABLE_LIMIT);
    struct pacha_ipc_fd transferred = { .fd = fd, .rights = PACHA_FD_RIGHT_CLOSE };
    struct pacha_ipc_msg msg = { .fds = &transferred, .fd_count = 1, .fd_capacity = 1 };
    CHECK(call(PACHA_IPC_SYSCALL_SEND, endpoint, (uintptr_t)&msg, 0, 0, 0, 0) == 0);
    refs(fd, 2, 2);
    CHECK(call(PACHA_IPC_SYSCALL_RECV, endpoint, (uintptr_t)&msg, 0, 0, 0, 0) == 0);
    CHECK(msg.fd_count == 1 && transferred.fd != fd);
    refs(fd, 2, 2);
    close_fd(transferred.fd);
    refs(fd, 1, 2);
    close_fd(endpoint);
    const uint64_t index = 0;
    const uint64_t view = call(PACHA_FD_SYSCALL_VMO_CREATE_PAGE_VIEW,
        fd, (uintptr_t)&index, 1, PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ, 0, 0);
    CHECK(view >= 16 && view < PACHA_FD_TABLE_LIMIT);
    refs(fd, 1, 3);
    const uint64_t view_map = call(PACHA_VM_SYSCALL_MMAP, view, 0, 4096,
        PACHA_PROT_READ, PACHA_MMAP_SHARED, 0);
    CHECK(view_map >= 4096);
    close_fd(view);
    refs(fd, 1, 3);
    CHECK(*(volatile unsigned char *)(uintptr_t)view_map == 73);
    CHECK(call(PACHA_VM_SYSCALL_MUNMAP, view_map, 4096, 0, 0, 0, 0) == 0);
    refs(fd, 1, 2);
    CHECK(call(PACHA_VM_SYSCALL_MUNMAP, owner, 12288, 0, 0, 0, 0) == 0);
    refs(fd, 1, 1);
    close_fd(fd);
    log_text("NATIVE_VMO_LIFETIME=PASS\n");
    (void)call(PACHA_PROCESS_SYSCALL_EXIT, 0, 0, 0, 0, 0, 0);
    __builtin_trap();
}
