#include "vfs_protocol.h"

enum {
    SYSCALL_ALLOC_PAGE = 0x1,
    SYSCALL_LOG = 0x9,
    SYSCALL_MAP_PAGE = 0x2,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_GRANT_CAP_ON_ENDPOINT = 0x24,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_ACCEPT_CAP_TRANSFER = 0x2A,
    SYSCALL_SHARE_CAP = 0x2B,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_GET_TICK_COUNT = 0x2D,
    SYSCALL_GET_PROCESS_SLOT = 0x2E,
    SYSCALL_GET_PROCESS_HANDLE = 0x5C,
    SYSCALL_GET_MEMORY_STATS = 0x3C,
    SYSCALL_GET_RTC_UNIX_TIME = 0x3E,
    SYSCALL_OK = 0,
    SYSCALL_ERR_ENDPOINT = 9,
    PAGE_RIGHT_CPU_READ = 0x1,
    PAGE_RIGHT_CPU_WRITE = 0x2,
    CAP_TRANSFER_ID_MIN = 0x1000,
    VFS_CONFIG_VA = 0x3C002000,
    VFS_REQUEST_VA = 0x26000000,
    VFS_RESPONSE_VA = 0x26001000,
    VFS_SESSION_STRIDE_BYTES = 0x2000,
    VFS_MAX_SESSIONS = 8,
    VFS_FAT_REQUEST_VA = 0x26100000,
    VFS_FAT_RESPONSE_VA = 0x26101000,
    VFS_NET_REQUEST_VA = 0x26200000,
    VFS_NET_RESPONSE_VA = 0x26201000,
    VFS_REPLY_ENDPOINT_ID = 0xE0,
    VFS_NET_REPLY_ENDPOINT_ID = 0xEA,
    VFS_TOKEN_TAG = 1ULL << 63,
    VFS_BACKEND_TOKEN_BASE = 1ULL << 40,
    VFS_ROOT_OBJECT_ID = 1,
    VFS_DEV_OBJECT_ID = 2,
    VFS_PROC_OBJECT_ID = 3,
    VFS_TMP_OBJECT_ID = 4,
    VFS_RUN_OBJECT_ID = 5,
    VFS_DEV_NULL_OBJECT_ID = 6,
    VFS_DEV_ZERO_OBJECT_ID = 7,
    VFS_DEV_NULL_OPEN_OBJECT_ID = 8,
    VFS_DEV_ZERO_OPEN_OBJECT_ID = 9,
    VFS_PROC_CPUINFO_OBJECT_ID = 10,
    VFS_PROC_MEMINFO_OBJECT_ID = 11,
    VFS_PROC_UPTIME_OBJECT_ID = 12,
    VFS_PROC_STAT_OBJECT_ID = 13,
    VFS_PROC_SELF_OBJECT_ID = 14,
    VFS_PROC_SELF_EXE_OBJECT_ID = 15,
    VFS_PROC_CPUINFO_OPEN_OBJECT_ID = 16,
    VFS_PROC_MEMINFO_OPEN_OBJECT_ID = 17,
    VFS_PROC_UPTIME_OPEN_OBJECT_ID = 18,
    VFS_PROC_STAT_OPEN_OBJECT_ID = 19,
    VFS_PROC_MOUNTS_OBJECT_ID = 20,
    VFS_PROC_SELF_STAT_OBJECT_ID = 21,
    VFS_PROC_SELF_STATUS_OBJECT_ID = 22,
    VFS_PROC_MOUNTS_OPEN_OBJECT_ID = 23,
    VFS_PROC_SELF_STAT_OPEN_OBJECT_ID = 24,
    VFS_PROC_SELF_STATUS_OPEN_OBJECT_ID = 25,
    VFS_PROC_NET_OBJECT_ID = 26,
    VFS_PROC_NET_DEV_OBJECT_ID = 27,
    VFS_PROC_NET_ROUTE_OBJECT_ID = 28,
    VFS_PROC_NET_DEV_OPEN_OBJECT_ID = 29,
    VFS_PROC_NET_ROUTE_OPEN_OBJECT_ID = 30,
    VFS_PROC_NET_CAPABILITYOS_OBJECT_ID = 31,
    VFS_PROC_NET_CAPABILITYOS_OPEN_OBJECT_ID = 32,
    VFS_DEV_RANDOM_OBJECT_ID = 33,
    VFS_DEV_URANDOM_OBJECT_ID = 34,
    VFS_DEV_RANDOM_OPEN_OBJECT_ID = 35,
    VFS_DEV_URANDOM_OPEN_OBJECT_ID = 36,
    VFS_PROC_DRIVER_OBJECT_ID = 37,
    VFS_PROC_DRIVER_RTC_OBJECT_ID = 38,
    VFS_PROC_DRIVER_RTC_OPEN_OBJECT_ID = 39,
    VFS_APK_DB_OBJECT_ID = 40,
    VFS_APK_CACHE_OBJECT_ID = 41,
    VFS_TMPFS_FILE_OBJECT_ID_BASE = 0x1000,
    VFS_TMPFS_OPEN_OBJECT_ID_BASE = 0x2000,
    VFS_TMPFS_MAX_FILES = 16,
    VFS_TMPFS_FILE_BYTES = 8 * 1024 * 1024,
    VFS_TMPFS_PAGES_PER_FILE = VFS_TMPFS_FILE_BYTES / FS_PAGE_BYTES,
    VFS_TMPFS_STORAGE_VA = 0x30000000,
    VFS_DIR_MODE = 0x4000,
    VFS_FILE_MODE = 0x8000,
    VFS_CREATE_FLAG_DIRECTORY = 1 << 0,
    VFS_CREATE_FLAG_TRUNCATE = 1 << 1,

    NET_PROTOCOL_REQUEST_MAGIC = 0x514E4554,
    NET_PROTOCOL_RESPONSE_MAGIC = 0x524E4554,
    NET_PROTOCOL_VERSION = 1,
    NET_OP_CONNECT = 1,
    NET_OP_GET_STATUS = 2,
    NET_STATUS_OK = 0,
    NET_FLAG_LINK_UP = 1 << 0,
    NET_FLAG_DHCP_BOUND = 1 << 1,
    NET_FLAG_GATEWAY_ARP = 1 << 2,
    PROC_MONOTONIC_NS_PER_TICK = 1000000,
};

struct vfs_mount_entry {
    u64 object_id;
    const char *name;
    u16 name_len;
};

struct vfs_builtin_file {
    u64 object_id;
    u64 open_object_id;
    const char *name;
    u16 name_len;
};

struct vfs_tmpfs_file {
    u8 used;
    u8 reserved0[7];
    u64 parent_object_id;
    u16 name_len;
    u16 reserved1;
    u32 size;
    char name[FS_MAX_PATH_BYTES + 1];
    u64 page_paddrs[VFS_TMPFS_PAGES_PER_FILE];
};

struct vfs_session {
    u8 active;
    u8 reserved0[7];
    u64 request_va;
    u64 response_va;
    u64 request_paddr;
    u64 response_paddr;
    u64 reply_endpoint_id;
    u64 session_nonce;
    u64 last_completed_seq;
    u64 root_token;
};

struct vfs_backend_session {
    u8 active;
    u8 reserved0[7];
    u64 endpoint_id;
    u64 process_handle;
    u64 request_va;
    u64 response_va;
    u64 request_paddr;
    u64 response_paddr;
    u64 session_nonce;
    u64 root_token;
    u64 next_seq;
};

struct net_request_header {
    u32 magic;
    u16 version;
    u16 op;
    u64 request_seq;
    u64 session_nonce;
    u64 arg0;
    u64 arg1;
    u64 arg2;
    u64 reserved0;
};

struct net_response_header {
    u32 magic;
    u16 version;
    u16 op;
    u64 response_seq;
    int status;
    u32 inline_bytes;
    u64 arg0;
    u64 arg1;
    u64 arg2;
    u64 reserved0;
};

struct net_status_payload {
    u8 mac[6];
    u8 link_up;
    u8 dhcp_bound;
    u32 ipv4_addr;
    u32 gateway_addr;
    u32 dns_addr;
    u32 dhcp_server_addr;
    u32 flags;
    u64 rx_packets;
    u64 tx_completions;
    u64 tcp_rx_segments;
    u64 tcp_rx_payload_bytes;
    u64 tcp_rx_in_order_bytes;
    u64 tcp_rx_duplicate_segments;
    u64 tcp_rx_out_of_order_segments;
    u64 tcp_rx_ooo_stored;
    u64 tcp_rx_ooo_drained;
    u64 tcp_rx_ooo_dropped;
    u64 tcp_rx_append_failed;
    u64 tcp_ack_sent;
    u64 tcp_ack_deferred;
    u64 tcp_ack_flushed;
    u64 tcp_tx_busy;
    u64 tcp_connect_requests;
    u64 tcp_connect_established;
    u64 tcp_tx_segments;
    u64 tcp_tx_payload_bytes;
    u64 tcp_read_requests;
    u64 tcp_read_would_block;
    u64 tcp_read_bytes;
    u64 tcp_poll_requests;
    u64 tcp_poll_readable;
    u64 tcp_rx_syn_ack;
    u64 tcp_rx_fin;
    u64 tcp_rx_rst;
    u64 tcp_active_connections;
    u64 tcp_established_connections;
    u64 tcp_rx_buffered_bytes;
    u64 tcp_rx_buffer_max_bytes;
    u64 tcp_ack_pending_connections;
    u64 net_service_requests;
    u64 net_service_work_loops;
    u64 net_service_idle_sleeps;
};

struct net_backend_session {
    u8 active;
    u8 reserved0[7];
    u64 endpoint_id;
    u64 process_handle;
    u64 request_va;
    u64 response_va;
    u64 request_paddr;
    u64 response_paddr;
    u64 session_nonce;
    u64 next_seq;
    struct net_status_payload last_status;
};

static struct vfs_session g_sessions[VFS_MAX_SESSIONS];
static struct vfs_session *g_session;
static struct vfs_backend_session g_root_backend;
static struct net_backend_session g_net_backend;
static struct vfs_tmpfs_file g_tmpfs_files[VFS_TMPFS_MAX_FILES];
static u64 g_endpoint_id;
static u64 g_net_endpoint_id;
static u64 g_net_process_slot;
static const struct vfs_mount_entry g_root_mounts[] = {
    { VFS_DEV_OBJECT_ID, "dev", 3 },
    { VFS_PROC_OBJECT_ID, "proc", 4 },
    { VFS_TMP_OBJECT_ID, "tmp", 3 },
    { VFS_RUN_OBJECT_ID, "run", 3 },
};
static const struct vfs_builtin_file g_dev_files[] = {
    { VFS_DEV_NULL_OBJECT_ID, VFS_DEV_NULL_OPEN_OBJECT_ID, "null", 4 },
    { VFS_DEV_ZERO_OBJECT_ID, VFS_DEV_ZERO_OPEN_OBJECT_ID, "zero", 4 },
    { VFS_DEV_RANDOM_OBJECT_ID, VFS_DEV_RANDOM_OPEN_OBJECT_ID, "random", 6 },
    { VFS_DEV_URANDOM_OBJECT_ID, VFS_DEV_URANDOM_OPEN_OBJECT_ID, "urandom", 7 },
};
static const struct vfs_builtin_file g_proc_files[] = {
    { VFS_PROC_CPUINFO_OBJECT_ID, VFS_PROC_CPUINFO_OPEN_OBJECT_ID, "cpuinfo", 7 },
    { VFS_PROC_MEMINFO_OBJECT_ID, VFS_PROC_MEMINFO_OPEN_OBJECT_ID, "meminfo", 7 },
    { VFS_PROC_UPTIME_OBJECT_ID, VFS_PROC_UPTIME_OPEN_OBJECT_ID, "uptime", 6 },
    { VFS_PROC_STAT_OBJECT_ID, VFS_PROC_STAT_OPEN_OBJECT_ID, "stat", 4 },
    { VFS_PROC_MOUNTS_OBJECT_ID, VFS_PROC_MOUNTS_OPEN_OBJECT_ID, "mounts", 6 },
};
static const struct vfs_mount_entry g_proc_dirs[] = {
    { VFS_PROC_SELF_OBJECT_ID, "self", 4 },
    { VFS_PROC_NET_OBJECT_ID, "net", 3 },
    { VFS_PROC_DRIVER_OBJECT_ID, "driver", 6 },
};
static const struct vfs_builtin_file g_proc_self_files[] = {
    { VFS_PROC_SELF_EXE_OBJECT_ID, 0, "exe", 3 },
    { VFS_PROC_SELF_STAT_OBJECT_ID, VFS_PROC_SELF_STAT_OPEN_OBJECT_ID, "stat", 4 },
    { VFS_PROC_SELF_STATUS_OBJECT_ID, VFS_PROC_SELF_STATUS_OPEN_OBJECT_ID, "status", 6 },
};
static const struct vfs_builtin_file g_proc_net_files[] = {
    { VFS_PROC_NET_DEV_OBJECT_ID, VFS_PROC_NET_DEV_OPEN_OBJECT_ID, "dev", 3 },
    { VFS_PROC_NET_ROUTE_OBJECT_ID, VFS_PROC_NET_ROUTE_OPEN_OBJECT_ID, "route", 5 },
    { VFS_PROC_NET_CAPABILITYOS_OBJECT_ID, VFS_PROC_NET_CAPABILITYOS_OPEN_OBJECT_ID, "capabilityos", 12 },
};
static const struct vfs_builtin_file g_proc_driver_files[] = {
    { VFS_PROC_DRIVER_RTC_OBJECT_ID, VFS_PROC_DRIVER_RTC_OPEN_OBJECT_ID, "rtc", 3 },
};
static char g_proc_cpuinfo_buf[8192];
static char g_proc_meminfo_buf[256];
static char g_proc_uptime_buf[64];
static char g_proc_net_dev_buf[512];
static char g_proc_net_route_buf[384];
static char g_proc_net_capabilityos_buf[3072];
static char g_proc_driver_rtc_buf[384];
static char g_proc_stat_buf[4096];
static const char g_proc_mounts[] =
    "rootfs / fat32 rw 0 0\n"
    "proc /proc proc rw,nosuid,nodev,noexec 0 0\n"
    "devtmpfs /dev devtmpfs rw,nosuid 0 0\n"
    "tmpfs /tmp tmpfs rw,nosuid,nodev 0 0\n"
    "tmpfs /run tmpfs rw,nosuid,nodev 0 0\n";
static const char g_proc_self_stat[] =
    "1 (dash) S 0 1 1 0 -1 4194304 0 0 0 0 1 0 0 0 20 0 1 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
static const char g_proc_self_status[] =
    "Name:\tdash\n"
    "Umask:\t0022\n"
    "State:\tS (sleeping)\n"
    "Tgid:\t1\n"
    "Ngid:\t0\n"
    "Pid:\t1\n"
    "PPid:\t0\n"
    "TracerPid:\t0\n"
    "Uid:\t0\t0\t0\t0\n"
    "Gid:\t0\t0\t0\t0\n"
    "Threads:\t1\n"
    "VmPeak:\t0 kB\n"
    "VmSize:\t0 kB\n"
    "VmRSS:\t0 kB\n"
    "RssAnon:\t0 kB\n"
    "RssFile:\t0 kB\n"
    "RssShmem:\t0 kB\n"
    "SigQ:\t0/0\n"
    "SigPnd:\t0000000000000000\n"
    "ShdPnd:\t0000000000000000\n"
    "SigBlk:\t0000000000000000\n"
    "SigIgn:\t0000000000000000\n"
    "SigCgt:\t0000000000000000\n"
    "CapInh:\t0000000000000000\n"
    "CapPrm:\t0000000000000000\n"
    "CapEff:\t0000000000000000\n"
    "CapBnd:\t0000000000000000\n"
    "CapAmb:\t0000000000000000\n";

static u64 cstr_len(const char *s) {
    u64 n = 0;
    while (s[n] != 0) n++;
    return n;
}

static int refresh_net_status(void);

static void append_char(char *buf, u64 cap, u64 *len, char ch) {
    if (*len + 1 >= cap) return;
    buf[*len] = ch;
    *len = *len + 1;
    buf[*len] = 0;
}

static void append_str(char *buf, u64 cap, u64 *len, const char *s) {
    for (u64 i = 0; s[i] != 0; i++) append_char(buf, cap, len, s[i]);
}

static void append_u64_dec(char *buf, u64 cap, u64 *len, u64 value) {
    char tmp[20];
    u64 n = 0;
    if (value == 0) {
        append_char(buf, cap, len, '0');
        return;
    }
    while (value != 0 && n < sizeof(tmp)) {
        tmp[n] = (char)('0' + (value % 10));
        value /= 10;
        n++;
    }
    while (n != 0) {
        n--;
        append_char(buf, cap, len, tmp[n]);
    }
}

static void append_u64_dec_width(char *buf, u64 cap, u64 *len, u64 value, u64 width) {
    char tmp[20];
    u64 n = 0;
    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value != 0 && n < sizeof(tmp)) {
            tmp[n] = (char)('0' + (value % 10));
            value /= 10;
            n++;
        }
    }
    while (n < width) {
        append_char(buf, cap, len, '0');
        width--;
    }
    while (n != 0) {
        n--;
        append_char(buf, cap, len, tmp[n]);
    }
}

