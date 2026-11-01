#ifndef LPR_VFS_OPEN_FLAGS_H
#define LPR_VFS_OPEN_FLAGS_H

#include <stdint.h>

/* x86-64 Linux open(2) ABI values.  Keep the legacy open/openat
 * normalization separate from the strict openat2 request sent to filed. */
#define LPR_LINUX_O_ACCMODE 00000003ull
#define LPR_LINUX_O_RDONLY 00000000ull
#define LPR_LINUX_O_WRONLY 00000001ull
#define LPR_LINUX_O_RDWR 00000002ull
#define LPR_LINUX_O_CREAT 00000100ull
#define LPR_LINUX_O_EXCL 00000200ull
#define LPR_LINUX_O_NOCTTY 00000400ull
#define LPR_LINUX_O_TRUNC 00001000ull
#define LPR_LINUX_O_APPEND 00002000ull
#define LPR_LINUX_O_NONBLOCK 00004000ull
#define LPR_LINUX_O_DSYNC 00010000ull
#define LPR_LINUX_FASYNC 00020000ull
#define LPR_LINUX_O_DIRECT 00040000ull
#define LPR_LINUX_O_LARGEFILE 00100000ull
#define LPR_LINUX_O_DIRECTORY 00200000ull
#define LPR_LINUX_O_NOFOLLOW 00400000ull
#define LPR_LINUX_O_NOATIME 01000000ull
#define LPR_LINUX_O_CLOEXEC 02000000ull
#define LPR_LINUX___O_SYNC 04000000ull
#define LPR_LINUX_O_PATH 010000000ull
#define LPR_LINUX___O_TMPFILE 020000000ull

#define LPR_LINUX_VALID_OPEN_FLAGS ( \
    LPR_LINUX_O_ACCMODE | LPR_LINUX_O_CREAT | LPR_LINUX_O_EXCL | \
    LPR_LINUX_O_NOCTTY | LPR_LINUX_O_TRUNC | LPR_LINUX_O_APPEND | \
    LPR_LINUX_O_NONBLOCK | LPR_LINUX_O_DSYNC | LPR_LINUX_FASYNC | \
    LPR_LINUX_O_DIRECT | LPR_LINUX_O_LARGEFILE | LPR_LINUX_O_DIRECTORY | \
    LPR_LINUX_O_NOFOLLOW | LPR_LINUX_O_NOATIME | LPR_LINUX_O_CLOEXEC | \
    LPR_LINUX___O_SYNC | LPR_LINUX_O_PATH | LPR_LINUX___O_TMPFILE)

#define LPR_LINUX_O_PATH_FLAGS (LPR_LINUX_O_DIRECTORY | \
    LPR_LINUX_O_NOFOLLOW | LPR_LINUX_O_PATH | LPR_LINUX_O_CLOEXEC)

static inline void lpr_linux_legacy_open_how(uint64_t *flags, uint64_t *mode)
{
    uint64_t normalized_flags = *flags & LPR_LINUX_VALID_OPEN_FLAGS;
    uint64_t normalized_mode = *mode & 07777u;

    /* Linux build_open_how(): O_PATH wins over every incompatible flag.
     * In particular, libc adds O_LARGEFILE to openat(), while strict
     * openat2 rejects O_PATH | O_LARGEFILE with EINVAL. */
    if (normalized_flags & LPR_LINUX_O_PATH)
        normalized_flags &= LPR_LINUX_O_PATH_FLAGS;
    if (!(normalized_flags & (LPR_LINUX_O_CREAT | LPR_LINUX___O_TMPFILE)))
        normalized_mode = 0;

    *flags = normalized_flags;
    *mode = normalized_mode;
}

#endif
