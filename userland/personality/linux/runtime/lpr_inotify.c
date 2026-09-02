#include "lpr_filed_internal.h"

#define LPR_INOTIFY_INSTANCE_BYTES (64ull * 1024ull)
#define LPR_INOTIFY_INSTANCE_MAGIC 0x31594649544f4e49ull
#define LPR_INOTIFY_POLL_INTERVAL_NS (100ull * 1000ull * 1000ull)
#define LPR_INOTIFY_MAX_WATCHES 32u
#define LPR_INOTIFY_MAX_EVENTS 256u

#define LPR_IN_ACCESS       0x00000001u
#define LPR_IN_MODIFY       0x00000002u
#define LPR_IN_ATTRIB       0x00000004u
#define LPR_IN_CLOSE_WRITE  0x00000008u
#define LPR_IN_CLOSE_NOWRITE 0x00000010u
#define LPR_IN_OPEN         0x00000020u
#define LPR_IN_MOVED_FROM   0x00000040u
#define LPR_IN_MOVED_TO     0x00000080u
#define LPR_IN_CREATE       0x00000100u
#define LPR_IN_DELETE       0x00000200u
#define LPR_IN_DELETE_SELF  0x00000400u
#define LPR_IN_MOVE_SELF    0x00000800u
#define LPR_IN_UNMOUNT      0x00002000u
#define LPR_IN_Q_OVERFLOW   0x00004000u
#define LPR_IN_IGNORED      0x00008000u
#define LPR_IN_ONLYDIR      0x01000000u
#define LPR_IN_DONT_FOLLOW  0x02000000u
#define LPR_IN_EXCL_UNLINK  0x04000000u
#define LPR_IN_MASK_CREATE  0x10000000u
#define LPR_IN_MASK_ADD     0x20000000u
#define LPR_IN_ISDIR        0x40000000u
#define LPR_IN_ONESHOT      0x80000000u

#define LPR_IN_ALL_EVENTS \
    (LPR_IN_ACCESS | LPR_IN_MODIFY | LPR_IN_ATTRIB | LPR_IN_CLOSE_WRITE | \
     LPR_IN_CLOSE_NOWRITE | LPR_IN_OPEN | LPR_IN_MOVED_FROM | \
     LPR_IN_MOVED_TO | LPR_IN_CREATE | LPR_IN_DELETE | \
     LPR_IN_DELETE_SELF | LPR_IN_MOVE_SELF)
#define LPR_IN_WATCH_FLAGS \
    (LPR_IN_ONLYDIR | LPR_IN_DONT_FOLLOW | LPR_IN_EXCL_UNLINK | \
     LPR_IN_MASK_CREATE | LPR_IN_MASK_ADD | LPR_IN_ONESHOT)

typedef struct lpr_linux_inotify_event {
    int32_t wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
} lpr_linux_inotify_event_t;

typedef struct lpr_inotify_watch {
    uint32_t active;
    int32_t wd;
    uint32_t mask;
    uint32_t is_directory;
    uint64_t dev;
    uint64_t ino;
    uint64_t nlink;
    int64_t size;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t reserved0;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    int64_t ctime_sec;
    int64_t ctime_nsec;
    char path[FILED_PATH_BYTES];
} lpr_inotify_watch_t;

typedef struct lpr_inotify_instance {
    uint64_t magic;
    volatile uint32_t lock_word;
    uint32_t next_wd;
    uint32_t event_head;
    uint32_t event_count;
    uint32_t overflow_queued;
    uint32_t reserved0;
    lpr_inotify_watch_t watches[LPR_INOTIFY_MAX_WATCHES];
    lpr_linux_inotify_event_t events[LPR_INOTIFY_MAX_EVENTS];
} lpr_inotify_instance_t;

_Static_assert(
    sizeof(lpr_inotify_instance_t) <= LPR_INOTIFY_INSTANCE_BYTES,
    "inotify instance mapping size");

