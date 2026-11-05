#include "../lpr_fault_diag.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char line[769];
    struct lpr_fault_diag d = {.sig = 4, .vector = 6, .rip = 0x1234,
        .rsp = 0x9876, .mapped = 1, .map_start = 0x1000, .map_offset = 0x234,
        .file = "/lib/test\nfile.so", .comm = "test", .byte_count = 16,
        .bytes = {0x0f, 0x0b, 0x90}};
    size_t n = lpr_fault_format(line, 768, &d);
    line[n] = 0;
    assert(strstr(line, "src=fault sig=4 vector=6 err=0x0 address=0x0 cr2=NA"));
    assert(strstr(line, "rip=0x1234 rsp=0x9876"));
    assert(strstr(line, "file=/lib/test?file.so map_start=0x1000 offset=0x234"));
    assert(strstr(line, "bytes=0f0b90"));
    assert(strchr(line, '\n') == line + n - 1);
    d.vector = 14; d.sig = 11; d.error = 7; d.address = 0xdead000;
    n = lpr_fault_format(line, 768, &d); line[n] = 0;
    assert(strstr(line, "sig=11 vector=14 err=0x7 address=0xdead000 cr2=0xdead000"));
    assert(!strstr(line, "bytes="));
    d.vector = 6; d.sig = 4; d.rip = 0x1fff;
    assert(lpr_fault_byte_limit(d.rip) == 1);
    n = lpr_fault_format(line, 768, &d); line[n] = 0;
    assert(strstr(line, "bytes=0f\n"));
    d.rip = 0x1ff1;
    assert(lpr_fault_byte_limit(d.rip) == 15);
    assert(lpr_fault_byte_limit(0x2000) == 16);
    d.byte_count = 0;
    n = lpr_fault_format(line, 768, &d); line[n] = 0;
    assert(strstr(line, "bytes=unavailable\n"));
    d.async = 1;
    n = lpr_fault_format(line, 768, &d); line[n] = 0;
    assert(strstr(line, "src=async sig=4 sender=unknown"));
    assert(!strstr(line, "bytes="));
    for (size_t capacity = 0; capacity <= 768; ++capacity) {
        memset(line, 'x', sizeof(line));
        n = lpr_fault_format(line, capacity, &d);
        assert(n <= capacity && line[capacity] == 'x');
        if (capacity) assert(line[n - 1] == '\n');
    }
    puts("fault formatter: #UD, #PF, page boundary, unavailable, async, capacity: PASS");
}
