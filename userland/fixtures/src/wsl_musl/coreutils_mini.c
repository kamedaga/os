#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef COREUTILS_MINI_SINGLE
static const char *mini_basename(const char *path) {
    const char *name = path;
    if (path == NULL) return "";
    for (const char *p = path; *p != 0; p++) {
        if (*p == '/') name = p + 1;
    }
    return name;
}
#endif

static int mini_error(const char *tool, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: ", tool);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return 1;
}

static int copy_fd(const char *tool, int fd) {
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) return 0;
        if (n < 0) return mini_error(tool, "read: %s", strerror(errno));
        char *p = buf;
        while (n > 0) {
            ssize_t w = write(STDOUT_FILENO, p, (size_t)n);
            if (w < 0) return mini_error(tool, "write: %s", strerror(errno));
            p += w;
            n -= w;
        }
    }
}

static int cmd_cat(const char *tool, int argc, char **argv) {
    if (argc == 1) return copy_fd(tool, STDIN_FILENO);
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            if (copy_fd(tool, STDIN_FILENO) != 0) rc = 1;
            continue;
        }
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            mini_error(tool, "%s: %s", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (copy_fd(tool, fd) != 0) rc = 1;
        close(fd);
    }
    return rc;
}

static int cmd_echo(const char *tool, int argc, char **argv) {
    (void)tool;
    int newline = 1;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-n") == 0) {
        newline = 0;
        i++;
    }
    for (; i < argc; i++) {
        if (i > (newline ? 1 : 2)) putchar(' ');
        fputs(argv[i], stdout);
    }
    if (newline) putchar('\n');
    return ferror(stdout) ? 1 : 0;
}

static void print_escaped(const char *s) {
    for (; *s != 0; s++) {
        if (*s != '\\') {
            putchar((unsigned char)*s);
            continue;
        }
        s++;
        if (*s == 0) {
            putchar('\\');
            break;
        }
        switch (*s) {
        case 'n': putchar('\n'); break;
        case 'r': putchar('\r'); break;
        case 't': putchar('\t'); break;
        case '\\': putchar('\\'); break;
        default: putchar((unsigned char)*s); break;
        }
    }
}

static int cmd_printf(const char *tool, int argc, char **argv) {
    (void)tool;
    if (argc < 2) return 0;
    print_escaped(argv[1]);
    return ferror(stdout) ? 1 : 0;
}

static int cmd_true(const char *tool, int argc, char **argv) {
    (void)tool; (void)argc; (void)argv;
    return 0;
}

static int cmd_false(const char *tool, int argc, char **argv) {
    (void)tool; (void)argc; (void)argv;
    return 1;
}

static int cmd_coreutils(const char *tool, int argc, char **argv) {
    (void)tool;
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        puts("coreutils-mini (PachaOS)");
        return 0;
    }
    puts("coreutils-mini commands:");
    puts("[ cat coreutils echo false grep head ls mkdir printf pwd rm rmdir sleep sync tail test touch true wc yes");
    return 0;
}

static int cmd_pwd(const char *tool, int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[4096];
    if (getcwd(buf, sizeof(buf)) == NULL) return mini_error(tool, "getcwd: %s", strerror(errno));
    puts(buf);
    return 0;
}

static int cmd_ls_one(const char *tool, const char *path, int multi) {
    struct stat st;
    if (stat(path, &st) != 0) return mini_error(tool, "%s: %s", path, strerror(errno));
    if (!S_ISDIR(st.st_mode)) {
        puts(path);
        return 0;
    }
    DIR *dir = opendir(path);
    if (dir == NULL) return mini_error(tool, "%s: %s", path, strerror(errno));
    if (multi) printf("%s:\n", path);
    struct dirent *ent;
    int first = 1;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!first) putchar('\n');
        fputs(ent->d_name, stdout);
        first = 0;
    }
    if (!first) putchar('\n');
    closedir(dir);
    return 0;
}

