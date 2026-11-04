/* SPDX-License-Identifier: MIT */
/* User-side timing only. No clock syscall, logging or setup inside TSC spans. */
#include <stdint.h>
#include <stddef.h>
#include <pacha/abi.h>
#include <pacha/ipc.h>
#include <pacha/syscall.h>
#ifndef BENCH_CASE
#define BENCH_CASE 0
#endif
#define TRIALS 31
#define PAGES 1024
#define PAGE 4096ull
#define RIGHTS (PACHA_FD_RIGHT_SHARE | PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE | PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WRITE | PACHA_FD_RIGHT_REVOKE)
static struct { uint64_t ticks, ops; } samples[TRIALS];
static long views[10000], duplicates[64];
static uint64_t view_count, duplicate_count;
static long view_error;
static struct pacha_fd_info info;
static void logstr(const char *s) { size_t n=0; while(s[n]) ++n; (void)pacha_syscall2(1,(uintptr_t)s,n); }
static void number(uint64_t n) { char b[24]; unsigned i=0; do { b[i++]='0'+n%10; n/=10; } while(n); for(unsigned j=0;j<i/2;j++){char c=b[j];b[j]=b[i-1-j];b[i-1-j]=c;} b[i]=0;logstr(b); }
__attribute__((noreturn)) static void fail(unsigned step, long rc) { logstr("NATIVE_VMO=FAIL step=");number(step);logstr(" rc_u64=");number(rc);logstr("\n");(void)pacha_syscall1(PACHA_PROCESS_SYSCALL_EXIT,1);__builtin_trap(); }
static void check(long rc,unsigned step) { if(rc) fail(step,rc); }
static uint64_t ticks(void) { unsigned lo,hi;__asm__ volatile("lfence; rdtsc; lfence":"=a"(lo),"=d"(hi)::"memory");return ((uint64_t)hi<<32)|lo; }
static uint64_t now(void) { uint64_t n;check(pacha_clock_read(1,&n),1);return n; }
static long create(uint64_t pages) { long fd=pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE,pages*PAGE,RIGHTS,PACHA_VMO_CREATE_ZERO_ON_DEMAND);if(fd<16 || fd>=PACHA_THREAD_SELF_FD)fail(2,fd);return fd; }
static void closefd(long fd) { check(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE,fd),3); }
static volatile unsigned char *map(long fd) { long p=pacha_syscall6(PACHA_VM_SYSCALL_MMAP,fd,0,PAGES*PAGE,PACHA_PROT_READ|PACHA_PROT_WRITE,PACHA_MMAP_SHARED,0);if(p<4096)fail(4,p);return (void *)(uintptr_t)p; }
static void unmap(volatile unsigned char *p) { check(pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP,(uintptr_t)p,PAGES*PAGE),5); }
static void touch(volatile unsigned char *p,unsigned char value) { for(unsigned i=0;i<PAGES;i++)p[i*PAGE]=value; }
static void fault_touch(volatile unsigned char *p) { for(unsigned i=0;i<PAGES;i++){if(p[i*PAGE])fail(13,i);p[i*PAGE]=0x6b;} }
static void verify(volatile unsigned char *p,unsigned char value) { for(unsigned i=0;i<PAGES;i++)if(p[i*PAGE]!=value)fail(6,i); }
#if BENCH_CASE >= 2
static void discard(long fd) { struct pacha_vmo_discard_range r={0,PAGES};check(pacha_syscall4(PACHA_FD_SYSCALL_VMO_DISCARD,fd,(uintptr_t)&r,1,0),7); }
#endif
static void stress(long fd, uint64_t pair[2]) {
    /* A view consisting only of a sparse hole still creates a parent edge,
     * without pinning any page in the measured range. No extra root page. */
    const uint64_t index=PACHA_VMO_PAGE_VIEW_HOLE;
    for(;view_count<10000;view_count++) {
        long v=pacha_syscall5(PACHA_FD_SYSCALL_VMO_CREATE_PAGE_VIEW,fd,(uintptr_t)&index,1,PACHA_FD_RIGHT_CLOSE|PACHA_FD_RIGHT_MAP_READ,PACHA_VMO_PAGE_VIEW_SPARSE);
        if(v<16 || v>=PACHA_THREAD_SELF_FD){if(v!=PACHA_SYSCALL_ERR_ALLOC)fail(14,v);view_error=v;break;} views[view_count]=v;
    }
    /* Reserve FD and queued IPC edges first in main, before view exhaustion. */
    (void)pair;
}
static void edges(long fd,uint64_t pair[2]) {
    for(;duplicate_count<64;duplicate_count++) {
        long v=pacha_syscall4(PACHA_FD_SYSCALL_DUP,fd,16,RIGHTS,0);
        if(v<16 || v>=PACHA_THREAD_SELF_FD)fail(8,v);duplicates[duplicate_count]=v;
    }
    check(pacha_syscall3(PACHA_IPC_SYSCALL_CHANNEL_CREATE,(uintptr_t)pair,
        PACHA_FD_RIGHT_SEND|PACHA_FD_RIGHT_RECV|PACHA_FD_RIGHT_CLOSE,0),9);
    struct pacha_ipc_fd transfers[19];
    for(unsigned i=0;i<19;i++)transfers[i]=(struct pacha_ipc_fd){.fd=fd,.rights=RIGHTS};
    struct pacha_ipc_msg msg={.fds=transfers,.fd_count=19};
    check(pacha_ipc_message_syscall(PACHA_IPC_SYSCALL_SEND,pair[0],&msg,0,0,0,0),10);
}
int main(void) {
    /* Calibrate elapsed TSC against monotonic ns outside measured spans. */
    uint64_t n0=now(),t0=ticks(),n1;
    do { n1=now(); } while(n1-n0<100000000ull);
    uint64_t dt=ticks()-t0,dn=n1-n0;
    logstr("NATIVE_VMO calibration_ticks=");number(dt);logstr(" calibration_ns=");number(dn);logstr("\n");
    long persistent=-1; uint64_t pair[2]={0};
    if(BENCH_CASE==3) { persistent=create(PAGES);edges(persistent,pair);stress(persistent,pair); }
    for(unsigned trial=0;trial<TRIALS+3;trial++) {
        long fd=persistent>=0?persistent:create(PAGES);
        volatile unsigned char *a=0,*b=0;
        uint64_t begin,end,ops=1;
        if(BENCH_CASE==0) {
            /* Successful GET_INFO takes K and the ordinary dispatcher exit. */
            check(pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO,fd,(uintptr_t)&info),11);
            ops=65536;begin=ticks();
            for(unsigned j=0;j<ops;j++)check(pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO,fd,(uintptr_t)&info),12);
            end=ticks();
        } else {
            a=map(fd);b=map(fd);
            if(BENCH_CASE==1) { ops=PAGES;begin=ticks();fault_touch(a);end=ticks();verify(b,0x6b); }
#if BENCH_CASE >= 2
            else {
                if(BENCH_CASE!=6) { touch(a,0x5a);verify(b,0x5a); }
                if(BENCH_CASE==4) {
                    discard(fd);ops=PAGES;begin=ticks();
                    /* Check the zero value before each store, so stale backing
                     * fails instead of silently becoming a performance result. */
                    fault_touch(a);
                    end=ticks();verify(b,0x6b);
                } else {
                    /* Case 5 leaves owned backing but removes all present PTEs.
                     * It measures detach + exit reclaim, never reclaim alone.
                     * Case 6 is its empty-backing control with identical maps. */
                    if(BENCH_CASE==5 || BENCH_CASE==6) { unmap(a);unmap(b);a=b=0; }
                    begin=ticks();discard(fd);end=ticks();
                    if(a){verify(a,0);verify(b,0);}
                }
            }
#endif
        }
        if(trial>=3)samples[trial-3]=(typeof(samples[0])){end-begin,ops};
        if(a)unmap(a);if(b)unmap(b);if(fd!=persistent)closefd(fd);
    }
    if(persistent>=0) {
        /* No further timed operations; teardown releases queued transfer edges. */
        closefd(pair[0]);closefd(pair[1]);
        for(unsigned i=0;i<view_count;i++)closefd(views[i]);
        for(unsigned i=0;i<duplicate_count;i++)closefd(duplicates[i]);
        closefd(persistent);
    }
    logstr("NATIVE_VMO views=");number(view_count);logstr(" view_error_u64=");number(view_error);
    logstr(" fd_edges=");number(duplicate_count);logstr(" ipc_edges=");number(BENCH_CASE==3?19:0);logstr("\n");
    for(unsigned i=0;i<TRIALS;i++) { logstr("NATIVE_VMO case=");number(BENCH_CASE);logstr(" trial=");number(i+1);logstr(" ops=");number(samples[i].ops);logstr(" ticks=");number(samples[i].ticks);logstr("\n"); }
    logstr("NATIVE_VMO=PASS\n");return 0;
}
