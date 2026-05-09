typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;
typedef int i32;

#define OFFSETOF(type, member) __builtin_offsetof(type, member)

enum {
    SYSCALL_ALLOC_PAGE = 0x1,
    SYSCALL_MAP_PAGE = 0x2,
    SYSCALL_LOG = 0x9,
    SYSCALL_ALLOC_MAP_PAGES = 0xC,
    SYSCALL_WAIT_EVENT = 0x17,
    SYSCALL_SPAWN_EXEC = 0x1D,
    SYSCALL_INSTALL_VM_OBJECT = 0x1E,
    SYSCALL_GRANT_VM_OBJECT = 0x1F,
    SYSCALL_INSTALL_EXEC_IMAGE = 0x20,
    SYSCALL_GRANT_CAP_ON_ENDPOINT = 0x24,
    SYSCALL_INSTALL_ENDPOINT = 0x26,
    SYSCALL_SHARE_CAP = 0x2B,
    SYSCALL_SIGNAL_ENDPOINT = 0x2C,
    SYSCALL_GET_TICK_COUNT = 0x2D,
    SYSCALL_GET_PROCESS_SLOT = 0x2E,
    SYSCALL_GET_PROCESS_STATUS = 0x30,
    SYSCALL_PROCESS_EXIT = 0x34,
    SYSCALL_GET_RTC_UNIX_TIME = 0x3E,
    SYSCALL_IPC_CALL_REPLY_RECV = 0x40,
    SYSCALL_MAP_ABI_TRAP_REPLY_TARGET_PAGES = 0x4C,
    SYSCALL_COPY_FROM_ABI_TRAP_REPLY_TARGET = 0x4D,
    SYSCALL_COPY_TO_ABI_TRAP_REPLY_TARGET = 0x4E,
    SYSCALL_SET_ABI_TRAP_REPLY_TARGET_FS_BASE = 0x4F,
    SYSCALL_PROTECT_ABI_TRAP_REPLY_TARGET_PAGES = 0x50,
    SYSCALL_UNMAP_ABI_TRAP_REPLY_TARGET_PAGES = 0x51,
    SYSCALL_RECLAIM_ABI_TRAP_REPLY_TARGET_PRIVATE_PAGES = 0x52,
    SYSCALL_FORK_ABI_TRAP_REPLY_TARGET = 0x53,
    SYSCALL_REPLY_ABI_TRAP_TARGET = 0x54,
    SYSCALL_COPY_TO_ABI_TRAP_TARGET = 0x55,
    SYSCALL_START_ABI_TRAP_TARGET = 0x56,
    SYSCALL_SET_ABI_TRAP_TARGET_REQUEST_PAGE = 0x57,
    SYSCALL_CLONE_ABI_TRAP_REPLY_TARGET = 0x58,
    SYSCALL_DETACH_ABI_TRAP_REPLY_TOKEN = 0x59,
    SYSCALL_SHARE_ABI_TRAP_REPLY_TARGET_PAGES_TO_TARGET = 0x5A,
    SYSCALL_UNMAP_ABI_TRAP_TARGET_PAGES = 0x5B,
    SYSCALL_OK = 0,
    SYSCALL_ERR_MAP = 5,
    SYSCALL_ERR_ENDPOINT = 9,
    IPC_CALL_FLAG_SIGNAL_ONLY = 0x2,
    WAIT_EVENT_FLAG_IPC_SIGNAL_ONLY = 0x2,

    PAGE_BYTES = 4096,
    PAGE_RIGHT_CPU_READ = 0x1,
    PAGE_RIGHT_CPU_WRITE = 0x2,
    TRAP_MAGIC = 0x3149424150415254ULL,
    TRAP_VERSION = 1,

    SERVICE_REGISTRY_SHADOW_VA = 0x3C2C0000,
    SERVICE_REGISTRY_MAGIC = 0x53525643,
    SERVICE_REGISTRY_VERSION = 1,
    SERVICE_REGISTRY_MAX_ENTRIES = 12,
    SERVICE_KIND_VFS = 2,
    SERVICE_KIND_CONSOLE = 10,
    SERVICE_KIND_NET = 11,
    SERVICE_KIND_TTY = 12,

