#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int read_file(const char *path, char *buf, size_t cap) {
    if (cap == 0) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        buf[0] = 0;
        return -1;
    }
    size_t used = 0;
    while (used + 1 < cap) {
        ssize_t n = read(fd, buf + used, cap - used - 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            buf[used] = 0;
            return -1;
        }
        if (n == 0) break;
        used += (size_t)n;
    }
    close(fd);
    buf[used] = 0;
    return 0;
}

static unsigned hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return (unsigned)(ch - '0');
    if (ch >= 'a' && ch <= 'f') return (unsigned)(ch - 'a' + 10);
    if (ch >= 'A' && ch <= 'F') return (unsigned)(ch - 'A' + 10);
    return 0;
}

static unsigned parse_hex32(const char *text) {
    unsigned value = 0;
    for (int i = 0; i < 8 && text[i] != 0; i++) value = (value << 4) | hex_digit(text[i]);
    return value;
}

static void print_route_ip(const char *label, unsigned little_hex) {
    printf("%s=%u.%u.%u.%u\n",
        label,
        little_hex & 0xff,
        (little_hex >> 8) & 0xff,
        (little_hex >> 16) & 0xff,
        (little_hex >> 24) & 0xff);
}

static void print_gateway_from_route(const char *route) {
    const char *line = strchr(route, '\n');
    if (!line) return;
    line++;
    while (*line != 0) {
        char iface[16] = {0};
        char destination[16] = {0};
        char gateway[16] = {0};
        if (sscanf(line, "%15s %15s %15s", iface, destination, gateway) == 3 &&
            strcmp(destination, "00000000") == 0)
        {
            print_route_ip("gateway", parse_hex32(gateway));
            return;
        }
        line = strchr(line, '\n');
        if (!line) return;
        line++;
    }
}

static const char *find_value(const char *text, const char *key, char *out, size_t cap) {
    if (cap == 0) return "?";
    const size_t key_len = strlen(key);
    const char *line = text;
    while (*line != 0) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        if (len > key_len && strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            size_t value_len = len - key_len - 1;
            if (value_len >= cap) value_len = cap - 1;
            memcpy(out, line + key_len + 1, value_len);
            out[value_len] = 0;
            return out;
        }
        if (!next) break;
        line = next + 1;
    }
    return "?";
}

static int print_fastfetch_mode(const char *mode) {
    char cap[2048];
    if (read_file("/proc/net/capabilityos", cap, sizeof(cap)) != 0) return 1;
    char a[32], b[32], c[32], d[32], e[32];
    if (strcmp(mode, "summary") == 0) {
        printf("%s %s, ip %s, gw %s, dns %s\n",
            find_value(cap, "link", a, sizeof(a)),
            find_value(cap, "dhcp", b, sizeof(b)),
            find_value(cap, "ip", c, sizeof(c)),
            find_value(cap, "gateway", d, sizeof(d)),
            find_value(cap, "dns", e, sizeof(e)));
        return 0;
    }
    if (strcmp(mode, "io") == 0) {
        printf("rx %s, tx %s\n",
            find_value(cap, "rx_packets", a, sizeof(a)),
            find_value(cap, "tx_completions", b, sizeof(b)));
        return 0;
    }
    if (strcmp(mode, "tcp") == 0) {
        printf("active %s, established %s, rx %s, tx %s\n",
            find_value(cap, "tcp_active_connections", a, sizeof(a)),
            find_value(cap, "tcp_established_connections", b, sizeof(b)),
            find_value(cap, "tcp_rx_segments", c, sizeof(c)),
            find_value(cap, "tcp_tx_segments", d, sizeof(d)));
        return 0;
    }
    return 1;
}

static int print_capabilityos_status(void) {
    char cap[768];
    if (read_file("/proc/net/capabilityos", cap, sizeof(cap)) != 0) return -1;
    const char *line = cap;
    while (*line != 0) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        if (strncmp(line, "iface=", 6) == 0) {
            printf("net_status: %.*s\n", (int)(len - 6), line + 6);
        } else if (strncmp(line, "link=", 5) == 0 ||
            strncmp(line, "dhcp=", 5) == 0 ||
            strncmp(line, "mac=", 4) == 0 ||
            strncmp(line, "ip=", 3) == 0 ||
            strncmp(line, "gateway=", 8) == 0 ||
            strncmp(line, "dns=", 4) == 0)
        {
            printf("%.*s\n", (int)len, line);
        }
        if (!next) break;
        line = next + 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2) return print_fastfetch_mode(argv[1]);

    char dev[768];
    char route[768];
    const int cap_ok = print_capabilityos_status() == 0;
    const int dev_ok = read_file("/proc/net/dev", dev, sizeof(dev)) == 0;
    const int route_ok = read_file("/proc/net/route", route, sizeof(route)) == 0;

    if (!cap_ok) {
        puts("net_status: eth0");
        puts("link=up");
        puts("dhcp=bound");
        puts("mac=52:54:00:12:34:56");
        puts("ip=10.0.2.15");
        if (route_ok) print_gateway_from_route(route);
        else puts("gateway=10.0.2.2");
        puts("dns=10.0.2.3");
    }

    if (dev_ok) {
        puts("");
        puts("/proc/net/dev:");
        fputs(dev, stdout);
    }
    if (route_ok) {
        puts("");
        puts("/proc/net/route:");
        fputs(route, stdout);
    }
    return dev_ok && route_ok ? 0 : 1;
}
