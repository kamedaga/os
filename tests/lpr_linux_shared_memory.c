#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef F_SEAL_EXEC
#define F_SEAL_EXEC 0x0020
#endif

static _Thread_local sigjmp_buf fault_return;
static _Thread_local volatile sig_atomic_t expected_signal, received_signal;
static _Thread_local void *received_address;

static void fault_handler(int signal, siginfo_t *info, void *context)
{
    (void)context;
    if (signal != expected_signal) _exit(201);
    received_signal = signal;
    received_address = info->si_addr;
    siglongjmp(fault_return, 1);
}

static int expect_fault(volatile unsigned char *address, int signal)
{
    expected_signal = signal;
    received_signal = 0;
    received_address = NULL;
    if (!sigsetjmp(fault_return, 1)) {
        volatile unsigned char byte = *address;
        (void)byte;
        return -1;
    }
    expected_signal = 0;
    return received_signal == signal && received_address == (const void *)address ? 0 : -1;
}

static void *thread_probe(void *argument)
{
    unsigned loops = argument ? (unsigned)(uintptr_t)argument : 1;
    for (unsigned i = 0; i < loops; ++i) {
    unsigned char *guard = mmap(NULL, 4096, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (guard == MAP_FAILED) return (void *)1;
    int failed = expect_fault(guard, SIGSEGV) || munmap(guard, 4096);
    if (failed) return (void *)1;
    }
    return NULL;
}

#define CHECK(condition) do { if (!(condition)) { \
    printf("SHARED_MEMORY=FAIL line=%u errno=%d\n", __LINE__, errno); return 1; \
} } while (0)

static int readonly_seals(void)
{
    const size_t size = (2U << 20) + 4096;
    int fd = memfd_create("shared-ro-seals", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(fd >= 0 && !ftruncate(fd, (off_t)size));
    unsigned char *may_write = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(may_write != MAP_FAILED);
    errno = 0;
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EBUSY);
    CHECK(!mprotect(may_write, 4096, PROT_READ | PROT_WRITE));
    may_write[0] = 173;
    CHECK(!munmap(may_write, 4096));
    CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE));
    unsigned char *first = mmap(NULL, size + 4096, PROT_READ, MAP_SHARED, fd, 0);
    unsigned char *second = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(first != MAP_FAILED && second != MAP_FAILED);
    CHECK(!expect_fault(first + size, SIGBUS));
    CHECK(!ftruncate(fd, 4096) && !expect_fault(second + 4096, SIGBUS));
    CHECK(!ftruncate(fd, (off_t)size) && second[4096] == 0);
    errno = 0;
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | 0x40000000) == -1 && errno == EINVAL);
    CHECK(fcntl(fd, F_GET_SEALS) == F_SEAL_FUTURE_WRITE);
    CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE));
    CHECK(first[0] == 173 && second[0] == 173);
    errno = 0;
    CHECK(mprotect(first, 4096, PROT_READ | PROT_WRITE) == -1);
    /* Native MPROTECT currently merges authority and invalid-range failures.
     * Keep the security regression strict without claiming Linux errno parity;
     * that needs a distinct generic native status, not an LPR guess/cache. */
    CHECK(errno == EACCES || errno == EINVAL);
    printf("SHARED_MEMORY readonly-mprotect-denied errno=%d (Linux=%d)\n", errno, EACCES);
    unsigned char byte = 0;
    CHECK(pread(fd, &byte, 1, 0) == 1 && byte == 173);
    errno = 0;
    CHECK(pwrite(fd, &byte, 1, 0) == -1 && errno == EPERM);
    CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SEAL));
    errno = 0;
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW) == -1 && errno == EPERM);
    CHECK(!close(fd) && first[0] == 173 && second[0] == 173);
    CHECK(!munmap(first, size + 4096) && !munmap(second, size));

    fd = memfd_create("shared-exec-seals", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(fd >= 0 && !ftruncate(fd, 4096) && !fchmod(fd, 0700));
    CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE));
    first = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(first != MAP_FAILED && !fcntl(fd, F_ADD_SEALS, F_SEAL_EXEC));
    int implied = F_SEAL_EXEC | F_SEAL_WRITE | F_SEAL_FUTURE_WRITE | F_SEAL_SHRINK | F_SEAL_GROW;
    CHECK((fcntl(fd, F_GET_SEALS) & implied) == implied);
    CHECK(first[0] == 0 && !close(fd) && !munmap(first, 4096));
    puts("SHARED_MEMORY RO-ceiling/duplicate-pins/holes/atomic-seals/EXEC=OK");
    return 0;
}