    VFS_REQUEST_VA = 0x2B000000,
    VFS_RESPONSE_VA = 0x2B001000,
    CONSOLE_REQUEST_VA = 0x2B010000,
    CONSOLE_RESPONSE_VA = 0x2B011000,
    NET_REQUEST_VA = 0x2B020000,
    NET_RESPONSE_VA = 0x2B021000,
    FS_REQUEST_MAGIC = 0x51534653,
    FS_RESPONSE_MAGIC = 0x52534653,
    FS_PROTOCOL_VERSION = 1,
    FS_MAX_PATH_BYTES = 128,
    FS_REQUEST_HEADER_BYTES = 72,
    FS_RESPONSE_HEADER_BYTES = 72,
    FS_RESPONSE_PAYLOAD_BYTES = PAGE_BYTES - FS_RESPONSE_HEADER_BYTES,
    FS_STAT_RECORD_BYTES = 56,
    FS_DIRENT_RECORD_BYTES = 24,
    FS_OP_CONNECT = 1,
    FS_OP_LOOKUP = 16,
    FS_OP_OPEN = 17,
    FS_OP_READ = 18,
    FS_OP_READDIR = 19,
    FS_OP_STAT = 20,
    FS_OP_CLOSE = 21,
    FS_OP_CREATE = 22,
    FS_OP_WRITE = 23,
    FS_OP_UNLINK = 24,
    FS_OP_RENAME = 25,
    FS_OP_OPEN_EXEC = 32,
    FS_STATUS_OK = 0,
    FS_STATUS_INVALID = 1,
    FS_STATUS_NOT_FOUND = 2,
    FS_STATUS_NOT_DIR = 3,
    FS_STATUS_NOT_SUPPORTED = 7,
    FS_STATUS_END_OF_DIR = 10,
    FS_OBJECT_NONE = 0,
    FS_OBJECT_MOUNT = 1,
    FS_OBJECT_DIRECTORY = 2,
    FS_OBJECT_FILE = 3,
    FS_OBJECT_OPEN_FILE = 4,
    FS_OBJECT_EXEC = 5,
    FS_DIR_MODE = 0x4000,
    FS_FILE_MODE = 0x8000,

    CONSOLE_REQUEST_MAGIC = 0x514E4F43,
    CONSOLE_RESPONSE_MAGIC = 0x524E4F43,
    CONSOLE_PROTOCOL_VERSION = 1,
    CONSOLE_OP_CONNECT = 1,
    CONSOLE_OP_READ = 2,
    CONSOLE_OP_WRITE = 3,
    CONSOLE_OP_GET_ATTR = 4,
    CONSOLE_OP_SET_ATTR = 5,
    CONSOLE_OP_GET_SIGNAL = 6,
    CONSOLE_STATUS_OK = 0,
    CONSOLE_STATUS_AGAIN = 1,
    CONSOLE_STATUS_INVALID = 2,
    CONSOLE_STATUS_IO_ERROR = 3,
    CONSOLE_STATUS_NOT_CONNECTED = 4,
    CONSOLE_STATUS_INTERRUPTED = 5,
    CONSOLE_REQUEST_HEADER_BYTES = 64,
    CONSOLE_RESPONSE_HEADER_BYTES = 64,
    CONSOLE_REQUEST_PAYLOAD_BYTES = PAGE_BYTES - CONSOLE_REQUEST_HEADER_BYTES,
    CONSOLE_RESPONSE_PAYLOAD_BYTES = PAGE_BYTES - CONSOLE_RESPONSE_HEADER_BYTES,
    NET_REQUEST_MAGIC = 0x514E4554,
    NET_RESPONSE_MAGIC = 0x524E4554,
    NET_PROTOCOL_VERSION = 1,
    NET_OP_CONNECT = 1,
    NET_OP_BIND = 3,
    NET_OP_SEND_TO = 4,
    NET_OP_RECV_FROM = 5,
    NET_OP_CLOSE = 6,
    NET_OP_POLL = 7,
    NET_OP_TCP_CONNECT = 8,
    NET_OP_TCP_WRITE = 9,
    NET_OP_TCP_READ = 10,
    NET_POLL_READABLE = 1 << 0,
    NET_POLL_WRITABLE = 1 << 2,
    NET_STATUS_OK = 0,
    NET_STATUS_INVALID = 2,
    NET_STATUS_NOT_CONNECTED = 4,
    NET_STATUS_NO_ROUTE = 5,
    NET_STATUS_PORT_IN_USE = 6,
    NET_STATUS_WOULD_BLOCK = 7,
    NET_STATUS_TOO_BIG = 8,
    NET_STATUS_BUSY = 9,
    NET_REQUEST_HEADER_BYTES = 56,
    NET_RESPONSE_HEADER_BYTES = 56,
    NET_REQUEST_PAYLOAD_BYTES = PAGE_BYTES - NET_REQUEST_HEADER_BYTES,
    NET_RESPONSE_PAYLOAD_BYTES = PAGE_BYTES - NET_RESPONSE_HEADER_BYTES,
    NET_UDP_MAX_PAYLOAD = 1200,
    NET_TCP_MAX_PAYLOAD = 1200,
    NET_TCP_READ_BYTES = NET_RESPONSE_PAYLOAD_BYTES,