static int cmd_ls(const char *tool, int argc, char **argv) {
    int rc = 0;
    int paths = 0;
    for (int i = 1; i < argc; i++) if (argv[i][0] != '-') paths++;
    if (paths == 0) return cmd_ls_one(tool, ".", 0);
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        if (cmd_ls_one(tool, argv[i], paths > 1) != 0) rc = 1;
    }
    return rc;
}

static int wc_fd(const char *tool, int fd, uint64_t *lines, uint64_t *bytes) {
    char buf[8192];
    *lines = 0;
    *bytes = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) return 0;
        if (n < 0) return mini_error(tool, "read: %s", strerror(errno));
        *bytes += (uint64_t)n;
        for (ssize_t i = 0; i < n; i++) if (buf[i] == '\n') *lines += 1;
    }
}

static int cmd_wc(const char *tool, int argc, char **argv) {
    int count_bytes = 0;
    int count_lines = 0;
    int first_file = 1;
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) count_bytes = 1;
        else if (strcmp(argv[i], "-l") == 0) count_lines = 1;
    }
    if (!count_bytes && !count_lines) count_lines = 1;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        first_file = 0;
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            mini_error(tool, "%s: %s", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        uint64_t lines = 0, bytes = 0;
        if (wc_fd(tool, fd, &lines, &bytes) == 0) {
            printf("%llu", (unsigned long long)(count_bytes ? bytes : lines));
            printf(" %s\n", argv[i]);
        } else {
            rc = 1;
        }
        close(fd);
    }
    if (first_file) {
        uint64_t lines = 0, bytes = 0;
        if (wc_fd(tool, STDIN_FILENO, &lines, &bytes) != 0) return 1;
        printf("%llu\n", (unsigned long long)(count_bytes ? bytes : lines));
    }
    return rc;
}

static long parse_n_arg(int argc, char **argv, long fallback, int *first_path) {
    *first_path = 1;
    if (argc >= 3 && strcmp(argv[1], "-n") == 0) {
        *first_path = 3;
        long n = strtol(argv[2], NULL, 10);
        return n < 0 ? 0 : n;
    }
    return fallback;
}

static int head_fd(const char *tool, int fd, long max_lines) {
    char ch;
    long lines = 0;
    while (lines < max_lines) {
        ssize_t n = read(fd, &ch, 1);
        if (n == 0) return 0;
        if (n < 0) return mini_error(tool, "read: %s", strerror(errno));
        if (write(STDOUT_FILENO, &ch, 1) != 1) return 1;
        if (ch == '\n') lines++;
    }
    return 0;
}

static int cmd_head(const char *tool, int argc, char **argv) {
    int first = 1;
    long n = parse_n_arg(argc, argv, 10, &first);
    if (first >= argc) return head_fd(tool, STDIN_FILENO, n);
    int rc = 0;
    for (int i = first; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            mini_error(tool, "%s: %s", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (head_fd(tool, fd, n) != 0) rc = 1;
        close(fd);
    }
    return rc;
}

static int read_all_fd(const char *tool, int fd, char **out, size_t *out_len) {
    size_t cap = 8192;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) return mini_error(tool, "malloc");
    for (;;) {
        if (len == cap) {
            cap *= 2;
            char *next = realloc(buf, cap);
            if (next == NULL) {
                free(buf);
                return mini_error(tool, "realloc");
            }
            buf = next;
        }
        ssize_t n = read(fd, buf + len, cap - len);
        if (n == 0) {
            *out = buf;
            *out_len = len;
            return 0;
        }
        if (n < 0) {
            free(buf);
            return mini_error(tool, "read: %s", strerror(errno));
        }
        len += (size_t)n;
    }
}

static int tail_buffer(const char *buf, size_t len, long max_lines) {
    size_t start = len;
    long lines = 0;
    while (start > 0 && lines <= max_lines) {
        start--;
        if (buf[start] == '\n' && start + 1 < len) lines++;
        if (lines == max_lines) {
            start++;
            break;
        }
    }
    if (lines < max_lines) start = 0;
    while (start < len) {
        ssize_t w = write(STDOUT_FILENO, buf + start, len - start);
        if (w <= 0) return 1;
        start += (size_t)w;
    }
    return 0;
}

