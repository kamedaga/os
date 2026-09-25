#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { PACHAOS_SYSCALL_LOG = 42 };
static int64_t lpr_linux_current_pid = 123;
static unsigned logs;
static char last[320];
static int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t address, uint64_t bytes)
{
    assert(nr == PACHAOS_SYSCALL_LOG && bytes < sizeof(last));
    memcpy(last, (const void *)(uintptr_t)address, bytes);
    last[bytes] = 0;
    assert(last[bytes - 1] == '\n');
    ++logs;
    return -5; // Diagnostics must not change the caller's operation result.
}
#include "../userland/personality/linux/runtime/support/mmap_profile.h"

int main(void)
{
    uint64_t stamp[6] = {1, 2, 3, 4, 5, 6};
    lpr_map_profile_emit("shared", 18, 5851, 1, stamp);
    assert(logs == 0);
    stamp[5] = 100000001;
    lpr_map_profile_emit("shared", 18, 5851, 1, stamp);
    assert(logs == 1);
    assert(strstr(last, "LPR_MAP_STAGE shared 000000000000007b 0000000000000012 00000000000016db 0000000000000001 "));
    for (unsigned i = 0; i < 100; ++i)
        lpr_map_profile_emit("rpc", 18, 5851, 1, stamp);
    assert(logs == 48);
    assert(lpr_map_profile_clock() != 0);
    puts("lpr mmap stage threshold/format/bound: PASS");
}
