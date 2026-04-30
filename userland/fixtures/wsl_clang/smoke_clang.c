extern int smoke_value(void);
extern void optional_weak_hook(void) __attribute__((weak));

static void user_log(const char *message, unsigned long length) {
    __asm__ volatile(
        "syscall"
        :
        : "a"(9UL), "D"(message), "S"(length), "d"(0UL)
        : "rcx", "r8", "r9", "r10", "r11", "memory");
}

__attribute__((constructor))
static void main_ctor(void) {
    static const char message[] = "smoke_clang: constructor\n";
    user_log(message, sizeof(message) - 1);
}

__attribute__((destructor))
static void main_dtor(void) {
    static const char message[] = "smoke_clang: destructor\n";
    user_log(message, sizeof(message) - 1);
}

static int ifunc_impl(void) {
    static const char message[] = "smoke_clang: ifunc impl\n";
    user_log(message, sizeof(message) - 1);
    return 7;
}

static void *ifunc_resolver(void) {
    static const char message[] = "smoke_clang: ifunc resolver\n";
    user_log(message, sizeof(message) - 1);
    return ifunc_impl;
}

int smoke_ifunc_value(void) __attribute__((ifunc("ifunc_resolver")));

void _start(void) {
    static const char start_message[] = "smoke_clang: entry\n";
    static const char ok_message[] = "smoke_clang: value=42\n";
    static const char bad_message[] = "smoke_clang: value mismatch\n";
    static const char weak_ok_message[] = "smoke_clang: weak=0\n";
    static const char weak_bad_message[] = "smoke_clang: weak resolved\n";
    static const char ifunc_ok_message[] = "smoke_clang: ifunc=7\n";
    static const char ifunc_bad_message[] = "smoke_clang: ifunc mismatch\n";
    user_log(start_message, sizeof(start_message) - 1);
    if (smoke_value() == 42) {
        user_log(ok_message, sizeof(ok_message) - 1);
    } else {
        user_log(bad_message, sizeof(bad_message) - 1);
    }
    if (optional_weak_hook == 0) {
        user_log(weak_ok_message, sizeof(weak_ok_message) - 1);
    } else {
        user_log(weak_bad_message, sizeof(weak_bad_message) - 1);
        optional_weak_hook();
    }
    if (smoke_ifunc_value() == 7) {
        user_log(ifunc_ok_message, sizeof(ifunc_ok_message) - 1);
    } else {
        user_log(ifunc_bad_message, sizeof(ifunc_bad_message) - 1);
    }
}
