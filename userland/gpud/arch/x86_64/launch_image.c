/* SPDX-License-Identifier: MIT */
#include "../../launch_image.h"
#include <pacha/abi.h>
#include <errno.h>

static uint16_t read16(const unsigned char *bytes) {
    return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
}

static uint32_t read32(const unsigned char *bytes) {
    return (uint32_t)read16(bytes) | (uint32_t)read16(bytes + 2) << 16;
}

static uint64_t read64(const unsigned char *bytes) {
    return (uint64_t)read32(bytes) | (uint64_t)read32(bytes + 4) << 32;
}

int gpud_launch_address_valid(uint64_t address, uint64_t size) {
    return address >= GPUD_LAUNCH_PAGE && address < UINT64_C(0x800000000000) &&
        size && size <= UINT64_C(0x800000000000) - address;
}

static int plan_image(const unsigned char *bytes, size_t size, uint64_t bias,
    struct gpud_launch_image *out) {
    if (!bytes || size < 64 || size > GPUD_LAUNCH_IMAGE_LIMIT ||
        (bias & (GPUD_LAUNCH_PAGE - 1))) return -EINVAL;
    /* ELF64, little endian, current version, native x86-64. */
    if (read32(bytes) != UINT32_C(0x464c457f) || bytes[4] != 2 || bytes[5] != 1 ||
        bytes[6] != 1 || read16(bytes + 18) != 62 || read32(bytes + 20) != 1 ||
        read16(bytes + 52) != 64 || read16(bytes + 54) != 56) return -ENOEXEC;
    uint16_t type = read16(bytes + 16);
    if ((type != 2 && type != 3) || (type == 2 && bias)) return -ENOEXEC;
    uint64_t offset = read64(bytes + 32);
    uint16_t count = read16(bytes + 56);
    if (!count || count > GPUD_LAUNCH_MAX_SEGMENTS || offset > size ||
        (uint64_t)count * 56 > size - offset) return -ENOEXEC;
    uint64_t entry = read64(bytes + 24);
    if (entry > UINT64_MAX - bias) return -ENOEXEC;
    out->entry = entry + bias;
    int executable_entry = 0;
    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char *header = bytes + offset + i * 56;
        uint32_t kind = read32(header), flags = read32(header + 4);
        /* Interpreter and initial TLS require a different startup contract. */
        if (kind == 3 || kind == 7) return -ENOTSUP;
        if (kind == UINT32_C(0x6474e551) && (flags & 1)) return -EACCES;
        if (kind != 1) continue;
        uint64_t file_offset = read64(header + 8), address = read64(header + 16);
        uint64_t file_size = read64(header + 32), memory_size = read64(header + 40);
        uint64_t alignment = read64(header + 48);
        if (file_size > memory_size || file_offset > size || file_size > size - file_offset ||
            flags & ~7u || ((flags & 3) == 3) || !(flags & 4) ||
            (alignment > 1 && (alignment & (alignment - 1))) ||
            (alignment > 1 && (((address - file_offset) | bias) & (alignment - 1))) ||
            address > UINT64_MAX - bias)
            return -ENOEXEC;
        if (!memory_size) continue;
        if (memory_size > GPUD_LAUNCH_IMAGE_LIMIT) return -E2BIG;
        uint64_t head = address & (GPUD_LAUNCH_PAGE - 1);
        uint64_t mapping_size = (head + memory_size + GPUD_LAUNCH_PAGE - 1) &
            ~(uint64_t)(GPUD_LAUNCH_PAGE - 1);
        address += bias;
        uint64_t mapping_address = address - head;
        if (!gpud_launch_address_valid(mapping_address, mapping_size) ||
            mapping_size > GPUD_LAUNCH_IMAGE_LIMIT - total) return -ENOEXEC;
        for (size_t j = 0; j < out->segment_count; ++j) {
            const struct gpud_launch_segment *other = &out->segments[j];
            if (mapping_address < other->address + other->mapping_size &&
                other->address < mapping_address + mapping_size) return -ENOEXEC;
        }
        uint64_t protection = PACHA_PROT_READ;
        if (flags & 2) protection |= PACHA_PROT_WRITE;
        if (flags & 1) protection |= PACHA_PROT_EXEC;
        out->segments[out->segment_count++] = (struct gpud_launch_segment){
            .address = mapping_address, .mapping_size = mapping_size,
            .file_offset = file_offset, .file_size = file_size,
            .data_offset = head, .protection = protection};
        total += mapping_size;
        if ((flags & 1) && out->entry >= address && out->entry - address < file_size)
            executable_entry = 1;
    }
    return out->segment_count && executable_entry ? 0 : -ENOEXEC;
}

int gpud_launch_image_plan(const void *bytes, size_t size, uint64_t load_bias,
    struct gpud_launch_image *out) {
    if (!out) return -EINVAL;
    struct gpud_launch_image plan = {0};
    int result = plan_image(bytes, size, load_bias, &plan);
    if (!result) *out = plan;
    return result;
}
