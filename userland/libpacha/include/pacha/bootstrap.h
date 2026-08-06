#pragma once

#include "pacha/abi.h"

static inline int pacha_bootstrap_fd_from_argv(char **argv)
{
    if (argv == 0) return -1;
    char **envp = argv;
    while (*envp != 0) envp++;
    envp++;
    while (*envp != 0) envp++;
    envp++;
    unsigned long long *auxv = (unsigned long long *)envp;
    for (; auxv[0] != 0; auxv += 2) {
        if (auxv[0] == PACHA_AT_BOOTSTRAP_FD && auxv[1] >= 16 && auxv[1] < 256)
            return (int)auxv[1];
    }
    return -1;
}
