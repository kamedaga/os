#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>

static int write_all(const char *message) {
    size_t len = strlen(message);
    ssize_t written = write(1, message, len);
    return written == (ssize_t)len ? 0 : 1;
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
    if (execve_child) {
        if (write_all("musl_smoke: execve child ok\n") != 0) return 12;
        return 0;
    }
    char *const next_argv[] = { "/cmd/musl_smoke.elf", "execve-child", 0 };
    char *const next_envp[] = { "PATH=/bin:/cmd", "CAPABILITYOS=1", "EXECVE_STAGE=1", 0 };
    execve("/cmd/musl_smoke.elf", next_argv, next_envp);
    if (write_all("musl_smoke: execve failed\n") != 0) return 13;
    return 14;
}
