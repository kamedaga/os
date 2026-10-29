/* Disposable startup timing probe. All timestamps share CLOCK_MONOTONIC.
 * Keep hot-path data in process memory; serial output occurs on exit or once
 * after the wallpaper window. This deliberately measures only the main thread.
 */
#if defined(LPR_EPHY_PROFILE) && LPR_EPHY_PROFILE
enum { EPHY_NR_MAX = 450, EPHY_WAIT_MAX = 4096 };
static struct { uint64_t cycles, count; } ephy_sys[EPHY_NR_MAX];
static struct {
    uint64_t nr, fd, a1, a2, a3, start, end;
    int64_t result;
#if defined(LPR_EPHY_SOCKETS) && LPR_EPHY_SOCKETS
    uint64_t timeout_ns;
    int32_t pollfd[8];
    uint16_t revents[8];
    unsigned peer[8], type[8], nfds;
#endif
} ephy_wait[EPHY_WAIT_MAX];
static unsigned ephy_wait_count, ephy_wait_dropped, ephy_dumped, ephy_probe_calls;
static uint64_t ephy_pid, ephy_tid, ephy_ppid, ephy_start_ns, ephy_start_tsc;
static unsigned ephy_selected, ephy_timed;
/* Browser processes are killed by the harness, so they dump once after this. */
static const uint64_t ephy_timed_dump_cycles = 37540000000ull; /* ~10 s */
static char ephy_exe[96];
#if defined(LPR_EPHY_SOCKETS) && LPR_EPHY_SOCKETS
enum { EPHY_PEERS=128, EPHY_FDS=512, EPHY_SOCKET_EVENTS=8192 };
static char ephy_peer[EPHY_PEERS][96];
static unsigned ephy_peer_count;
static struct { uint64_t fd; unsigned peer; } ephy_fd[EPHY_FDS];
static unsigned ephy_fd_count;
static struct { uint64_t nr, fd, peer, start, end; int64_t result; }
    ephy_socket_events[EPHY_SOCKET_EVENTS];
static uint64_t ephy_socket_count;
#endif

