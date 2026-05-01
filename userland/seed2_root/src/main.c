enum {
    SYSCALL_LOG = 0x9,
    SYSCALL_WAIT_EVENT = 0x17,
};

static unsigned long cstr_len(const char *s) {
    unsigned long n = 0;
    while (s[n] != 0) n++;
    return n;
}

static void user_log_len(const char *message, unsigned long len) {
    unsigned long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"((unsigned long)SYSCALL_LOG), "D"((unsigned long)message), "S"(len)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    (void)ret;
}

static void user_log(const char *message) {
    user_log_len(message, cstr_len(message));
}

static unsigned long wait_event(void) {
    unsigned long ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"((unsigned long)SYSCALL_WAIT_EVENT), "D"(1UL), "S"(1UL)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

void seed2_root_main(void) {
    user_log("[seed2_root] started\n");
    user_log("[seed2_root] rootfs manager skeleton\n");
    user_log("[seed2_root] manifest scheduler not implemented yet\n");

    for (;;) {
        (void)wait_event();
    }
}
