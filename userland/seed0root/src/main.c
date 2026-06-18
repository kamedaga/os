#include "pacha/ipc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    SEED0ROOT_BOOTSTRAP_MAGIC = 0x305254424f4f5453ull,
    SEED0ROOT_BOOTSTRAP_VERSION = 1,
    SEED0ROOT_BOOTSTRAP_VA = 0x3e000000ull,
    SEED0ROOT_BOOTSTRAP_MAX_MODULES = 8,
    SEED0ROOT_BOOTSTRAP_NAME_BYTES = 64,
    SEED0ROOT_KOBOXD_BOOTSTRAP_MAGIC = 0x3150474b42584f4bull,
    SEED0ROOT_KOBOXD_BOOTSTRAP_VERSION = 1,
    SEED0ROOT_KOBOXD_BOOTSTRAP_VA = 0x3f000000ull,
    SEED0ROOT_KOBOXD_MODULE_TABLE_VA = 0x3f010000ull,
    SEED0ROOT_KOBOXD_MODULE_IMAGE_BASE = 0x3f100000ull,
    SEED0ROOT_PAGE_SIZE = 4096,
    SEED0ROOT_ELF64_EHDR_BYTES = 64,
    SEED0ROOT_ELF64_PHDR_BYTES = 56,
    SEED0ROOT_ELF_CLASS_64 = 2,
    SEED0ROOT_ELF_DATA_LSB = 1,
    SEED0ROOT_ELF_VERSION_CURRENT = 1,
    SEED0ROOT_ELF_TYPE_EXEC = 2,
    SEED0ROOT_ELF_TYPE_DYN = 3,
    SEED0ROOT_ELF_MACHINE_X86_64 = 0x3e,
    SEED0ROOT_ELF_PT_LOAD = 1,
    SEED0ROOT_ELF_PF_X = 1,
    SEED0ROOT_ELF_PF_W = 2,
    SEED0ROOT_ELF_PF_R = 4,
    SEED0ROOT_CHILD_LOAD_BASE = 0x30000000ull,
    SEED0ROOT_CHILD_STACK_TOP = 0x70000000ull,
    SEED0ROOT_CHILD_STACK_SIZE = 0x20000ull,
    SEED0ROOT_AT_NULL = 0,
    SEED0ROOT_AT_PHDR = 3,
    SEED0ROOT_AT_PHENT = 4,
    SEED0ROOT_AT_PHNUM = 5,
    SEED0ROOT_AT_PAGESZ = 6,
    SEED0ROOT_AT_BASE = 7,
    SEED0ROOT_AT_RANDOM = 25,
    SEED0ROOT_AT_EXECFN = 31,
};

struct seed0root_bootstrap {
    uint64_t magic;
    uint64_t version;
    uint64_t device_fd;
    uint64_t koboxd_image_va;
    uint64_t koboxd_image_size;
    uint64_t module_count;
    uint64_t modules_va;
};

struct seed0root_bootstrap_module {
    char name[SEED0ROOT_BOOTSTRAP_NAME_BYTES];
    uint64_t image_va;
    uint64_t image_size;
};

struct seed0root_koboxd_bootstrap {
    uint64_t magic;
    uint64_t version;
    uint64_t device_fd;
    uint64_t module_count;
    uint64_t modules_va;
};

struct seed0root_loaded_process {
    int process_fd;
    uint64_t runtime_entry;
    uint64_t phdr_va;
    uint64_t phent;
    uint64_t phnum;
    uint16_t load_segments;
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

static void wr64(unsigned char *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) {
        p[i] = (unsigned char)(value >> (i * 8));
    }
}

static uint64_t align_down(uint64_t value)
{
    return value & ~(uint64_t)(SEED0ROOT_PAGE_SIZE - 1);
}

