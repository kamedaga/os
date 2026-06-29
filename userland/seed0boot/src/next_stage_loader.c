#include "next_stage_loader.h"

#include "pacha/ipc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    SEED0_PAGE_SIZE = 4096,
    SEED0_ELF64_EHDR_BYTES = 64,
    SEED0_ELF64_PHDR_BYTES = 56,
    SEED0_ELF_CLASS_64 = 2,
    SEED0_ELF_DATA_LSB = 1,
    SEED0_ELF_VERSION_CURRENT = 1,
    SEED0_ELF_TYPE_EXEC = 2,
    SEED0_ELF_TYPE_DYN = 3,
    SEED0_ELF_MACHINE_X86_64 = 0x3e,
    SEED0_ELF_PT_LOAD = 1,
    SEED0_ELF_PF_X = 1,
    SEED0_ELF_PF_W = 2,
    SEED0_ELF_PF_R = 4,
    SEED0_AT_NULL = 0,
    SEED0_AT_PHDR = 3,
    SEED0_AT_PHENT = 4,
    SEED0_AT_PHNUM = 5,
    SEED0_AT_PAGESZ = 6,
    SEED0_AT_BASE = 7,
    SEED0_AT_ENTRY = 9,
    SEED0_AT_UID = 11,
    SEED0_AT_EUID = 12,
    SEED0_AT_GID = 13,
    SEED0_AT_EGID = 14,
    SEED0_AT_SECURE = 23,
    SEED0_AT_RANDOM = 25,
    SEED0_AT_EXECFN = 31,
};

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

static uint64_t align_down(uint64_t value)
{
    return value & ~(uint64_t)(SEED0_PAGE_SIZE - 1);
}

static int align_up(uint64_t value, uint64_t *out)
{
    if (value > UINT64_MAX - (SEED0_PAGE_SIZE - 1)) {
        return -1;
    }
    *out = (value + (SEED0_PAGE_SIZE - 1)) & ~(uint64_t)(SEED0_PAGE_SIZE - 1);
    return 0;
}

static uint64_t prot_from_elf_flags(uint32_t flags)
{
    uint64_t prot = 0;
    if ((flags & SEED0_ELF_PF_R) != 0) {
        prot |= PACHA_PROT_READ;
    }
    if ((flags & SEED0_ELF_PF_W) != 0) {
        prot |= PACHA_PROT_WRITE;
    }
    if ((flags & SEED0_ELF_PF_X) != 0) {
        prot |= PACHA_PROT_EXEC;
    }
    return prot;
}

static void wr64(unsigned char *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) {
        p[i] = (unsigned char)(value >> (i * 8));
    }
}