    LINUX_SYS_READ = 0,
    LINUX_SYS_WRITE = 1,
    LINUX_SYS_OPEN = 2,
    LINUX_SYS_CLOSE = 3,
    LINUX_SYS_STAT = 4,
    LINUX_SYS_FSTAT = 5,
    LINUX_SYS_LSTAT = 6,
    LINUX_SYS_POLL = 7,
    LINUX_SYS_LSEEK = 8,
    LINUX_SYS_MMAP = 9,
    LINUX_SYS_MPROTECT = 10,
    LINUX_SYS_MUNMAP = 11,
    LINUX_SYS_BRK = 12,
    LINUX_SYS_RT_SIGACTION = 13,
    LINUX_SYS_RT_SIGPROCMASK = 14,
    LINUX_SYS_IOCTL = 16,
    LINUX_SYS_PREAD64 = 17,
    LINUX_SYS_READV = 19,
    LINUX_SYS_WRITEV = 20,
    LINUX_SYS_ACCESS = 21,
    LINUX_SYS_PIPE = 22,
    LINUX_SYS_SELECT = 23,
    LINUX_SYS_MADVISE = 28,
    LINUX_SYS_DUP = 32,
    LINUX_SYS_DUP2 = 33,
    LINUX_SYS_SETITIMER = 38,
    LINUX_SYS_GETPID = 39,
    LINUX_SYS_SOCKET = 41,
    LINUX_SYS_CONNECT = 42,
    LINUX_SYS_SENDTO = 44,
    LINUX_SYS_RECVFROM = 45,
    LINUX_SYS_SENDMSG = 46,
    LINUX_SYS_RECVMSG = 47,
    LINUX_SYS_SHUTDOWN = 48,
    LINUX_SYS_BIND = 49,
    LINUX_SYS_GETSOCKNAME = 51,
    LINUX_SYS_GETPEERNAME = 52,
    LINUX_SYS_SETSOCKOPT = 54,
    LINUX_SYS_GETSOCKOPT = 55,
    LINUX_SYS_CLONE = 56,
    LINUX_SYS_FORK = 57,
    LINUX_SYS_VFORK = 58,
    LINUX_SYS_WAIT4 = 61,
    LINUX_SYS_KILL = 62,
    LINUX_SYS_UNAME = 63,
    LINUX_SYS_FCNTL = 72,
    LINUX_SYS_FLOCK = 73,
    LINUX_SYS_GETCWD = 79,
    LINUX_SYS_CHDIR = 80,
    LINUX_SYS_RENAME = 82,
    LINUX_SYS_UNLINK = 87,
    LINUX_SYS_READLINK = 89,
    LINUX_SYS_CHMOD = 90,
    LINUX_SYS_FCHMOD = 91,
    LINUX_SYS_CHOWN = 92,
    LINUX_SYS_FCHOWN = 93,
    LINUX_SYS_LCHOWN = 94,
    LINUX_SYS_UMASK = 95,
    LINUX_SYS_GETTIMEOFDAY = 96,
    LINUX_SYS_GETUID = 102,
    LINUX_SYS_GETGID = 104,
    LINUX_SYS_GETEUID = 107,
    LINUX_SYS_GETEGID = 108,
    LINUX_SYS_SETPGID = 109,
    LINUX_SYS_GETPPID = 110,
    LINUX_SYS_GETPGID = 121,
    LINUX_SYS_SIGALTSTACK = 131,
    LINUX_SYS_STATFS = 137,
    LINUX_SYS_FSTATFS = 138,
    LINUX_SYS_EXECVE = 59,
    LINUX_SYS_EXIT = 60,
    LINUX_SYS_ARCH_PRCTL = 158,
    LINUX_SYS_MOUNT = 165,
    LINUX_SYS_UMOUNT2 = 166,
    LINUX_SYS_GETTID = 186,
    LINUX_SYS_TKILL = 200,
    LINUX_SYS_TIME = 201,
    LINUX_SYS_FUTEX = 202,
    LINUX_SYS_SCHED_GETAFFINITY = 204,
    LINUX_SYS_GETDENTS64 = 217,
    LINUX_SYS_SET_TID_ADDRESS = 218,
    LINUX_SYS_CLOCK_GETTIME = 228,
    LINUX_SYS_EXIT_GROUP = 231,
    LINUX_SYS_TGKILL = 234,
    LINUX_SYS_OPENAT = 257,
    LINUX_SYS_NEWFSTATAT = 262,
    LINUX_SYS_UNLINKAT = 263,
    LINUX_SYS_RENAMEAT = 264,
    LINUX_SYS_PSELECT6 = 270,
    LINUX_SYS_PPOLL = 271,
    LINUX_SYS_SET_ROBUST_LIST = 273,
    LINUX_SYS_SPLICE = 275,
    LINUX_SYS_UTIMENSAT = 280,
    LINUX_SYS_EVENTFD2 = 290,
    LINUX_SYS_DUP3 = 292,
    LINUX_SYS_PIPE2 = 293,
    LINUX_SYS_PRLIMIT64 = 302,
    LINUX_SYS_GETRANDOM = 318,
    LINUX_SYS_RENAMEAT2 = 316,
    LINUX_SYS_MEMBARRIER = 324,
    LINUX_SYS_RSEQ = 334,

