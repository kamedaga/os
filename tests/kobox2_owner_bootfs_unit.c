/* SPDX-License-Identifier: MIT */
#include "../userland/kobox2_owner/bootfs.h"
#include <pacha/ipc.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned char image[240], valid[240];
static unsigned calls;
void *pacha_mmap(pacha_fd_t fd, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset)
{ (void)fd; (void)size; (void)prot; (void)flags; (void)offset; ++calls; return image; }
int pacha_munmap(void *address, uint64_t size)
{ (void)address; (void)size; return 0; }
static void wr(unsigned offset, uint64_t value, unsigned size)
{ for (unsigned i = 0; i < size; ++i) image[offset + i] = (unsigned char)(value >> (8 * i)); }
static void bad(void)
{
    struct ph_bootfs archive = {0};
    assert(ph_bootfs_view(&archive, image, sizeof(image)) == -EBADMSG);
    memcpy(image, valid, sizeof(image));
}
int main(void)
{
    wr(0, 0x53465442, 4); wr(4, 104, 4); wr(8, 240, 8);
    wr(16, 2, 4); wr(20, 96, 4); wr(24, 104, 8); wr(32, 200, 8);
    wr(40, 4, 8); wr(48, 208, 8); wr(56, 32, 8);
    wr(104, 0, 4); wr(108, 2, 2); image[110] = 1; wr(112, 208, 8); wr(120, 3, 8);
    wr(152, 2, 4); wr(156, 2, 2); image[158] = 1; wr(160, 208, 8); wr(168, 3, 8);
    memcpy(image + 200, "/a/b", 4); memcpy(image + 208, "abc", 3);
    memcpy(valid, image, sizeof(image));
    struct ph_bootfs archive = {0};
    assert(ph_bootfs_view(&archive, image, sizeof(image)) == 0);
    const unsigned char *bytes; size_t size;
    assert(ph_bootfs_find(&archive, "/a", &bytes, &size) == 0 && size == 3 && !memcmp(bytes, "abc", 3));
    assert(ph_bootfs_find(&archive, "/b", &bytes, &size) == 0 && size == 3);
    assert(ph_bootfs_find(&archive, "/absent", &bytes, &size) == -ENOENT && !bytes && !size);
    wr(4, 0, 4); bad(); wr(4, 103, 4); bad(); wr(4, 105, 4); bad();
    /* The former version/header pair is not a valid extent or an alias. */
    wr(4, 1, 2); wr(6, 104, 2); bad();
    wr(8, UINT64_MAX, 8); bad(); wr(24, UINT64_MAX, 8); bad();
    wr(32, 199, 8); bad(); wr(40, UINT64_MAX, 8); bad(); wr(48, 203, 8); bad();
    wr(56, 33, 8); bad(); image[64] = 1; bad(); image[110] = 2; bad();
    image[111] = 2; bad(); image[132] = 1; bad(); wr(104, 4, 4); bad();
    wr(112, 200, 8); bad(); wr(120, UINT64_MAX, 8); bad(); image[201] = 0; bad();
    image[201] = '.'; bad(); image[203] = 'a'; bad(); wr(152, 0, 4); bad();
    assert(ph_bootfs_open(&archive, 16, UINT64_MAX) == -EINVAL && !calls);
    assert(ph_bootfs_open(&archive, 16, sizeof(image)) == 0 && calls == 1);
    assert(ph_bootfs_close(&archive) == 0 && !archive.image);
    puts("owner bootfs: valid aliases, exact header extent, legacy rejection and malformed/overflow cases passed");
    return 0;
}
