#include <assert.h>
#include <stdint.h>
#include "userland/personality/linux/runtime/lpr_vfs/open_flags.h"

int main(void)
{
    uint64_t flags = LPR_LINUX_O_PATH | LPR_LINUX_O_LARGEFILE;
    uint64_t mode = 0777;
    lpr_linux_legacy_open_how(&flags, &mode);
    assert(flags == LPR_LINUX_O_PATH);
    assert(mode == 0);

    flags = LPR_LINUX_O_PATH | LPR_LINUX_O_DIRECTORY |
        LPR_LINUX_O_NOFOLLOW | LPR_LINUX_O_CLOEXEC |
        LPR_LINUX_O_RDWR | LPR_LINUX_O_CREAT | (1ull << 63);
    mode = 04755;
    lpr_linux_legacy_open_how(&flags, &mode);
    assert(flags == (LPR_LINUX_O_PATH | LPR_LINUX_O_DIRECTORY |
        LPR_LINUX_O_NOFOLLOW | LPR_LINUX_O_CLOEXEC));
    assert(mode == 0);

    flags = LPR_LINUX_O_CREAT | LPR_LINUX_O_RDWR | (1ull << 63);
    mode = 017777;
    lpr_linux_legacy_open_how(&flags, &mode);
    assert(flags == (LPR_LINUX_O_CREAT | LPR_LINUX_O_RDWR));
    assert(mode == 07777);

    flags = LPR_LINUX___O_TMPFILE | LPR_LINUX_O_DIRECTORY |
        LPR_LINUX_O_RDWR;
    mode = 010640;
    lpr_linux_legacy_open_how(&flags, &mode);
    assert(flags == (LPR_LINUX___O_TMPFILE | LPR_LINUX_O_DIRECTORY |
        LPR_LINUX_O_RDWR));
    assert(mode == 0640);

    flags = LPR_LINUX_O_RDONLY | (1ull << 63);
    mode = 0777;
    lpr_linux_legacy_open_how(&flags, &mode);
    assert(flags == LPR_LINUX_O_RDONLY);
    assert(mode == 0);
    return 0;
}
