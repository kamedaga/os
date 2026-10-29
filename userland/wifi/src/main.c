/* SPDX-License-Identifier: MIT */
/* Linux/musl frontend; the backend owns supplicant and connection state. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <openssl/evp.h>

#ifdef WIFI_TEST_ROOT
#define OWNER_UID ((uid_t)WIFI_TEST_OWNER_UID)
#define LOCK_MS 150
#define RESPONSE_MS 350
#else
#define WIFI_TEST_ROOT ""
#define OWNER_UID ((uid_t)0)
#define LOCK_MS 5000
#define RESPONSE_MS 30000
#endif
#define RUNTIME_DIR WIFI_TEST_ROOT "/run/pachaos/wifi"
#define CONFIG_DIR WIFI_TEST_ROOT "/etc/pachaos"
#define CONTENT_MAX (64U * 1024U)

static volatile sig_atomic_t interrupted;
static struct termios saved_terminal;
static int terminal_changed;
static char password[64], config[2048];

static void wipe(void *memory, size_t size)
{
    volatile unsigned char *bytes = memory;
    while (size--) *bytes++ = 0;
}

static void on_signal(int signo) { interrupted = signo; }

static int restore_terminal(void)
{
    if (!terminal_changed) return 0;
    /* TCSETSF restores echo and discards unread password bytes in one
     * existing termios operation, including through PachaOS's TTY bridge. */
    int result;
    do { result = tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_terminal); }
    while (result < 0 && errno == EINTR);
    if (result == 0) terminal_changed = 0;
    return result < 0 ? -errno : 0;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int pause_retry(int64_t deadline)
{
    if (interrupted) return -EINTR;
    int64_t now = monotonic_ms();
    if (now < 0) return -errno;
    if (now >= deadline) return -ETIMEDOUT;
    struct timespec delay = {.tv_nsec = 20000000};
    if (nanosleep(&delay, NULL) < 0 && errno != EINTR) return -errno;
    return interrupted ? -EINTR : 0;
}

static int safe_stat(const struct stat *st, int directory, int private_directory,
    int sticky_runtime_parent)
{
    if (directory) {
        /* PachaOS mounts /run as root-owned sticky tmpfs. Its protected
         * children still require trusted ownership and non-writable modes. */
        int protected_parent = sticky_runtime_parent && st->st_uid == 0 &&
            (st->st_mode & S_ISVTX);
        if (!S_ISDIR(st->st_mode) || (st->st_uid != 0 && st->st_uid != OWNER_UID) ||
            ((st->st_mode & 0022) && !protected_parent)) return -EPERM;
        if (private_directory && (st->st_uid != OWNER_UID || (st->st_mode & 0777) != 0700))
            return -EPERM;
    } else if (!S_ISREG(st->st_mode) || st->st_uid != OWNER_UID ||
        (st->st_mode & 07777) != 0600 || st->st_nlink != 1) return -EPERM;
    return 0;
}

static int open_directory(const char *path, int private_directory)
{
    char copy[PATH_MAX];
    if (strlen(path) >= sizeof(copy)) return -ENAMETOOLONG;
    strcpy(copy, path);
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -errno;
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0 && errno == ENOENT) {
            if (mkdirat(fd, part, 0700) < 0 && errno != EEXIST) {
                int error = -errno; close(fd); return error;
            }
            next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        }
        if (next < 0) { int error = -errno; close(fd); return error; }
        close(fd);
        fd = next;
        struct stat st;
        if (fstat(fd, &st) < 0) { int error = -errno; close(fd); return error; }
        int runtime_parent = !strcmp(path, RUNTIME_DIR) && !strcmp(part, "run");
        int error = safe_stat(&st, 1, private_directory && (!save || !*save), runtime_parent);
        if (error) { close(fd); return error; }
    }
    return fd;
}

static int check_existing(int dir, const char *name)
{
    struct stat st;
    if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
        return errno == ENOENT ? 0 : -errno;
    return safe_stat(&st, 0, 0, 0);
}

