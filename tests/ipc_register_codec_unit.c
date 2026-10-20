#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <pacha/ipc.h>
static unsigned used;
static int attached;
static uint64_t expected_send, expected_receive;
long pacha_ipc_registers(uint64_t nr, uint64_t fd, uint64_t flags, uint64_t timeout,
    uint64_t words[4], struct pacha_ipc_extension *ext, int *committed) {
    assert(nr == PACHA_IPC_SYSCALL_CALL && fd == 42);
    assert(flags == PACHA_WAIT_UNINTERRUPTIBLE && timeout == UINT64_MAX);
    for (unsigned i=0;i<4;++i) assert(words[i] == (i < used ? 0xabc000u+i : 0));
    assert(!!ext == attached);
    if (ext) {
        assert(ext->send_array == expected_send && ext->send_count == 1);
        assert(ext->receive_array == expected_receive && ext->receive_capacity == 1);
        struct pacha_ipc_fd *received=(void *)(uintptr_t)ext->receive_array;
        received[0]=(struct pacha_ipc_fd){.fd=91};
        ext->received_count=1;
    }
    for (unsigned i=0;i<4;++i) words[i]=100+i;
    *committed=1;
    return 0;
}
int main(void) {
    for (used=0;used<=4;++used) for (attached=0;attached<=1;++attached) {
        struct pacha_ipc_msg send={0},receive={0};
        uint64_t words[4]={0};
        for (unsigned i=0;i<used;++i) words[i]=0xabc000u+i;
        send.word0=words[0];send.word1=words[1];send.word2=words[2];send.word3=words[3];
        struct pacha_ipc_fd in={.fd=31},out={0};
        if(attached) {send.fds=&in;send.fd_count=1;receive.fds=&out;receive.fd_capacity=1;}
        expected_send=(uintptr_t)&in;expected_receive=(uintptr_t)&out;
        int committed=0;
        assert(!pacha_ipc_message_syscall(PACHA_IPC_SYSCALL_CALL,42,&send,&receive,
            UINT64_MAX,PACHA_WAIT_UNINTERRUPTIBLE,&committed));
        assert(committed==1 && receive.word0==100 && receive.word1==101 && receive.word2==102 && receive.word3==103);
        assert(receive.fd_count==(unsigned)attached);
        if(attached) assert(out.fd==91);
    }
    puts("IPC register packing: 0..4 initialized words, zero suffix, optional FD extension, reply and commitment PASS");
}
