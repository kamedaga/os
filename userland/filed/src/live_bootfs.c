#include "filed/live_bootfs.h"

#include "miniz_tinfl.h"

#include <stdlib.h>
#include <string.h>

enum {
    BOOTFS_MAGIC = 0x53465442u,
    BOOTFS_VERSION = 1,
    BOOTFS_HEADER_BYTES = 104,
    BOOTFS_ENTRY_BYTES = 48,
    BOOTFS_KIND_REGULAR = 1,
    BOOTFS_PATH_BYTES = 512,
    PACKED_ROOT_HEADER_BYTES = 16,
    LIVE_ROOT_MAX_BYTES = 64u * 1024u * 1024u,
};

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int fits(uint64_t total, uint64_t offset, uint64_t length)
{
    return offset <= total && length <= total - offset;
}

static int make_parent(filed_tmpfs_backend_t *tmpfs, uint64_t root,
    char *path, uint64_t *out_parent, const char **out_name)
{
    uint64_t parent = root;
    char *part = path + 1;
    if (path[0] != '/' || *part == '\0') return -22;
    for (;;) {
        char *slash = strchr(part, '/');
        const size_t length = slash ? (size_t)(slash - part) : strlen(part);
        if (length == 0 || length >= FILED_TMPFS_NAME_BYTES ||
            (length == 1 && part[0] == '.') ||
            (length == 2 && part[0] == '.' && part[1] == '.')) return -22;
        if (slash == NULL) {
            *out_parent = parent;
            *out_name = part;
            return 0;
        }
        *slash = '\0';
        uint64_t child = 0;
        int status = filed_tmpfs_backend_lookup(tmpfs, parent, part, &child);
        if (status == -2)
            status = filed_tmpfs_backend_mkdir(tmpfs, parent, part, 0755, &child);
        *slash = '/';
        if (status != 0) return status;
        parent = child;
        part = slash + 1;
    }
}

static int ensure_dir(filed_tmpfs_backend_t *tmpfs, uint64_t parent,
    const char *name, uint64_t *out_object)
{
    int status = filed_tmpfs_backend_lookup(tmpfs, parent, name, out_object);
    if (status == -2)
        return filed_tmpfs_backend_mkdir(tmpfs, parent, name, 0755, out_object);
    if (status != 0) return status;
    storage_statx_reply_t stat;
    status = filed_tmpfs_backend_statx(tmpfs, *out_object, &stat);
    return status != 0 ? status :
        ((stat.kind & 0170000u) == 0040000u ? 0 : -20);
}