    AT_FDCWD_U64 = 0xffffffffffffff9cULL,
    AT_EMPTY_PATH = 0x1000,
    O_ACCMODE = 00000003,
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_RDWR = 2,
    O_NONBLOCK = 00004000,
    O_CREAT = 00000100,
    O_TRUNC = 00001000,
    O_CLOEXEC = 02000000,
    O_DIRECTORY = 00200000,
    AF_INET = 2,
    SOCK_STREAM = 1,
    SOCK_DGRAM = 2,
    SOCK_TYPE_MASK = 0xf,
    SOCK_NONBLOCK = O_NONBLOCK,
    SOCK_CLOEXEC = O_CLOEXEC,
    MSG_DONTWAIT = 0x40,
    IPPROTO_TCP = 6,
    IPPROTO_UDP = 17,
    SOL_SOCKET = 1,
    SO_TYPE = 3,
    SO_ERROR = 4,
    SO_KEEPALIVE = 9,
    SO_RCVTIMEO = 20,
    SO_SNDTIMEO = 21,
    SOL_TCP = 6,
    TCP_NODELAY = 1,
    POLLIN = 0x001,
    POLLOUT = 0x004,
    POLLERR = 0x008,
    POLLHUP = 0x010,
    POLLNVAL = 0x020,
    POLLRDNORM = 0x040,
    POLLWRNORM = 0x100,
    FS_CREATE_FLAG_TRUNCATE = 1 << 1,
    WNOHANG = 1,
    WUNTRACED = 2,
    WCONTINUED = 8,
    SIGHUP = 1,
    SIGINT = 2,
    SIGQUIT = 3,
    SIGILL = 4,
    SIGTRAP = 5,
    SIGABRT = 6,
    SIGKILL = 9,
    SIGSEGV = 11,
    SIGPIPE = 13,
    SIGALRM = 14,
    SIGTERM = 15,
    SIGCHLD = 17,
    SIGCONT = 18,
    SIGSTOP = 19,
    SIGTSTP = 20,
    SIGTTIN = 21,
    SIGTTOU = 22,
    SIGURG = 23,
    SIGWINCH = 28,
    SS_ONSTACK = 1,
    SS_DISABLE = 2,
    SS_AUTODISARM = 0x80000000,
    MINSIGSTKSZ = 2048,
    CLONE_VM = 0x00000100,
    CLONE_FS = 0x00000200,
    CLONE_FILES = 0x00000400,
    CLONE_SIGHAND = 0x00000800,
    CLONE_THREAD = 0x00010000,
    CLONE_SYSVSEM = 0x00040000,
    CLONE_SETTLS = 0x00080000,
    CLONE_PARENT_SETTID = 0x00100000,
    CLONE_CHILD_CLEARTID = 0x00200000,
    CLONE_DETACHED = 0x00400000,
    CLONE_CHILD_SETTID = 0x01000000,
    F_DUPFD = 0,
    F_GETFD = 1,
    F_SETFD = 2,
    F_GETFL = 3,
    F_SETFL = 4,
    F_GETLK = 5,
    F_SETLK = 6,
    F_SETLKW = 7,
    F_RDLCK = 0,
    F_WRLCK = 1,
    F_UNLCK = 2,
    F_DUPFD_CLOEXEC = 1030,
    LOCK_SH = 1,
    LOCK_EX = 2,
    LOCK_NB = 4,
    LOCK_UN = 8,
    SEEK_SET = 0,
    SEEK_CUR = 1,
    SEEK_END = 2,
    DT_UNKNOWN = 0,
    DT_DIR = 4,
    DT_REG = 8,
    FUTEX_WAIT = 0,
    FUTEX_WAKE = 1,
    FUTEX_PRIVATE_FLAG = 128,
    FUTEX_CLOCK_REALTIME = 256,
    FUTEX_CMD_MASK = 0x7f,
    MEMBARRIER_CMD_QUERY = 0,
    MEMBARRIER_CMD_GLOBAL = 1 << 0,
    MEMBARRIER_CMD_PRIVATE_EXPEDITED = 1 << 3,
    MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED = 1 << 4,

    TRAP_RESPONSE_FLAG_EXIT = 1,
    ARCH_SET_FS = 0x1002,
    TCGETS = 0x5401,
    TCSETS = 0x5402,
    TCSETSW = 0x5403,
    TCSETSF = 0x5404,
    TIOCGPGRP = 0x540F,
    TIOCSPGRP = 0x5410,
    TIOCGWINSZ = 0x5413,
    TIOCSWINSZ = 0x5414,

