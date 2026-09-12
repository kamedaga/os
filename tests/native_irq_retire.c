/* SPDX-License-Identifier: MIT */
/* Real MSI-X retirement/alias lifetime on a boot-only virtio-rng device. */
#define main native_device_contract_main
#include "native_device_contract.c"
#undef main
#include "../userland/seed0boot/src/bootstrap_abi.h"
#include <pacha/syscall.h>

static unsigned char waiter_stack[65536] __attribute__((aligned(PAGE)));
static uint64_t waiter_ready, waiter_done, waiter_result, waiter_revents, waiter_irq;

static void acquire_device(void) {
    const struct seed0_init_descriptor_page *boot = seed0_bootstrap_descriptor();
    CHECK(boot && boot->device_count <= SEED0_INIT_MAX_DEVICE_DESCRIPTORS);
    uint64_t source = 0;
    for (uint64_t i = 0; i < boot->device_count; ++i) {
        if (boot->devices[i].vendor_id == 0x1af4 && boot->devices[i].device_id == 0x1044) {
            CHECK(!source);
            source = boot->devices[i].init_device_fd;
        }
    }
    CHECK(is_fd(source));
    CHECK(call(PACHA_FD_SYSCALL_DUP, source, DEVICE_FD, query(source).rights, 0, 0, 0) == DEVICE_FD);
}

__attribute__((used, noreturn)) void irq_waiter_body(void) {
    struct pacha_pollfd wait = {.fd = (int)waiter_irq, .events = PACHA_FD_EVENT_READABLE};
    __atomic_store_n(&waiter_ready, 1, __ATOMIC_RELEASE);
    waiter_result = pacha_syscall3(PACHA_FD_SYSCALL_WAIT_MANY, (uintptr_t)&wait, 1, UINT64_MAX);
    waiter_revents = wait.revents;
    __atomic_store_n(&waiter_done, 1, __ATOMIC_RELEASE);
    (void)pacha_syscall3(PACHA_THREAD_SYSCALL_EXIT, 0, 0, 0);
    __builtin_trap();
}

extern void irq_waiter_entry(void);
__asm__(".text\n.global irq_waiter_entry\nirq_waiter_entry: "
    "and $-16,%rsp; call irq_waiter_body; ud2\n");

static uint64_t start_waiter(uint64_t irq) {
    waiter_irq = irq;
    waiter_ready = waiter_done = 0;
    const uint64_t thread = call(PACHA_THREAD_SYSCALL_CREATE, PACHA_PROCESS_SELF_FD,
        (uintptr_t)irq_waiter_entry, (uintptr_t)waiter_stack + sizeof(waiter_stack), 0, 0,
        PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE);
    CHECK(is_fd(thread));
    CHECK(call(PACHA_THREAD_SYSCALL_START, thread, 0, 0, 0, 0, 0) == 0);
    const uint64_t until = now() + UINT64_C(2000000000);
    while (!__atomic_load_n(&waiter_ready, __ATOMIC_ACQUIRE)) CHECK(now() < until);
    const uint64_t delay[] = {0, 20000000};
    CHECK(call(PACHA_RUNTIME_SYSCALL_NANOSLEEP, (uintptr_t)delay, 0, 0, 0, 0, 0) == 0);
    CHECK(!__atomic_load_n(&waiter_done, __ATOMIC_ACQUIRE));
    return thread;
}

static void join_waiter(uint64_t thread) {
    const uint64_t until = now() + UINT64_C(2000000000);
    while (!__atomic_load_n(&waiter_done, __ATOMIC_ACQUIRE)) CHECK(now() < until);
    CHECK(waiter_result == 1 && waiter_revents == PACHA_FD_EVENT_HANGUP);
    struct pacha_pollfd done = {.fd = (int)thread, .events = PACHA_FD_EVENT_READABLE};
    CHECK(call(PACHA_FD_SYSCALL_WAIT_MANY, (uintptr_t)&done, 1,
        UINT64_C(2000000000), 0, 0, 0) == 1);
    uint64_t state[4];
    CHECK(call(PACHA_THREAD_SYSCALL_WAIT, thread, (uintptr_t)state, 0, 0, 0, 0) == 0);
    CHECK(state[0] == 2 && state[1] == 0);
    close_fd(thread);
}

static struct pacha_capsule_irq_route route_info(uint64_t irq) {
    struct pacha_capsule_irq_route route;
    CHECK(call(PACHA_CAPSULE_SYSCALL_IRQ_ROUTE, irq, (uintptr_t)&route, 3, 0, 0, 0) == 3);
    CHECK(route.message_data == vector_base && route.hwirq == vector_base);
    CHECK((route.message_address & ~UINT64_C(0xff000)) == UINT64_C(0xfee00000));
    return route;
}

static void program_route(struct pacha_capsule_irq_route route) {
    w32(table + 12, 1);
    w64(table, route.message_address);
    w32(table + 8, (uint32_t)route.message_data);
    set_config(msix_cap + 2, 2, (config(msix_cap + 2, 2) | 0x8000) & ~0x4000u);
}

