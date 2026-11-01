#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define LPR_LINUX_AF_INET 2
#define LPR_LINUX_AF_INET6 10
#define LPR_LINUX_EFAULT EFAULT
#define LPR_LINUX_EINVAL EINVAL
#define LPR_LINUX_EAFNOSUPPORT EAFNOSUPPORT
#define lpr_memcmp memcmp
#define lpr_memcpy memcpy
#define lpr_memset memset
#include "mapped-types.inc"
#include "mapped-copy.inc"
int main(void) {
    lpr_linux_sockaddr_in6_t a = {.family=10,.port_be=0x1234};
    a.address[10]=a.address[11]=255; a.address[12]=127; a.address[15]=1;
    uint32_t ip=0; uint16_t port=0,mapped=0;
    assert(!lpr_socket_decode_address(10,(uintptr_t)&a,sizeof(a),0,&ip,&port,&mapped));
    assert(!memcmp(&ip,a.address+12,4) && port==a.port_be && mapped==1);
    assert(lpr_socket_decode_address(10,(uintptr_t)&a,27,0,&ip,&port,&mapped)==-EINVAL);
    unsigned char out[40]; memset(out,0xcc,sizeof(out)); uint32_t n=sizeof(out);
    assert(!lpr_socket_copy_sockaddr(10,(uintptr_t)out,(uintptr_t)&n,ip,port,1));
    assert(n==28 && !memcmp(out,&a,28) && out[28]==0xcc);
    n=3; memset(out,0xcc,sizeof(out));
    assert(!lpr_socket_copy_sockaddr(10,(uintptr_t)out,(uintptr_t)&n,ip,port,1));
    assert(n==28 && !memcmp(out,&a,3) && out[3]==0xcc);
    memset(a.address,0,16);
    assert(!lpr_socket_decode_address(10,(uintptr_t)&a,28,1,&ip,&port,&mapped));
    assert(!ip && !mapped);
    n=28; assert(!lpr_socket_copy_sockaddr(10,(uintptr_t)out,(uintptr_t)&n,0,port,0));
    assert(!memcmp(out,&a,28));
    assert(lpr_socket_decode_address(10,(uintptr_t)&a,28,0,&ip,&port,&mapped)==-ENETUNREACH);
    a.address[0]=0x20; a.address[1]=1;
    assert(lpr_socket_decode_address(10,(uintptr_t)&a,28,0,&ip,&port,&mapped)==-ENETUNREACH);
    assert(lpr_socket_decode_address(10,(uintptr_t)&a,28,1,&ip,&port,&mapped)==-EADDRNOTAVAIL);
    a.family=2; assert(lpr_socket_decode_address(10,(uintptr_t)&a,28,0,&ip,&port,&mapped)==-EAFNOSUPPORT);
    lpr_linux_sockaddr_in_t v4={.family=2,.port_be=0x4321,.addr_be=0x0100007f};
    assert(!lpr_socket_decode_address(2,(uintptr_t)&v4,16,0,&ip,&port,&mapped));
    n=40;assert(!lpr_socket_copy_sockaddr(2,(uintptr_t)out,(uintptr_t)&n,ip,port,0));
    assert(n==16 && !memcmp(out,&v4,16));
    puts("Production mapped-address helpers: IPv4 roundtrip, mapped UDP, wildcard bind, truncation bounds, native IPv6 rejection PASS");
}
