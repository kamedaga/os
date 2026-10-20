/* Kernel RAM copies must not touch live user XState. All paths use GPRs,
 * including early boot before feature discovery. These are not MMIO APIs.
 * DF is clear at every C entry; no path changes it (including IRQ nesting). */
#include <stddef.h>
#include <stdint.h>

static unsigned int string_features;

/* Called by the BSP before AP publication. Afterwards this is immutable.
 * ERMS/FSRM change performance, not REP instruction availability. */
void kernel_memory_init(void) {
    unsigned int a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    if (a < 7) return;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    string_features = ((b >> 9) & 1u) | (((d >> 4) & 1u) << 1);
}

void *memcpy(void *restrict destination, const void *restrict source, size_t count) {
    void *original = destination;
    if ((string_features & 2u) || ((string_features & 1u) && count >= 128)) {
        __asm__ volatile("rep movsb" : "+D"(destination), "+S"(source), "+c"(count) : : "memory");
    } else {
        size_t words = count / 8;
        count %= 8;
        __asm__ volatile("rep movsq" : "+D"(destination), "+S"(source), "+c"(words) : : "memory");
        __asm__ volatile("rep movsb" : "+D"(destination), "+S"(source), "+c"(count) : : "memory");
    }
    return original;
}

void *memset(void *destination, int value, size_t count) {
    void *original = destination;
    uint64_t byte = (unsigned char)value;
    /* FSRM promises fast short MOV, not fast short STOS. */
    if ((string_features & 1u) && count >= 128) {
        __asm__ volatile("rep stosb" : "+D"(destination), "+c"(count) : "a"(byte) : "memory");
    } else {
        size_t words = count / 8;
        count %= 8;
        uint64_t fill = byte * UINT64_C(0x0101010101010101);
        __asm__ volatile("rep stosq" : "+D"(destination), "+c"(words) : "a"(fill) : "memory");
        __asm__ volatile("rep stosb" : "+D"(destination), "+c"(count) : "a"(byte) : "memory");
    }
    return original;
}
