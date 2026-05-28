#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct item {
    const char *name;
    int score;
};

static int compare_items(const void *lhs, const void *rhs) {
    const struct item *a = (const struct item *)lhs;
    const struct item *b = (const struct item *)rhs;
    if (a->score != b->score) return b->score - a->score;
    return strcmp(a->name, b->name);
}

static unsigned long checksum(const char *text) {
    unsigned long hash = 5381;
    while (*text != '\0') {
        hash = ((hash << 5) + hash) ^ (unsigned char)*text;
        text++;
    }
    return hash;
}

static int write_report(const char *path, const struct item *items, size_t count) {
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return 1;
    }

    for (size_t i = 0; i < count; i++) {
        fprintf(fp, "%zu,%s,%d,%lu\n", i + 1, items[i].name, items[i].score, checksum(items[i].name));
    }

    if (fclose(fp) != 0) {
        fprintf(stderr, "close %s failed: %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

static int read_report(const char *path) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "read %s failed: %s\n", path, strerror(errno));
        return 1;
    }

    char line[128];
    puts("report:");
    while (fgets(line, sizeof(line), fp) != NULL) {
        fputs(line, stdout);
    }

    if (ferror(fp)) {
        fprintf(stderr, "read error on %s\n", path);
        fclose(fp);
        return 1;
    }

    fclose(fp);
    return 0;
}

int main(void) {
    struct item input[] = {
        { "compiler", 92 },
        { "linker", 88 },
        { "libc", 95 },
        { "filesystem", 84 },
        { "execve", 90 },
    };
    const size_t count = sizeof(input) / sizeof(input[0]);

    struct item *items = malloc(count * sizeof(items[0]));
    if (items == NULL) {
        fputs("malloc failed\n", stderr);
        return 1;
    }

    memcpy(items, input, count * sizeof(items[0]));
    qsort(items, count, sizeof(items[0]), compare_items);

    int total = 0;
    for (size_t i = 0; i < count; i++) total += items[i].score;

    printf("items=%zu average=%.2f best=%s\n", count, (double)total / (double)count, items[0].name);

    const char *path = "/tmp/clang-report.txt";
    int status = write_report(path, items, count);
    free(items);
    if (status != 0) return status;

    return read_report(path);
}
