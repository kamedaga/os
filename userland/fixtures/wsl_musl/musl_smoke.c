#include <stddef.h>
#include <unistd.h>

static void cap_log(const char *message, unsigned long length) {
    __asm__ volatile(
        "syscall"
        :
        : "a"(9UL), "D"(message), "S"(length), "d"(0UL)
        : "rcx", "r8", "r9", "r10", "r11", "memory");
}

void _start(void) {
    static const char entry_message[] = "musl_smoke: entry\n";
    static const char write_message[] = "musl_smoke: write via musl\n";
    static const char write_ok_message[] = "musl_smoke: write ok\n";
    static const char write_fail_message[] = "musl_smoke: write failed\n";

    cap_log(entry_message, sizeof(entry_message) - 1);

    ssize_t written = write(1, write_message, sizeof(write_message) - 1);
    if (written == (ssize_t)(sizeof(write_message) - 1)) {
        cap_log(write_ok_message, sizeof(write_ok_message) - 1);
    } else {
        cap_log(write_fail_message, sizeof(write_fail_message) - 1);
    }
}
