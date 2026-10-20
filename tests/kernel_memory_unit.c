#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#define memcpy kernel_test_memcpy
#define memset kernel_test_memset
#include "../kernel/src/arch/x86_64/memory.c"
#undef memcpy
#undef memset

/* PROT_NONE neighbours catch vector-width overreads and tail overwrites.
 * Canaries also cover all small lengths and independent unaligned pointers. */
int main(void) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *a = mmap(0, page*3, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    unsigned char *b = mmap(0, page*3, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    assert(a != MAP_FAILED && b != MAP_FAILED);
    assert(!mprotect(a+page, page, PROT_READ|PROT_WRITE));
    assert(!mprotect(b+page, page, PROT_READ|PROT_WRITE));
    a += page; b += page;
    kernel_memory_init();
    printf("CPUID string_features=%u (ERMS=1 FSRM=2)\n", string_features);
    for (unsigned mode=0; mode<4; ++mode) {
        string_features=mode;
        for (size_t n=0; n<=page; ++n) {
            for (size_t i=0; i<page; ++i) { a[i]=(unsigned char)(i*37); b[i]=0xA5; }
            assert(kernel_test_memcpy(b+page-n,a+page-n,n)==b+page-n);
            for (size_t i=0; i<page; ++i) assert(b[i]==(i<page-n ? 0xA5:a[i]));
            assert(kernel_test_memset(b+page-n,0x13C,n)==b+page-n);
            for (size_t i=0; i<page; ++i) assert(b[i]==(i<page-n ? 0xA5:0x3C));
        }
        for (size_t src=0;src<16;++src) for (size_t dst=0;dst<16;++dst)
            for (size_t n=0;n<257;++n) {
                for (size_t i=0;i<page;++i) {a[i]=(unsigned char)(i*37);b[i]=0xA5;}
                kernel_test_memcpy(b+dst,a+src,n);
                for(size_t i=0;i<dst+n+1;++i) assert(b[i]==(i<dst||i>=dst+n ? 0xA5:a[src+i-dst]));
            }
    }
    assert(!munmap(a-page,page*3));assert(!munmap(b-page,page*3));
    puts("kernel memory PASS");
}
