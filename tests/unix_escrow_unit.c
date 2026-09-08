/* The native FD table is mocked; this verifies escrow ownership and failure
 * atomicity, not native IPC or an external provider's import contract. */
#include "escrow.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct pacha_fd_info table[256];
static unsigned closes;

int pacha_fd_get_info(int fd, struct pacha_fd_info *out)
{
    if (fd < 16 || fd >= 256 || !table[fd].kind) return -1;
    *out = table[fd];
    return 0;
}

int pacha_fd_close(int fd)
{
    assert(fd >= 16 && fd < 256 && table[fd].kind);
    table[fd] = (struct pacha_fd_info){0};
    closes++;
    return 0;
}

int main(void)
{
    struct unix_escrow escrow;
    unix_escrow_init(&escrow);
    const uint64_t rights = PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_WAIT;
    table[20] = table[21] = (struct pacha_fd_info){ .kind = PACHA_FD_KIND_CHANNEL, .rights = rights };
    struct pacha_ipc_fd incoming[3] = { { .fd = 19 },
        { .fd = 20, .rights = UINT64_MAX, .transfer_flags = PACHA_IPC_TRANSFER_MOVE }, { .fd = 21 } };
    struct unix_transfer_item item = { .provider = 1, .object = 42,
        .capability_first = 1, .capability_count = 2, .rights = 7, .flags = 3,
        .provider_data = 2 };
    uint64_t reference = 0;
    assert(unix_escrow_capture(&escrow, &item, incoming, 2, &reference) == -EINVAL);
    table[21].rights = 0;
    assert(unix_escrow_capture(&escrow, &item, incoming, 3, &reference) == -EBADF);
    assert(incoming[1].fd == 20 && incoming[2].fd == 21 && !escrow.entries && !closes);
    table[21].rights = rights;
    incoming[2].fd = 20;
    assert(unix_escrow_capture(&escrow, &item, incoming, 3, &reference) == -EINVAL);
    assert(incoming[1].fd == 20 && !escrow.entries && !closes);
    incoming[2].fd = UINT64_MAX;
    assert(unix_escrow_capture(&escrow, &item, incoming, 3, &reference) == -EBADF);
    incoming[2].fd = 21;
    item.provider = UNIX_TRANSFER_SOCKET;
    assert(unix_escrow_capture(&escrow, &item, incoming, 3, &reference) == -EINVAL);
    item.provider = 1;
    assert(unix_escrow_capture(&escrow, &item, incoming, 3, &reference) == 0);
    assert(reference && incoming[0].fd == 19 && !incoming[1].fd && !incoming[2].fd);
    assert(unix_escrow_retain(&escrow, reference) == 0);
    unix_escrow_release(&escrow, reference);
    assert(!closes);
    struct unix_transfer_item exported;
    struct pacha_ipc_fd caps[4] = {{0}};
    assert(unix_escrow_export(&escrow, reference, &exported, caps, 1) == -ENOBUFS);
    assert(!caps[0].fd);
    assert(unix_escrow_export(&escrow, reference, &exported, caps, 4) == 0);
    assert(exported.capability_first == 0 && exported.capability_count == 2);
    assert(exported.provider_data == item.provider_data);
    assert(caps[0].fd == 20 && caps[1].fd == 21 && caps[0].rights == rights);
    assert(!caps[0].flags && !caps[0].transfer_flags && !caps[1].transfer_flags);
    assert(unix_escrow_matches(&escrow, reference, &item));
    item.provider_data++;
    assert(!unix_escrow_matches(&escrow, reference, &item));
    item.provider_data--;
    item.object++;
    assert(!unix_escrow_matches(&escrow, reference, &item));
    item.object--;
    item.capability_first = 17;
    assert(unix_escrow_matches(&escrow, reference, &item));
    unix_escrow_release(&escrow, reference);
    assert(closes == 2 && !escrow.entries);
    assert(unix_escrow_retain(&escrow, reference) == -EBADF);
    assert(unix_escrow_export(&escrow, reference, &exported, caps, 4) == -EBADF);
    unix_escrow_release(&escrow, reference);
    assert(closes == 2);

    item.capability_first = 0;
    item.capability_count = 1;
    incoming[0].fd = 22;
    table[22] = (struct pacha_fd_info){ .kind = PACHA_FD_KIND_EVENT, .rights = rights };
    escrow.next_id = UINT64_MAX;
    assert(unix_escrow_capture(&escrow, &item, incoming, 1, &reference) == 0);
    assert(reference == UINT64_MAX);
    incoming[0].fd = 23;
    table[23] = table[22];
    assert(unix_escrow_capture(&escrow, &item, incoming, 1, &reference) == -EOVERFLOW);
    assert(incoming[0].fd == 23);
    unix_escrow_destroy(&escrow);
    assert(closes == 3 && !escrow.entries);
    assert(table[23].kind); /* rejected request still owns its capability */
    pacha_fd_close(23);
    puts("unix escrow unit: ok");
}
