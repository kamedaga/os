#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int copy_stream(FILE *in, FILE *out) {
    char buf[1024];
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), in);
        if (n != 0 && fwrite(buf, 1, n, out) != n) return 1;
        if (n < sizeof(buf)) {
            if (ferror(in)) return 1;
            return fflush(out) == 0 ? 0 : 1;
        }
    }
}

static int cmd_cat(int argc, char **argv) {
    if (argc <= 2) return copy_stream(stdin, stdout);
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) return 1;
        if (copy_stream(f, stdout) != 0) rc = 1;
        fclose(f);
    }
    return rc;
}

static int cmd_seq(int argc, char **argv) {
    if (argc != 4) return 1;
    char *end = NULL;
    long first = strtol(argv[2], &end, 10);
    if (!end || *end != 0) return 1;
    long last = strtol(argv[3], &end, 10);
    if (!end || *end != 0) return 1;
    const long step = first <= last ? 1 : -1;
    for (long v = first;; v += step) {
        if (printf("%ld\n", v) < 0) return 1;
        if (v == last) break;
    }
    return fflush(stdout) == 0 ? 0 : 1;
}

static int cmd_wc(int argc, char **argv) {
    int count_lines = 0;
    int count_bytes = 0;
    if (argc == 3 && strcmp(argv[2], "-l") == 0) count_lines = 1;
    else if (argc == 3 && strcmp(argv[2], "-c") == 0) count_bytes = 1;
    else return 1;

    unsigned long long lines = 0;
    unsigned long long bytes = 0;
    int ch;
    while ((ch = fgetc(stdin)) != EOF) {
        bytes++;
        if (ch == '\n') lines++;
    }
    if (ferror(stdin)) return 1;
    printf("%llu\n", count_lines ? lines : (count_bytes ? bytes : 0));
    return fflush(stdout) == 0 ? 0 : 1;
}

static int parse_count(const char *text, long *out) {
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (!end || *end != 0 || value < 0) return 0;
    *out = value;
    return 1;
}

static int cmd_head(int argc, char **argv) {
    long limit = 10;
    int byte_mode = 0;
    if (argc == 4 && strcmp(argv[2], "-n") == 0) {
        if (!parse_count(argv[3], &limit)) return 1;
    } else if (argc == 4 && strcmp(argv[2], "-c") == 0) {
        if (!parse_count(argv[3], &limit)) return 1;
        byte_mode = 1;
    } else {
        return 1;
    }

    int ch;
    long emitted = 0;
    while ((ch = fgetc(stdin)) != EOF) {
        if (fputc(ch, stdout) == EOF) return 1;
        if (byte_mode) {
            emitted++;
            if (emitted >= limit) break;
        } else if (ch == '\n') {
            emitted++;
            if (emitted >= limit) break;
        }
    }
    return fflush(stdout) == 0 ? 0 : 1;
}

static int cmd_tr(int argc, char **argv) {
    if (argc != 4 || strcmp(argv[2], "a-z") != 0 || strcmp(argv[3], "A-Z") != 0) return 1;
    int ch;
    while ((ch = fgetc(stdin)) != EOF) {
        if (fputc(toupper((unsigned char)ch), stdout) == EOF) return 1;
    }
    return fflush(stdout) == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    if (strcmp(argv[1], "cat") == 0) return cmd_cat(argc, argv);
    if (strcmp(argv[1], "seq") == 0) return cmd_seq(argc, argv);
    if (strcmp(argv[1], "wc") == 0) return cmd_wc(argc, argv);
    if (strcmp(argv[1], "head") == 0) return cmd_head(argc, argv);
    if (strcmp(argv[1], "tr") == 0) return cmd_tr(argc, argv);
    return 1;
}
