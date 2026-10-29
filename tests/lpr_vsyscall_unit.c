#include "../userland/personality/linux/runtime/lpr_vsyscall.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

int main(void)
{
    const uint64_t entry = UINT64_C(0xffffffffff600800);
    uintptr_t target = lpr_vsyscall_fault_target(14, 0x14, entry);
    assert(target);
    assert(!lpr_vsyscall_fault_target(14, 0x4, entry)); /* Data fault. */
    assert(!lpr_vsyscall_fault_target(13, 0x14, entry));
    assert(!lpr_vsyscall_fault_target(14, 0x14, entry + 1));
    assert(!lpr_vsyscall_fault_target(14, 0x14, UINT64_C(0xffffffffff600000)));
    int (*getcpu)(uint32_t *, uint32_t *, void *) = (void *)target;
    uint32_t cpu = UINT32_MAX, node = UINT32_MAX;
    int result = getcpu(&cpu, &node, NULL);
    assert(result == 0 || result == -ENOSYS);
    if (!result) {
        assert(cpu != UINT32_MAX && node == 0);
        assert(!getcpu(NULL, NULL, NULL));
    }
    puts("LPR_VSYSCALL_UNIT=PASS");
}
