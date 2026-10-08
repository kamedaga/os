/* SPDX-License-Identifier: MIT */
/* Real native frames/TLS and production fiber/exception primitives. The tiny
 * test scheduler forces migration; it does not stand in for Linux SMP gates. */
#include "../userland/kobox2_adapter/host.h"
#include "../kobox2/linux-sandbox/kobox/arch/x86_64/fp_entry.h"

struct ph_image ph_core;
static struct ph_task boot_thread, *threads[2];
static struct ph_fiber hubs[2], *guest;
static atomic_uint go, ready, slots[2], done;
static unsigned migrations, notifications, exceptions, notify_depth, fault_depth;
static void *fault_page;
enum { ROUNDS = 32, FIXUP_RESULT = 0x1234567 };
struct fp_bank { unsigned char bytes[512]; } __attribute__((aligned(16)));
static struct fp_bank seed, nested_seed, clobber;
extern int notification_fp_invoke(int (*)(uint64_t), uint64_t,
    const struct fp_bank *, struct fp_bank *, struct fp_bank *);

static void make_bank(struct fp_bank *bank, unsigned salt) {
    memset(bank, 0, sizeof(*bank));
    bank->bytes[0] = 0x7f; bank->bytes[1] = 0x0b;
    bank->bytes[2] = 0x25; bank->bytes[4] = 0xff;
    bank->bytes[6] = salt; bank->bytes[7] = 1;
    bank->bytes[8] = salt; bank->bytes[9] = 0x56; bank->bytes[10] = 0x12;
    bank->bytes[16] = salt; bank->bytes[17] = 0x67; bank->bytes[18] = 0x23;
    bank->bytes[24] = 0xa5; bank->bytes[25] = 0x7f;
    for (unsigned i = 0; i < 8; ++i) {
        bank->bytes[32 + i * 16] = salt + i;
        bank->bytes[39 + i * 16] = 0x80;
        bank->bytes[40 + i * 16] = 0xff; bank->bytes[41 + i * 16] = 0x3f;
    }
    for (unsigned i = 160; i < 416; ++i) bank->bytes[i] = salt + i;
}

static void same_range(const struct fp_bank *a, const struct fp_bank *b,
    unsigned first, unsigned last) {
    for (unsigned i = first; i < last; ++i) PH_CHECK(a->bytes[i] == b->bytes[i]);
}

static void same_bank(const struct fp_bank *a, const struct fp_bank *b) {
    /* Reserved FXSAVE bytes are unspecified; every architectural payload and
     * control field, including all x87 and full-width XMM registers, counts. */
    same_range(a, b, 0, 5); same_range(a, b, 6, 28);
    for (unsigned i = 0; i < 8; ++i) same_range(a, b, 32 + i * 16, 42 + i * 16);
    same_range(a, b, 160, 416);
}

static int checked(int (*call)(uint64_t), const struct fp_bank *bank) {
    struct fp_bank observed, installed;
    int status = notification_fp_invoke(call, 0, bank, &observed, &installed);
    same_bank(bank, &installed);
    same_bank(bank, &observed);
    return status;
}

static void await(atomic_uint *word) {
    while (!atomic_load_explicit(word, memory_order_acquire)) ph_wait(word, 0);
}

static void publish(atomic_uint *word) {
    atomic_store_explicit(word, 1, memory_order_release);
    ph_wake(word);
}

static void migrate(void) {
    uint64_t mask;
    PH_OK(ph_notifications_save(&mask));
    unsigned cpu = (unsigned)ph_current_cpu();
    struct ph_task *before = ph_current_task();
    PH_CHECK(cpu < 2 && before == threads[cpu] && ph_current_fiber() == guest);
    guest->logical_cpu = -1;
    guest->state = PH_FIBER_SWITCHED;
    ph_set_current_fiber(&hubs[cpu]);
    ph_fiber_switch(&guest->rsp, hubs[cpu].rsp);
    /* No cached native task pointer may be used after this actual transfer. */
    PH_CHECK(ph_current_cpu() == (int)(1 - cpu) && ph_current_task() != before);
    PH_CHECK(ph_current_task() == threads[1 - cpu] && ph_current_fiber() == guest);
    ++migrations;
    PH_OK(ph_notifications_restore(mask));
}

static KOBOX_GPR_LEAF int signal_self(uint64_t unused) {
    (void)unused;
    return (int)pacha_syscall2(PACHA_THREAD_SYSCALL_SIGNAL,
        ph_current_task()->fd, PH_NOTIFICATION_SIGNAL);
}

