#include <errno.h>
#include <stddef.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <sys/auxv.h>
#include <unistd.h>

#ifndef SYS_futex
#define SYS_futex 202
#endif
#ifndef SYS_clone
#define SYS_clone 56
#endif
#ifndef SYS_gettid
#define SYS_gettid 186
#endif
#ifndef SYS_exit_group
#define SYS_exit_group 231
#endif
#define CAP_FUTEX_WAIT 0
#define CAP_FUTEX_WAKE 1
#define CAP_FUTEX_PRIVATE_FLAG 128
#ifndef CLONE_VM
#define CLONE_VM 0x00000100
#endif
#ifndef CLONE_FS
#define CLONE_FS 0x00000200
#endif
#ifndef CLONE_FILES
#define CLONE_FILES 0x00000400
#endif
#ifndef CLONE_SIGHAND
#define CLONE_SIGHAND 0x00000800
#endif
#ifndef CLONE_THREAD
#define CLONE_THREAD 0x00010000
#endif

static volatile int pthread_value = 0;
static volatile pid_t main_pid_seen = 0;
static volatile pid_t main_tid_seen = 0;
static volatile pid_t child_pid_seen = 0;
static volatile pid_t child_tid_seen = 0;
static volatile int child_tls_seen = 0;
static volatile int four_pthread_ready_mask = 0;
static volatile int four_pthread_done_mask = 0;
static volatile int four_pthread_tls_sum = 0;
static volatile int four_pthread_futex_eagain_count = 0;
static volatile int four_pthread_start_gate = 0;
static volatile pid_t four_pthread_pid_seen[4] = { 0, 0, 0, 0 };
static volatile pid_t four_pthread_tid_seen[4] = { 0, 0, 0, 0 };
static volatile int four_pthread_futex_words[4] = { 1, 1, 1, 1 };
static volatile int exit_group_child_ready = 0;
static volatile int exit_group_wait_word = 0;
static volatile int exit_group_running_ready = 0;
static volatile int exit_group_running_spin = 1;
static __thread int tls_probe = 31;

static int write_all(const char *message) {
    size_t len = strlen(message);
    ssize_t written = write(1, message, len);
    return written == (ssize_t)len ? 0 : 1;
}

static void write_dec(long value) {
    char buf[32];
    char tmp[24];
    int pos = 0;
    int n = 0;
    if (value < 0) {
        buf[pos++] = '-';
        value = -value;
    }
    do {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && n < (int)sizeof(tmp));
    while (n > 0) buf[pos++] = tmp[--n];
    buf[pos++] = '\n';
    (void)write(1, buf, (size_t)pos);
}

static int futex_smoke(void) {
    int word = 1;
    errno = 0;
    long ret = syscall(SYS_futex, &word, CAP_FUTEX_WAIT | CAP_FUTEX_PRIVATE_FLAG, 0, NULL, NULL, 0);
    if (ret != -1 || errno != EAGAIN) {
        (void)write_all("musl_smoke: futex wait mismatch failed\n");
        return 20;
    }

    errno = 0;
    ret = syscall(SYS_futex, &word, CAP_FUTEX_WAKE | CAP_FUTEX_PRIVATE_FLAG, 1, NULL, NULL, 0);
    if (ret != 0) {
        (void)write_all("musl_smoke: futex wake failed\n");
        return 21;
    }

    struct timespec ts = { 0, 0 };
    word = 0;
    errno = 0;
    ret = syscall(SYS_futex, &word, CAP_FUTEX_WAIT | CAP_FUTEX_PRIVATE_FLAG, 0, &ts, NULL, 0);
    if (ret != -1 || errno != ETIMEDOUT) {
        (void)write_all("musl_smoke: futex timeout failed\n");
        return 22;
    }

    return write_all("musl_smoke: futex ok\n") == 0 ? 0 : 23;
}

static int clone_invalid_flags_smoke(void) {
    char child_stack[4096];
    void *child_stack_top = child_stack + sizeof(child_stack);
    const long thread_flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;

    errno = 0;
    long ret = syscall(SYS_clone, CLONE_VM, 0, 0, 0, 0);
    if (ret != -1 || errno != EINVAL) {
        (void)write_all("musl_smoke: clone non-thread flags failed\n");
        return 28;
    }

    errno = 0;
    ret = syscall(SYS_clone, thread_flags, child_stack_top, 0, 0, 0);
    if (ret != -1 || errno != EINVAL) {
        (void)write_all("musl_smoke: clone missing settls failed\n");
        return 29;
    }

    return write_all("musl_smoke: clone invalid flags ok\n") == 0 ? 0 : 30;
}

static void *pthread_entry(void *arg) {
    long value = (long)arg;
    child_pid_seen = getpid();
    child_tid_seen = (pid_t)syscall(SYS_gettid);
    tls_probe += 11;
    child_tls_seen = tls_probe;
    pthread_value = (int)(value + 1);
    return (void *)7;
}