static int atomic_write(int dir, const char *name, const char *id,
    const char *data, size_t size)
{
    char temporary[96];
    int error = check_existing(dir, name);
    if (error) return error;
    if (snprintf(temporary, sizeof(temporary), ".%s.%s.tmp", name, id) >= (int)sizeof(temporary))
        return -ENAMETOOLONG;
    int fd = openat(dir, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -errno;
    size_t used = 0;
    while (used < size) {
        if (interrupted) { error = -EINTR; break; }
        ssize_t count = write(fd, data + used, size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count < 0 ? -errno : -EIO; break; }
        used += (size_t)count;
    }
    if (!error && fsync(fd) < 0) error = -errno;
    if (close(fd) < 0 && !error) error = -errno;
    if (!error) error = check_existing(dir, name);
    if (!error && interrupted) error = -EINTR;
    if (!error && renameat(dir, temporary, dir, name) < 0) error = -errno;
    if (error) (void)unlinkat(dir, temporary, 0);
    else if (fsync(dir) < 0) error = -errno;
    return error;
}

static int read_file(int dir, const char *name, char **out, int require_fresh)
{
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -errno;
    struct stat st;
    int error = fstat(fd, &st) < 0 ? -errno : safe_stat(&st, 0, 0, 0);
    if (!error && require_fresh) {
        struct timespec now;
        if (clock_gettime(CLOCK_REALTIME, &now) < 0) error = -errno;
        else if (now.tv_sec < 0 || st.st_mtim.tv_nsec < 0 || st.st_mtim.tv_nsec >= 1000000000 ||
            st.st_mtim.tv_sec < now.tv_sec - 5 || st.st_mtim.tv_sec > now.tv_sec ||
            (st.st_mtim.tv_sec == now.tv_sec - 5 && st.st_mtim.tv_nsec < now.tv_nsec) ||
            (st.st_mtim.tv_sec == now.tv_sec && st.st_mtim.tv_nsec > now.tv_nsec))
            error = -ESTALE;
    }
    if (!error && (st.st_size < 0 || st.st_size > CONTENT_MAX)) error = -EFBIG;
    char *text = error ? NULL : malloc(CONTENT_MAX + 1);
    if (!error && !text) error = -ENOMEM;
    size_t used = 0;
    while (!error) {
        ssize_t count = read(fd, text + used, CONTENT_MAX + 1 - used);
        if (count < 0 && errno == EINTR && !interrupted) continue;
        if (count < 0) { error = -errno; break; }
        if (!count) break;
        used += (size_t)count;
        if (used > CONTENT_MAX) { error = -EFBIG; break; }
    }
    close(fd);
    if (!error && memchr(text, 0, used)) error = -EPROTO;
    if (error) { free(text); return error; }
    text[used] = 0;
    *out = text;
    return 0;
}

static int lock_runtime(int dir)
{
    int fd = openat(dir, "lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -errno;
    struct stat st;
    int error = fstat(fd, &st) < 0 ? -errno : safe_stat(&st, 0, 0, 0);
    int64_t start = monotonic_ms();
    if (start < 0 && !error) error = -errno;
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    while (!error && fcntl(fd, F_SETLK, &lock) < 0) {
        if (errno != EACCES && errno != EAGAIN && errno != EINTR) { error = -errno; break; }
        error = pause_retry(start + LOCK_MS);
    }
    if (error) { close(fd); return error; }
    /* Never unlink the lock: another CLI may already be waiting on its inode. */
    return fd;
}

static int read_session(int dir, char session[17])
{
    char *text = NULL;
    int error = read_file(dir, "session", &text, 0);
    if (error) return error;
    if (strlen(text) != 17 || text[16] != '\n') error = -EPROTO;
    for (unsigned i = 0; !error && i < 16; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            error = -EPROTO;
    if (!error) { memcpy(session, text, 16); session[16] = 0; }
    free(text);
    return error;
}

static int verify_session(int dir, const char *session)
{
    char current[17];
    int error = read_session(dir, current);
    return error ? error : strcmp(session, current) ? -ESTALE : 0;
}

static int read_password(size_t minimum)
{
    if (tcgetattr(STDIN_FILENO, &saved_terminal) < 0) return -errno;
    struct termios hidden = saved_terminal;
    hidden.c_lflag &= ~(ECHO | ECHONL);
    terminal_changed = 1;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &hidden) < 0) {
        int error = -errno;
        (void)restore_terminal();
        return error;
    }
    fputs("Password: ", stderr);
    fflush(stderr);
    size_t used = 0;
    int error = 0;
    for (;;) {
        if (interrupted) { error = -EINTR; break; }
        /* Bounded poll avoids the signal-before-read race without leaving
         * an uninterruptible hidden terminal or spinning for keyboard input. */
        struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
        int ready = poll(&input, 1, 50);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { error = -errno; break; }
        if (!ready) continue;
        unsigned char ch;
        ssize_t count = read(STDIN_FILENO, &ch, 1);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count < 0 ? -errno : -ENODATA; break; }
        if (ch == '\n') break;
        if (ch < 32 || ch == 127 || used == sizeof(password) - 1) {
            error = -EINVAL; break;
        }
        password[used++] = (char)ch;
    }
    int restore = restore_terminal();
    fputc('\n', stderr);
    if (!error) error = restore;
    if (!error && used < minimum) error = -EINVAL;
    if (error) wipe(password, sizeof(password));
    return error;
}

