/* SPDX-License-Identifier: MIT */
/* Linux regression launcher shim: reserve anonymous PROT_NONE windows in
 * ascending VA order, like PachaOS. Never replace an existing mapping. */
#define _GNU_SOURCE
#include <errno.h>
#include <link.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static atomic_uint_fast64_t next_window;

static int find_core(struct dl_phdr_info *image, size_t size, void *argument) {
    (void)size;
    if (strstr(image->dlpi_name, "linux-boot-runtime.so")) {
        /* Keep all windows near the core, as both current loaders do;
         * this test varies order, not the distance from the loaded image. */
        *(uint64_t *)argument = (image->dlpi_addr - (UINT64_C(1) << 30)) &
            ~((UINT64_C(2) << 20) - 1);
        return 1;
    }
    return 0;
}

void *mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset) {
    if (!address && protection == PROT_NONE &&
        flags == (MAP_PRIVATE | MAP_ANONYMOUS)) {
        const uint64_t alignment = UINT64_C(2) << 20;

        if (!length || length > (UINT64_C(256) << 20)) {
            errno = EINVAL;
            return MAP_FAILED;
        }
        if (!atomic_load_explicit(&next_window, memory_order_relaxed)) {
            uint64_t start = 0, expected = 0;

            dl_iterate_phdr(find_core, &start);
            if (!start) {
                return (void *)syscall(SYS_mmap, address, length, protection, flags, fd, offset);
            }
            atomic_compare_exchange_strong(&next_window, &expected, start);
        }
        uint64_t stride = (length + alignment - 1) & ~(alignment - 1);
        uint64_t requested = atomic_fetch_add_explicit(&next_window, stride,
            memory_order_relaxed);
        void *mapped = (void *)syscall(SYS_mmap, requested, length, protection,
            flags | MAP_FIXED_NOREPLACE, fd, offset);

        if (mapped != MAP_FAILED && mapped != (void *)(uintptr_t)requested) {
            (void)syscall(SYS_munmap, mapped, length);
            errno = EOPNOTSUPP;
            return MAP_FAILED;
        }
        return mapped;
    }
    return (void *)syscall(SYS_mmap, address, length, protection, flags, fd, offset);
}
