#define _GNU_SOURCE

#include "pacha/abi.h"
#include "pacha/ipc.h"
#include "pacha/launch.h"
#include "storage/bootstrap.h"
#include "storage_boot/boot_config.h"
#include "../../kobox2_owner/bootfs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    STORAGE_BOOT_PAGE_SIZE = 4096,
    STORAGE_BOOT_USER_ASLR_BASE_VA = 0x0000010000000000ull,
    STORAGE_BOOT_USER_ASLR_END_VA = 0x0000010040000000ull,
    STORAGE_BOOT_USER_ASLR_GRANULE = 0x10000ull,
    STORAGE_BOOT_ELF64_EHDR_BYTES = 64,
    STORAGE_BOOT_ELF64_PHDR_BYTES = 56,
    STORAGE_BOOT_ELF_CLASS_64 = 2,
    STORAGE_BOOT_ELF_DATA_LSB = 1,
    STORAGE_BOOT_ELF_VERSION_CURRENT = 1,
    STORAGE_BOOT_ELF_TYPE_EXEC = 2,
    STORAGE_BOOT_ELF_TYPE_DYN = 3,
    STORAGE_BOOT_ELF_MACHINE_X86_64 = 0x3e,
    STORAGE_BOOT_ELF_PT_LOAD = 1,
    STORAGE_BOOT_ELF_PT_DYNAMIC = 2,
    STORAGE_BOOT_ELF_PF_X = 1,
    STORAGE_BOOT_ELF_PF_W = 2,
    STORAGE_BOOT_ELF_PF_R = 4,
    STORAGE_BOOT_ELF_DT_NULL = 0,
    STORAGE_BOOT_ELF_DT_RELA = 7,
    STORAGE_BOOT_ELF_DT_RELASZ = 8,
    STORAGE_BOOT_ELF_DT_RELAENT = 9,
    STORAGE_BOOT_ELF_DT_RELACOUNT = 0x6ffffff9,
    STORAGE_BOOT_ELF_RELA_BYTES = 24,
    STORAGE_BOOT_ELF_R_X86_64_RELATIVE = 8,
    STORAGE_BOOT_AT_NULL = 0,
    STORAGE_BOOT_AT_PHDR = 3,
    STORAGE_BOOT_AT_PHENT = 4,
    STORAGE_BOOT_AT_PHNUM = 5,
    STORAGE_BOOT_AT_PAGESZ = 6,
    STORAGE_BOOT_AT_BASE = 7,
    STORAGE_BOOT_AT_ENTRY = 9,
    STORAGE_BOOT_AT_UID = 11,
    STORAGE_BOOT_AT_EUID = 12,
    STORAGE_BOOT_AT_GID = 13,
    STORAGE_BOOT_AT_EGID = 14,
    STORAGE_BOOT_AT_SECURE = 23,
    STORAGE_BOOT_AT_RANDOM = 25,
    STORAGE_BOOT_AT_EXECFN = 31,
    STORAGE_BOOT_MMAP_SHARED = 1ull << 3,
    STORAGE_BOOT_PROT_READ = 1ull << 0,
    STORAGE_BOOT_PROT_WRITE = 1ull << 1,
    STORAGE_BOOT_PROT_EXEC = 1ull << 2,
};

static const char storage_boot_seed0root_argv0[] = "/sbin/seed0root.elf";

static int create_inherited_vmo_from_bytes(const void *data, uint64_t size, const char *label)
{
    if (data == NULL || size == 0) {
        return -1;
    }
    if (size > UINT64_MAX - (STORAGE_BOOT_PAGE_SIZE - 1)) {
        return -2;
    }
    const uint64_t map_size = (size + (STORAGE_BOOT_PAGE_SIZE - 1)) & ~(uint64_t)(STORAGE_BOOT_PAGE_SIZE - 1);
    const uint64_t rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_READ |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int fd = pacha_vmo_create(map_size, rights, 0);
    if (fd < 16) {
        fprintf(stderr, "[storage_boot] %s: vmo_create failed status=%d\n", label, fd);
        return -3;
    }
    unsigned char *mapped = pacha_mmap(
        fd,
        map_size,
        STORAGE_BOOT_PROT_READ | STORAGE_BOOT_PROT_WRITE,
        STORAGE_BOOT_MMAP_SHARED,
        0);
    if (mapped == NULL) {
        (void)pacha_fd_close(fd);
        return -4;
    }
    memset(mapped, 0, (size_t)map_size);
    memcpy(mapped, data, (size_t)size);
    (void)pacha_munmap(mapped, map_size);
    return fd;
}

static int read_fd_exact(int fd, void *out, uint64_t size, const char *label)
{
    if (fd < 16 || out == NULL || size == 0) {
        return -1;
    }
    unsigned char *cursor = out;
    uint64_t done = 0;
    while (done < size) {
        const long got = pacha_fd_read(fd, cursor + done, size - done);
        if (got <= 0) {
            fprintf(stderr, "[storage_boot] %s: fd_read failed fd=%d got=%ld done=%llu size=%llu\n",
                label,
                fd,
                got,
                (unsigned long long)done,
                (unsigned long long)size);
            return -2;
        }
        done += (uint64_t)got;
    }
    return 0;
}

