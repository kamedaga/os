#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <termios.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { perror(#test);return 1; } } while (0)
int main(void)
{
    long page=sysconf(_SC_PAGESIZE);
    unsigned char *p=mmap(NULL,2*(size_t)page,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(p!=MAP_FAILED);
    CHECK(!mprotect(p+page,(size_t)page,PROT_NONE));
    /* The raw syscall ABI is 36 bytes, even when libc termios is larger.
     * A protected following page catches both GET overwrite and SET overread. */
    unsigned char *raw=p+page-36;
    memset(raw-16,0xa5,16);
    CHECK(!syscall(SYS_ioctl,STDIN_FILENO,TCGETS,raw));
    for (int i=1;i<=16;++i) CHECK(raw[-i]==0xa5);
    CHECK(!syscall(SYS_ioctl,STDIN_FILENO,TCSETS,raw));
    CHECK(!syscall(SYS_ioctl,STDIN_FILENO,TCSETSW,raw));
    CHECK(!syscall(SYS_ioctl,STDIN_FILENO,TCSETSF,raw));
    struct termios t;
    CHECK(isatty(STDIN_FILENO)==1);
    CHECK(!tcgetattr(STDIN_FILENO,&t));
    CHECK(!tcsetattr(STDIN_FILENO,TCSANOW,&t));
    CHECK(!munmap(p,2*(size_t)page));
    puts("TERMIOS_PROBE=PASS raw-36-byte-boundary libc-isatty-get-set");
    return 0;
}
