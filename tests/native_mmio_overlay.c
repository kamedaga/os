/* SPDX-License-Identifier: MIT */
/* Reuse the dedicated virtio-rng discovery and real page-fault probes. This
 * fixture runs as boot init, not through the rootfs/service launch path. */
#define main native_device_contract_main
#include "native_device_contract.c"
#undef main
#include "../userland/seed0boot/src/bootstrap_abi.h"
#include "../userland/kobox2_adapter/device_pci.h"
#include <pacha/syscall.h>

#define ARENA UINT64_C(0x10000000000)
#define MAP_FIXED UINT64_C(1)
#define MAP_NOREPLACE UINT64_C(2)
#define MAP_NORESERVE (UINT64_C(1) << 5)
#define RESERVATION_FLAGS (PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS | MAP_NORESERVE)

static unsigned char racer_stack[65536] __attribute__((aligned(PAGE)));
static uint64_t racer_ready, racer_stop, racer_attempts, racer_failure;
static uint64_t pressure_fds[PACHA_FD_TABLE_LIMIT];

__attribute__((used, noreturn)) void overlay_racer_body(void) {
    __atomic_store_n(&racer_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&racer_stop, __ATOMIC_ACQUIRE)) {
        long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, 0, ARENA + PAGE,
            PAGE, 0, RESERVATION_FLAGS | MAP_NOREPLACE, 0);
        if ((uint64_t)address == ARENA + PAGE)
            __atomic_store_n(&racer_failure, 1, __ATOMIC_RELEASE);
        __atomic_fetch_add(&racer_attempts, 1, __ATOMIC_RELEASE);
    }
    (void)pacha_syscall3(PACHA_THREAD_SYSCALL_EXIT, 0, 0, 0);
    __builtin_trap();
}

extern void overlay_racer_entry(void);
__asm__(".text\n.global overlay_racer_entry\noverlay_racer_entry: "
    "and $-16,%rsp; call overlay_racer_body; ud2\n");

static uint64_t start_racer(void) {
    const uint64_t rights = PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE;
    const uint64_t fd = call(PACHA_THREAD_SYSCALL_CREATE, PACHA_PROCESS_SELF_FD,
        (uintptr_t)overlay_racer_entry, (uintptr_t)racer_stack + sizeof(racer_stack),
        0, 0, rights);
    CHECK(is_fd(fd));
    CHECK(call(PACHA_THREAD_SYSCALL_START, fd, 0, 0, 0, 0, 0) == 0);
    const uint64_t deadline = now() + UINT64_C(2000000000);
    while (!__atomic_load_n(&racer_attempts, __ATOMIC_ACQUIRE)) CHECK(now() < deadline);
    return fd;
}

static void stop_racer(uint64_t fd) {
    __atomic_store_n(&racer_stop, 1, __ATOMIC_RELEASE);
    struct pacha_pollfd done = {.fd = (int)fd, .events = PACHA_FD_EVENT_READABLE};
    CHECK(call(PACHA_FD_SYSCALL_WAIT_MANY, (uintptr_t)&done, 1,
        UINT64_C(2000000000), 0, 0, 0) == 1);
    uint64_t state[4];
    CHECK(call(PACHA_THREAD_SYSCALL_WAIT, fd, (uintptr_t)state, 0, 0, 0, 0) == 0);
    CHECK(state[0] == 2 && state[1] == 0);
    close_fd(fd);
    CHECK(!__atomic_load_n(&racer_failure, __ATOMIC_ACQUIRE));
    log_text("MMIO_RESERVATION_RACE_ATTEMPTS="); log_number(racer_attempts);
}

void *memcpy(void *destination, const void *source, size_t length) {
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < length; ++i) out[i] = in[i];
    return destination;
}

static void acquire_device(void) {
    CHECK(seed0_bootstrap_descriptor() != NULL);
    const uint64_t source = claim_pci_device(0x1af4, 0x1044);
    CHECK(call(PACHA_FD_SYSCALL_DUP, source, DEVICE_FD,
        query(source).rights, 0, 0, 0) == DEVICE_FD);
    close_fd(source);
}