static void append_hex_digit(char *buf, u64 cap, u64 *len, u8 value) {
    append_char(buf, cap, len, (char)(value < 10 ? ('0' + value) : ('A' + value - 10)));
}

static void append_hex_byte(char *buf, u64 cap, u64 *len, u8 value) {
    append_hex_digit(buf, cap, len, (u8)((value >> 4) & 0xF));
    append_hex_digit(buf, cap, len, (u8)(value & 0xF));
}

static void append_mac(char *buf, u64 cap, u64 *len, const u8 mac[6]) {
    for (u64 i = 0; i < 6; i++) {
        if (i != 0) append_char(buf, cap, len, ':');
        append_hex_byte(buf, cap, len, mac[i]);
    }
}

static void append_ipv4(char *buf, u64 cap, u64 *len, u32 addr) {
    append_u64_dec(buf, cap, len, (addr >> 24) & 0xFF);
    append_char(buf, cap, len, '.');
    append_u64_dec(buf, cap, len, (addr >> 16) & 0xFF);
    append_char(buf, cap, len, '.');
    append_u64_dec(buf, cap, len, (addr >> 8) & 0xFF);
    append_char(buf, cap, len, '.');
    append_u64_dec(buf, cap, len, addr & 0xFF);
}

static void append_route_hex_ipv4(char *buf, u64 cap, u64 *len, u32 addr) {
    append_hex_byte(buf, cap, len, (u8)(addr & 0xFF));
    append_hex_byte(buf, cap, len, (u8)((addr >> 8) & 0xFF));
    append_hex_byte(buf, cap, len, (u8)((addr >> 16) & 0xFF));
    append_hex_byte(buf, cap, len, (u8)((addr >> 24) & 0xFF));
}

static void append_meminfo_line(char *buf, u64 cap, u64 *len, const char *name, u64 kb) {
    append_str(buf, cap, len, name);
    append_str(buf, cap, len, ": ");
    append_u64_dec(buf, cap, len, kb);
    append_str(buf, cap, len, " kB\n");
}

struct cpuid_regs {
    u32 eax;
    u32 ebx;
    u32 ecx;
    u32 edx;
};

struct cpu_identity {
    char vendor[13];
    char brand[49];
    u64 logical_count;
    u64 core_count;
    u64 threads_per_core;
};

static void cpuid_read(u32 leaf, u32 subleaf, struct cpuid_regs *regs) {
    u32 eax;
    u32 ebx;
    u32 ecx;
    u32 edx;
    __asm__ volatile(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(subleaf)
        : "memory");
    regs->eax = eax;
    regs->ebx = ebx;
    regs->ecx = ecx;
    regs->edx = edx;
}

static void copy_cpuid_word(char *dst, u32 value) {
    dst[0] = (char)(value & 0xFF);
    dst[1] = (char)((value >> 8) & 0xFF);
    dst[2] = (char)((value >> 16) & 0xFF);
    dst[3] = (char)((value >> 24) & 0xFF);
}

static int cstr_has_non_space(const char *s) {
    for (u64 i = 0; s[i] != 0; i++) {
        if (s[i] != ' ') return 1;
    }
    return 0;
}

static void cstr_copy(char *dst, u64 cap, const char *src) {
    u64 i = 0;
    if (cap == 0) return;
    while (i + 1 < cap && src[i] != 0) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void trim_cpu_brand(char *s) {
    u64 first = 0;
    u64 len = cstr_len(s);
    while (first < len && s[first] == ' ') first++;
    if (first != 0) {
        u64 out = 0;
        while (first + out <= len) {
            s[out] = s[first + out];
            out++;
        }
        len = cstr_len(s);
    }
    while (len != 0 && s[len - 1] == ' ') {
        len--;
        s[len] = 0;
    }
}

static void cpuid_detect_topology(u32 leaf, u64 *logical_count, u64 *threads_per_core) {
    for (u32 subleaf = 0; subleaf < 8; subleaf++) {
        struct cpuid_regs regs;
        cpuid_read(leaf, subleaf, &regs);
        const u32 level_count = regs.ebx & 0xFFFF;
        const u32 level_type = (regs.ecx >> 8) & 0xFF;
        if (level_type == 0 || level_count == 0) break;
        if (level_type == 1) {
            *threads_per_core = level_count;
        } else if (level_type == 2) {
            *logical_count = level_count;
        }
    }
}

static void read_cpu_identity(struct cpu_identity *identity) {
    cstr_copy(identity->vendor, sizeof(identity->vendor), "PachaOS");
    cstr_copy(identity->brand, sizeof(identity->brand), "PachaOS Virtual CPU");
    identity->logical_count = 1;
    identity->core_count = 1;
    identity->threads_per_core = 1;

    struct cpuid_regs regs;
    cpuid_read(0, 0, &regs);
    const u32 max_basic_leaf = regs.eax;
    copy_cpuid_word(identity->vendor + 0, regs.ebx);
    copy_cpuid_word(identity->vendor + 4, regs.edx);
    copy_cpuid_word(identity->vendor + 8, regs.ecx);
    identity->vendor[12] = 0;

    cpuid_read(0x80000000U, 0, &regs);
    const u32 max_extended_leaf = regs.eax;
    if (max_extended_leaf >= 0x80000004U) {
        char brand[49];
        for (u32 i = 0; i < 3; i++) {
            cpuid_read(0x80000002U + i, 0, &regs);
            copy_cpuid_word(brand + i * 16 + 0, regs.eax);
            copy_cpuid_word(brand + i * 16 + 4, regs.ebx);
            copy_cpuid_word(brand + i * 16 + 8, regs.ecx);
            copy_cpuid_word(brand + i * 16 + 12, regs.edx);
        }
        brand[48] = 0;
        trim_cpu_brand(brand);
        if (cstr_has_non_space(brand)) {
            cstr_copy(identity->brand, sizeof(identity->brand), brand);
        }
    }

    u64 logical_count = 0;
    u64 threads_per_core = 1;
    if (max_basic_leaf >= 0x1FU) {
        cpuid_detect_topology(0x1FU, &logical_count, &threads_per_core);
    }
    if (logical_count == 0 && max_basic_leaf >= 0x0BU) {
        cpuid_detect_topology(0x0BU, &logical_count, &threads_per_core);
    }
    if (logical_count == 0 && max_basic_leaf >= 1) {
        cpuid_read(1, 0, &regs);
        logical_count = (regs.ebx >> 16) & 0xFF;
    }

    u64 core_count = 0;
    if (max_extended_leaf >= 0x80000008U) {
        cpuid_read(0x80000008U, 0, &regs);
        core_count = (regs.ecx & 0xFF) + 1;
    }
    if (logical_count == 0) logical_count = core_count;
    if (logical_count == 0) logical_count = 1;
    if (threads_per_core == 0) threads_per_core = 1;
    if (core_count == 0 || core_count > logical_count) {
        core_count = logical_count / threads_per_core;
        if (core_count == 0) core_count = logical_count;
    }
    if (core_count > logical_count) core_count = logical_count;

    identity->logical_count = logical_count;
    identity->core_count = core_count;
    identity->threads_per_core = threads_per_core;
}

static void user_log_len(const char *message, u64 len) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"((u64)SYSCALL_LOG), "D"((u64)message), "S"(len)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    (void)ret;
}

static void user_log(const char *message) {
    user_log_len(message, cstr_len(message));
}

static u64 session_request_va(u64 slot) {
    return VFS_REQUEST_VA + slot * VFS_SESSION_STRIDE_BYTES;
}

static u64 session_response_va(u64 slot) {
    return session_request_va(slot) + FS_PAGE_BYTES;
}

static struct vfs_session *find_session_by_request_paddr(u64 request_paddr) {
    for (u64 i = 0; i < VFS_MAX_SESSIONS; i++) {
        if (g_sessions[i].active && g_sessions[i].request_paddr == request_paddr) return &g_sessions[i];
    }
    return 0;
}

static struct vfs_session *alloc_session_slot(void) {
    for (u64 i = 0; i < VFS_MAX_SESSIONS; i++) {
        if (!g_sessions[i].active) return &g_sessions[i];
    }
    return 0;
}

static u64 session_slot(struct vfs_session *session) {
    return (u64)(session - g_sessions);
}

static u64 syscall2(u64 nr, u64 arg0, u64 arg1) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(arg0), "S"(arg1)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall0(u64 nr) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall1(u64 nr, u64 arg0) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(arg0)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 syscall3(u64 nr, u64 arg0, u64 arg1, u64 arg2) {
    u64 ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(arg0), "S"(arg1), "d"(arg2)
        : "rcx", "r8", "r9", "r10", "r11", "memory");
    return ret;
}

static u64 wait_event(void) {
    return syscall2(SYSCALL_WAIT_EVENT, 1, 1);
}

static u64 wait_event_poll(void) {
    return syscall2(SYSCALL_WAIT_EVENT, 0, 1);
}

static void clear_page(u64 va) {
    volatile u64 *p = (volatile u64 *)va;
    for (u64 i = 0; i < 512; i++) p[i] = 0;
}

static u64 tmpfs_page_va(u64 file_index, u64 page_index) {
    return VFS_TMPFS_STORAGE_VA + file_index * VFS_TMPFS_FILE_BYTES + page_index * FS_PAGE_BYTES;
}

static int ensure_tmpfs_page(u64 file_index, u64 page_index) {
    if (file_index >= VFS_TMPFS_MAX_FILES || page_index >= VFS_TMPFS_PAGES_PER_FILE) return 0;
    if (g_tmpfs_files[file_index].page_paddrs[page_index] != 0) return 1;
    const u64 paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (paddr < FS_PAGE_BYTES) return 0;
    const u64 va = tmpfs_page_va(file_index, page_index);
    if (syscall3(SYSCALL_MAP_PAGE, va, paddr, 1) != SYSCALL_OK) return 0;
    clear_page(va);
    g_tmpfs_files[file_index].page_paddrs[page_index] = paddr;
    return 1;
}

static u8 tmpfs_read_byte(u64 file_index, u64 offset) {
    const u64 page_index = offset / FS_PAGE_BYTES;
    const u64 page_offset = offset % FS_PAGE_BYTES;
    if (file_index >= VFS_TMPFS_MAX_FILES || page_index >= VFS_TMPFS_PAGES_PER_FILE) return 0;
    if (g_tmpfs_files[file_index].page_paddrs[page_index] == 0) return 0;
    volatile u8 *page = (volatile u8 *)tmpfs_page_va(file_index, page_index);
    return page[page_offset];
}

static int tmpfs_write_bytes(u64 file_index, u64 offset, const volatile u8 *src, u32 length) {
    if (file_index >= VFS_TMPFS_MAX_FILES || offset > VFS_TMPFS_FILE_BYTES || offset + length > VFS_TMPFS_FILE_BYTES) return 0;
    u32 done = 0;
    while (done < length) {
        const u64 write_offset = offset + done;
        const u64 page_index = write_offset / FS_PAGE_BYTES;
        const u64 page_offset = write_offset % FS_PAGE_BYTES;
        if (!ensure_tmpfs_page(file_index, page_index)) return 0;
        u32 chunk = (u32)(FS_PAGE_BYTES - page_offset);
        if (chunk > length - done) chunk = length - done;
        volatile u8 *page = (volatile u8 *)tmpfs_page_va(file_index, page_index);
        for (u32 i = 0; i < chunk; i++) page[page_offset + i] = src[done + i];
        done += chunk;
    }
    return 1;
}

static void tmpfs_clear_file_storage(u64 file_index) {
    if (file_index >= VFS_TMPFS_MAX_FILES) return;
    for (u64 i = 0; i < VFS_TMPFS_PAGES_PER_FILE; i++) {
        if (g_tmpfs_files[file_index].page_paddrs[i] != 0) clear_page(tmpfs_page_va(file_index, i));
    }
}

static void copy_from_volatile(volatile u8 *dst, const volatile u8 *src, u64 bytes) {
    __asm__ volatile(
        "rep movsb"
        : "+D"(dst), "+S"(src), "+c"(bytes)
        :
        : "memory");
}

static u64 token_from_object_id(u64 object_id) {
    return VFS_TOKEN_TAG | object_id;
}

static int is_cap_token(u64 token) {
    return (token & VFS_TOKEN_TAG) != 0 && (token & ~VFS_TOKEN_TAG) != 0;
}

static int is_backend_token(u64 token) {
    return is_cap_token(token) && ((token & ~VFS_TOKEN_TAG) & VFS_BACKEND_TOKEN_BASE) != 0;
}

static u64 wrap_backend_token(u64 backend_token) {
    if (!is_cap_token(backend_token)) return 0;
    return VFS_TOKEN_TAG | VFS_BACKEND_TOKEN_BASE | (backend_token & ~VFS_TOKEN_TAG);
}

static u64 unwrap_backend_token(u64 token) {
    if (!is_backend_token(token)) return 0;
    return VFS_TOKEN_TAG | ((token & ~VFS_TOKEN_TAG) & ~VFS_BACKEND_TOKEN_BASE);
}

static u64 root_token(void) {
    return token_from_object_id(VFS_ROOT_OBJECT_ID);
}

static u64 object_id_from_token(u64 token) {
    if ((token & VFS_TOKEN_TAG) == 0) return 0;
    const u64 object_id = token & ~VFS_TOKEN_TAG;
    if (object_id >= VFS_ROOT_OBJECT_ID && object_id <= VFS_PROC_DRIVER_RTC_OPEN_OBJECT_ID) return object_id;
    if (object_id >= VFS_TMPFS_FILE_OBJECT_ID_BASE &&
        object_id < VFS_TMPFS_FILE_OBJECT_ID_BASE + VFS_TMPFS_MAX_FILES) return object_id;
    if (object_id >= VFS_TMPFS_OPEN_OBJECT_ID_BASE &&
        object_id < VFS_TMPFS_OPEN_OBJECT_ID_BASE + VFS_TMPFS_MAX_FILES) return object_id;
    return 0;
}

static u64 tmpfs_index_from_file_object_id(u64 object_id) {
    if (object_id < VFS_TMPFS_FILE_OBJECT_ID_BASE ||
        object_id >= VFS_TMPFS_FILE_OBJECT_ID_BASE + VFS_TMPFS_MAX_FILES) return VFS_TMPFS_MAX_FILES;
    return object_id - VFS_TMPFS_FILE_OBJECT_ID_BASE;
}

static u64 tmpfs_index_from_open_object_id(u64 object_id) {
    if (object_id < VFS_TMPFS_OPEN_OBJECT_ID_BASE ||
        object_id >= VFS_TMPFS_OPEN_OBJECT_ID_BASE + VFS_TMPFS_MAX_FILES) return VFS_TMPFS_MAX_FILES;
    return object_id - VFS_TMPFS_OPEN_OBJECT_ID_BASE;
}

static int is_tmpfs_file_object_id(u64 object_id) {
    const u64 index = tmpfs_index_from_file_object_id(object_id);
    return index < VFS_TMPFS_MAX_FILES && g_tmpfs_files[index].used;
}

static int is_tmpfs_open_object_id(u64 object_id) {
    const u64 index = tmpfs_index_from_open_object_id(object_id);
    return index < VFS_TMPFS_MAX_FILES && g_tmpfs_files[index].used;
}

static int is_directory_object_id(u64 object_id) {
    return (object_id >= VFS_ROOT_OBJECT_ID && object_id <= VFS_RUN_OBJECT_ID) ||
        object_id == VFS_PROC_SELF_OBJECT_ID ||
        object_id == VFS_PROC_NET_OBJECT_ID ||
        object_id == VFS_PROC_DRIVER_OBJECT_ID ||
        object_id == VFS_APK_DB_OBJECT_ID ||
        object_id == VFS_APK_CACHE_OBJECT_ID;
}

static int is_file_object_id(u64 object_id) {
    return object_id == VFS_DEV_NULL_OBJECT_ID ||
        object_id == VFS_DEV_ZERO_OBJECT_ID ||
        object_id == VFS_DEV_RANDOM_OBJECT_ID ||
        object_id == VFS_DEV_URANDOM_OBJECT_ID ||
        object_id == VFS_PROC_CPUINFO_OBJECT_ID ||
        object_id == VFS_PROC_MEMINFO_OBJECT_ID ||
        object_id == VFS_PROC_UPTIME_OBJECT_ID ||
        object_id == VFS_PROC_STAT_OBJECT_ID ||
        object_id == VFS_PROC_MOUNTS_OBJECT_ID ||
        object_id == VFS_PROC_SELF_EXE_OBJECT_ID ||
        object_id == VFS_PROC_SELF_STAT_OBJECT_ID ||
        object_id == VFS_PROC_SELF_STATUS_OBJECT_ID ||
        object_id == VFS_PROC_NET_DEV_OBJECT_ID ||
        object_id == VFS_PROC_NET_ROUTE_OBJECT_ID ||
        object_id == VFS_PROC_NET_CAPABILITYOS_OBJECT_ID ||
        object_id == VFS_PROC_DRIVER_RTC_OBJECT_ID ||
        is_tmpfs_file_object_id(object_id);
}