static int populate_archive(filed_tmpfs_backend_t *tmpfs,
    const unsigned char *image, uint64_t image_size,
    uint32_t *out_files, uint64_t *out_bytes, int allow_packed)
{
    if (tmpfs == NULL || image == NULL || out_files == NULL ||
        out_bytes == NULL || image_size < BOOTFS_HEADER_BYTES) return -22;
    *out_files = 0;
    *out_bytes = 0;
    const uint64_t total = rd64(image + 8);
    const uint32_t count = rd32(image + 16);
    const uint64_t table_bytes = rd32(image + 20);
    const uint64_t table_offset = rd64(image + 24);
    const uint64_t strings_offset = rd64(image + 32);
    const uint64_t strings_bytes = rd64(image + 40);
    if (rd32(image) != BOOTFS_MAGIC || rd16(image + 4) != BOOTFS_VERSION ||
        rd16(image + 6) != BOOTFS_HEADER_BYTES || total > image_size ||
        table_bytes != (uint64_t)count * BOOTFS_ENTRY_BYTES ||
        !fits(total, table_offset, table_bytes) ||
        !fits(total, strings_offset, strings_bytes)) return -22;
    /* The outer archive keeps filed directly executable by seed0boot. Its
     * packed member is identified by format, not a pathname. Only the
     * bounded inner archive becomes the writable RAM root. */
    static const unsigned char packed_magic[8] = {
        'P', 'A', 'C', 'H', 'A', 'Z', '1', 0
    };
    const unsigned char *packed = NULL;
    uint64_t packed_size = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *entry = image + table_offset +
            (uint64_t)i * BOOTFS_ENTRY_BYTES;
        const uint32_t path_offset = rd32(entry);
        const uint16_t path_length = rd16(entry + 4);
        const uint64_t data_offset = rd64(entry + 8);
        const uint64_t data_size = rd64(entry + 16);
        if (entry[6] != BOOTFS_KIND_REGULAR || path_length < 2 ||
            !fits(strings_bytes, path_offset, path_length) ||
            !fits(total, data_offset, data_size)) return -22;
        if (data_size < PACKED_ROOT_HEADER_BYTES ||
            memcmp(image + data_offset, packed_magic, sizeof(packed_magic)) != 0)
            continue;
        if (!allow_packed || packed != NULL) return -22;
        packed = image + data_offset;
        packed_size = data_size;
    }
    if (packed != NULL) {
        const uint64_t expanded_size = rd64(packed + 8);
        if (expanded_size < BOOTFS_HEADER_BYTES ||
            expanded_size > LIVE_ROOT_MAX_BYTES) return -22;
        unsigned char *expanded = malloc((size_t)expanded_size);
        if (expanded == NULL) return -12;
        const size_t got = tinfl_decompress_mem_to_mem(expanded,
            (size_t)expanded_size, packed + PACKED_ROOT_HEADER_BYTES,
            (size_t)packed_size - PACKED_ROOT_HEADER_BYTES,
            TINFL_FLAG_PARSE_ZLIB_HEADER);
        int status = -22;
        if (got == (size_t)expanded_size)
            status = populate_archive(tmpfs, expanded, expanded_size,
                out_files, out_bytes, 0);
        free(expanded);
        return status;
    }
    uint64_t root = 0;
    int status = filed_tmpfs_backend_mount_root(tmpfs, &root);
    if (status != 0) return status;
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *entry = image + table_offset +
            (uint64_t)i * BOOTFS_ENTRY_BYTES;
        const uint32_t path_offset = rd32(entry);
        const uint16_t path_length = rd16(entry + 4);
        const uint64_t data_offset = rd64(entry + 8);
        const uint64_t data_size = rd64(entry + 16);
        const uint32_t mode = rd32(entry + 24);
        if (entry[6] != BOOTFS_KIND_REGULAR || path_length < 2 ||
            path_length >= BOOTFS_PATH_BYTES ||
            !fits(strings_bytes, path_offset, path_length) ||
            !fits(total, data_offset, data_size) ||
            (mode & ~0777u) != 0) return -22;
        char path[BOOTFS_PATH_BYTES];
        memcpy(path, image + strings_offset + path_offset, path_length);
        if (memchr(path, '\0', path_length) != NULL) return -22;
        path[path_length] = '\0';
        uint64_t parent = 0;
        const char *name = NULL;
        status = make_parent(tmpfs, root, path, &parent, &name);
        if (status != 0) return status;
        uint64_t file = 0;
        status = filed_tmpfs_backend_create(tmpfs, parent, name, mode, &file);
        if (status != 0) return status;
        uint64_t written = 0;
        status = filed_tmpfs_backend_pwrite(tmpfs, file, 0,
            image + data_offset, data_size, &written);
        if (status != 0 || written != data_size) return status != 0 ? status : -5;
        ++*out_files;
        *out_bytes += data_size;
    }
    /* The VFS overlays these directories with detached tmpfs mounts. Real
     * directory entries keep getdents consistent with lookup in live root. */
    uint64_t dev = 0, tmp = 0, run = 0, shm = 0;
    status = ensure_dir(tmpfs, root, "dev", &dev);
    if (status == 0) status = ensure_dir(tmpfs, root, "tmp", &tmp);
    if (status == 0) status = ensure_dir(tmpfs, root, "run", &run);
    if (status == 0) status = ensure_dir(tmpfs, dev, "shm", &shm);
    return status;
}

int filed_live_bootfs_populate(filed_tmpfs_backend_t *tmpfs,
    const unsigned char *image, uint64_t image_size,
    uint32_t *out_files, uint64_t *out_bytes)
{
    return populate_archive(tmpfs, image, image_size,
        out_files, out_bytes, 1);
}