    SPAWN_RESULT_TAG = 1ULL << 63,
    SPAWN_RESULT_PROCESS_MASK = 0xffffffffULL,
    SPAWN_FLAG_BOOTSTRAP_EXTENDED_DESCRIPTOR_TABLE = 1 << 2,
    SPAWN_FLAG_CHILD_BOOTSTRAP_OWNER = 1 << 3,
    BOOTSTRAP_CAP_KIND_VM_OBJECT = 2,
    VM_OBJECT_TOKEN_TAG = 1ULL << 62,
    EXEC_IMAGE_TOKEN_TAG = (1ULL << 62) | (1ULL << 61),
    VM_RIGHT_READ_MAP = 0x5,
    VM_RIGHT_READ_MAP_GRANT = 0xD,
    EXEC_RIGHT_EXEC_GRANT = 0x3,
    EXEC_LOADER_BOOTSTRAP_MAGIC = 0x5845434C44523031ULL,
    EXEC_LOADER_BOOTSTRAP_VERSION = 2,
    EXEC_LOADER_BOOTSTRAP_FLAG_SERVICE_MODE = 1ULL << 0,
    EXEC_LOADER_SERVICE_ENDPOINT_ID = 0x93,
    EXEC_LOADER_SERVICE_REQUEST_MAGIC = 0x5845434C44535651ULL,
    EXEC_LOADER_SERVICE_RESPONSE_MAGIC = 0x5845434C44535652ULL,
    EXEC_LOADER_SERVICE_VERSION = 1,
    EXEC_LOADER_SERVICE_OP_LAUNCH = 1,
    EXEC_LOADER_SERVICE_STATUS_OK = 0,
    LINUX_ABI_BOOTSTRAP_MAGIC = 0x4C41424943464731ULL,
    LINUX_ABI_BOOTSTRAP_VERSION = 2,
    LINUX_ABI_BOOTSTRAP_READY = 0x4C414249524459ULL,
    EXEC_LOADER_CONFIG_TARGET_VA = 0x3C002000,
    LINUX_ABI_CONFIG_TARGET_VA = 0x3C002000,
    LINUX_ABI_ENDPOINT_ID = 0x90,
    LINUX_ABI_SELF_WAKE_ENDPOINT_ID = 0x91,
    EXECVE_MAIN_IMAGE_VA = 0x24000000,
    EXECVE_LD_IMAGE_VA = 0x26200000,
    EXECVE_CONFIG_VA = 0x26400000,
    EXECVE_TABLE_VA = 0x26401000,
    EXECVE_LOADER_REQUEST_VA = 0x26402000,
    EXECVE_LOADER_RESPONSE_VA = 0x26403000,
    EXECVE_LOADER_SERVICE_CONFIG_VA = 0x26404000,
    EXECVE_LOADER_SERVICE_TABLE_VA = 0x26405000,
    LINUX_ABI_REQUEST_PAGES_VA = 0x26500000,
    LINUX_ABI_REQUEST_PAGE_COUNT = 64,
    FILE_CACHE_BASE_VA = 0x28000000,
    FILE_CACHE_BYTES = 8 * 1024 * 1024,
    FILE_CACHE_MAX = 12,
    EXECVE_MAX_IMAGE_BYTES = 16 * 1024 * 1024,
    EXECVE_MAX_LD_BYTES = 768 * 1024,
    EXECVE_MAX_ARGV = 8,
    EXECVE_MAX_ENVP = 16,
    EXECVE_MAX_ARG_DATA_BYTES = 2048,
};