static int is_open_file_object_id(u64 object_id) {
    return object_id == VFS_DEV_NULL_OPEN_OBJECT_ID ||
        object_id == VFS_DEV_ZERO_OPEN_OBJECT_ID ||
        object_id == VFS_DEV_RANDOM_OPEN_OBJECT_ID ||
        object_id == VFS_DEV_URANDOM_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_CPUINFO_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_MEMINFO_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_UPTIME_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_STAT_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_MOUNTS_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_SELF_STAT_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_SELF_STATUS_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_NET_DEV_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_NET_ROUTE_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_NET_CAPABILITYOS_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_DRIVER_RTC_OPEN_OBJECT_ID ||
        is_tmpfs_open_object_id(object_id);
}

static int is_directory_token(u64 token) {
    return is_directory_object_id(object_id_from_token(token));
}

static int is_file_token(u64 token) {
    return is_file_object_id(object_id_from_token(token));
}

static int is_open_file_token(u64 token) {
    return is_open_file_object_id(object_id_from_token(token));
}

static u64 open_object_id_for_file(u64 object_id) {
    for (u64 i = 0; i < sizeof(g_dev_files) / sizeof(g_dev_files[0]); i++) {
        if (g_dev_files[i].object_id == object_id) return g_dev_files[i].open_object_id;
    }
    for (u64 i = 0; i < sizeof(g_proc_files) / sizeof(g_proc_files[0]); i++) {
        if (g_proc_files[i].object_id == object_id) return g_proc_files[i].open_object_id;
    }
    for (u64 i = 0; i < sizeof(g_proc_self_files) / sizeof(g_proc_self_files[0]); i++) {
        if (g_proc_self_files[i].object_id == object_id) return g_proc_self_files[i].open_object_id;
    }
    for (u64 i = 0; i < sizeof(g_proc_net_files) / sizeof(g_proc_net_files[0]); i++) {
        if (g_proc_net_files[i].object_id == object_id) return g_proc_net_files[i].open_object_id;
    }
    for (u64 i = 0; i < sizeof(g_proc_driver_files) / sizeof(g_proc_driver_files[0]); i++) {
        if (g_proc_driver_files[i].object_id == object_id) return g_proc_driver_files[i].open_object_id;
    }
    const u64 tmpfs_index = tmpfs_index_from_file_object_id(object_id);
    if (tmpfs_index < VFS_TMPFS_MAX_FILES && g_tmpfs_files[tmpfs_index].used) {
        return VFS_TMPFS_OPEN_OBJECT_ID_BASE + tmpfs_index;
    }
    return 0;
}

static int is_root_path(const volatile u8 *path, u16 len) {
    if (len == 0) return 1;
    if (len == 1 && path[0] == '/') return 1;
    return 0;
}

static int path_equals(const volatile u8 *path, u16 len, const char *name, u16 name_len) {
    if (len != name_len) return 0;
    for (u16 i = 0; i < len; i++) {
        if (path[i] != (u8)name[i]) return 0;
    }
    return 1;
}

static u64 lookup_proc_self_exact_path(u64 base_object_id, const volatile u8 *path, u16 len) {
    if ((base_object_id == VFS_ROOT_OBJECT_ID && path_equals(path, len, "/proc/self/exe", 14)) ||
        (base_object_id == VFS_PROC_OBJECT_ID && path_equals(path, len, "self/exe", 8)) ||
        (base_object_id == VFS_PROC_SELF_OBJECT_ID && path_equals(path, len, "exe", 3)))
    {
        return token_from_object_id(VFS_PROC_SELF_EXE_OBJECT_ID);
    }
    if ((base_object_id == VFS_ROOT_OBJECT_ID && path_equals(path, len, "/proc/self/stat", 15)) ||
        (base_object_id == VFS_PROC_OBJECT_ID && path_equals(path, len, "self/stat", 9)) ||
        (base_object_id == VFS_PROC_SELF_OBJECT_ID && path_equals(path, len, "stat", 4)))
    {
        return token_from_object_id(VFS_PROC_SELF_STAT_OBJECT_ID);
    }
    if ((base_object_id == VFS_ROOT_OBJECT_ID && path_equals(path, len, "/proc/self/status", 17)) ||
        (base_object_id == VFS_PROC_OBJECT_ID && path_equals(path, len, "self/status", 11)) ||
        (base_object_id == VFS_PROC_SELF_OBJECT_ID && path_equals(path, len, "status", 6)))
    {
        return token_from_object_id(VFS_PROC_SELF_STATUS_OBJECT_ID);
    }
    return 0;
}

static u64 lookup_proc_net_exact_path(u64 base_object_id, const volatile u8 *path, u16 len) {
    if ((base_object_id == VFS_ROOT_OBJECT_ID && path_equals(path, len, "/proc/net/dev", 13)) ||
        (base_object_id == VFS_PROC_OBJECT_ID && path_equals(path, len, "net/dev", 7)) ||
        (base_object_id == VFS_PROC_NET_OBJECT_ID && path_equals(path, len, "dev", 3)))
    {
        return token_from_object_id(VFS_PROC_NET_DEV_OBJECT_ID);
    }
    if ((base_object_id == VFS_ROOT_OBJECT_ID && path_equals(path, len, "/proc/net/route", 15)) ||
        (base_object_id == VFS_PROC_OBJECT_ID && path_equals(path, len, "net/route", 9)) ||
        (base_object_id == VFS_PROC_NET_OBJECT_ID && path_equals(path, len, "route", 5)))
    {
        return token_from_object_id(VFS_PROC_NET_ROUTE_OBJECT_ID);
    }
    if ((base_object_id == VFS_ROOT_OBJECT_ID && path_equals(path, len, "/proc/net/capabilityos", 22)) ||
        (base_object_id == VFS_PROC_OBJECT_ID && path_equals(path, len, "net/capabilityos", 16)) ||
        (base_object_id == VFS_PROC_NET_OBJECT_ID && path_equals(path, len, "capabilityos", 12)))
    {
        return token_from_object_id(VFS_PROC_NET_CAPABILITYOS_OBJECT_ID);
    }
    return 0;
}

static u64 lookup_proc_driver_exact_path(u64 base_object_id, const volatile u8 *path, u16 len) {
    if ((base_object_id == VFS_ROOT_OBJECT_ID && path_equals(path, len, "/proc/driver/rtc", 16)) ||
        (base_object_id == VFS_PROC_OBJECT_ID && path_equals(path, len, "driver/rtc", 10)) ||
        (base_object_id == VFS_PROC_DRIVER_OBJECT_ID && path_equals(path, len, "rtc", 3)))
    {
        return token_from_object_id(VFS_PROC_DRIVER_RTC_OBJECT_ID);
    }
    return 0;
}

static u64 lookup_mount_name(const volatile u8 *name, u16 name_len) {
    for (u64 i = 0; i < sizeof(g_root_mounts) / sizeof(g_root_mounts[0]); i++) {
        if (path_equals(name, name_len, g_root_mounts[i].name, g_root_mounts[i].name_len)) {
            return token_from_object_id(g_root_mounts[i].object_id);
        }
    }
    return 0;
}

static u64 lookup_dev_name(const volatile u8 *name, u16 name_len) {
    for (u64 i = 0; i < sizeof(g_dev_files) / sizeof(g_dev_files[0]); i++) {
        if (path_equals(name, name_len, g_dev_files[i].name, g_dev_files[i].name_len)) {
            return token_from_object_id(g_dev_files[i].object_id);
        }
    }
    return 0;
}

static u64 lookup_proc_name(const volatile u8 *name, u16 name_len) {
    for (u64 i = 0; i < sizeof(g_proc_dirs) / sizeof(g_proc_dirs[0]); i++) {
        if (path_equals(name, name_len, g_proc_dirs[i].name, g_proc_dirs[i].name_len)) {
            return token_from_object_id(g_proc_dirs[i].object_id);
        }
    }
    for (u64 i = 0; i < sizeof(g_proc_files) / sizeof(g_proc_files[0]); i++) {
        if (path_equals(name, name_len, g_proc_files[i].name, g_proc_files[i].name_len)) {
            return token_from_object_id(g_proc_files[i].object_id);
        }
    }
    return 0;
}

static u64 lookup_proc_self_name(const volatile u8 *name, u16 name_len) {
    for (u64 i = 0; i < sizeof(g_proc_self_files) / sizeof(g_proc_self_files[0]); i++) {
        if (path_equals(name, name_len, g_proc_self_files[i].name, g_proc_self_files[i].name_len)) {
            return token_from_object_id(g_proc_self_files[i].object_id);
        }
    }
    return 0;
}

static u64 lookup_proc_net_name(const volatile u8 *name, u16 name_len) {
    for (u64 i = 0; i < sizeof(g_proc_net_files) / sizeof(g_proc_net_files[0]); i++) {
        if (path_equals(name, name_len, g_proc_net_files[i].name, g_proc_net_files[i].name_len)) {
            return token_from_object_id(g_proc_net_files[i].object_id);
        }
    }
    return 0;
}

static u64 lookup_proc_driver_name(const volatile u8 *name, u16 name_len) {
    for (u64 i = 0; i < sizeof(g_proc_driver_files) / sizeof(g_proc_driver_files[0]); i++) {
        if (path_equals(name, name_len, g_proc_driver_files[i].name, g_proc_driver_files[i].name_len)) {
            return token_from_object_id(g_proc_driver_files[i].object_id);
        }
    }
    return 0;
}

static u64 lookup_tmpfs_name(u64 parent_object_id, const volatile u8 *name, u16 name_len) {
    if (parent_object_id != VFS_TMP_OBJECT_ID &&
        parent_object_id != VFS_RUN_OBJECT_ID &&
        parent_object_id != VFS_APK_DB_OBJECT_ID &&
        parent_object_id != VFS_APK_CACHE_OBJECT_ID)
    {
        return 0;
    }
    if (name_len == 0 || name_len > FS_MAX_PATH_BYTES) return 0;
    for (u64 i = 0; i < VFS_TMPFS_MAX_FILES; i++) {
        if (!g_tmpfs_files[i].used || g_tmpfs_files[i].parent_object_id != parent_object_id) continue;
        if (path_equals(name, name_len, g_tmpfs_files[i].name, g_tmpfs_files[i].name_len)) {
            return token_from_object_id(VFS_TMPFS_FILE_OBJECT_ID_BASE + i);
        }
    }
    return 0;
}

static int path_single_component(const volatile u8 *path, u16 len) {
    if (len == 0 || len > FS_MAX_PATH_BYTES) return 0;
    for (u16 i = 0; i < len; i++) {
        if (path[i] == '/') return 0;
    }
    return 1;
}

static int path_has_literal_prefix(const volatile u8 *path, u16 len, const char *prefix, u16 prefix_len) {
    if (len <= prefix_len) return 0;
    for (u16 i = 0; i < prefix_len; i++) {
        if (path[i] != (u8)prefix[i]) return 0;
    }
    return 1;
}

static int tmpfs_parent_for_path(const volatile u8 *path, u16 path_len, u64 *parent_out, const volatile u8 **name_out, u16 *name_len_out) {
    struct tmpfs_prefix {
        const char *prefix;
        u16 prefix_len;
        u64 parent;
    };
    static const struct tmpfs_prefix prefixes[] = {
        { "/tmp/", 5, VFS_TMP_OBJECT_ID },
        { "/run/", 5, VFS_RUN_OBJECT_ID },
        { "/lib/apk/db/", 12, VFS_APK_DB_OBJECT_ID },
        { "/var/lib/apk/db/", 16, VFS_APK_DB_OBJECT_ID },
        { "/var/lib/apk/", 13, VFS_APK_DB_OBJECT_ID },
        { "/var/cache/apk/", 15, VFS_APK_CACHE_OBJECT_ID },
        { "/etc/apk/cache/", 15, VFS_APK_CACHE_OBJECT_ID },
    };
    for (u64 i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        if (!path_has_literal_prefix(path, path_len, prefixes[i].prefix, prefixes[i].prefix_len)) continue;
        const u16 name_len = path_len - prefixes[i].prefix_len;
        const volatile u8 *name = path + prefixes[i].prefix_len;
        if (!path_single_component(name, name_len)) return 0;
        *parent_out = prefixes[i].parent;
        *name_out = name;
        *name_len_out = name_len;
        return 1;
    }
    return 0;
}

static u64 lookup_tmpfs_exact_path(const volatile u8 *path, u16 path_len) {
    u64 parent = 0;
    const volatile u8 *name = (const volatile u8 *)0;
    u16 name_len = 0;
    if (!tmpfs_parent_for_path(path, path_len, &parent, &name, &name_len)) return 0;
    return lookup_tmpfs_name(parent, name, name_len);
}

static u64 lookup_tmpfs_dir_exact_path(u64 base_object_id, const volatile u8 *path, u16 path_len) {
    if (base_object_id != VFS_ROOT_OBJECT_ID) return 0;
    if (path_equals(path, path_len, "/lib/apk/db", 11)) return token_from_object_id(VFS_APK_DB_OBJECT_ID);
    if (path_equals(path, path_len, "/var/lib/apk", 12)) return token_from_object_id(VFS_APK_DB_OBJECT_ID);
    if (path_equals(path, path_len, "/var/lib/apk/db", 15)) return token_from_object_id(VFS_APK_DB_OBJECT_ID);
    if (path_equals(path, path_len, "/var/cache/apk", 14)) return token_from_object_id(VFS_APK_CACHE_OBJECT_ID);
    if (path_equals(path, path_len, "/etc/apk/cache", 14)) return token_from_object_id(VFS_APK_CACHE_OBJECT_ID);
    return 0;
}

static int path_should_not_forward_backend(u64 base_token, const volatile u8 *path, u16 path_len) {
    if (object_id_from_token(base_token) != VFS_ROOT_OBJECT_ID) return 0;
    u64 parent = 0;
    const volatile u8 *name = (const volatile u8 *)0;
    u16 name_len = 0;
    if (tmpfs_parent_for_path(path, path_len, &parent, &name, &name_len)) return 1;
    return path_equals(path, path_len, "/etc/apk/cache", 14) ||
        path_equals(path, path_len, "/var/lib/apk", 12) ||
        path_equals(path, path_len, "/var/lib/apk/db", 15) ||
        path_equals(path, path_len, "/etc/apk/protected_paths.d", 26) ||
        path_equals(path, path_len, "/etc/apk/repositories.d", 23);
}

static int root_mount_name_exists(const volatile u8 *name, u16 name_len) {
    for (u64 i = 0; i < sizeof(g_root_mounts) / sizeof(g_root_mounts[0]); i++) {
        if (path_equals(name, name_len, g_root_mounts[i].name, g_root_mounts[i].name_len)) return 1;
    }
    return 0;
}

