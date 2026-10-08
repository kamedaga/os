/* SPDX-License-Identifier: MIT */
#include "bootfs.h"

#include <pacha/ipc.h>
#include <errno.h>
#include <string.h>

static uint16_t rd16(const unsigned char *p)
{ return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t rd32(const unsigned char *p)
{ return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static uint64_t rd64(const unsigned char *p)
{ return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static int fits(uint64_t size, uint64_t offset, uint64_t length)
{ return offset <= size && length <= size - offset; }
static int zero(const unsigned char *p, size_t n)
{
    while (n--) if (*p++) return 0;
    return 1;
}

static int valid_path(const unsigned char *p, size_t n)
{
    if (n < 2 || p[0] != '/' || p[n - 1] == '/') return 0;
    size_t component = 1;
    for (size_t i = 1; i <= n; ++i) {
        if (i != n && p[i] == '\0') return 0;
        if (i != n && p[i] != '/') continue;
        size_t length = i - component;
        if (!length || (length == 1 && p[component] == '.') ||
            (length == 2 && p[component] == '.' && p[component + 1] == '.'))
            return 0;
        component = i + 1;
    }
    return 1;
}

int ph_bootfs_view(struct ph_bootfs *archive, const void *bytes, size_t size)
{
    if (!archive || !bytes || size < 104) return -EINVAL;
    const unsigned char *image = bytes;
    struct ph_bootfs view = {.image = image, .size = size,
        .entry_count = rd32(image + 16), .entries = rd64(image + 24),
        .strings = rd64(image + 32), .string_size = rd64(image + 40),
        .data = rd64(image + 48), .data_size = rd64(image + 56)};
    uint64_t total = rd64(image + 8);
    uint64_t entry_size = (uint64_t)view.entry_count * 48;
    if (rd32(image) != 0x53465442 || rd32(image + 4) != 104 ||
        total < 104 || total > size ||
        entry_size != rd32(image + 20) || !zero(image + 64, 40) ||
        view.entries < 104 || !fits(total, view.entries, entry_size) ||
        view.strings < view.entries + entry_size ||
        !fits(total, view.strings, view.string_size) ||
        view.data < view.strings + view.string_size ||
        !fits(total, view.data, view.data_size) || view.data_size != total - view.data)
        return -EBADMSG;
    const unsigned char *previous = NULL;
    size_t previous_size = 0;
    for (uint32_t i = 0; i < view.entry_count; ++i) {
        const unsigned char *entry = image + view.entries + (uint64_t)i * 48;
        uint32_t name_offset = rd32(entry);
        size_t name_size = rd16(entry + 4);
        uint64_t data_offset = rd64(entry + 8), data_size = rd64(entry + 16);
        if (entry[6] != 1 || (entry[7] & ~1u) || !zero(entry + 28, 20) ||
            !fits(view.string_size, name_offset, name_size) ||
            data_offset < view.data || !fits(total, data_offset, data_size) ||
            !valid_path(image + view.strings + name_offset, name_size))
            return -EBADMSG;
        const unsigned char *name = image + view.strings + name_offset;
        /* Pack sorts paths. Requiring that order rejects duplicate authority
         * names without a quadratic scan or a second allocated index. */
        if (previous) {
            size_t common = previous_size < name_size ? previous_size : name_size;
            int order = memcmp(previous, name, common);
            if (order > 0 || (!order && previous_size >= name_size))
                return -EBADMSG;
        }
        previous = name;
        previous_size = name_size;
    }
    view.size = (size_t)total;
    *archive = view;
    return 0;
}

int ph_bootfs_open(struct ph_bootfs *archive, int fd, uint64_t size)
{
    if (!archive || fd < 16 || size < 104 || size > SIZE_MAX - 4095)
        return -EINVAL;
    size_t mapped = ((size_t)size + 4095) & ~(size_t)4095;
    void *image = pacha_mmap(fd, mapped, PACHA_PROT_READ, PACHA_MMAP_SHARED, 0);
    if (!image) return -EIO;
    int error = ph_bootfs_view(archive, image, (size_t)size);
    if (error) { (void)pacha_munmap(image, mapped); return error; }
    archive->mapped_size = mapped;
    return 0;
}

int ph_bootfs_find(const struct ph_bootfs *archive, const char *path,
                  const unsigned char **bytes, size_t *size)
{
    if (!archive || !archive->image || !path || !bytes || !size) return -EINVAL;
    *bytes = NULL;
    *size = 0;
    size_t length = strlen(path);
    uint32_t low = 0, high = archive->entry_count;
    while (low < high) {
        uint32_t index = low + (high - low) / 2;
        const unsigned char *entry = archive->image + archive->entries + (uint64_t)index * 48;
        size_t name_size = rd16(entry + 4);
        const unsigned char *name = archive->image + archive->strings + rd32(entry);
        size_t common = length < name_size ? length : name_size;
        int order = memcmp(path, name, common);
        if (!order) order = length < name_size ? -1 : length > name_size;
        if (order < 0) high = index;
        else if (order > 0) low = index + 1;
        else {
            *bytes = archive->image + rd64(entry + 8);
            *size = (size_t)rd64(entry + 16);
            return 0;
        }
    }
    return -ENOENT;
}

int ph_bootfs_close(struct ph_bootfs *archive)
{
    if (!archive) return -EINVAL;
    int error = archive->mapped_size ?
        pacha_munmap((void *)archive->image, archive->mapped_size) : 0;
    if (!error) memset(archive, 0, sizeof(*archive));
    return error;
}