static int align_up(uint64_t value, uint64_t *out)
{
    if (value > UINT64_MAX - (SEED0ROOT_PAGE_SIZE - 1)) {
        return -1;
    }
    *out = (value + (SEED0ROOT_PAGE_SIZE - 1)) & ~(uint64_t)(SEED0ROOT_PAGE_SIZE - 1);
    return 0;
}

static uint64_t prot_from_elf_flags(uint32_t flags)
{
    uint64_t prot = 0;
    if ((flags & SEED0ROOT_ELF_PF_R) != 0) prot |= PACHA_PROT_READ;
    if ((flags & SEED0ROOT_ELF_PF_W) != 0) prot |= PACHA_PROT_WRITE;
    if ((flags & SEED0ROOT_ELF_PF_X) != 0) prot |= PACHA_PROT_EXEC;
    return prot;
}

static int validate_elf_header(const char *path, const unsigned char *image, uint64_t image_size)
{
    if (image == NULL || image_size < SEED0ROOT_ELF64_EHDR_BYTES) {
        return -1;
    }
    if (image[0] != 0x7f || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') {
        fprintf(stderr, "[seed0root] exec: %s ELF magic invalid\n", path);
        return -2;
    }
    if (image[4] != SEED0ROOT_ELF_CLASS_64 || image[5] != SEED0ROOT_ELF_DATA_LSB ||
        image[6] != SEED0ROOT_ELF_VERSION_CURRENT) {
        fprintf(stderr, "[seed0root] exec: %s unsupported ELF ident\n", path);
        return -3;
    }
    const uint16_t e_type = rd16(image + 16);
    const uint16_t e_machine = rd16(image + 18);
    const uint32_t e_version = rd32(image + 20);
    const uint16_t e_phentsize = rd16(image + 54);
    const uint16_t e_phnum = rd16(image + 56);
    if ((e_type != SEED0ROOT_ELF_TYPE_EXEC && e_type != SEED0ROOT_ELF_TYPE_DYN) ||
        e_machine != SEED0ROOT_ELF_MACHINE_X86_64 ||
        e_version != SEED0ROOT_ELF_VERSION_CURRENT ||
        e_phentsize < SEED0ROOT_ELF64_PHDR_BYTES ||
        e_phnum == 0) {
        fprintf(stderr, "[seed0root] exec: %s unsupported ELF type=%u machine=%04x phnum=%u\n",
            path,
            e_type,
            e_machine,
            e_phnum);
        return -4;
    }
    const uint64_t e_phoff = rd64(image + 32);
    const uint64_t phdr_bytes = (uint64_t)e_phentsize * e_phnum;
    if (e_phoff > image_size || phdr_bytes > image_size - e_phoff) {
        fprintf(stderr, "[seed0root] exec: %s program headers out of range\n", path);
        return -5;
    }
    return 0;
}

static int map_elf_segment(
    const char *path,
    int process_fd,
    uint64_t load_bias,
    const unsigned char *image,
    uint64_t image_size,
    const unsigned char *ph,
    uint16_t index)
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
        fprintf(stderr, "[seed0root] exec: %s invalid PT_LOAD[%u]\n", path, index);
        return -1;
    }
    if (p_memsz == 0) return 0;

    const uint64_t target_va = align_down(p_vaddr + load_bias);
    const uint64_t page_offset = (p_vaddr + load_bias) - target_va;
    uint64_t map_size = 0;
    if (align_up(page_offset + p_memsz, &map_size) != 0) return -2;

    const uint64_t vmo_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE |
        PACHA_FD_RIGHT_MAP_EXEC;
    const int vmo_fd = pacha_vmo_create(map_size, vmo_rights, 0);
    if (vmo_fd < 16) {
        fprintf(stderr, "[seed0root] exec: vmo_create failed segment=%u status=%d\n", index, vmo_fd);
        return -3;
    }
    unsigned char *mapped = pacha_mmap(vmo_fd, map_size, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (mapped == NULL) {
        (void)pacha_fd_close(vmo_fd);
        return -4;
    }
    memset(mapped, 0, (size_t)map_size);
    memcpy(mapped + page_offset, image + p_offset, (size_t)p_filesz);
    const int map_status = pacha_process_map(
        process_fd,
        vmo_fd,
        target_va,
        map_size,
        prot_from_elf_flags(p_flags),
        0);
    (void)pacha_munmap(mapped, map_size);
    (void)pacha_fd_close(vmo_fd);
    if (map_status != 0) return -5;

    printf("[seed0root] exec: mapped PT_LOAD[%u] process_fd=%d target=0x%llx size=%llu\n",
        index,
        process_fd,
        (unsigned long long)target_va,
        (unsigned long long)map_size);
    return 0;
}

