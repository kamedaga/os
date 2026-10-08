#include "filed/exec_native.h"
#include "filed/exec.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "filed/storage.h"
#include "filed/runtime.h"
#include "pacha/abi.h"
#include "pacha/ipc.h"
#include "pacha/syscall.h"
#include "pacha/trace.h"

enum {
    FILED_EXEC_PAGE_SIZE = 4096,
    FILED_EXEC_ELF64_EHDR_BYTES = 64,
    FILED_EXEC_ELF64_PHDR_BYTES = 56,
    FILED_EXEC_ELF_CLASS_64 = 2,
    FILED_EXEC_ELF_DATA_LSB = 1,
    FILED_EXEC_ELF_VERSION_CURRENT = 1,
    FILED_EXEC_ELF_TYPE_EXEC = 2,
    FILED_EXEC_ELF_TYPE_DYN = 3,
    FILED_EXEC_ELF_MACHINE_X86_64 = 0x3e,
    FILED_EXEC_ELF_PT_LOAD = 1,
    FILED_EXEC_ELF_PT_INTERP = 3,
    FILED_EXEC_ELF_PT_PHDR = 6,
    FILED_EXEC_ELF_PF_X = 1,
    FILED_EXEC_ELF_PF_W = 2,
    FILED_EXEC_ELF_PF_R = 4,
    FILED_EXEC_AT_NULL = 0,
    FILED_EXEC_AT_PHDR = 3,
    FILED_EXEC_AT_PHENT = 4,
    FILED_EXEC_AT_PHNUM = 5,
    FILED_EXEC_AT_PAGESZ = 6,
    FILED_EXEC_AT_BASE = 7,
    FILED_EXEC_AT_ENTRY = 9,
    FILED_EXEC_AT_RANDOM = 25,
    FILED_EXEC_AT_EXECFN = 31,
    FILED_EXEC_MAX_INTERP_BYTES = 4096,
    FILED_EXEC_DYN_MAIN_BASE = 0x10000000ull,
    FILED_EXEC_DYN_INTERP_BASE = 0x20000000ull,
    FILED_EXEC_WALK_RIGHTS =
        FILED_RIGHT_LOOKUP |
        FILED_RIGHT_STAT |
        FILED_RIGHT_GETDENTS,
};

typedef struct filed_exec_image {
    unsigned char *bytes;
    uint64_t size;
} filed_exec_image_t;

typedef struct filed_exec_plan {
    int process_fd;
    int thread_fd;
    uint64_t runtime_entry;
    uint64_t main_entry;
    uint64_t interpreter_base;
    uint64_t phdr_va;
    uint64_t phent;
    uint64_t phnum;
    uint16_t load_segments;
} filed_exec_plan_t;

typedef struct filed_exec_loaded_image {
    uint64_t entry;
    uint64_t base;
    uint64_t phdr_va;
    uint64_t phent;
    uint64_t phnum;
    uint16_t load_segments;
} filed_exec_loaded_image_t;

static int filed_exec_validate_elf_header(const filed_exec_image_t *image);

static uint64_t filed_exec_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void filed_exec_metric(const char *op, uint64_t start_ns, uint64_t end_ns)
{
    if (op == NULL || start_ns == 0 || end_ns < start_ns) {
        return;
    }
    pacha_trace2(PACHA_TRACE_COMPONENT_FILED, PACHA_TRACE_EVENT_FILED_EXEC_METRIC, PACHA_TRACE_CLASS_METRIC, pacha_trace_name_id(op), end_ns - start_ns);
}

