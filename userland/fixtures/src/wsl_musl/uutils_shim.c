#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *basename_const(const char *path) {
    const char *name = path;
    for (const char *p = path; *p != 0; p++) {
        if (*p == '/') name = p + 1;
    }
    return name;
}

int main(int argc, char **argv, char **envp) {
    const char *tool = argc > 0 && argv[0] != NULL ? basename_const(argv[0]) : "";
    if (tool[0] == 0 || strcmp(tool, "uutils_shim.elf") == 0 || strcmp(tool, "uutils-shim") == 0) {
        fputs("uutils-shim: expected invocation through a coreutils command name\n", stderr);
        return 125;
    }

    char **next_argv = calloc((size_t)argc + 2, sizeof(char *));
    if (next_argv == NULL) {
        perror("uutils-shim: calloc");
        return 125;
    }

    next_argv[0] = "/cmd/coreutils.elf";
    next_argv[1] = (char *)tool;
    for (int i = 1; i < argc; i++) next_argv[i + 1] = argv[i];
    next_argv[argc + 1] = NULL;

    execve("/cmd/coreutils.elf", next_argv, envp);
    const int saved_errno = errno;
    perror("uutils-shim: execve /cmd/coreutils.elf");
    return saved_errno == ENOENT ? 127 : 126;
}