static int cmd_tail(const char *tool, int argc, char **argv) {
    int first = 1;
    long n = parse_n_arg(argc, argv, 10, &first);
    int fd = STDIN_FILENO;
    if (first < argc) {
        fd = open(argv[first], O_RDONLY);
        if (fd < 0) return mini_error(tool, "%s: %s", argv[first], strerror(errno));
    }
    char *buf = NULL;
    size_t len = 0;
    int rc = read_all_fd(tool, fd, &buf, &len);
    if (fd != STDIN_FILENO) close(fd);
    if (rc != 0) return rc;
    rc = tail_buffer(buf, len, n);
    free(buf);
    return rc;
}

static int cmd_grep(const char *tool, int argc, char **argv) {
    int quiet = 0;
    int arg = 1;
    if (arg < argc && strcmp(argv[arg], "-q") == 0) {
        quiet = 1;
        arg++;
    }
    if (arg >= argc) return mini_error(tool, "missing pattern");
    const char *pat = argv[arg++];
    FILE *in = stdin;
    if (arg < argc) {
        in = fopen(argv[arg], "r");
        if (in == NULL) return mini_error(tool, "%s: %s", argv[arg], strerror(errno));
    }
    char *line = NULL;
    size_t cap = 0;
    int matched = 0;
    while (getline(&line, &cap, in) >= 0) {
        if (strstr(line, pat) != NULL) {
            matched = 1;
            if (!quiet) fputs(line, stdout);
        }
    }
    free(line);
    if (in != stdin) fclose(in);
    return matched ? 0 : 1;
}

static int cmd_yes(const char *tool, int argc, char **argv) {
    (void)tool;
    signal(SIGPIPE, SIG_DFL);
    const char *word = argc > 1 ? argv[1] : "y";
    for (;;) {
        if (printf("%s\n", word) < 0) return 1;
    }
}