static u64 lookup_path(u64 base_token, const volatile u8 *path, u16 len) {
    const u64 base_object_id = object_id_from_token(base_token);
    if (base_object_id == 0) return 0;
    if (is_root_path(path, len)) return base_token;
    const u64 self_exact_token = lookup_proc_self_exact_path(base_object_id, path, len);
    if (self_exact_token != 0) return self_exact_token;
    const u64 net_exact_token = lookup_proc_net_exact_path(base_object_id, path, len);
    if (net_exact_token != 0) return net_exact_token;
    const u64 driver_exact_token = lookup_proc_driver_exact_path(base_object_id, path, len);
    if (driver_exact_token != 0) return driver_exact_token;
    const u64 tmpfs_exact_token = lookup_tmpfs_exact_path(path, len);
    if (tmpfs_exact_token != 0) return tmpfs_exact_token;
    const u64 tmpfs_dir_exact_token = lookup_tmpfs_dir_exact_path(base_object_id, path, len);
    if (tmpfs_dir_exact_token != 0) return tmpfs_dir_exact_token;

    u16 pos = 0;
    u64 current_object_id = base_object_id;
    if (len > 0 && path[0] == '/') {
        current_object_id = VFS_ROOT_OBJECT_ID;
        pos = 1;
    }

    while (pos < len && path[pos] == '/') pos++;
    if (pos >= len) return token_from_object_id(current_object_id);

    const u16 start = pos;
    while (pos < len && path[pos] != '/') pos++;
    const u16 component_len = pos - start;
    while (pos < len && path[pos] == '/') pos++;

    if (current_object_id == VFS_ROOT_OBJECT_ID) {
        const u64 token = lookup_mount_name(path + start, component_len);
        if (token == 0) return 0;
        current_object_id = object_id_from_token(token);
    } else if (current_object_id == VFS_DEV_OBJECT_ID) {
        const u64 token = lookup_dev_name(path + start, component_len);
        if (token == 0) return 0;
        current_object_id = object_id_from_token(token);
    } else if (current_object_id == VFS_PROC_OBJECT_ID) {
        const u64 token = lookup_proc_name(path + start, component_len);
        if (token == 0) return 0;
        current_object_id = object_id_from_token(token);
    } else if (current_object_id == VFS_PROC_SELF_OBJECT_ID) {
        const u64 token = lookup_proc_self_name(path + start, component_len);
        if (token == 0) return 0;
        current_object_id = object_id_from_token(token);
    } else if (current_object_id == VFS_PROC_NET_OBJECT_ID) {
        const u64 token = lookup_proc_net_name(path + start, component_len);
        if (token == 0) return 0;
        current_object_id = object_id_from_token(token);
    } else if (current_object_id == VFS_PROC_DRIVER_OBJECT_ID) {
        const u64 token = lookup_proc_driver_name(path + start, component_len);
        if (token == 0) return 0;
        current_object_id = object_id_from_token(token);
    } else if (current_object_id == VFS_TMP_OBJECT_ID ||
        current_object_id == VFS_RUN_OBJECT_ID ||
        current_object_id == VFS_APK_DB_OBJECT_ID ||
        current_object_id == VFS_APK_CACHE_OBJECT_ID)
    {
        const u64 token = lookup_tmpfs_name(current_object_id, path + start, component_len);
        if (token == 0) return 0;
        current_object_id = object_id_from_token(token);
    } else {
        return 0;
    }

    if (pos >= len) return token_from_object_id(current_object_id);
    if (current_object_id != VFS_DEV_OBJECT_ID &&
        current_object_id != VFS_PROC_OBJECT_ID &&
        current_object_id != VFS_PROC_SELF_OBJECT_ID &&
        current_object_id != VFS_PROC_NET_OBJECT_ID &&
        current_object_id != VFS_PROC_DRIVER_OBJECT_ID &&
        current_object_id != VFS_TMP_OBJECT_ID &&
        current_object_id != VFS_RUN_OBJECT_ID &&
        current_object_id != VFS_APK_DB_OBJECT_ID &&
        current_object_id != VFS_APK_CACHE_OBJECT_ID) return 0;

    const u16 second_start = pos;
    while (pos < len && path[pos] != '/') pos++;
    const u16 second_len = pos - second_start;
    while (pos < len && path[pos] == '/') pos++;
    if (pos < len) return 0;
    if (current_object_id == VFS_DEV_OBJECT_ID) return lookup_dev_name(path + second_start, second_len);
    if (current_object_id == VFS_PROC_OBJECT_ID) return lookup_proc_name(path + second_start, second_len);
    if (current_object_id == VFS_PROC_SELF_OBJECT_ID) return lookup_proc_self_name(path + second_start, second_len);
    if (current_object_id == VFS_PROC_NET_OBJECT_ID) return lookup_proc_net_name(path + second_start, second_len);
    if (current_object_id == VFS_PROC_DRIVER_OBJECT_ID) return lookup_proc_driver_name(path + second_start, second_len);
    return lookup_tmpfs_name(current_object_id, path + second_start, second_len);
}

static void write_response(
    u16 op,
    u64 seq,
    i32 status,
    u64 result_token,
    u64 file_bytes,
    u64 cursor_next,
    u8 object_kind,
    u16 inline_bytes
) {
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_session->response_va;
    response->magic = FS_RESPONSE_MAGIC;
    response->version = FS_PROTOCOL_VERSION;
    response->op = op;
    response->status = status;
    response->result_flags = 0;
    response->result_token = result_token;
    response->file_bytes = file_bytes;
    response->cursor_next = cursor_next;
    response->inline_bytes = inline_bytes;
    response->object_kind = object_kind;
    response->reserved0 = 0;
    response->reserved1 = 0;
    response->arg0 = 0;
    response->arg1 = 0;
    __asm__ volatile("" ::: "memory");
    response->response_seq = seq;
    if (g_session->reply_endpoint_id != 0) {
        (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, g_session->reply_endpoint_id, 0);
    }
}

static void reply_status(u16 op, u64 seq, i32 status) {
    clear_page(g_session->response_va);
    write_response(op, seq, status, 0, 0, 0, FS_OBJECT_NONE, 0);
}

static void reply_dir_lookup(u16 op, u64 seq, u64 token) {
    clear_page(g_session->response_va);
    const u8 kind = token == root_token() ? FS_OBJECT_MOUNT : FS_OBJECT_DIRECTORY;
    write_response(op, seq, FS_STATUS_OK, token, 0, 0, kind, 0);
}

static u64 tmpfs_file_bytes_from_token(u64 token) {
    const u64 index = tmpfs_index_from_file_object_id(object_id_from_token(token));
    if (index < VFS_TMPFS_MAX_FILES && g_tmpfs_files[index].used) return g_tmpfs_files[index].size;
    return 0;
}

static u64 dev_file_bytes_from_object_id(u64 object_id) {
    if (object_id == VFS_DEV_ZERO_OBJECT_ID ||
        object_id == VFS_DEV_ZERO_OPEN_OBJECT_ID ||
        object_id == VFS_DEV_RANDOM_OBJECT_ID ||
        object_id == VFS_DEV_RANDOM_OPEN_OBJECT_ID ||
        object_id == VFS_DEV_URANDOM_OBJECT_ID ||
        object_id == VFS_DEV_URANDOM_OPEN_OBJECT_ID)
    {
        return ~0ULL;
    }
    return 0;
}

static u64 proc_visible_cpu_count(const struct cpu_identity *identity) {
    u64 count = identity->logical_count;
    if (count == 0) count = 1;
    if (count > 64) count = 64;
    return count;
}

static const char *build_proc_cpuinfo(u64 *bytes_out) {
    struct cpu_identity identity;
    read_cpu_identity(&identity);
    const u64 count = proc_visible_cpu_count(&identity);
    u64 len = 0;
    g_proc_cpuinfo_buf[0] = 0;
    for (u64 i = 0; i < count; i++) {
        append_str(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, "processor\t: ");
        append_u64_dec(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, i);
        append_str(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, "\n");
        append_str(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, "vendor_id\t: ");
        append_str(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, identity.vendor);
        append_str(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, "\n");
        append_str(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, "model name\t: ");
        append_str(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, identity.brand);
        append_str(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, "\n");
        append_str(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, "cpu cores\t: ");
        append_u64_dec(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, identity.core_count);
        append_str(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, "\n");
        append_str(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, "siblings\t: ");
        append_u64_dec(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, identity.logical_count);
        append_str(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, "\n");
        append_str(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, "core id\t\t: ");
        append_u64_dec(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, identity.core_count == 0 ? 0 : (i % identity.core_count));
        append_str(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, "\n");
        append_str(g_proc_cpuinfo_buf, sizeof(g_proc_cpuinfo_buf), &len, "physical id\t: 0\n\n");
    }
    if (bytes_out != 0) *bytes_out = len;
    return g_proc_cpuinfo_buf;
}

static const char *build_proc_stat(u64 *bytes_out) {
    struct cpu_identity identity;
    read_cpu_identity(&identity);
    const u64 count = proc_visible_cpu_count(&identity);
    u64 len = 0;
    g_proc_stat_buf[0] = 0;
    append_str(g_proc_stat_buf, sizeof(g_proc_stat_buf), &len, "cpu  100 0 50 1000 0 0 0 0 0 0\n");
    for (u64 i = 0; i < count; i++) {
        append_str(g_proc_stat_buf, sizeof(g_proc_stat_buf), &len, "cpu");
        append_u64_dec(g_proc_stat_buf, sizeof(g_proc_stat_buf), &len, i);
        append_str(g_proc_stat_buf, sizeof(g_proc_stat_buf), &len, " 25 0 12 250 0 0 0 0 0 0\n");
    }
    append_str(g_proc_stat_buf, sizeof(g_proc_stat_buf), &len,
        "intr 0\n"
        "ctxt 0\n"
        "btime 0\n"
        "processes 1\n"
        "procs_running 1\n"
        "procs_blocked 0\n");
    if (bytes_out != 0) *bytes_out = len;
    return g_proc_stat_buf;
}

static const char *build_proc_meminfo(u64 *bytes_out) {
    u64 stats[4];
    stats[0] = 512ULL * 1024ULL * 1024ULL;
    stats[1] = 256ULL * 1024ULL * 1024ULL;
    stats[2] = 256ULL * 1024ULL * 1024ULL;
    stats[3] = 4096;
    const u64 status = syscall1(SYSCALL_GET_MEMORY_STATS, (u64)stats);
    if (status != SYSCALL_OK || stats[0] == 0 || stats[2] > stats[0]) {
        stats[0] = 512ULL * 1024ULL * 1024ULL;
        stats[1] = 256ULL * 1024ULL * 1024ULL;
        stats[2] = 256ULL * 1024ULL * 1024ULL;
        stats[3] = 4096;
    }

    const u64 total_kb = stats[0] / 1024;
    const u64 free_kb = stats[2] / 1024;
    u64 len = 0;
    g_proc_meminfo_buf[0] = 0;
    append_meminfo_line(g_proc_meminfo_buf, sizeof(g_proc_meminfo_buf), &len, "MemTotal", total_kb);
    append_meminfo_line(g_proc_meminfo_buf, sizeof(g_proc_meminfo_buf), &len, "MemFree", free_kb);
    append_meminfo_line(g_proc_meminfo_buf, sizeof(g_proc_meminfo_buf), &len, "MemAvailable", free_kb);
    append_meminfo_line(g_proc_meminfo_buf, sizeof(g_proc_meminfo_buf), &len, "Buffers", 0);
    append_meminfo_line(g_proc_meminfo_buf, sizeof(g_proc_meminfo_buf), &len, "Cached", 0);
    append_meminfo_line(g_proc_meminfo_buf, sizeof(g_proc_meminfo_buf), &len, "SwapTotal", 0);
    append_meminfo_line(g_proc_meminfo_buf, sizeof(g_proc_meminfo_buf), &len, "SwapFree", 0);
    if (bytes_out != 0) *bytes_out = len;
    return g_proc_meminfo_buf;
}

static const char *build_proc_uptime(u64 *bytes_out) {
    const u64 ticks = syscall0(SYSCALL_GET_TICK_COUNT);
    const u64 ns = ticks * (u64)PROC_MONOTONIC_NS_PER_TICK;
    const u64 seconds = ns / 1000000000ULL;
    const u64 centis = (ns % 1000000000ULL) / 10000000ULL;
    u64 len = 0;
    g_proc_uptime_buf[0] = 0;
    append_u64_dec(g_proc_uptime_buf, sizeof(g_proc_uptime_buf), &len, seconds);
    append_char(g_proc_uptime_buf, sizeof(g_proc_uptime_buf), &len, '.');
    append_u64_dec_width(g_proc_uptime_buf, sizeof(g_proc_uptime_buf), &len, centis, 2);
    append_str(g_proc_uptime_buf, sizeof(g_proc_uptime_buf), &len, " 0.00\n");
    if (bytes_out != 0) *bytes_out = len;
    return g_proc_uptime_buf;
}

static int rtc_is_leap_year(u64 year) {
    if ((year % 400) == 0) return 1;
    if ((year % 100) == 0) return 0;
    return (year % 4) == 0;
}

static u64 rtc_days_in_month(u64 year, u64 month) {
    static const u8 days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && rtc_is_leap_year(year)) return 29;
    if (month < 1 || month > 12) return 30;
    return days[month - 1];
}

static void rtc_unix_to_utc(u64 epoch, u64 *year, u64 *month, u64 *day, u64 *hour, u64 *minute, u64 *second) {
    u64 days = epoch / 86400ULL;
    u64 rem = epoch % 86400ULL;
    *hour = rem / 3600ULL;
    rem %= 3600ULL;
    *minute = rem / 60ULL;
    *second = rem % 60ULL;

    u64 y = 1970;
    while (1) {
        const u64 diy = rtc_is_leap_year(y) ? 366 : 365;
        if (days < diy) break;
        days -= diy;
        y++;
    }

    u64 m = 1;
    while (1) {
        const u64 dim = rtc_days_in_month(y, m);
        if (days < dim) break;
        days -= dim;
        m++;
    }

    *year = y;
    *month = m;
    *day = days + 1;
}

static const char *build_proc_driver_rtc(u64 *bytes_out) {
    u64 epoch = syscall0(SYSCALL_GET_RTC_UNIX_TIME);
    if (epoch == 0) epoch = 0;

    u64 year = 1970;
    u64 month = 1;
    u64 day = 1;
    u64 hour = 0;
    u64 minute = 0;
    u64 second = 0;
    rtc_unix_to_utc(epoch, &year, &month, &day, &hour, &minute, &second);

    u64 len = 0;
    g_proc_driver_rtc_buf[0] = 0;
    append_str(g_proc_driver_rtc_buf, sizeof(g_proc_driver_rtc_buf), &len, "rtc_time\t: ");
    append_u64_dec_width(g_proc_driver_rtc_buf, sizeof(g_proc_driver_rtc_buf), &len, hour, 2);
    append_char(g_proc_driver_rtc_buf, sizeof(g_proc_driver_rtc_buf), &len, ':');
    append_u64_dec_width(g_proc_driver_rtc_buf, sizeof(g_proc_driver_rtc_buf), &len, minute, 2);
    append_char(g_proc_driver_rtc_buf, sizeof(g_proc_driver_rtc_buf), &len, ':');
    append_u64_dec_width(g_proc_driver_rtc_buf, sizeof(g_proc_driver_rtc_buf), &len, second, 2);
    append_str(g_proc_driver_rtc_buf, sizeof(g_proc_driver_rtc_buf), &len, "\nrtc_date\t: ");
    append_u64_dec_width(g_proc_driver_rtc_buf, sizeof(g_proc_driver_rtc_buf), &len, year, 4);
    append_char(g_proc_driver_rtc_buf, sizeof(g_proc_driver_rtc_buf), &len, '-');
    append_u64_dec_width(g_proc_driver_rtc_buf, sizeof(g_proc_driver_rtc_buf), &len, month, 2);
    append_char(g_proc_driver_rtc_buf, sizeof(g_proc_driver_rtc_buf), &len, '-');
    append_u64_dec_width(g_proc_driver_rtc_buf, sizeof(g_proc_driver_rtc_buf), &len, day, 2);
    append_str(g_proc_driver_rtc_buf, sizeof(g_proc_driver_rtc_buf), &len,
        "\nalrm_time\t: 00:00:00\n"
        "alrm_date\t: ****-**-**\n"
        "alarm_IRQ\t: no\n"
        "24hr\t\t: yes\n"
        "update_IRQ\t: no\n");
    if (bytes_out != 0) *bytes_out = len;
    return g_proc_driver_rtc_buf;
}

static void fallback_net_status(struct net_status_payload *status) {
    status->mac[0] = 0x52;
    status->mac[1] = 0x54;
    status->mac[2] = 0x00;
    status->mac[3] = 0x12;
    status->mac[4] = 0x34;
    status->mac[5] = 0x56;
    status->link_up = 1;
    status->dhcp_bound = 1;
    status->ipv4_addr = 0x0A00020FULL;
    status->gateway_addr = 0x0A000202ULL;
    status->dns_addr = 0x0A000203ULL;
    status->dhcp_server_addr = 0x0A000202ULL;
    status->flags = NET_FLAG_LINK_UP | NET_FLAG_DHCP_BOUND | NET_FLAG_GATEWAY_ARP;
    status->rx_packets = 2;
    status->tx_completions = 3;
    status->tcp_rx_segments = 0;
    status->tcp_rx_payload_bytes = 0;
    status->tcp_rx_in_order_bytes = 0;
    status->tcp_rx_duplicate_segments = 0;
    status->tcp_rx_out_of_order_segments = 0;
    status->tcp_rx_ooo_stored = 0;
    status->tcp_rx_ooo_drained = 0;
    status->tcp_rx_ooo_dropped = 0;
    status->tcp_rx_append_failed = 0;
    status->tcp_ack_sent = 0;
    status->tcp_ack_deferred = 0;
    status->tcp_ack_flushed = 0;
    status->tcp_tx_busy = 0;
    status->tcp_connect_requests = 0;
    status->tcp_connect_established = 0;
    status->tcp_tx_segments = 0;
    status->tcp_tx_payload_bytes = 0;
    status->tcp_read_requests = 0;
    status->tcp_read_would_block = 0;
    status->tcp_read_bytes = 0;
    status->tcp_poll_requests = 0;
    status->tcp_poll_readable = 0;
    status->tcp_rx_syn_ack = 0;
    status->tcp_rx_fin = 0;
    status->tcp_rx_rst = 0;
    status->tcp_active_connections = 0;
    status->tcp_established_connections = 0;
    status->tcp_rx_buffered_bytes = 0;
    status->tcp_rx_buffer_max_bytes = 0;
    status->tcp_ack_pending_connections = 0;
    status->net_service_requests = 0;
    status->net_service_work_loops = 0;
    status->net_service_idle_sleeps = 0;
}

