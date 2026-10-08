/* SPDX-License-Identifier: MIT */
#include "filed/payload.h"
#include <stdlib.h>
#include <string.h>

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static void wr64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static int charge(size_t *total, uint64_t n, size_t width)
{
    if (n > (SIZE_MAX - *total) / width) return -75;
    *total += (size_t)n * width;
    return 0;
}
static int region(uint64_t off, uint64_t len, size_t size)
{
    return len ? off >= KB2_FILESYSTEM_EXEC_PAYLOAD_SIZE &&
        off <= size && len <= size - off : off == 0;
}
static int string_region(uint64_t off, uint64_t len, const uint8_t *p,
                         size_t size)
{
    return len && region(off, len, size) && !p[off + len - 1] &&
        !memchr(p + off, 0, (size_t)len - 1);
}

int filed_exec_payload_encode(const filed_exec_path_t *r, uint8_t **out,
                              size_t *out_size)
{
    if (!r || !out || !out_size ||
        (r->argc && !r->argv) || (r->envc && !r->envp) ||
        (r->inherit_fd_count && !r->fd_grants) ||
        (r->inherit_handle_count && !r->inherit_handles) ||
        (r->fd_patch_count && !r->fd_patches)) return -22;
    *out = NULL;
    *out_size = 0;
    size_t size = KB2_FILESYSTEM_EXEC_PAYLOAD_SIZE;
    const uint64_t counts[] = { r->argc, r->envc, r->inherit_fd_count,
        r->inherit_handle_count, r->fd_patch_count };
    const size_t widths[] = { KB2_FILESYSTEM_EXEC_STRING_SIZE,
        KB2_FILESYSTEM_EXEC_STRING_SIZE, KB2_FILESYSTEM_EXEC_GRANT_SIZE,
        sizeof(uint64_t), KB2_FILESYSTEM_EXEC_PATCH_SIZE };
    size_t offsets[5] = {0};
    for (unsigned i = 0; i < 5; ++i) {
        offsets[i] = counts[i] ? size : 0;
        int status = charge(&size, counts[i], widths[i]);
        if (status) return status;
    }
    size_t string_offset = size;
    for (unsigned list = 0; list < 2; ++list) {
        uint64_t count = list ? r->envc : r->argc;
        const filed_exec_string_ref_t *refs = list ? r->envp : r->argv;
        for (uint64_t i = 0; i < count; ++i) {
            if (!filed_exec_string_ref_valid(r, refs[i])) return -22;
            int status = charge(&size, refs[i].length, 1);
            if (status) return status;
        }
    }
    uint8_t *p = calloc(1, size);
    if (!p) return -12;
    wr64(p + KB2_FILESYSTEM_EXEC_PAYLOAD_FLAGS_OFFSET, r->flags);
    wr64(p + KB2_FILESYSTEM_EXEC_PAYLOAD_ARGC_OFFSET, r->argc);
    wr64(p + KB2_FILESYSTEM_EXEC_PAYLOAD_ENVC_OFFSET, r->envc);
    wr64(p + KB2_FILESYSTEM_EXEC_PAYLOAD_INHERIT_FD_COUNT_OFFSET, r->inherit_fd_count);
    wr64(p + KB2_FILESYSTEM_EXEC_PAYLOAD_INHERIT_HANDLE_COUNT_OFFSET, r->inherit_handle_count);
    wr64(p + KB2_FILESYSTEM_EXEC_PAYLOAD_FD_PATCH_COUNT_OFFSET, r->fd_patch_count);
    const size_t spans[] = { KB2_FILESYSTEM_EXEC_PAYLOAD_ARGV_OFFSET_OFFSET,
        KB2_FILESYSTEM_EXEC_PAYLOAD_ENVP_OFFSET_OFFSET,
        KB2_FILESYSTEM_EXEC_PAYLOAD_GRANTS_OFFSET_OFFSET,
        KB2_FILESYSTEM_EXEC_PAYLOAD_HANDLES_OFFSET_OFFSET,
        KB2_FILESYSTEM_EXEC_PAYLOAD_PATCHES_OFFSET_OFFSET };
    for (unsigned i = 0; i < 5; ++i) {
        wr64(p + spans[i], offsets[i]);
        wr64(p + spans[i] + 8, counts[i] * widths[i]);
    }
    for (unsigned list = 0; list < 2; ++list) {
        uint64_t count = list ? r->envc : r->argc;
        const filed_exec_string_ref_t *refs = list ? r->envp : r->argv;
        for (uint64_t i = 0; i < count; ++i) {
            uint8_t *ref = p + offsets[list] + i * KB2_FILESYSTEM_EXEC_STRING_SIZE;
            wr64(ref + KB2_FILESYSTEM_EXEC_STRING_OFFSET_OFFSET, string_offset);
            wr64(ref + KB2_FILESYSTEM_EXEC_STRING_LENGTH_OFFSET, refs[i].length);
            memcpy(p + string_offset, r->strings + refs[i].offset,
                   (size_t)refs[i].length);
            string_offset += (size_t)refs[i].length;
        }
    }
    for (uint64_t i = 0; i < r->inherit_fd_count; ++i) {
        uint8_t *g = p + offsets[2] + i * KB2_FILESYSTEM_EXEC_GRANT_SIZE;
        wr64(g + KB2_FILESYSTEM_EXEC_GRANT_TARGET_OFFSET, r->fd_grants[i].target);
        wr64(g + KB2_FILESYSTEM_EXEC_GRANT_RIGHTS_OFFSET, r->fd_grants[i].rights);
        wr64(g + KB2_FILESYSTEM_EXEC_GRANT_FLAGS_OFFSET, r->fd_grants[i].flags);
    }
    for (uint64_t i = 0; i < r->inherit_handle_count; ++i)
        wr64(p + offsets[3] + i * 8, r->inherit_handles[i]);
    for (uint64_t i = 0; i < r->fd_patch_count; ++i) {
        uint8_t *patch = p + offsets[4] + i * KB2_FILESYSTEM_EXEC_PATCH_SIZE;
        if (r->fd_patches[i].reserved0) { free(p); return -22; }
        wr64(patch + KB2_FILESYSTEM_EXEC_PATCH_KIND_OFFSET, r->fd_patches[i].kind);
        wr64(patch + KB2_FILESYSTEM_EXEC_PATCH_INDEX_OFFSET, r->fd_patches[i].index);
        wr64(patch + KB2_FILESYSTEM_EXEC_PATCH_OFFSET_OFFSET, r->fd_patches[i].offset);
    }
    *out = p;
    *out_size = size;
    return 0;
}

