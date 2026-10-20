#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
static uint64_t invoke(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *, uint64_t *);
#define PACHA_POLL_TEST_INVOKE invoke
#include "pacha/poll.h"
static unsigned mode, calls;
static uint64_t invoke(uint64_t nr, uint64_t address, uint64_t count,
    uint64_t timeout, uint64_t *index, uint64_t *events) {
    assert(nr == 51 || nr == 52); assert(timeout == 99); assert(*index == 2);
    struct pacha_pollfd *fds = (void *)(uintptr_t)address;
    for (unsigned i=0; i<count; ++i) {
        assert(fds[i].revents == 0);
        assert(fds[i].min_write_bytes == 58+i);
    }
    ++calls;
    if (mode == 1) { *index=count-1; *events=4; return 1; }
    if (mode == 2) { fds[0].revents=1; fds[count-1].revents=4; return 2; }
    /* INVALID shares numeric 1 with one ready. No event output on error. */
    if (mode == 3) return 1;
    return 2;
}
int main(void) {
    _Static_assert(offsetof(struct pacha_pollfd,min_write_bytes)==16,"minimum");
    _Static_assert(offsetof(struct pacha_pollfd,revents)==24,"output");
    struct pacha_pollfd fds[256];
    for (unsigned i=0;i<256;++i) fds[i]=(struct pacha_pollfd){.fd=16+i,.events=5,.min_write_bytes=58+i,.revents=~0ull};
    for (unsigned nr=51;nr<=52;++nr) for(mode=0;mode<4;++mode) {
        assert(pacha_poll_registers(nr,(uintptr_t)fds,256,99,2)==(mode==1||mode==3?1:2));
        for(unsigned i=0;i<256;++i) {
            assert(fds[i].min_write_bytes==58+i);
            assert(fds[i].revents==(mode==2&&i==0?1:(mode==1||mode==2)&&i==255?4:0));
        }
    }
    assert(calls==8); puts("POLL_REGISTER_UNIT=OK");
}