static int find_bootstrap_fd(char **argv, int *out_fd)
{
    if (argv == NULL || out_fd == NULL) {
        return -1;
    }
    *out_fd = -1;
    char **p = argv;
    while (*p != NULL) {
        p++;
    }
    p++;
    while (*p != NULL) {
        p++;
    }
    p++;
    for (;;) {
        const uint64_t key = (uint64_t)(uintptr_t)p[0];
        const uint64_t value = (uint64_t)(uintptr_t)p[1];
        if (key == STORAGE_BOOT_AT_NULL) {
            break;
        }
        if (key == PACHA_AT_BOOTSTRAP_FD && value >= 16 && value <= UINT32_MAX) {
            *out_fd = (int)value;
            return 0;
        }
        p += 2;
    }
    return -2;
}

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr64(unsigned char *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) {
        p[i] = (unsigned char)(value >> (i * 8));
    }
}

static uint64_t align_down(uint64_t value)
{
    return value & ~(uint64_t)(STORAGE_BOOT_PAGE_SIZE - 1);
}

static int align_up(uint64_t value, uint64_t *out)
{
    if (value > UINT64_MAX - (STORAGE_BOOT_PAGE_SIZE - 1)) {
        return -1;
    }
    *out = (value + (STORAGE_BOOT_PAGE_SIZE - 1)) & ~(uint64_t)(STORAGE_BOOT_PAGE_SIZE - 1);
    return 0;
}

static uint64_t prot_from_elf_flags(uint32_t flags)
{
    uint64_t prot = 0;
    if ((flags & STORAGE_BOOT_ELF_PF_R) != 0) {
        prot |= STORAGE_BOOT_PROT_READ;
    }
    if ((flags & STORAGE_BOOT_ELF_PF_W) != 0) {
        prot |= STORAGE_BOOT_PROT_WRITE;
    }
    if ((flags & STORAGE_BOOT_ELF_PF_X) != 0) {
        prot |= STORAGE_BOOT_PROT_EXEC;
    }
    return prot;
}

struct storage_boot_loaded_process {
    int process_fd;
    uint64_t runtime_entry;
    uint64_t load_bias;
    uint64_t phdr_va;
    uint64_t phent;
    uint64_t phnum;
    uint16_t load_segments;
};

static int validate_elf_header(const char *path, const unsigned char *image, uint64_t image_size)
{
    if (image == NULL || image_size < STORAGE_BOOT_ELF64_EHDR_BYTES) {
        return -1;
    }
    if (image[0] != 0x7f || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') {
        fprintf(stderr, "[storage_boot] exec: %s ELF magic invalid\n", path);
        return -2;
    }
    if (image[4] != STORAGE_BOOT_ELF_CLASS_64 || image[5] != STORAGE_BOOT_ELF_DATA_LSB ||
        image[6] != STORAGE_BOOT_ELF_VERSION_CURRENT) {
        fprintf(stderr,
            "[storage_boot] exec: %s unsupported ELF ident class=%u data=%u version=%u\n",
            path,
            image[4],
            image[5],
            image[6]);
        return -3;
    }
    const uint16_t e_type = rd16(image + 16);
    const uint16_t e_machine = rd16(image + 18);
    const uint32_t e_version = rd32(image + 20);
    const uint16_t e_phentsize = rd16(image + 54);
    const uint16_t e_phnum = rd16(image + 56);
    if ((e_type != STORAGE_BOOT_ELF_TYPE_EXEC && e_type != STORAGE_BOOT_ELF_TYPE_DYN) ||
        e_machine != STORAGE_BOOT_ELF_MACHINE_X86_64 ||
        e_version != STORAGE_BOOT_ELF_VERSION_CURRENT ||
        e_phentsize < STORAGE_BOOT_ELF64_PHDR_BYTES ||
        e_phnum == 0) {
        fprintf(stderr,
            "[storage_boot] exec: %s unsupported ELF type=%u machine=%04x version=%u phentsize=%u phnum=%u\n",
            path,
            e_type,
            e_machine,
            e_version,
            e_phentsize,
            e_phnum);
        return -4;
    }
    const uint64_t e_phoff = rd64(image + 32);
    const uint64_t phdr_bytes = (uint64_t)e_phentsize * e_phnum;
    if (e_phoff > image_size || phdr_bytes > image_size - e_phoff) {
        fprintf(stderr, "[storage_boot] exec: %s program headers out of range\n", path);
        return -5;
    }
    return 0;
}

