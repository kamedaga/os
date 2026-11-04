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
lpr_state_t lpr_state;
static uint64_t directory_offset;
static unsigned char live_fds[16];
uint64_t lpr_filed_control_offset(uint64_t fd) { (void)fd;return directory_offset; }
void lpr_filed_control_set_offset(uint64_t fd,uint64_t value) { (void)fd;directory_offset=value; }
int lpr_control_fd_active(uint64_t fd) { return fd<sizeof(live_fds) && live_fds[fd]; }
uint16_t lpr_dirent_reclen(uint64_t n) { return (uint16_t)((19+n+1+7)&~7ull); }
uint8_t lpr_dtype_from_mode(uint64_t mode) { return (uint8_t)((mode>>12)&15); }
size_t lpr_strlen(const char *s) { return strlen(s); }
static void proc_fd_listing_test(void)
{
    unsigned char bytes[128];lpr_fd_table_capacity=16;live_fds[0]=live_fds[3]=live_fds[15]=1;
    assert(lpr_proc_fd_getdents(4,0,16)==-LPR_LINUX_EFAULT);
    assert(lpr_proc_fd_getdents(4,(uintptr_t)bytes,23)==-LPR_LINUX_EINVAL && !directory_offset);
    assert(lpr_proc_fd_getdents(4,(uintptr_t)bytes,48)==48 && directory_offset==2);
    assert(!strcmp((char *)bytes+19,".") && !strcmp((char *)bytes+24+19,".."));
    assert(lpr_proc_fd_getdents(4,(uintptr_t)bytes,24)==24 && !strcmp((char *)bytes+19,"0"));
    live_fds[0]=0; /* Removal before the cookie cannot shift or skip FD 3. */
    assert(lpr_proc_fd_getdents(4,(uintptr_t)bytes,24)==24 && !strcmp((char *)bytes+19,"3"));
    assert(bytes[18]==10 && directory_offset==17);
    assert(lpr_proc_fd_getdents(4,(uintptr_t)bytes,sizeof(bytes))==24 && !strcmp((char *)bytes+19,"15"));
    assert(!lpr_proc_fd_getdents(4,(uintptr_t)bytes,sizeof(bytes)));
    directory_offset=0;assert(lpr_proc_fd_getdents(4,(uintptr_t)bytes,24)==24 && !strcmp((char *)bytes+19,"."));
}
int main(void)
{
    proc_fd_listing_test();
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
