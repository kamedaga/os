/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_KOBOX2_IPC_COMMON_H
#define NATIVE_KOBOX2_IPC_COMMON_H

#include "../userland/kobox2_adapter/ipc.h"
#include "../userland/kobox2_adapter/bootstrap.h"
#include <pacha/syscall.h>
#include <errno.h>

#define IPC_TEST_PAGE 4096ul
#define IPC_TEST_CODE 0x44000000ul
#define IPC_TEST_STACK 0x45000000ul
#define IPC_TEST_CONFIG 0x46000000ul
#define IPC_TEST_CHILD_FD 16
#define IPC_TEST_RW (PACHA_PROT_READ | PACHA_PROT_WRITE)
#define IPC_TEST_VMO_RIGHTS (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE | \
    PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE)
#define IPC_TEST_MARK UINT64_C(0x7274726e)

/* Test-only native messages, not gpud protocol opcodes. */
enum { IPC_TEST_READY = 1, IPC_TEST_SHARE, IPC_TEST_RETURN, IPC_TEST_MOVE, IPC_TEST_HELD,
    IPC_TEST_BOOTSTRAP_DONE, IPC_TEST_PACKAGE_SNAPSHOT, IPC_TEST_PACKAGE_MUTATED,
    IPC_TEST_PACKAGE_REJECTED };
enum { IPC_TEST_NORMAL, IPC_TEST_STALE, IPC_TEST_BOOTSTRAP, IPC_TEST_BOOTSTRAP_BAD_ORDER,
    IPC_TEST_TIMED, IPC_TEST_PACKAGE, IPC_TEST_PACKAGE_CORRUPT, IPC_TEST_PACKAGE_OLD_GRANT };
#define IPC_TEST_BOOTSTRAP_ARTIFACTS 20u
#define IPC_TEST_BOOTSTRAP_RESOURCES 3u
#define IPC_TEST_BOOTSTRAP_ITEMS (2u + IPC_TEST_BOOTSTRAP_ARTIFACTS + IPC_TEST_BOOTSTRAP_RESOURCES)
struct ipc_test_config {
    uint64_t generation, mode;
    struct ph_package_identity identity;
};

void *memset(void *memory, int value, size_t count) {
    unsigned char *bytes = memory;
    for (size_t i = 0; i < count; ++i) bytes[i] = (unsigned char)value;
    return memory;
}

void *memcpy(void *destination, const void *source, size_t count) {
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < count; ++i) out[i] = in[i];
    return destination;
}

int memcmp(const void *left, const void *right, size_t size) {
    const unsigned char *a = left, *b = right;
    for (size_t i = 0; i < size; ++i) if (a[i] != b[i]) return a[i] - b[i];
    return 0;
}

size_t strlen(const char *text) {
    size_t size = 0;
    while (text[size]) ++size;
    return size;
}

static inline void ipc_test_log(const char *text) {
    size_t count = 0;
    while (text[count]) ++count;
    (void)pacha_syscall2(1, (uintptr_t)text, count);
}

__attribute__((noreturn)) static inline void ipc_test_fail(unsigned int line) {
    char digits[16];
    unsigned int count = 0;
    do { digits[count++] = '0' + line % 10; line /= 10; } while (line);
    for (unsigned int i = 0; i < count / 2; ++i) {
        char swap = digits[i]; digits[i] = digits[count - i - 1]; digits[count - i - 1] = swap;
    }
    digits[count++] = '\n'; digits[count] = 0;
#ifdef IPC_TEST_CHILD
    ipc_test_log("NATIVE_KOBOX2_IPC_CHILD line=");
#else
    ipc_test_log("NATIVE_KOBOX2_IPC_PARENT line=");
#endif
    ipc_test_log(digits);
    ipc_test_log("NATIVE_KOBOX2_IPC=FAIL\n");
    (void)pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT, 1);
    __builtin_trap();
}

#define IPC_CHECK(expression) do { if (!(expression)) ipc_test_fail(__LINE__); } while (0)

static inline uint64_t ipc_test_now(void) {
    uint64_t time[2];
    long status;
    /* Remote stop/kill temporarily closes syscall admission. NOT_READY has
     * not populated time; neither inspect it nor turn cancellation into an
     * assertion failure. A killed child will be descheduled by its owner. */
    do {
        status = pacha_syscall2(PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME, 1, (uintptr_t)time);
        if (status == PACHA_SYSCALL_ERR_NOT_READY) __asm__ volatile("pause" ::: "memory");
    } while (status == PACHA_SYSCALL_ERR_NOT_READY);
    IPC_CHECK(!status);
    return time[0] * UINT64_C(1000000000) + time[1];
}

static inline int ipc_test_receive(struct ph_ipc *ipc, struct ph_ipc_packet *packet) {
    uint64_t deadline = ipc_test_now() + UINT64_C(5000000000);
    int result;
    while ((result = ph_ipc_receive_wait(ipc, ipc->generation, packet, 5000)) == -EAGAIN)
        IPC_CHECK(ipc_test_now() < deadline);
    return result;
}

static inline void ipc_test_send(struct ph_ipc *ipc, struct ph_ipc_packet *packet) {
    uint64_t deadline = ipc_test_now() + UINT64_C(5000000000);
    int result;
    while ((result = ph_ipc_send(ipc, packet)) == -EAGAIN)
        IPC_CHECK(ipc_test_now() < deadline);
    IPC_CHECK(!result);
}

static inline uint64_t ipc_test_used_fds(void) {
    struct pacha_fd_table_info table;
    IPC_CHECK(!pacha_syscall2(PACHA_FD_SYSCALL_TABLE, 0, (uintptr_t)&table));
    return table.capacity - table.free_slots;
}

static inline void ipc_test_close(uint64_t fd) {
    IPC_CHECK(!pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, fd));
}

static inline void *ipc_test_map(uint64_t fd, size_t size, uint64_t protection) {
    long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, fd, 0, size,
        protection, PACHA_MMAP_SHARED, 0);
    IPC_CHECK(address >= (long)IPC_TEST_PAGE);
    return (void *)(uintptr_t)address;
}

static inline void ipc_test_unmap(void *address, size_t size) {
    IPC_CHECK(!pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, (uintptr_t)address, size));
}

#endif
