#include "filed/exec.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filed/runtime.h"
#include "pacha/abi.h"
#include "pacha/ipc.h"
#include "pacha/syscall.h"

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
    FILED_EXEC_ELF_PF_X = 1,
    FILED_EXEC_ELF_PF_W = 2,
    FILED_EXEC_ELF_PF_R = 4,
    FILED_EXEC_AT_NULL = 0,
    FILED_EXEC_AT_PHDR = 3,
    FILED_EXEC_AT_PHENT = 4,
    FILED_EXEC_AT_PHNUM = 5,
    FILED_EXEC_AT_PAGESZ = 6,
    FILED_EXEC_AT_BASE = 7,
    FILED_EXEC_AT_RANDOM = 25,
    FILED_EXEC_AT_EXECFN = 31,
    FILED_EXEC_MAX_IMAGE_BYTES = 64ull * 1024ull * 1024ull,
};

typedef struct filed_exec_image {
    unsigned char *bytes;
    uint64_t size;
} filed_exec_image_t;

typedef struct filed_exec_plan {
    int process_fd;
    int thread_fd;
    uint64_t runtime_entry;
    uint64_t phdr_va;
    uint64_t phent;
    uint64_t phnum;
    uint16_t load_segments;
} filed_exec_plan_t;

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

static uint64_t filed_exec_prot_from_elf_flags(uint32_t flags)
{
    uint64_t prot = 0;
    if ((flags & FILED_EXEC_ELF_PF_R) != 0) prot |= PACHA_PROT_READ;
    if ((flags & FILED_EXEC_ELF_PF_W) != 0) prot |= PACHA_PROT_WRITE;
    if ((flags & FILED_EXEC_ELF_PF_X) != 0) prot |= PACHA_PROT_EXEC;
    return prot;
}