static int map_bytes_into_process(
    int process_fd,
    uint64_t target_va,
    const void *data,
    uint64_t size,
    uint64_t prot)
{
    if (process_fd < 16 || target_va == 0 || data == NULL || size == 0) {
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
        PACHA_FD_RIGHT_MAP_WRITE;
    const int vmo_fd = pacha_vmo_create(map_size, vmo_rights, 0);
    if (vmo_fd < 16) {
        return -3;
    }

    unsigned char *mapped = pacha_mmap(
        vmo_fd,
        map_size,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (mapped == NULL) {
        (void)pacha_fd_close(vmo_fd);
        return -4;
    }

    memset(mapped, 0, (size_t)map_size);
    memcpy(mapped, data, (size_t)size);
    const int status = pacha_process_map(process_fd, vmo_fd, target_va, map_size, prot, 0);
    (void)pacha_munmap(mapped, map_size);
    (void)pacha_fd_close(vmo_fd);
    return status == 0 ? 0 : -5;
}

static int mark_fd_inherit(int fd, const char *label)
{
    if (fd < 16) {
        return -1;
    }
    const long status = pacha_fd_fcntl(
        fd,
        PACHA_FD_FCNTL_SET_FLAGS,
        PACHA_FD_FLAG_INHERIT,
        PACHA_FD_FLAG_INHERIT);
    if (status != 0) {
        fprintf(stderr, "[seed0root] %s: mark fd inherit failed fd=%d status=%ld\n",
            label,
            fd,
            status);
        return -2;
    }
    return 0;
}

static int load_elf_process(
    const char *path,
    const unsigned char *image,
    uint64_t image_size,
    struct seed0root_loaded_process *out)
{
    memset(out, 0, sizeof(*out));
    out->process_fd = -1;
    int status = validate_elf_header(path, image, image_size);
    if (status != 0) return status;

    const uint64_t e_entry = rd64(image + 24);
    const uint16_t e_type = rd16(image + 16);
    const uint64_t e_phoff = rd64(image + 32);
    const uint16_t e_phentsize = rd16(image + 54);
    const uint16_t e_phnum = rd16(image + 56);
    const uint64_t load_bias = e_type == SEED0ROOT_ELF_TYPE_DYN ? SEED0ROOT_CHILD_LOAD_BASE : 0;
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
        fprintf(stderr, "[seed0root] exec: process_create failed status=%d\n", process_fd);
        return -7;
    }

    uint16_t load_count = 0;
    for (uint16_t i = 0; i < e_phnum; i++) {
        const unsigned char *ph = image + e_phoff + (uint64_t)i * e_phentsize;
        if (rd32(ph + 0) != SEED0ROOT_ELF_PT_LOAD) continue;
        status = map_elf_segment(path, process_fd, load_bias, image, image_size, ph, i);
        if (status != 0) {
            (void)pacha_fd_close(process_fd);
            return status;
        }
        load_count++;
    }
    if (load_count == 0) {
        (void)pacha_fd_close(process_fd);
        return -6;
    }

    out->process_fd = process_fd;
    out->runtime_entry = e_entry + load_bias;
    out->phdr_va = load_bias + e_phoff;
    out->phent = e_phentsize;
    out->phnum = e_phnum;
    out->load_segments = load_count;
    printf("[seed0root] exec: staged %s process_fd=%d entry=0x%llx load_segments=%u\n",
        path,
        process_fd,
        (unsigned long long)out->runtime_entry,
        load_count);
    return 0;
}

static int push_u64(unsigned char *stack, uint64_t *sp, uint64_t value)
{
    if (*sp < 8) return -1;
    *sp -= 8;
    wr64(stack + *sp, value);
    return 0;
}

static int start_loaded_process(const struct seed0root_loaded_process *loaded, const char *argv0)
{
    const int process_fd = loaded->process_fd;
    const uint64_t stack_base = SEED0ROOT_CHILD_STACK_TOP - SEED0ROOT_CHILD_STACK_SIZE;
    const uint64_t stack_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int stack_fd = pacha_vmo_create(SEED0ROOT_CHILD_STACK_SIZE, stack_rights, 0);
    if (stack_fd < 16) return -2;
    unsigned char *stack = pacha_mmap(
        stack_fd,
        SEED0ROOT_CHILD_STACK_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (stack == NULL) {
        (void)pacha_fd_close(stack_fd);
        return -3;
    }
    memset(stack, 0, (size_t)SEED0ROOT_CHILD_STACK_SIZE);

    uint64_t sp = SEED0ROOT_CHILD_STACK_SIZE;
    const uint64_t argv0_len = (uint64_t)strlen(argv0) + 1;
    sp -= argv0_len;
    memcpy(stack + sp, argv0, (size_t)argv0_len);
    const uint64_t argv0_va = stack_base + sp;
    sp &= ~15ull;
    sp -= 16;
    const uint64_t random_va = stack_base + sp;
    for (unsigned i = 0; i < 16; i++) stack[sp + i] = (unsigned char)(0x41u + i * 7u);
    sp &= ~15ull;

    if (push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_NULL) != 0 ||
        push_u64(stack, &sp, argv0_va) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_EXECFN) != 0 ||
        push_u64(stack, &sp, random_va) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_RANDOM) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_BASE) != 0 ||
        push_u64(stack, &sp, SEED0ROOT_PAGE_SIZE) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_PAGESZ) != 0 ||
        push_u64(stack, &sp, loaded->phnum) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_PHNUM) != 0 ||
        push_u64(stack, &sp, loaded->phent) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_PHENT) != 0 ||
        push_u64(stack, &sp, loaded->phdr_va) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_PHDR) != 0 ||
        push_u64(stack, &sp, 0) != 0 ||
        push_u64(stack, &sp, 0) != 0 ||
        push_u64(stack, &sp, argv0_va) != 0 ||
        push_u64(stack, &sp, 1) != 0) {
        (void)pacha_munmap(stack, SEED0ROOT_CHILD_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -5;
    }

    const int map_status = pacha_process_map(
        process_fd,
        stack_fd,
        stack_base,
        SEED0ROOT_CHILD_STACK_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        0);
    (void)pacha_munmap(stack, SEED0ROOT_CHILD_STACK_SIZE);
    (void)pacha_fd_close(stack_fd);
    if (map_status != 0) return -6;

    const uint64_t thread_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_KILL |
        PACHA_FD_RIGHT_START |
        PACHA_FD_RIGHT_SET_CONTEXT;
    const int thread_fd = pacha_thread_create(process_fd, loaded->runtime_entry, stack_base + sp, 0, 0, thread_rights);
    if (thread_fd < 16) return -7;
    const int start_status = pacha_thread_start(thread_fd);
    if (start_status != 0) {
        (void)pacha_fd_close(thread_fd);
        return -8;
    }
    printf("[seed0root] exec: started process_fd=%d thread_fd=%d entry=0x%llx stack=0x%llx\n",
        process_fd,
        thread_fd,
        (unsigned long long)loaded->runtime_entry,
        (unsigned long long)(stack_base + sp));
    return 0;
}