struct ipc_message { u64 status; u64 request_va; u64 reserved0; u64 reserved1; u64 reserved2; };
enum abi_handler_result_kind {
    ABI_HANDLER_REPLY_NOW = 1,
    ABI_HANDLER_PENDING = 2,
    ABI_HANDLER_EXIT_TARGET = 3,
    ABI_HANDLER_LEGACY_NEXT_MESSAGE = 4,
    ABI_HANDLER_WAIT_NEXT = 5,
    ABI_HANDLER_EXIT_CURRENT = 6,
};
struct abi_handler_result {
    enum abi_handler_result_kind kind;
    u64 result;
    u64 flags;
    u64 principal;
    struct ipc_message next_message;
};
struct trap_request {
    u64 magic; unsigned version; unsigned kind; unsigned flavor; unsigned reserved0;
    u64 caller_principal; u64 thread_id; u64 rip; u64 rsp; u64 fault_addr; u64 error_code; u64 nr; u64 args[6];
};
struct service_entry { u64 kind; u64 process_slot; u64 endpoint_id; u64 flags; };
struct service_registry_page { u64 magic; u64 version; u64 entry_count; u64 reserved0; struct service_entry entries[SERVICE_REGISTRY_MAX_ENTRIES]; };
struct fs_request_header {
    u32 magic; u16 version; u16 op; u64 request_seq; u64 object_token; u64 offset; u32 length; u32 flags;
    u16 path_bytes; u16 inline_bytes; u32 reserved0; u64 arg0; u64 arg1; u64 session_nonce;
};
struct fs_response_header {
    u32 magic; u16 version; u16 op; u64 response_seq; i32 status; u32 result_flags; u64 result_token;
    u64 file_bytes; u64 cursor_next; u16 inline_bytes; u8 object_kind; u8 reserved0; u32 reserved1; u64 arg0; u64 arg1;
};
struct console_request_header {
    u32 magic; u16 version; u16 op; u64 request_seq; u64 session_nonce; u32 length; u32 flags;
    u64 arg0; u64 arg1; u64 arg2; u64 reserved0;
};
struct console_response_header {
    u32 magic; u16 version; u16 op; u64 response_seq; i32 status; u32 result_flags;
    u32 inline_bytes; u32 reserved0; u64 arg0; u64 arg1; u64 reserved1; u64 reserved2;
};
struct net_request_header {
    u32 magic; u16 version; u16 op; u64 request_seq; u64 session_nonce;
    u64 arg0; u64 arg1; u64 arg2; u64 reserved0;
};
struct net_response_header {
    u32 magic; u16 version; u16 op; u64 response_seq; i32 status; u32 inline_bytes;
    u64 arg0; u64 arg1; u64 arg2; u64 reserved0;
};
struct fs_stat_record { u8 object_kind; u8 reserved0[7]; u64 size_bytes; u32 mode_bits; u32 reserved1; u64 mtime_unix_sec; u64 reserved2[2]; };
struct fs_dirent_record { u64 next_cursor; u8 object_kind; u8 reserved0[7]; u16 name_bytes; u16 reserved1; u32 reserved2; };
struct linux_stat {
    u64 st_dev; u64 st_ino; u64 st_nlink; u32 st_mode; u32 st_uid; u32 st_gid; u32 __pad0;
    u64 st_rdev; i64 st_size; i64 st_blksize; i64 st_blocks;
    i64 st_atime; u64 st_atime_nsec; i64 st_mtime; u64 st_mtime_nsec; i64 st_ctime; u64 st_ctime_nsec;
    i64 __unused[3];
};
struct linux_statfs {
    i64 f_type;
    i64 f_bsize;
    u64 f_blocks;
    u64 f_bfree;
    u64 f_bavail;
    u64 f_files;
    u64 f_ffree;
    i32 f_fsid[2];
    i64 f_namelen;
    i64 f_frsize;
    i64 f_flags;
    i64 f_spare[4];
};

enum fd_kind { FD_UNUSED = 0, FD_STDIO = 1, FD_FILE = 2, FD_DIR = 3, FD_PIPE_READ = 4, FD_PIPE_WRITE = 5, FD_TTY = 6, FD_SOCKET = 7, FD_RANDOM = 8 };
struct fd_entry {
    enum fd_kind kind;
    u64 token;
    u64 offset;
    u64 size;
    u32 mode_bits;
    u32 fd_flags;
    u8 object_kind;
    u8 pipe_id;
    u8 socket_connected;
    u8 socket_type;
    u8 socket_connecting;
    u8 socket_reserved0;
    u16 path_len;
    u16 socket_local_port;
    u16 socket_remote_port;
    u32 socket_local_ip;
    u32 socket_remote_ip;
    char path[FS_MAX_PATH_BYTES + 1];
};
struct vfs_client { int active; u64 endpoint_id; u64 process_slot; u64 request_paddr; u64 response_paddr; u64 root_token; u64 next_seq; u64 session_nonce; };
struct console_client { int active; int is_tty; u64 endpoint_id; u64 process_slot; u64 request_paddr; u64 response_paddr; u64 next_seq; u64 session_nonce; };
struct net_client_state { int active; u64 endpoint_id; u64 process_slot; u64 request_paddr; u64 response_paddr; u64 next_seq; u64 session_nonce; };
struct path_cache_entry {
    u8 used;
    u8 kind;
    u16 path_len;
    u64 token;
    u64 size;
    struct fs_stat_record stat;
    char path[FS_MAX_PATH_BYTES + 1];
};
struct file_cache_entry {
    u8 used;
    u8 kind;
    u16 path_len;
    u64 token;
    u64 size;
    u64 buffer_va;
    struct fs_stat_record stat;
    char path[FS_MAX_PATH_BYTES + 1];
};

enum { PIPE_MAX = 8, PIPE_BUFFER_BYTES = 4096 };
struct pipe_entry {
    u8 used;
    u8 pending_read;
    u16 read_refs;
    u16 write_refs;
    u64 head;
    u64 len;
    u64 pending_principal;
    u64 pending_dst;
    u64 pending_len;
    u8 bytes[PIPE_BUFFER_BYTES];
};

