#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int cmp_ints(const void *a, const void *b)
{
    const int left = *(const int *)a;
    const int right = *(const int *)b;
    return (left > right) - (left < right);
}

int main(int argc, char **argv)
{
    const char *tmp_dir = "/tmp/lpr-cli-dir";
    const char *tmp_file = "/tmp/lpr-cli-dir/out.txt";
    const char *tmp_renamed = "/tmp/lpr-cli-dir/out.renamed";

    if (argc != 5 ||
        strcmp(argv[1], "--self") != 0 ||
        strcmp(argv[2], "/cmd/lpr_ldmusl_smoke.elf") != 0 ||
        strcmp(argv[3], "--write") != 0 ||
        strcmp(argv[4], "/tmp/lpr_cli_out.txt") != 0)
    {
        return 5;
    }

    const char *library_path = getenv("LD_LIBRARY_PATH");
    if (library_path == NULL || strcmp(library_path, "/lib/linux") != 0) {
        return 10;
    }

    char *heap = malloc(128);
    if (heap == NULL) {
        return 11;
    }
    snprintf(heap, 128, "heap:%s", library_path);
    if (strcmp(heap, "heap:/lib/linux") != 0) {
        free(heap);
        return 12;
    }
    free(heap);

    FILE *file = fopen("/cmd/lpr_ldmusl_smoke.elf", "rb");
    if (file == NULL) {
        return 20;
    }
    unsigned char magic[4] = {0};
    if (fread(magic, 1, sizeof(magic), file) != sizeof(magic)) {
        fclose(file);
        return 21;
    }
    if (memcmp(magic, "\177""ELF", 4) != 0) {
        fclose(file);
        return 22;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 23;
    }
    if (ftell(file) <= 0) {
        fclose(file);
        return 24;
    }
    if (fclose(file) != 0) {
        return 25;
    }

    struct stat st;
    if (stat("/cmd/lpr_ldmusl_smoke.elf", &st) != 0 || st.st_size <= 0) {
        return 30;
    }
    if (lstat("/cmd/lpr_ldmusl_smoke.elf", &st) != 0 || !S_ISREG(st.st_mode)) {
        return 31;
    }
    if (fstatat(AT_FDCWD, "/cmd/lpr_ldmusl_smoke.elf", &st, 0) != 0 || !S_ISREG(st.st_mode)) {
        return 32;
    }
    if (access("/cmd/lpr_ldmusl_smoke.elf", R_OK) != 0) {
        return 33;
    }
    errno = 0;
    if (stat("/cmd/definitely-missing-for-lpr", &st) == 0 || errno != ENOENT) {
        return 36;
    }
    errno = 0;
    int bad_dir_fd = open("/cmd/lpr_ldmusl_smoke.elf", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (bad_dir_fd >= 0 || errno != ENOTDIR) {
        if (bad_dir_fd >= 0) {
            close(bad_dir_fd);
        }
        return 37;
    }
    errno = 0;
    int write_dir_fd = open("/cmd", O_RDWR | O_DIRECTORY | O_CLOEXEC);
    if (write_dir_fd >= 0 || errno != EISDIR) {
        if (write_dir_fd >= 0) {
            close(write_dir_fd);
        }
        return 38;
    }

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL || cwd[0] != '/') {
        return 34;
    }
    char resolved[PATH_MAX];
    if (realpath("/cmd/lpr_ldmusl_smoke.elf", resolved) == NULL ||
        strcmp(resolved, "/cmd/lpr_ldmusl_smoke.elf") != 0)
    {
        return 35;
    }

    DIR *dir = opendir("/cmd");
    if (dir == NULL) {
        return 40;
    }
    int found_self = 0;
    for (;;) {
        struct dirent *de = readdir(dir);
        if (de == NULL) {
            break;
        }
        if (strcmp(de->d_name, "lpr_ldmusl_smoke.elf") == 0) {
            found_self = 1;
        }
    }
    if (closedir(dir) != 0) {
        return 41;
    }
    if (!found_self) {
        return 42;
    }

    int dir_fd = open("/cmd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        return 60;
    }
    int rel_fd = openat(dir_fd, "lpr_ldmusl_smoke.elf", O_RDONLY | O_CLOEXEC);
    if (rel_fd < 0) {
        close(dir_fd);
        return 61;
    }
    unsigned char rel_magic[4] = {0};
    if (read(rel_fd, rel_magic, sizeof(rel_magic)) != (ssize_t)sizeof(rel_magic) ||
        memcmp(rel_magic, "\177""ELF", 4) != 0)
    {
        close(rel_fd);
        close(dir_fd);
        return 62;
    }
    char dirent_buffer[256];
    errno = 0;
    if (syscall(SYS_getdents64, rel_fd, dirent_buffer, sizeof(dirent_buffer)) >= 0 ||
        errno != ENOTDIR)
    {
        close(rel_fd);
        close(dir_fd);
        return 64;
    }
    if (close(rel_fd) != 0 || close(dir_fd) != 0) {
        return 63;
    }

    (void)unlink(tmp_renamed);
    (void)unlink(tmp_file);
    (void)rmdir(tmp_dir);
    (void)unlink(argv[4]);

    FILE *out = fopen(argv[4], "wb");
    if (out == NULL) {
        return 70;
    }
    if (fprintf(out, "argc=%d self=%s libc=%s\n", argc, argv[2], library_path) < 0) {
        fclose(out);
        return 71;
    }
    if (fputs("stdio-write-ok\n", out) < 0) {
        fclose(out);
        return 72;
    }
    if (fflush(out) != 0 || fsync(fileno(out)) != 0 || fclose(out) != 0) {
        return 73;
    }

    FILE *in = fopen(argv[4], "rb");
    if (in == NULL) {
        return 74;
    }
    char line[128];
    if (fgets(line, sizeof(line), in) == NULL ||
        strstr(line, "argc=5") == NULL ||
        strstr(line, "libc=/lib/linux") == NULL)
    {
        fclose(in);
        return 75;
    }
    if (fgets(line, sizeof(line), in) == NULL ||
        strcmp(line, "stdio-write-ok\n") != 0)
    {
        fclose(in);
        return 76;
    }
    if (fclose(in) != 0) {
        return 77;
    }
    if (unlink(argv[4]) != 0) {
        return 78;
    }

    if (mkdir(tmp_dir, 0700) != 0) {
        return 80;
    }
    int tmp_dir_fd = open(tmp_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (tmp_dir_fd < 0) {
        return 81;
    }
    int nested_fd = openat(tmp_dir_fd, "out.txt", O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (nested_fd < 0) {
        close(tmp_dir_fd);
        return 82;
    }
    errno = 0;
    int duplicate_fd = openat(tmp_dir_fd, "out.txt", O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (duplicate_fd >= 0 || errno != EEXIST) {
        if (duplicate_fd >= 0) {
            close(duplicate_fd);
        }
        close(nested_fd);
        close(tmp_dir_fd);
        return 94;
    }
    const char nested_payload[] = "rename-readback-ok\n";
    if (write(nested_fd, nested_payload, sizeof(nested_payload) - 1u) != (ssize_t)(sizeof(nested_payload) - 1u) ||
        fsync(nested_fd) != 0 ||
        close(nested_fd) != 0)
    {
        close(tmp_dir_fd);
        return 83;
    }
    if (renameat(tmp_dir_fd, "out.txt", tmp_dir_fd, "out.renamed") != 0) {
        close(tmp_dir_fd);
        return 84;
    }
    int renamed_fd = openat(tmp_dir_fd, "out.renamed", O_RDONLY | O_CLOEXEC);
    if (renamed_fd < 0) {
        close(tmp_dir_fd);
        return 85;
    }
    char nested_readback[sizeof(nested_payload)] = {0};
    const ssize_t nested_read = read(renamed_fd, nested_readback, sizeof(nested_readback) - 1u);
    if (nested_read != (ssize_t)(sizeof(nested_readback) - 1u)) {
        close(tmp_dir_fd);
        return 86;
    }
    if (strcmp(nested_readback, nested_payload) != 0) {
        close(renamed_fd);
        close(tmp_dir_fd);
        return 92;
    }
    if (close(renamed_fd) != 0) {
        close(tmp_dir_fd);
        return 93;
    }
    if (unlinkat(tmp_dir_fd, "out.renamed", 0) != 0) {
        close(tmp_dir_fd);
        return 87;
    }
    if (openat(tmp_dir_fd, "out.renamed", O_RDONLY | O_CLOEXEC) >= 0 || errno != ENOENT) {
        close(tmp_dir_fd);
        return 88;
    }
    if (close(tmp_dir_fd) != 0) {
        return 89;
    }
    if (rmdir(tmp_dir) != 0) {
        return 90;
    }
    if (access(tmp_dir, F_OK) == 0 || errno != ENOENT) {
        return 91;
    }
    errno = 0;
    if (close(tmp_dir_fd) == 0 || errno != EBADF) {
        return 95;
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 50;
    }

    int values[] = { 4, 1, 3, 2 };
    qsort(values, sizeof(values) / sizeof(values[0]), sizeof(values[0]), cmp_ints);
    if (values[0] != 1 || values[3] != 4) {
        return 51;
    }

    errno = 0;
    FILE *missing = fopen("/cmd/definitely-missing-for-lpr", "rb");
    if (missing != NULL) {
        fclose(missing);
        return 52;
    }
    if (errno != ENOENT) {
        return 53;
    }
    perror("[lpr-ldmusl] missing-ok");

    printf("[lpr-ldmusl] OK libc=%s file=%s\n", library_path, argv[4]);
    return 0;
}
