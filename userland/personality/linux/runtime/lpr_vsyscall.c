#include "lpr_vsyscall.h"
#include <errno.h>

static int lpr_vsyscall_getcpu(uint32_t *cpu, uint32_t *node, void *unused)
{
    (void)unused;
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000000u), "c"(0u));
    if (eax < 0x80000001u) return -ENOSYS;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000001u), "c"(0u));
    if (!(edx & (1u << 27))) return -ENOSYS;
    /* PachaOS publishes the logical CPU slot in IA32_TSC_AUX. Reading it
     * samples the executing CPU without inventing a fixed CPU ID. PachaOS
     * currently exposes one memory node, independent of host NUMA topology. */
    __asm__ volatile("rdtscp" : "=a"(eax), "=d"(edx), "=c"(ecx) :: "memory");
    if (cpu) *cpu = ecx;
    if (node) *node = 0;
    return 0;
}

uintptr_t lpr_vsyscall_fault_target(uint64_t vector, uint64_t error, uint64_t rip)
{
    /* The Linux legacy getcpu entry is a call target, not mapped code on
     * PachaOS. Redirect only its instruction-fetch fault. The native fault
     * return preserves the original call stack and argument registers, so
     * the helper returns normally without reading an untrusted saved RSP. */
    if (vector == 14 && (error & (1u << 4)) &&
        rip == UINT64_C(0xffffffffff600800))
        return (uintptr_t)lpr_vsyscall_getcpu;
    return 0;
}