static uint16_t filed_exec_rd16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t filed_exec_rd32(const unsigned char *p)
{
    return (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static uint64_t filed_exec_rd64(const unsigned char *p)
{
    return (uint64_t)filed_exec_rd32(p) | ((uint64_t)filed_exec_rd32(p + 4) << 32);
}

static void filed_exec_wr64(unsigned char *p, uint64_t value)
{
    for (unsigned int i = 0; i < 8; ++i) {
        p[i] = (unsigned char)(value >> (i * 8));
    }
}

static uint64_t filed_exec_align_down(uint64_t value)
{
    return value & ~(uint64_t)(FILED_EXEC_PAGE_SIZE - 1);
}

static int filed_exec_align_up(uint64_t value, uint64_t *out)
{
    if (out == NULL || value > UINT64_MAX - (FILED_EXEC_PAGE_SIZE - 1)) {
        return -75;
    }
    *out = (value + (FILED_EXEC_PAGE_SIZE - 1)) & ~(uint64_t)(FILED_EXEC_PAGE_SIZE - 1);
    return 0;
}

static uint64_t filed_exec_max_u64(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

static uint64_t filed_exec_prot_from_elf_flags(uint32_t flags)
{
    uint64_t prot = 0;
    if ((flags & FILED_EXEC_ELF_PF_R) != 0) prot |= PACHA_PROT_READ;
    if ((flags & FILED_EXEC_ELF_PF_W) != 0) prot |= PACHA_PROT_WRITE;
    if ((flags & FILED_EXEC_ELF_PF_X) != 0) prot |= PACHA_PROT_EXEC;
    return prot;
}

static int filed_exec_validate_elf_ehdr_prefix(const unsigned char *bytes)
{
    if (bytes == NULL) {
        return -8;
    }
    if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
        return -8;
    }
    if (bytes[4] != FILED_EXEC_ELF_CLASS_64 ||
        bytes[5] != FILED_EXEC_ELF_DATA_LSB ||
        bytes[6] != FILED_EXEC_ELF_VERSION_CURRENT)
    {
        return -8;
    }
    const uint16_t e_type = filed_exec_rd16(bytes + 16);
    const uint16_t e_machine = filed_exec_rd16(bytes + 18);
    const uint32_t e_version = filed_exec_rd32(bytes + 20);
    if ((e_type != FILED_EXEC_ELF_TYPE_EXEC && e_type != FILED_EXEC_ELF_TYPE_DYN) ||
        e_machine != FILED_EXEC_ELF_MACHINE_X86_64 ||
        e_version != FILED_EXEC_ELF_VERSION_CURRENT)
    {
        return -8;
    }
    const uint16_t e_phentsize = filed_exec_rd16(bytes + 54);
    const uint16_t e_phnum = filed_exec_rd16(bytes + 56);
    if (e_phentsize < FILED_EXEC_ELF64_PHDR_BYTES || e_phnum == 0) {
        return -8;
    }
    return 0;
}

static void filed_exec_discard_process_fd(int process_fd)
{
    if (process_fd >= 16) {
        (void)pacha_syscall2(
            PACHA_PROCESS_SYSCALL_KILL,
            (uint64_t)(uint32_t)process_fd,
            1);
        (void)pacha_fd_close(process_fd);
    }
}


static int filed_exec_read_range(filed_runtime_t *runtime,
    uint64_t handle, uint64_t object, uint64_t size, uint64_t offset,
    unsigned char *buffer, uint64_t length)
{
    (void)object;
    if (!runtime || (length && !buffer) || offset > size ||
        length > size-offset || length > SIZE_MAX) return -22;
    size_t done = 0;
    while (done < length) {
        size_t got = 0;
        int status = filed_storage_exec_pread(runtime,handle,offset+done,buffer+done,
                                         (size_t)length-done,&got);
        if (status) return status;
        if (!got) return -8;
        done += got;
    }
    return 0;
}

static int filed_exec_read_image(
    filed_runtime_t *runtime,
    filed_handle_id_t handle_id,
    filed_exec_image_t *out_image)
{
    filed_storage_stat_t stat;
    unsigned char ehdr[FILED_EXEC_ELF64_EHDR_BYTES];
    unsigned char *image = NULL;
    if (!runtime || !out_image || !handle_id) return -22;
    memset(out_image, 0, sizeof(*out_image));
    int status = filed_storage_exec_stat(runtime, handle_id, &stat);
    if (status) return status;
    if ((stat.mode & 0170000) != 0100000 ||
        stat.size < FILED_EXEC_ELF64_EHDR_BYTES || stat.size > SIZE_MAX)
        return -8;
    if (!(stat.mode & 0111)) return -13;

    status = filed_exec_read_range(
        runtime,
        handle_id,
        handle_id,
        stat.size,
        0,
        ehdr,
        sizeof(ehdr));
    if (status != 0) {
        return status;
    }

    if (filed_exec_validate_elf_ehdr_prefix(ehdr) != 0) {
        return -8;
    }

    const uint64_t e_phoff = filed_exec_rd64(ehdr + 32);
    const uint16_t e_phentsize = filed_exec_rd16(ehdr + 54);
    const uint16_t e_phnum = filed_exec_rd16(ehdr + 56);
    const uint64_t phdr_bytes = (uint64_t)e_phentsize * e_phnum;
    if (e_phoff > stat.size || phdr_bytes > stat.size - e_phoff) {
        return -8;
    }
    const uint64_t metadata_size =
        filed_exec_max_u64(e_phoff + phdr_bytes, FILED_EXEC_ELF64_EHDR_BYTES);
    if (metadata_size > SIZE_MAX || metadata_size > stat.size) {
        return -8;
    }

    image = malloc((size_t)metadata_size);
    if (image == NULL) {
        return -12;
    }
    memcpy(image, ehdr, sizeof(ehdr));
    if (metadata_size > sizeof(ehdr)) {
        status = filed_exec_read_range(
            runtime,
            handle_id,
            handle_id,
            stat.size,
            sizeof(ehdr),
            image + sizeof(ehdr),
            metadata_size - sizeof(ehdr));
        if (status != 0) {
            free(image);
            return status;
        }
    }

    filed_exec_image_t metadata_image = {
        .bytes = image,
        .size = metadata_size,
    };
    if (filed_exec_validate_elf_header(&metadata_image) != 0) {
        free(image);
        return -8;
    }

    uint64_t needed_size = metadata_size;
    for (uint16_t i = 0; i < e_phnum; ++i) {
        const unsigned char *ph = image + e_phoff + (uint64_t)i * e_phentsize;
        const uint32_t p_type = filed_exec_rd32(ph);
        if (p_type == FILED_EXEC_ELF_PT_LOAD || p_type == FILED_EXEC_ELF_PT_INTERP) {
            const uint64_t p_offset = filed_exec_rd64(ph + 8);
            const uint64_t p_filesz = filed_exec_rd64(ph + 32);
            if (p_offset > stat.size || p_filesz > stat.size - p_offset) {
                free(image);
                return -8;
            }
            if (p_offset > UINT64_MAX - p_filesz) {
                free(image);
                return -75;
            }
            const uint64_t end = p_offset + p_filesz;
            if (end > needed_size) {
                needed_size = end;
            }
        }
    }
    if (needed_size < metadata_size || needed_size > SIZE_MAX) {
        free(image);
        return -8;
    }

    if (needed_size > metadata_size) {
        unsigned char *expanded = realloc(image, (size_t)needed_size);
        if (expanded == NULL) {
            free(image);
            return -12;
        }
        image = expanded;
        status = filed_exec_read_range(
            runtime,
            handle_id,
            handle_id,
            stat.size,
            metadata_size,
            image + metadata_size,
            needed_size - metadata_size);
        if (status != 0) {
            free(image);
            return status;
        }
    }

    out_image->bytes = image;
    out_image->size = needed_size;
    return 0;
}

static int filed_exec_validate_elf_header(const filed_exec_image_t *image)
{
    if (image == NULL || image->bytes == NULL || image->size < FILED_EXEC_ELF64_EHDR_BYTES) {
        return -8;
    }
    const unsigned char *bytes = image->bytes;
    if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
        return -8;
    }
    if (bytes[4] != FILED_EXEC_ELF_CLASS_64 ||
        bytes[5] != FILED_EXEC_ELF_DATA_LSB ||
        bytes[6] != FILED_EXEC_ELF_VERSION_CURRENT)
    {
        return -8;
    }
    const uint16_t e_type = filed_exec_rd16(bytes + 16);
    const uint16_t e_machine = filed_exec_rd16(bytes + 18);
    const uint32_t e_version = filed_exec_rd32(bytes + 20);
    const uint16_t e_phentsize = filed_exec_rd16(bytes + 54);
    const uint16_t e_phnum = filed_exec_rd16(bytes + 56);
    if ((e_type != FILED_EXEC_ELF_TYPE_EXEC && e_type != FILED_EXEC_ELF_TYPE_DYN) ||
        e_machine != FILED_EXEC_ELF_MACHINE_X86_64 ||
        e_version != FILED_EXEC_ELF_VERSION_CURRENT ||
        e_phentsize < FILED_EXEC_ELF64_PHDR_BYTES ||
        e_phnum == 0)
    {
        return -8;
    }
    const uint64_t e_phoff = filed_exec_rd64(bytes + 32);
    const uint64_t phdr_bytes = (uint64_t)e_phentsize * e_phnum;
    if (e_phoff > image->size || phdr_bytes > image->size - e_phoff) {
        return -8;
    }
    return 0;
}

static int filed_exec_map_segment(
    int process_fd,
    const filed_exec_image_t *image,
    const unsigned char *ph,
    uint16_t index,
    uint64_t target_va,
    uint64_t *out_mapped_va)
{
    const uint32_t p_flags = filed_exec_rd32(ph + 4);
    const uint64_t p_offset = filed_exec_rd64(ph + 8);
    const uint64_t p_vaddr = filed_exec_rd64(ph + 16);
    const uint64_t p_filesz = filed_exec_rd64(ph + 32);
    const uint64_t p_memsz = filed_exec_rd64(ph + 40);
    if (out_mapped_va != NULL) {
        *out_mapped_va = 0;
    }
    if (image == NULL || image->bytes == NULL || p_memsz < p_filesz ||
        p_offset > image->size || p_filesz > image->size - p_offset ||
        (p_memsz != 0 && p_vaddr > UINT64_MAX - p_memsz))
    {
        return -8;
    }
    if (p_memsz == 0) {
        return 0;
    }

    const uint64_t page_offset = p_vaddr - filed_exec_align_down(p_vaddr);
    uint64_t map_size = 0;
    if (filed_exec_align_up(page_offset + p_memsz, &map_size) != 0) {
        return -75;
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
        return -12;
    }
    unsigned char *mapped = pacha_mmap(
        vmo_fd,
        map_size,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (mapped == NULL) {
        (void)pacha_fd_close(vmo_fd);
        return -12;
    }
    memset(mapped, 0, (size_t)map_size);
    memcpy(mapped + page_offset, image->bytes + p_offset, (size_t)p_filesz);
    const long map_result = pacha_process_map(
        process_fd,
        vmo_fd,
        target_va,
        map_size,
        filed_exec_prot_from_elf_flags(p_flags),
        0);
    (void)pacha_munmap(mapped, map_size);
    (void)pacha_fd_close(vmo_fd);
    if (map_result < 4096) {
        fprintf(stderr,
            "[filed] exec map failed segment=%u target=0x%llx size=%llu status=%ld\n",
            index,
            (unsigned long long)target_va,
            (unsigned long long)map_size,
            map_result);
        return -12;
    }
    if (out_mapped_va != NULL) {
        *out_mapped_va = (uint64_t)map_result;
    }
    return 0;
}

static int filed_exec_get_interp_path(const filed_exec_image_t *image, char *out_path, size_t out_size)
{
    if (out_path == NULL || out_size == 0) {
        return -22;
    }
    out_path[0] = '\0';
    if (image == NULL || image->bytes == NULL) {
        return -22;
    }

    const unsigned char *bytes = image->bytes;
    const uint64_t e_phoff = filed_exec_rd64(bytes + 32);
    const uint16_t e_phentsize = filed_exec_rd16(bytes + 54);
    const uint16_t e_phnum = filed_exec_rd16(bytes + 56);

    for (uint16_t i = 0; i < e_phnum; ++i) {
        const unsigned char *ph = bytes + e_phoff + (uint64_t)i * e_phentsize;
        if (filed_exec_rd32(ph) != FILED_EXEC_ELF_PT_INTERP) {
            continue;
        }
        if (out_path[0] != '\0') {
            return -8;
        }
        const uint64_t p_offset = filed_exec_rd64(ph + 8);
        const uint64_t p_filesz = filed_exec_rd64(ph + 32);
        if (p_filesz == 0 ||
            p_filesz >= out_size ||
            p_filesz > FILED_EXEC_MAX_INTERP_BYTES ||
            p_offset > image->size ||
            p_filesz > image->size - p_offset)
        {
            return -8;
        }
        if (image->bytes[p_offset + p_filesz - 1u] != '\0') {
            return -8;
        }
        memcpy(out_path, image->bytes + p_offset, (size_t)p_filesz);
        if (out_path[0] != '/') {
            return -8;
        }
    }
    return 0;
}

static int filed_exec_load_image_into_process(
    int process_fd,
    const filed_exec_image_t *image,
    uint64_t dyn_base,
    filed_exec_loaded_image_t *loaded)
{
    if (process_fd < 16 || image == NULL || image->bytes == NULL || loaded == NULL) {
        return -22;
    }
    memset(loaded, 0, sizeof(*loaded));

    int status = filed_exec_validate_elf_header(image);
    if (status != 0) {
        return status;
    }

    const unsigned char *bytes = image->bytes;
    const uint64_t e_entry = filed_exec_rd64(bytes + 24);
    const uint16_t e_type = filed_exec_rd16(bytes + 16);
    const uint64_t e_phoff = filed_exec_rd64(bytes + 32);
    const uint16_t e_phentsize = filed_exec_rd16(bytes + 54);
    const uint16_t e_phnum = filed_exec_rd16(bytes + 56);
    uint64_t load_bias = 0;
    const int use_aslr = e_type == FILED_EXEC_ELF_TYPE_DYN;
    uint64_t phdr_va = 0;

    uint16_t load_count = 0;
    for (uint16_t i = 0; i < e_phnum; ++i) {
        const unsigned char *ph = bytes + e_phoff + (uint64_t)i * e_phentsize;
        const uint32_t p_type = filed_exec_rd32(ph);
        if (p_type == FILED_EXEC_ELF_PT_PHDR) {
            phdr_va = filed_exec_rd64(ph + 16);
            continue;
        }
        if (p_type != FILED_EXEC_ELF_PT_LOAD) {
            continue;
        }
        const uint64_t p_vaddr = filed_exec_rd64(ph + 16);
        if (use_aslr && load_count == 0) {
            if (dyn_base > UINT64_MAX - p_vaddr) {
                return -75;
            }
            load_bias = dyn_base;
        }
        if (p_vaddr > UINT64_MAX - load_bias) {
            return -75;
        }
        const uint64_t target_va = filed_exec_align_down(p_vaddr + load_bias);
        uint64_t mapped_va = 0;
        status = filed_exec_map_segment(process_fd, image, ph, i, target_va, &mapped_va);
        if (status != 0) {
            return status;
        }
        if (use_aslr && load_count == 0) {
            load_bias = mapped_va - filed_exec_align_down(p_vaddr);
        }
        load_count++;
    }
    if (load_count == 0) {
        return -8;
    }

    if (phdr_va == 0) {
        phdr_va = e_phoff;
    }
    loaded->entry = e_entry + load_bias;
    loaded->base = load_bias;
    loaded->phdr_va = phdr_va + load_bias;
    loaded->phent = e_phentsize;
    loaded->phnum = e_phnum;
    loaded->load_segments = load_count;
    return 0;
}

static int filed_exec_read_interpreter(filed_runtime_t *runtime,
    const char *path, filed_exec_image_t *out)
{
    uint64_t handle = 0;
    int status=filed_storage_exec_open(runtime,0,path,&handle);
    if (!status) status = filed_exec_read_image(runtime, handle, out);
    if(handle) (void)filed_storage_exec_close(runtime, handle);
    return status;
}

static int filed_exec_load_image(
    filed_runtime_t *runtime,
    const filed_exec_image_t *image,
    const struct pacha_process_fd_grant *grants, uint64_t grant_count,
    filed_exec_plan_t *plan)
{
    char interp_path[FILED_EXEC_MAX_INTERP_BYTES];
    filed_exec_loaded_image_t main_loaded;
    filed_exec_loaded_image_t interp_loaded;
    filed_exec_image_t interp_image;

    if (runtime == NULL || image == NULL || image->bytes == NULL || plan == NULL) {
        return -22;
    }
    memset(plan, 0, sizeof(*plan));
    memset(&interp_image, 0, sizeof(interp_image));
    plan->process_fd = -1;
    plan->thread_fd = -1;

    int status = filed_exec_validate_elf_header(image);
    if (status != 0) {
        return status;
    }
    status = filed_exec_get_interp_path(image, interp_path, sizeof(interp_path));
    if (status != 0) {
        return status;
    }

    const uint64_t process_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_KILL |
        PACHA_FD_RIGHT_SPAWN |
        PACHA_FD_RIGHT_MAP_INTO |
        PACHA_FD_RIGHT_SET_CONTEXT;
    const int process_fd = pacha_process_create(process_rights, 0, grants, grant_count);
    if (process_fd < 16) {
        return -12;
    }

    status = filed_exec_load_image_into_process(
        process_fd,
        image,
        FILED_EXEC_DYN_MAIN_BASE,
        &main_loaded);
    if (status != 0) {
        filed_exec_discard_process_fd(process_fd);
        return status;
    }

    plan->process_fd = process_fd;
    plan->main_entry = main_loaded.entry;
    plan->runtime_entry = main_loaded.entry;
    plan->interpreter_base = 0;
    plan->phdr_va = main_loaded.phdr_va;
    plan->phent = main_loaded.phent;
    plan->phnum = main_loaded.phnum;
    plan->load_segments = main_loaded.load_segments;

    if (interp_path[0] != '\0') {
        status = filed_exec_read_interpreter(runtime, interp_path, &interp_image);
        if (status != 0) {
            filed_exec_discard_process_fd(process_fd);
            return status;
        }
        status = filed_exec_load_image_into_process(
            process_fd,
            &interp_image,
            FILED_EXEC_DYN_INTERP_BASE,
            &interp_loaded);
        free(interp_image.bytes);
        if (status != 0) {
            filed_exec_discard_process_fd(process_fd);
            return status;
        }
        plan->runtime_entry = interp_loaded.entry;
        plan->interpreter_base = interp_loaded.base;
    }
    return 0;
}

static int filed_exec_push_u64(unsigned char *stack, uint64_t *sp, uint64_t value)
{
    if (stack == NULL || sp == NULL || *sp < 8) {
        return -22;
    }
    *sp -= 8;
    filed_exec_wr64(stack + *sp, value);
    return 0;
}

static int filed_exec_copy_stack_string(
    unsigned char *stack,
    uint64_t *sp,
    uint64_t stack_base,
    const char *value,
    uint64_t *out_va)
{
    if (stack == NULL || sp == NULL || value == NULL || out_va == NULL) {
        return -22;
    }
    const uint64_t length = (uint64_t)strlen(value) + 1u;
    if (length == 0 || length >= *sp) {
        return -75;
    }
    *sp -= length;
    memcpy(stack + *sp, value, (size_t)length);
    *out_va = stack_base + *sp;
    return 0;
}

static uint64_t filed_exec_request_argc(const filed_exec_path_t *request)
{
    if (request == NULL) {
        return 0;
    }
    return request->argc != 0 ? request->argc : 1u;
}

static const char *filed_exec_request_arg(
    const filed_exec_path_t *request,
    uint64_t index)
{
    if (request == NULL) {
        return "";
    }
    if (request->argc != 0 && index < request->argc) {
        return filed_exec_string(request, request->argv[index]);
    }
    return request->path;
}

static int filed_exec_start_plan_impl(
    filed_exec_plan_t *plan,
    const filed_exec_path_t *request,
    int bootstrap_fd, uint64_t *argv_va, uint64_t *envp_va)
{
    if (plan == NULL || request == NULL || plan->process_fd < 16) {
        return -22;
    }


    const uint64_t stack_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int stack_fd = pacha_vmo_create(PACHA_PROCESS_DEFAULT_STACK_SIZE, stack_rights, 0);
    if (stack_fd < 16) {
        return -12;
    }
    unsigned char *stack = pacha_mmap(
        stack_fd,
        PACHA_PROCESS_DEFAULT_STACK_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (stack == NULL) {
        (void)pacha_fd_close(stack_fd);
        return -12;
    }
    memset(stack, 0, (size_t)PACHA_PROCESS_DEFAULT_STACK_SIZE);
    const long stack_map = pacha_process_map_flags(
        plan->process_fd,
        stack_fd,
        PACHA_PROCESS_MAP_ANYWHERE,
        PACHA_PROCESS_DEFAULT_STACK_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        0,
        PACHA_PROCESS_MAP_PRIVATE);
    if (stack_map < 4096) {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -12;
    }
    const uint64_t stack_base = (uint64_t)stack_map;

    uint64_t sp = PACHA_PROCESS_DEFAULT_STACK_SIZE;
    const uint64_t argc = filed_exec_request_argc(request);
    const uint64_t envc = request->envc;

    for (uint64_t i = envc; i > 0; --i) {
        const int status = filed_exec_copy_stack_string(
            stack,
            &sp,
            stack_base,
            filed_exec_string(request, request->envp[i - 1u]),
            &envp_va[i - 1u]);
        if (status != 0) {
            (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
            (void)pacha_fd_close(stack_fd);
            return status;
        }
    }
    for (uint64_t i = argc; i > 0; --i) {
        const int status = filed_exec_copy_stack_string(
            stack,
            &sp,
            stack_base,
            filed_exec_request_arg(request, i - 1u),
            &argv_va[i - 1u]);
        if (status != 0) {
            (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
            (void)pacha_fd_close(stack_fd);
            return status;
        }
    }
    const uint64_t argv0_va = argv_va[0];
    sp &= ~15ull;
    sp -= 16;
    const uint64_t random_va = stack_base + sp;
    if (pacha_getrandom(stack + sp, 16, 0) != 16) {
        fprintf(stderr, "[filed] exec random failed\n");
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -5;
    }
    sp &= ~15ull;

    const int has_bootstrap =
        (request->flags & FILED_EXEC_BOOTSTRAP_FD) != 0 && bootstrap_fd >= 16;
    if (filed_exec_push_u64(stack, &sp, 0) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_NULL) != 0 ||
        (has_bootstrap &&
            (filed_exec_push_u64(stack, &sp, FILED_EXEC_NATIVE_BOOTSTRAP_FD) != 0 ||
             filed_exec_push_u64(stack, &sp, PACHA_AT_BOOTSTRAP_FD) != 0)) ||
        filed_exec_push_u64(stack, &sp, argv0_va) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_EXECFN) != 0 ||
        filed_exec_push_u64(stack, &sp, random_va) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_RANDOM) != 0 ||
        filed_exec_push_u64(stack, &sp, plan->main_entry) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_ENTRY) != 0 ||
        filed_exec_push_u64(stack, &sp, plan->interpreter_base) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_BASE) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_PAGE_SIZE) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_PAGESZ) != 0 ||
        filed_exec_push_u64(stack, &sp, plan->phnum) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_PHNUM) != 0 ||
        filed_exec_push_u64(stack, &sp, plan->phent) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_PHENT) != 0 ||
        filed_exec_push_u64(stack, &sp, plan->phdr_va) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_PHDR) != 0 ||
        filed_exec_push_u64(stack, &sp, 0) != 0)
    {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -12;
    }
    for (uint64_t i = envc; i > 0; --i) {
        if (filed_exec_push_u64(stack, &sp, envp_va[i - 1u]) != 0) {
            (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
            (void)pacha_fd_close(stack_fd);
            return -12;
        }
    }
    if (filed_exec_push_u64(stack, &sp, 0) != 0) {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -12;
    }
    for (uint64_t i = argc; i > 0; --i) {
        if (filed_exec_push_u64(stack, &sp, argv_va[i - 1u]) != 0) {
            (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
            (void)pacha_fd_close(stack_fd);
            return -12;
        }
    }
    if (filed_exec_push_u64(stack, &sp, argc) != 0) {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -12;
    }

    (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
    (void)pacha_fd_close(stack_fd);

    const uint64_t thread_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_KILL |
        PACHA_FD_RIGHT_START |
        PACHA_FD_RIGHT_SET_CONTEXT;
    const int thread_fd = pacha_thread_create(
        plan->process_fd,
        plan->runtime_entry,
        stack_base + sp,
        0,
        0,
        thread_rights);
    if (thread_fd < 16) {
        return -12;
    }
    const int start_status = pacha_thread_start(thread_fd);
    if (start_status != 0) {
        fprintf(stderr,
            "[filed] exec thread start failed entry=0x%llx stack=0x%llx status=%d\n",
            (unsigned long long)plan->runtime_entry,
            (unsigned long long)(stack_base + sp),
            start_status);
        (void)pacha_fd_close(thread_fd);
        return -5;
    }
    plan->thread_fd = thread_fd;
    return 0;
}


static int filed_exec_start_plan(filed_exec_plan_t *plan,
    const filed_exec_path_t *request, int bootstrap_fd)
{
    if (!request || request->argc > SIZE_MAX / sizeof(uint64_t) - 1 ||
        request->envc > SIZE_MAX / sizeof(uint64_t)) return -7;
    size_t argc = request->argc ? (size_t)request->argc : 1;
    size_t envc = (size_t)request->envc;
    if (argc > SIZE_MAX / sizeof(uint64_t) - envc) return -7;
    uint64_t *vectors = calloc(argc + envc, sizeof(*vectors));
    if (!vectors) return -12;
    int status = filed_exec_start_plan_impl(plan,request,bootstrap_fd,
                                vectors,vectors+argc);
    free(vectors);
    return status;
}

int filed_exec_native_handle(
    struct filed_runtime *runtime,
    filed_handle_id_t handle_id,
    const filed_exec_path_t *request,
    const int *inherit_fds,
    uint64_t inherit_fd_count,
    int bootstrap_fd,
    int *out_process_fd,
    int *out_thread_fd)
{
    filed_exec_image_t image;
    filed_exec_plan_t plan;
    struct pacha_process_fd_grant prepared[PACHA_PROCESS_CREATE_MAX_GRANTS];
    uint64_t prepared_count = 0;

    if (out_process_fd != NULL) *out_process_fd = -1;
    if (out_thread_fd != NULL) *out_thread_fd = -1;
    if (runtime == NULL || request == NULL || out_process_fd == NULL || out_thread_fd == NULL) {
        return -22;
    }

    memset(prepared, 0, sizeof(prepared));
    uint64_t stage_start = filed_exec_now_ns();
    int status = filed_exec_build_grants(
        runtime,
        request,
        inherit_fds,
        inherit_fd_count,
        bootstrap_fd,
        prepared,
        &prepared_count);
    filed_exec_metric("prepare_inherit_fds", stage_start, filed_exec_now_ns());
    if (status != 0) {
        return status;
    }

    stage_start = filed_exec_now_ns();
    status = filed_exec_read_image(runtime, handle_id, &image);
    filed_exec_metric("read_image", stage_start, filed_exec_now_ns());
    if (status != 0) {
        fprintf(stderr, "[filed] exec read image failed status=%d\n", status);
        return status;
    }

    stage_start = filed_exec_now_ns();
    status = filed_exec_load_image(runtime, &image, prepared, prepared_count, &plan);
    filed_exec_metric("load_image", stage_start, filed_exec_now_ns());
    free(image.bytes);
    if (status != 0) {
        fprintf(stderr, "[filed] exec load image failed status=%d\n", status);
        return status;
    }

    stage_start = filed_exec_now_ns();
    status = filed_exec_start_plan(&plan, request, bootstrap_fd);
    filed_exec_metric("start_plan", stage_start, filed_exec_now_ns());
    if (status != 0) {
        fprintf(stderr, "[filed] exec start failed status=%d\n", status);
        if (plan.thread_fd >= 16) {
            (void)pacha_fd_close(plan.thread_fd);
        }
        filed_exec_discard_process_fd(plan.process_fd);
        return status;
    }

    *out_process_fd = plan.process_fd;
    *out_thread_fd = plan.thread_fd;
    return 0;
}