static int map_elf_segment(
    const char *path,
    int process_fd,
    uint64_t target_va,
    const unsigned char *image,
    uint64_t image_size,
    const unsigned char *ph,
    uint16_t index,
    uint64_t *out_mapped_va)
{
    const uint32_t p_flags = rd32(ph + 4);
    const uint64_t p_offset = rd64(ph + 8);
    const uint64_t p_vaddr = rd64(ph + 16);
    const uint64_t p_filesz = rd64(ph + 32);
    const uint64_t p_memsz = rd64(ph + 40);
    if (p_memsz < p_filesz ||
        p_offset > image_size ||
        p_filesz > image_size - p_offset ||
        (p_memsz != 0 && p_vaddr > UINT64_MAX - p_memsz)) {
        fprintf(stderr, "[storage_boot] exec: %s invalid PT_LOAD[%u]\n", path, index);
        return -1;
    }
    if (p_memsz == 0) {
        return 0;
    }

    const uint64_t page_offset = p_vaddr - align_down(p_vaddr);
    uint64_t map_size = 0;
    if (align_up(page_offset + p_memsz, &map_size) != 0) {
        return -2;
    }

    const uint64_t vmo_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_MAP_EXEC;
    const int vmo_fd = pacha_vmo_create(map_size, vmo_rights, 0);
    if (vmo_fd < 16) {
        fprintf(stderr, "[storage_boot] exec: vmo_create failed segment=%u status=%d\n", index, vmo_fd);
        return -3;
    }
    unsigned char *mapped = pacha_mmap(vmo_fd, map_size, STORAGE_BOOT_PROT_READ | STORAGE_BOOT_PROT_WRITE, STORAGE_BOOT_MMAP_SHARED, 0);
    if (mapped == NULL) {
        fprintf(stderr, "[storage_boot] exec: mmap staging VMO failed segment=%u fd=%d\n", index, vmo_fd);
        (void)pacha_fd_close(vmo_fd);
        return -4;
    }

    memset(mapped, 0, (size_t)map_size);
    memcpy(mapped + page_offset, image + p_offset, (size_t)p_filesz);

    const uint64_t prot = prot_from_elf_flags(p_flags);
    const long map_result = pacha_process_map(process_fd, vmo_fd, target_va, map_size, prot, 0);
    (void)pacha_munmap(mapped, map_size);
    (void)pacha_fd_close(vmo_fd);
    if (map_result < 4096) {
        fprintf(stderr,
            "[storage_boot] exec: process_map failed segment=%u process_fd=%d status=%ld\n",
            index,
            process_fd,
            map_result);
        return -5;
    }
    if (out_mapped_va != NULL) *out_mapped_va = (uint64_t)map_result;

    return 0;
}

static int elf_vaddr_to_file_offset(
    const unsigned char *image,
    uint64_t image_size,
    uint64_t vaddr,
    uint64_t length,
    uint64_t *out_offset)
{
    if (image == NULL || out_offset == NULL || length > image_size) {
        return -1;
    }
    const uint64_t e_phoff = rd64(image + 32);
    const uint16_t e_phentsize = rd16(image + 54);
    const uint16_t e_phnum = rd16(image + 56);
    for (uint16_t i = 0; i < e_phnum; i++) {
        const unsigned char *ph = image + e_phoff + (uint64_t)i * e_phentsize;
        if (rd32(ph + 0) != STORAGE_BOOT_ELF_PT_LOAD) {
            continue;
        }
        const uint64_t p_offset = rd64(ph + 8);
        const uint64_t p_vaddr = rd64(ph + 16);
        const uint64_t p_filesz = rd64(ph + 32);
        if (vaddr < p_vaddr) {
            continue;
        }
        const uint64_t in_segment = vaddr - p_vaddr;
        if (in_segment > p_filesz || length > p_filesz - in_segment) {
            continue;
        }
        if (p_offset > image_size || in_segment > image_size - p_offset || length > image_size - p_offset - in_segment) {
            return -1;
        }
        *out_offset = p_offset + in_segment;
        return 0;
    }
    return -1;
}

static int apply_elf_relative_relocations(unsigned char *image, uint64_t image_size, uint64_t load_bias)
{
    if (image == NULL || load_bias == 0) {
        return 0;
    }

    const uint64_t e_phoff = rd64(image + 32);
    const uint16_t e_phentsize = rd16(image + 54);
    const uint16_t e_phnum = rd16(image + 56);
    uint64_t dynamic_vaddr = 0;
    uint64_t dynamic_size = 0;
    for (uint16_t i = 0; i < e_phnum; i++) {
        const unsigned char *ph = image + e_phoff + (uint64_t)i * e_phentsize;
        if (rd32(ph + 0) != STORAGE_BOOT_ELF_PT_DYNAMIC) {
            continue;
        }
        dynamic_vaddr = rd64(ph + 16);
        dynamic_size = rd64(ph + 40);
        break;
    }
    if (dynamic_vaddr == 0 || dynamic_size < 16) {
        return 0;
    }

    uint64_t rela_vaddr = 0;
    uint64_t rela_size = 0;
    uint64_t rela_ent = STORAGE_BOOT_ELF_RELA_BYTES;
    uint64_t rela_count = 0;
    for (uint64_t cursor = 0; cursor + 16 <= dynamic_size; cursor += 16) {
        uint64_t dyn_off = 0;
        if (elf_vaddr_to_file_offset(image, image_size, dynamic_vaddr + cursor, 16, &dyn_off) != 0) {
            return -1;
        }
        const uint64_t tag = rd64(image + dyn_off);
        const uint64_t value = rd64(image + dyn_off + 8);
        if (tag == STORAGE_BOOT_ELF_DT_NULL) {
            break;
        }
        switch (tag) {
        case STORAGE_BOOT_ELF_DT_RELA:
            rela_vaddr = value;
            break;
        case STORAGE_BOOT_ELF_DT_RELASZ:
            rela_size = value;
            break;
        case STORAGE_BOOT_ELF_DT_RELAENT:
            rela_ent = value;
            break;
        case STORAGE_BOOT_ELF_DT_RELACOUNT:
            rela_count = value;
            break;
        default:
            break;
        }
    }
    if (rela_vaddr == 0 || rela_size == 0) {
        return 0;
    }
    if (rela_ent != STORAGE_BOOT_ELF_RELA_BYTES || (rela_size % rela_ent) != 0) {
        return -1;
    }

    uint64_t total = rela_size / rela_ent;
    if (rela_count != 0) {
        if (rela_count > total) {
            return -1;
        }
        total = rela_count;
    }
    for (uint64_t i = 0; i < total; i++) {
        uint64_t rela_off = 0;
        if (elf_vaddr_to_file_offset(image, image_size, rela_vaddr + i * rela_ent, STORAGE_BOOT_ELF_RELA_BYTES, &rela_off) != 0) {
            return -1;
        }
        const uint64_t r_offset = rd64(image + rela_off);
        const uint64_t r_info = rd64(image + rela_off + 8);
        const uint64_t r_addend = rd64(image + rela_off + 16);
        const uint32_t r_type = (uint32_t)(r_info & 0xffffffffu);
        const uint64_t r_sym = r_info >> 32;
        if (r_type != STORAGE_BOOT_ELF_R_X86_64_RELATIVE || r_sym != 0) {
            return -1;
        }
        uint64_t target_off = 0;
        if (elf_vaddr_to_file_offset(image, image_size, r_offset, 8, &target_off) != 0 ||
            r_addend > UINT64_MAX - load_bias)
        {
            return -1;
        }
        wr64(image + target_off, load_bias + r_addend);
    }
    return 0;
}

