#include <assert.h>
#include <stdio.h>
#include "../userland/filed/src/exec/linux_lpr/relocate.h"

static unsigned char mem[4096], ph[112];
static void put(unsigned char *p, uint64_t x) { memcpy(p, &x, 8); }
static void setup(void)
{
    memset(mem, 0, sizeof(mem)); memset(ph, 0, sizeof(ph));
    put(ph, 1 | (UINT64_C(6) << 32)); /* writable PT_LOAD */
    put(ph + 32, 2048); put(ph + 40, sizeof(mem));
    put(ph + 56, 2); put(ph + 72, 128); put(ph + 88, 64);
    put(mem + 128, 7); put(mem + 136, 256);
    put(mem + 144, 8); put(mem + 152, 48);
    put(mem + 160, 9); put(mem + 168, 24);
    put(mem + 256, 512); put(mem + 264, 8); put(mem + 272, 1024);
    put(mem + 280, 3000); put(mem + 288, 8); put(mem + 296, 128);
}
static int run(uint64_t bias)
{
    return lpr_exec_relocate_runtime(mem, sizeof(mem), bias, bias, ph, 2, 56);
}
static void rejected(void)
{
    unsigned char before[sizeof(mem)]; memcpy(before, mem, sizeof(mem));
    assert(run(0x4000000) == -8);
    assert(memcmp(before, mem, sizeof(mem)) == 0);
}
int main(void)
{
    setup(); assert(run(0x4000000) == 0);
    assert(lpr_reloc_u64(mem + 512) == 0x4000400);
    assert(lpr_reloc_u64(mem + 3000) == 0x4000080); /* BSS destination */
    setup(); assert(run(0x8000000) == 0);
    assert(lpr_reloc_u64(mem + 512) == 0x8000400);
    setup(); put(mem + 272, (uint64_t)-16); assert(run(0x4000000) == 0);
    assert(lpr_reloc_u64(mem + 512) == 0x3fffff0);
    setup(); put(mem + 288, 0); assert(run(0x4000000) == 0);
    assert(lpr_reloc_u64(mem + 3000) == 0); /* R_NONE */
    setup(); put(ph + 56, 0); assert(run(0x4000000) == 0);
    setup(); put(mem + 128, 0); assert(run(0x4000000) == 0);
    setup(); put(mem + 288, (UINT64_C(1) << 32) | 8); rejected();
    setup(); put(mem + 288, 1); rejected();
    setup(); put(ph, 1 | (UINT64_C(4) << 32)); rejected();
    setup(); put(mem + 280, 4090); rejected();
    setup(); put(mem + 280, 280); rejected();
    setup(); put(mem + 280, 160); rejected();
    setup(); put(mem + 280, UINT64_MAX - 3); rejected();
    setup(); put(mem + 136, 2030); rejected(); /* metadata in BSS */
    setup(); put(mem + 136, UINT64_MAX - 16); rejected();
    setup(); put(mem + 152, 25); rejected();
    setup(); put(mem + 168, 16); rejected();
    setup(); put(mem + 160, 7); rejected(); /* duplicate tag */
    setup(); put(mem + 160, 0); rejected(); /* missing RELAENT */
    setup(); put(mem + 176, 21); rejected(); /* missing DT_NULL */
    setup(); put(mem + 160, 1); rejected(); /* dependency */
    setup(); put(mem + 160, 36); rejected(); /* unsupported RELR */
    setup(); put(ph + 88, 63); rejected();
    setup(); put(mem + 296, (uint64_t)-0x4000001); rejected();
    setup(); assert(run(UINT64_MAX - 512) == -8);
    puts("FILED_LPR_RELOCATION_UNIT=OK");
    return 0;
}