enum { FUTEX_WAITER_MAX = 32 };
struct futex_waiter {
    u8 used;
    u64 principal;
    u64 owner_pid;
    u64 uaddr;
};

enum { EXEC_CACHE_MAX = 16 };
struct exec_cache_entry {
    u8 used;
    u8 loader_service_cached;
    u16 path_len;
    u64 vm_token;
    u64 file_bytes;
    char path[FS_MAX_PATH_BYTES + 1];
};

enum { VM_REGION_MAX = 1024 };
struct vm_region { u64 start; u64 size; u64 prot; int used; };

enum { LINUX_PROCESS_MAX = 16, LINUX_CHILD_MAX = 16 };
struct linux_process_state {
    u8 used;
    u8 exec_pending;
    u32 exit_status;
    u64 exec_pending_principal;
    u64 pid;
    u64 tid;
    u64 pgid;
    u64 principal;
    struct fd_entry fds[32];
    u64 mmap_next_va;
    u64 brk_next_va;
    struct vm_region regions[VM_REGION_MAX];
    u16 cwd_len;
    char cwd[FS_MAX_PATH_BYTES + 1];
    u8 child_used[LINUX_CHILD_MAX];
    u64 child_slot[LINUX_CHILD_MAX];
    u8 wait_pending;
    i64 wait_pid;
    u64 wait_status_va;
    u64 clear_child_tid;
    u8 profile_enabled;
    u64 sigaltstack_sp;
    u64 sigaltstack_size;
    u32 sigaltstack_flags;
    u64 sig_handler[65];
    u64 sig_flags[65];
};

struct linux_stack_t {
    u64 ss_sp;
    u32 ss_flags;
    u32 reserved0;
    u64 ss_size;
};

struct exec_loader_config {
    u64 magic; u64 version; u64 executable_vm_token; u64 executable_file_bytes; u64 flags;
    u64 interpreter_vm_token; u64 interpreter_file_bytes; u64 bootfs_vm_token; u64 bootfs_file_bytes;
    u64 fs_endpoint_id; u64 fs_compat_process_slot; u64 abi_trap_endpoint_id; u64 abi_trap_endpoint_process_slot;
    u64 abi_trap_flavor; u64 abi_trap_request_page_va;
    u16 execfn_offset; u16 execfn_bytes; u16 argv_count; u16 envp_count; u16 arg_data_bytes; u16 reserved_arg0;
    u16 argv_offsets[EXECVE_MAX_ARGV]; u16 argv_bytes[EXECVE_MAX_ARGV];
    u16 envp_offsets[EXECVE_MAX_ENVP]; u16 envp_bytes[EXECVE_MAX_ENVP];
    u8 arg_data[EXECVE_MAX_ARG_DATA_BYTES];
};
struct exec_loader_service_request {
    u64 magic;
    u64 version;
    u64 op;
    u64 seq;
    u64 response_paddr;
    struct exec_loader_config config;
};
struct exec_loader_service_response {
    u64 magic;
    u64 version;
    u64 op;
    u64 seq;
    u64 status;
    u64 child_process_slot;
};

struct bootstrap_page_descriptor { u64 source_va; u64 target_va; u64 flags; };
struct bootstrap_cap_descriptor { u64 source_token; u64 target_token_va; u64 rights_bits; u8 kind; u8 reserved[7]; };
struct bootstrap_descriptor_table {
    u16 page_count; u16 cap_count; u32 reserved0;
    struct bootstrap_page_descriptor page_descriptors[136];
    struct bootstrap_cap_descriptor cap_descriptors[8];
};
struct linux_abi_bootstrap_config {
    u64 magic;
    u64 version;
    u64 exec_loader_vm_token;
    u64 standard_interpreter_vm_token;
    u64 standard_interpreter_file_bytes;
    u64 abi_trap_request_page_va;
    u64 status;
    u16 exec_path_bytes;
    u8 reserved0[6];
    char exec_path[128];
};