static lpr_inotify_instance_t *lpr_inotify_instance(uint64_t fd)
{
    lpr_event_backend_t *event = lpr_event_backend(fd);
    if (event == 0 || !event->active ||
        event->subtype != LPR_EVENT_BACKEND_INOTIFY ||
        event->counter < 4096u ||
        event->deadline_ns != LPR_INOTIFY_INSTANCE_BYTES)
    {
        return 0;
    }
    lpr_inotify_instance_t *instance =
        (lpr_inotify_instance_t *)(uintptr_t)event->counter;
    return instance->magic == LPR_INOTIFY_INSTANCE_MAGIC ? instance : 0;
}

int lpr_linux_inotify_active(uint64_t fd)
{
    return lpr_inotify_instance(fd) != 0;
}

static void lpr_inotify_snapshot(
    lpr_inotify_watch_t *watch, const lpr_linux_stat_t *stat)
{
    watch->dev = stat->st_dev;
    watch->ino = stat->st_ino;
    watch->nlink = stat->st_nlink;
    watch->size = stat->st_size;
    watch->mode = stat->st_mode;
    watch->uid = stat->st_uid;
    watch->gid = stat->st_gid;
    watch->mtime_sec = stat->st_mtime_sec;
    watch->mtime_nsec = stat->st_mtime_nsec;
    watch->ctime_sec = stat->st_ctime_sec;
    watch->ctime_nsec = stat->st_ctime_nsec;
    watch->is_directory =
        (stat->st_mode & LPR_LINUX_S_IFMT) == LPR_LINUX_S_IFDIR;
}

static int64_t lpr_inotify_stat(
    const lpr_inotify_watch_t *watch, lpr_linux_stat_t *stat)
{
    const uint64_t flags =
        (watch->mask & LPR_IN_DONT_FOLLOW) != 0 ?
        LPR_LINUX_AT_SYMLINK_NOFOLLOW : 0;
    return lpr_linux_newfstatat(
        (uint64_t)(int64_t)LPR_LINUX_AT_FDCWD,
        (uint64_t)(uintptr_t)watch->path,
        (uint64_t)(uintptr_t)stat,
        flags);
}

static void lpr_inotify_queue(
    lpr_inotify_instance_t *instance, int32_t wd, uint32_t mask)
{
    if (mask == 0) return;
    if (instance->event_count >= LPR_INOTIFY_MAX_EVENTS) {
        if (instance->overflow_queued) return;
        instance->event_head = 0;
        instance->event_count = 1;
        instance->overflow_queued = 1;
        instance->events[0] = (lpr_linux_inotify_event_t){
            .wd = -1,
            .mask = LPR_IN_Q_OVERFLOW,
        };
        return;
    }
    const uint32_t slot =
        (instance->event_head + instance->event_count) %
        LPR_INOTIFY_MAX_EVENTS;
    instance->events[slot] = (lpr_linux_inotify_event_t){
        .wd = wd,
        .mask = mask,
    };
    instance->event_count++;
}

static void lpr_inotify_drain_timer(lpr_event_backend_t *event)
{
    if (event == 0 || event->wait_fd.raw < 16) return;
    uint64_t expirations = 0;
    (void)lpr_pacha_syscall3(
        PACHAOS_SYSCALL_FD_READ,
        (uint64_t)(uint32_t)event->wait_fd.raw,
        (uint64_t)(uintptr_t)&expirations,
        sizeof(expirations));
}

