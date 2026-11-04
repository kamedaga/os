/* Fork-only children must retain executable identity and enumerate open FDs. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    char before[4096], after[4096];
    ssize_t n = readlink("/proc/self/exe", before, sizeof(before));
    if (n <= 0) return 1;
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) { perror("opendir proc fd"); return 2; }
    int found_self = 0;
    char own[32];
    snprintf(own, sizeof(own), "%d", dirfd(directory));
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (!strcmp(entry->d_name, own)) found_self = 1;
    rewinddir(directory);
    entry = readdir(directory);
    int rewound = entry && !strcmp(entry->d_name, ".");
    closedir(directory);
    if (!found_self || !rewound) return 3;
    pid_t pid = fork();
    if (pid < 0) return 4;
    if (!pid) {
        ssize_t m = readlink("/proc/self/exe", after, sizeof(after));
        if (m != n || memcmp(before, after, (size_t)n)) {
            fprintf(stderr, "FORK_IDENTITY_FAIL errno=%d bytes=%zd\n", errno, m);
            _exit(5);
        }
        _exit(0);
    }
    int status;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status)) return 5;
    puts("PROC_FD_AND_FORK_IDENTITY_PASS");
    return 0;
}