static int cmd_mkdir(const char *tool, int argc, char **argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        if (mkdir(argv[i], 0777) != 0 && errno != EEXIST) {
            mini_error(tool, "%s: %s", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

static int cmd_rm(const char *tool, int argc, char **argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        if (unlink(argv[i]) != 0 && rmdir(argv[i]) != 0) {
            mini_error(tool, "%s: %s", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

static int cmd_rmdir(const char *tool, int argc, char **argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        if (rmdir(argv[i]) != 0) {
            mini_error(tool, "%s: %s", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

static int cmd_touch(const char *tool, int argc, char **argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0666);
        if (fd < 0) {
            mini_error(tool, "%s: %s", argv[i], strerror(errno));
            rc = 1;
        } else {
            close(fd);
        }
    }
    return rc;
}

static int cmd_sleep(const char *tool, int argc, char **argv) {
    (void)tool;
    unsigned sec = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 0;
    sleep(sec);
    return 0;
}

static int cmd_test(const char *tool, int argc, char **argv) {
    (void)tool;
    if (argc > 1 && strcmp(argv[argc - 1], "]") == 0) argc--;
    if (argc == 2) return argv[1][0] != 0 ? 0 : 1;
    if (argc == 3) {
        struct stat st;
        if (strcmp(argv[1], "-e") == 0) return stat(argv[2], &st) == 0 ? 0 : 1;
        if (strcmp(argv[1], "-f") == 0) return stat(argv[2], &st) == 0 && S_ISREG(st.st_mode) ? 0 : 1;
        if (strcmp(argv[1], "-d") == 0) return stat(argv[2], &st) == 0 && S_ISDIR(st.st_mode) ? 0 : 1;
    }
    if (argc == 4 && strcmp(argv[2], "=") == 0) return strcmp(argv[1], argv[3]) == 0 ? 0 : 1;
    return 1;
}

static int cmd_sync(const char *tool, int argc, char **argv) {
    (void)tool; (void)argc; (void)argv;
    if (syscall(SYS_sync) != 0) {
        return mini_error(tool, "sync: %s", strerror(errno));
    }
    return 0;
}

typedef int (*mini_main_t)(const char *, int, char **);

typedef struct mini_tool {
    const char *name;
    mini_main_t main;
} mini_tool_t;

#ifndef COREUTILS_MINI_SINGLE
static const mini_tool_t tools[] = {
    {"[", cmd_test},
    {"cat", cmd_cat},
    {"coreutils", cmd_coreutils},
    {"echo", cmd_echo},
    {"false", cmd_false},
    {"grep", cmd_grep},
    {"head", cmd_head},
    {"ls", cmd_ls},
    {"mkdir", cmd_mkdir},
    {"printf", cmd_printf},
    {"pwd", cmd_pwd},
    {"rm", cmd_rm},
    {"rmdir", cmd_rmdir},
    {"sleep", cmd_sleep},
    {"sync", cmd_sync},
    {"tail", cmd_tail},
    {"test", cmd_test},
    {"touch", cmd_touch},
    {"true", cmd_true},
    {"wc", cmd_wc},
    {"yes", cmd_yes},
};
#endif

int main(int argc, char **argv) {
#ifdef COREUTILS_MINI_SINGLE
#if defined(COREUTILS_MINI_APP_LBRACKET)
    return cmd_test("[", argc, argv);
#elif defined(COREUTILS_MINI_APP_CAT)
    return cmd_cat("cat", argc, argv);
#elif defined(COREUTILS_MINI_APP_COREUTILS)
    return cmd_coreutils("coreutils", argc, argv);
#elif defined(COREUTILS_MINI_APP_ECHO)
    return cmd_echo("echo", argc, argv);
#elif defined(COREUTILS_MINI_APP_FALSE)
    return cmd_false("false", argc, argv);
#elif defined(COREUTILS_MINI_APP_GREP)
    return cmd_grep("grep", argc, argv);
#elif defined(COREUTILS_MINI_APP_HEAD)
    return cmd_head("head", argc, argv);
#elif defined(COREUTILS_MINI_APP_LS)
    return cmd_ls("ls", argc, argv);
#elif defined(COREUTILS_MINI_APP_MKDIR)
    return cmd_mkdir("mkdir", argc, argv);
#elif defined(COREUTILS_MINI_APP_PRINTF)
    return cmd_printf("printf", argc, argv);
#elif defined(COREUTILS_MINI_APP_PWD)
    return cmd_pwd("pwd", argc, argv);
#elif defined(COREUTILS_MINI_APP_RM)
    return cmd_rm("rm", argc, argv);
#elif defined(COREUTILS_MINI_APP_RMDIR)
    return cmd_rmdir("rmdir", argc, argv);
#elif defined(COREUTILS_MINI_APP_SLEEP)
    return cmd_sleep("sleep", argc, argv);
#elif defined(COREUTILS_MINI_APP_SYNC)
    return cmd_sync("sync", argc, argv);
#elif defined(COREUTILS_MINI_APP_TAIL)
    return cmd_tail("tail", argc, argv);
#elif defined(COREUTILS_MINI_APP_TEST)
    return cmd_test("test", argc, argv);
#elif defined(COREUTILS_MINI_APP_TOUCH)
    return cmd_touch("touch", argc, argv);
#elif defined(COREUTILS_MINI_APP_TRUE)
    return cmd_true("true", argc, argv);
#elif defined(COREUTILS_MINI_APP_WC)
    return cmd_wc("wc", argc, argv);
#elif defined(COREUTILS_MINI_APP_YES)
    return cmd_yes("yes", argc, argv);
#else
#error "COREUTILS_MINI_SINGLE requires a COREUTILS_MINI_APP_* macro"
#endif
#else
    const char *tool = argc > 0 ? mini_basename(argv[0]) : "";
    if (strncmp(tool, "coreutils-mini", 14) == 0 || strcmp(tool, "coreutils") == 0) {
        if (argc < 2) return mini_error(tool, "missing tool name");
        tool = argv[1];
        argc--;
        argv++;
    }
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        if (strcmp(tool, tools[i].name) == 0) return tools[i].main(tool, argc, argv);
    }
    return mini_error("coreutils-mini", "unsupported tool: %s", tool);
#endif
}
