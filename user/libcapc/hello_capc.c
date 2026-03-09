#include <stdint.h>

#include "capc.h"

int main(void) {
    static const char msg[] = "hello from libcapc C sample via syscall_log\n";
    long rc = cap_write(1, msg, sizeof(msg) - 1);
    return rc < 0 ? 1 : 0;
}