static int apply_elf_relative_relocations_to_window(
    const unsigned char *image,
    uint64_t image_size,
    unsigned char *window,
    uint64_t window_size,
    uint64_t min_vaddr,
    uint64_t load_bias)
{
    (void)image_size;
    if (image == NULL || window == NULL || load_bias == 0) {
        return 0;
    }
    const uint64_t e_phoff = rd64(image + 32);
    const uint16_t e_phentsize = rd16(image + 54);
    const uint16_t e_phnum = rd16(image + 56);
    uint64_t dynamic_vaddr = 0;
    uint64_t dynamic_size = 0;
    for (uint16_t i = 0; i < e_phnum; i++) {
        const unsigned char *ph = image + e_phoff + (uint64_t)i * e_phentsize;
        if (rd32(ph + 0) == STORAGE_BOOT_ELF_PT_DYNAMIC) {
            dynamic_vaddr = rd64(ph + 16);
            dynamic_size = rd64(ph + 40);
            break;
        }
    }
    if (dynamic_vaddr == 0 || dynamic_size < 16 || dynamic_vaddr < min_vaddr) {
        return 0;
    }
    uint64_t dyn_base = dynamic_vaddr - min_vaddr;
    if (dyn_base > window_size || dynamic_size > window_size - dyn_base) {
        return -1;
    }

    uint64_t rela_vaddr = 0;
    uint64_t rela_size = 0;
    uint64_t rela_ent = STORAGE_BOOT_ELF_RELA_BYTES;
    uint64_t rela_count = 0;
    for (uint64_t cursor = 0; cursor + 16 <= dynamic_size; cursor += 16) {
        const uint64_t dyn_off = dyn_base + cursor;
        const uint64_t tag = rd64(window + dyn_off);
        const uint64_t value = rd64(window + dyn_off + 8);
        if (tag == STORAGE_BOOT_ELF_DT_NULL) {
            break;
        }
        switch (tag) {
        case STORAGE_BOOT_ELF_DT_RELA:
            rela_vaddr = value;
            break;
        case STORAGE_BOOT_ELF_DT_RELASZ:
            rela_size = value;
            break;
        case STORAGE_BOOT_ELF_DT_RELAENT:
            rela_ent = value;
            break;
        case STORAGE_BOOT_ELF_DT_RELACOUNT:
            rela_count = value;
            break;
        default:
            break;
        }
    }
    if (rela_vaddr == 0 || rela_size == 0) {
        return 0;
    }
    if (rela_ent != STORAGE_BOOT_ELF_RELA_BYTES || (rela_size % rela_ent) != 0 || rela_vaddr < min_vaddr) {
        return -1;
    }
    uint64_t rela_base = rela_vaddr - min_vaddr;
    if (rela_base > window_size || rela_size > window_size - rela_base) {
        return -1;
    }
    uint64_t total = rela_size / rela_ent;
    if (rela_count != 0) {
        if (rela_count > total) {
            return -1;
        }
        total = rela_count;
    }
    for (uint64_t i = 0; i < total; i++) {
        const uint64_t rela_off = rela_base + i * rela_ent;
        const uint64_t r_offset = rd64(window + rela_off);
        const uint64_t r_info = rd64(window + rela_off + 8);
        const uint64_t r_addend = rd64(window + rela_off + 16);
        const uint32_t r_type = (uint32_t)(r_info & 0xffffffffu);
        const uint64_t r_sym = r_info >> 32;
        if (r_type != STORAGE_BOOT_ELF_R_X86_64_RELATIVE || r_sym != 0 ||
            r_offset < min_vaddr || r_addend > UINT64_MAX - load_bias)
        {
            return -1;
        }
        const uint64_t target_off = r_offset - min_vaddr;
        if (target_off > window_size || 8 > window_size - target_off) {
            return -1;
        }
        wr64(window + target_off, load_bias + r_addend);
    }
    return 0;
}

