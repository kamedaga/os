#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static _Atomic uint32_t words[2];
static long update(_Atomic uint32_t *second, uint32_t encoded)
{
    return syscall(SYS_futex, &words[0], FUTEX_WAKE_OP | FUTEX_PRIVATE_FLAG,
        1, 1, second, encoded);
}
static void *increment(void *native)
{
    for (unsigned i = 0; i < 2000; ++i) {
        if (native) atomic_fetch_add(&words[1], 1);
        else assert(update(&words[1], FUTEX_OP(FUTEX_OP_ADD, 1, FUTEX_OP_CMP_EQ, 0)) == 0);
    }
    return NULL;
}
static void *wait_word(void *index)
{
    unsigned i = (unsigned)(uintptr_t)index;
    struct timespec timeout = {.tv_sec = 2};
    long result = syscall(SYS_futex, &words[i], FUTEX_WAIT_PRIVATE, 5, &timeout);
    assert(result == 0 || (result == -1 && errno == EAGAIN));
    return NULL;
}
static _Atomic uint32_t *changing;
static void *protect_word(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 1000; ++i) {
        assert(!mprotect(changing, 4096, PROT_READ));
        assert(!mprotect(changing, 4096, PROT_READ | PROT_WRITE));
    }
    return NULL;
}
int main(void)
{
    const uint32_t expected[] = {3, 1, 0xffffffffu, 0xfffffffcu, 0xfffffffdu};
    for (unsigned op = 0; op < 5; ++op) {
        atomic_store(&words[1], 0xfffffffeu);
        assert(update(&words[1], FUTEX_OP(op, 3, FUTEX_OP_CMP_NE, 0)) == 0);
        assert(atomic_load(&words[1]) == expected[op]);
    }
    assert(update(&words[1], FUTEX_OP((uint32_t)(FUTEX_OP_SET | FUTEX_OP_OPARG_SHIFT), 31, FUTEX_OP_CMP_EQ, 0)) == 0);
    assert(atomic_load(&words[1]) == 0x80000000u);
    assert(update(&words[1], FUTEX_OP(FUTEX_OP_ADD, -1, FUTEX_OP_CMP_LT, -1)) == 0);
    assert(atomic_load(&words[1]) == 0x7fffffffu);
    errno = 0;
    assert(update(&words[1], FUTEX_OP(FUTEX_OP_SET, 7, 15, 0)) == -1 && errno == ENOSYS);
    assert(atomic_load(&words[1]) == 7);
    atomic_store(&words[1], 0);
    pthread_t increments[4];
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&increments[i], NULL, increment, (void *)(uintptr_t)(i & 1)));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(increments[i], NULL));
    assert(atomic_load(&words[1]) == 8000);
    unsigned woke = 0;
    for (unsigned round = 0; round < 20; ++round) {
        pthread_t first, second;
        atomic_store(&words[0], 5); atomic_store(&words[1], 5);
        assert(!pthread_create(&first, NULL, wait_word, (void *)0));
        assert(!pthread_create(&second, NULL, wait_word, (void *)1));
        usleep(10000);
        // A late waiter observes the changed value instead of missing its wake.
        atomic_store(&words[0], 6);
        long count = update(&words[1], FUTEX_OP(FUTEX_OP_SET, 6, FUTEX_OP_CMP_EQ, 5));
        assert(count >= 0 && count <= 2); woke += (unsigned)count;
        assert(!pthread_join(first, NULL) && !pthread_join(second, NULL));
    }
    assert(woke > 0);
    changing = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(changing != MAP_FAILED); atomic_store(changing, 7);
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        assert(update(changing, FUTEX_OP(FUTEX_OP_SET, 9, FUTEX_OP_CMP_EQ, 7)) == 0);
        _exit(atomic_load(changing) == 9 ? 0 : 1);
    }
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(atomic_load(changing) == 7);
    assert(!mprotect(changing, 4096, PROT_READ)); errno = 0;
    assert(update(changing, FUTEX_OP(FUTEX_OP_SET, 9, FUTEX_OP_CMP_EQ, 7)) == -1 && errno == EFAULT);
    assert(atomic_load(changing) == 7);
    assert(!mprotect(changing, 4096, PROT_READ | PROT_WRITE));
    pthread_t protector; assert(!pthread_create(&protector, NULL, protect_word, NULL));
    for (unsigned i = 0; i < 2000; ++i) {
        long result = update(changing, FUTEX_OP(FUTEX_OP_OR, 0, FUTEX_OP_CMP_EQ, 0));
        assert(result == 0 || (result == -1 && errno == EFAULT));
    }
    assert(!pthread_join(protector, NULL));
    assert(!munmap(changing, 4096)); errno = 0;
    assert(update(changing, FUTEX_OP(FUTEX_OP_SET, 9, FUTEX_OP_CMP_EQ, 0)) == -1 && errno == EFAULT);
    printf("FUTEX_WAKE_OP=PASS atomic-user-race=8000 two-key-wakes=%u COW readonly mprotect-race unmapped\n", woke);
}