static void reserve_arena(void) {
    CHECK(call(PACHA_VM_SYSCALL_MMAP, 0, ARENA, 3 * PAGE, 0,
        RESERVATION_FLAGS | MAP_FIXED, 0) == ARENA);
    /* Force actual VMA splits while retaining NONE and untouched backing. */
    CHECK(call(PACHA_VM_SYSCALL_MPROTECT, ARENA + PAGE, PAGE, 0, 0, 0, 0) == 0);
}

static void reservation_survived(void) {
    for (unsigned page = 0; page < 3; ++page) {
        const uint64_t address = ARENA + page * PAGE;
        expect_fault(address, 0);
        CHECK(call(PACHA_VM_SYSCALL_MMAP, 0, address, PAGE, 0,
            RESERVATION_FLAGS | MAP_NOREPLACE, 0) != address);
    }
}

int main(void) {
    stage = "overlay-bootstrap";
    acquire_device();
    CHECK(call(PACHA_PROCESS_SYSCALL_SIGNAL_CTL, PACHA_PROCESS_SIGNAL_CTL_REGISTER_FAULT,
        (uintptr_t)fault_entry, (uintptr_t)fault_stack, sizeof(fault_stack), 0, 0) == 0);
    discover();
    reserve_arena();

    struct ph_pci port = {0};
    struct ph_pci_mapping leases[4];
    const struct ph_pci_config settings = {
        .device_fd = DEVICE_FD, .generation = 1,
        .reservation = (void *)ARENA, .reservation_size = 3 * PAGE,
        .mappings = leases, .mapping_capacity = 4,
    };
    CHECK(ph_pci_init(&port, &settings) == 0);
    const struct pacha_capsule_bar_info bar = bar_info(common_cap.bar);
    const uint64_t physical = (bar.start + common_cap.offset) & ~(uint64_t)(PAGE - 1);
    const uint64_t register_offset = (bar.start + common_cap.offset) & (PAGE - 1);
    const uintptr_t alias = ARENA + PAGE + register_offset;
    const struct kobox_linux_pci_host *host = &port.host;

    stage = "ordinary-uc-minus-alias";
    const uintptr_t ordinary_address = MAP_BASE + 0x100000;
    const uint64_t ordinary = map_bar(common_cap.bar,
        physical - (bar.start & ~(uint64_t)(PAGE - 1)), ordinary_address,
        PACHA_CAPSULE_MMIO_CACHE_UC_MINUS | PACHA_CAPSULE_MMIO_READ_ONLY);
    CHECK(is_fd(ordinary));
    CHECK(query(ordinary).flags == (PACHA_CAPSULE_MMIO_CACHE_UC_MINUS | PACHA_CAPSULE_MMIO_READ_ONLY));
    CHECK(r8(ordinary_address + register_offset + 20) == r8(common + 20));
    expect_fault(ordinary_address + register_offset + 20, 1);
    close_fd(ordinary);
    expect_fault(ordinary_address + register_offset + 20, 0);

    stage = "overlay-fd-exhaustion";
    uint64_t before[3], after[3];
    CHECK(call(PACHA_FD_SYSCALL_TABLE, 0, (uintptr_t)before, 0, 0, 0, 0) == 0);
    size_t held = 0;
    for (; held < PACHA_FD_TABLE_LIMIT; ++held) {
        uint64_t fd = call(PACHA_FD_SYSCALL_DUP, DEVICE_FD, 16,
            query(DEVICE_FD).rights, 0, 0, 0);
        if (!is_fd(fd)) break;
        pressure_fds[held] = fd;
    }
    CHECK(held && held < PACHA_FD_TABLE_LIMIT);
    CHECK(call(PACHA_FD_SYSCALL_TABLE, 0, (uintptr_t)after, 0, 0, 0, 0) == 0);
    CHECK(after[2] == 0);
    for (unsigned round = 0; round < 8; ++round) {
        CHECK(host->memory_map(&port, (void *)(ARENA + PAGE), physical, PAGE,
            KOBOX_LINUX_MEMORY_READ, KOBOX_MMIO_UC) < 0);
        reservation_survived();
    }
    while (held) close_fd(pressure_fds[--held]);
    CHECK(call(PACHA_FD_SYSCALL_TABLE, 0, (uintptr_t)after, 0, 0, 0, 0) == 0);
    CHECK(after[2] == before[2]);
    const uint64_t racer = start_racer();

    for (unsigned round = 0; round < 32; ++round) {
        stage = "overlay-map";
        const enum kobox_mmio_cache cache = round % 2 ? KOBOX_MMIO_UC_MINUS : KOBOX_MMIO_UC;
        CHECK(host->memory_map(&port, (void *)(ARENA + PAGE), physical, PAGE,
            KOBOX_LINUX_MEMORY_READ, cache) == 0);
        CHECK(r8(alias + 20) == r8(common + 20));
        expect_fault(alias + 20, 1);
        expect_fault(ARENA, 0);
        expect_fault(ARENA + 2 * PAGE, 0);
        stage = "overlay-pinned";
        CHECK(call(PACHA_VM_SYSCALL_MPROTECT, ARENA + PAGE, PAGE,
            PACHA_PROT_READ | PACHA_PROT_WRITE, 0, 0, 0) != 0);
        CHECK(call(PACHA_VM_SYSCALL_MUNMAP, ARENA + PAGE, PAGE, 0, 0, 0, 0) != 0);
        CHECK(call(PACHA_VM_SYSCALL_MMAP, 0, ARENA + PAGE, PAGE, 0,
            RESERVATION_FLAGS | MAP_FIXED, 0) != ARENA + PAGE);
        const uint64_t copy = call(PACHA_FD_SYSCALL_DUP, leases[0].fd, 16,
            query(leases[0].fd).rights, 0, 0, 0);
        CHECK(is_fd(copy));
        stage = "overlay-last-close";
        CHECK(host->memory_unmap(&port, (void *)(ARENA + PAGE), PAGE) == 0);
        CHECK(r8(alias + 20) == r8(common + 20)); /* dup retains the MMIO lease */
        close_fd(copy);
        reservation_survived();
        stage = "overlay-unsupported-cache";
        CHECK(host->memory_map(&port, (void *)(ARENA + PAGE), physical, PAGE,
            KOBOX_LINUX_MEMORY_READ, KOBOX_MMIO_WC) < 0);
        reservation_survived();
    }
    stage = "overlay-racer-join";
    stop_racer(racer);
    stage = "overlay-backed-none";
    CHECK(call(PACHA_VM_SYSCALL_MPROTECT, ARENA + PAGE, PAGE,
        PACHA_PROT_READ | PACHA_PROT_WRITE, 0, 0, 0) == 0);
    *(volatile uint64_t *)(ARENA + PAGE) = UINT64_C(0x1122334455667788);
    CHECK(call(PACHA_VM_SYSCALL_MPROTECT, ARENA + PAGE, PAGE, 0, 0, 0, 0) == 0);
    CHECK(host->memory_map(&port, (void *)(ARENA + PAGE), physical, PAGE,
        KOBOX_LINUX_MEMORY_READ, KOBOX_MMIO_UC) < 0);
    CHECK(call(PACHA_VM_SYSCALL_MPROTECT, ARENA + PAGE, PAGE, PACHA_PROT_READ, 0, 0, 0) == 0);
    CHECK(*(volatile uint64_t *)(ARENA + PAGE) == UINT64_C(0x1122334455667788));
    CHECK(ph_pci_revoke(&port, 1) == 0 && ph_pci_destroy(&port) == 0);
    CHECK(call(PACHA_VM_SYSCALL_MUNMAP, ARENA, 3 * PAGE, 0, 0, 0, 0) == 0);
    for (unsigned i = 0; i < map_count; ++i) close_fd(maps[i].fd);
    close_fd(DEVICE_FD);
    log_text("NATIVE_MMIO_OVERLAY=PASS\n");
    return 0;
}