int filed_exec_payload_decode(const uint8_t *p, size_t size, const char *path,
                              uint64_t directory, uint64_t flags,
                              filed_exec_path_t *r)
{
    if (!p || size < KB2_FILESYSTEM_EXEC_PAYLOAD_SIZE || !path || !r)
        return -22;
    memset(r, 0, sizeof(*r));
    if (rd64(p + KB2_FILESYSTEM_EXEC_PAYLOAD_FLAGS_OFFSET) != flags) return -22;
    const size_t spans[] = { KB2_FILESYSTEM_EXEC_PAYLOAD_ARGV_OFFSET_OFFSET,
        KB2_FILESYSTEM_EXEC_PAYLOAD_ENVP_OFFSET_OFFSET,
        KB2_FILESYSTEM_EXEC_PAYLOAD_GRANTS_OFFSET_OFFSET,
        KB2_FILESYSTEM_EXEC_PAYLOAD_HANDLES_OFFSET_OFFSET,
        KB2_FILESYSTEM_EXEC_PAYLOAD_PATCHES_OFFSET_OFFSET };
    const size_t widths[] = { KB2_FILESYSTEM_EXEC_STRING_SIZE,
        KB2_FILESYSTEM_EXEC_STRING_SIZE, KB2_FILESYSTEM_EXEC_GRANT_SIZE,
        sizeof(uint64_t), KB2_FILESYSTEM_EXEC_PATCH_SIZE };
    uint64_t counts[] = { rd64(p + KB2_FILESYSTEM_EXEC_PAYLOAD_ARGC_OFFSET),
        rd64(p + KB2_FILESYSTEM_EXEC_PAYLOAD_ENVC_OFFSET),
        rd64(p + KB2_FILESYSTEM_EXEC_PAYLOAD_INHERIT_FD_COUNT_OFFSET),
        rd64(p + KB2_FILESYSTEM_EXEC_PAYLOAD_INHERIT_HANDLE_COUNT_OFFSET),
        rd64(p + KB2_FILESYSTEM_EXEC_PAYLOAD_FD_PATCH_COUNT_OFFSET) };
    uint64_t offsets[5], lengths[5];
    size_t allocation_size = size;
    size_t array_offsets[5];
    for (unsigned i = 0; i < 5; ++i) {
        offsets[i] = rd64(p + spans[i]);
        lengths[i] = rd64(p + spans[i] + 8);
        if (counts[i] > SIZE_MAX / widths[i] ||
            lengths[i] != counts[i] * widths[i] ||
            !region(offsets[i], lengths[i], size)) return -22;
        for (unsigned j = 0; j < i; ++j)
            if (lengths[i] && lengths[j] &&
                offsets[i] < offsets[j] + lengths[j] &&
                offsets[j] < offsets[i] + lengths[i]) return -22;
        array_offsets[i] = allocation_size;
        int status = charge(&allocation_size, counts[i], widths[i]);
        if (status) return status;
    }
    /* Align native decoded arrays independently of serialized offsets. */
    if (size > SIZE_MAX - 7) return -75;
    size_t padding = ((size + 7) & ~(size_t)7) - size;
    if (allocation_size > SIZE_MAX - padding) return -75;
    allocation_size += padding;
    for (unsigned i = 0; i < 5; ++i) array_offsets[i] += padding;
    uint8_t *owned = calloc(1, allocation_size);
    if (!owned) return -12;
    memcpy(owned, p, size);
    *r = (filed_exec_path_t){ .dir_handle = directory, .flags = flags,
        .argc = counts[0], .envc = counts[1], .inherit_fd_count = counts[2],
        .inherit_handle_count = counts[3], .fd_patch_count = counts[4],
        .path = (char *)path, .strings = (char *)owned, .string_bytes = size,
        .argv = (void *)(owned + array_offsets[0]),
        .envp = (void *)(owned + array_offsets[1]),
        .fd_grants = (void *)(owned + array_offsets[2]),
        .inherit_handles = (void *)(owned + array_offsets[3]),
        .fd_patches = (void *)(owned + array_offsets[4]), .allocation = owned };
    for (unsigned list = 0; list < 2; ++list) {
        filed_exec_string_ref_t *refs = list ? r->envp : r->argv;
        for (uint64_t i = 0; i < counts[list]; ++i) {
            const uint8_t *q = p + offsets[list] + i * KB2_FILESYSTEM_EXEC_STRING_SIZE;
            refs[i].offset = rd64(q);
            refs[i].length = rd64(q + 8);
            if (!string_region(refs[i].offset, refs[i].length, p, size)) goto invalid;
            for (unsigned j = 0; j < 5; ++j)
                if (lengths[j] && refs[i].offset < offsets[j] + lengths[j] &&
                    offsets[j] < refs[i].offset + refs[i].length) goto invalid;
        }
    }
    for (uint64_t i = 0; i < counts[2]; ++i) {
        const uint8_t *q = p + offsets[2] + i * KB2_FILESYSTEM_EXEC_GRANT_SIZE;
        r->fd_grants[i] = (filed_exec_fd_grant_t){rd64(q),rd64(q+8),rd64(q+16)};
    }
    for (uint64_t i = 0; i < counts[3]; ++i)
        r->inherit_handles[i] = rd64(p + offsets[3] + i * 8);
    for (uint64_t i = 0; i < counts[4]; ++i) {
        const uint8_t *q = p + offsets[4] + i * KB2_FILESYSTEM_EXEC_PATCH_SIZE;
        r->fd_patches[i] = (filed_exec_fd_patch_t){rd64(q),rd64(q+8),rd64(q+16),rd64(q+24)};
        if (r->fd_patches[i].reserved0) goto invalid;
    }
    return 0;
invalid:
    filed_exec_payload_destroy(r);
    return -22;
}
void filed_exec_payload_destroy(filed_exec_path_t *r)
{
    if (r) { free(r->allocation); memset(r, 0, sizeof(*r)); }
}