static int pthread_smoke(void) {
    pthread_t thread;
    void *joined = NULL;
    tls_probe = 100;
    main_pid_seen = getpid();
    main_tid_seen = (pid_t)syscall(SYS_gettid);
    child_pid_seen = 0;
    child_tid_seen = 0;
    child_tls_seen = 0;
    pthread_value = 0;
    if (pthread_create(&thread, NULL, pthread_entry, (void *)41) != 0) {
        (void)write_all("musl_smoke: pthread create failed\n");
        return 24;
    }
    if (pthread_join(thread, &joined) != 0) {
        (void)write_all("musl_smoke: pthread join failed\n");
        return 25;
    }
    if (main_pid_seen == 0 || main_tid_seen == 0 || child_pid_seen != main_pid_seen || child_tid_seen == main_tid_seen) {
        (void)write_all("musl_smoke: thread group failed\n");
        return 31;
    }
    if (tls_probe != 100) {
        (void)write_all("musl_smoke: main tls changed\n");
        return 32;
    }
    if (child_tls_seen != 42) {
        (void)write_all("musl_smoke: child tls failed\n");
        return 35;
    }
    if (write_all("musl_smoke: thread group ok\n") != 0) return 33;
    if (pthread_value != 42 || (long)joined != 7) {
        (void)write_all("musl_smoke: pthread result failed\n");
        return 26;
    }
    if (write_all("musl_smoke: clear-child-tid join ok\n") != 0) return 34;
    return write_all("musl_smoke: pthread ok\n") == 0 ? 0 : 27;
}

static void *four_pthread_entry(void *arg) {
    long index = (long)arg;
    tls_probe = 200 + (int)index;
    four_pthread_pid_seen[index] = getpid();
    four_pthread_tid_seen[index] = (pid_t)syscall(SYS_gettid);
    errno = 0;
    long ret = syscall(SYS_futex, (int *)&four_pthread_futex_words[index], CAP_FUTEX_WAIT | CAP_FUTEX_PRIVATE_FLAG, 0, NULL, NULL, 0);
    if (ret == -1 && errno == EAGAIN) __sync_fetch_and_add(&four_pthread_futex_eagain_count, 1);
    __sync_fetch_and_add(&four_pthread_tls_sum, tls_probe);
    __sync_fetch_and_or(&four_pthread_ready_mask, 1 << index);
    while (!four_pthread_start_gate) {
        syscall(SYS_futex, (int *)&four_pthread_start_gate, CAP_FUTEX_WAIT | CAP_FUTEX_PRIVATE_FLAG, 0, NULL, NULL, 0);
    }
    __sync_fetch_and_or(&four_pthread_done_mask, 1 << index);
    return (void *)(index + 11);
}

static int four_pthread_smoke(void) {
    pthread_t threads[4];
    main_pid_seen = getpid();
    main_tid_seen = (pid_t)syscall(SYS_gettid);
    four_pthread_ready_mask = 0;
    four_pthread_done_mask = 0;
    four_pthread_tls_sum = 0;
    four_pthread_futex_eagain_count = 0;
    four_pthread_start_gate = 0;
    for (int i = 0; i < 4; i++) {
        four_pthread_pid_seen[i] = 0;
        four_pthread_tid_seen[i] = 0;
        four_pthread_futex_words[i] = 1;
    }
    for (int i = 0; i < 4; i++) {
        int create_status = pthread_create(&threads[i], NULL, four_pthread_entry, (void *)(long)i);
        if (create_status != 0) {
            (void)write_all("musl_smoke: 4 pthread create failed\n");
            (void)write_all("musl_smoke: 4 pthread create index\n");
            write_dec(i);
            (void)write_all("musl_smoke: 4 pthread create status\n");
            write_dec(create_status);
            return 41;
        }
    }
    while (four_pthread_ready_mask != 0x0f) {
        __asm__ volatile("pause");
    }
    four_pthread_start_gate = 1;
    syscall(SYS_futex, (int *)&four_pthread_start_gate, CAP_FUTEX_WAKE | CAP_FUTEX_PRIVATE_FLAG, 4, NULL, NULL, 0);
    for (int i = 0; i < 4; i++) {
        void *joined = NULL;
        if (pthread_join(threads[i], &joined) != 0) {
            (void)write_all("musl_smoke: 4 pthread join failed\n");
            return 42;
        }
        if ((long)joined != i + 11) {
            (void)write_all("musl_smoke: 4 pthread return failed\n");
            return 43;
        }
    }
    if (four_pthread_ready_mask != 0x0f || four_pthread_done_mask != 0x0f) {
        (void)write_all("musl_smoke: 4 pthread masks failed\n");
        return 44;
    }
    if (four_pthread_tls_sum != (200 + 201 + 202 + 203)) {
        (void)write_all("musl_smoke: 4 pthread tls failed\n");
        return 45;
    }
    if (four_pthread_futex_eagain_count != 4) {
        (void)write_all("musl_smoke: 4 pthread futex failed\n");
        return 46;
    }
    for (int i = 0; i < 4; i++) {
        if (four_pthread_pid_seen[i] != main_pid_seen || four_pthread_tid_seen[i] == 0 || four_pthread_tid_seen[i] == main_tid_seen) {
            (void)write_all("musl_smoke: 4 pthread tid failed\n");
            return 47;
        }
        for (int j = i + 1; j < 4; j++) {
            if (four_pthread_tid_seen[i] == four_pthread_tid_seen[j]) {
                (void)write_all("musl_smoke: 4 pthread duplicate tid failed\n");
                return 48;
            }
        }
    }
    if (write_all("musl_smoke: 4 pthreads joined\n") != 0) return 49;
    return write_all("musl_smoke: 4 pthreads ok\n") == 0 ? 0 : 50;
}

