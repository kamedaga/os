#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Run each operation separately: a broken revocation may kill the process. */
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 3) return 2;
    char path[256];
    if (snprintf(path, sizeof(path), "%s/mapping-lifetime-XXXXXX", argv[1]) >= (int)sizeof(path)) return 2;
    int fd = mkstemp(path);
    if (fd < 0 || ftruncate(fd, 4096)) { perror("create"); return 1; }
    unsigned char *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); return 1; }
    map[0] = 42;
    printf("MAPPING_LIFETIME op=%s addr=%p before=42\n", argv[2], (void *)map);
    int status;
    if (!strcmp(argv[2], "unlink")) status = unlink(path);
    else if (!strcmp(argv[2], "rename-over")) {
        char replacement[280];
        snprintf(replacement, sizeof(replacement), "%s-replacement", path);
        int other = open(replacement, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (other < 0 || write(other, "replacement", 11) != 11 || close(other)) return 1;
        status = rename(replacement, path);
    }
    else if (!strcmp(argv[2], "grow")) status = ftruncate(fd, 8192);
    else if (!strcmp(argv[2], "same-size")) status = ftruncate(fd, 4096);
    else if (!strcmp(argv[2], "shrink")) status = ftruncate(fd, 2048);
    else if (!strcmp(argv[2], "write-grow")) status = pwrite(fd, "x", 1, 8191) == 1 ? 0 : -1;
    else return 2;
    if (status) { perror(argv[2]); return 1; }
    printf("MAPPING_LIFETIME op=%s mutation=OK reading\n", argv[2]);
    if (map[0] != 42) { puts("MAPPING_LIFETIME wrong-data"); return 1; }
    map[1] = 77;
    unsigned char byte = 0;
    if (pread(fd, &byte, 1, 1) != 1 || byte != 77) {
        puts("MAPPING_LIFETIME incoherent-read"); return 1;
    }
    if (munmap(map, 4096) || close(fd)) return 1;
    if (strcmp(argv[2], "unlink") && unlink(path)) return 1;
    puts("MAPPING_LIFETIME=OK");
    return 0;
}