static int load_elf_process_single_window(
    const char *path,
    const unsigned char *image,
    uint64_t image_size,
    struct storage_boot_loaded_process *out)
{
    (void)path;
    const uint64_t e_entry = rd64(image + 24);
    const uint64_t e_phoff = rd64(image + 32);
    const uint16_t e_phentsize = rd16(image + 54);
    const uint16_t e_phnum = rd16(image + 56);
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    uint16_t load_count = 0;
    for (uint16_t i = 0; i < e_phnum; i++) {
        const unsigned char *ph = image + e_phoff + (uint64_t)i * e_phentsize;
        if (rd32(ph + 0) != STORAGE_BOOT_ELF_PT_LOAD) {
            continue;
        }
        const uint64_t p_vaddr = rd64(ph + 16);
        const uint64_t p_memsz = rd64(ph + 40);
        const uint64_t start = align_down(p_vaddr);
        uint64_t end = 0;
        if (p_memsz == 0 || p_vaddr > UINT64_MAX - p_memsz ||
            align_up(p_vaddr + p_memsz, &end) != 0) {
            return -1;
        }
        if (start < min_vaddr) min_vaddr = start;
        if (end > max_vaddr) max_vaddr = end;
        load_count++;
    }
    if (load_count == 0 || min_vaddr == UINT64_MAX || max_vaddr <= min_vaddr) {
        return -1;
    }
    const uint64_t window_size = max_vaddr - min_vaddr;
    const uint64_t vmo_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_MAP_EXEC;
    const int vmo_fd = pacha_vmo_create(window_size, vmo_rights, 0);
    if (vmo_fd < 16) {
        return -2;
    }
    unsigned char *window = pacha_mmap(vmo_fd, window_size, STORAGE_BOOT_PROT_READ | STORAGE_BOOT_PROT_WRITE, STORAGE_BOOT_MMAP_SHARED, 0);
    if (window == NULL) {
        (void)pacha_fd_close(vmo_fd);
        return -3;
    }
    memset(window, 0, (size_t)window_size);
    for (uint16_t i = 0; i < e_phnum; i++) {
        const unsigned char *ph = image + e_phoff + (uint64_t)i * e_phentsize;
        if (rd32(ph + 0) != STORAGE_BOOT_ELF_PT_LOAD) {
            continue;
        }
        const uint64_t p_offset = rd64(ph + 8);
        const uint64_t p_vaddr = rd64(ph + 16);
        const uint64_t p_filesz = rd64(ph + 32);
        const uint64_t dst_off = p_vaddr - min_vaddr;
        if (p_offset > image_size || p_filesz > image_size - p_offset ||
            dst_off > window_size || p_filesz > window_size - dst_off) {
            (void)pacha_munmap(window, window_size);
            (void)pacha_fd_close(vmo_fd);
            return -4;
        }
        memcpy(window + dst_off, image + p_offset, (size_t)p_filesz);
    }
    if (window_size > STORAGE_BOOT_USER_ASLR_END_VA - STORAGE_BOOT_USER_ASLR_BASE_VA) {
        (void)pacha_munmap(window, window_size);
        (void)pacha_fd_close(vmo_fd);
        return -5;
    }
    const uint64_t target_base = 0x20000000ull;
    const uint64_t load_bias = target_base - min_vaddr;
    const int reloc_status = apply_elf_relative_relocations_to_window(
        image,
        image_size,
        window,
        window_size,
        min_vaddr,
        load_bias);
    if (reloc_status != 0) {
        (void)pacha_munmap(window, window_size);
        (void)pacha_fd_close(vmo_fd);
        return -6;
    }
    for (uint16_t i = 0; i < e_phnum; i++) {
        const unsigned char *ph = image + e_phoff + (uint64_t)i * e_phentsize;
        if (rd32(ph + 0) != STORAGE_BOOT_ELF_PT_LOAD) {
            continue;
        }
        const uint32_t p_flags = rd32(ph + 4);
        const uint64_t p_vaddr = rd64(ph + 16);
        const uint64_t p_memsz = rd64(ph + 40);
        const uint64_t page_vaddr = align_down(p_vaddr);
        const uint64_t page_offset = p_vaddr - page_vaddr;
        uint64_t map_size = 0;
        if (p_memsz == 0 || align_up(page_offset + p_memsz, &map_size) != 0) {
            (void)pacha_munmap(window, window_size);
            (void)pacha_fd_close(vmo_fd);
            return -7;
        }
        const uint64_t prot = prot_from_elf_flags(p_flags);
        if ((prot & (STORAGE_BOOT_PROT_WRITE | STORAGE_BOOT_PROT_EXEC)) ==
            (STORAGE_BOOT_PROT_WRITE | STORAGE_BOOT_PROT_EXEC)) {
            (void)pacha_munmap(window, window_size);
            (void)pacha_fd_close(vmo_fd);
            return -7;
        }
        const uint64_t vmo_offset = page_vaddr - min_vaddr;
        const long map_result = pacha_process_map(
            out->process_fd,
            vmo_fd,
            target_base + vmo_offset,
            map_size,
            prot,
            vmo_offset);
        if (map_result < 4096) {
            (void)pacha_munmap(window, window_size);
            (void)pacha_fd_close(vmo_fd);
            return -7;
        }
    }
    (void)pacha_munmap(window, window_size);
    (void)pacha_fd_close(vmo_fd);
    out->runtime_entry = e_entry + load_bias;
    out->load_bias = load_bias;
    out->phdr_va = load_bias + e_phoff;
    out->phent = e_phentsize;
    out->phnum = e_phnum;
    out->load_segments = load_count;
    return 0;
}