enum security { TRANSITION, WPA2, WPA3, OPEN };

static int build_config(const char *ssid, enum security security, const char *country)
{
    static const char hex[] = "0123456789abcdef";
    char encoded[65], escaped[4 * sizeof(password) + 1], psk[65] = {0};
    size_t size = strlen(ssid), used = 0;
    for (size_t i = 0; i < size; ++i) {
        encoded[2 * i] = hex[(unsigned char)ssid[i] >> 4];
        encoded[2 * i + 1] = hex[(unsigned char)ssid[i] & 15];
    }
    encoded[2 * size] = 0;
    for (size_t i = 0; password[i]; ++i) {
        unsigned char ch = (unsigned char)password[i];
        if (ch == '"' || ch == '\\') { escaped[used++] = '\\'; escaped[used++] = (char)ch; }
        else if (ch >= 127) {
            escaped[used++] = '\\'; escaped[used++] = 'x';
            escaped[used++] = hex[ch >> 4]; escaped[used++] = hex[ch & 15];
        } else escaped[used++] = (char)ch;
    }
    escaped[used] = 0;
    int error = 0;
    if (security == TRANSITION || security == WPA2) {
        unsigned char derived[32];
        if (PKCS5_PBKDF2_HMAC(password, (int)strlen(password),
                (const unsigned char *)ssid, (int)size, 4096, EVP_sha1(), 32, derived) != 1)
            error = -EIO;
        if (!error) for (size_t i = 0; i < sizeof(derived); ++i) {
            psk[2 * i] = hex[derived[i] >> 4]; psk[2 * i + 1] = hex[derived[i] & 15];
        }
        wipe(derived, sizeof(derived));
    }
    char secrets[512] = {0};
    if (!error && security == TRANSITION)
        snprintf(secrets, sizeof(secrets), "\tpsk=%s\n\tsae_password=P\"%s\"\n", psk, escaped);
    else if (!error && security == WPA2)
        snprintf(secrets, sizeof(secrets), "\tpsk=%s\n", psk);
    else if (!error && security == WPA3)
        snprintf(secrets, sizeof(secrets), "\tsae_password=P\"%s\"\n", escaped);
    if (!error) {
        int count = snprintf(config, sizeof(config),
            "update_config=0\n%s%s%snetwork={\n\tssid=%s\n\tkey_mgmt=%s\n\tieee80211w=%u\n%s}\n",
            country ? "country=" : "", country ? country : "", country ? "\n" : "",
            encoded, security == OPEN ? "NONE" : security == WPA2 ? "WPA-PSK" :
            security == WPA3 ? "SAE" : "WPA-PSK SAE",
            security == WPA3 ? 2u : security == OPEN || security == WPA2 ? 0u : 1u, secrets);
        if (count < 0 || count >= (int)sizeof(config)) error = -EOVERFLOW;
    }
    wipe(escaped, sizeof(escaped)); wipe(psk, sizeof(psk)); wipe(secrets, sizeof(secrets));
    return error;
}

static int request(int dir, const char *session, const char *id, const char *command)
{
    char body[64];
    int size = snprintf(body, sizeof(body), "%s %s\n", id, command);
    int error = atomic_write(dir, "request", id, body, (size_t)size);
    if (error) return error;
    int64_t start = monotonic_ms();
    if (start < 0) return -errno;
    for (;;) {
        if (interrupted) return -EINTR;
        error = verify_session(dir, session);
        if (error) return error;
        char *response = NULL;
        error = read_file(dir, "response", &response, 0);
        if (!error) {
            char *newline = strchr(response, '\n');
            if (!newline || newline - response < 34 || response[32] != ' ') error = -EPROTO;
            else {
                for (unsigned i = 0; i < 32; ++i)
                    if (!((response[i] >= '0' && response[i] <= '9') ||
                        (response[i] >= 'a' && response[i] <= 'f'))) error = -EPROTO;
                if (!error && !memcmp(response, id, 32)) {
                    char *end;
                    errno = 0;
                    long status = strtol(response + 33, &end, 10);
                    if (errno || end != newline || end == response + 33 || status < -4095 || status > 0)
                        error = -EPROTO;
                    else if (status) error = (int)status;
                    else {
                        error = verify_session(dir, session);
                        if (error) { free(response); return error; }
                        if (strcmp(command, "scan") && newline[1]) fputs(newline + 1, stdout);
                        free(response);
                        return 0;
                    }
                }
            }
            free(response);
            if (error) return error;
        } else if (error != -ENOENT) return error;
        error = pause_retry(start + RESPONSE_MS);
        if (error) return error;
    }
}