static int stage_koboxd_bootstrap(
    const struct seed0root_bootstrap *bootstrap,
    const struct seed0root_loaded_process *loaded)
{
    if (bootstrap == NULL ||
        loaded == NULL ||
        loaded->process_fd < 16 ||
        bootstrap->module_count == 0 ||
        bootstrap->module_count > SEED0ROOT_BOOTSTRAP_MAX_MODULES ||
        bootstrap->modules_va == 0)
    {
        return -1;
    }

    const struct seed0root_bootstrap_module *source_modules =
        (const struct seed0root_bootstrap_module *)(uintptr_t)bootstrap->modules_va;
    struct seed0root_bootstrap_module child_modules[SEED0ROOT_BOOTSTRAP_MAX_MODULES];
    memset(child_modules, 0, sizeof(child_modules));

    uint64_t next_va = SEED0ROOT_KOBOXD_MODULE_IMAGE_BASE;
    for (uint64_t i = 0; i < bootstrap->module_count; i++) {
        const struct seed0root_bootstrap_module *src = &source_modules[i];
        if (src->name[0] == '\0' || src->image_va == 0 || src->image_size < 4) {
            return -2;
        }
        const unsigned char *image = (const unsigned char *)(uintptr_t)src->image_va;
        if (image[0] != 0x7f || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') {
            fprintf(stderr, "[seed0root] koboxd bootstrap module %s is not ELF\n", src->name);
            return -3;
        }

        snprintf(child_modules[i].name, sizeof(child_modules[i].name), "%s", src->name);
        child_modules[i].image_va = next_va;
        child_modules[i].image_size = src->image_size;

        const int map_status = map_bytes_into_process(
            loaded->process_fd,
            child_modules[i].image_va,
            image,
            src->image_size,
            PACHA_PROT_READ);
        if (map_status != 0) {
            fprintf(stderr,
                "[seed0root] koboxd bootstrap map module=%s target=0x%llx status=%d\n",
                child_modules[i].name,
                (unsigned long long)child_modules[i].image_va,
                map_status);
            return -4;
        }

        uint64_t map_size = 0;
        if (align_up(src->image_size, &map_size) != 0) {
            return -5;
        }
        printf("[seed0root] koboxd bootstrap module=%s source=0x%llx target=0x%llx bytes=%llu\n",
            child_modules[i].name,
            (unsigned long long)src->image_va,
            (unsigned long long)child_modules[i].image_va,
            (unsigned long long)src->image_size);
        next_va += map_size;
    }

    const struct seed0root_koboxd_bootstrap child_bootstrap = {
        .magic = SEED0ROOT_KOBOXD_BOOTSTRAP_MAGIC,
        .version = SEED0ROOT_KOBOXD_BOOTSTRAP_VERSION,
        .device_fd = bootstrap->device_fd,
        .module_count = bootstrap->module_count,
        .modules_va = SEED0ROOT_KOBOXD_MODULE_TABLE_VA,
    };

    int status = map_bytes_into_process(
        loaded->process_fd,
        SEED0ROOT_KOBOXD_BOOTSTRAP_VA,
        &child_bootstrap,
        sizeof(child_bootstrap),
        PACHA_PROT_READ);
    if (status == 0) {
        status = map_bytes_into_process(
            loaded->process_fd,
            SEED0ROOT_KOBOXD_MODULE_TABLE_VA,
            child_modules,
            sizeof(child_modules[0]) * bootstrap->module_count,
            PACHA_PROT_READ);
    }
    if (status != 0) {
        fprintf(stderr, "[seed0root] koboxd bootstrap config map failed status=%d\n", status);
        return -6;
    }

    printf("[seed0root] koboxd bootstrap package mapped modules=%llu config=0x%llx table=0x%llx\n",
        (unsigned long long)bootstrap->module_count,
        (unsigned long long)SEED0ROOT_KOBOXD_BOOTSTRAP_VA,
        (unsigned long long)SEED0ROOT_KOBOXD_MODULE_TABLE_VA);
    return 0;
}

