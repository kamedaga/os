/* SPDX-License-Identifier: MIT */
#include "../src/relay.c"
#include <assert.h>
#include <stdio.h>

static unsigned file_closes, endpoint_closes;
static int file_error, endpoint_error;
int filed_storage_close(filed_runtime_t *rt,uint64_t handle)
{
    assert(rt && handle==123);
    ++file_closes;return file_error;
}
int pacha_fd_close(int fd)
{
    assert(fd==70);
    ++endpoint_closes;return endpoint_error;
}
int main(void)
{
    filed_runtime_t rt={.storage_generation=7};
    struct filed_lease lease={.client=1,.handle=123,.fd=70};
    assert(!retire_lease(&rt,&lease) && !lease.handle && lease.fd==-1);
    assert(file_closes==1 && endpoint_closes==1 && rt.storage_generation==7);
    lease=(struct filed_lease){.client=1,.handle=123,.fd=70};
    file_error=-EIO;
    assert(retire_lease(&rt,&lease)==-EIO && lease.handle==123 && lease.fd==70);
    assert(!rt.storage_generation && file_closes==2 && endpoint_closes==1);
    assert(retire_lease(&rt,&lease)==-EIO && file_closes==2 && endpoint_closes==1);
    rt.storage_generation=7;file_error=0;endpoint_error=-EBUSY;
    /* A distinct stopped-fixture generation, not rebinding the failed lease. */
    lease=(struct filed_lease){.client=1,.handle=123,.fd=70};
    assert(retire_lease(&rt,&lease)==-EBUSY && !lease.handle && lease.fd==70);
    assert(!rt.storage_generation && file_closes==3 && endpoint_closes==2);
    assert(retire_lease(&rt,&lease)==-EIO && file_closes==3 && endpoint_closes==2);
    rt.storage_generation=7;endpoint_error=0;
    lease=(struct filed_lease){.client=9,.handle=123,.fd=70};
    assert(!retire_lease(&rt,&lease) && lease.handle==123 && lease.fd==-1);
    assert(file_closes==3 && endpoint_closes==3);
    puts("filed FD escrow retirement: close ordering / ambiguous close retained / partial FD close retained / client OFD untouched PASS");
    return 0;
}
