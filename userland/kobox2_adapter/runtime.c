/* SPDX-License-Identifier: MIT */
/* Minimal support for the freestanding sandbox launcher, not Linux primitives. */
#include "host.h"
#include <elf.h>

/* Native init is relocated before normal C initialization. Do not use globals
 * or logging here: this static PIE needs only base-relative relocations. */
void ph_relocate_self(uintptr_t base) {
    const Elf64_Ehdr *header = (void *)base;
    const Elf64_Phdr *segments = (void *)(base + header->e_phoff);

    for (unsigned i = 0; i < header->e_phnum; ++i) {
        if (segments[i].p_type != PT_DYNAMIC) {
            continue;
        }

        const Elf64_Dyn *dynamic = (void *)(base + segments[i].p_vaddr);
        Elf64_Rela *relocations = NULL;
        size_t relocation_bytes = 0;

        for (; dynamic->d_tag != DT_NULL; ++dynamic) {
            if (dynamic->d_tag == DT_RELA) {
                relocations = (void *)(base + dynamic->d_un.d_ptr);
            }
            if (dynamic->d_tag == DT_RELASZ) {
                relocation_bytes = dynamic->d_un.d_val;
            }
        }

        for (size_t j = 0; j < relocation_bytes / sizeof(*relocations); ++j) {
            const Elf64_Rela *relocation = &relocations[j];

            if (ELF64_R_TYPE(relocation->r_info) != R_X86_64_RELATIVE) {
                __builtin_trap();
            }
            *(uintptr_t *)(base + relocation->r_offset) = base + relocation->r_addend;
        }
    }
}

void *memcpy(void *destination, const void *source, size_t length) {
    unsigned char *dest = destination;
    const unsigned char *src = source;

    while (length-- != 0) {
        *dest++ = *src++;
    }
    return destination;
}

void *memset(void *destination, int value, size_t length) {
    unsigned char *dest = destination;

    while (length-- != 0) {
        *dest++ = (unsigned char)value;
    }
    return destination;
}

int memcmp(const void *left, const void *right, size_t length) {
    const unsigned char *left_bytes = left;
    const unsigned char *right_bytes = right;

    for (size_t i = 0; i < length; ++i) {
        if (left_bytes[i] != right_bytes[i]) {
            return left_bytes[i] - right_bytes[i];
        }
    }
    return 0;
}

size_t strlen(const char *text) {
    size_t length = 0;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

int strcmp(const char *left, const char *right) {
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

void ph_log(const char *text) {
    /* Native user-log syscall; no file service is required during boot. */
    (void)pacha_syscall2(1, (uintptr_t)text, strlen(text));
}

void ph_number(const char *name, uint64_t value) {
    char buffer[160];
    char reverse_digits[24];
    size_t length = 0;
    unsigned digit_count = 0;

    while (*name != '\0' && length < 120) {
        buffer[length++] = *name++;
    }
    buffer[length++] = '=';

    do {
        reverse_digits[digit_count++] = '0' + value % 10;
        value /= 10;
    } while (value != 0);

    while (digit_count != 0) {
        buffer[length++] = reverse_digits[--digit_count];
    }
    buffer[length++] = '\n';
    buffer[length] = '\0';
    ph_log(buffer);
}

static void (*failure_reporter)(const char *, unsigned, uint64_t);
static void (*exception_reporter)(const struct pacha_native_fault_frame *);

void ph_set_failure_reporter(void (*reporter)(const char *, unsigned, uint64_t)) {
    failure_reporter = reporter;
}

void ph_set_exception_reporter(void (*reporter)(const struct pacha_native_fault_frame *)) {
    exception_reporter = reporter;
}

void ph_report_exception(const struct pacha_native_fault_frame *frame) {
    if (exception_reporter) exception_reporter(frame);
}

_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result) {
    /* A fatal sandbox exit otherwise leaves an unobservable generic EIO on
     * GOP-only live boots. Best-effort reporting must never delay exit. */
    if (failure_reporter) failure_reporter(file, line, result);
    ph_log("PACHA_KOBOX_FOUNDATION=FAIL\n");
    ph_log(file);
    ph_number("line", line);
    ph_number("result", result);
    (void)pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT, 1);

    for (;;) {
        __asm__ volatile("pause");
    }
}

void *ph_alloc(size_t size) {
    PH_CHECK(size != 0 && size <= SIZE_MAX - PH_PAGE_SIZE);
    size = (size + PH_PAGE_SIZE - 1) & ~(PH_PAGE_SIZE - 1);

    long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, 0, 0, size,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS, 0);
    if (address < (long)PH_PAGE_SIZE) {
        ph_fail(__FILE__, __LINE__, address);
    }
    return (void *)(uintptr_t)address;
}

void ph_free(void *base, size_t size) {
    size_t mapped_size = (size + PH_PAGE_SIZE - 1) & ~(PH_PAGE_SIZE - 1);

    PH_OK(pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, (uintptr_t)base, mapped_size));
}

void ph_wait(atomic_uint *word, unsigned expected) {
    long result = pacha_syscall3(PACHA_RUNTIME_SYSCALL_FUTEX_WAIT,
        (uintptr_t)word, expected, 0);

    /* NOT_READY means the producer changed the word before we could sleep. */
    if (result != 0 && result != PACHA_SYSCALL_ERR_NOT_READY)
        ph_fail(__FILE__, __LINE__, result);
}

void ph_wake(atomic_uint *word) {
    long result = pacha_syscall2(PACHA_RUNTIME_SYSCALL_FUTEX_WAKE,
        (uintptr_t)word, UINT64_MAX);

    PH_CHECK(result >= 0);
}

void ph_lock(atomic_uint *lock) {
    /* 0: free, 1: owned without known waiters, 2: owned with possible waiters.
     * A failed fast path must not erase another waiter's state. */
    unsigned expected = 0;
    if (atomic_compare_exchange_strong_explicit(lock, &expected, 1,
            memory_order_acquire, memory_order_relaxed)) {
        return;
    }
    /* Keep state 2 when acquiring after contention, so our unlock also wakes
     * sleepers left behind by another owner. The futex expected-value check
     * handles release between publishing state 2 and entering the kernel. */
    while (atomic_exchange_explicit(lock, 2, memory_order_acquire) != 0) {
        ph_wait(lock, 2);
    }
}

void ph_unlock(atomic_uint *lock) {
    if (atomic_exchange_explicit(lock, 0, memory_order_release) == 2) {
        ph_wake(lock);
    }
}
