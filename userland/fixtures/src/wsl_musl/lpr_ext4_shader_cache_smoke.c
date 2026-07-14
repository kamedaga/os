#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    DEFAULT_PARTS = 8,
    MAX_PARTS = 50,
    ANONYMOUS_FILE_BYTES = 1664,
};

static const char cache_root[] = "/home/.cache/ext4_shader_cache_smoke";

struct __attribute__((packed)) cache_header {
    char magic[8];
    uint32_t version;
    uint64_t uuid;
};

static int parse_parts(const char *text)
{
    char *end = NULL;
    errno = 0;
    const long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value <= 0 || value > MAX_PARTS) {
        return -1;
    }
    return (int)value;
}

static int mkdir_allow_existing(const char *path, mode_t mode)
{
    if (mkdir(path, mode) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static int initialize_db_file(const char *path, uint64_t uuid)
{
    const int fd = open(path, O_CREAT | O_CLOEXEC | O_RDWR, 0644);
    if (fd < 0) {
        return -1;
    }

    const struct cache_header header = {
        .magic = {'M', 'E', 'S', 'A', '_', 'D', 'B', '\0'},
        .version = 1,
        .uuid = uuid,
    };
    int status = 0;
    if (ftruncate(fd, (off_t)sizeof(header)) != 0) {
        status = -2;
    } else if (lseek(fd, 0, SEEK_SET) != 0 ||
        write(fd, &header, sizeof(header)) != (ssize_t)sizeof(header))
    {
        status = -3;
    } else if (fsync(fd) != 0) {
        status = -4;
    }
    const int saved_errno = errno;
    if (close(fd) != 0 && status == 0) {
        status = -5;
    }
    errno = saved_errno;
    return status;
}

static int exercise_unlinked_file(void)
{
    static const char path[] =
        "/home/.cache/ext4_shader_cache_smoke/mesa-shared-format-table-XXXXXX";
    char temporary_path[sizeof(path)];
    memcpy(temporary_path, path, sizeof(path));

    const int fd = mkstemp(temporary_path);
    if (fd < 0) {
        return -1;
    }

    int status = 0;
    if (unlink(temporary_path) != 0) {
        status = -2;
    } else if (ftruncate(fd, ANONYMOUS_FILE_BYTES) != 0) {
        status = -3;
    } else {
        uint8_t payload[ANONYMOUS_FILE_BYTES];
        for (size_t byte = 0; byte < sizeof(payload); ++byte) {
            payload[byte] = (uint8_t)(byte * 37u + 11u);
        }
        if (lseek(fd, 0, SEEK_SET) != 0 ||
            write(fd, payload, sizeof(payload)) != (ssize_t)sizeof(payload))
        {
            status = -4;
        } else if (fsync(fd) != 0) {
            status = -5;
        }
    }

    const int saved_errno = errno;
    if (close(fd) != 0 && status == 0) {
        status = -6;
    }
    errno = saved_errno;
    return status;
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "usage: %s [parts]\n", argv[0]);
        return 2;
    }
    const int parts = argc == 2 ? parse_parts(argv[1]) : DEFAULT_PARTS;
    if (parts < 0) {
        fprintf(stderr, "EXT4_SHADER_CACHE_ARGUMENTS=FAIL\n");
        return 2;
    }

    if (mkdir_allow_existing("/home/.cache", 0700) != 0 ||
        mkdir_allow_existing(cache_root, 0755) != 0)
    {
        printf("EXT4_SHADER_CACHE_SETUP=FAIL errno=%d\n", errno);
        return 1;
    }

    printf("EXT4_SHADER_CACHE_START parts=%d header_bytes=%zu\n",
        parts,
        sizeof(struct cache_header));
    fflush(stdout);

    for (int part = 0; part < parts; ++part) {
        char part_path[160];
        char cache_path[192];
        char index_path[192];
        if (snprintf(part_path, sizeof(part_path), "%s/part%d", cache_root, part) >=
                (int)sizeof(part_path) ||
            snprintf(cache_path, sizeof(cache_path), "%s/mesa_cache.db", part_path) >=
                (int)sizeof(cache_path) ||
            snprintf(index_path, sizeof(index_path), "%s/mesa_cache.idx", part_path) >=
                (int)sizeof(index_path))
        {
            printf("EXT4_SHADER_CACHE_PATH=FAIL part=%d\n", part);
            return 1;
        }
        if (mkdir(part_path, 0755) != 0) {
            printf("EXT4_SHADER_CACHE_MKDIR=FAIL part=%d errno=%d\n", part, errno);
            return 1;
        }

        const uint64_t uuid = UINT64_C(0x5348445200000000) | (uint32_t)(part + 1);
        int status = initialize_db_file(cache_path, uuid);
        if (status != 0) {
            printf("EXT4_SHADER_CACHE_FILE=FAIL part=%d file=db stage=%d errno=%d\n",
                part,
                status,
                errno);
            return 1;
        }
        status = initialize_db_file(index_path, uuid);
        if (status != 0) {
            printf("EXT4_SHADER_CACHE_FILE=FAIL part=%d file=idx stage=%d errno=%d\n",
                part,
                status,
                errno);
            return 1;
        }

        printf("EXT4_SHADER_CACHE_PART=%d status=OK backend_pwrite_offset=0 bytes=%zu files=2\n",
            part,
            sizeof(struct cache_header));
        fflush(stdout);
    }

    const int unlink_status = exercise_unlinked_file();
    if (unlink_status != 0) {
        printf("EXT4_SHADER_CACHE_UNLINK=FAIL stage=%d errno=%d\n", unlink_status, errno);
        return 1;
    }
    printf("EXT4_SHADER_CACHE_UNLINK=OK bytes=%d order=open-unlink-ftruncate-write-fsync-close\n",
        ANONYMOUS_FILE_BYTES);

    sync();
    printf("EXT4_SHADER_CACHE_DONE parts=%d completed=%d failures=0\n", parts, parts);
    return 0;
}