static char *ephy_put_u(char *p, uint64_t value)
{
    char reversed[20]; unsigned n = 0;
    do { reversed[n++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (n) *p++ = reversed[--n];
    return p;
}
static char *ephy_put_s(char *p, int64_t value)
{
    if (value < 0) { *p++ = '-'; return ephy_put_u(p, (uint64_t)(-(value+1))+1u); }
    return ephy_put_u(p, (uint64_t)value);
}
static char *ephy_put_text(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}
static void ephy_emit(char *begin, char *end)
{
    *end++ = '\n';
    (void)lpr_linux_write(2, (uint64_t)(uintptr_t)begin,
                          (uint64_t)(end-begin), 0);
}
static uint64_t ephy_now_ns(void)
{
    struct pachaos_timespec t = {0};
    if (lpr_pacha_clock_gettime(LPR_LINUX_CLOCK_MONOTONIC, &t)) return 0;
    return t.tv_sec * 1000000000ull + t.tv_nsec;
}
static void ephy_begin(void)
{
    const uint64_t pid = (uint64_t)lpr_linux_getpid();
    if (pid == ephy_pid) return;
    ephy_pid = pid;
    ephy_selected = 0;
    ephy_tid = (uint64_t)lpr_linux_gettid();
    ephy_ppid = (uint64_t)lpr_linux_getppid();
    ephy_wait_count = ephy_wait_dropped = ephy_dumped = ephy_probe_calls = 0;
    lpr_memset(ephy_sys, 0, sizeof(ephy_sys));
    lpr_memset(ephy_exe, 0, sizeof(ephy_exe));
#if defined(LPR_EPHY_SOCKETS) && LPR_EPHY_SOCKETS
    ephy_peer_count=ephy_fd_count=0;
    ephy_socket_count=0;
    lpr_memset(ephy_fd,0,sizeof(ephy_fd));
#endif
    const int64_t n = lpr_linux_readlinkat_to_buffer(
        (uint64_t)-100ll, (uint64_t)(uintptr_t)"/proc/self/exe",
        ephy_exe, sizeof(ephy_exe)-1u);
    if (n <= 0) ephy_exe[0] = '?';
    const char *base = lpr_strrchr(ephy_exe, '/');
    base = base ? base + 1 : ephy_exe;
    const char *names[] = {"epiphany", "WebKitWebProcess", "WebKitNetworkProcess",
        "dbus-daemon", "dbus-run-session", "xdg-dbus-proxy", "bwrap", "Xorg"};
    ephy_timed = 0;
    for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); ++i)
        if (!lpr_strcmp(base, names[i])) { ephy_selected = 1; ephy_timed = i < 3; }
    if (!ephy_selected) return;
    ephy_start_ns = ephy_now_ns();
    ephy_start_tsc = pacha_trace_read_tsc();
    char line[256], *p = ephy_put_text(line, "[ephy-prof] begin ");
    p = ephy_put_u(p, pid); *p++ = ' ';
    p = ephy_put_u(p, ephy_ppid); *p++ = ' ';
    p = ephy_put_u(p, ephy_tid); *p++ = ' ';
    p = ephy_put_u(p, ephy_start_ns); *p++ = ' ';
    p = ephy_put_u(p, ephy_start_tsc); *p++ = ' ';
    p = ephy_put_text(p, ephy_exe);
    ephy_emit(line, p);
}
static int ephy_main(void)
{
    return ephy_selected && ephy_pid && (uint64_t)lpr_linux_gettid() == ephy_tid;
}
static int ephy_is_wait(uint64_t nr)
{
    switch (nr) {
    case 7: case 23: case 35: case 42: case 45: case 47:
    case 202: case 230: case 232: case 270: case 271: case 281:
        return 1;
    default: return 0;
    }
}
#if defined(LPR_EPHY_SOCKETS) && LPR_EPHY_SOCKETS
static unsigned ephy_fd_peer(uint64_t fd)
{
    for (unsigned i=0;i<ephy_fd_count;++i)
        if (ephy_fd[i].fd==fd) return ephy_fd[i].peer;
    return 0;
}
static unsigned ephy_fd_type(uint64_t fd)
{
    lpr_fd_pin_t pin={0};
    if (lpr_fd_table_pin(&lpr_control_fd_table,(uint32_t)fd,&pin)) return 0;
    const unsigned type=pin.ops_id;
    lpr_fd_unpin(&pin);
    return type;
}
static void ephy_set_fd_peer(uint64_t fd, unsigned peer)
{
    if (!peer) return;
    for (unsigned i=0;i<ephy_fd_count;++i)
        if (ephy_fd[i].fd==fd) { ephy_fd[i].peer=peer; return; }
    if (ephy_fd_count<EPHY_FDS) {
        ephy_fd[ephy_fd_count].fd=fd;
        ephy_fd[ephy_fd_count++].peer=peer;
    }
}
static unsigned ephy_add_peer(uint64_t addr, uint64_t length)
{
    if (!addr || length<3 || length>256) return 0;
    const unsigned char *p=(const unsigned char *)(uintptr_t)addr;
    if (p[0]!=1 || p[1]!=0) return 0; /* AF_UNIX, little endian. */
    char name[96]; unsigned n=0;
    if (p[2]==0) name[n++]='@';
    for (uint64_t i=2+(p[2]==0);i<length && n<95 && p[i];++i) {
        unsigned char c=p[i]; name[n++]=(c>=32 && c<127) ? (char)c : '?';
    }
    name[n]=0;
    for (unsigned i=0;i<ephy_peer_count;++i)
        if (!lpr_strcmp(ephy_peer[i],name)) return i+1;
    if (ephy_peer_count==EPHY_PEERS) return 0;
    lpr_memcpy(ephy_peer[ephy_peer_count],name,n+1);
    return ++ephy_peer_count;
}
static void ephy_socket_record(uint64_t nr,uint64_t fd,uint64_t a1,
    uint64_t a2,uint64_t start,uint64_t end,int64_t result)
{
    if (!ephy_main() || ephy_dumped) return;
    unsigned peer=ephy_fd_peer(fd);
    if ((nr==42 || nr==49) && result==0) {
        peer=ephy_add_peer(a1,a2);
        ephy_set_fd_peer(fd,peer);
    } else if ((nr==43 || nr==288) && result>=0) {
        ephy_set_fd_peer((uint64_t)result,peer);
    } else if (nr==3) {
        for (unsigned i=0;i<ephy_fd_count;++i)
            if (ephy_fd[i].fd==fd) { ephy_fd[i]=ephy_fd[--ephy_fd_count]; break; }
    }
    if (!peer || !(nr==42 || nr==49 || nr==43 || nr==288 ||
       nr==0 || nr==1 || nr==19 || nr==20 || nr==44 || nr==45 ||
       nr==46 || nr==47 || nr==299 || nr==307)) return;
    const unsigned slot=(unsigned)(ephy_socket_count++ % EPHY_SOCKET_EVENTS);
    ephy_socket_events[slot].nr=nr;
    ephy_socket_events[slot].fd=fd;
    ephy_socket_events[slot].peer=peer;
    ephy_socket_events[slot].start=start;
    ephy_socket_events[slot].end=end;
    ephy_socket_events[slot].result=result;
}
#endif
static void ephy_record(uint64_t nr, uint64_t fd, uint64_t a1,
                        uint64_t a2, uint64_t a3, uint64_t start,
                        uint64_t end, int64_t result)
{
    if (!ephy_main() || end < start) return;
    if (nr < EPHY_NR_MAX) {
        ephy_sys[nr].cycles += end-start;
        ephy_sys[nr].count++;
    }
    if (ephy_is_wait(nr) && end-start >= 100000000ull) {
        if (ephy_wait_count < EPHY_WAIT_MAX) {
            const unsigned i = ephy_wait_count++;
            ephy_wait[i].nr=nr; ephy_wait[i].fd=fd;
            ephy_wait[i].a1=a1; ephy_wait[i].a2=a2;
            ephy_wait[i].a3=a3; ephy_wait[i].start=start;
            ephy_wait[i].end=end; ephy_wait[i].result=result;
#if defined(LPR_EPHY_SOCKETS) && LPR_EPHY_SOCKETS
            ephy_wait[i].nfds=0;
            ephy_wait[i].timeout_ns=UINT64_MAX;
            if ((nr==7 || nr==271) && result>=0 && fd>0x10000u && a1<=8u) {
                const unsigned char *pollfds=(const unsigned char *)(uintptr_t)fd;
                ephy_wait[i].nfds=(unsigned)a1;
                for (unsigned j=0;j<a1;++j) {
                    int32_t f; uint16_t rev;
                    lpr_memcpy(&f,pollfds+j*8u,4u);
                    lpr_memcpy(&rev,pollfds+j*8u+6u,2u);
                    ephy_wait[i].pollfd[j]=f;
                    ephy_wait[i].revents[j]=rev;
                    ephy_wait[i].peer[j]=f>=0 ? ephy_fd_peer((uint64_t)f) : 0;
                    ephy_wait[i].type[j]=f>=0 ? ephy_fd_type((uint64_t)f) : 0;
                }
            }
            const uint64_t timeout_ptr=(nr==35) ? fd :
                ((nr==230 || nr==271) ? a2 : 0);
            if (timeout_ptr>0x10000u && result!=-LPR_LINUX_EFAULT) {
                int64_t sec,nsec;
                lpr_memcpy(&sec,(const void *)(uintptr_t)timeout_ptr,8u);
                lpr_memcpy(&nsec,(const void *)(uintptr_t)(timeout_ptr+8u),8u);
                if (sec>=0 && sec<3600 && nsec>=0 && nsec<1000000000)
                    ephy_wait[i].timeout_ns=(uint64_t)sec*1000000000u+(uint64_t)nsec;
            }
#endif
        } else ++ephy_wait_dropped;
    }
}
static void ephy_dump(uint64_t reason, uint64_t exit_status);
static void ephy_timed_dump(void)
{
    if (ephy_timed && !ephy_dumped && pacha_trace_read_tsc() - ephy_start_tsc >= ephy_timed_dump_cycles)
        ephy_dump(2, 0);
}
static void ephy_dump(uint64_t reason, uint64_t exit_status)
{
    if (!ephy_main() || ephy_dumped) return;
    ephy_dumped = 1;
    char line[512], *p = ephy_put_text(line, "[ephy-prof] end ");
    p=ephy_put_u(p,ephy_pid); *p++=' ';
    p=ephy_put_u(p,reason); *p++=' ';
    p=ephy_put_u(p,exit_status); *p++=' ';
    p=ephy_put_u(p,ephy_now_ns()); *p++=' ';
    p=ephy_put_u(p,pacha_trace_read_tsc()); *p++=' ';
    p=ephy_put_u(p,ephy_wait_count); *p++=' ';
    p=ephy_put_u(p,ephy_wait_dropped); ephy_emit(line,p);
    for (unsigned nr=0;nr<EPHY_NR_MAX;++nr) {
        if (!ephy_sys[nr].count) continue;
        p=ephy_put_text(line,"[ephy-prof] sys ");
        p=ephy_put_u(p,ephy_pid); *p++=' ';
        p=ephy_put_u(p,nr); *p++=' ';
        p=ephy_put_u(p,ephy_sys[nr].count); *p++=' ';
        p=ephy_put_u(p,ephy_sys[nr].cycles); ephy_emit(line,p);
    }
    for (unsigned i=0;i<ephy_wait_count;++i) {
        p=ephy_put_text(line,"[ephy-prof] wait ");
        p=ephy_put_u(p,ephy_pid); *p++=' ';
        p=ephy_put_u(p,ephy_wait[i].nr); *p++=' ';
        p=ephy_put_u(p,ephy_wait[i].fd); *p++=' ';
        p=ephy_put_u(p,ephy_wait[i].a1); *p++=' ';
        p=ephy_put_u(p,ephy_wait[i].a2); *p++=' ';
        p=ephy_put_u(p,ephy_wait[i].a3); *p++=' ';
        p=ephy_put_u(p,ephy_wait[i].start); *p++=' ';
        p=ephy_put_u(p,ephy_wait[i].end); *p++=' ';
        p=ephy_put_s(p,ephy_wait[i].result); ephy_emit(line,p);
#if defined(LPR_EPHY_SOCKETS) && LPR_EPHY_SOCKETS
        if (ephy_wait[i].nfds || ephy_wait[i].timeout_ns!=UINT64_MAX) {
            p=ephy_put_text(line,"[ephy-net] waitdetail ");
            p=ephy_put_u(p,ephy_pid); *p++=' ';
            p=ephy_put_u(p,i); *p++=' ';
            p=ephy_put_u(p,ephy_wait[i].timeout_ns);
            for (unsigned j=0;j<ephy_wait[i].nfds;++j) {
                *p++=' ';
                p=ephy_put_s(p,ephy_wait[i].pollfd[j]); *p++=':';
                p=ephy_put_u(p,ephy_wait[i].peer[j]); *p++=':';
                p=ephy_put_u(p,ephy_wait[i].revents[j]); *p++=':';
                p=ephy_put_u(p,ephy_wait[i].type[j]);
            }
            ephy_emit(line,p);
        }
#endif
    }
#if defined(LPR_EPHY_SOCKETS) && LPR_EPHY_SOCKETS
    for (unsigned i=0;i<ephy_peer_count;++i) {
        p=ephy_put_text(line,"[ephy-net] peer ");
        p=ephy_put_u(p,ephy_pid); *p++=' ';
        p=ephy_put_u(p,i+1); *p++=' ';
        p=ephy_put_text(p,ephy_peer[i]); ephy_emit(line,p);
    }
    const uint64_t first=ephy_socket_count>EPHY_SOCKET_EVENTS ?
        ephy_socket_count-EPHY_SOCKET_EVENTS : 0;
    for (uint64_t i=first;i<ephy_socket_count;++i) {
        const unsigned slot=(unsigned)(i%EPHY_SOCKET_EVENTS);
        p=ephy_put_text(line,"[ephy-net] event ");
        p=ephy_put_u(p,ephy_pid); *p++=' ';
        p=ephy_put_u(p,ephy_socket_events[slot].nr); *p++=' ';
        p=ephy_put_u(p,ephy_socket_events[slot].fd); *p++=' ';
        p=ephy_put_u(p,ephy_socket_events[slot].peer); *p++=' ';
        p=ephy_put_u(p,ephy_socket_events[slot].start); *p++=' ';
        p=ephy_put_u(p,ephy_socket_events[slot].end); *p++=' ';
        p=ephy_put_s(p,ephy_socket_events[slot].result); ephy_emit(line,p);
    }
    p=ephy_put_text(line,"[ephy-net] dropped ");
    p=ephy_put_u(p,ephy_pid); *p++=' ';
    p=ephy_put_u(p,first); ephy_emit(line,p);
#endif
}
#endif
