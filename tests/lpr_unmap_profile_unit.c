#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { PACHAOS_SYSCALL_LOG = 42 };
static int64_t lpr_linux_current_pid = 123;
static unsigned logs;
static char last[208];
static int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t address, uint64_t length)
{
    assert(nr == PACHAOS_SYSCALL_LOG && length < sizeof(last));
    memcpy(last, (const void *)(uintptr_t)address, length);
    last[length] = 0;
    assert(last[length - 1] == '\n');
    ++logs;
    return -5;
}
#include "../userland/personality/linux/runtime/support/unmap_profile.h"

int main(void)
{
    lpr_unmap_profile_snapshot_t snapshot = {0};
    uint64_t stamp[4] = {100, 110, 140, 160};
    for (unsigned i = 0; i < 65535; ++i)
        assert(!lpr_unmap_profile_record(4096, 0, stamp, &snapshot));
    assert(!snapshot.value[0] && !logs);
    assert(lpr_unmap_profile_record(16384, 3, stamp, &snapshot));
    assert(snapshot.value[0] == 65536 && snapshot.value[1] == 65535);
    assert(snapshot.value[2] == 1 && snapshot.value[3] == 0);
    assert(snapshot.value[4] == 655360 && snapshot.value[5] == 1966080);
    assert(snapshot.value[6] == 1310720);
    assert(snapshot.value[7] == 100 && snapshot.value[8] == 160);
    lpr_unmap_profile_emit(&snapshot);
    assert(logs == 1 && strstr(last, "LPR_UNMAP_STAGE 000000000000007b 0000000000010000 "));
    for (unsigned i = 65536; i < 17u * 65536u; ++i)
        if (lpr_unmap_profile_record(8192, 0, stamp, &snapshot))
            lpr_unmap_profile_emit(&snapshot);
    assert(logs == 16);
    lpr_unmap_profile_reset();
    stamp[2] = 109;
    assert(!lpr_unmap_profile_record(0, 1, stamp, &snapshot));
    assert(lpr_unmap_profile_totals.value[0] == 1);
    assert(lpr_unmap_profile_totals.value[1] == 0);
    assert(lpr_unmap_profile_totals.value[2] == 1);
    assert(lpr_unmap_profile_totals.value[3] == 1);
    assert(!lpr_unmap_profile_totals.value[4] && !lpr_unmap_profile_totals.value[5]);
    assert(lpr_unmap_profile_clock() != 0);
    puts("unmap profile stages, bounds, errors and reset: PASS");
}