enum { LINUX_SYSCALL_PROFILE_COUNT = 335, FS_PROFILE_OP_COUNT = 33, NET_PROFILE_OP_COUNT = 11 };
struct linux_abi_profile {
    u64 syscall_total;
    u64 syscall_counts[LINUX_SYSCALL_PROFILE_COUNT + 1];
    u64 syscall_ticks[LINUX_SYSCALL_PROFILE_COUNT + 1];
    u64 syscall_max_ticks[LINUX_SYSCALL_PROFILE_COUNT + 1];
    u64 vfs_requests;
    u64 vfs_op_counts[FS_PROFILE_OP_COUNT];
    u64 vfs_read_request_bytes;
    u64 vfs_write_request_bytes;
    u64 vfs_inline_write_bytes;
    u64 vfs_wait_calls;
    u64 vfs_wait_loops;
    u64 vfs_wait_timeouts;
    u64 vfs_wait_slow;
    u64 fs_read_bytes;
    u64 fs_read_cmd_bytes;
    u64 fs_read_lib_bytes;
    u64 fs_read_tmp_bytes;
    u64 fs_read_proc_bytes;
    u64 fs_write_bytes;
    u64 mmap_calls;
    u64 mmap_pages;
    u64 mmap_file_calls;
    u64 mmap_file_pages;
    u64 mmap_file_bytes;
    u64 mprotect_calls;
    u64 mprotect_pages;
    u64 brk_calls;
    u64 net_requests;
    u64 net_op_counts[NET_PROFILE_OP_COUNT];
    u64 net_payload_tx_bytes;
    u64 net_payload_rx_bytes;
    u64 net_wait_calls;
    u64 net_wait_loops;
    u64 net_wait_timeouts;
    u64 net_wait_slow;
    u64 net_tcp_connect_attempts;
    u64 net_tcp_connect_poll_loops;
    u64 poll_calls;
    u64 poll_wait_loops;
    u64 select_calls;
    u64 select_wait_loops;
    u64 getrandom_calls;
    u64 getrandom_bytes;
    u64 file_cache_hits;
    u64 file_cache_misses;
    u64 file_cache_fill_bytes;
    u64 path_cache_hits;
    u64 path_cache_misses;
    u64 open_cache_hits;
    u64 open_cache_misses;
};

static u64 trap_request_page_va = 0;
static struct vfs_client g_vfs;
static struct console_client g_console;
static struct net_client_state g_net;
static struct linux_abi_profile g_prof;
static struct linux_process_state g_processes[LINUX_PROCESS_MAX];
static struct path_cache_entry g_path_cache[FILE_CACHE_MAX];
static struct file_cache_entry g_file_cache[FILE_CACHE_MAX];
static u64 g_file_cache_next_offset = 0;
static u8 g_exit_record_used[LINUX_PROCESS_MAX];
static u64 g_exit_record_pid[LINUX_PROCESS_MAX];
static u32 g_exit_record_status[LINUX_PROCESS_MAX];
static u8 g_deferred_start_used[LINUX_PROCESS_MAX];
static u64 g_deferred_start_principal[LINUX_PROCESS_MAX];
static u32 g_deferred_pipe_wake_mask = 0;
static struct pipe_entry g_pipes[PIPE_MAX];
static struct futex_waiter g_futex_waiters[FUTEX_WAITER_MAX];
static u8 g_request_page_mapped[LINUX_ABI_REQUEST_PAGE_COUNT];
static int execve_scratch_ready = 0;
static int execve_loader_service_scratch_ready = 0;
static u64 execve_main_scratch_pages = 0;
static u64 g_exec_loader_vm_token = 0;
static u64 g_exec_loader_exec_token = 0;
static u64 g_exec_loader_service_slot = 0;
static u64 g_exec_loader_service_request_paddr = 0;
static u64 g_exec_loader_service_response_paddr = 0;
static int g_exec_loader_service_connected = 0;
static u64 g_exec_loader_service_seq = 1;
static u64 g_standard_interpreter_vm_token = 0;
static u64 g_standard_interpreter_bytes = 0;
static struct exec_cache_entry g_exec_cache[EXEC_CACHE_MAX];
static u64 g_root_linux_principal = 0;
static int g_root_linux_principal_set = 0;
static char g_exec_path[FS_MAX_PATH_BYTES + 1];
static u16 g_exec_path_len = 0;

static void deliver_tty_signal(u64 signo);
static void remove_futex_waiters_for_principal(u64 principal);
static u64 wake_futex_waiters(u64 owner_pid, u64 uaddr, u64 max_wake);

struct linux_abi_context {
    struct linux_process_state *proc;
    const struct trap_request *request;
    u64 reply_target_principal;
};

static struct linux_abi_context *g_abi_ctx = 0;

static struct linux_process_state *abi_current_proc(void) {
    return g_abi_ctx != 0 ? g_abi_ctx->proc : 0;
}

static u64 abi_reply_target_principal(void) {
    return g_abi_ctx != 0 ? g_abi_ctx->reply_target_principal : 0;
}

static void abi_set_reply_target_principal(u64 principal) {
    if (g_abi_ctx != 0) g_abi_ctx->reply_target_principal = principal;
}

#define g_proc (abi_current_proc())
#define g_fds (g_proc->fds)
#define g_mmap_next_va (g_proc->mmap_next_va)
#define g_brk_next_va (g_proc->brk_next_va)
#define g_regions (g_proc->regions)
#define g_cwd (g_proc->cwd)
#define g_cwd_len (g_proc->cwd_len)
