#include "pacha/capsule.h"
#include "pacha/syscall.h"

typedef unsigned long long u64;

static u64 cstr_len(const char *s) {
    u64 n = 0;
    while (s[n] != 0) n++;
    return n;
}

static void log_text(const char *s) {
    (void)pacha_syscall2(1, (u64)s, cstr_len(s));
}

static void log_hex(const char *prefix, u64 value) {
    static const char digits[] = "0123456789abcdef";
    char buf[96];
    u64 n = 0;
    while (prefix[n] != 0 && n + 19 < sizeof(buf)) {
        buf[n] = prefix[n];
        n++;
    }
    buf[n++] = '0';
    buf[n++] = 'x';
    for (int i = 15; i >= 0; i--)
        buf[n++] = digits[(value >> ((u64)i * 4)) & 0xf];
    buf[n++] = '\n';
    (void)pacha_syscall2(1, (u64)buf, n);
}

void device_fd_boot_smoke_main(void) {
    log_text("[device_fd_boot_smoke] start\n");
    const long count = pacha_capsule_pci_function_count();
    if (count <= 0) {
        log_hex("[device_fd_boot_smoke] catalog count/status=", (u64)count);
        return;
    }
    struct pacha_capsule_pci_function function = {0};
    const int lookup = pacha_capsule_pci_function_at(0, &function);
    if (lookup != 0) {
        log_hex("[device_fd_boot_smoke] lookup status=", (u64)(long long)lookup);
        return;
    }
    const int fd = pacha_capsule_pci_claim(0);
    if (fd < 16) {
        log_hex("[device_fd_boot_smoke] claim status=", (u64)(long long)fd);
        return;
    }
    struct pacha_capsule_info info = {0};
    const int status = pacha_capsule_expect_kind(fd, PACHA_CAPSULE_KIND_DEVICE, &info);
    if (status != 0 || info.fd != (u64)(unsigned)fd ||
        info.rights == 0 || info.device != function.resource_id) {
        log_hex("[device_fd_boot_smoke] query status=", (u64)(long long)status);
        log_hex("[device_fd_boot_smoke] resource=", info.device);
        (void)pacha_capsule_close(fd);
        return;
    }
    (void)pacha_capsule_close(fd);
    log_text("[device_fd_boot_smoke] OK\n");
}
