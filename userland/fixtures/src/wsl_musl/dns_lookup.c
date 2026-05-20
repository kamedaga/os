#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

static unsigned parse_ipv4_text(const char *text) {
    unsigned parts[4] = {0, 0, 0, 0};
    unsigned part = 0;
    const char *p = text;
    while (*p && part < 4) {
        if (*p < '0' || *p > '9') return 0;
        unsigned value = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (unsigned)(*p - '0');
            if (value > 255) return 0;
            p++;
        }
        parts[part++] = value;
        if (part == 4) break;
        if (*p != '.') return 0;
        p++;
    }
    if (part != 4) return 0;
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

static unsigned read_dns_from_proc(void) {
    FILE *fp = fopen("/proc/net/capabilityos", "r");
    if (!fp) return parse_ipv4_text("10.0.2.3");
    char line[128];
    unsigned dns = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "dns=", 4) != 0) continue;
        char *end = strchr(line + 4, '\n');
        if (end) *end = 0;
        dns = parse_ipv4_text(line + 4);
        break;
    }
    fclose(fp);
    return dns ? dns : parse_ipv4_text("10.0.2.3");
}

static int append_qname(unsigned char *packet, int offset, int cap, const char *name) {
    const char *label = name;
    while (*label) {
        const char *dot = strchr(label, '.');
        int len = dot ? (int)(dot - label) : (int)strlen(label);
        if (len <= 0 || len > 63 || offset + 1 + len >= cap) return -1;
        packet[offset++] = (unsigned char)len;
        memcpy(packet + offset, label, (size_t)len);
        offset += len;
        if (!dot) break;
        label = dot + 1;
    }
    if (offset + 1 >= cap) return -1;
    packet[offset++] = 0;
    return offset;
}

static unsigned read_be32(const unsigned char *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

static unsigned short read_be16(const unsigned char *p) {
    return (unsigned short)(((unsigned short)p[0] << 8) | p[1]);
}

static int skip_name(const unsigned char *packet, int len, int offset) {
    int jumps = 0;
    while (offset < len && jumps < 16) {
        unsigned char c = packet[offset++];
        if (c == 0) return offset;
        if ((c & 0xc0) == 0xc0) return offset + 1 <= len ? offset + 1 : -1;
        if ((c & 0xc0) != 0 || offset + c > len) return -1;
        offset += c;
        jumps++;
    }
    return -1;
}

int main(int argc, char **argv) {
    const char *name = argc >= 2 ? argv[1] : "example.com";
    unsigned dns = argc >= 3 ? parse_ipv4_text(argv[2]) : read_dns_from_proc();
    if (!dns) {
        fprintf(stderr, "dns_lookup: invalid dns server\n");
        return 2;
    }

    unsigned char query[512];
    memset(query, 0, sizeof(query));
    query[0] = 0x12;
    query[1] = 0x34;
    query[2] = 0x01;
    query[5] = 0x01;
    int qlen = append_qname(query, 12, (int)sizeof(query), name);
    if (qlen < 0 || qlen + 4 > (int)sizeof(query)) {
        fprintf(stderr, "dns_lookup: invalid name\n");
        return 2;
    }
    query[qlen + 1] = 1;
    query[qlen + 3] = 1;
    qlen += 4;

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    dst.sin_addr.s_addr = htonl(dns);

    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        perror("connect");
        close(fd);
        return 1;
    }
    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    (void)getsockname(fd, (struct sockaddr *)&local, &local_len);
    int so_error = -1;
    socklen_t so_error_len = sizeof(so_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) != 0 || so_error != 0) {
        perror("getsockopt");
        close(fd);
        return 1;
    }

    if (write(fd, query, (size_t)qlen) < 0) {
        perror("write");
        close(fd);
        return 1;
    }
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 5000) <= 0 || (pfd.revents & POLLIN) == 0) {
        perror("poll");
        close(fd);
        return 1;
    }
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0 || !FD_ISSET(fd, &rfds)) {
        perror("select");
        close(fd);
        return 1;
    }

    unsigned char response[512];
    ssize_t n = recvfrom(fd, response, sizeof(response), 0, NULL, NULL);
    close(fd);
    if (n < 0) {
        perror("recvfrom");
        return 1;
    }
    if (n < 12 || response[0] != 0x12 || response[1] != 0x34) {
        fprintf(stderr, "dns_lookup: invalid response\n");
        return 1;
    }

    int ancount = read_be16(response + 6);
    int offset = 12;
    offset = skip_name(response, (int)n, offset);
    if (offset < 0 || offset + 4 > n) return 1;
    offset += 4;

    int printed = 0;
    printf("%s via %u.%u.%u.%u\n", name, (dns >> 24) & 255, (dns >> 16) & 255, (dns >> 8) & 255, dns & 255);
    for (int i = 0; i < ancount; i++) {
        offset = skip_name(response, (int)n, offset);
        if (offset < 0 || offset + 10 > n) break;
        unsigned type = read_be16(response + offset);
        unsigned cls = read_be16(response + offset + 2);
        unsigned ttl = read_be32(response + offset + 4);
        unsigned rdlen = read_be16(response + offset + 8);
        offset += 10;
        if (offset + (int)rdlen > n) break;
        if (type == 1 && cls == 1 && rdlen == 4) {
            unsigned ip = read_be32(response + offset);
            printf("A %u.%u.%u.%u ttl=%u\n", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, ttl);
            printed++;
        }
        offset += (int)rdlen;
    }
    if (!printed) {
        puts("no A records");
        return 1;
    }
    return 0;
}
