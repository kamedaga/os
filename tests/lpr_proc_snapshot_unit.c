/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_vfs/proc.c"

int lpr_strcmp(const char *a, const char *b) { return strcmp(a, b); }
void *lpr_memset(void *p, int c, size_t n) { return memset(p, c, n); }
void *lpr_memcpy(void *p, const void *q, size_t n) { return memcpy(p, q, n); }
size_t lpr_strnlen(const char *p, size_t n) { return strnlen(p, n); }
int64_t lpr_linux_getpid(void) { return 42; }
int lpr_supervisor_query_process(uint64_t pid, lprs_process_query_t *out)
{
    if (pid != 42 && pid != 77) return -LPR_LINUX_ESRCH;
    out->pid = pid;
    if (pid == 77) out->run_state = LPRS_PROCESS_RUN_STATE_ZOMBIE;
    return 0;
}
int64_t lpr_linux_memfd_create(uint64_t name, uint64_t flags)
{
    assert(!strcmp((const char *)(uintptr_t)name, "probe"));
    assert(flags == LPR_LINUX_MFD_ALLOW_SEALING);
    return 37;
}
int main(void)
{
    const char *paths[] = {"/proc/cpuinfo", "/proc/meminfo", "/proc/stat",
        "/proc/mounts", "/proc/version"};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        int64_t status = 999;
        assert(lpr_linux_proc_snapshot_access(paths[i], 0, 0, &status) && !status);
        assert(lpr_linux_proc_snapshot_access(paths[i], 4, 0x200, &status) && !status);
        assert(lpr_linux_proc_snapshot_access(paths[i], 4, 0x1100, &status) && !status);
        assert(lpr_linux_proc_snapshot_access(paths[i], 2, 0, &status) && status == -LPR_LINUX_EACCES);
        assert(lpr_linux_proc_snapshot_access(paths[i], 1, 0, &status) && status == -LPR_LINUX_EACCES);
        assert(lpr_linux_proc_snapshot_access(paths[i], 8, 0, &status) && status == -LPR_LINUX_EINVAL);
        assert(lpr_linux_proc_snapshot_access(paths[i], 4, 0x80000000, &status) && status == -LPR_LINUX_EINVAL);
    }
    int64_t status = 999;
    assert(!lpr_linux_proc_snapshot_access("/proc/absent", 4, 0, &status) && status == 999);
    assert(!lpr_linux_proc_snapshot_access("/proc/cpuinfo/child", 4, 0, &status));
    assert(!lpr_linux_proc_snapshot_access("/file", 4, 0, &status));
    assert(lpr_proc_snapshot_create("probe", LPR_LINUX_O_RDONLY | LPR_LINUX_O_NOCTTY) == 37);
    assert(lpr_proc_snapshot_create("probe", LPR_LINUX_O_WRONLY) == -LPR_LINUX_EACCES);
    assert(lpr_proc_snapshot_create("probe", LPR_LINUX_O_DIRECTORY) == -LPR_LINUX_EINVAL);
    uint64_t pid = 0;
    assert(lpr_linux_proc_pid_directory("/proc/self", &pid, &status) && pid == 42 && !status);
    assert(lpr_linux_proc_pid_directory("/proc/42/", &pid, &status) && pid == 42 && !status);
    assert(lpr_linux_proc_pid_directory("/proc/77", &pid, &status) && pid == 77 && !status);
    assert(lpr_linux_proc_pid_directory("/proc/99", &pid, &status) && status == -LPR_LINUX_ENOENT);
    assert(!lpr_linux_proc_pid_directory("/proc/42/stat", &pid, &status));
    assert(!lpr_linux_proc_pid_directory("/proc/self/fd/3", &pid, &status));
    assert(!lpr_linux_proc_pid_directory("/proc/42suffix", &pid, &status));
    char target[8] = {0};
    assert(lpr_linux_proc_readlink("/proc/self", target, sizeof(target), &status));
    assert(status == 2 && !memcmp(target, "42", 2));
    assert(lpr_linux_proc_readlink("/proc/self", target, 1, &status) && status == 1 && target[0] == '4');
    puts("proc snapshots: metadata-open flags, existence/read access, denied writes/exec and filesystem fallthrough PASS");
}
