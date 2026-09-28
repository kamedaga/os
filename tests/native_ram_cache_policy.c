/* SPDX-License-Identifier: MIT */
/* Exercise the native 2 MiB WB -> UC- -> WB transaction without a GPU. */
#include <stddef.h>
#include <stdint.h>
#include <pacha/ipc.h>

#define RANGE_BYTES (2u * 1024u * 1024u)
#define VMO_BYTES (384u * 1024u * 1024u)
#define CACHE_OFFSET (68u * 1024u * 1024u)
#ifndef CACHE_POLICY_ALIAS_COUNT
#define CACHE_POLICY_ALIAS_COUNT 1
#endif
#if CACHE_POLICY_ALIAS_COUNT < 1 || CACHE_POLICY_ALIAS_COUNT > 64
#error CACHE_POLICY_ALIAS_COUNT must be between 1 and 64
#endif
#ifndef CACHE_POLICY_CLONE_COUNT
#define CACHE_POLICY_CLONE_COUNT 0
#endif
#if CACHE_POLICY_CLONE_COUNT < 0 || CACHE_POLICY_CLONE_COUNT > 20
#error CACHE_POLICY_CLONE_COUNT must be between 0 and 20
#endif
#ifndef CACHE_POLICY_ACTIVE_READERS
#define CACHE_POLICY_ACTIVE_READERS 0
#endif
#if CACHE_POLICY_ACTIVE_READERS != 0 && CACHE_POLICY_ACTIVE_READERS != 1
#error CACHE_POLICY_ACTIVE_READERS must be 0 or 1
#endif
#ifndef CACHE_POLICY_SCATTER_SPAN_PAGES
#define CACHE_POLICY_SCATTER_SPAN_PAGES 0
#endif
#if CACHE_POLICY_SCATTER_SPAN_PAGES < 0 || CACHE_POLICY_SCATTER_SPAN_PAGES > 128
#error CACHE_POLICY_SCATTER_SPAN_PAGES must be between 0 and 128
#endif

/* Messages remain mutable to keep test output in a writable load segment. */
char message_create[] = "RAM_CACHE_POLICY=FAIL create ";
char message_map[] = "RAM_CACHE_POLICY=FAIL map ";
char message_clone[] = "RAM_CACHE_POLICY=FAIL clone ";
char message_scatter[] = "RAM_CACHE_POLICY=FAIL scatter ";
char message_begin[] = "RAM_CACHE_POLICY=BEGIN 2MiB UC-\n";
char message_uc_failure[] = "RAM_CACHE_POLICY=FAIL uc ";
char message_uc_done[] = "RAM_CACHE_POLICY=UC_DONE\n";
char message_wb_failure[] = "RAM_CACHE_POLICY=FAIL wb ";
char message_wb_done[] = "RAM_CACHE_POLICY=WB_DONE\n";
char message_wc_failure[] = "RAM_CACHE_POLICY=FAIL wc ";
char message_view_failure[] = "RAM_CACHE_POLICY=FAIL page_view ";
char message_view_map_failure[] = "RAM_CACHE_POLICY=FAIL page_view_map ";
char message_view_content_failure[] = "RAM_CACHE_POLICY=FAIL page_view_content\n";
char message_view_done[] = "RAM_CACHE_POLICY=PAGE_VIEW_DONE\n";
char message_release[] = "RAM_CACHE_POLICY=FAIL release\n";
char message_pass[] = "RAM_CACHE_POLICY=PASS\n";
static uint64_t page_view_indices[RANGE_BYTES / 4096];

static long call(long nr, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "+a"(nr) : "D"(a), "S"(b), "d"(c),
        "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return nr;
}

static void log_text(const char *text) {
    size_t n = 0;
    while (text[n]) ++n;
    (void)call(1, (long)(uintptr_t)text, (long)n, 0, 0, 0, 0);
}

static void log_hex(uint64_t value) {
    char text[] = "0x0000000000000000\n";
    for (unsigned i = 0; i != 16; ++i) {
        text[17 - i] = "0123456789abcdef"[(value >> (4 * i)) & 15];
    }
    log_text(text);
}

__attribute__((noreturn)) static void finish(int status) {
    (void)call(PACHA_PROCESS_SYSCALL_EXIT, status, 0, 0, 0, 0, 0);
    __builtin_trap();
}

