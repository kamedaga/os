#include <assert.h>
#include <stdio.h>
#include "../userland/personality/linux/runtime/lpr_dispatch.c"

int main(void)
{
    /* Unsupported requests must not dereference even an invalid word. */
    assert(lpr_sys_futex(1, 6, 0, 0, 0, 0) == -95);
    assert(lpr_sys_futex(1, 134, 0, 0, 0, 0) == -95);
    assert(lpr_sys_futex(1, 7, 0, 0, 0, 0) == -95);
    assert(lpr_sys_futex(1, 8, 0, 0, 0, 0) == -95);
    assert(lpr_sys_futex(1, 11, 0, 0, 0, 0) == -95);
    assert(lpr_sys_futex(1, 12, 0, 0, 0, 0) == -95);
    assert(lpr_sys_futex(1, 13, 0, 0, 0, 0) == -95);
    assert(lpr_sys_futex(1, 127, 0, 0, 0, 0) == -38);
    assert(lpr_sys_futex(1, 6 | 512, 0, 0, 0, 0) == -38);
    puts("futex protocol unit PASS");
    return 0;
}