static void copy_net_status(struct net_status_payload *dst, const struct net_status_payload *src) {
    for (u64 i = 0; i < 6; i++) dst->mac[i] = src->mac[i];
    dst->link_up = src->link_up;
    dst->dhcp_bound = src->dhcp_bound;
    dst->ipv4_addr = src->ipv4_addr;
    dst->gateway_addr = src->gateway_addr;
    dst->dns_addr = src->dns_addr;
    dst->dhcp_server_addr = src->dhcp_server_addr;
    dst->flags = src->flags;
    dst->rx_packets = src->rx_packets;
    dst->tx_completions = src->tx_completions;
    dst->tcp_rx_segments = src->tcp_rx_segments;
    dst->tcp_rx_payload_bytes = src->tcp_rx_payload_bytes;
    dst->tcp_rx_in_order_bytes = src->tcp_rx_in_order_bytes;
    dst->tcp_rx_duplicate_segments = src->tcp_rx_duplicate_segments;
    dst->tcp_rx_out_of_order_segments = src->tcp_rx_out_of_order_segments;
    dst->tcp_rx_ooo_stored = src->tcp_rx_ooo_stored;
    dst->tcp_rx_ooo_drained = src->tcp_rx_ooo_drained;
    dst->tcp_rx_ooo_dropped = src->tcp_rx_ooo_dropped;
    dst->tcp_rx_append_failed = src->tcp_rx_append_failed;
    dst->tcp_ack_sent = src->tcp_ack_sent;
    dst->tcp_ack_deferred = src->tcp_ack_deferred;
    dst->tcp_ack_flushed = src->tcp_ack_flushed;
    dst->tcp_tx_busy = src->tcp_tx_busy;
    dst->tcp_connect_requests = src->tcp_connect_requests;
    dst->tcp_connect_established = src->tcp_connect_established;
    dst->tcp_tx_segments = src->tcp_tx_segments;
    dst->tcp_tx_payload_bytes = src->tcp_tx_payload_bytes;
    dst->tcp_read_requests = src->tcp_read_requests;
    dst->tcp_read_would_block = src->tcp_read_would_block;
    dst->tcp_read_bytes = src->tcp_read_bytes;
    dst->tcp_poll_requests = src->tcp_poll_requests;
    dst->tcp_poll_readable = src->tcp_poll_readable;
    dst->tcp_rx_syn_ack = src->tcp_rx_syn_ack;
    dst->tcp_rx_fin = src->tcp_rx_fin;
    dst->tcp_rx_rst = src->tcp_rx_rst;
    dst->tcp_active_connections = src->tcp_active_connections;
    dst->tcp_established_connections = src->tcp_established_connections;
    dst->tcp_rx_buffered_bytes = src->tcp_rx_buffered_bytes;
    dst->tcp_rx_buffer_max_bytes = src->tcp_rx_buffer_max_bytes;
    dst->tcp_ack_pending_connections = src->tcp_ack_pending_connections;
    dst->net_service_requests = src->net_service_requests;
    dst->net_service_work_loops = src->net_service_work_loops;
    dst->net_service_idle_sleeps = src->net_service_idle_sleeps;
}

static void current_net_status(struct net_status_payload *status) {
    if (refresh_net_status()) {
        copy_net_status(status, &g_net_backend.last_status);
    } else {
        fallback_net_status(status);
    }
}

static const char *build_proc_net_dev(u64 *bytes_out) {
    struct net_status_payload status;
    current_net_status(&status);
    u64 len = 0;
    g_proc_net_dev_buf[0] = 0;
    append_str(g_proc_net_dev_buf, sizeof(g_proc_net_dev_buf), &len, "Inter-|   Receive                                                |  Transmit\n");
    append_str(g_proc_net_dev_buf, sizeof(g_proc_net_dev_buf), &len, " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
    append_str(g_proc_net_dev_buf, sizeof(g_proc_net_dev_buf), &len, "  eth0:");
    append_u64_dec(g_proc_net_dev_buf, sizeof(g_proc_net_dev_buf), &len, status.rx_packets * 590);
    append_char(g_proc_net_dev_buf, sizeof(g_proc_net_dev_buf), &len, ' ');
    append_u64_dec(g_proc_net_dev_buf, sizeof(g_proc_net_dev_buf), &len, status.rx_packets);
    append_str(g_proc_net_dev_buf, sizeof(g_proc_net_dev_buf), &len, "    0    0    0     0          0         0 ");
    append_u64_dec(g_proc_net_dev_buf, sizeof(g_proc_net_dev_buf), &len, status.tx_completions * 128);
    append_char(g_proc_net_dev_buf, sizeof(g_proc_net_dev_buf), &len, ' ');
    append_u64_dec(g_proc_net_dev_buf, sizeof(g_proc_net_dev_buf), &len, status.tx_completions);
    append_str(g_proc_net_dev_buf, sizeof(g_proc_net_dev_buf), &len, "    0    0    0     0       0       0          0\n");
    if (bytes_out != 0) *bytes_out = len;
    return g_proc_net_dev_buf;
}

static const char *build_proc_net_route(u64 *bytes_out) {
    struct net_status_payload status;
    current_net_status(&status);
    const u32 mask = 0xFFFFFF00U;
    const u32 network = status.ipv4_addr & mask;
    u64 len = 0;
    g_proc_net_route_buf[0] = 0;
    append_str(g_proc_net_route_buf, sizeof(g_proc_net_route_buf), &len, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
    append_str(g_proc_net_route_buf, sizeof(g_proc_net_route_buf), &len, "eth0\t00000000\t");
    append_route_hex_ipv4(g_proc_net_route_buf, sizeof(g_proc_net_route_buf), &len, status.gateway_addr);
    append_str(g_proc_net_route_buf, sizeof(g_proc_net_route_buf), &len, "\t0003\t0\t0\t100\t00000000\t0\t0\t0\n");
    append_str(g_proc_net_route_buf, sizeof(g_proc_net_route_buf), &len, "eth0\t");
    append_route_hex_ipv4(g_proc_net_route_buf, sizeof(g_proc_net_route_buf), &len, network);
    append_str(g_proc_net_route_buf, sizeof(g_proc_net_route_buf), &len, "\t00000000\t0001\t0\t0\t100\t");
    append_route_hex_ipv4(g_proc_net_route_buf, sizeof(g_proc_net_route_buf), &len, mask);
    append_str(g_proc_net_route_buf, sizeof(g_proc_net_route_buf), &len, "\t0\t0\t0\n");
    if (bytes_out != 0) *bytes_out = len;
    return g_proc_net_route_buf;
}

static void append_proc_net_counter(char *buf, u64 cap, u64 *len, const char *name, u64 value) {
    append_char(buf, cap, len, '\n');
    append_str(buf, cap, len, name);
    append_char(buf, cap, len, '=');
    append_u64_dec(buf, cap, len, value);
}

static const char *build_proc_net_capabilityos(u64 *bytes_out) {
    struct net_status_payload status;
    current_net_status(&status);
    u64 len = 0;
    g_proc_net_capabilityos_buf[0] = 0;
    append_str(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "iface=eth0\nlink=");
    append_str(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, status.link_up ? "up" : "down");
    append_str(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "\ndhcp=");
    append_str(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, status.dhcp_bound ? "bound" : "pending");
    append_str(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "\nmac=");
    append_mac(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, status.mac);
    append_str(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "\nip=");
    append_ipv4(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, status.ipv4_addr);
    append_str(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "\ngateway=");
    append_ipv4(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, status.gateway_addr);
    append_str(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "\ndns=");
    append_ipv4(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, status.dns_addr);
    append_str(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "\ndhcp_server=");
    append_ipv4(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, status.dhcp_server_addr);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "rx_packets", status.rx_packets);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tx_completions", status.tx_completions);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_rx_segments", status.tcp_rx_segments);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_rx_payload_bytes", status.tcp_rx_payload_bytes);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_rx_in_order_bytes", status.tcp_rx_in_order_bytes);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_rx_duplicate_segments", status.tcp_rx_duplicate_segments);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_rx_out_of_order_segments", status.tcp_rx_out_of_order_segments);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_rx_ooo_stored", status.tcp_rx_ooo_stored);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_rx_ooo_drained", status.tcp_rx_ooo_drained);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_rx_ooo_dropped", status.tcp_rx_ooo_dropped);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_rx_append_failed", status.tcp_rx_append_failed);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_ack_sent", status.tcp_ack_sent);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_ack_deferred", status.tcp_ack_deferred);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_ack_flushed", status.tcp_ack_flushed);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_tx_busy", status.tcp_tx_busy);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_connect_requests", status.tcp_connect_requests);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_connect_established", status.tcp_connect_established);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_tx_segments", status.tcp_tx_segments);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_tx_payload_bytes", status.tcp_tx_payload_bytes);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_read_requests", status.tcp_read_requests);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_read_would_block", status.tcp_read_would_block);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_read_bytes", status.tcp_read_bytes);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_poll_requests", status.tcp_poll_requests);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_poll_readable", status.tcp_poll_readable);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_rx_syn_ack", status.tcp_rx_syn_ack);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_rx_fin", status.tcp_rx_fin);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_rx_rst", status.tcp_rx_rst);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_active_connections", status.tcp_active_connections);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_established_connections", status.tcp_established_connections);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_rx_buffered_bytes", status.tcp_rx_buffered_bytes);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_rx_buffer_max_bytes", status.tcp_rx_buffer_max_bytes);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "tcp_ack_pending_connections", status.tcp_ack_pending_connections);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "net_service_requests", status.net_service_requests);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "net_service_work_loops", status.net_service_work_loops);
    append_proc_net_counter(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, "net_service_idle_sleeps", status.net_service_idle_sleeps);
    append_char(g_proc_net_capabilityos_buf, sizeof(g_proc_net_capabilityos_buf), &len, '\n');
    if (bytes_out != 0) *bytes_out = len;
    return g_proc_net_capabilityos_buf;
}

static const char *proc_content_from_object_id(u64 object_id, u64 *bytes_out) {
    const char *content = 0;
    if (object_id == VFS_PROC_CPUINFO_OBJECT_ID || object_id == VFS_PROC_CPUINFO_OPEN_OBJECT_ID) {
        return build_proc_cpuinfo(bytes_out);
    } else if (object_id == VFS_PROC_MEMINFO_OBJECT_ID || object_id == VFS_PROC_MEMINFO_OPEN_OBJECT_ID) {
        return build_proc_meminfo(bytes_out);
    } else if (object_id == VFS_PROC_UPTIME_OBJECT_ID || object_id == VFS_PROC_UPTIME_OPEN_OBJECT_ID) {
        return build_proc_uptime(bytes_out);
    } else if (object_id == VFS_PROC_STAT_OBJECT_ID || object_id == VFS_PROC_STAT_OPEN_OBJECT_ID) {
        return build_proc_stat(bytes_out);
    } else if (object_id == VFS_PROC_MOUNTS_OBJECT_ID || object_id == VFS_PROC_MOUNTS_OPEN_OBJECT_ID) {
        content = g_proc_mounts;
    } else if (object_id == VFS_PROC_NET_DEV_OBJECT_ID || object_id == VFS_PROC_NET_DEV_OPEN_OBJECT_ID) {
        return build_proc_net_dev(bytes_out);
    } else if (object_id == VFS_PROC_NET_ROUTE_OBJECT_ID || object_id == VFS_PROC_NET_ROUTE_OPEN_OBJECT_ID) {
        return build_proc_net_route(bytes_out);
    } else if (object_id == VFS_PROC_NET_CAPABILITYOS_OBJECT_ID || object_id == VFS_PROC_NET_CAPABILITYOS_OPEN_OBJECT_ID) {
        return build_proc_net_capabilityos(bytes_out);
    } else if (object_id == VFS_PROC_DRIVER_RTC_OBJECT_ID || object_id == VFS_PROC_DRIVER_RTC_OPEN_OBJECT_ID) {
        return build_proc_driver_rtc(bytes_out);
    } else if (object_id == VFS_PROC_SELF_STAT_OBJECT_ID || object_id == VFS_PROC_SELF_STAT_OPEN_OBJECT_ID) {
        content = g_proc_self_stat;
    } else if (object_id == VFS_PROC_SELF_STATUS_OBJECT_ID || object_id == VFS_PROC_SELF_STATUS_OPEN_OBJECT_ID) {
        content = g_proc_self_status;
    }
    if (content != 0 && bytes_out != 0) *bytes_out = cstr_len(content);
    return content;
}

static u64 file_bytes_from_token(u64 token) {
    const u64 dev_bytes = dev_file_bytes_from_object_id(object_id_from_token(token));
    if (dev_bytes != 0) return dev_bytes;
    u64 bytes = 0;
    if (proc_content_from_object_id(object_id_from_token(token), &bytes) != 0) return bytes;
    return tmpfs_file_bytes_from_token(token);
}

static void reply_file_lookup(u16 op, u64 seq, u64 token) {
    clear_page(g_session->response_va);
    write_response(op, seq, FS_STATUS_OK, token, file_bytes_from_token(token), 0, FS_OBJECT_FILE, 0);
}

static void reply_stat(u64 seq, u64 token) {
    clear_page(g_session->response_va);
    volatile struct fs_stat_record *record = (volatile struct fs_stat_record *)(g_session->response_va + FS_RESPONSE_HEADER_BYTES);
    const u64 object_id = object_id_from_token(token);
    const int is_dir = is_directory_object_id(object_id);
    const u64 tmpfs_file_index = tmpfs_index_from_file_object_id(object_id);
    const u64 tmpfs_open_index = tmpfs_index_from_open_object_id(object_id);
    record->object_kind = is_dir ? FS_OBJECT_DIRECTORY : FS_OBJECT_FILE;
    record->size_bytes = dev_file_bytes_from_object_id(object_id);
    if (tmpfs_file_index < VFS_TMPFS_MAX_FILES && g_tmpfs_files[tmpfs_file_index].used) {
        record->size_bytes = g_tmpfs_files[tmpfs_file_index].size;
    } else if (tmpfs_open_index < VFS_TMPFS_MAX_FILES && g_tmpfs_files[tmpfs_open_index].used) {
        record->size_bytes = g_tmpfs_files[tmpfs_open_index].size;
    } else {
        u64 static_bytes = 0;
        if (proc_content_from_object_id(object_id, &static_bytes) != 0) record->size_bytes = static_bytes;
    }
    record->mode_bits = is_dir ? VFS_DIR_MODE : VFS_FILE_MODE;
    record->reserved1 = 0;
    record->mtime_unix_sec = 0;
    record->reserved2[0] = 0;
    record->reserved2[1] = 0;
    const u8 kind = token == root_token() ? FS_OBJECT_MOUNT : record->object_kind;
    write_response(FS_OP_STAT, seq, FS_STATUS_OK, 0, 0, 0, kind, sizeof(struct fs_stat_record));
}

static void write_dirent_response(u64 seq, u64 result_token, u64 next_cursor, const char *name, u16 name_len, u8 object_kind) {
    volatile struct fs_dirent_record *record = (volatile struct fs_dirent_record *)(g_session->response_va + FS_RESPONSE_HEADER_BYTES);
    record->next_cursor = next_cursor;
    record->object_kind = object_kind;
    record->reserved0[0] = 0;
    record->reserved0[1] = 0;
    record->reserved0[2] = 0;
    record->reserved0[3] = 0;
    record->reserved0[4] = 0;
    record->reserved0[5] = 0;
    record->reserved0[6] = 0;
    record->name_bytes = name_len;
    record->reserved1 = 0;
    record->reserved2 = 0;

    volatile u8 *payload = (volatile u8 *)(g_session->response_va + FS_RESPONSE_HEADER_BYTES + FS_DIRENT_RECORD_BYTES);
    for (u16 i = 0; i < name_len; i++) payload[i] = (u8)name[i];
    write_response(
        FS_OP_READDIR,
        seq,
        FS_STATUS_OK,
        result_token,
        0,
        next_cursor,
        object_kind,
        (u16)(FS_DIRENT_RECORD_BYTES + name_len)
    );
}

