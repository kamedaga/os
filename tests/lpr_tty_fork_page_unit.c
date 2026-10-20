#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../userland/personality/linux/runtime/lpr_filed_internal.h"

lpr_state_t lpr_state;
static unsigned next_fd = 100;
static unsigned closes, unmaps;
void lpr_state_lock(volatile uint32_t *word) { (void)word; }
void lpr_state_unlock(volatile uint32_t *word) { *word = 0; }
void lpr_fs_session_after_fork_child(void) {}
void lpr_netd_page_pool_after_fork_child(void) {}
int64_t lpr_pacha_status_to_errno(int64_t n) { return n; }
int64_t lpr_pacha_syscall1(uint64_t nr, uint64_t fd) {
    assert(nr == PACHAOS_SYSCALL_FD_CLOSE && fd >= 100); ++closes; return 0;
}
int64_t lpr_pacha_syscall2(uint64_t nr,uint64_t address,uint64_t size) {
    assert(nr == PACHAOS_SYSCALL_MUNMAP && size == TERMD_PAGE_BYTES);
    ++unmaps; return munmap((void *)(uintptr_t)address,size);
}
int64_t lpr_pacha_syscall3(uint64_t nr,uint64_t size,uint64_t rights,uint64_t flags) {
    (void)rights; (void)flags;
    assert(nr == PACHAOS_SYSCALL_VMO_CREATE && size == TERMD_PAGE_BYTES);
    return next_fd++;
}
int64_t lpr_pacha_syscall6(uint64_t nr,uint64_t fd,uint64_t off,uint64_t size,uint64_t prot,uint64_t flags,uint64_t hint) {
    (void)fd;(void)off;(void)prot;(void)flags;(void)hint;
    assert(nr == PACHAOS_SYSCALL_MMAP);
    void *p=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    assert(p != MAP_FAILED); return (int64_t)(uintptr_t)p;
}
int main(void) {
    /* Native fork preserves shared VMO mappings but copies process-local locks.
     * Parent and child requests must never reuse the same RPC envelope. */
    lpr_tty_wire_page_fd=-1;
    void *page; pacha_fd_t fd=lpr_create_tty_wire_page(&page);
    pacha_service_envelope_t *parent=page;
    parent->request_id=41; parent->result=12;
    lpr_destroy_tty_wire_page(fd,page);
    pid_t child=fork();assert(child>=0);
    if(!child) {
        lpr_reset_fork_child_rpc_state();
        assert(closes==1 && unmaps==1);
        lpr_reset_fork_child_rpc_state();
        assert(closes==1 && unmaps==1);
        void *copy; pacha_fd_t child_fd=lpr_create_tty_wire_page(&copy);
        pacha_service_envelope_t *header=copy;
        header->request_id=42; header->result=13;
        lpr_destroy_tty_wire_page(child_fd,copy);
        _exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child && status==0);
    fprintf(stderr,"parent RPC after child: id=%llu result=%llu\n",(unsigned long long)parent->request_id,(unsigned long long)parent->result);
    assert(parent->request_id==41 && parent->result==12);
    assert(!closes && !unmaps);
    puts("TTY fork RPC isolation PASS");
    return 0;
}
