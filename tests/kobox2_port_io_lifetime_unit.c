/* SPDX-License-Identifier: MIT */
/* Exercise the real callback admission/retirement code with a blocking host
 * byte operation. This is a lifetime oracle, not hardware PIO evidence. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>
#include "../userland/kobox2_adapter/exception.c"

static atomic_bool entered, release_operation, contended, stopped;
static unsigned operations;
void ph_lock(atomic_uint *lock) {
    while (atomic_exchange_explicit(lock, 1, memory_order_acquire)) {
        atomic_store(&contended, true);
        sched_yield();
    }
}
void ph_unlock(atomic_uint *lock) { atomic_store_explicit(lock, 0, memory_order_release); }
_Noreturn void ph_fail(const char *file, unsigned line, uint64_t result) {
    fprintf(stderr, "%s:%u: %llu\n", file, line, (unsigned long long)result);
    abort();
}
static long byte_operation(void *context, uint16_t port, bool writing, uint8_t value) {
    assert(context == &operations && port == 0x3cc && !writing && value == 0);
    ++operations;
    atomic_store(&entered, true);
    while (!atomic_load(&release_operation)) sched_yield();
    return 0x55;
}
static void *fault(void *context) {
    assert(port_io_fault(context));
    return NULL;
}
static void *stop(void *context) {
    (void)context;
    ph_port_io_stop();
    atomic_store(&stopped, true);
    return NULL;
}
int main(void) {
    const uint8_t instruction = 0xec;
    struct ph_fault_record record = {.vector = 13,
        .frame = {.rip = (uintptr_t)&instruction, .rdx = 0x3cc}};
    ph_port_io_start(byte_operation, &operations);
    pthread_t first, second;
    assert(!pthread_create(&first, NULL, fault, &record));
    while (!atomic_load(&entered)) sched_yield();
    assert(!pthread_create(&second, NULL, stop, NULL));
    while (!atomic_load(&contended)) sched_yield();
    assert(!atomic_load(&stopped));
    atomic_store(&release_operation, true);
    assert(!pthread_join(first, NULL));
    assert(!pthread_join(second, NULL));
    assert(operations == 1 && atomic_load(&stopped));
    assert(record.frame.rax == 0x55 && record.frame.rip == (uintptr_t)&instruction + 1);
    record.frame.rip = (uintptr_t)&instruction;
    assert(!port_io_fault(&record) && operations == 1);
    puts("port IO callback retirement: waits for in-flight access; rejects after stop PASS");
}