static int validate_elf_header(const char *path, const unsigned char *image, uint32_t image_size)
{
    if (image == 0 || image_size < SEED0_ELF64_EHDR_BYTES) {
        return -1;
    }
    if (image[0] != 0x7f || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') {
        fprintf(stderr, "[seed0boot] next-stage: %s ELF magic invalid\n", path);
        return -2;
    }
    if (image[4] != SEED0_ELF_CLASS_64 || image[5] != SEED0_ELF_DATA_LSB ||
        image[6] != SEED0_ELF_VERSION_CURRENT) {
        fprintf(stderr,
            "[seed0boot] next-stage: %s unsupported ELF ident class=%u data=%u version=%u\n",
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
    if ((e_type != SEED0_ELF_TYPE_EXEC && e_type != SEED0_ELF_TYPE_DYN) ||
        e_machine != SEED0_ELF_MACHINE_X86_64 ||
        e_version != SEED0_ELF_VERSION_CURRENT ||
        e_phentsize < SEED0_ELF64_PHDR_BYTES ||
        e_phnum == 0) {
        fprintf(stderr,
            "[seed0boot] next-stage: %s unsupported ELF type=%u machine=%04x version=%u phentsize=%u phnum=%u\n",
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
    if (e_phoff > image_size || phdr_bytes > (uint64_t)image_size - e_phoff) {
        fprintf(stderr, "[seed0boot] next-stage: %s program headers out of range\n", path);
        return -5;
    }
    return 0;
}

static int stage_load_segment(
    const char *path,
    int process_fd,
    uint64_t target_va,
    const unsigned char *image,
    uint32_t image_size,
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
        p_filesz > (uint64_t)image_size - p_offset ||
        (p_memsz != 0 && p_vaddr > UINT64_MAX - p_memsz)) {
        fprintf(stderr, "[seed0boot] next-stage: %s invalid PT_LOAD[%u]\n", path, index);
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
        fprintf(stderr, "[seed0boot] next-stage: vmo_create failed segment=%u status=%d\n", index, vmo_fd);
        return -3;
    }
    unsigned char *mapped = pacha_mmap(vmo_fd, map_size, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (mapped == 0) {
        fprintf(stderr, "[seed0boot] next-stage: mmap staging VMO failed segment=%u fd=%d\n", index, vmo_fd);
        (void)pacha_fd_close(vmo_fd);
        return -4;
    }

    memset(mapped, 0, (size_t)map_size);
    memcpy(mapped + page_offset, image + p_offset, (size_t)p_filesz);

    const uint64_t prot = prot_from_elf_flags(p_flags);
    const long map_status = pacha_process_map(process_fd, vmo_fd, target_va, map_size, prot, 0);
    if (map_status < 4096) {
        fprintf(stderr,
            "[seed0boot] next-stage: process_map failed segment=%u process_fd=%d vmo_fd=%d status=%ld\n",
            index,
            process_fd,
            vmo_fd,
            map_status);
        (void)pacha_munmap(mapped, map_size);
        (void)pacha_fd_close(vmo_fd);
        return -5;
    }

    (void)pacha_munmap(mapped, map_size);
    (void)pacha_fd_close(vmo_fd);
    if (out_mapped_va != 0) {
        *out_mapped_va = (uint64_t)map_status;
    }

    (void)index;
    (void)process_fd;
    (void)prot;
    (void)p_filesz;
    (void)p_memsz;
    return 0;
}

int seed0_map_bytes_into_process(
    int process_fd,
    uint64_t target_va,
    const void *data,
    uint64_t size,
    uint64_t prot)
{
    if (process_fd < 16 || target_va == 0 || data == 0 || size == 0) {
        return -1;
    }
    uint64_t map_size = 0;
    if (align_up(size, &map_size) != 0) {
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
        return -3;
    }
    unsigned char *mapped = pacha_mmap(vmo_fd, map_size, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (mapped == 0) {
        (void)pacha_fd_close(vmo_fd);
        return -4;
    }
    memset(mapped, 0, (size_t)map_size);
    memcpy(mapped, data, (size_t)size);
    const long map_status = pacha_process_map(process_fd, vmo_fd, target_va, map_size, prot, 0);
    (void)pacha_munmap(mapped, map_size);
    (void)pacha_fd_close(vmo_fd);
    return map_status >= 4096 ? 0 : -5;
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

int seed0_start_process(const struct seed0_loaded_process *loaded, const char *argv0, int bootstrap_fd)
{
    if (loaded == 0 || loaded->process_fd < 16 || loaded->runtime_entry == 0 ||
        loaded->phdr_va == 0 || loaded->phent == 0 || loaded->phnum == 0 || argv0 == 0) {
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
        fprintf(stderr, "[seed0boot] next-stage: stack vmo_create failed status=%d\n", stack_fd);
        return -2;
    }
    unsigned char *stack = pacha_mmap(
        stack_fd,
        PACHA_PROCESS_DEFAULT_STACK_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (stack == 0) {
        (void)pacha_fd_close(stack_fd);
        return -3;
    }
    memset(stack, 0, (size_t)PACHA_PROCESS_DEFAULT_STACK_SIZE);
    const long stack_map = pacha_process_map(
        process_fd,
        stack_fd,
        PACHA_PROCESS_MAP_ANYWHERE,
        PACHA_PROCESS_DEFAULT_STACK_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        0);
    if (stack_map < 4096) {
        fprintf(stderr, "[seed0boot] next-stage: stack process_map failed status=%ld\n", stack_map);
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

    if (push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, SEED0_AT_NULL) != 0 ||
        (bootstrap_fd >= 16 && (push_u64(stack, &sp, (uint64_t)(uint32_t)bootstrap_fd) != 0 || push_u64(stack, &sp, PACHA_AT_BOOTSTRAP_FD) != 0)) ||
        push_u64(stack, &sp, argv0_va) != 0 || push_u64(stack, &sp, SEED0_AT_EXECFN) != 0 ||
        push_u64(stack, &sp, random_va) != 0 || push_u64(stack, &sp, SEED0_AT_RANDOM) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, SEED0_AT_SECURE) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, SEED0_AT_EGID) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, SEED0_AT_GID) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, SEED0_AT_EUID) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, SEED0_AT_UID) != 0 ||
        push_u64(stack, &sp, loaded->runtime_entry) != 0 || push_u64(stack, &sp, SEED0_AT_ENTRY) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, SEED0_AT_BASE) != 0 ||
        push_u64(stack, &sp, SEED0_PAGE_SIZE) != 0 || push_u64(stack, &sp, SEED0_AT_PAGESZ) != 0 ||
        push_u64(stack, &sp, loaded->phnum) != 0 || push_u64(stack, &sp, SEED0_AT_PHNUM) != 0 ||
        push_u64(stack, &sp, loaded->phent) != 0 || push_u64(stack, &sp, SEED0_AT_PHENT) != 0 ||
        push_u64(stack, &sp, loaded->phdr_va) != 0 || push_u64(stack, &sp, SEED0_AT_PHDR) != 0 ||
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
        fprintf(stderr, "[seed0boot] next-stage: thread_create failed status=%d\n", thread_fd);
        return -7;
    }
    const int start_status = pacha_thread_start(thread_fd);
    if (start_status != 0) {
        fprintf(stderr, "[seed0boot] next-stage: thread_start failed thread_fd=%d status=%d\n", thread_fd, start_status);
        (void)pacha_fd_close(thread_fd);
        return -8;
    }
    (void)thread_fd;
    printf("[seed0boot] next-stage started entry=0x%llx\n",
        (unsigned long long)loaded->runtime_entry);
    return 0;
}

int seed0_load_elf_process(
    const char *path,
    const unsigned char *image,
    uint32_t image_size,
    struct seed0_loaded_process *out)
{
    if (out == 0) {
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
    const int use_aslr = e_type == SEED0_ELF_TYPE_DYN;
    const uint64_t process_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_KILL |
        PACHA_FD_RIGHT_SPAWN |
        PACHA_FD_RIGHT_MAP_INTO |
        PACHA_FD_RIGHT_SET_CONTEXT;
    const int process_fd = pacha_process_create(process_rights, 0);
    if (process_fd < 16) {
        fprintf(stderr, "[seed0boot] next-stage: process_create failed status=%d\n", process_fd);
        return -7;
    }

    uint16_t load_count = 0;
    for (uint16_t i = 0; i < e_phnum; i++) {
        const unsigned char *ph = image + e_phoff + (uint64_t)i * e_phentsize;
        if (rd32(ph + 0) != SEED0_ELF_PT_LOAD) {
            continue;
        }
        const uint64_t p_vaddr = rd64(ph + 16);
        const uint64_t requested_va = (use_aslr && load_count == 0) ? PACHA_PROCESS_MAP_ANYWHERE : align_down(p_vaddr + load_bias);
        uint64_t mapped_va = 0;
        status = stage_load_segment(path, process_fd, requested_va, image, image_size, ph, i, &mapped_va);
        if (status != 0) {
            (void)pacha_fd_close(process_fd);
            return status;
        }
        if (use_aslr && load_count == 0) {
            load_bias = mapped_va - align_down(p_vaddr);
        }
        load_count++;
    }
    if (load_count == 0) {
        fprintf(stderr, "[seed0boot] next-stage: %s has no PT_LOAD segments\n", path);
        (void)pacha_fd_close(process_fd);
        return -6;
    }

    out->process_fd = process_fd;
    out->runtime_entry = e_entry + load_bias;
    out->load_bias = load_bias;
    out->phdr_va = load_bias + e_phoff;
    out->phent = e_phentsize;
    out->phnum = e_phnum;
    out->load_segments = load_count;
    printf("[seed0boot] next-stage loaded path=%s segments=%u\n",
        path,
        load_count);
    return 0;
}

int seed0_stage_next_elf(const char *path, const unsigned char *image, uint32_t image_size)
{
    struct seed0_loaded_process loaded;
    int status = seed0_load_elf_process(path, image, image_size, &loaded);
    if (status != 0) return status;
    return seed0_start_process(&loaded, path, -1);
}
