static void user_log(const char *message, unsigned long length) {
    __asm__ volatile(
        "syscall"
        :
        : "a"(9UL), "D"(message), "S"(length), "d"(0UL)
        : "rcx", "r8", "r9", "r10", "r11", "memory");
}

__attribute__((constructor))
static void lib_ctor(void) {
    static const char message[] = "libsmoke_clang: constructor\n";
    user_log(message, sizeof(message) - 1);
}

__attribute__((destructor))
static void lib_dtor(void) {
    static const char message[] = "libsmoke_clang: destructor\n";
    user_log(message, sizeof(message) - 1);
}

int smoke_value(void) {
    static const char message[] = "libsmoke_clang: smoke_value\n";
    user_log(message, sizeof(message) - 1);
    return 42;
}