static int filed_exec_set_inherit(int fd, int enabled)
{
    if (fd < 16) {
        return -22;
    }
    const long status = pacha_fd_fcntl(
        fd,
        PACHA_FD_FCNTL_SET_FLAGS,
        enabled ? PACHA_FD_FLAG_INHERIT : 0,
        PACHA_FD_FLAG_INHERIT);
    return status == 0 ? 0 : -22;
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

static int filed_exec_prepare_inherit_fds(
    const filed_wire_exec_path_t *request,
    const int *inherit_fds,
    uint64_t inherit_fd_count,
    int bootstrap_fd,
    int *prepared,
    uint64_t *out_count)
{
    uint64_t count = 0;
    if (request == NULL || prepared == NULL || out_count == NULL) {
        return -22;
    }

    if (inherit_fd_count > FILED_WIRE_EXEC_MAX_INHERIT_FDS) {
        return -22;
    }
    if (inherit_fd_count != 0 && inherit_fds == NULL) {
        return -22;
    }

    if ((request->flags & FILED_WIRE_EXEC_INHERIT_FDS) != 0) {
        for (uint64_t i = 0; i < inherit_fd_count; ++i) {
            const int fd = inherit_fds[i];
            if (fd < 16) {
                return -22;
            }
            if (filed_exec_set_inherit(fd, 1) != 0) {
                return -13;
            }
            prepared[count++] = fd;
        }
    } else if (inherit_fd_count != 0) {
        return -22;
    }

    if ((request->flags & FILED_WIRE_EXEC_BOOTSTRAP_FD) != 0) {
        if (bootstrap_fd < 16) {
            return -22;
        }
        if (filed_exec_set_inherit(bootstrap_fd, 1) != 0) {
            return -13;
        }
        prepared[count++] = bootstrap_fd;
    }

    *out_count = count;
    return 0;
}

static void filed_exec_clear_prepared_inherit_fds(const int *prepared, uint64_t count)
{
    if (prepared == NULL) {
        return;
    }
    for (uint64_t i = 0; i < count; ++i) {
        if (prepared[i] >= 16) {
            (void)filed_exec_set_inherit(prepared[i], 0);
        }
    }
}

static int filed_exec_read_image(
    filed_runtime_t *runtime,
    filed_handle_id_t handle_id,
    filed_exec_image_t *out_image)
{
    filed_vfs_io_decision_t stat_decision;
    koboxd_wire_fs_statx_t stat;
    filed_status_t vfs_status;
    unsigned char *image;
    uint64_t offset = 0;

    if (runtime == NULL || out_image == NULL) {
        return -22;
    }
    memset(out_image, 0, sizeof(*out_image));

    vfs_status = filed_vfs_stat_prepare(&runtime->vfs, handle_id, &stat_decision);
    if (vfs_status != FILED_OK) {
        return -13;
    }
    memset(&stat, 0, sizeof(stat));
    int status = filed_kobox_backend_statx(&runtime->backend, stat_decision.backend_object, &stat);
    if (status != 0) {
        return status;
    }
    if ((stat.kind & 0170000u) != 0100000u || stat.size < FILED_EXEC_ELF64_EHDR_BYTES ||
        stat.size > FILED_EXEC_MAX_IMAGE_BYTES)
    {
        return -8;
    }

    image = malloc((size_t)stat.size);
    if (image == NULL) {
        return -12;
    }

    while (offset < stat.size) {
        filed_vfs_io_decision_t read_decision;
        uint64_t got = 0;
        uint64_t want = stat.size - offset;
        if (want > FILED_WIRE_IO_BYTES) {
            want = FILED_WIRE_IO_BYTES;
        }
        vfs_status = filed_vfs_pread_prepare(&runtime->vfs, handle_id, offset, want, &read_decision);
        if (vfs_status != FILED_OK) {
            free(image);
            return -13;
        }
        status = filed_kobox_backend_pread(
            &runtime->backend,
            read_decision.backend_object,
            read_decision.offset,
            image + offset,
            read_decision.length,
            &got);
        if (status != 0 || got == 0 || got > want) {
            free(image);
            return status != 0 ? status : -5;
        }
        offset += got;
    }

    out_image->bytes = image;
    out_image->size = stat.size;
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

static int filed_exec_load_image(const filed_exec_image_t *image, filed_exec_plan_t *plan)
{
    if (image == NULL || image->bytes == NULL || plan == NULL) {
        return -22;
    }
    memset(plan, 0, sizeof(*plan));
    plan->process_fd = -1;
    plan->thread_fd = -1;

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
    const int process_fd = pacha_process_create(process_rights, 0);
    if (process_fd < 16) {
        return -12;
    }

    uint16_t load_count = 0;
    for (uint16_t i = 0; i < e_phnum; ++i) {
        const unsigned char *ph = bytes + e_phoff + (uint64_t)i * e_phentsize;
        if (filed_exec_rd32(ph) != FILED_EXEC_ELF_PT_LOAD) {
            continue;
        }
        const uint64_t p_vaddr = filed_exec_rd64(ph + 16);
        const uint64_t target_va =
            (use_aslr && load_count == 0) ? 0 : filed_exec_align_down(p_vaddr + load_bias);
        uint64_t mapped_va = 0;
        status = filed_exec_map_segment(process_fd, image, ph, i, target_va, &mapped_va);
        if (status != 0) {
            filed_exec_discard_process_fd(process_fd);
            return status;
        }
        if (use_aslr && load_count == 0) {
            load_bias = mapped_va - filed_exec_align_down(p_vaddr);
        }
        load_count++;
    }
    if (load_count == 0) {
        filed_exec_discard_process_fd(process_fd);
        return -8;
    }

    plan->process_fd = process_fd;
    plan->runtime_entry = e_entry + load_bias;
    plan->phdr_va = load_bias + e_phoff;
    plan->phent = e_phentsize;
    plan->phnum = e_phnum;
    plan->load_segments = load_count;
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

static int filed_exec_start_plan(
    filed_exec_plan_t *plan,
    const filed_wire_exec_path_t *request,
    int bootstrap_fd)
{
    if (plan == NULL || request == NULL || plan->process_fd < 16) {
        return -22;
    }

    const char *argv0 = request->argv0[0] != '\0' ? request->argv0 : request->path;
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
    const long stack_map = pacha_process_map(
        plan->process_fd,
        stack_fd,
        0,
        PACHA_PROCESS_DEFAULT_STACK_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        0);
    if (stack_map < 4096) {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -12;
    }
    const uint64_t stack_base = (uint64_t)stack_map;

    uint64_t sp = PACHA_PROCESS_DEFAULT_STACK_SIZE;
    const uint64_t argv0_len = (uint64_t)strlen(argv0) + 1;
    if (argv0_len >= sp) {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -75;
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

    const int has_bootstrap =
        (request->flags & FILED_WIRE_EXEC_BOOTSTRAP_FD) != 0 && bootstrap_fd >= 16;
    if (filed_exec_push_u64(stack, &sp, 0) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_NULL) != 0 ||
        (has_bootstrap &&
            (filed_exec_push_u64(stack, &sp, (uint64_t)(uint32_t)bootstrap_fd) != 0 ||
             filed_exec_push_u64(stack, &sp, PACHA_AT_BOOTSTRAP_FD) != 0)) ||
        filed_exec_push_u64(stack, &sp, argv0_va) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_EXECFN) != 0 ||
        filed_exec_push_u64(stack, &sp, random_va) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_RANDOM) != 0 ||
        filed_exec_push_u64(stack, &sp, 0) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_BASE) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_PAGE_SIZE) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_PAGESZ) != 0 ||
        filed_exec_push_u64(stack, &sp, plan->phnum) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_PHNUM) != 0 ||
        filed_exec_push_u64(stack, &sp, plan->phent) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_PHENT) != 0 ||
        filed_exec_push_u64(stack, &sp, plan->phdr_va) != 0 ||
        filed_exec_push_u64(stack, &sp, FILED_EXEC_AT_PHDR) != 0 ||
        filed_exec_push_u64(stack, &sp, 0) != 0 ||
        filed_exec_push_u64(stack, &sp, 0) != 0 ||
        filed_exec_push_u64(stack, &sp, argv0_va) != 0 ||
        filed_exec_push_u64(stack, &sp, 1) != 0)
    {
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
        (void)pacha_fd_close(thread_fd);
        return -5;
    }
    plan->thread_fd = thread_fd;
    return 0;
}