void ph_dispatch_notifications(void) {
    PH_CHECK(ph_current_fiber() == guest && notify_depth < 2);
    struct ph_task *before = ph_current_task();
    ++notifications;
    ++notify_depth;
    migrate();
    if (notify_depth == 1) {
        /* A second real native frame remains suspended while its handler
         * migrates. Both nested and outer returns must change native thread. */
        PH_OK(checked(signal_self, &nested_seed));
        migrate();
    }
    PH_CHECK(ph_current_task() != before);
    --notify_depth;
    __asm__ volatile("fxrstorq %0" : : "m"(clobber) : "memory");
}

static KOBOX_GPR_LEAF int deferred_signal(uint64_t unused) {
    (void)unused;
    uint64_t mask;
    unsigned count = notifications;
    PH_OK(ph_notifications_save(&mask));
    PH_OK(signal_self(0));
    PH_CHECK(notifications == count && PH_TLS_LOAD(unsigned, notification_deferred));
    PH_OK(ph_notifications_restore(mask));
    PH_CHECK(notifications == count + 2);
    return 0;
}

static KOBOX_GPR_LEAF int page_probe(uint64_t unused) {
    (void)unused;
    unsigned long result;
    __asm__ volatile("lea 1f(%%rip), %%r11; mov (%1), %%rax; 1:"
        : "=a"(result) : "r"(fault_page) : "r11", "cc", "memory");
    return (int)result;
}

static KOBOX_GPR_LEAF int opcode_probe(uint64_t unused) {
    (void)unused;
    unsigned long result = 0;
    __asm__ volatile("lea 1f(%%rip), %%r11; ud2; 1:"
        : "+a"(result) : : "r11", "cc", "memory");
    return (int)result;
}

static enum kobox_linux_exception_result recover(struct kobox_linux_exception_frame *frame) {
    char marker;
    struct ph_task *before = ph_current_task();
    unsigned cpu = (unsigned)ph_current_cpu();
    PH_CHECK(frame->size == sizeof(*frame) && frame->cpu == cpu && cpu < 2);
    PH_CHECK(frame->vector == 6 || (frame->vector == 14 &&
        frame->fault_address == (uintptr_t)fault_page));
    PH_CHECK((uintptr_t)&marker - (uintptr_t)guest->stack < guest->stack_size);
    PH_CHECK((uintptr_t)&marker - (uintptr_t)before->fault_stack >= PH_THREAD_STACK_SIZE);
    PH_CHECK(PH_TLS_LOAD(uint64_t, notification_mask) == PH_NOTIFICATION_MASK);
    PH_CHECK(fault_depth < 2);
    ++exceptions;
    ++fault_depth;
    migrate();
    if (fault_depth == 1) {
        /* The original native fault must already be completed: an ordinary
         * nested upcall and a second recoverable fault are now legal. */
        PH_OK(ph_notifications_restore(0));
        PH_OK(checked(signal_self, &nested_seed));
        PH_OK(ph_notifications_restore(PH_NOTIFICATION_MASK));
        PH_CHECK(checked(opcode_probe, &nested_seed) == FIXUP_RESULT);
    }
    PH_CHECK(ph_current_task() != before);
    --fault_depth;
    frame->ip = frame->registers[KOBOX_EXCEPTION_R11];
    frame->registers[KOBOX_EXCEPTION_AX] = FIXUP_RESULT;
    __asm__ volatile("fxrstorq %0" : : "m"(clobber) : "memory");
    return KOBOX_EXCEPTION_RESUME;
}

static void *exercise(void *unused) {
    (void)unused;
    PH_OK(ph_notifications_restore(0));
    for (unsigned round = 0; round < ROUNDS; ++round) {
        PH_OK(checked(signal_self, &seed));
        PH_OK(checked(deferred_signal, &seed));
        PH_CHECK(checked(page_probe, &seed) == FIXUP_RESULT);
        PH_CHECK(checked(opcode_probe, &seed) == FIXUP_RESULT);
        PH_CHECK(!notify_depth && !fault_depth && !PH_TLS_LOAD(uint64_t, notification_mask));
    }
    uint64_t mask;
    PH_OK(ph_notifications_save(&mask));
    unsigned cpu = (unsigned)ph_current_cpu();
    guest->logical_cpu = -1;
    guest->state = PH_FIBER_DEAD;
    ph_set_current_fiber(&hubs[cpu]);
    uint64_t discarded;
    ph_fiber_switch(&discarded, hubs[cpu].rsp);
    ph_fail(__FILE__, __LINE__, 0);
}