static void reply_readdir(u64 seq, u64 token, u64 cursor) {
    clear_page(g_session->response_va);
    if (token == root_token()) {
        if (cursor >= sizeof(g_root_mounts) / sizeof(g_root_mounts[0])) {
            write_response(FS_OP_READDIR, seq, FS_STATUS_END_OF_DIR, 0, 0, cursor, FS_OBJECT_DIRECTORY, 0);
            return;
        }
        const struct vfs_mount_entry *entry = &g_root_mounts[cursor];
        write_dirent_response(seq, token_from_object_id(entry->object_id), cursor + 1, entry->name, entry->name_len, FS_OBJECT_DIRECTORY);
        return;
    }
    if (token == token_from_object_id(VFS_DEV_OBJECT_ID)) {
        if (cursor >= sizeof(g_dev_files) / sizeof(g_dev_files[0])) {
            write_response(FS_OP_READDIR, seq, FS_STATUS_END_OF_DIR, 0, 0, cursor, FS_OBJECT_DIRECTORY, 0);
            return;
        }
        const struct vfs_builtin_file *entry = &g_dev_files[cursor];
        write_dirent_response(seq, token_from_object_id(entry->object_id), cursor + 1, entry->name, entry->name_len, FS_OBJECT_FILE);
        return;
    }
    if (token == token_from_object_id(VFS_PROC_OBJECT_ID)) {
        const u64 dir_count = sizeof(g_proc_dirs) / sizeof(g_proc_dirs[0]);
        if (cursor < dir_count) {
            const struct vfs_mount_entry *entry = &g_proc_dirs[cursor];
            write_dirent_response(seq, token_from_object_id(entry->object_id), cursor + 1, entry->name, entry->name_len, FS_OBJECT_DIRECTORY);
            return;
        }
        const u64 file_cursor = cursor - dir_count;
        if (file_cursor >= sizeof(g_proc_files) / sizeof(g_proc_files[0])) {
            write_response(FS_OP_READDIR, seq, FS_STATUS_END_OF_DIR, 0, 0, cursor, FS_OBJECT_DIRECTORY, 0);
            return;
        }
        const struct vfs_builtin_file *entry = &g_proc_files[file_cursor];
        write_dirent_response(seq, token_from_object_id(entry->object_id), cursor + 1, entry->name, entry->name_len, FS_OBJECT_FILE);
        return;
    }
    if (token == token_from_object_id(VFS_PROC_SELF_OBJECT_ID)) {
        if (cursor >= sizeof(g_proc_self_files) / sizeof(g_proc_self_files[0])) {
            write_response(FS_OP_READDIR, seq, FS_STATUS_END_OF_DIR, 0, 0, cursor, FS_OBJECT_DIRECTORY, 0);
            return;
        }
        const struct vfs_builtin_file *entry = &g_proc_self_files[cursor];
        write_dirent_response(seq, token_from_object_id(entry->object_id), cursor + 1, entry->name, entry->name_len, FS_OBJECT_FILE);
        return;
    }
    if (token == token_from_object_id(VFS_PROC_NET_OBJECT_ID)) {
        if (cursor >= sizeof(g_proc_net_files) / sizeof(g_proc_net_files[0])) {
            write_response(FS_OP_READDIR, seq, FS_STATUS_END_OF_DIR, 0, 0, cursor, FS_OBJECT_DIRECTORY, 0);
            return;
        }
        const struct vfs_builtin_file *entry = &g_proc_net_files[cursor];
        write_dirent_response(seq, token_from_object_id(entry->object_id), cursor + 1, entry->name, entry->name_len, FS_OBJECT_FILE);
        return;
    }
    if (token == token_from_object_id(VFS_PROC_DRIVER_OBJECT_ID)) {
        if (cursor >= sizeof(g_proc_driver_files) / sizeof(g_proc_driver_files[0])) {
            write_response(FS_OP_READDIR, seq, FS_STATUS_END_OF_DIR, 0, 0, cursor, FS_OBJECT_DIRECTORY, 0);
            return;
        }
        const struct vfs_builtin_file *entry = &g_proc_driver_files[cursor];
        write_dirent_response(seq, token_from_object_id(entry->object_id), cursor + 1, entry->name, entry->name_len, FS_OBJECT_FILE);
        return;
    }
    const u64 dir_object_id = object_id_from_token(token);
    if (dir_object_id == VFS_TMP_OBJECT_ID ||
        dir_object_id == VFS_RUN_OBJECT_ID ||
        dir_object_id == VFS_APK_DB_OBJECT_ID ||
        dir_object_id == VFS_APK_CACHE_OBJECT_ID)
    {
        u64 seen = 0;
        for (u64 i = 0; i < VFS_TMPFS_MAX_FILES; i++) {
            if (!g_tmpfs_files[i].used || g_tmpfs_files[i].parent_object_id != dir_object_id) continue;
            if (seen++ < cursor) continue;
            write_dirent_response(
                seq,
                token_from_object_id(VFS_TMPFS_FILE_OBJECT_ID_BASE + i),
                cursor + 1,
                g_tmpfs_files[i].name,
                g_tmpfs_files[i].name_len,
                FS_OBJECT_FILE
            );
            return;
        }
        write_response(FS_OP_READDIR, seq, FS_STATUS_END_OF_DIR, 0, 0, cursor, FS_OBJECT_DIRECTORY, 0);
        return;
    }
    if (is_directory_token(token)) {
        write_response(FS_OP_READDIR, seq, FS_STATUS_END_OF_DIR, 0, 0, cursor, FS_OBJECT_DIRECTORY, 0);
        return;
    }
    write_response(FS_OP_READDIR, seq, FS_STATUS_NOT_FOUND, 0, 0, cursor, FS_OBJECT_NONE, 0);
}

static void reply_open(u64 seq, u64 token) {
    const u64 object_id = object_id_from_token(token);
    const u64 open_object_id = open_object_id_for_file(object_id);
    if (open_object_id == 0) {
        reply_status(FS_OP_OPEN, seq, FS_STATUS_NOT_FOUND);
        return;
    }
    clear_page(g_session->response_va);
    write_response(FS_OP_OPEN, seq, FS_STATUS_OK, token_from_object_id(open_object_id), file_bytes_from_token(token), 0, FS_OBJECT_OPEN_FILE, 0);
}

static int cpu_has_rdrand(void) {
    u32 eax = 1, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    (void)ebx;
    (void)edx;
    return (ecx & (1u << 30)) != 0;
}

static int rdrand64(u64 *out) {
    unsigned char ok = 0;
    u64 value = 0;
    for (u32 attempt = 0; attempt < 16; attempt++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
        if (ok) {
            *out = value;
            return 1;
        }
    }
    return 0;
}

static int fill_random_bytes(volatile u8 *payload, u16 bytes) {
    if (!cpu_has_rdrand()) return 0;
    u16 copied = 0;
    while (copied < bytes) {
        u64 value = 0;
        if (!rdrand64(&value)) return 0;
        const u16 chunk = (u16)(((u64)(bytes - copied) < sizeof(value)) ? (bytes - copied) : sizeof(value));
        for (u16 i = 0; i < chunk; i++) payload[copied + i] = (u8)(value >> (i * 8));
        copied = (u16)(copied + chunk);
    }
    return 1;
}

static void reply_read(u64 seq, u64 token, u64 offset, u32 length) {
    clear_page(g_session->response_va);
    const u64 object_id = object_id_from_token(token);
    if (object_id == VFS_DEV_NULL_OPEN_OBJECT_ID) {
        write_response(FS_OP_READ, seq, FS_STATUS_OK, 0, 0, offset, FS_OBJECT_OPEN_FILE, 0);
        return;
    }
    if (object_id == VFS_DEV_ZERO_OPEN_OBJECT_ID) {
        u16 bytes = (u16)length;
        if (bytes > FS_RESPONSE_PAYLOAD_BYTES) bytes = FS_RESPONSE_PAYLOAD_BYTES;
        volatile u8 *payload = (volatile u8 *)(g_session->response_va + FS_RESPONSE_HEADER_BYTES);
        for (u16 i = 0; i < bytes; i++) payload[i] = 0;
        write_response(FS_OP_READ, seq, FS_STATUS_OK, 0, 0, offset + bytes, FS_OBJECT_OPEN_FILE, bytes);
        return;
    }
    if (object_id == VFS_DEV_RANDOM_OPEN_OBJECT_ID || object_id == VFS_DEV_URANDOM_OPEN_OBJECT_ID) {
        u16 bytes = (u16)length;
        if (bytes > FS_RESPONSE_PAYLOAD_BYTES) bytes = FS_RESPONSE_PAYLOAD_BYTES;
        volatile u8 *payload = (volatile u8 *)(g_session->response_va + FS_RESPONSE_HEADER_BYTES);
        if (!fill_random_bytes(payload, bytes)) {
            write_response(FS_OP_READ, seq, FS_STATUS_IO_ERROR, 0, 0, offset, FS_OBJECT_OPEN_FILE, 0);
            return;
        }
        write_response(FS_OP_READ, seq, FS_STATUS_OK, 0, ~0ULL, offset + bytes, FS_OBJECT_OPEN_FILE, bytes);
        return;
    }
    u64 proc_bytes = 0;
    const char *proc_content = proc_content_from_object_id(object_id, &proc_bytes);
    if (proc_content != 0) {
        u16 bytes = 0;
        if (offset < proc_bytes) {
            const u64 remaining = proc_bytes - offset;
            bytes = (u16)(remaining < length ? remaining : length);
            if (bytes > FS_RESPONSE_PAYLOAD_BYTES) bytes = FS_RESPONSE_PAYLOAD_BYTES;
            volatile u8 *payload = (volatile u8 *)(g_session->response_va + FS_RESPONSE_HEADER_BYTES);
            for (u16 i = 0; i < bytes; i++) payload[i] = (u8)proc_content[offset + i];
        }
        write_response(FS_OP_READ, seq, FS_STATUS_OK, 0, proc_bytes, offset + bytes, FS_OBJECT_OPEN_FILE, bytes);
        return;
    }
    const u64 tmpfs_index = tmpfs_index_from_open_object_id(object_id);
    if (tmpfs_index < VFS_TMPFS_MAX_FILES && g_tmpfs_files[tmpfs_index].used) {
        struct vfs_tmpfs_file *file = &g_tmpfs_files[tmpfs_index];
        u16 bytes = 0;
        if (offset < file->size) {
            const u64 remaining = file->size - offset;
            bytes = (u16)(remaining < length ? remaining : length);
            if (bytes > FS_RESPONSE_PAYLOAD_BYTES) bytes = FS_RESPONSE_PAYLOAD_BYTES;
            volatile u8 *payload = (volatile u8 *)(g_session->response_va + FS_RESPONSE_HEADER_BYTES);
            for (u16 i = 0; i < bytes; i++) payload[i] = tmpfs_read_byte(tmpfs_index, offset + i);
        }
        write_response(FS_OP_READ, seq, FS_STATUS_OK, 0, file->size, offset + bytes, FS_OBJECT_OPEN_FILE, bytes);
        return;
    }
    write_response(FS_OP_READ, seq, FS_STATUS_NOT_FOUND, 0, 0, offset, FS_OBJECT_NONE, 0);
}

static void reply_write(u64 seq, u64 token, u64 offset, u32 length, const volatile u8 *payload, u16 inline_bytes) {
    const u64 object_id = object_id_from_token(token);
    if (!is_open_file_object_id(object_id)) {
        reply_status(FS_OP_WRITE, seq, FS_STATUS_NOT_FOUND);
        return;
    }
    if (object_id == VFS_PROC_CPUINFO_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_MEMINFO_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_UPTIME_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_STAT_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_MOUNTS_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_SELF_STAT_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_SELF_STATUS_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_NET_DEV_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_NET_ROUTE_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_NET_CAPABILITYOS_OPEN_OBJECT_ID ||
        object_id == VFS_PROC_DRIVER_RTC_OPEN_OBJECT_ID)
    {
        reply_status(FS_OP_WRITE, seq, FS_STATUS_NO_RIGHT);
        return;
    }
    const u64 tmpfs_index = tmpfs_index_from_open_object_id(object_id);
    if (tmpfs_index < VFS_TMPFS_MAX_FILES && g_tmpfs_files[tmpfs_index].used) {
        struct vfs_tmpfs_file *file = &g_tmpfs_files[tmpfs_index];
        if (length > inline_bytes || offset > VFS_TMPFS_FILE_BYTES || offset + length > VFS_TMPFS_FILE_BYTES) {
            reply_status(FS_OP_WRITE, seq, FS_STATUS_TOO_BIG);
            return;
        }
        if (!tmpfs_write_bytes(tmpfs_index, offset, payload, length)) {
            reply_status(FS_OP_WRITE, seq, FS_STATUS_IO_ERROR);
            return;
        }
        if (offset + length > file->size) file->size = (u32)(offset + length);
    }
    clear_page(g_session->response_va);
    write_response(
        FS_OP_WRITE,
        seq,
        FS_STATUS_OK,
        0,
        tmpfs_index < VFS_TMPFS_MAX_FILES ? g_tmpfs_files[tmpfs_index].size : offset + length,
        offset + length,
        FS_OBJECT_OPEN_FILE,
        0
    );
}

static int tmpfs_parent_from_path(const volatile u8 *path, u16 path_len, u64 *parent_out, const volatile u8 **name_out, u16 *name_len_out) {
    if (path_len == 0 || path_len > FS_MAX_PATH_BYTES) return 0;
    return tmpfs_parent_for_path(path, path_len, parent_out, name_out, name_len_out);
}

static int path_targets_tmpfs(const volatile u8 *path, u16 path_len) {
    u64 parent = 0;
    const volatile u8 *name = (const volatile u8 *)0;
    u16 name_len = 0;
    return tmpfs_parent_from_path(path, path_len, &parent, &name, &name_len);
}

static void reply_tmpfs_create(u64 seq, const volatile u8 *path, u16 path_len, u32 flags) {
    if ((flags & VFS_CREATE_FLAG_DIRECTORY) != 0) {
        reply_status(FS_OP_CREATE, seq, FS_STATUS_NOT_SUPPORTED);
        return;
    }
    u64 parent = 0;
    const volatile u8 *name = (const volatile u8 *)0;
    u16 name_len = 0;
    if (!tmpfs_parent_from_path(path, path_len, &parent, &name, &name_len)) {
        reply_status(FS_OP_CREATE, seq, FS_STATUS_NOT_DIR);
        return;
    }
    const u64 existing = lookup_tmpfs_name(parent, name, name_len);
    if (existing != 0) {
        const u64 index = tmpfs_index_from_file_object_id(object_id_from_token(existing));
        if ((flags & VFS_CREATE_FLAG_TRUNCATE) != 0 && index < VFS_TMPFS_MAX_FILES) {
            g_tmpfs_files[index].size = 0;
            tmpfs_clear_file_storage(index);
        }
        reply_file_lookup(FS_OP_CREATE, seq, existing);
        return;
    }
    for (u64 i = 0; i < VFS_TMPFS_MAX_FILES; i++) {
        if (g_tmpfs_files[i].used) continue;
        g_tmpfs_files[i].used = 1;
        g_tmpfs_files[i].parent_object_id = parent;
        g_tmpfs_files[i].name_len = name_len;
        g_tmpfs_files[i].size = 0;
        tmpfs_clear_file_storage(i);
        for (u16 j = 0; j < name_len; j++) g_tmpfs_files[i].name[j] = (char)name[j];
        g_tmpfs_files[i].name[name_len] = 0;
        reply_file_lookup(FS_OP_CREATE, seq, token_from_object_id(VFS_TMPFS_FILE_OBJECT_ID_BASE + i));
        return;
    }
    reply_status(FS_OP_CREATE, seq, FS_STATUS_BUSY);
}

static void reply_tmpfs_unlink(u64 seq, const volatile u8 *path, u16 path_len) {
    u64 parent = 0;
    const volatile u8 *name = (const volatile u8 *)0;
    u16 name_len = 0;
    if (!tmpfs_parent_from_path(path, path_len, &parent, &name, &name_len)) {
        reply_status(FS_OP_UNLINK, seq, FS_STATUS_NOT_DIR);
        return;
    }
    const u64 existing = lookup_tmpfs_name(parent, name, name_len);
    if (existing == 0) {
        reply_status(FS_OP_UNLINK, seq, FS_STATUS_NOT_FOUND);
        return;
    }
    const u64 index = tmpfs_index_from_file_object_id(object_id_from_token(existing));
    if (index >= VFS_TMPFS_MAX_FILES) {
        reply_status(FS_OP_UNLINK, seq, FS_STATUS_NOT_FOUND);
        return;
    }
    g_tmpfs_files[index].used = 0;
    g_tmpfs_files[index].size = 0;
    tmpfs_clear_file_storage(index);
    reply_status(FS_OP_UNLINK, seq, FS_STATUS_OK);
}

static void reply_tmpfs_rename(u64 seq, const volatile u8 *old_path, u16 old_path_len, const volatile u8 *new_path, u16 new_path_len) {
    u64 old_parent = 0;
    u64 new_parent = 0;
    const volatile u8 *old_name = (const volatile u8 *)0;
    const volatile u8 *new_name = (const volatile u8 *)0;
    u16 old_name_len = 0;
    u16 new_name_len = 0;
    if (!tmpfs_parent_from_path(old_path, old_path_len, &old_parent, &old_name, &old_name_len) ||
        !tmpfs_parent_from_path(new_path, new_path_len, &new_parent, &new_name, &new_name_len))
    {
        reply_status(FS_OP_RENAME, seq, FS_STATUS_NOT_DIR);
        return;
    }
    const u64 old_token = lookup_tmpfs_name(old_parent, old_name, old_name_len);
    if (old_token == 0) {
        reply_status(FS_OP_RENAME, seq, FS_STATUS_NOT_FOUND);
        return;
    }
    const u64 old_index = tmpfs_index_from_file_object_id(object_id_from_token(old_token));
    if (old_index >= VFS_TMPFS_MAX_FILES || !g_tmpfs_files[old_index].used) {
        reply_status(FS_OP_RENAME, seq, FS_STATUS_NOT_FOUND);
        return;
    }
    const u64 new_token = lookup_tmpfs_name(new_parent, new_name, new_name_len);
    if (new_token != 0) {
        const u64 new_index = tmpfs_index_from_file_object_id(object_id_from_token(new_token));
        if (new_index == old_index) {
            reply_status(FS_OP_RENAME, seq, FS_STATUS_OK);
            return;
        }
        if (new_index < VFS_TMPFS_MAX_FILES) g_tmpfs_files[new_index].used = 0;
    }
    g_tmpfs_files[old_index].parent_object_id = new_parent;
    g_tmpfs_files[old_index].name_len = new_name_len;
    for (u16 j = 0; j < new_name_len; j++) g_tmpfs_files[old_index].name[j] = (char)new_name[j];
    g_tmpfs_files[old_index].name[new_name_len] = 0;
    reply_status(FS_OP_RENAME, seq, FS_STATUS_OK);
}