int filed_exec_handle(
    struct filed_runtime *runtime,
    filed_handle_id_t handle_id,
    const filed_wire_exec_path_t *request,
    const int *inherit_fds,
    uint64_t inherit_fd_count,
    int bootstrap_fd,
    int *out_process_fd,
    int *out_thread_fd)
{
    filed_exec_image_t image;
    filed_exec_plan_t plan;
    int prepared[FILED_WIRE_EXEC_MAX_INHERIT_FDS + 1];
    uint64_t prepared_count = 0;

    if (out_process_fd != NULL) *out_process_fd = -1;
    if (out_thread_fd != NULL) *out_thread_fd = -1;
    if (runtime == NULL || request == NULL || out_process_fd == NULL || out_thread_fd == NULL) {
        return -22;
    }

    memset(prepared, 0, sizeof(prepared));
    int status = filed_exec_prepare_inherit_fds(
        request,
        inherit_fds,
        inherit_fd_count,
        bootstrap_fd,
        prepared,
        &prepared_count);
    if (status != 0) {
        return status;
    }

    status = filed_exec_read_image(runtime, handle_id, &image);
    if (status != 0) {
        filed_exec_clear_prepared_inherit_fds(prepared, prepared_count);
        return status;
    }

    status = filed_exec_load_image(&image, &plan);
    free(image.bytes);
    if (status != 0) {
        filed_exec_clear_prepared_inherit_fds(prepared, prepared_count);
        return status;
    }

    status = filed_exec_start_plan(&plan, request, bootstrap_fd);
    filed_exec_clear_prepared_inherit_fds(prepared, prepared_count);
    if (status != 0) {
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