static void lpr_inotify_scan_locked(lpr_inotify_instance_t *instance)
{
    for (uint32_t index = 0; index < LPR_INOTIFY_MAX_WATCHES; index++) {
        lpr_inotify_watch_t *watch = &instance->watches[index];
        if (!watch->active) continue;
        lpr_linux_stat_t current;
        lpr_memset(&current, 0, sizeof(current));
        const int64_t status = lpr_inotify_stat(watch, &current);
        if (status == -LPR_LINUX_ENOENT) {
            lpr_inotify_queue(
                instance,
                watch->wd,
                (watch->mask & LPR_IN_DELETE_SELF) | LPR_IN_IGNORED);
            watch->active = 0;
            continue;
        }
        if (status != 0) continue;

        uint32_t changed = 0;
        if (current.st_dev != watch->dev || current.st_ino != watch->ino) {
            changed |= LPR_IN_MOVE_SELF;
        }
        if (current.st_mode != watch->mode || current.st_uid != watch->uid ||
            current.st_gid != watch->gid || current.st_nlink != watch->nlink ||
            current.st_ctime_sec != watch->ctime_sec ||
            current.st_ctime_nsec != watch->ctime_nsec)
        {
            changed |= LPR_IN_ATTRIB;
        }
        if (current.st_size != watch->size ||
            current.st_mtime_sec != watch->mtime_sec ||
            current.st_mtime_nsec != watch->mtime_nsec)
        {
            changed |= LPR_IN_MODIFY | LPR_IN_CLOSE_WRITE;
            if (watch->is_directory) {
                /* Directory mtime changes reflect entry creation, removal, or
                 * rename.  With no kernel fsnotify source, wake directory
                 * consumers so they can rescan the authoritative directory. */
                changed |= LPR_IN_CREATE | LPR_IN_DELETE |
                    LPR_IN_MOVED_FROM | LPR_IN_MOVED_TO;
            }
        }
        lpr_inotify_snapshot(watch, &current);
        changed &= watch->mask;
        if (changed == 0) continue;
        if (watch->is_directory) changed |= LPR_IN_ISDIR;
        lpr_inotify_queue(instance, watch->wd, changed);
        if ((watch->mask & LPR_IN_ONESHOT) != 0) {
            lpr_inotify_queue(instance, watch->wd, LPR_IN_IGNORED);
            watch->active = 0;
        }
    }
}

static void lpr_inotify_scan(uint64_t fd)
{
    lpr_inotify_instance_t *instance = lpr_inotify_instance(fd);
    lpr_event_backend_t *event = lpr_event_backend(fd);
    if (instance == 0 || event == 0) return;
    lpr_inotify_drain_timer(event);
    lpr_state_lock(&instance->lock_word);
    lpr_inotify_scan_locked(instance);
    lpr_state_unlock(&instance->lock_word);
}

int64_t lpr_linux_inotify_init1(uint64_t flags)
{
    if ((flags & ~(LPR_LINUX_O_CLOEXEC | LPR_LINUX_O_NONBLOCK)) != 0) {
        return -LPR_LINUX_EINVAL;
    }
    const int fd = lpr_fd_slot_alloc();
    if (fd < 0) return fd;
    const int64_t mapped = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_MMAP,
        0,
        0,
        LPR_INOTIFY_INSTANCE_BYTES,
        PACHAOS_PROT_READ | PACHAOS_PROT_WRITE,
        PACHAOS_MMAP_PRIVATE | PACHAOS_MMAP_ANONYMOUS,
        0);
    if (mapped < 4096) return lpr_pacha_status_to_errno(mapped);
    lpr_inotify_instance_t *instance =
        (lpr_inotify_instance_t *)(uintptr_t)mapped;
    lpr_memset(instance, 0, LPR_INOTIFY_INSTANCE_BYTES);
    instance->magic = LPR_INOTIFY_INSTANCE_MAGIC;
    instance->next_wd = 1;

    const uint64_t rights =
        PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_DUP;
    const int64_t timer_fd = lpr_pacha_syscall6(
        PACHAOS_SYSCALL_TIMERFD_CREATE,
        PACHAOS_CLOCK_MONOTONIC,
        0,
        LPR_INOTIFY_POLL_INTERVAL_NS,
        LPR_INOTIFY_POLL_INTERVAL_NS,
        rights,
        PACHA_FD_FLAG_NONBLOCK);
    if (timer_fd < 16) {
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)instance,
            LPR_INOTIFY_INSTANCE_BYTES);
        return lpr_pacha_status_to_errno(timer_fd);
    }
    const int install_status = lpr_control_install_fd(
        (uint64_t)(uint32_t)fd,
        LPR_FD_OPS_EVENT,
        flags,
        0,
        (uint64_t)(uintptr_t)instance);
    if (install_status != 0) {
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)timer_fd);
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)instance,
            LPR_INOTIFY_INSTANCE_BYTES);
        return install_status;
    }
    lpr_event_backend_t *event =
        lpr_event_backend((uint64_t)(uint32_t)fd);
    if (event == 0) {
        lpr_control_close_fd((uint64_t)(uint32_t)fd);
        (void)lpr_close_native_fd_if_open((uint64_t)(uint32_t)timer_fd);
        (void)lpr_pacha_syscall2(
            PACHAOS_SYSCALL_MUNMAP,
            (uint64_t)(uintptr_t)instance,
            LPR_INOTIFY_INSTANCE_BYTES);
        return -LPR_LINUX_EIO;
    }
    event->active = 1;
    event->subtype = LPR_EVENT_BACKEND_INOTIFY;
    event->flags = (uint32_t)flags;
    event->counter = (uint64_t)(uintptr_t)instance;
    event->deadline_ns = LPR_INOTIFY_INSTANCE_BYTES;
    event->wait_fd.raw = (int32_t)timer_fd;
    event->notify_fd.raw = -1;
    return fd;
}

