#ifndef CAP_ABILITY_OS_CAP_SYSCALL_H
#define CAP_ABILITY_OS_CAP_SYSCALL_H

#include <stdint.h>

#include "cap_abi.h"

enum cap_syscall_no {
    CAP_SYSCALL_ALLOC_PAGE = 0x1,
    CAP_SYSCALL_MAP_PAGE = 0x2,
    CAP_SYSCALL_MOVE_CAP = 0x3,
    CAP_SYSCALL_DROP_PRESENT = 0x4,
    CAP_SYSCALL_SWITCH_THREAD = 0x5,
    CAP_SYSCALL_SEND_CAP = 0x6,
    CAP_SYSCALL_REVOKE_TREE = 0x7,
    CAP_SYSCALL_GRANT_CAP = 0x8,
    CAP_SYSCALL_LOG = 0x9,
    CAP_SYSCALL_RECV_CAP = 0xA,
    CAP_SYSCALL_MAP_MMIO = 0xB,
    CAP_SYSCALL_ALLOC_MAP_PAGES = 0xC,
    CAP_SYSCALL_CREATE_WINDOW = 0xD,
    CAP_SYSCALL_QUEUE_SUBMIT = 0xE,
    CAP_SYSCALL_QUEUE_NOTIFY = 0xF,
    CAP_SYSCALL_UNTYPED_ALLOC = 0x10,
    CAP_SYSCALL_UNTYPED_RETYPE_PAGES = 0x11,
    CAP_SYSCALL_UNTYPED_RESET = 0x12,
    CAP_SYSCALL_UNTYPED_ALLOC_MAP_PAGES = 0x13,
    CAP_SYSCALL_GRANT_CAPS_BATCH = 0x14,
    CAP_SYSCALL_INSTALL_ENDPOINT = 0x26,
    CAP_SYSCALL_SIGNAL_ENDPOINT = 0x2C
};

enum cap_syscall_status {
    CAP_SYSCALL_OK = 0,
    CAP_SYSCALL_ERR_INVALID = 1,
    CAP_SYSCALL_ERR_NOT_READY = 2,
    CAP_SYSCALL_ERR_ALLOC = 4,
    CAP_SYSCALL_ERR_MAP = 5,
    CAP_SYSCALL_ERR_MOVE = 6,
    CAP_SYSCALL_ERR_DROP_PRESENT = 7,
    CAP_SYSCALL_ERR_SEND = 8,
    CAP_SYSCALL_ERR_ENDPOINT = 9,
    CAP_SYSCALL_ERR_REVOKE = 10,
    CAP_SYSCALL_ERR_GRANT = 11,
    CAP_SYSCALL_ERR_LOG = 12,
    CAP_SYSCALL_ERR_EMPTY = 13
};

static inline uint64_t cap_syscall0(uint64_t nr) {
    uint64_t ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr)
        : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t cap_syscall1(uint64_t nr, uint64_t a0) {
    uint64_t ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0)
        : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t cap_syscall2(uint64_t nr, uint64_t a0, uint64_t a1) {
    uint64_t ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t cap_syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    uint64_t ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
        : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t cap_syscall4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
        : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t cap_syscall5(
    uint64_t nr,
    uint64_t a0,
    uint64_t a1,
    uint64_t a2,
    uint64_t a3,
    uint64_t a4
) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8") = a4;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t cap_syscall6(
    uint64_t nr,
    uint64_t a0,
    uint64_t a1,
    uint64_t a2,
    uint64_t a3,
    uint64_t a4,
    uint64_t a5
) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8") = a4;
    register uint64_t r9 __asm__("r9") = a5;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return ret;
}

#endif /* CAP_ABILITY_OS_CAP_SYSCALL_H */
