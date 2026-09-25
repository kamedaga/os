#include "escrow.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct unix_escrow_entry {
    struct unix_escrow_entry *next;
    uint64_t id;
    uint32_t references;
    struct unix_transfer_item item;
    struct pacha_ipc_fd caps[UNIX_TRANSFER_CAPS_PER_ITEM];
};

void unix_escrow_init(struct unix_escrow *escrow)
{
    *escrow = (struct unix_escrow){ .next_id = 1 };
}

static struct unix_escrow_entry *find(struct unix_escrow *escrow, uint64_t id)
{
    for (struct unix_escrow_entry *entry = escrow->entries; entry; entry = entry->next)
        if (entry->id == id) return entry;
    return NULL;
}

static void dispose(struct unix_escrow_entry *entry)
{
    for (unsigned i = 0; i < entry->item.capability_count; i++)
        (void)pacha_fd_close((int)entry->caps[i].fd);
    free(entry);
}

void unix_escrow_destroy(struct unix_escrow *escrow)
{
    while (escrow->entries) {
        struct unix_escrow_entry *entry = escrow->entries;
        escrow->entries = entry->next;
        dispose(entry);
    }
}

int unix_escrow_capture(struct unix_escrow *escrow, const struct unix_transfer_item *item,
    struct pacha_ipc_fd *incoming, unsigned count, uint64_t *reference)
{
    if (!item || !reference || !incoming || item->provider == UNIX_TRANSFER_SOCKET ||
        !item->object || !item->capability_count ||
        item->capability_count > UNIX_TRANSFER_CAPS_PER_ITEM ||
        item->capability_first > count || item->capability_count > count - item->capability_first)
        return -EINVAL;
    if (!escrow->next_id) return -EOVERFLOW;
    struct unix_escrow_entry *entry = calloc(1, sizeof(*entry));
    if (!entry) return -ENOMEM;
    for (unsigned i = 0; i < item->capability_count; i++) {
        const uint64_t fd = incoming[item->capability_first + i].fd;
        struct pacha_fd_info info;
        if (fd < 16 || fd >= PACHA_FD_TABLE_LIMIT || pacha_fd_get_info((int)fd, &info) != 0 ||
            !(info.rights & PACHA_FD_RIGHT_TRANSFER)) { free(entry); return -EBADF; }
        for (unsigned j = 0; j < i; j++)
            if (entry->caps[j].fd == fd) { free(entry); return -EINVAL; }
        entry->caps[i] = (struct pacha_ipc_fd){ .fd = fd, .rights = info.rights };
    }
    entry->item = *item;
    entry->item.capability_first = 0;
    entry->id = escrow->next_id++;
    entry->references = 1;
    entry->next = escrow->entries;
    escrow->entries = entry;
    for (unsigned i = 0; i < item->capability_count; i++)
        incoming[item->capability_first + i].fd = 0;
    *reference = entry->id;
    return 0;
}

int unix_escrow_retain(struct unix_escrow *escrow, uint64_t id)
{
    struct unix_escrow_entry *entry = find(escrow, id);
    if (!entry) return -EBADF;
    if (entry->references == UINT32_MAX) return -EOVERFLOW;
    entry->references++;
    return 0;
}

void unix_escrow_release(struct unix_escrow *escrow, uint64_t id)
{
    struct unix_escrow_entry **cursor = &escrow->entries;
    while (*cursor && (*cursor)->id != id) cursor = &(*cursor)->next;
    if (!*cursor) return;
    struct unix_escrow_entry *entry = *cursor;
    if (--entry->references) return;
    *cursor = entry->next;
    dispose(entry);
}

int unix_escrow_export(struct unix_escrow *escrow, uint64_t id,
    struct unix_transfer_item *item, struct pacha_ipc_fd *caps, unsigned capacity)
{
    struct unix_escrow_entry *entry = find(escrow, id);
    if (!entry) return -EBADF;
    if (!item || !caps || entry->item.capability_count > capacity) return -ENOBUFS;
    *item = entry->item;
    memcpy(caps, entry->caps, item->capability_count * sizeof(*caps));
    return 0;
}

int unix_escrow_matches(struct unix_escrow *escrow, uint64_t id, const struct unix_transfer_item *item)
{
    struct unix_escrow_entry *entry = find(escrow, id);
    if (!entry || !item) return 0;
    struct unix_transfer_item normalized = *item;
    normalized.capability_first = 0;
    return memcmp(&entry->item, &normalized, sizeof(normalized)) == 0;
}