int64_t lpr_linux_inotify_add_watch(
    uint64_t fd, uint64_t path_raw, uint64_t mask_raw)
{
    lpr_inotify_instance_t *instance = lpr_inotify_instance(fd);
    if (instance == 0) return -LPR_LINUX_EBADF;
    if (path_raw == 0) return -LPR_LINUX_EFAULT;
    const uint32_t mask = (uint32_t)mask_raw;
    if (mask_raw > UINT32_MAX || (mask & LPR_IN_ALL_EVENTS) == 0 ||
        (mask & ~(LPR_IN_ALL_EVENTS | LPR_IN_WATCH_FLAGS)) != 0)
    {
        return -LPR_LINUX_EINVAL;
    }
    char normalized[FILED_PATH_BYTES];
    const int64_t normalize_status = lpr_cwd_normalize(
        (const char *)(uintptr_t)path_raw,
        normalized,
        sizeof(normalized));
    if (normalize_status != 0) return normalize_status;

    lpr_inotify_watch_t candidate;
    lpr_memset(&candidate, 0, sizeof(candidate));
    candidate.mask = mask;
    lpr_memcpy(candidate.path, normalized, sizeof(candidate.path));
    lpr_linux_stat_t stat;
    lpr_memset(&stat, 0, sizeof(stat));
    const int64_t stat_status = lpr_inotify_stat(&candidate, &stat);
    if (stat_status != 0) return stat_status;
    if ((mask & LPR_IN_ONLYDIR) != 0 &&
        (stat.st_mode & LPR_LINUX_S_IFMT) != LPR_LINUX_S_IFDIR)
    {
        return -LPR_LINUX_ENOTDIR;
    }

    lpr_state_lock(&instance->lock_word);
    lpr_inotify_watch_t *free_watch = 0;
    for (uint32_t index = 0; index < LPR_INOTIFY_MAX_WATCHES; index++) {
        lpr_inotify_watch_t *watch = &instance->watches[index];
        if (!watch->active) {
            if (free_watch == 0) free_watch = watch;
            continue;
        }
        if (lpr_strcmp(watch->path, normalized) != 0) continue;
        if ((mask & LPR_IN_MASK_CREATE) != 0) {
            lpr_state_unlock(&instance->lock_word);
            return -LPR_LINUX_EEXIST;
        }
        watch->mask = (mask & LPR_IN_MASK_ADD) != 0 ?
            watch->mask | (mask & ~LPR_IN_MASK_ADD) : mask;
        lpr_inotify_snapshot(watch, &stat);
        const int32_t wd = watch->wd;
        lpr_state_unlock(&instance->lock_word);
        return wd;
    }
    if (free_watch == 0) {
        lpr_state_unlock(&instance->lock_word);
        return -LPR_LINUX_ENOSPC;
    }
    lpr_memcpy(free_watch, &candidate, sizeof(*free_watch));
    free_watch->active = 1;
    free_watch->wd = (int32_t)instance->next_wd++;
    if (instance->next_wd == 0 || instance->next_wd > INT32_MAX) {
        instance->next_wd = 1;
    }
    lpr_inotify_snapshot(free_watch, &stat);
    const int32_t wd = free_watch->wd;
    lpr_state_unlock(&instance->lock_word);
    return wd;
}