static int load_elf_process(
    const char *path,
    const unsigned char *image,
    uint64_t image_size,
    const struct pacha_process_fd_grant *grants,
    uint64_t grant_count,
    struct storage_boot_loaded_process *out)
{
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->process_fd = -1;
    int status = validate_elf_header(path, image, image_size);
    if (status != 0) {
        return status;
    }

    const uint64_t e_entry = rd64(image + 24);
    const uint16_t e_type = rd16(image + 16);
    const uint64_t e_phoff = rd64(image + 32);
    const uint16_t e_phentsize = rd16(image + 54);
    const uint16_t e_phnum = rd16(image + 56);
    uint64_t load_bias = 0;
    const int use_aslr = e_type == STORAGE_BOOT_ELF_TYPE_DYN;
    const uint64_t process_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_KILL |
        PACHA_FD_RIGHT_SPAWN |
        PACHA_FD_RIGHT_MAP_INTO |
        PACHA_FD_RIGHT_SET_CONTEXT;
    const int process_fd = pacha_process_create(process_rights, 0, grants, grant_count);
    if (process_fd < 16) {
        fprintf(stderr, "[storage_boot] exec: process_create failed status=%d\n", process_fd);
        for (uint64_t i = 0; i < grant_count; ++i) {
            struct pacha_fd_info info;
            memset(&info, 0, sizeof(info));
            const int info_status = pacha_fd_get_info((int)grants[i].source_fd, &info);
            fprintf(stderr,
                "[storage_boot] exec: grant[%llu] source=%llu target=%llu info=%d kind=%llu actual=0x%llx requested=0x%llx flags=0x%llx\n",
                (unsigned long long)i,
                (unsigned long long)grants[i].source_fd,
                (unsigned long long)grants[i].target_fd,
                info_status,
                (unsigned long long)info.kind,
                (unsigned long long)info.rights,
                (unsigned long long)grants[i].rights,
                (unsigned long long)grants[i].flags);
        }
        return -7;
    }
    out->process_fd = process_fd;

    if (use_aslr) {
        status = load_elf_process_single_window(path, image, image_size, out);
        if (status != 0) {
            fprintf(stderr, "[storage_boot] exec: %s single-window load failed status=%d\n", path, status);
            (void)pacha_fd_close(process_fd);
            memset(out, 0, sizeof(*out));
            out->process_fd = -1;
            return status;
        }
        return 0;
    }

    uint16_t load_count = 0;
    int relocated = 0;
    for (uint16_t i = 0; i < e_phnum; i++) {
        const unsigned char *ph = image + e_phoff + (uint64_t)i * e_phentsize;
        if (rd32(ph + 0) != STORAGE_BOOT_ELF_PT_LOAD) {
            continue;
        }
        const uint64_t p_vaddr = rd64(ph + 16);
        const uint64_t requested_va = (use_aslr && load_count == 0) ? PACHA_PROCESS_MAP_ANYWHERE : align_down(p_vaddr + load_bias);
        uint64_t mapped_va = 0;
        status = map_elf_segment(path, process_fd, requested_va, image, image_size, ph, i, &mapped_va);
        if (status != 0) {
            (void)pacha_fd_close(process_fd);
            return status;
        }
        if (use_aslr && load_count == 0) {
            load_bias = mapped_va - align_down(p_vaddr);
            status = apply_elf_relative_relocations((unsigned char *)image, image_size, load_bias);
            if (status != 0) {
                fprintf(stderr, "[storage_boot] exec: %s relocation failed status=%d\n", path, status);
                (void)pacha_fd_close(process_fd);
                return -8;
            }
            relocated = 1;
        }
        load_count++;
    }
    if (load_count == 0) {
        fprintf(stderr, "[storage_boot] exec: %s has no PT_LOAD segments\n", path);
        (void)pacha_fd_close(process_fd);
        return -6;
    }
    if (!relocated) {
        status = apply_elf_relative_relocations((unsigned char *)image, image_size, load_bias);
        if (status != 0) {
            fprintf(stderr, "[storage_boot] exec: %s relocation failed status=%d\n", path, status);
            (void)pacha_fd_close(process_fd);
            return -8;
        }
    }

    out->runtime_entry = e_entry + load_bias;
    out->load_bias = load_bias;
    out->phdr_va = load_bias + e_phoff;
    out->phent = e_phentsize;
    out->phnum = e_phnum;
    out->load_segments = load_count;
    return 0;
}

static int push_u64(unsigned char *stack, uint64_t *sp, uint64_t value)
{
    if (*sp < 8) {
        return -1;
    }
    *sp -= 8;
    wr64(stack + *sp, value);
    return 0;
}