static int launch_koboxd(void)
{
    const struct seed0root_bootstrap *bootstrap =
        (const struct seed0root_bootstrap *)(uintptr_t)SEED0ROOT_BOOTSTRAP_VA;
    if (bootstrap->magic != SEED0ROOT_BOOTSTRAP_MAGIC ||
        bootstrap->version != SEED0ROOT_BOOTSTRAP_VERSION ||
        bootstrap->koboxd_image_va == 0 ||
        bootstrap->koboxd_image_size == 0 ||
        bootstrap->device_fd < 16 ||
        bootstrap->module_count == 0 ||
        bootstrap->module_count > SEED0ROOT_BOOTSTRAP_MAX_MODULES ||
        bootstrap->modules_va == 0) {
        fprintf(stderr,
            "[seed0root] bootstrap unavailable magic=0x%llx version=%llu device_fd=%llu koboxd_va=0x%llx size=%llu modules=%llu table=0x%llx\n",
            (unsigned long long)bootstrap->magic,
            (unsigned long long)bootstrap->version,
            (unsigned long long)bootstrap->device_fd,
            (unsigned long long)bootstrap->koboxd_image_va,
            (unsigned long long)bootstrap->koboxd_image_size,
            (unsigned long long)bootstrap->module_count,
            (unsigned long long)bootstrap->modules_va);
        return -1;
    }

    const unsigned char *image = (const unsigned char *)(uintptr_t)bootstrap->koboxd_image_va;
    int status = mark_fd_inherit((int)bootstrap->device_fd, "koboxd device fd");
    if (status != 0) {
        fprintf(stderr, "[seed0root] koboxd device fd inherit failed status=%d fd=%llu\n",
            status,
            (unsigned long long)bootstrap->device_fd);
        return status;
    }

    struct seed0root_loaded_process loaded;
    status = load_elf_process("/sbin/koboxd.elf", image, bootstrap->koboxd_image_size, &loaded);
    if (status != 0) {
        fprintf(stderr, "[seed0root] koboxd load failed status=%d\n", status);
        return status;
    }
    status = stage_koboxd_bootstrap(bootstrap, &loaded);
    if (status != 0) {
        fprintf(stderr, "[seed0root] koboxd bootstrap package failed status=%d\n", status);
        return status;
    }
    status = start_loaded_process(&loaded, "/sbin/koboxd.elf");
    if (status != 0) {
        fprintf(stderr, "[seed0root] koboxd start failed status=%d\n", status);
        return status;
    }
    printf("[seed0root] koboxd started\n");
    return 0;
}

int main(int argc, char **argv)
{
    printf("[seed0root] start argc=%d argv0=%s\n",
        argc,
        (argc > 0 && argv != NULL && argv[0] != NULL) ? argv[0] : "(null)");

    struct timespec ts = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        printf("[seed0root] monotonic=%llu.%09llu\n",
            (unsigned long long)ts.tv_sec,
            (unsigned long long)ts.tv_nsec);
    } else {
        fprintf(stderr, "[seed0root] clock_gettime failed\n");
        return 2;
    }

    char *buf = malloc(64);
    if (buf == NULL) {
        fprintf(stderr, "[seed0root] malloc failed\n");
        return 3;
    }
    snprintf(buf, 64, "[seed0root] malloc/stdout OK\n");
    fputs(buf, stdout);
    free(buf);

    fprintf(stderr, "[seed0root] stderr OK\n");
    int launch_status = launch_koboxd();
    if (launch_status != 0) {
        fprintf(stderr, "[seed0root] koboxd launch failed status=%d\n", launch_status);
        return 4;
    }
    printf("[seed0root] ready\n");
    fflush(stdout);
    fflush(stderr);
    return 0;
}