int64_t lpr_linux_inotify_rm_watch(uint64_t fd, uint64_t wd_raw)
{
    lpr_inotify_instance_t *instance = lpr_inotify_instance(fd);
    if (instance == 0) return -LPR_LINUX_EBADF;
    if (wd_raw > INT32_MAX) return -LPR_LINUX_EINVAL;
    lpr_state_lock(&instance->lock_word);
    for (uint32_t index = 0; index < LPR_INOTIFY_MAX_WATCHES; index++) {
        lpr_inotify_watch_t *watch = &instance->watches[index];
        if (watch->active && watch->wd == (int32_t)wd_raw) {
            lpr_inotify_queue(instance, watch->wd, LPR_IN_IGNORED);
            watch->active = 0;
            lpr_state_unlock(&instance->lock_word);
            return 0;
        }
    }
    lpr_state_unlock(&instance->lock_word);
    return -LPR_LINUX_EINVAL;
}

int64_t lpr_linux_inotify_read(uint64_t fd, uint64_t buf, uint64_t count)
{
    lpr_inotify_instance_t *instance = lpr_inotify_instance(fd);
    lpr_event_backend_t *event = lpr_event_backend(fd);
    if (instance == 0 || event == 0) return -LPR_LINUX_EBADF;
    if (buf == 0) return -LPR_LINUX_EFAULT;
    if (count < sizeof(lpr_linux_inotify_event_t)) return -LPR_LINUX_EINVAL;
    for (;;) {
        lpr_inotify_scan(fd);
        lpr_state_lock(&instance->lock_word);
        uint64_t written = 0;
        while (instance->event_count != 0 &&
               count - written >= sizeof(lpr_linux_inotify_event_t))
        {
            lpr_memcpy(
                (void *)(uintptr_t)(buf + written),
                &instance->events[instance->event_head],
                sizeof(lpr_linux_inotify_event_t));
            instance->event_head =
                (instance->event_head + 1u) % LPR_INOTIFY_MAX_EVENTS;
            instance->event_count--;
            written += sizeof(lpr_linux_inotify_event_t);
        }
        if (instance->event_count == 0) {
            instance->event_head = 0;
            instance->overflow_queued = 0;
        }
        lpr_state_unlock(&instance->lock_word);
        if (written != 0) return (int64_t)written;
        if ((event->flags & LPR_LINUX_O_NONBLOCK) != 0) {
            return -LPR_LINUX_EAGAIN;
        }
        lpr_wait_graph_t graph;
        lpr_wait_deadline_t deadline;
        lpr_wait_graph_init(&graph);
        int64_t status = lpr_wait_graph_add_fd(&graph, fd, 0x0001u);
        if (status == 0) status = lpr_wait_deadline_init(&deadline, -1);
        if (status == 0) status = lpr_wait_graph_block(&graph, &deadline);
        if (status != 0) return status;
    }
}

uint32_t lpr_linux_inotify_poll_events(uint64_t fd, uint32_t events)
{
    lpr_inotify_instance_t *instance = lpr_inotify_instance(fd);
    if (instance == 0) return 0;
    lpr_inotify_scan(fd);
    lpr_state_lock(&instance->lock_word);
    const uint32_t ready = instance->event_count != 0 ?
        events & 0x0001u : 0;
    lpr_state_unlock(&instance->lock_word);
    return ready;
}