static int start_loaded_process(
    const struct storage_boot_loaded_process *loaded,
    const char *argv0,
    int bootstrap_fd)
{
    if (loaded == NULL || loaded->process_fd < 16 || loaded->runtime_entry == 0 ||
        loaded->phdr_va == 0 || loaded->phent == 0 || loaded->phnum == 0 || argv0 == NULL) {
        return -1;
    }

    const int process_fd = loaded->process_fd;
    const uint64_t stack_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int stack_fd = pacha_vmo_create(PACHA_PROCESS_DEFAULT_STACK_SIZE, stack_rights, 0);
    if (stack_fd < 16) {
        fprintf(stderr, "[storage_boot] exec: stack vmo_create failed status=%d\n", stack_fd);
        return -2;
    }
    unsigned char *stack = pacha_mmap(
        stack_fd,
        PACHA_PROCESS_DEFAULT_STACK_SIZE,
        STORAGE_BOOT_PROT_READ | STORAGE_BOOT_PROT_WRITE,
        STORAGE_BOOT_MMAP_SHARED,
        0);
    if (stack == NULL) {
        (void)pacha_fd_close(stack_fd);
        return -3;
    }
    memset(stack, 0, (size_t)PACHA_PROCESS_DEFAULT_STACK_SIZE);
    const long stack_map = pacha_process_map(
        process_fd,
        stack_fd,
        PACHA_PROCESS_MAP_ANYWHERE,
        PACHA_PROCESS_DEFAULT_STACK_SIZE,
        STORAGE_BOOT_PROT_READ | STORAGE_BOOT_PROT_WRITE,
        0);
    if (stack_map < 4096) {
        fprintf(stderr, "[storage_boot] exec: stack process_map failed status=%ld\n", stack_map);
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -6;
    }
    const uint64_t stack_base = (uint64_t)stack_map;

    uint64_t sp = PACHA_PROCESS_DEFAULT_STACK_SIZE;
    const uint64_t argv0_len = (uint64_t)strlen(argv0) + 1;
    if (argv0_len > 256 || sp < argv0_len + 16) {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -4;
    }
    sp -= argv0_len;
    memcpy(stack + sp, argv0, (size_t)argv0_len);
    const uint64_t argv0_va = stack_base + sp;
    sp &= ~15ull;
    sp -= 16;
    const uint64_t random_va = stack_base + sp;
    if (pacha_getrandom(stack + sp, 16, 0) != 16) {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -5;
    }
    sp &= ~15ull;

    if (push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_NULL) != 0 ||
        (bootstrap_fd >= 16 && (push_u64(stack, &sp, (uint64_t)(uint32_t)bootstrap_fd) != 0 || push_u64(stack, &sp, PACHA_AT_BOOTSTRAP_FD) != 0)) ||
        push_u64(stack, &sp, argv0_va) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_EXECFN) != 0 ||
        push_u64(stack, &sp, random_va) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_RANDOM) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_SECURE) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_EGID) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_GID) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_EUID) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_UID) != 0 ||
        push_u64(stack, &sp, loaded->runtime_entry) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_ENTRY) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_BASE) != 0 ||
        push_u64(stack, &sp, STORAGE_BOOT_PAGE_SIZE) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_PAGESZ) != 0 ||
        push_u64(stack, &sp, loaded->phnum) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_PHNUM) != 0 ||
        push_u64(stack, &sp, loaded->phent) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_PHENT) != 0 ||
        push_u64(stack, &sp, loaded->phdr_va) != 0 || push_u64(stack, &sp, STORAGE_BOOT_AT_PHDR) != 0 ||
        push_u64(stack, &sp, 0) != 0 ||
        push_u64(stack, &sp, 0) != 0 ||
        push_u64(stack, &sp, argv0_va) != 0 ||
        push_u64(stack, &sp, 1) != 0) {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -5;
    }

    (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
    (void)pacha_fd_close(stack_fd);

    const uint64_t thread_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_KILL |
        PACHA_FD_RIGHT_START |
        PACHA_FD_RIGHT_SET_CONTEXT;
    const int thread_fd = pacha_thread_create(process_fd, loaded->runtime_entry, stack_base + sp, 0, 0, thread_rights);
    if (thread_fd < 16) {
        fprintf(stderr, "[storage_boot] exec: thread_create failed status=%d\n", thread_fd);
        return -7;
    }
    const int start_status = pacha_thread_start(thread_fd);
    if (start_status != 0) {
        fprintf(stderr, "[storage_boot] exec: thread_start failed thread_fd=%d status=%d\n", thread_fd, start_status);
        (void)pacha_fd_close(thread_fd);
        return -8;
    }
    return 0;
}