static void expect_old_closed(uint64_t irq) {
    struct pacha_capsule_irq_route ignored;
    uint64_t count;
    CHECK(query(irq).flags & PACHA_CAPSULE_IRQ_RETIRED);
    CHECK(call(PACHA_CAPSULE_SYSCALL_IRQ_ROUTE, irq, (uintptr_t)&ignored, 3, 0, 0, 0) == PACHA_SYSCALL_ERR_CLOSED);
    CHECK(call(PACHA_CAPSULE_SYSCALL_IRQ_POLL, irq, UINT64_MAX, (uintptr_t)&count, 1, 0, 0) == PACHA_SYSCALL_ERR_CLOSED);
    CHECK(call(PACHA_CAPSULE_SYSCALL_IRQ_QUIESCE, irq, 0, 0, 0, 0, 0) == PACHA_SYSCALL_ERR_CLOSED);
    CHECK(call(PACHA_CAPSULE_SYSCALL_IRQ_RETIRE, irq, 0, 0, 0, 0, 0) == PACHA_SYSCALL_ERR_CLOSED);
    struct pacha_pollfd poll = {.fd = (int)irq, .events = 0};
    CHECK(call(PACHA_FD_SYSCALL_WAIT_MANY, (uintptr_t)&poll, 1, 0, 0, 0, 0) == 1);
    CHECK(poll.revents == PACHA_FD_EVENT_HANGUP);
}

int main(void) {
    stage = "irq-retire-bootstrap";
    acquire_device();
    discover();
    reset_device();
    memset(dma_bytes, 0, sizeof(dma_bytes));
    const uint64_t dma = call(PACHA_CAPSULE_SYSCALL_DERIVE_DMA_MAPPING, DEVICE_FD,
        (uintptr_t)dma_bytes, TEST_IOVA, DMA_SIZE, PACHA_CAPSULE_DMA_BIDIRECTIONAL, 0);
    CHECK(is_fd(dma));
    for (unsigned round = 0; round < 8; ++round) {
        stage = "irq-retire-allocate";
        w32(table + 12, 1);
        const uint64_t irq = derive_irq();
        CHECK(is_fd(irq));
        CHECK(call(PACHA_CAPSULE_SYSCALL_IRQ_QUIESCE, irq, 0, 0, 0, 0, 0) == 0);
        const struct pacha_capsule_irq_route route = route_info(irq);
        const uint64_t alias = call(PACHA_FD_SYSCALL_DUP, irq, 16, query(irq).rights, 0, 0, 0);
        CHECK(is_fd(alias));
        const uint64_t restricted = call(PACHA_FD_SYSCALL_DUP, irq, 16,
            query(irq).rights & ~PACHA_FD_RIGHT_IRQ_ACK, 0, 0, 0);
        CHECK(is_fd(restricted));
        CHECK(call(PACHA_CAPSULE_SYSCALL_IRQ_RETIRE, restricted, 0, 0, 0, 0, 0) == PACHA_SYSCALL_ERR_INVALID);
        close_fd(restricted);
        CHECK(call(PACHA_CAPSULE_SYSCALL_IRQ_ROUTE, irq, 1, 3, 0, 0, 0) == PACHA_SYSCALL_ERR_INVALID);
        program_route(route);
        memset(dma_bytes, 0, sizeof(dma_bytes));
        setup_queue();
        w32(table + 12, 0);
        request_rng(1);
        const uint64_t until = now() + UINT64_C(2000000000);
        while (!irq_count(irq)) CHECK(now() < until);
        stage = "irq-retire-pending-refusal";
        w32(table + 12, 1);
        request_rng(2);
        CHECK(r32(pba) & 1);
        CHECK(call(PACHA_CAPSULE_SYSCALL_IRQ_RETIRE, irq, 0, 0, 0, 0, 0) == PACHA_SYSCALL_ERR_NOT_READY);
        CHECK(!(query(irq).flags & PACHA_CAPSULE_IRQ_RETIRED));
        CHECK(derive_irq() == PACHA_SYSCALL_ERR_NOT_READY);
        reset_device();
        CHECK(call(PACHA_CAPSULE_SYSCALL_IRQ_QUIESCE, irq, 0, 0, 0, 0, 0) == 0);
        stage = "irq-retire-waiter";
        const uint64_t thread = start_waiter(alias);
        CHECK(call(PACHA_CAPSULE_SYSCALL_IRQ_RETIRE, irq, 0, 0, 0, 0, 0) == 0);
        join_waiter(thread);
        expect_old_closed(irq);
        expect_old_closed(alias);
        stage = "irq-retire-replacement";
        const uint64_t replacement = derive_irq();
        CHECK(is_fd(replacement));
        CHECK(call(PACHA_CAPSULE_SYSCALL_IRQ_QUIESCE, replacement, 0, 0, 0, 0, 0) == 0);
        program_route(route_info(replacement));
        memset(dma_bytes, 0, sizeof(dma_bytes));
        setup_queue();
        w32(table + 12, 0);
        expect_old_closed(alias);
        close_fd(irq);
        close_fd(alias); /* Must not mask or release the replacement. */
        CHECK(!(r32(table + 12) & 1));
        request_rng(1);
        const uint64_t resumed_until = now() + UINT64_C(2000000000);
        while (!irq_count(replacement)) CHECK(now() < resumed_until);
        reset_device();
        CHECK(call(PACHA_CAPSULE_SYSCALL_IRQ_RETIRE, replacement, 0, 0, 0, 0, 0) == 0);
        close_fd(replacement);
    }
    close_fd(dma);
    for (unsigned i = 0; i < map_count; ++i) close_fd(maps[i].fd);
    close_fd(DEVICE_FD);
    log_text("NATIVE_IRQ_RETIRE=PASS\n");
    return 0;
}