static void *worker(void *argument) {
    unsigned cpu = (unsigned)(uintptr_t)argument;
    struct ph_task *thread = ph_current_task();
    hubs[cpu].bound = true;
    hubs[cpu].state = PH_FIBER_RUNNING;
    hubs[cpu].logical_cpu = (int)cpu;
    ph_set_current_fiber(&hubs[cpu]);
    await(&go);
    PH_CHECK(thread == threads[cpu]);
    atomic_fetch_add_explicit(&ready, 1, memory_order_release);
    ph_wake(&ready);
    for (;;) {
        await(&slots[cpu]);
        atomic_store_explicit(&slots[cpu], 0, memory_order_relaxed);
        if (atomic_load_explicit(&done, memory_order_acquire)) break;
        PH_CHECK(guest->state == PH_FIBER_NEW || guest->state == PH_FIBER_SWITCHED);
        guest->state = PH_FIBER_RUNNING;
        guest->logical_cpu = (int)cpu;
        ph_set_current_fiber(guest);
        ph_fiber_switch(&hubs[cpu].rsp, guest->rsp);
        PH_CHECK(ph_current_fiber() == &hubs[cpu] &&
            PH_TLS_LOAD(uint64_t, notification_mask) == PH_NOTIFICATION_MASK);
        if (guest->state == PH_FIBER_DEAD) {
            publish(&done);
            publish(&slots[1 - cpu]);
            break;
        }
        /* Publish only after switch has saved RSP: the destination must never
         * resume the fiber while the source still executes on its stack. */
        publish(&slots[1 - cpu]);
    }
    ph_set_current_fiber(NULL);
    return NULL;
}

static void *origin_entry(void *argument) {
    PH_CHECK(argument == &boot_thread && ph_current_fiber()->origin == ph_current_task());
    return NULL;
}

void ph_main(void) {
    boot_thread.self = &boot_thread;
    boot_thread.bound = true;
    PH_OK(pacha_syscall1(PACHA_THREAD_SYSCALL_SET_FS_BASE, (uintptr_t)&boot_thread));
    boot_thread.fault_stack = ph_alloc(PH_THREAD_STACK_SIZE);
    ph_thread_setup(&boot_thread);
    PH_OK(ph_exceptions_install(recover));
    make_bank(&seed, 0x31); make_bank(&nested_seed, 0x61); make_bank(&clobber, 0x91);
    fault_page = ph_alloc(PH_PAGE_SIZE);
    PH_OK(pacha_syscall3(PACHA_VM_SYSCALL_MPROTECT, (uintptr_t)fault_page, PH_PAGE_SIZE, 0));
    guest = ph_fiber_create(exercise, NULL);
    for (unsigned i = 0; i < 2; ++i) threads[i] = ph_thread_create(worker, (void *)(uintptr_t)i);
    publish(&go);
    for (;;) {
        unsigned count = atomic_load_explicit(&ready, memory_order_acquire);
        if (count == 2) break;
        ph_wait(&ready, count);
    }
    PH_CHECK(threads[0] != threads[1] && threads[0]->fd != threads[1]->fd);
    publish(&slots[0]);
    await(&done);
    for (unsigned i = 0; i < 2; ++i) PH_OK(ph_thread_join(threads[i]));
    PH_CHECK(migrations == ROUNDS * 16 && notifications == ROUNDS * 8 && exceptions == ROUNDS * 4);
    PH_CHECK(!fault_depth && !notify_depth && guest->state == PH_FIBER_DEAD);
    ph_fiber_destroy(guest);
    ph_free(fault_page, PH_PAGE_SIZE);
    /* Failed Linux task creation can start an origin thread that immediately
     * returns. Exercise its real exit/WAIT/FD-close before releasing its stack. */
    struct ph_fiber *origin = ph_fiber_create(origin_entry, &boot_thread);
    struct ph_task *origin_thread = ph_fiber_thread_start(origin);
    struct pacha_pollfd completion = {.fd = origin_thread->fd, .events = PACHA_FD_EVENT_READABLE};
    PH_CHECK(ph_wait_readable(&completion, 1) == 1 &&
        completion.revents == PACHA_FD_EVENT_READABLE);
    PH_CHECK(origin->state == PH_FIBER_DEAD);
    ph_fiber_destroy(origin);
    ph_number("FIBER_NATIVE_MIGRATIONS", migrations);
    ph_number("FIBER_NATIVE_NOTIFICATIONS", notifications);
    ph_number("FIBER_NATIVE_EXCEPTIONS", exceptions);
    ph_log("FIBER_NATIVE_ORIGIN_JOIN=PASS\n");
    ph_log("KOBOX2_FIBER_NATIVE=PASS\n");
    PH_OK(pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT, 0));
    ph_fail(__FILE__, __LINE__, 0);
}
