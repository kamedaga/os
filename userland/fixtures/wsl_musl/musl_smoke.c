#include <errno.h>
#include <stddef.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <sys/auxv.h>
#include <unistd.h>

#ifndef SYS_futex
#define SYS_futex 202
#endif
#define CAP_FUTEX_WAIT 0
#define CAP_FUTEX_WAKE 1
#define CAP_FUTEX_PRIVATE_FLAG 128

static volatile int pthread_value = 0;

static int write_all(const char *message) {
    size_t len = strlen(message);
    ssize_t written = write(1, message, len);
    return written == (ssize_t)len ? 0 : 1;
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

static void *pthread_entry(void *arg) {
    long value = (long)arg;
    pthread_value = (int)(value + 1);
    return (void *)7;
}

static int pthread_smoke(void) {
    pthread_t thread;
    void *joined = NULL;
    pthread_value = 0;
    if (pthread_create(&thread, NULL, pthread_entry, (void *)41) != 0) {
        (void)write_all("musl_smoke: pthread create failed\n");
        return 24;
    }
    if (pthread_join(thread, &joined) != 0) {
        (void)write_all("musl_smoke: pthread join failed\n");
        return 25;
    }
    if (pthread_value != 42 || (long)joined != 7) {
        (void)write_all("musl_smoke: pthread result failed\n");
        return 26;
    }
    return write_all("musl_smoke: pthread ok\n") == 0 ? 0 : 27;
}

int main(int argc, char **argv, char **envp) {
    if (write_all("musl_smoke: main write via musl\n") != 0) return 1;
    if (argc != 2) return 2;
    if (argv == NULL || argv[0] == NULL || strcmp(argv[0], "/cmd/musl_smoke.elf") != 0) return 3;
    int execve_child = argv[1] != NULL && strcmp(argv[1], "execve-child") == 0;
    if (!execve_child && (argv[1] == NULL || strcmp(argv[1], "argv-smoke") != 0)) return 4;
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
    if (execve_child) {
        int pthread_status = pthread_smoke();
        if (pthread_status != 0) return pthread_status;
        if (write_all("musl_smoke: execve child ok\n") != 0) return 12;
        return 0;
    }
    char *const next_argv[] = { "/cmd/musl_smoke.elf", "execve-child", 0 };
    char *const next_envp[] = { "PATH=/bin:/cmd", "CAPABILITYOS=1", "EXECVE_STAGE=1", 0 };
    execve("/cmd/musl_smoke.elf", next_argv, next_envp);
    if (write_all("musl_smoke: execve failed\n") != 0) return 13;
    return 14;
}