static u64 make_backend_session_nonce(u64 request_paddr, u64 response_paddr, u64 endpoint_id, u64 process_slot) {
    u64 nonce = request_paddr ^
        ((response_paddr << 17) | (response_paddr >> 47)) ^
        ((endpoint_id << 7) | (endpoint_id >> 57)) ^
        process_slot ^
        0x9e3779b97f4a7c15ULL;
    return nonce == 0 ? 1 : nonce;
}

static int install_backend_endpoint(void) {
    if (g_root_backend.endpoint_id == 0 || g_root_backend.process_handle == 0) return 0;
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, g_root_backend.endpoint_id, g_root_backend.process_handle) == SYSCALL_OK;
}

static int grant_backend_response_page(void) {
    u64 ret = syscall3(
        SYSCALL_GRANT_CAP_ON_ENDPOINT,
        g_root_backend.response_paddr,
        g_root_backend.endpoint_id,
        PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE
    );
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_backend_endpoint()) {
        ret = syscall3(
            SYSCALL_GRANT_CAP_ON_ENDPOINT,
            g_root_backend.response_paddr,
            g_root_backend.endpoint_id,
            PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE
        );
    }
    return ret == SYSCALL_OK;
}

static int share_backend_request_page(void) {
    u64 ret = syscall2(SYSCALL_SHARE_CAP, g_root_backend.request_paddr, g_root_backend.endpoint_id);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_backend_endpoint()) {
        ret = syscall2(SYSCALL_SHARE_CAP, g_root_backend.request_paddr, g_root_backend.endpoint_id);
    }
    return ret == SYSCALL_OK;
}

static int signal_backend(void) {
    u64 ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_root_backend.endpoint_id, 0);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_backend_endpoint()) {
        ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_root_backend.endpoint_id, 0);
    }
    return ret == SYSCALL_OK;
}

static int wait_backend_response(u64 expected_seq, u16 expected_op) {
    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_root_backend.response_va;
    for (u64 i = 0; i < 8192; i++) {
        if (response->response_seq == expected_seq) {
            return response->magic == FS_RESPONSE_MAGIC &&
                response->version == FS_PROTOCOL_VERSION &&
                response->op == expected_op;
        }
        (void)wait_event_poll();
    }
    return 0;
}

static int connect_root_backend(u64 endpoint_id, u64 process_slot) {
    if (endpoint_id == 0 || process_slot == 0) return 0;

    g_root_backend.endpoint_id = endpoint_id;
    g_root_backend.process_handle = process_slot;
    g_root_backend.request_va = VFS_FAT_REQUEST_VA;
    g_root_backend.response_va = VFS_FAT_RESPONSE_VA;
    g_root_backend.request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    g_root_backend.response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (g_root_backend.request_paddr < 0x1000 || g_root_backend.response_paddr < 0x1000) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, g_root_backend.request_va, g_root_backend.request_paddr, 1) != SYSCALL_OK) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, g_root_backend.response_va, g_root_backend.response_paddr, 1) != SYSCALL_OK) return 0;
    if (!grant_backend_response_page()) return 0;

    clear_page(g_root_backend.request_va);
    clear_page(g_root_backend.response_va);
    const u64 process_slot_self = syscall0(SYSCALL_GET_PROCESS_HANDLE);
    g_root_backend.session_nonce = make_backend_session_nonce(
        g_root_backend.request_paddr,
        g_root_backend.response_paddr,
        endpoint_id,
        process_slot_self
    );
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)g_root_backend.request_va;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = FS_OP_CONNECT;
    request->object_token = 0;
    request->offset = 0;
    request->length = 0;
    request->flags = 0;
    request->path_bytes = 0;
    request->inline_bytes = 0;
    request->reserved0 = 0;
    request->arg0 = g_root_backend.response_paddr;
    request->arg1 = process_slot_self;
    request->session_nonce = g_root_backend.session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = 1;

    if (!share_backend_request_page()) return 0;
    if (!wait_backend_response(1, FS_OP_CONNECT)) return 0;

    volatile struct fs_response_header *response = (volatile struct fs_response_header *)g_root_backend.response_va;
    if (response->status != FS_STATUS_OK || !is_cap_token(response->result_token)) return 0;
    g_root_backend.root_token = response->result_token;
    g_root_backend.next_seq = 2;
    g_root_backend.active = 1;
    return 1;
}

static u64 make_net_session_nonce(u64 request_paddr, u64 response_paddr, u64 endpoint_id, u64 process_slot) {
    u64 nonce = request_paddr ^
        ((response_paddr << 17) | (response_paddr >> 47)) ^
        ((endpoint_id << 7) | (endpoint_id >> 57)) ^
        process_slot ^
        0x6e65742d73746174ULL;
    return nonce == 0 ? 1 : nonce;
}

static int install_net_endpoint(void) {
    if (g_net_backend.endpoint_id == 0 || g_net_backend.process_handle == 0) return 0;
    return syscall3(SYSCALL_INSTALL_ENDPOINT, 0, g_net_backend.endpoint_id, g_net_backend.process_handle) == SYSCALL_OK;
}

static int grant_net_response_page(void) {
    u64 ret = syscall3(
        SYSCALL_GRANT_CAP_ON_ENDPOINT,
        g_net_backend.response_paddr,
        g_net_backend.endpoint_id,
        PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE
    );
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_net_endpoint()) {
        ret = syscall3(
            SYSCALL_GRANT_CAP_ON_ENDPOINT,
            g_net_backend.response_paddr,
            g_net_backend.endpoint_id,
            PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE
        );
    }
    return ret == SYSCALL_OK;
}

static int share_net_request_page(void) {
    u64 ret = syscall2(SYSCALL_SHARE_CAP, g_net_backend.request_paddr, g_net_backend.endpoint_id);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_net_endpoint()) {
        ret = syscall2(SYSCALL_SHARE_CAP, g_net_backend.request_paddr, g_net_backend.endpoint_id);
    }
    return ret == SYSCALL_OK;
}

static int signal_net_backend(void) {
    u64 ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_net_backend.endpoint_id, 0);
    if (ret == SYSCALL_OK) return 1;
    if (ret == SYSCALL_ERR_ENDPOINT && install_net_endpoint()) {
        ret = syscall2(SYSCALL_SIGNAL_ENDPOINT, g_net_backend.endpoint_id, 0);
    }
    return ret == SYSCALL_OK;
}

static int wait_net_response(u64 expected_seq, u16 expected_op) {
    volatile struct net_response_header *response = (volatile struct net_response_header *)g_net_backend.response_va;
    for (u64 i = 0; i < 1024; i++) {
        if (response->response_seq == expected_seq) {
            return response->magic == NET_PROTOCOL_RESPONSE_MAGIC &&
                response->version == NET_PROTOCOL_VERSION &&
                response->op == expected_op &&
                response->status == NET_STATUS_OK;
        }
        (void)wait_event_poll();
    }
    return 0;
}

static int connect_net_backend(u64 endpoint_id, u64 process_slot) {
    if (endpoint_id == 0 || process_slot == 0) return 0;

    g_net_backend.endpoint_id = endpoint_id;
    g_net_backend.process_handle = process_slot;
    g_net_backend.request_va = VFS_NET_REQUEST_VA;
    g_net_backend.response_va = VFS_NET_RESPONSE_VA;
    g_net_backend.request_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    g_net_backend.response_paddr = syscall0(SYSCALL_ALLOC_PAGE);
    if (g_net_backend.request_paddr < 0x1000 || g_net_backend.response_paddr < 0x1000) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, g_net_backend.request_va, g_net_backend.request_paddr, 1) != SYSCALL_OK) return 0;
    if (syscall3(SYSCALL_MAP_PAGE, g_net_backend.response_va, g_net_backend.response_paddr, 1) != SYSCALL_OK) return 0;
    if (!grant_net_response_page()) return 0;

    clear_page(g_net_backend.request_va);
    clear_page(g_net_backend.response_va);
    const u64 process_slot_self = syscall0(SYSCALL_GET_PROCESS_HANDLE);
    g_net_backend.session_nonce = make_net_session_nonce(
        g_net_backend.request_paddr,
        g_net_backend.response_paddr,
        endpoint_id,
        process_slot_self
    );

    volatile struct net_request_header *request = (volatile struct net_request_header *)g_net_backend.request_va;
    request->magic = NET_PROTOCOL_REQUEST_MAGIC;
    request->version = NET_PROTOCOL_VERSION;
    request->op = NET_OP_CONNECT;
    request->arg0 = g_net_backend.response_paddr;
    request->arg1 = process_slot_self;
    request->arg2 = 0;
    request->reserved0 = 0;
    request->session_nonce = g_net_backend.session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = 1;

    if (!share_net_request_page()) return 0;
    if (!wait_net_response(1, NET_OP_CONNECT)) return 0;
    g_net_backend.next_seq = 2;
    g_net_backend.active = 1;
    user_log("RootVfs: net backend connect ok\n");
    return 1;
}

static int ensure_net_backend_connected(void) {
    if (g_net_backend.active) return 1;
    if (g_net_endpoint_id == 0 || g_net_process_slot == 0) return 0;
    if (connect_net_backend(g_net_endpoint_id, g_net_process_slot)) return 1;
    return 0;
}

static int refresh_net_status(void) {
    if (!ensure_net_backend_connected()) return 0;
    clear_page(g_net_backend.request_va);
    clear_page(g_net_backend.response_va);
    const u64 seq = g_net_backend.next_seq++;
    volatile struct net_request_header *request = (volatile struct net_request_header *)g_net_backend.request_va;
    request->magic = NET_PROTOCOL_REQUEST_MAGIC;
    request->version = NET_PROTOCOL_VERSION;
    request->op = NET_OP_GET_STATUS;
    request->arg0 = 0;
    request->arg1 = 0;
    request->arg2 = 0;
    request->reserved0 = 0;
    request->session_nonce = g_net_backend.session_nonce;
    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_net_backend()) return 0;
    if (!wait_net_response(seq, NET_OP_GET_STATUS)) {
        g_net_backend.active = 0;
        return 0;
    }
    volatile struct net_response_header *response = (volatile struct net_response_header *)g_net_backend.response_va;
    if (response->inline_bytes < sizeof(struct net_status_payload)) return 0;
    volatile struct net_status_payload *payload = (volatile struct net_status_payload *)(g_net_backend.response_va + sizeof(struct net_response_header));
    for (u64 i = 0; i < 6; i++) g_net_backend.last_status.mac[i] = payload->mac[i];
    g_net_backend.last_status.link_up = payload->link_up;
    g_net_backend.last_status.dhcp_bound = payload->dhcp_bound;
    g_net_backend.last_status.ipv4_addr = payload->ipv4_addr;
    g_net_backend.last_status.gateway_addr = payload->gateway_addr;
    g_net_backend.last_status.dns_addr = payload->dns_addr;
    g_net_backend.last_status.dhcp_server_addr = payload->dhcp_server_addr;
    g_net_backend.last_status.flags = payload->flags;
    g_net_backend.last_status.rx_packets = payload->rx_packets;
    g_net_backend.last_status.tx_completions = payload->tx_completions;
    g_net_backend.last_status.tcp_rx_segments = payload->tcp_rx_segments;
    g_net_backend.last_status.tcp_rx_payload_bytes = payload->tcp_rx_payload_bytes;
    g_net_backend.last_status.tcp_rx_in_order_bytes = payload->tcp_rx_in_order_bytes;
    g_net_backend.last_status.tcp_rx_duplicate_segments = payload->tcp_rx_duplicate_segments;
    g_net_backend.last_status.tcp_rx_out_of_order_segments = payload->tcp_rx_out_of_order_segments;
    g_net_backend.last_status.tcp_rx_ooo_stored = payload->tcp_rx_ooo_stored;
    g_net_backend.last_status.tcp_rx_ooo_drained = payload->tcp_rx_ooo_drained;
    g_net_backend.last_status.tcp_rx_ooo_dropped = payload->tcp_rx_ooo_dropped;
    g_net_backend.last_status.tcp_rx_append_failed = payload->tcp_rx_append_failed;
    g_net_backend.last_status.tcp_ack_sent = payload->tcp_ack_sent;
    g_net_backend.last_status.tcp_ack_deferred = payload->tcp_ack_deferred;
    g_net_backend.last_status.tcp_ack_flushed = payload->tcp_ack_flushed;
    g_net_backend.last_status.tcp_tx_busy = payload->tcp_tx_busy;
    g_net_backend.last_status.tcp_connect_requests = payload->tcp_connect_requests;
    g_net_backend.last_status.tcp_connect_established = payload->tcp_connect_established;
    g_net_backend.last_status.tcp_tx_segments = payload->tcp_tx_segments;
    g_net_backend.last_status.tcp_tx_payload_bytes = payload->tcp_tx_payload_bytes;
    g_net_backend.last_status.tcp_read_requests = payload->tcp_read_requests;
    g_net_backend.last_status.tcp_read_would_block = payload->tcp_read_would_block;
    g_net_backend.last_status.tcp_read_bytes = payload->tcp_read_bytes;
    g_net_backend.last_status.tcp_poll_requests = payload->tcp_poll_requests;
    g_net_backend.last_status.tcp_poll_readable = payload->tcp_poll_readable;
    g_net_backend.last_status.tcp_rx_syn_ack = payload->tcp_rx_syn_ack;
    g_net_backend.last_status.tcp_rx_fin = payload->tcp_rx_fin;
    g_net_backend.last_status.tcp_rx_rst = payload->tcp_rx_rst;
    g_net_backend.last_status.tcp_active_connections = payload->tcp_active_connections;
    g_net_backend.last_status.tcp_established_connections = payload->tcp_established_connections;
    g_net_backend.last_status.tcp_rx_buffered_bytes = payload->tcp_rx_buffered_bytes;
    g_net_backend.last_status.tcp_rx_buffer_max_bytes = payload->tcp_rx_buffer_max_bytes;
    g_net_backend.last_status.tcp_ack_pending_connections = payload->tcp_ack_pending_connections;
    g_net_backend.last_status.net_service_requests = payload->net_service_requests;
    g_net_backend.last_status.net_service_work_loops = payload->net_service_work_loops;
    g_net_backend.last_status.net_service_idle_sleeps = payload->net_service_idle_sleeps;
    return 1;
}

static int backend_request(
    u16 op,
    u64 backend_token,
    u64 offset,
    u32 length,
    u32 flags,
    const volatile u8 *path,
    u16 path_bytes,
    const volatile u8 *inline_payload,
    u16 inline_bytes
) {
    if (!g_root_backend.active) return 0;
    if ((u64)path_bytes + (u64)inline_bytes > FS_PAGE_BYTES - FS_REQUEST_HEADER_BYTES) return 0;
    const u64 seq = g_root_backend.next_seq++;
    clear_page(g_root_backend.request_va);
    clear_page(g_root_backend.response_va);

    volatile struct fs_request_header *request = (volatile struct fs_request_header *)g_root_backend.request_va;
    request->magic = FS_REQUEST_MAGIC;
    request->version = FS_PROTOCOL_VERSION;
    request->op = op;
    request->object_token = backend_token;
    request->offset = offset;
    request->length = length;
    request->flags = flags;
    request->path_bytes = path_bytes;
    request->inline_bytes = inline_bytes;
    request->reserved0 = 0;
    request->arg0 = 0;
    request->arg1 = 0;
    request->session_nonce = g_root_backend.session_nonce;

    volatile u8 *payload = (volatile u8 *)(g_root_backend.request_va + FS_REQUEST_HEADER_BYTES);
    for (u16 i = 0; i < path_bytes; i++) payload[i] = path[i];
    for (u16 i = 0; i < inline_bytes; i++) payload[path_bytes + i] = inline_payload[i];

    __asm__ volatile("" ::: "memory");
    request->request_seq = seq;
    if (!signal_backend()) return 0;
    return wait_backend_response(seq, op);
}

static void forward_backend_response(u16 op, u64 client_seq, u64 cursor_bias) {
    volatile struct fs_response_header *backend = (volatile struct fs_response_header *)g_root_backend.response_va;
    u16 inline_bytes = backend->inline_bytes;
    if (inline_bytes > FS_RESPONSE_PAYLOAD_BYTES) {
        reply_status(op, client_seq, FS_STATUS_INVALID);
        return;
    }

    clear_page(g_session->response_va);
    volatile u8 *dst = (volatile u8 *)(g_session->response_va + FS_RESPONSE_HEADER_BYTES);
    const volatile u8 *src = (const volatile u8 *)(g_root_backend.response_va + FS_RESPONSE_HEADER_BYTES);
    copy_from_volatile(dst, src, inline_bytes);

    u64 result_token = backend->result_token == 0 ? 0 : wrap_backend_token(backend->result_token);
    u64 cursor_next = backend->cursor_next;
    if (op == FS_OP_READDIR) {
        cursor_next += cursor_bias;
        if (backend->status == FS_STATUS_OK && inline_bytes >= FS_DIRENT_RECORD_BYTES) {
            volatile struct fs_dirent_record *record = (volatile struct fs_dirent_record *)dst;
            record->next_cursor += cursor_bias;
        }
    }
    write_response(op, client_seq, backend->status, result_token, backend->file_bytes, cursor_next, backend->object_kind, inline_bytes);
}