static void *exit_group_wait_entry(void *arg) {
    (void)arg;
    exit_group_child_ready = 1;
    while (exit_group_wait_word == 0) {
        syscall(SYS_futex, (int *)&exit_group_wait_word, CAP_FUTEX_WAIT | CAP_FUTEX_PRIVATE_FLAG, 0, NULL, NULL, 0);
    }
    return (void *)9;
}

static void *exit_group_running_entry(void *arg) {
    (void)arg;
    exit_group_running_ready = 1;
    while (exit_group_running_spin) {
        __asm__ volatile("pause");
    }
    return (void *)10;
}

static int exit_group_teardown_smoke(void) {
    pthread_t waiter;
    pthread_t runner;
    exit_group_child_ready = 0;
    exit_group_wait_word = 0;
    exit_group_running_ready = 0;
    exit_group_running_spin = 1;
    if (pthread_create(&waiter, NULL, exit_group_wait_entry, NULL) != 0) {
        (void)write_all("musl_smoke: exit_group thread create failed\n");
        return 36;
    }
    if (pthread_create(&runner, NULL, exit_group_running_entry, NULL) != 0) {
        (void)write_all("musl_smoke: exit_group running thread create failed\n");
        return 39;
    }
    while (!exit_group_child_ready || !exit_group_running_ready) {
        syscall(SYS_gettid);
    }
    if (write_all("musl_smoke: exit_group child waiting\n") != 0) return 37;
    if (write_all("musl_smoke: exit_group running child ready\n") != 0) return 40;
    syscall(SYS_exit_group, 0);
    (void)write_all("musl_smoke: exit_group returned\n");
    return 38;
}

int main(int argc, char **argv, char **envp) {
    if (write_all("musl_smoke: main write via musl\n") != 0) return 1;
    if (argc != 2) return 2;
    if (argv == NULL || argv[0] == NULL || strcmp(argv[0], "/cmd/musl_smoke.elf") != 0) return 3;
    int execve_child = argv[1] != NULL && strcmp(argv[1], "execve-child") == 0;
    int exit_group_smoke = argv[1] != NULL && strcmp(argv[1], "exit-group-smoke") == 0;
    int four_pthread_mode = argv[1] != NULL && strcmp(argv[1], "4-pthread-smoke") == 0;
    if (!execve_child && !exit_group_smoke && !four_pthread_mode && (argv[1] == NULL || strcmp(argv[1], "argv-smoke") != 0)) return 4;
    if (envp == NULL || envp[0] == NULL || envp[1] == NULL) return 5;
    if (getenv("PATH") == NULL || strcmp(getenv("PATH"), "/bin:/cmd") != 0) return 6;
    if (getenv("CAPABILITYOS") == NULL || strcmp(getenv("CAPABILITYOS"), "1") != 0) return 7;
    if (execve_child && (getenv("EXECVE_STAGE") == NULL || strcmp(getenv("EXECVE_STAGE"), "1") != 0)) return 11;
    if (getauxval(AT_PAGESZ) != 4096) return 8;
    const char *execfn = (const char *)getauxval(AT_EXECFN);
    if (execfn == NULL || strcmp(execfn, "/cmd/musl_smoke.elf") != 0) return 9;
    if (write_all("musl_smoke: argv envp auxv ok\n") != 0) return 10;
    int futex_status = futex_smoke();
    if (futex_status != 0) return futex_status;
    if (exit_group_smoke) {
        return exit_group_teardown_smoke();
    }
    if (four_pthread_mode) {
        return four_pthread_smoke();
    }
    if (execve_child) {
        int clone_invalid_status = clone_invalid_flags_smoke();
        if (clone_invalid_status != 0) return clone_invalid_status;
        int pthread_status = pthread_smoke();
        if (pthread_status != 0) return pthread_status;
        if (write_all("musl_smoke: execve child ok\n") != 0) return 12;
        return 0;
    }
    char *const next_argv[] = { "/cmd/musl_smoke.elf", "execve-child", 0 };
    char *const next_envp[] = { "PATH=/bin:/usr/bin:/usr/lib/uutils:/cmd", "CAPABILITYOS=1", "EXECVE_STAGE=1", 0 };
    execve("/cmd/musl_smoke.elf", next_argv, next_envp);
    if (write_all("musl_smoke: execve failed\n") != 0) return 13;
    return 14;
}