static int disk_writeback(void)
{
    char name[4096];
    unsigned char byte = 0;
    const char *directory = getenv("SHARED_DISK_DIRECTORY");
    int name_length = snprintf(name, sizeof(name), "%s/shared-file-XXXXXX",
        directory ? directory : "/tmp");
    CHECK(name_length > 0 && (size_t)name_length < sizeof(name));
    int disk = mkstemp(name);
    CHECK(disk >= 0 && !ftruncate(disk, 8192));
    unsigned char *file_map = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
        MAP_SHARED, disk, 0);
    CHECK(file_map != MAP_FAILED);
    file_map[4096] = 157;
    CHECK(!msync(file_map, 8192, MS_SYNC) && !fsync(disk));
    CHECK(pread(disk, &byte, 1, 4096) == 1 && byte == 157);
    struct stat info;
    CHECK(!fstat(disk, &info) && info.st_blocks > 0);
    int direct = open(name, O_RDONLY | O_DIRECT);
    if (direct >= 0) {
        void *aligned = NULL;
        CHECK(!posix_memalign(&aligned, 4096, 4096));
        /* Native writes do not redirty a Linux PTE. A direct read must dirty
         * and flush the shared cache even immediately after an earlier sync. */
        file_map[4096] = 181;
        CHECK(pread(direct, aligned, 4096, 4096) == 4096);
        CHECK(((unsigned char *)aligned)[0] == 181);
        file_map[4096] = 157;
        CHECK(!fsync(disk) && !close(direct));
        free(aligned);
        puts("SHARED_MEMORY post-fsync-native-write/direct-read=OK");
    } else {
        CHECK(errno == EINVAL || errno == EOPNOTSUPP);
    }
    CHECK(!munmap(file_map, 8192) && !close(disk));
    disk = open(name, O_RDONLY | O_DIRECT);
    if (disk >= 0) {
        void *aligned = NULL;
        CHECK(!posix_memalign(&aligned, 4096, 4096));
        CHECK(pread(disk, aligned, 4096, 4096) == 4096);
        CHECK(((unsigned char *)aligned)[0] == 157);
        free(aligned);
        CHECK(!close(disk));
        puts("SHARED_MEMORY sparse-file/fsync/direct-read=OK");
    } else {
        CHECK(errno == EINVAL || errno == EOPNOTSUPP);
        puts("SHARED_MEMORY sparse-file/fsync=OK direct-read=UNSUPPORTED");
    }
    CHECK(!unlink(name));
    puts("SHARED_DISK=PASS");
    return 0;
}

static int file_identity(void)
{
    char first_name[] = "/tmp/shared-identity-XXXXXX";
    char other_name[] = "/tmp/shared-unrelated-XXXXXX";
    char link_name[sizeof(first_name) + 8];
    int first = mkstemp(first_name), other = mkstemp(other_name);
    CHECK(first >= 0 && other >= 0);
    CHECK(!ftruncate(first, 8192) && !ftruncate(other, 8192));
    volatile unsigned char *a = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
        MAP_SHARED, first, 0);
    volatile unsigned char *b = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
        MAP_SHARED, other, 0);
    CHECK(a != MAP_FAILED && b != MAP_FAILED);
    a[4096] = 73;
    b[4096] = 91;
    CHECK(snprintf(link_name, sizeof(link_name), "%s-link", first_name) > 0);
    CHECK(!link(first_name, link_name));
    int alias = open(link_name, O_RDWR);
    CHECK(alias >= 0 && !close(first));
    CHECK(!unlink(first_name) && !unlink(link_name));
    /* A distinct open file and an unlinked hardlink must still refresh the
     * original mapping. Another inode's aliases must remain unchanged. */
    CHECK(!ftruncate(alias, 4096));
    CHECK(!expect_fault(a + 4096, SIGBUS) && b[4096] == 91);
    CHECK(!ftruncate(alias, 8192) && a[4096] == 0 && b[4096] == 91);
    unsigned char byte = 123;
    CHECK(pwrite(alias, &byte, 1, 4096) == 1 && a[4096] == byte);
    CHECK(!ftruncate(other, 4096));
    CHECK(!expect_fault(b + 4096, SIGBUS) && a[4096] == byte);
    CHECK(!munmap((void *)a, 8192) && !munmap((void *)b, 8192));
    CHECK(!close(alias) && !close(other) && !unlink(other_name));
    puts("SHARED_MEMORY hardlink/reopen/unlink/independent-inodes=OK");
    return 0;
}

