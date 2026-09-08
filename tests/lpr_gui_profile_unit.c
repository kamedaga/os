#include <assert.h>
#include <stdint.h>
#include <string.h>
#define LPR_GUI_PROFILE 1
#define LPR_LINUX_CLOCK_MONOTONIC 1
#define PACHAOS_SYSCALL_LOG 1
#define lpr_memset memset
#define lpr_memcpy memcpy
static size_t lpr_strnlen(const char *s, size_t n)
{ size_t i=0; while (i<n && s[i]) ++i; return i; }
#define lpr_strrchr strrchr
#define lpr_strcmp strcmp
struct pachaos_timespec { uint64_t tv_sec, tv_nsec; };
static int64_t lpr_linux_write(uint64_t fd, uint64_t a, uint64_t b)
{ assert(fd == 2 && b < 256); (void)a; return (int64_t)b; }
static int lpr_pacha_clock_gettime(uint64_t c, struct pachaos_timespec *t)
{ (void)c; t->tv_sec = 1; t->tv_nsec = 0; return 0; }
static int64_t lpr_linux_getpid(void) { return 1; }
static int64_t lpr_linux_gettid(void) { return 1; }
static uint64_t pacha_trace_read_tsc(void) { return 1000; }
static int64_t lpr_linux_readlinkat_to_buffer(uint64_t a, uint64_t b, char *p, uint64_t n)
{ (void)a; (void)b; (void)p; (void)n; return -1; }
#include "../userland/personality/linux/runtime/lpr_gui_profile.h"
int main(void)
{
    lpr_gui_init();
    assert(!lpr_gui_selected);
    lpr_gui_start = 1000;
    lpr_gui_record(9, 1010, 1000 + LPR_GUI_BIN_CYCLES + 20);
    assert(lpr_gui_bins[0][9].cycles == LPR_GUI_BIN_CYCLES - 10);
    assert(lpr_gui_bins[1][9].cycles == 20);
    assert(lpr_gui_bins[0][9].count == 1 && lpr_gui_bins[1][9].count == 0);
    lpr_gui_record(9, 999, 1001);
    lpr_gui_record(9, 2000, 1000);
    assert(lpr_gui_bins[0][9].count == 1);
    const uint64_t limit = 1000 + LPR_GUI_BIN_CYCLES * LPR_GUI_BINS;
    lpr_gui_record(17, limit - 10, limit + 10);
    assert(lpr_gui_bins[LPR_GUI_BINS-1][17].cycles == 10);
    assert(lpr_gui_kind(9, 0x20) == 8 && lpr_gui_kind(9, 0) == 7);
    assert(lpr_gui_kind(262, 0) == 4 && lpr_gui_kind(999, 0) == 23);
    char path[256];
    memset(path,'x',sizeof(path)); path[sizeof(path)-1] = 0;
    for (unsigned i=0;i<300;++i)
        lpr_gui_profile_file(8,0,2,0,0,0,path);
    assert(lpr_gui_file_count == 5);
    assert(lpr_gui_files[3].ordinal == 128 && lpr_gui_files[4].ordinal == 256);
    assert(strlen(lpr_gui_files[0].path) == 127);
    lpr_gui_profile_span(0,1010,1020);
    assert(lpr_gui_bins[0][24].cycles == 10 && lpr_gui_bins[0][24].count == 1);
    lpr_gui_profile_span(LPR_GUI_WAIT_DRAIN,1020,1040);
    assert(lpr_gui_bins[0][39].cycles == 20);
    lpr_gui_profile_span(16,1020,1040); /* out-of-range detail is ignored */
    lpr_gui_profile_peer(7,99,"\0/tmp/dbus\n",11);
    assert(strcmp(lpr_gui_peers[0].path,"?/tmp/dbus?") == 0);
    for (unsigned i = 0; i < 64; ++i) lpr_gui_profile_peer(i,100+i,path,sizeof(path));
    assert(lpr_gui_peer_count == 64 && lpr_gui_peer_dropped == 1);
    assert(strlen(lpr_gui_peers[1].path) == 127);
    for (unsigned i = 0; i < 2049; ++i)
        lpr_gui_profile_wait(1000,1010,7,1,2,1,UINT64_MAX,-5,99,1);
    assert(lpr_gui_wait_count == 2048 && lpr_gui_wait_dropped == 1);
    assert(lpr_gui_waits[0].status == (uint64_t)-5);
    assert(lpr_gui_waits[0].handle == lpr_gui_peers[0].handle);
    lpr_gui_dump();
    assert(lpr_gui_dumped);
    return 0;
}