int main(void) {
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_REVOKE | PACHA_FD_RIGHT_SHARE |
        PACHA_FD_RIGHT_CACHE_POLICY;
    const long fd = call(PACHA_FD_SYSCALL_VMO_CREATE, VMO_BYTES, rights, 0, 0, 0, 0);
    if (fd < 16 || fd >= PACHA_FD_TABLE_LIMIT) { log_text(message_create); log_hex(fd); finish(1); }
    const long requested = 0x200000000L;
    for (unsigned alias = 0; alias < CACHE_POLICY_ALIAS_COUNT; ++alias) {
        /* Repeated mappings of one VMO exercise real PTE-alias withdrawal
         * without allocating unrelated physical pages or changing the ABI. */
        const long alias_address = requested + (long)alias * (2L * VMO_BYTES);
        const long mapping_bytes = alias == 0 ? VMO_BYTES : RANGE_BYTES;
        const long backing_offset = alias == 0 ? 0 : CACHE_OFFSET;
        const long mapped = call(PACHA_VM_SYSCALL_MMAP, fd, alias_address, mapping_bytes,
            PACHA_PROT_READ | PACHA_PROT_WRITE,
            PACHA_MMAP_SHARED | PACHA_MMAP_FIXED_NOREPLACE, backing_offset);
        if (mapped != alias_address) {
            log_text(message_map); log_hex(mapped); finish(1);
        }
    }
    /* The GPU sandbox has already populated its image and module pages in
     * the same 384 MiB VMO before Mesa asks for the GTT range. Preserve that
     * page-store shape; an otherwise empty VMO misses the live preflight. */
    volatile uint8_t *image_bytes = (volatile uint8_t *)(uintptr_t)requested;
    for (size_t offset = 0; offset < CACHE_OFFSET; offset += 4096)
        image_bytes[offset] = (uint8_t)(offset / 4096);
    volatile uint8_t *bytes = image_bytes + CACHE_OFFSET;
#if CACHE_POLICY_SCATTER_SPAN_PAGES > 0
    /* Keep each target PFN separated by unrelated allocations so the cache
     * writer must inspect many physical 2 MiB identity PDEs, as it can in a
     * long-lived sandbox. Release filler before transition: its PTEs are not
     * aliases of the target and must not explain the result. */
    const long filler_size = RANGE_BYTES * CACHE_POLICY_SCATTER_SPAN_PAGES;
    const long filler_fd = call(PACHA_FD_SYSCALL_VMO_CREATE, filler_size,
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE | PACHA_FD_RIGHT_CLOSE,
        0, 0, 0, 0);
    if (filler_fd < 16 || filler_fd >= PACHA_FD_TABLE_LIMIT) {
        log_text(message_scatter); log_hex(filler_fd); finish(1);
    }
    const long filler_address = requested + 64L * (2L * VMO_BYTES);
    const long filler_mapping = call(PACHA_VM_SYSCALL_MMAP, filler_fd,
        filler_address, filler_size, PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED | PACHA_MMAP_FIXED_NOREPLACE, 0);
    if (filler_mapping != filler_address) {
        log_text(message_scatter); log_hex(filler_mapping); finish(1);
    }
    volatile uint8_t *filler = (volatile uint8_t *)(uintptr_t)filler_address;
#endif
    for (size_t offset = 0; offset < RANGE_BYTES; offset += 4096) {
        bytes[offset] = (uint8_t)(offset / 4096);
#if CACHE_POLICY_SCATTER_SPAN_PAGES > 0
        for (size_t index = 0; index < CACHE_POLICY_SCATTER_SPAN_PAGES; ++index)
            filler[(offset / 4096 * CACHE_POLICY_SCATTER_SPAN_PAGES + index) * 4096] = (uint8_t)index;
#endif
    }
#if CACHE_POLICY_SCATTER_SPAN_PAGES > 0
    if (call(PACHA_VM_SYSCALL_MUNMAP, filler_address, filler_size, 0, 0, 0, 0) != 0 ||
        call(PACHA_FD_SYSCALL_CLOSE, filler_fd, 0, 0, 0, 0, 0) != 0) {
        log_text(message_scatter); finish(1);
    }
#endif
    long children[CACHE_POLICY_CLONE_COUNT ? CACHE_POLICY_CLONE_COUNT : 1];
    for (unsigned i = 0; i < CACHE_POLICY_CLONE_COUNT; ++i) {
        const long child = call(PACHA_PROCESS_SYSCALL_CLONE,
            PACHA_FD_RIGHT_KILL | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_CLOSE,
            PACHA_PROCESS_CLONE_CURRENT_THREAD, 0, 0, 0, 0);
        if (child == 0) {
            /* A parked process retains its own CR3 and inherited shared
             * mappings, matching the all-process TLB rendezvous workload. */
            const uint64_t delay[2] = {0, 1000000};
            volatile uint8_t sample = 0;
            for (;;) {
                if (CACHE_POLICY_ACTIVE_READERS) {
                    /* Exercise a target PTE while another CPU withdraws and
                     * republishes its cache type; the reader must resume. */
                    sample ^= bytes[(i * 4096) % RANGE_BYTES];
                }
                (void)sample;
                (void)call(PACHA_RUNTIME_SYSCALL_NANOSLEEP,
                    (long)(uintptr_t)delay, 0, 0, 0, 0, 0);
            }
        }
        if (child < 16 || child >= PACHA_FD_TABLE_LIMIT) {
            log_text(message_clone); log_hex(child); finish(1);
        }
        children[i] = child;
    }
    log_text(message_begin);
    const long uc = call(PACHA_FD_SYSCALL_VMO_CACHE_POLICY, fd, CACHE_OFFSET, RANGE_BYTES, PACHA_VMO_CACHE_UC_MINUS, 0, 0);
    if (uc != 0) { log_text(message_uc_failure); log_hex(uc); finish(1); }
    log_text(message_uc_done);
    const long wb = call(PACHA_FD_SYSCALL_VMO_CACHE_POLICY, fd, CACHE_OFFSET, RANGE_BYTES, PACHA_VMO_CACHE_WB, 0, 0);
    if (wb != 0) { log_text(message_wb_failure); log_hex(wb); finish(1); }
    log_text(message_wb_done);
    /* Match the 512-page WC GTT view that Mesa asks Kobox2 to export.  This
     * exercises the native syscall's user-list copy and FD rights, which the
     * state-only page-view test cannot cover.  The view borrows real pages
     * from a populated parent, rather than allocating a second WB alias. */
    const long wc = call(PACHA_FD_SYSCALL_VMO_CACHE_POLICY, fd, CACHE_OFFSET,
        RANGE_BYTES, PACHA_VMO_CACHE_WC, 0, 0);
    if (wc != 0) { log_text(message_wc_failure); log_hex(wc); finish(1); }
    for (size_t index = 0; index < RANGE_BYTES / 4096; ++index)
        page_view_indices[index] = CACHE_OFFSET / 4096 + index;
    const uint64_t view_rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_REVOKE | PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const long view_fd = call(PACHA_FD_SYSCALL_VMO_CREATE_PAGE_VIEW, fd,
        (long)(uintptr_t)page_view_indices, RANGE_BYTES / 4096, view_rights,
        PACHA_FD_FLAG_CLOEXEC, 0);
    if (view_fd < 16 || view_fd >= PACHA_FD_TABLE_LIMIT) {
        log_text(message_view_failure); log_hex(view_fd); finish(1);
    }
    const long view_address = 0x10000000000L;
    const long view_mapping = call(PACHA_VM_SYSCALL_MMAP, view_fd, view_address,
        RANGE_BYTES, PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED | PACHA_MMAP_FIXED_NOREPLACE, 0);
    if (view_mapping != view_address) {
        log_text(message_view_map_failure); log_hex(view_mapping); finish(1);
    }
    volatile const uint8_t *view_bytes = (volatile const uint8_t *)(uintptr_t)view_address;
    if (view_bytes[0] != 0 || view_bytes[RANGE_BYTES - 4096] != 255) {
        log_text(message_view_content_failure); finish(1);
    }
    if (call(PACHA_VM_SYSCALL_MUNMAP, view_address, RANGE_BYTES, 0, 0, 0, 0) != 0 ||
        call(PACHA_FD_SYSCALL_CLOSE, view_fd, 0, 0, 0, 0, 0) != 0) {
        log_text(message_release); finish(1);
    }
    const long view_wb = call(PACHA_FD_SYSCALL_VMO_CACHE_POLICY, fd,
        CACHE_OFFSET, RANGE_BYTES, PACHA_VMO_CACHE_WB, 0, 0);
    if (view_wb != 0) { log_text(message_wb_failure); log_hex(view_wb); finish(1); }
    log_text(message_view_done);
    for (unsigned alias = 0; alias < CACHE_POLICY_ALIAS_COUNT; ++alias) {
        const long alias_address = requested + (long)alias * (2L * VMO_BYTES);
        const long mapping_bytes = alias == 0 ? VMO_BYTES : RANGE_BYTES;
        if (call(PACHA_VM_SYSCALL_MUNMAP, alias_address, mapping_bytes, 0, 0, 0, 0) != 0) {
            log_text(message_release); finish(1);
        }
    }
    if (call(PACHA_FD_SYSCALL_CLOSE, fd, 0, 0, 0, 0, 0) != 0) {
        log_text(message_release); finish(1);
    }
    for (unsigned i = 0; i < CACHE_POLICY_CLONE_COUNT; ++i) {
        if (call(PACHA_PROCESS_SYSCALL_KILL, children[i], 0, 0, 0, 0, 0) != 0 ||
            call(PACHA_FD_SYSCALL_CLOSE, children[i], 0, 0, 0, 0, 0) != 0) {
            log_text(message_release); finish(1);
        }
    }
    log_text(message_pass);
    finish(0);
}