static int file_batch_boundary(void)
{
    const size_t boundary = 2u * 1024u * 1024u;
    const size_t bytes = boundary + 17, mapped = boundary + 8192;
    char name[] = "/tmp/shared-batch-XXXXXX";
    int writer = mkstemp(name);
    CHECK(writer >= 0 && !ftruncate(writer, (off_t)bytes));
    unsigned char pair[2] = {73, 91};
    CHECK(pwrite(writer, pair, 2, boundary - 1) == 2);
    int reader = open(name, O_RDONLY);
    CHECK(reader >= 0 && !unlink(name));
    volatile unsigned char *a = mmap(NULL, mapped, PROT_READ, MAP_SHARED, reader, 0);
    volatile unsigned char *b = mmap(NULL, mapped - 4096, PROT_READ,
        MAP_SHARED, reader, 4096);
    CHECK(a != MAP_FAILED && b != MAP_FAILED);
    CHECK(a[4096] == 0 && a[boundary - 1] == 73 && a[boundary] == 91);
    CHECK(b[boundary - 4096] == 91 && a[bytes - 1] == 0 && a[bytes] == 0);
    CHECK(!expect_fault(a + boundary + 4096, SIGBUS));
    CHECK(!expect_fault(b + boundary, SIGBUS));
    CHECK(!ftruncate(writer, (off_t)boundary));
    CHECK(a[boundary - 1] == 73 && !expect_fault(a + boundary, SIGBUS));
    CHECK(!expect_fault(b + boundary - 4096, SIGBUS));
    CHECK(!ftruncate(writer, (off_t)bytes));
    CHECK(a[boundary] == 0 && b[boundary - 4096] == 0);
    CHECK(!munmap((void *)a, mapped) && !munmap((void *)b, mapped - 4096));
    CHECK(!close(reader) && !close(writer));
    puts("SHARED_MEMORY file-batch-boundary/offset/partial-EOF/shrink-grow=OK");
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "disk")) return disk_writeback();
    struct sigaction action = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGBUS, &action, NULL) && !sigaction(SIGSEGV, &action, NULL));
    int empty = memfd_create("shared-empty", MFD_CLOEXEC);
    CHECK(empty >= 0);
    volatile unsigned char *holes = mmap(NULL, 12288, PROT_NONE, MAP_SHARED, empty, 0);
    CHECK(holes != MAP_FAILED && !mprotect((void *)holes, 12288, PROT_READ | PROT_WRITE));
    CHECK(!expect_fault(holes, SIGBUS) && !expect_fault(holes + 4096, SIGBUS));
    CHECK(!ftruncate(empty, 8192) && holes[0] == 0 && holes[4096] == 0);
    holes[4096] = 23;
    CHECK(!ftruncate(empty, 0) && !expect_fault(holes + 4096, SIGBUS));
    CHECK(mmap((void *)holes + 4096, 4096, PROT_READ, MAP_SHARED | MAP_FIXED, empty, 0) == (void *)holes + 4096);
    CHECK(!expect_fault(holes + 4096, SIGBUS) && !munmap((void *)holes + 4096, 4096));
    CHECK(mmap((void *)holes + 4096, 4096, PROT_READ,
        MAP_SHARED | MAP_FIXED_NOREPLACE, empty, 0) == (void *)holes + 4096);
    CHECK(!expect_fault(holes + 4096, SIGBUS));
    CHECK(!ftruncate(empty, 4096));
    CHECK(holes[0] == 0 && holes[4096] == 0 && !expect_fault(holes + 8192, SIGBUS));
    CHECK(!munmap((void *)holes, 12288) && !close(empty));
    puts("SHARED_MEMORY all-hole/first-hole/PROT_NONE/FIXED/NOREPLACE/refill=OK");
    int fd = memfd_create("shared-foundation", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(fd >= 0 && !ftruncate(fd, 8192));
    volatile unsigned char *whole = mmap(NULL, 12288, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    volatile unsigned char *offset = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096);
    CHECK(whole != MAP_FAILED && offset != MAP_FAILED);
    whole[0] = 31;
    whole[4096] = 47;
    CHECK(offset[0] == 47);
    offset[1] = 63;
    unsigned char byte = 0;
    CHECK(pread(fd, &byte, 1, 4097) == 1 && byte == 63);
    byte = 79;
    CHECK(pwrite(fd, &byte, 1, 2) == 1 && whole[2] == 79);
    CHECK(!msync((void *)whole, 8192, MS_SYNC));
    puts("SHARED_MEMORY aliases/pread/pwrite/nonzero-offset=OK");

    CHECK(!expect_fault(whole + 8192, SIGBUS));
    CHECK(!ftruncate(fd, 4096) && whole[0] == 31);
    CHECK(!expect_fault(whole + 4096, SIGBUS) && !expect_fault(offset, SIGBUS));
    CHECK(!ftruncate(fd, 8192) && whole[4096] == 0 && offset[0] == 0);
    CHECK(!mprotect((void *)offset, 4096, PROT_NONE));
    CHECK(!expect_fault(offset, SIGSEGV));
    CHECK(!mprotect((void *)offset, 4096, PROT_READ | PROT_WRITE));
    puts("SHARED_MEMORY EOF/shrink/grow/SIGBUS-vs-SIGSEGV=OK");

    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        if (whole[0] != 31 || expect_fault(whole + 8192, SIGBUS)) _exit(202);
        whole[3] = 97;
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(whole[3] == 97);
    for (unsigned i = 0; i < 12; ++i) {
        pthread_t thread;
        void *result = NULL;
        CHECK(!pthread_create(&thread, NULL, thread_probe, NULL));
        CHECK(!pthread_join(thread, &result) && result == NULL);
    }
    puts("SHARED_MEMORY fork/thread-fault-registration=OK");
    pthread_t racers[4];
    for (unsigned i = 0; i < 4; ++i)
        CHECK(!pthread_create(&racers[i], NULL, thread_probe, (void *)16));
    for (unsigned i = 0; i < 8; ++i) {
        child = fork();
        CHECK(child >= 0);
        if (!child) _exit(expect_fault(whole + 8192, SIGBUS) ? 203 : 0);
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    for (unsigned i = 0; i < 4; ++i) {
        void *result = NULL;
        CHECK(!pthread_join(racers[i], &result) && result == NULL);
    }
    puts("SHARED_MEMORY concurrent-fork/thread-init-exit=OK");

    CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE));
    void *read_only = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(read_only != MAP_FAILED);
    CHECK(mprotect(read_only, 4096, PROT_READ | PROT_WRITE) == -1);
    errno = 0;
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EPERM);
    whole[4] = 113;
    CHECK(*(volatile unsigned char *)read_only == 31);
    CHECK(!munmap((void *)whole + 4096, 4096));
    void *target = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(target != MAP_FAILED);
    void *moved = mremap((void *)whole, 4096, 4096, MREMAP_MAYMOVE | MREMAP_FIXED, target);
    CHECK(moved == target && ((volatile unsigned char *)moved)[4] == 113);
    CHECK(!munmap(moved, 4096) && !munmap((void *)whole + 8192, 4096));
    CHECK(!munmap((void *)offset, 4096));
    CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE));
    CHECK(!close(fd));
    CHECK(*(volatile unsigned char *)read_only == 31 && !munmap(read_only, 4096));
    puts("SHARED_MEMORY seals/partial-unmap/mremap/close-lifetime=OK");

    CHECK(!readonly_seals());
    CHECK(!file_identity());
    CHECK(!file_batch_boundary());
    CHECK(!disk_writeback());
    puts("SHARED_MEMORY=PASS");
    return 0;
}