static void usage(void)
{
    fputs("Usage: wifi status | scan | disconnect\n"
        "       wifi connect SSID [--wpa2|--wpa3|--open] [--country XX]\n"
        "Password is read from a terminal; default: WPA2/WPA3 transition.\n", stderr);
}

int main(int argc, char **argv)
{
    umask(077);
    int result = 0, dir = -1, lock = -1, config_dir = -1;
    enum security security = TRANSITION;
    const char *ssid = NULL, *command = argc > 1 ? argv[1] : "";
    char country[3] = {0};
    char session[17];
    if (argc == 2 && (!strcmp(command, "--help") || !strcmp(command, "help"))) { usage(); return 0; }
    if (!strcmp(command, "connect")) {
        if (argc < 3 || !argv[2][0] || strlen(argv[2]) > 32) { usage(); return 2; }
        ssid = argv[2];
        int security_set = 0;
        for (int i = 3; i < argc; ++i) {
            enum security mode;
            if (!strcmp(argv[i], "--country")) {
                if (++i == argc || country[0] || strlen(argv[i]) != 2) { usage(); return 2; }
                for (unsigned j = 0; j < 2; ++j) {
                    unsigned char ch = (unsigned char)argv[i][j];
                    if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
                    if (ch < 'A' || ch > 'Z') { usage(); return 2; }
                    country[j] = (char)ch;
                }
                continue;
            }
            if (!strcmp(argv[i], "--wpa2")) mode = WPA2;
            else if (!strcmp(argv[i], "--wpa3")) mode = WPA3;
            else if (!strcmp(argv[i], "--open")) mode = OPEN;
            else { usage(); return 2; }
            if (security_set++) { usage(); return 2; }
            security = mode;
        }
    } else if (argc != 2 || (strcmp(command, "scan") && strcmp(command, "status") && strcmp(command, "disconnect"))) {
        usage(); return 2;
    }
    if (geteuid() != OWNER_UID) { fputs("wifi: root privileges required\n", stderr); return 1; }
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP, SIGPIPE,
        SIGALRM, SIGUSR1, SIGUSR2};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i)
        if (sigaction(signals[i], &action, NULL) < 0) { result = -errno; goto done; }
    dir = open_directory(RUNTIME_DIR, 1);
    if (dir < 0) { result = dir; dir = -1; goto done; }
    result = read_session(dir, session);
    if (result) goto done;
    if (ssid && security != OPEN) {
        result = read_password(security == WPA3 ? 1 : 8);
        if (result) goto done;
    }
    lock = lock_runtime(dir);
    if (lock < 0) { result = lock; lock = -1; goto done; }
    result = verify_session(dir, session);
    if (result) goto done;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) { result = -errno; goto done; }
    char id[33];
    snprintf(id, sizeof(id), "%s%016" PRIx64, session,
        (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec);
    if (ssid) {
        config_dir = open_directory(CONFIG_DIR, 0);
        if (config_dir < 0) { result = config_dir; config_dir = -1; goto done; }
        result = build_config(ssid, security, country[0] ? country : NULL);
        wipe(password, sizeof(password));
        if (!result) result = atomic_write(config_dir, "wifi.conf", id, config, strlen(config));
        wipe(config, sizeof(config));
        if (result) goto done;
    }
    if (strcmp(command, "status")) result = request(dir, session, id, command);
    if (!result && (!strcmp(command, "status") || !strcmp(command, "scan"))) {
        char *text = NULL;
        result = read_file(dir, !strcmp(command, "status") ? "status" : "scan-results", &text,
            !strcmp(command, "status"));
        if (!result) result = verify_session(dir, session);
        if (!result) fputs(text, stdout);
        free(text);
    }
done:
    { int restored = restore_terminal(); if (!result) result = restored; }
    wipe(password, sizeof(password)); wipe(config, sizeof(config));
    if (config_dir >= 0) close(config_dir);
    if (lock >= 0) close(lock);
    if (dir >= 0) close(dir);
    if (result) fprintf(stderr, "wifi: %s%s\n", strerror(-result),
        result == -ETIMEDOUT ? " (control deadline expired; no success confirmed)" : "");
    return interrupted ? 128 + interrupted : result ? 1 : 0;
}
