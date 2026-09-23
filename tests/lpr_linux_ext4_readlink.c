#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char directory[] = "/root/readlink-test-XXXXXX";
    assert(mkdtemp(directory));
    assert(chdir(directory) == 0);
    for (int length = 1; length <= 2; ++length) {
        const char *file = length == 1 ? "a" : "aa";
        int fd = open(file, O_CREAT | O_EXCL | O_WRONLY, 0600);
        assert(fd >= 0 && write(fd, "OK", 2) == 2 && close(fd) == 0);
    }
    const size_t lengths[] = {1, 59, 60, 66, 255};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        char target[256], name[32], output[256];
        size_t base_length = lengths[i] % 2 ? 1 : 2;
        for (size_t j = 0; j < lengths[i] - base_length; j += 2) {
            target[j] = '.';
            target[j + 1] = '/';
        }
        memset(target + lengths[i] - base_length, 'a', base_length);
        target[lengths[i]] = 0;
        snprintf(name, sizeof(name), "link-%zu", lengths[i]);
        assert(symlink(target, name) == 0);
        for (int repeat = 0; repeat < 128; ++repeat) {
            memset(output, '?', sizeof(output));
            assert(readlink(name, output, sizeof(output)) == (ssize_t)lengths[i]);
            assert(memcmp(output, target, lengths[i]) == 0 && output[lengths[i]] == '?');
            size_t truncated = lengths[i] < 3 ? lengths[i] : 3;
            assert(readlink(name, output, 3) == (ssize_t)truncated);
            assert(memcmp(output, target, truncated) == 0);
            int fd = open(name, O_RDONLY);
            assert(fd >= 0 && read(fd, output, 2) == 2);
            assert(memcmp(output, "OK", 2) == 0 && close(fd) == 0);
        }
        printf("ext4 readlink length=%zu read/truncate/follow OK\n", lengths[i]);
    }
    puts("EXT4_READLINK=OK");
    return 0;
}