static int launch_seed0root(const struct storage_boot_config *boot_config,
    const struct ph_bootfs *archive, const uint8_t root_uuid[16],
    const uint64_t storage_policy[3])
{
    const unsigned char *image = NULL, *filed_image = NULL;
    size_t image_size = 0, filed_image_size = 0;
    int status = ph_bootfs_find(archive, "/sbin/seed0root.elf", &image, &image_size);
    if (!status) status = ph_bootfs_find(archive, "/sbin/filed.elf", &filed_image, &filed_image_size);
    if (status || !image_size || !filed_image_size) return status ? status : -22;
    const int filed_image_fd = create_inherited_vmo_from_bytes(filed_image, filed_image_size, "filed.elf");
    if (filed_image_fd < 16) return filed_image_fd;
    const uint64_t device_fd = boot_config->device_fd;
    const uint64_t ready_channel_fd = boot_config->ready_channel_fd;
    const uint64_t service_ready_channel_fd = boot_config->root_handoff_channel_fd;
    const uint64_t power_channel_fd = boot_config->power_channel_fd;
    const storage_seed0root_bootstrap_t bootstrap = {
        .magic = STORAGE_SEED0ROOT_BOOTSTRAP_MAGIC,
        .device_fd = device_fd,
        .ready_channel_fd = ready_channel_fd,
        .service_ready_channel_fd = service_ready_channel_fd,
        .filed_image_fd = (uint64_t)(uint32_t)filed_image_fd,
        .filed_image_size = filed_image_size,
        .bootfs_fd = boot_config->bootfs_fd,
        .bootfs_size = boot_config->bootfs_size,
        .sandbox_ram_bytes = storage_policy[0],
        .queue_size = storage_policy[1],
        .queue_arena_bytes = storage_policy[2],
        .power_channel_fd = power_channel_fd,
        .framebuffer_paddr = boot_config->framebuffer_paddr,
        .framebuffer_size = boot_config->framebuffer_size,
        .framebuffer_width = boot_config->framebuffer_width,
        .framebuffer_height = boot_config->framebuffer_height,
        .framebuffer_pitch = boot_config->framebuffer_pitch,
    };
    storage_seed0root_bootstrap_t bootstrap_package = bootstrap;
    memcpy(bootstrap_package.root_uuid, root_uuid, 16);
    const int bootstrap_fd = create_inherited_vmo_from_bytes(&bootstrap_package, sizeof(bootstrap_package), "seed0root bootstrap fd");
    if (bootstrap_fd < 16) {
        (void)pacha_fd_close(filed_image_fd);
        return 32;
    }

    struct storage_boot_loaded_process loaded;
    struct pacha_process_fd_grant grants[9] = {
        PACHA_LAUNCH_LOG_GRANTS(PACHA_FD_RIGHT_TRANSFER),
        PACHA_LAUNCH_GRANT(device_fd, device_fd,
            PACHA_LAUNCH_DEVICE_DRIVER | PACHA_FD_RIGHT_TRANSFER),
        PACHA_LAUNCH_GRANT(ready_channel_fd, ready_channel_fd, PACHA_LAUNCH_SIGNAL),
        PACHA_LAUNCH_GRANT(service_ready_channel_fd, service_ready_channel_fd,
            PACHA_LAUNCH_SERVER | PACHA_LAUNCH_SIGNAL),
        PACHA_LAUNCH_GRANT(filed_image_fd, filed_image_fd, PACHA_LAUNCH_BLOB),
        PACHA_LAUNCH_GRANT(bootstrap_fd, bootstrap_fd, PACHA_LAUNCH_BLOB),
        PACHA_LAUNCH_GRANT(power_channel_fd, power_channel_fd,
            PACHA_LAUNCH_CLIENT | PACHA_FD_RIGHT_TRANSFER),
        PACHA_LAUNCH_GRANT(boot_config->bootfs_fd, boot_config->bootfs_fd,
            PACHA_LAUNCH_BLOB | PACHA_FD_RIGHT_TRANSFER),
    };
    status = load_elf_process(storage_boot_seed0root_argv0, image, image_size,
        grants, 9, &loaded);
    if (status != 0) {
        (void)pacha_fd_close(bootstrap_fd);
        (void)pacha_fd_close(filed_image_fd);
        fprintf(stderr, "[storage_boot] seed0root: ELF load failed status=%d\n", status);
        return 25;
    }
    status = start_loaded_process(&loaded, storage_boot_seed0root_argv0, bootstrap_fd);
    (void)pacha_fd_close(bootstrap_fd);
    (void)pacha_fd_close(filed_image_fd);
    if (status != 0) {
        fprintf(stderr, "[storage_boot] seed0root: start failed status=%d\n", status);
        return 26;
    }
    printf("[storage_boot] seed0root started\n");
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    int bootstrap_fd = -1;
    if (find_bootstrap_fd(argv, &bootstrap_fd)) return 2;
    struct storage_boot_config config = {0};
    int error = read_fd_exact(bootstrap_fd, &config, sizeof(config), "boot config");
    (void)pacha_fd_close(bootstrap_fd);
    if (error || config.magic != STORAGE_BOOT_CONFIG_MAGIC ||
        config.size != sizeof(config) || config.device_fd < 16 ||
        config.ready_channel_fd < 16 || config.root_handoff_channel_fd < 16 ||
        config.power_channel_fd < 16 || config.bootfs_fd < 16) return 2;
    struct ph_bootfs archive = {0};
    error = ph_bootfs_open(&archive, (int)config.bootfs_fd, config.bootfs_size);
    const unsigned char *bytes = NULL;
    size_t size = 0;
    uint8_t root_uuid[16];
    uint64_t policy[3] = {0};
    if (!error) error = ph_bootfs_find(&archive, "/etc/pacha/root-uuid", &bytes, &size);
    if (!error && size == sizeof(root_uuid)) memcpy(root_uuid, bytes, sizeof(root_uuid));
    else if (!error) error = -22;
    if (!error && !storage_root_uuid_valid(root_uuid)) error = -22;
    if (!error) error = ph_bootfs_find(&archive, "/etc/pacha/storage-policy", &bytes, &size);
    if (!error && size != 24) error = -22;
    if (!error) {
        for (unsigned i = 0; i < 3; ++i) policy[i] = rd64(bytes + i * 8);
        if (!policy[0] || policy[1] < 2 || policy[1] > 32768 ||
            (policy[1] & (policy[1] - 1)) || !policy[2]) error = -22;
    }
    /* Linux selects the UUID among the authorized PCI descendants. The
     * bootstrap loader neither opens a disk nor guesses a GPT partition. */
    if (!error) error = launch_seed0root(&config, &archive, root_uuid, policy);
    int closed = ph_bootfs_close(&archive);
    (void)pacha_fd_close((int)config.bootfs_fd);
    if (!error) error = closed;
    if (error) fprintf(stderr, "[storage_boot] bootfs launch failed status=%d\n", error);
    else puts("[storage_boot] bootfs root launcher done");
    return error ? 3 : 0;
}