static int backend_readdir_entry_shadowed_by_builtin_root(void) {
    volatile struct fs_response_header *backend = (volatile struct fs_response_header *)g_root_backend.response_va;
    if (backend->status != FS_STATUS_OK || backend->inline_bytes < FS_DIRENT_RECORD_BYTES) return 0;
    volatile struct fs_dirent_record *record = (volatile struct fs_dirent_record *)(g_root_backend.response_va + FS_RESPONSE_HEADER_BYTES);
    if (backend->inline_bytes < FS_DIRENT_RECORD_BYTES + record->name_bytes) return 0;
    const volatile u8 *name = (const volatile u8 *)(g_root_backend.response_va + FS_RESPONSE_HEADER_BYTES + FS_DIRENT_RECORD_BYTES);
    return root_mount_name_exists(name, record->name_bytes);
}

static void forward_backend_request(
    u16 op,
    u64 client_seq,
    u64 backend_token,
    u64 offset,
    u32 length,
    u32 flags,
    const volatile u8 *path,
    u16 path_bytes,
    const volatile u8 *inline_payload,
    u16 inline_bytes,
    u64 cursor_bias
) {
    if (!backend_request(op, backend_token, offset, length, flags, path, path_bytes, inline_payload, inline_bytes)) {
        reply_status(op, client_seq, FS_STATUS_IO_ERROR);
        return;
    }
    forward_backend_response(op, client_seq, cursor_bias);
}

static void forward_backend_root_readdir_filtered(u64 client_seq, u64 backend_cursor, u32 length, u32 flags, u64 cursor_bias) {
    for (u64 guard = 0; guard < 64; guard++) {
        if (!backend_request(
            FS_OP_READDIR,
            g_root_backend.root_token,
            backend_cursor,
            length,
            flags,
            (const volatile u8 *)0,
            0,
            (const volatile u8 *)0,
            0
        )) {
            reply_status(FS_OP_READDIR, client_seq, FS_STATUS_IO_ERROR);
            return;
        }

        volatile struct fs_response_header *backend = (volatile struct fs_response_header *)g_root_backend.response_va;
        if (backend->status != FS_STATUS_OK) {
            forward_backend_response(FS_OP_READDIR, client_seq, cursor_bias);
            return;
        }
        if (!backend_readdir_entry_shadowed_by_builtin_root()) {
            forward_backend_response(FS_OP_READDIR, client_seq, cursor_bias);
            return;
        }

        volatile struct fs_dirent_record *record = (volatile struct fs_dirent_record *)(g_root_backend.response_va + FS_RESPONSE_HEADER_BYTES);
        if (record->next_cursor <= backend_cursor) {
            reply_status(FS_OP_READDIR, client_seq, FS_STATUS_INVALID);
            return;
        }
        backend_cursor = record->next_cursor;
    }
    reply_status(FS_OP_READDIR, client_seq, FS_STATUS_BUSY);
}

static void handle_fs_request(void) {
    if (g_session == 0 || !g_session->active) return;
    volatile struct fs_request_header *request = (volatile struct fs_request_header *)g_session->request_va;
    if (request->magic != FS_REQUEST_MAGIC || request->version != FS_PROTOCOL_VERSION) return;
    const u64 seq = request->request_seq;
    if (seq == 0 || seq <= g_session->last_completed_seq) return;
    if (request->session_nonce != g_session->session_nonce) return;

    if (request->op == FS_OP_CONNECT) {
        reply_status(FS_OP_CONNECT, seq, FS_STATUS_BUSY);
    } else if (request->op == FS_OP_LOOKUP) {
        if (request->path_bytes > FS_MAX_PATH_BYTES) {
            reply_status(FS_OP_LOOKUP, seq, FS_STATUS_INVALID);
        } else {
            const volatile u8 *path = (const volatile u8 *)(g_session->request_va + FS_REQUEST_HEADER_BYTES);
            if (is_backend_token(request->object_token)) {
                forward_backend_request(
                    FS_OP_LOOKUP,
                    seq,
                    unwrap_backend_token(request->object_token),
                    0,
                    0,
                    0,
                    path,
                    request->path_bytes,
                    (const volatile u8 *)0,
                    0,
                    0
                );
            } else {
                const u64 token = lookup_path(request->object_token, path, request->path_bytes);
                if (token != 0 && is_directory_token(token)) reply_dir_lookup(FS_OP_LOOKUP, seq, token);
                else if (token != 0 && is_file_token(token)) reply_file_lookup(FS_OP_LOOKUP, seq, token);
                else if (path_should_not_forward_backend(request->object_token, path, request->path_bytes)) {
                    reply_status(FS_OP_LOOKUP, seq, FS_STATUS_NOT_FOUND);
                }
                else if (g_root_backend.active && object_id_from_token(request->object_token) == VFS_ROOT_OBJECT_ID) {
                    forward_backend_request(
                        FS_OP_LOOKUP,
                        seq,
                        g_root_backend.root_token,
                        0,
                        0,
                        0,
                        path,
                        request->path_bytes,
                        (const volatile u8 *)0,
                        0,
                        0
                    );
                } else {
                    reply_status(FS_OP_LOOKUP, seq, FS_STATUS_NOT_FOUND);
                }
            }
        }
    } else if (request->op == FS_OP_STAT) {
        if (is_backend_token(request->object_token)) {
            forward_backend_request(
                FS_OP_STAT,
                seq,
                unwrap_backend_token(request->object_token),
                0,
                0,
                0,
                (const volatile u8 *)0,
                0,
                (const volatile u8 *)0,
                0,
                0
            );
        } else if (is_directory_token(request->object_token) || is_file_token(request->object_token) || is_open_file_token(request->object_token)) {
            reply_stat(seq, request->object_token);
        }
        else reply_status(FS_OP_STAT, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_READDIR) {
        if (is_backend_token(request->object_token)) {
            forward_backend_request(
                FS_OP_READDIR,
                seq,
                unwrap_backend_token(request->object_token),
                request->offset,
                request->length,
                request->flags,
                (const volatile u8 *)0,
                0,
                (const volatile u8 *)0,
                0,
                0
            );
        } else if (request->object_token == root_token() &&
            g_root_backend.active &&
            request->offset >= sizeof(g_root_mounts) / sizeof(g_root_mounts[0]))
        {
            const u64 bias = sizeof(g_root_mounts) / sizeof(g_root_mounts[0]);
            forward_backend_root_readdir_filtered(
                seq,
                request->offset - bias,
                request->length,
                request->flags,
                bias
            );
        } else if (is_directory_token(request->object_token)) reply_readdir(seq, request->object_token, request->offset);
        else reply_status(FS_OP_READDIR, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_CLOSE) {
        if (is_backend_token(request->object_token)) {
            forward_backend_request(
                FS_OP_CLOSE,
                seq,
                unwrap_backend_token(request->object_token),
                0,
                0,
                0,
                (const volatile u8 *)0,
                0,
                (const volatile u8 *)0,
                0,
                0
            );
        } else {
            reply_status(FS_OP_CLOSE, seq, FS_STATUS_OK);
        }
    } else if (request->op == FS_OP_STATFS) {
        if (is_backend_token(request->object_token) || (request->object_token == root_token() && g_root_backend.active)) {
            forward_backend_request(
                FS_OP_STATFS,
                seq,
                is_backend_token(request->object_token) ? unwrap_backend_token(request->object_token) : g_root_backend.root_token,
                0,
                0,
                0,
                (const volatile u8 *)0,
                0,
                (const volatile u8 *)0,
                0,
                0
            );
        } else {
            clear_page(g_session->response_va);
            write_response(FS_OP_STATFS, seq, FS_STATUS_OK, 0, 0, 0, FS_OBJECT_MOUNT, 0);
        }
    } else if (request->op == FS_OP_OPEN) {
        if (is_backend_token(request->object_token)) {
            forward_backend_request(
                request->op,
                seq,
                unwrap_backend_token(request->object_token),
                0,
                0,
                request->flags,
                (const volatile u8 *)0,
                0,
                (const volatile u8 *)0,
                0,
                0
            );
        } else if (is_directory_token(request->object_token)) reply_status(request->op, seq, FS_STATUS_IS_DIR);
        else if (is_file_token(request->object_token)) reply_open(seq, request->object_token);
        else reply_status(request->op, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_OPEN_EXEC) {
        if (is_backend_token(request->object_token)) {
            forward_backend_request(
                request->op,
                seq,
                unwrap_backend_token(request->object_token),
                0,
                0,
                request->flags,
                (const volatile u8 *)0,
                0,
                (const volatile u8 *)0,
                0,
                0
            );
        } else if (is_directory_token(request->object_token)) reply_status(request->op, seq, FS_STATUS_IS_DIR);
        else if (is_file_token(request->object_token)) reply_status(request->op, seq, FS_STATUS_NO_RIGHT);
        else reply_status(request->op, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_READ) {
        if (is_backend_token(request->object_token)) {
            forward_backend_request(
                request->op,
                seq,
                unwrap_backend_token(request->object_token),
                request->offset,
                request->length,
                request->flags,
                (const volatile u8 *)0,
                0,
                (const volatile u8 *)0,
                0,
                0
            );
        } else if (is_open_file_token(request->object_token)) reply_read(seq, request->object_token, request->offset, request->length);
        else reply_status(request->op, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_WRITE) {
        if (is_backend_token(request->object_token)) {
            const volatile u8 *payload = (const volatile u8 *)(g_session->request_va + FS_REQUEST_HEADER_BYTES + request->path_bytes);
            forward_backend_request(
                request->op,
                seq,
                unwrap_backend_token(request->object_token),
                request->offset,
                request->length,
                request->flags,
                (const volatile u8 *)0,
                0,
                payload,
                request->inline_bytes,
                0
            );
        } else if (is_open_file_token(request->object_token)) {
            const volatile u8 *payload = (const volatile u8 *)(g_session->request_va + FS_REQUEST_HEADER_BYTES + request->path_bytes);
            reply_write(seq, request->object_token, request->offset, request->length, payload, request->inline_bytes);
        }
        else reply_status(request->op, seq, FS_STATUS_NOT_FOUND);
    } else if (request->op == FS_OP_CREATE || request->op == FS_OP_UNLINK || request->op == FS_OP_RENAME) {
        const volatile u8 *path = (const volatile u8 *)(g_session->request_va + FS_REQUEST_HEADER_BYTES);
        if (request->op == FS_OP_CREATE &&
            object_id_from_token(request->object_token) == VFS_ROOT_OBJECT_ID &&
            request->path_bytes > 0 &&
            request->path_bytes <= FS_MAX_PATH_BYTES &&
            path_targets_tmpfs(path, request->path_bytes))
        {
            reply_tmpfs_create(seq, path, request->path_bytes, request->flags);
        } else if (request->op == FS_OP_UNLINK &&
            object_id_from_token(request->object_token) == VFS_ROOT_OBJECT_ID &&
            request->path_bytes > 0 &&
            request->path_bytes <= FS_MAX_PATH_BYTES &&
            path_targets_tmpfs(path, request->path_bytes))
        {
            reply_tmpfs_unlink(seq, path, request->path_bytes);
        } else if (request->op == FS_OP_RENAME &&
            object_id_from_token(request->object_token) == VFS_ROOT_OBJECT_ID &&
            request->path_bytes > 0 &&
            request->path_bytes <= FS_MAX_PATH_BYTES &&
            request->inline_bytes > 0 &&
            request->inline_bytes <= FS_MAX_PATH_BYTES &&
            path_targets_tmpfs(path, request->path_bytes) &&
            path_targets_tmpfs(path + request->path_bytes, request->inline_bytes))
        {
            reply_tmpfs_rename(seq, path, request->path_bytes, path + request->path_bytes, request->inline_bytes);
        } else if (is_backend_token(request->object_token)) {
            const volatile u8 *path = (const volatile u8 *)(g_session->request_va + FS_REQUEST_HEADER_BYTES);
            const volatile u8 *inline_payload = path + request->path_bytes;
            forward_backend_request(
                request->op,
                seq,
                unwrap_backend_token(request->object_token),
                request->offset,
                request->length,
                request->flags,
                path,
                request->path_bytes,
                inline_payload,
                request->inline_bytes,
                0
            );
        } else if (g_root_backend.active && object_id_from_token(request->object_token) == VFS_ROOT_OBJECT_ID) {
            const volatile u8 *path = (const volatile u8 *)(g_session->request_va + FS_REQUEST_HEADER_BYTES);
            const volatile u8 *inline_payload = path + request->path_bytes;
            forward_backend_request(
                request->op,
                seq,
                g_root_backend.root_token,
                request->offset,
                request->length,
                request->flags,
                path,
                request->path_bytes,
                inline_payload,
                request->inline_bytes,
                0
            );
        } else {
            reply_status(request->op, seq, FS_STATUS_NO_RIGHT);
        }
    } else {
        reply_status(request->op, seq, FS_STATUS_NOT_SUPPORTED);
    }

    g_session->last_completed_seq = seq;
}

static void handle_connect_transfer(u64 transfer_id) {
    const u64 request_paddr = syscall2(SYSCALL_ACCEPT_CAP_TRANSFER, transfer_id, 0);
    if (request_paddr < 0x1000) return;
    struct vfs_session *session = find_session_by_request_paddr(request_paddr);
    const int is_new_session = session == 0;
    if (session == 0) session = alloc_session_slot();
    if (session == 0) {
        user_log("RootVfs: no free session slot\n");
        return;
    }

    const u64 slot = session_slot(session);
    const u64 request_va = session_request_va(slot);
    const u64 response_va = session_response_va(slot);
    if (is_new_session && syscall3(SYSCALL_MAP_PAGE, request_va, request_paddr, 0) != SYSCALL_OK) return;

    volatile struct fs_request_header *request = (volatile struct fs_request_header *)request_va;
    if (request->magic != FS_REQUEST_MAGIC ||
        request->version != FS_PROTOCOL_VERSION ||
        request->op != FS_OP_CONNECT ||
        request->request_seq == 0 ||
        request->arg0 < 0x1000 ||
        request->session_nonce == 0)
    {
        user_log("RootVfs: invalid connect request\n");
        return;
    }
    if (is_new_session && syscall3(SYSCALL_MAP_PAGE, response_va, request->arg0, 1) != SYSCALL_OK) return;

    clear_page(response_va);
    session->active = 1;
    session->request_va = request_va;
    session->response_va = response_va;
    session->request_paddr = request_paddr;
    session->response_paddr = request->arg0;
    session->reply_endpoint_id = syscall3(SYSCALL_INSTALL_ENDPOINT, 0, VFS_REPLY_ENDPOINT_ID + slot, request->arg1) == SYSCALL_OK
        ? VFS_REPLY_ENDPOINT_ID + slot
        : 0;
    session->session_nonce = request->session_nonce;
    session->last_completed_seq = 0;
    session->root_token = root_token();
    g_session = session;
    write_response(FS_OP_CONNECT, request->request_seq, FS_STATUS_OK, session->root_token, 0, 0, FS_OBJECT_MOUNT, 0);
    session->last_completed_seq = request->request_seq;
    user_log("RootVfs: session connect ok\n");
}

void rootfs_vfs_main(void) {
    user_log("RootVfs: started\n");
    user_log("RootVfs: builtin mounts /dev /proc /tmp /run\n");
    volatile u64 *config = (volatile u64 *)VFS_CONFIG_VA;
    g_endpoint_id = config[0];
    const u64 fat_endpoint_id = config[3];
    const u64 fat_process_slot = config[4];
    g_net_endpoint_id = config[5];
    g_net_process_slot = config[6];
    if (connect_root_backend(fat_endpoint_id, fat_process_slot)) {
        user_log("RootVfs: fat backend connect ok\n");
    } else if (fat_endpoint_id != 0) {
        user_log("RootVfs: fat backend connect failed\n");
    } else {
        user_log("RootVfs: fat backend missing\n");
    }
    if (g_endpoint_id != 0) {
        user_log("RootVfs: endpoint ready\n");
        config[2] = 1;
    } else {
        user_log("RootVfs: endpoint missing\n");
    }
    if (g_net_endpoint_id != 0 && g_net_process_slot != 0) {
        user_log("RootVfs: net backend lazy\n");
    } else {
        user_log("RootVfs: net backend missing\n");
    }
    for (;;) {
        const u64 received = wait_event();
        if (received >= CAP_TRANSFER_ID_MIN) handle_connect_transfer(received);
        for (u64 i = 0; i < VFS_MAX_SESSIONS; i++) {
            if (!g_sessions[i].active) continue;
            g_session = &g_sessions[i];
            handle_fs_request();
        }
    }
}
