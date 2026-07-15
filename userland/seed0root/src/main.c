#include "pacha/ipc.h"
#include "pacha/service_abi.h"
#include "pacha/syscall.h"
#include "filed/payload.h"
#include "filed/ipc_protocol.h"
#include "lpr_supervisor/boot_config.h"
#include "lpr_supervisor/ipc_protocol.h"
#include "personality/linux_lpr.h"
#include "personality/lpr_manifest.h"

#ifndef SEED0ROOT_DEFAULT_BOOT_PROFILE
#define SEED0ROOT_DEFAULT_BOOT_PROFILE 0u
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

enum {
    SEED0ROOT_BOOTSTRAP_MAGIC = 0x305254424f4f5453ull,
    SEED0ROOT_STORAGE_READY_MAGIC = 0x3159445252545330ull,
    SEED0ROOT_SERVICES_READY_MAGIC = 0x3159445256533053ull,
    SEED0ROOT_BOOTSTRAP_MAX_MODULES = 8,
    SEED0ROOT_BOOTSTRAP_NAME_BYTES = 64,
    SEED0ROOT_FILED_STORAGE_BOOTSTRAP_MAGIC = 0x3150474b42584f4bull,
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
    SEED0ROOT_AT_NULL = 0,
    SEED0ROOT_AT_PHDR = 3,
    SEED0ROOT_AT_PHENT = 4,
    SEED0ROOT_AT_PHNUM = 5,
    SEED0ROOT_AT_PAGESZ = 6,
    SEED0ROOT_AT_BASE = 7,
    SEED0ROOT_AT_ENTRY = 9,
    SEED0ROOT_AT_UID = 11,
    SEED0ROOT_AT_EUID = 12,
    SEED0ROOT_AT_GID = 13,
    SEED0ROOT_AT_EGID = 14,
    SEED0ROOT_AT_SECURE = 23,
    SEED0ROOT_AT_RANDOM = 25,
    SEED0ROOT_AT_EXECFN = 31,
    SEED0ROOT_TASK_STATE_EXITED = 2,
    SEED0ROOT_BOOT_PROFILE_MEMORY = 1u << 0,
    SEED0ROOT_BOOT_PROFILE_BENCH = 1u << 1,
    SEED0ROOT_BOOT_PROFILE_FS_WRITE = 1u << 2,
    SEED0ROOT_BOOT_PROFILE_LPR = 1u << 3,
    SEED0ROOT_BOOT_PROFILE_LUA = 1u << 4,
    SEED0ROOT_BOOT_PROFILE_DYN_NEEDED = 1u << 5,
    SEED0ROOT_BOOT_PROFILE_CHIBICC = 1u << 6,
    SEED0ROOT_BOOT_PROFILE_APK = 1u << 7,
    SEED0ROOT_BOOT_PROFILE_CURL = 1u << 8,
    SEED0ROOT_BOOT_PROFILE_HTTPS = 1u << 9,
    SEED0ROOT_BOOT_PROFILE_APK_UPDATE = 1u << 10,
};

struct seed0root_bootstrap_module {
    char name[SEED0ROOT_BOOTSTRAP_NAME_BYTES];
    uint64_t image_fd;
    uint64_t image_size;
};

struct seed0root_bootstrap {
    uint64_t magic;
    uint64_t device_fd;
    uint64_t ready_channel_fd;
    uint64_t service_ready_channel_fd;
    uint64_t filed_image_fd;
    uint64_t filed_image_size;
    uint64_t module_count;
    struct seed0root_bootstrap_module modules[SEED0ROOT_BOOTSTRAP_MAX_MODULES];
};

struct seed0root_filed_storage_bootstrap {
    uint64_t magic;
    uint64_t device_fd;
    uint64_t filed_endpoint_fd;
    uint64_t module_count;
    struct seed0root_bootstrap_module modules[SEED0ROOT_BOOTSTRAP_MAX_MODULES];
};

struct seed0root_loaded_process {
    int process_fd;
    uint64_t runtime_entry;
    uint64_t load_bias;
    uint64_t phdr_va;
    uint64_t phent;
    uint64_t phnum;
    uint16_t load_segments;
};

struct seed0root_started_process {
    int process_fd;
    int thread_fd;
    uint64_t start_ms;
};

struct seed0root_wait_result {
    uint64_t state;
    uint64_t exit_code;
    uint64_t end_ns;
    uint64_t end_cycles;
    uint64_t elapsed_ns;
    uint64_t elapsed_cycles;
};

struct seed0root_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

static int load_elf_process(
    const char *path,
    const unsigned char *image,
    uint64_t image_size,
    struct seed0root_loaded_process *out);
static int mark_fd_inherit(int fd, const char *label);
static int start_loaded_process(
    const struct seed0root_loaded_process *loaded,
    const char *argv0,
    int bootstrap_fd,
    struct seed0root_started_process *out_started);

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

static uint64_t seed0root_now_ns(void)
{
    struct seed0root_timespec ts;
    memset(&ts, 0, sizeof(ts));
    const long status = pacha_syscall2(
        PACHA_RUNTIME_SYSCALL_CLOCK_GETTIME,
        PACHA_TIMERFD_CLOCK_MONOTONIC,
        (uint64_t)(uintptr_t)&ts);
    if (status != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t seed0root_read_tsc(void)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static int seed0root_starts_with(const char *text, const char *prefix)
{
    if (text == NULL || prefix == NULL) {
        return 0;
    }
    while (*prefix != '\0') {
        if (*text != *prefix) {
            return 0;
        }
        text++;
        prefix++;
    }
    return 1;
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
        fprintf(stderr, "[seed0root] exec: %s invalid PT_LOAD[%u]\n", path, index);
        return -1;
    }
    if (p_memsz == 0) return 0;

    const uint64_t page_offset = p_vaddr - align_down(p_vaddr);
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
    const long mmap_result = pacha_syscall6(
        PACHA_VM_SYSCALL_MMAP,
        (uint64_t)(uint32_t)vmo_fd,
        0,
        map_size,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    unsigned char *mapped = mmap_result < 4096 ? NULL : (unsigned char *)(uintptr_t)mmap_result;
    if (mapped == NULL) {
        fprintf(stderr,
            "[seed0root] exec: mmap failed %s PT_LOAD[%u] target=0x%llx size=%llu status=%ld\n",
            path,
            index,
            (unsigned long long)target_va,
            (unsigned long long)map_size,
            mmap_result);
        (void)pacha_fd_close(vmo_fd);
        return -4;
    }
    memset(mapped, 0, (size_t)map_size);
    memcpy(mapped + page_offset, image + p_offset, (size_t)p_filesz);
    const long map_result = pacha_process_map(
        process_fd,
        vmo_fd,
        target_va,
        map_size,
        prot_from_elf_flags(p_flags),
        0);
    (void)pacha_munmap(mapped, map_size);
    (void)pacha_fd_close(vmo_fd);
    if (map_result < 4096) return -5;
    if (out_mapped_va != NULL) *out_mapped_va = (uint64_t)map_result;

    return 0;
}

static int create_inherited_vmo_from_bytes_with_extra_rights(
    const void *data,
    uint64_t size,
    const char *label,
    uint64_t extra_rights)
{
    if (data == NULL || size == 0) {
        return -1;
    }
    const int trace = label != NULL && strcmp(label, "filed bootstrap fd") == 0;
    if (trace) {
        printf("[seed0root] %s create size=%llu\n", label, (unsigned long long)size);
        fflush(stdout);
    }
    uint64_t map_size = 0;
    if (align_up(size, &map_size) != 0) {
        return -2;
    }
    const uint64_t rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_READ |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE |
        extra_rights;
    const int fd = pacha_vmo_create(map_size, rights, 0);
    if (fd < 16) {
        return -3;
    }
    if (trace) {
        printf("[seed0root] %s vmo fd=%d map_size=%llu\n", label, fd, (unsigned long long)map_size);
        fflush(stdout);
    }
    unsigned char *mapped = pacha_mmap(fd, map_size, PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (mapped == NULL) {
        (void)pacha_fd_close(fd);
        return -4;
    }
    if (trace) {
        printf("[seed0root] %s mapped\n", label);
        fflush(stdout);
    }
    memset(mapped, 0, (size_t)map_size);
    memcpy(mapped, data, (size_t)size);
    (void)pacha_munmap(mapped, map_size);
    const int inherit_status = mark_fd_inherit(fd, label);
    if (inherit_status != 0) {
        (void)pacha_fd_close(fd);
        return -5;
    }
    if (trace) {
        printf("[seed0root] %s inherit ready\n", label);
        fflush(stdout);
    }
    return fd;
}

static int create_inherited_vmo_from_bytes(const void *data, uint64_t size, const char *label)
{
    return create_inherited_vmo_from_bytes_with_extra_rights(data, size, label, 0);
}

static int read_bootstrap_fd(int fd, void *out, uint64_t size, const char *label)
{
    if (fd < 16 || out == NULL || size == 0) {
        return -1;
    }
    const long got = pacha_fd_read(fd, out, size);
    if (got != (long)size) {
        fprintf(stderr, "[seed0root] %s: bootstrap fd read failed fd=%d got=%ld size=%llu\n",
            label,
            fd,
            got,
            (unsigned long long)size);
        return -2;
    }
    return 0;
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

static int find_seed0root_bootstrap_fd(char **argv, int *out_fd)
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

    uint64_t bootstrap_fd = 0;
    const uint64_t *auxv = (const uint64_t *)(const void *)p;
    for (unsigned i = 0; i < 64; i++) {
        const uint64_t type = auxv[i * 2u];
        const uint64_t value = auxv[i * 2u + 1u];
        if (type == 0) {
            break;
        }
        if (type == PACHA_AT_BOOTSTRAP_FD) {
            bootstrap_fd = value;
        }
    }
    if (bootstrap_fd < 16) {
        return -2;
    }
    *out_fd = (int)bootstrap_fd;
    return 0;
}

static const uint64_t seed0root_channel_rights =
    PACHA_FD_RIGHT_INSPECT |
    PACHA_FD_RIGHT_DUP |
    PACHA_FD_RIGHT_WAIT |
    PACHA_FD_RIGHT_POLL |
    PACHA_FD_RIGHT_SET_FLAGS |
    PACHA_FD_RIGHT_CLOSE |
    PACHA_FD_RIGHT_SEND |
    PACHA_FD_RIGHT_RECV |
    PACHA_FD_RIGHT_CALL |
    PACHA_FD_RIGHT_TRANSFER;

static int recv_ipc_wait(int fd, struct pacha_ipc_msg *msg)
{
    if (fd < 16 || msg == NULL) {
        return -1;
    }
    return pacha_ipc_recv_wait(fd, msg, PACHA_FD_WAIT_FOREVER);
}

static void seed0root_dump_filed_error_token(int endpoint_fd, uint64_t token, const char *context);
static void seed0root_dump_lprs_error_token(int endpoint_fd, uint64_t token, const char *context);
static int seed0root_create_filed_page(int *out_fd, void **out_mapped);
static void seed0root_destroy_filed_page(int fd, void *mapped);

static int seed0root_filed_payload_size(uint32_t op, uint32_t *out_payload_size)
{
    if (out_payload_size == NULL) {
        return -22;
    }
    switch (op) {
    case FILED_OP_VFS_OPENAT:
        *out_payload_size = sizeof(filed_path_request_t);
        return 0;
    case FILED_OP_VFS_PREAD:
        *out_payload_size = sizeof(filed_io_t);
        return 0;
    case FILED_OP_VFS_CLOSE:
        *out_payload_size = sizeof(filed_handle_request_t);
        return 0;
    case FILED_OP_EXEC_PATH:
        *out_payload_size = sizeof(filed_exec_path_t);
        return 0;
    case FILED_OP_DIAG_DUMP_METRICS:
        *out_payload_size = 0;
        return 0;
    case FILED_OP_DIAG_SET_CACHE_SLOTS:
        *out_payload_size = sizeof(filed_diag_request_t);
        return 0;
    case FILED_OP_VFS_SYNC_ALL:
        *out_payload_size = 0;
        return 0;
    default:
        return -95;
    }
}

static int seed0root_filed_page_call(
    int endpoint_fd,
    uint32_t op,
    uint64_t request_id,
    int transfer_fd,
    uint64_t word2,
    struct pacha_ipc_msg *out_reply,
    struct pacha_ipc_fd *reply_fds,
    uint64_t reply_fd_capacity)
{
    if (endpoint_fd < 16 || request_id == 0 || out_reply == NULL) {
        return -1;
    }

    uint32_t payload_size = 0;
    int status = seed0root_filed_payload_size(op, &payload_size);
    if (status != 0) {
        return status;
    }

    int owned_page_fd = -1;
    void *owned_page = NULL;
    if (transfer_fd < 16) {
        status = seed0root_create_filed_page(&owned_page_fd, &owned_page);
        if (status != 0) {
            return status;
        }
        transfer_fd = owned_page_fd;
    }
    void *page = owned_page;
    if (page == NULL) {
        page = pacha_mmap(
            transfer_fd,
            FILED_PAGE_BYTES,
            PACHA_PROT_READ | PACHA_PROT_WRITE,
            PACHA_MMAP_SHARED,
            0);
        if (page == NULL) {
            if (owned_page_fd >= 16) {
                seed0root_destroy_filed_page(owned_page_fd, owned_page);
            }
            return -2;
        }
    }

    if (op == FILED_OP_VFS_OPENAT) {
        filed_openat_t openat_payload;
        memcpy(&openat_payload, page, sizeof(openat_payload));
        memset(page, 0, FILED_PAGE_BYTES);
        filed_path_request_t *path =
            (filed_path_request_t *)((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES);
        path->dir_handle = openat_payload.dir_handle;
        path->rights = openat_payload.rights;
        path->flags = openat_payload.open_flags;
        snprintf(path->path, sizeof(path->path), "%s", openat_payload.name);
    } else if (op == FILED_OP_VFS_CLOSE) {
        memset(page, 0, FILED_PAGE_BYTES);
        filed_handle_request_t *handle =
            (filed_handle_request_t *)((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES);
        handle->handle = word2;
    } else if (op == FILED_OP_DIAG_SET_CACHE_SLOTS) {
        memset(page, 0, FILED_PAGE_BYTES);
        filed_diag_request_t *diag =
            (filed_diag_request_t *)((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES);
        diag->subject = word2;
    } else if (payload_size != 0) {
        memmove((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES, page, payload_size);
        memset(page, 0, PACHA_SERVICE_HEADER_BYTES);
    } else {
        memset(page, 0, PACHA_SERVICE_HEADER_BYTES);
    }

    pacha_service_envelope_t *header = (pacha_service_envelope_t *)page;
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = FILED_SERVICE_ID;
    header->op = op;
    header->flags = payload_size != 0 ? PACHA_SERVICE_FLAG_PAGE_PAYLOAD : 0;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = payload_size;
    header->fd_count = 0;

    struct pacha_ipc_fd fd_item;
    memset(&fd_item, 0, sizeof(fd_item));
    fd_item.fd = (uint64_t)(uint32_t)transfer_fd;
    fd_item.rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    fd_item.flags = 0;
    fd_item.transfer_flags = 0;

    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = 0,
        .word2 = 0,
        .word3 = request_id,
        .fds = &fd_item,
        .fd_count = 1,
    };
    const int reply_fd = pacha_ipc_call(endpoint_fd, &request);
    if (reply_fd < 16) {
        struct pacha_fd_info endpoint_info;
        struct pacha_fd_info transfer_info;
        memset(&endpoint_info, 0, sizeof(endpoint_info));
        memset(&transfer_info, 0, sizeof(transfer_info));
        const int endpoint_info_status = pacha_fd_get_info(endpoint_fd, &endpoint_info);
        const int transfer_info_status = transfer_fd >= 16 ?
            pacha_fd_get_info(transfer_fd, &transfer_info) :
            -22;
        fprintf(stderr,
            "[seed0root] filed call failed op=%llu request=0x%llx endpoint_fd=%d endpoint_info=%d kind=%llu rights=0x%llx flags=0x%llx transfer_fd=%d transfer_info=%d kind=%llu rights=0x%llx flags=0x%llx result=%d\n",
            (unsigned long long)op,
            (unsigned long long)request_id,
            endpoint_fd,
            endpoint_info_status,
            (unsigned long long)endpoint_info.kind,
            (unsigned long long)endpoint_info.rights,
            (unsigned long long)endpoint_info.flags,
            transfer_fd,
            transfer_info_status,
            (unsigned long long)transfer_info.kind,
            (unsigned long long)transfer_info.rights,
            (unsigned long long)transfer_info.flags,
            reply_fd);
        if (page != owned_page) {
            (void)pacha_munmap(page, FILED_PAGE_BYTES);
        }
        if (owned_page_fd >= 16) {
            seed0root_destroy_filed_page(owned_page_fd, owned_page);
        }
        return reply_fd;
    }

    memset(out_reply, 0, sizeof(*out_reply));
    out_reply->fds = reply_fds;
    out_reply->fd_capacity = reply_fd_capacity;
    const int recv_status = recv_ipc_wait(reply_fd, out_reply);
    (void)pacha_fd_close(reply_fd);
    if (recv_status != 0) {
        if (page != owned_page) {
            (void)pacha_munmap(page, FILED_PAGE_BYTES);
        }
        if (owned_page_fd >= 16) {
            seed0root_destroy_filed_page(owned_page_fd, owned_page);
        }
        return recv_status;
    }
    const pacha_service_envelope_t *reply_header = (const pacha_service_envelope_t *)page;
    if (out_reply->word0 != PACHA_SERVICE_REPLY_MAGIC ||
        out_reply->word3 != request_id ||
        reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        reply_header->service_id != FILED_SERVICE_ID ||
        reply_header->op != op ||
        reply_header->request_id != request_id)
    {
        if (page != owned_page) {
            (void)pacha_munmap(page, FILED_PAGE_BYTES);
        }
        if (owned_page_fd >= 16) {
            seed0root_destroy_filed_page(owned_page_fd, owned_page);
        }
        return -2;
    }
    out_reply->word1 = (uint64_t)reply_header->status;
    out_reply->word2 = reply_header->result;
    if (reply_header->status < 0) {
        if (reply_header->result != 0) {
            seed0root_dump_filed_error_token(endpoint_fd, reply_header->result, "filed call");
        }
        fprintf(stderr,
            "[seed0root] filed negative reply op=%llu request=0x%llx status=%lld result=%llu fd_count=%llu\n",
            (unsigned long long)op,
            (unsigned long long)request_id,
            (long long)reply_header->status,
            (unsigned long long)reply_header->result,
            (unsigned long long)out_reply->fd_count);
        fflush(stderr);
        status = (int)reply_header->status;
        if (page != owned_page) {
            (void)pacha_munmap(page, FILED_PAGE_BYTES);
        }
        if (owned_page_fd >= 16) {
            seed0root_destroy_filed_page(owned_page_fd, owned_page);
        }
        return status;
    }
    if (op != FILED_OP_VFS_OPENAT &&
        op != FILED_OP_VFS_CLOSE &&
        op != FILED_OP_DIAG_SET_CACHE_SLOTS &&
        op != FILED_OP_EXEC_PATH &&
        payload_size != 0)
    {
        memmove(page, (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES, payload_size);
    }
    if (page != owned_page) {
        (void)pacha_munmap(page, FILED_PAGE_BYTES);
    }
    if (owned_page_fd >= 16) {
        seed0root_destroy_filed_page(owned_page_fd, owned_page);
    }
    return 0;
}

static int seed0root_filed_page_call_fdv(
    int endpoint_fd,
    uint32_t op,
    uint64_t request_id,
    const struct pacha_ipc_fd *fds,
    uint64_t fd_count,
    uint64_t word2,
    struct pacha_ipc_msg *out_reply,
    struct pacha_ipc_fd *reply_fds,
    uint64_t reply_fd_capacity)
{
    if (endpoint_fd < 16 || request_id == 0 || out_reply == NULL) {
        return -1;
    }
    (void)word2;
    uint32_t payload_size = 0;
    int status = seed0root_filed_payload_size(op, &payload_size);
    if (status != 0 || fd_count == 0 || fds == NULL || fds[0].fd < 16) {
        return status != 0 ? status : -22;
    }

    void *page = pacha_mmap(
        (int)(uint32_t)fds[0].fd,
        FILED_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (page == NULL) {
        return -2;
    }
    if (payload_size != 0) {
        memmove((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES, page, payload_size);
        memset(page, 0, PACHA_SERVICE_HEADER_BYTES);
    } else {
        memset(page, 0, PACHA_SERVICE_HEADER_BYTES);
    }
    pacha_service_envelope_t *header = (pacha_service_envelope_t *)page;
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = FILED_SERVICE_ID;
    header->op = op;
    header->flags = payload_size != 0 ? PACHA_SERVICE_FLAG_PAGE_PAYLOAD : 0;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = payload_size;
    header->fd_count = fd_count > 0 ? (uint32_t)(fd_count - 1u) : 0u;

    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = 0,
        .word2 = 0,
        .word3 = request_id,
        .fds = (struct pacha_ipc_fd *)fds,
        .fd_count = fd_count,
    };
    const int reply_fd = pacha_ipc_call(endpoint_fd, &request);
    if (reply_fd < 16) {
        (void)pacha_munmap(page, FILED_PAGE_BYTES);
        return reply_fd;
    }
    memset(out_reply, 0, sizeof(*out_reply));
    out_reply->fds = reply_fds;
    out_reply->fd_capacity = reply_fd_capacity;
    const int recv_status = recv_ipc_wait(reply_fd, out_reply);
    (void)pacha_fd_close(reply_fd);
    if (recv_status != 0) {
        (void)pacha_munmap(page, FILED_PAGE_BYTES);
        return recv_status;
    }
    const pacha_service_envelope_t *reply_header = (const pacha_service_envelope_t *)page;
    if (out_reply->word0 != PACHA_SERVICE_REPLY_MAGIC ||
        out_reply->word3 != request_id ||
        reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        reply_header->service_id != FILED_SERVICE_ID ||
        reply_header->op != op ||
        reply_header->request_id != request_id)
    {
        (void)pacha_munmap(page, FILED_PAGE_BYTES);
        return -2;
    }
    out_reply->word1 = (uint64_t)reply_header->status;
    out_reply->word2 = reply_header->result;
    if (reply_header->status < 0) {
        if (reply_header->result != 0) {
            seed0root_dump_filed_error_token(endpoint_fd, reply_header->result, "filed fdv call");
        }
        status = (int)reply_header->status;
        (void)pacha_munmap(page, FILED_PAGE_BYTES);
        return status;
    }
    if (op != FILED_OP_EXEC_PATH && payload_size != 0) {
        memmove(page, (uint8_t *)page + PACHA_SERVICE_HEADER_BYTES, payload_size);
    }
    (void)pacha_munmap(page, FILED_PAGE_BYTES);
    return 0;
}

static int seed0root_filed_service_call(
    int endpoint_fd,
    uint32_t op,
    uint64_t request_id,
    uint32_t payload_size,
    int transfer_fd,
    struct pacha_ipc_msg *out_reply,
    pacha_service_envelope_t *out_header)
{
    if (endpoint_fd < 16 || request_id == 0 || out_reply == NULL ||
        payload_size > PACHA_SERVICE_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES)
    {
        return -1;
    }

    int page_fd = -1;
    void *page = NULL;
    int status = seed0root_create_filed_page(&page_fd, &page);
    if (status != 0) {
        return status;
    }

    pacha_service_envelope_t *header = (pacha_service_envelope_t *)page;
    memset(header, 0, sizeof(*header));
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = FILED_SERVICE_ID;
    header->op = op;
    header->flags = payload_size != 0 ? PACHA_SERVICE_FLAG_PAGE_PAYLOAD : 0;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = payload_size;
    header->fd_count = transfer_fd >= 16 ? 1u : 0u;

    if (payload_size >= sizeof(filed_service_endpoint_request_t)) {
        filed_service_endpoint_request_t *payload =
            (filed_service_endpoint_request_t *)((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES);
        payload->endpoint_kind = op;
    }

    struct pacha_ipc_fd fds[2];
    uint64_t fd_count = 0;
    memset(fds, 0, sizeof(fds));
    fds[fd_count].fd = (uint64_t)(uint32_t)page_fd;
    fds[fd_count].rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    fd_count++;
    if (transfer_fd >= 16) {
        fds[fd_count].fd = (uint64_t)(uint32_t)transfer_fd;
        fds[fd_count].rights = seed0root_channel_rights;
        fd_count++;
    }

    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = 0,
        .word2 = 0,
        .word3 = request_id,
        .fds = fds,
        .fd_count = fd_count,
    };
    const int reply_fd = pacha_ipc_call(endpoint_fd, &request);
    if (reply_fd < 16) {
        seed0root_destroy_filed_page(page_fd, page);
        return reply_fd;
    }

    memset(out_reply, 0, sizeof(*out_reply));
    status = recv_ipc_wait(reply_fd, out_reply);
    (void)pacha_fd_close(reply_fd);
    if (status != 0) {
        seed0root_destroy_filed_page(page_fd, page);
        return status;
    }

    const pacha_service_envelope_t *reply_header =
        (const pacha_service_envelope_t *)page;
    if (out_reply->word0 != PACHA_SERVICE_REPLY_MAGIC ||
        out_reply->word3 != request_id ||
        reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        reply_header->service_id != FILED_SERVICE_ID ||
        reply_header->op != op ||
        reply_header->request_id != request_id)
    {
        seed0root_destroy_filed_page(page_fd, page);
        return -2;
    }
    if (out_header != NULL) {
        memcpy(out_header, reply_header, sizeof(*out_header));
    }
    if (reply_header->status < 0) {
        if (reply_header->result != 0) {
            seed0root_dump_filed_error_token(endpoint_fd, reply_header->result, "filed call");
        }
        status = (int)reply_header->status;
        seed0root_destroy_filed_page(page_fd, page);
        return status;
    }

    seed0root_destroy_filed_page(page_fd, page);
    return 0;
}

static int seed0root_lprs_call(
    int endpoint_fd,
    uint32_t op,
    uint64_t request_id,
    int page_fd,
    void *page,
    uint32_t payload_size,
    int transfer_fd,
    struct pacha_ipc_msg *out_reply)
{
    if (endpoint_fd < 16 || request_id == 0 || out_reply == NULL ||
        payload_size > LPRS_PAYLOAD_BYTES)
    {
        return -1;
    }
    int owned_page_fd = -1;
    void *owned_page = NULL;
    if (page_fd < 16 || page == NULL) {
        const int create_status = seed0root_create_filed_page(&owned_page_fd, &owned_page);
        if (create_status != 0) {
            return create_status;
        }
        page_fd = owned_page_fd;
        page = owned_page;
    }

    pacha_service_envelope_t *header = (pacha_service_envelope_t *)page;
    memset(header, 0, sizeof(*header));
    header->magic = PACHA_SERVICE_REQUEST_MAGIC;
    header->abi_version = PACHA_SERVICE_ABI_VERSION;
    header->service_id = LPRS_SERVICE_ID;
    header->op = op;
    header->flags = payload_size != 0 ? PACHA_SERVICE_FLAG_PAGE_PAYLOAD : 0;
    header->request_id = request_id;
    header->trace_id = request_id;
    header->payload_size = payload_size;
    header->fd_count = transfer_fd >= 16 ? 1u : 0u;

    struct pacha_ipc_fd fds[2];
    uint64_t fd_count = 0;
    memset(fds, 0, sizeof(fds));
    fds[fd_count].fd = (uint64_t)(uint32_t)page_fd;
    fds[fd_count].rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    fd_count++;
    if (transfer_fd >= 16) {
        fds[fd_count].fd = (uint64_t)(uint32_t)transfer_fd;
        fds[fd_count].rights =
            PACHA_FD_RIGHT_INSPECT |
            PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_WAIT |
            PACHA_FD_RIGHT_POLL |
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_KILL;
        fd_count++;
    }

    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC,
        .word1 = 0,
        .word2 = 0,
        .word3 = request_id,
        .fds = fds,
        .fd_count = fd_count,
    };
    const int reply_fd = pacha_ipc_call(endpoint_fd, &request);
    if (reply_fd < 16) {
        if (owned_page_fd >= 16) {
            seed0root_destroy_filed_page(owned_page_fd, owned_page);
        }
        return reply_fd;
    }

    memset(out_reply, 0, sizeof(*out_reply));
    const int recv_status = recv_ipc_wait(reply_fd, out_reply);
    (void)pacha_fd_close(reply_fd);
    if (recv_status != 0) {
        if (owned_page_fd >= 16) {
            seed0root_destroy_filed_page(owned_page_fd, owned_page);
        }
        return recv_status;
    }

    const pacha_service_envelope_t *reply_header =
        (const pacha_service_envelope_t *)page;
    if (out_reply->word0 != PACHA_SERVICE_REPLY_MAGIC ||
        out_reply->word3 != request_id ||
        reply_header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        reply_header->request_id != request_id)
    {
        if (owned_page_fd >= 16) {
            seed0root_destroy_filed_page(owned_page_fd, owned_page);
        }
        return -2;
    }
    if ((int64_t)out_reply->word1 < 0) {
        if (out_reply->word2 != 0) {
            seed0root_dump_lprs_error_token(
                endpoint_fd,
                out_reply->word2,
                "lpr supervisor call");
        }
        const int status = (int)(int64_t)out_reply->word1;
        if (owned_page_fd >= 16) {
            seed0root_destroy_filed_page(owned_page_fd, owned_page);
        }
        return status;
    }
    if (owned_page_fd >= 16) {
        seed0root_destroy_filed_page(owned_page_fd, owned_page);
    }
    return 0;
}

static int seed0root_dump_filed_metrics(int filed_endpoint_fd)
{
    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    return seed0root_filed_page_call(
        filed_endpoint_fd,
        FILED_OP_DIAG_DUMP_METRICS,
        0x5eed0f12u,
        -1,
        0,
        &reply,
        NULL,
        0);
}

static int seed0root_filed_sync_all(int filed_endpoint_fd)
{
    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    return seed0root_filed_page_call(
        filed_endpoint_fd,
        FILED_OP_VFS_SYNC_ALL,
        0x5eed0f16u,
        -1,
        0,
        &reply,
        NULL,
        0);
}

static int seed0root_set_filed_cache_slots(int filed_endpoint_fd, uint64_t slots)
{
    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    const int status = seed0root_filed_page_call(
        filed_endpoint_fd,
        FILED_OP_DIAG_SET_CACHE_SLOTS,
        0x5eed0f10u,
        -1,
        slots,
        &reply,
        NULL,
        0);
    if (status == 0) {
        printf("[seed0root] filed cache slots=%llu\n", (unsigned long long)reply.word2);
    }
    return status;
}

static int seed0root_run_filed_no_cache_probe(int filed_endpoint_fd)
{
    if (filed_endpoint_fd < 16) {
        return -1;
    }

    int page_fd = -1;
    void *page = NULL;
    int status = seed0root_set_filed_cache_slots(filed_endpoint_fd, 0);
    if (status != 0) {
        fprintf(stderr, "[seed0root] filed cache disable failed status=%d\n", status);
        return status;
    }

    status = seed0root_create_filed_page(&page_fd, &page);
    if (status != 0) {
        return status;
    }

    filed_openat_t *openat = (filed_openat_t *)page;
    openat->dir_handle = 0;
    openat->rights =
        FILED_RIGHT_READ |
        FILED_RIGHT_STAT |
        FILED_RIGHT_EXEC;
    openat->open_flags = FILED_OPEN_CLOEXEC;
    snprintf(openat->name, sizeof(openat->name), "%s", "/cmd/libc_vfs_exec_smoke.elf");

    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    status = seed0root_filed_page_call(
        filed_endpoint_fd,
        FILED_OP_VFS_OPENAT,
        0x5eed0f13u,
        page_fd,
        0,
        &reply,
        NULL,
        0);
    seed0root_destroy_filed_page(page_fd, page);
    page_fd = -1;
    page = NULL;
    if (status != 0) {
        fprintf(stderr, "[seed0root] filed no-cache open failed status=%d\n", status);
        (void)seed0root_set_filed_cache_slots(filed_endpoint_fd, 64);
        return status;
    }

    const uint64_t handle = reply.word2;
    status = seed0root_create_filed_page(&page_fd, &page);
    if (status != 0) {
        (void)seed0root_filed_page_call(
            filed_endpoint_fd,
            FILED_OP_VFS_CLOSE,
            0x5eed0f15u,
            -1,
            handle,
            &reply,
            NULL,
            0);
        (void)seed0root_set_filed_cache_slots(filed_endpoint_fd, 64);
        return status;
    }

    filed_io_t *io = (filed_io_t *)page;
    io->handle = handle;
    io->offset = 0;
    io->length = 4;
    memset(&reply, 0, sizeof(reply));
    status = seed0root_filed_page_call(
        filed_endpoint_fd,
        FILED_OP_VFS_PREAD,
        0x5eed0f14u,
        page_fd,
        0,
        &reply,
        NULL,
        0);

    int probe_status = status;
    if (probe_status == 0 &&
        (reply.word2 != 4 ||
            io->data[0] != 0x7f ||
            io->data[1] != 'E' ||
            io->data[2] != 'L' ||
            io->data[3] != 'F'))
    {
        fprintf(stderr,
            "[seed0root] filed no-cache probe invalid result bytes=%llu magic=%02x %02x %02x %02x\n",
            (unsigned long long)reply.word2,
            io->data[0],
            io->data[1],
            io->data[2],
            io->data[3]);
        probe_status = -2;
    }
    seed0root_destroy_filed_page(page_fd, page);

    memset(&reply, 0, sizeof(reply));
    const int close_status = seed0root_filed_page_call(
        filed_endpoint_fd,
        FILED_OP_VFS_CLOSE,
        0x5eed0f15u,
        -1,
        handle,
        &reply,
        NULL,
        0);
    const int restore_status = seed0root_set_filed_cache_slots(filed_endpoint_fd, 64);
    if (probe_status == 0) {
        probe_status = close_status;
    }
    if (probe_status == 0) {
        probe_status = restore_status;
    }
    printf("[seed0root] filed no-cache probe status=%d\n", probe_status);
    return probe_status;
}

static int seed0root_read_filed_text(
    int filed_endpoint_fd,
    const char *path,
    char *out,
    size_t out_capacity)
{
    if (filed_endpoint_fd < 16 || path == NULL || out == NULL || out_capacity == 0) {
        return -1;
    }
    out[0] = '\0';

    int page_fd = -1;
    void *page = NULL;
    int status = seed0root_create_filed_page(&page_fd, &page);
    if (status != 0) {
        return status;
    }

    filed_openat_t *openat = (filed_openat_t *)page;
    openat->dir_handle = 0;
    openat->rights = FILED_RIGHT_READ | FILED_RIGHT_STAT;
    openat->open_flags = FILED_OPEN_CLOEXEC;
    snprintf(openat->name, sizeof(openat->name), "%s", path);

    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    status = seed0root_filed_page_call(
        filed_endpoint_fd,
        FILED_OP_VFS_OPENAT,
        0x5eed0f21u,
        page_fd,
        0,
        &reply,
        NULL,
        0);
    seed0root_destroy_filed_page(page_fd, page);
    page_fd = -1;
    page = NULL;
    if (status != 0) {
        return status;
    }

    const uint64_t handle = reply.word2;
    status = seed0root_create_filed_page(&page_fd, &page);
    if (status != 0) {
        (void)seed0root_filed_page_call(
            filed_endpoint_fd,
            FILED_OP_VFS_CLOSE,
            0x5eed0f23u,
            -1,
            handle,
            &reply,
            NULL,
            0);
        return status;
    }

    filed_io_t *io = (filed_io_t *)page;
    io->handle = handle;
    io->offset = 0;
    io->length = out_capacity - 1;
    memset(&reply, 0, sizeof(reply));
    status = seed0root_filed_page_call(
        filed_endpoint_fd,
        FILED_OP_VFS_PREAD,
        0x5eed0f22u,
        page_fd,
        0,
        &reply,
        NULL,
        0);
    if (status == 0) {
        size_t copied = (size_t)reply.word2;
        if (copied >= out_capacity) {
            copied = out_capacity - 1;
        }
        memcpy(out, io->data, copied);
        out[copied] = '\0';
    }
    seed0root_destroy_filed_page(page_fd, page);

    memset(&reply, 0, sizeof(reply));
    const int close_status = seed0root_filed_page_call(
        filed_endpoint_fd,
        FILED_OP_VFS_CLOSE,
        0x5eed0f23u,
        -1,
        handle,
        &reply,
        NULL,
        0);
    return status == 0 ? close_status : status;
}

static int seed0root_profile_has_token(const char *profile, const char *token)
{
    if (profile == NULL || token == NULL || token[0] == '\0') {
        return 0;
    }
    const size_t token_len = strlen(token);
    const char *p = profile;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') {
            p++;
        }
        const char *start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != ',') {
            p++;
        }
        if ((size_t)(p - start) == token_len && memcmp(start, token, token_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static unsigned seed0root_boot_profile_flags(int filed_endpoint_fd)
{
    unsigned flags = SEED0ROOT_DEFAULT_BOOT_PROFILE;
    char profile[128];
    const int status = seed0root_read_filed_text(
        filed_endpoint_fd,
        "/etc/pacha_boot_profile",
        profile,
        sizeof(profile));
    if (status != 0) {
        return flags;
    }
    if (seed0root_profile_has_token(profile, "all")) {
        flags |=
            SEED0ROOT_BOOT_PROFILE_MEMORY |
            SEED0ROOT_BOOT_PROFILE_BENCH |
            SEED0ROOT_BOOT_PROFILE_FS_WRITE |
            SEED0ROOT_BOOT_PROFILE_LPR;
    }
    if (seed0root_profile_has_token(profile, "memory")) {
        flags |= SEED0ROOT_BOOT_PROFILE_MEMORY;
    }
    if (seed0root_profile_has_token(profile, "bench")) {
        flags |= SEED0ROOT_BOOT_PROFILE_BENCH;
    }
    if (seed0root_profile_has_token(profile, "fs-write")) {
        flags |= SEED0ROOT_BOOT_PROFILE_FS_WRITE;
    }
    if (seed0root_profile_has_token(profile, "lpr")) {
        flags |= SEED0ROOT_BOOT_PROFILE_LPR;
    }
    if (seed0root_profile_has_token(profile, "lua")) {
        flags |= SEED0ROOT_BOOT_PROFILE_LUA;
    }
    if (seed0root_profile_has_token(profile, "dyn-needed")) {
        flags |= SEED0ROOT_BOOT_PROFILE_DYN_NEEDED;
    }
    if (seed0root_profile_has_token(profile, "chibicc")) {
        flags |= SEED0ROOT_BOOT_PROFILE_CHIBICC;
    }
    if (seed0root_profile_has_token(profile, "apk")) {
        flags |= SEED0ROOT_BOOT_PROFILE_APK;
    }
    if (seed0root_profile_has_token(profile, "apk-update")) {
        flags |= SEED0ROOT_BOOT_PROFILE_APK_UPDATE;
    }
    if (seed0root_profile_has_token(profile, "curl")) {
        flags |= SEED0ROOT_BOOT_PROFILE_CURL;
    }
    if (seed0root_profile_has_token(profile, "https") ||
        seed0root_profile_has_token(profile, "curl-https")) {
        flags |= SEED0ROOT_BOOT_PROFILE_HTTPS;
    }
    printf("[seed0root] boot profile flags=%u\n", flags);
    return flags;
}

static int seed0root_wait_process(int process_fd, const char *label, struct seed0root_wait_result *out_result)
{
    if (process_fd < 16) {
        return -1;
    }
    if (out_result != NULL) {
        memset(out_result, 0, sizeof(*out_result));
    }
    const int quiet_bench = seed0root_starts_with(label, "lpr busybox ");
    const uint64_t start_ns = seed0root_now_ns();
    const uint64_t start_cycles = seed0root_read_tsc();
    uint64_t status_words[4] = {0, 0, 0, 0};
    for (;;) {
        const long wait_status = pacha_syscall2(
            PACHA_PROCESS_SYSCALL_WAIT,
            (uint64_t)(uint32_t)process_fd,
            (uint64_t)(uintptr_t)status_words);
        if (wait_status == 0) {
            const uint64_t state = status_words[0];
            const uint64_t exit_code = status_words[1];
            const uint64_t end_ns = seed0root_now_ns();
            const uint64_t elapsed_ns =
                (start_ns != 0 && end_ns >= start_ns) ? end_ns - start_ns : 0;
            const uint64_t end_cycles = seed0root_read_tsc();
            const uint64_t elapsed_cycles =
                (start_cycles != 0 && end_cycles >= start_cycles) ? end_cycles - start_cycles : 0;
            if (out_result != NULL) {
                out_result->state = state;
                out_result->exit_code = exit_code;
                out_result->end_ns = end_ns;
                out_result->end_cycles = end_cycles;
                out_result->elapsed_ns = elapsed_ns;
                out_result->elapsed_cycles = elapsed_cycles;
            }
            if (!quiet_bench) {
                printf("[seed0root] %s completed state=%llu exit=%llu ns=%llu cycles=%llu\n",
                    label != NULL ? label : "process",
                    (unsigned long long)state,
                    (unsigned long long)exit_code,
                    (unsigned long long)elapsed_ns,
                    (unsigned long long)elapsed_cycles);
            }
            if (state == SEED0ROOT_TASK_STATE_EXITED && exit_code == 0) {
                return 0;
            }
            return -5;
        }
        if (wait_status != PACHA_SYSCALL_ERR_NOT_READY &&
            wait_status != PACHA_ERR_NOT_READY)
        {
            fprintf(stderr,
                "[seed0root] %s wait failed status=%ld\n",
                label != NULL ? label : "process",
                wait_status);
            return -(int)wait_status;
        }

        struct pacha_pollfd pollfd = {
            .fd = process_fd,
            .events = PACHA_FD_EVENT_READABLE,
            .revents = 0,
        };
        (void)pacha_fd_wait_many(&pollfd, 1, PACHA_FD_WAIT_FOREVER);
    }
}

static int seed0root_create_wire_page(uint64_t size, int *out_fd, void **out_mapped)
{
    if (size == 0 || out_fd == NULL || out_mapped == NULL) {
        return -1;
    }
    *out_fd = -1;
    *out_mapped = NULL;
    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int fd = pacha_vmo_create(size, rights, 0);
    if (fd < 16) {
        fprintf(stderr,
            "[seed0root] wire page create failed bytes=%llu status=%d\n",
            (unsigned long long)size,
            fd);
        return fd;
    }
    const long map_result = pacha_syscall6(
        PACHA_VM_SYSCALL_MMAP,
        (uint64_t)(uint32_t)fd,
        0,
        size,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    void *mapped = map_result < 4096 ? NULL : (void *)(uintptr_t)map_result;
    if (mapped == NULL) {
        fprintf(stderr,
            "[seed0root] wire page map failed bytes=%llu status=%ld\n",
            (unsigned long long)size,
            map_result);
        (void)pacha_fd_close(fd);
        return -2;
    }
    memset(mapped, 0, (size_t)size);
    *out_fd = fd;
    *out_mapped = mapped;
    return 0;
}

static void seed0root_destroy_wire_page(uint64_t size, int fd, void *mapped)
{
    if (mapped != NULL) {
        (void)pacha_munmap(mapped, size);
    }
    if (fd >= 16) {
        (void)pacha_fd_close(fd);
    }
}

static int seed0root_create_filed_page(int *out_fd, void **out_mapped)
{
    return seed0root_create_wire_page(FILED_PAGE_BYTES, out_fd, out_mapped);
}

static void seed0root_destroy_filed_page(int fd, void *mapped)
{
    seed0root_destroy_wire_page(FILED_PAGE_BYTES, fd, mapped);
}

static void seed0root_dump_filed_error_token(int endpoint_fd, uint64_t token, const char *context)
{
    (void)endpoint_fd;
    (void)token;
    fprintf(stderr,
        "[seed0root] filed negative reply context=%s; detailed error trace is emitted by service\n",
        context != NULL ? context : "unknown");
}

static void seed0root_dump_lprs_error_token(int endpoint_fd, uint64_t token, const char *context)
{
    (void)endpoint_fd;
    (void)token;
    fprintf(stderr,
        "[seed0root] lprs negative reply context=%s; detailed error trace is emitted by service\n",
        context != NULL ? context : "unknown");
}

static int seed0root_exec_add_string(
    filed_exec_path_t *exec,
    filed_exec_string_ref_t *ref,
    const char *value)
{
    if (exec == NULL || ref == NULL || value == NULL) {
        return -22;
    }
    const uint64_t length = (uint64_t)strlen(value) + 1u;
    if (length == 0 ||
        length > UINT16_MAX ||
        exec->string_bytes + length > FILED_EXEC_STRING_BYTES)
    {
        return -7;
    }
    ref->offset = (uint16_t)exec->string_bytes;
    ref->length = (uint16_t)length;
    memcpy(exec->strings + exec->string_bytes, value, (size_t)length);
    exec->string_bytes += length;
    return 0;
}

static int seed0root_exit_code_expected(uint64_t exit_code, const uint64_t *expected_exits, uint64_t expected_count)
{
    if (expected_exits == NULL || expected_count == 0) {
        return exit_code == 0;
    }
    for (uint64_t i = 0; i < expected_count; i++) {
        if (exit_code == expected_exits[i]) {
            return 1;
        }
    }
    return 0;
}

static int seed0root_run_exec_path_smoke_expect_any(
    int filed_endpoint_fd,
    const char *path,
    const char *const *argv,
    uint64_t argc,
    const char *env,
    const char *label,
    uint64_t exec_flags,
    const uint64_t *expected_exits,
    uint64_t expected_count)
{
    if (filed_endpoint_fd < 16 || path == NULL || label == NULL ||
        argc > FILED_EXEC_MAX_ARGS)
    {
        return -1;
    }

    int page_fd = -1;
    void *page = NULL;
    int status = seed0root_create_filed_page(&page_fd, &page);
    if (status != 0) {
        return status;
    }

    filed_exec_path_t *exec = (filed_exec_path_t *)page;
    exec->dir_handle = 0;
    exec->flags = exec_flags;
    exec->argc = argc == 0 ? 1 : argc;
    exec->envc = env != NULL ? 1 : 0;
    snprintf(exec->path, sizeof(exec->path), "%s", path);
    for (uint64_t i = 0; i < exec->argc; i++) {
        const char *arg = (argc > 0 && argv != NULL && argv[i] != NULL) ? argv[i] : path;
        status = seed0root_exec_add_string(exec, &exec->argv[i], arg);
        if (status != 0) {
            seed0root_destroy_filed_page(page_fd, page);
            return status;
        }
    }
    if (env != NULL) {
        status = seed0root_exec_add_string(exec, &exec->envp[0], env);
        if (status != 0) {
            seed0root_destroy_filed_page(page_fd, page);
            return status;
        }
    }

    const uint64_t exec_start_ns = seed0root_now_ns();
    const uint64_t exec_start_cycles = seed0root_read_tsc();
    struct pacha_ipc_fd reply_fds[2];
    memset(reply_fds, 0, sizeof(reply_fds));
    struct pacha_ipc_msg reply;
    status = seed0root_filed_page_call(
        filed_endpoint_fd,
        FILED_OP_EXEC_PATH,
        0x5eed0f11u,
        page_fd,
        0,
        &reply,
        reply_fds,
        2);
    const uint64_t exec_reply_ns = seed0root_now_ns();
    const uint64_t exec_reply_cycles = seed0root_read_tsc();
    seed0root_destroy_filed_page(page_fd, page);
    if (status == -2) {
        return 0;
    }
    if (status != 0) {
        fprintf(stderr, "[seed0root] %s failed status=%d\n", label, status);
        return status;
    }
    if (reply.fd_count < 2 || reply_fds[0].fd < 16 || reply_fds[1].fd < 16) {
        fprintf(stderr,
            "[seed0root] %s reply invalid fd_count=%llu process_fd=%llu thread_fd=%llu\n",
            label,
            (unsigned long long)reply.fd_count,
            (unsigned long long)reply_fds[0].fd,
            (unsigned long long)reply_fds[1].fd);
        if (reply_fds[1].fd >= 16) {
            (void)pacha_fd_close((int)reply_fds[1].fd);
        }
        if (reply_fds[0].fd >= 16) {
            (void)pacha_fd_close((int)reply_fds[0].fd);
        }
        return -2;
    }

    const int process_fd = (int)reply_fds[0].fd;
    const int thread_fd = (int)reply_fds[1].fd;
    const int quiet_bench = seed0root_starts_with(label, "lpr busybox ");
    if (!quiet_bench) {
        printf("[seed0root] %s started process_fd=%d thread_fd=%d\n", label, process_fd, thread_fd);
    }

    struct seed0root_wait_result wait_result;
    status = seed0root_wait_process(process_fd, label, &wait_result);
    if (status == -5 &&
        wait_result.state == SEED0ROOT_TASK_STATE_EXITED &&
        seed0root_exit_code_expected(wait_result.exit_code, expected_exits, expected_count))
    {
        status = 0;
    }
    if (quiet_bench) {
        const uint64_t exec_end_ns = wait_result.end_ns != 0 ? wait_result.end_ns : seed0root_now_ns();
        const uint64_t exec_end_cycles = wait_result.end_cycles != 0 ? wait_result.end_cycles : seed0root_read_tsc();
        const uint64_t elapsed_ns =
            (exec_start_ns != 0 && exec_end_ns >= exec_start_ns) ? exec_end_ns - exec_start_ns : 0;
        const uint64_t reply_ns =
            (exec_start_ns != 0 && exec_reply_ns >= exec_start_ns) ? exec_reply_ns - exec_start_ns : 0;
        const uint64_t wait_ns =
            (exec_reply_ns != 0 && exec_end_ns >= exec_reply_ns) ? exec_end_ns - exec_reply_ns : 0;
        const uint64_t elapsed_cycles =
            (exec_start_cycles != 0 && exec_end_cycles >= exec_start_cycles) ? exec_end_cycles - exec_start_cycles : 0;
        const uint64_t reply_cycles =
            (exec_start_cycles != 0 && exec_reply_cycles >= exec_start_cycles) ? exec_reply_cycles - exec_start_cycles : 0;
        const uint64_t wait_cycles =
            (exec_reply_cycles != 0 && exec_end_cycles >= exec_reply_cycles) ? exec_end_cycles - exec_reply_cycles : 0;
        printf("[seed0root] %s exec_to_exit ns=%llu us=%llu exec_reply_us=%llu wait_after_reply_us=%llu cycles=%llu exec_reply_cycles=%llu wait_after_reply_cycles=%llu status=%d\n",
            label,
            (unsigned long long)elapsed_ns,
            (unsigned long long)(elapsed_ns / 1000ull),
            (unsigned long long)(reply_ns / 1000ull),
            (unsigned long long)(wait_ns / 1000ull),
            (unsigned long long)elapsed_cycles,
            (unsigned long long)reply_cycles,
            (unsigned long long)wait_cycles,
            status);
    }
    (void)pacha_fd_close(thread_fd);
    (void)pacha_fd_close(process_fd);
    return status;
}

static int seed0root_run_exec_path_smoke_expect(
    int filed_endpoint_fd,
    const char *path,
    const char *const *argv,
    uint64_t argc,
    const char *env,
    const char *label,
    uint64_t exec_flags,
    uint64_t expected_exit)
{
    const uint64_t expected_exits[] = {expected_exit};
    return seed0root_run_exec_path_smoke_expect_any(
        filed_endpoint_fd,
        path,
        argv,
        argc,
        env,
        label,
        exec_flags,
        expected_exits,
        1);
}

static int seed0root_run_exec_path_smoke(
    int filed_endpoint_fd,
    const char *path,
    const char *const *argv,
    uint64_t argc,
    const char *env,
    const char *label,
    uint64_t exec_flags)
{
    return seed0root_run_exec_path_smoke_expect(
        filed_endpoint_fd,
        path,
        argv,
        argc,
        env,
        label,
        exec_flags,
        0);
}

static int seed0root_run_libc_vfs_exec_smoke(int filed_endpoint_fd)
{
    const char *argv[] = { "/cmd/libc_vfs_exec_smoke.elf" };
    return seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/libc_vfs_exec_smoke.elf",
        argv,
        1,
        "PACHA_LIBC_VFS_EXEC_PARENT=1",
        "libc vfs exec smoke",
        0);
}

static int seed0root_run_libc_mix_bench(int filed_endpoint_fd)
{
    const char *argv[] = { "/cmd/libc_mix_bench.elf" };
    return seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/libc_mix_bench.elf",
        argv,
        1,
        "PACHA_LIBC_MIX_BENCH=1",
        "libc mix bench",
        0);
}

static int seed0root_run_libc_alloc_probe(int filed_endpoint_fd)
{
    const char *argv[] = { "/cmd/libc_alloc_probe.elf" };
    return seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/libc_alloc_probe.elf",
        argv,
        1,
        "PACHA_LIBC_ALLOC_PROBE=1",
        "libc alloc probe",
        0);
}

static int seed0root_run_lpr_minimal_smoke(int filed_endpoint_fd)
{
    const char *argv[] = { "/cmd/lpr_minimal_linux.elf" };
    return seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/lpr_minimal_linux.elf",
        argv,
        1,
        NULL,
        "lpr minimal smoke",
        FILED_EXEC_LINUX_LPR);
}

static int seed0root_run_lpr_ldmusl_smoke(int filed_endpoint_fd)
{
    const char *argv[] = {
        "/cmd/lpr_ldmusl_smoke.elf",
        "--self",
        "/cmd/lpr_ldmusl_smoke.elf",
        "--write",
        "/tmp/lpr_cli_out.txt",
    };
    return seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/lpr_ldmusl_smoke.elf",
        argv,
        5,
        "LD_LIBRARY_PATH=/lib/linux",
        "lpr ld-musl smoke",
        FILED_EXEC_LINUX_LPR);
}

static int seed0root_run_lpr_pty_probe(int filed_endpoint_fd)
{
    const char *argv[] = { "/cmd/lpr_pty_probe.elf" };
    return seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/lpr_pty_probe.elf",
        argv,
        1,
        "LD_LIBRARY_PATH=/lib/linux",
        "lpr pty probe",
        FILED_EXEC_LINUX_LPR);
}

static int seed0root_run_lpr_busybox_command(
    int filed_endpoint_fd,
    const char *label,
    const char *const *argv,
    uint64_t argc)
{
    return seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/alpine-busybox.elf",
        argv,
        argc,
        "LD_LIBRARY_PATH=/lib:/lib/linux",
        label,
        FILED_EXEC_LINUX_LPR);
}

static int seed0root_run_lpr_busybox_cold_echo_smoke(int filed_endpoint_fd)
{
    const char *argv[] = {
        "busybox",
        "echo",
        "lpr-busybox-cold-ok",
    };
    return seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox cold echo", argv, 3);
}

static int seed0root_run_lpr_busybox_dynamic_smoke(int filed_endpoint_fd)
{
    const char *echo_argv[] = {
        "busybox",
        "echo",
        "lpr-busybox-dynamic-ok",
    };
    int status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox echo", echo_argv, 3);
    if (status != 0) {
        return status;
    }

    const char *ls_argv[] = {
        "busybox",
        "ls",
        "/cmd",
    };
    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox ls", ls_argv, 3);
    if (status != 0) {
        return status;
    }

    const char *cat_argv[] = {
        "busybox",
        "cat",
        "/etc/pacha_boot_profile",
    };
    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox cat", cat_argv, 3);
    if (status != 0) {
        return status;
    }

    const char *stty_argv[] = {
        "busybox",
        "stty",
        "-F",
        "/dev/ptmx",
        "-a",
    };
    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox stty ptmx", stty_argv, 5);
    if (status != 0) {
        return status;
    }

    const char *dirname_argv[] = {
        "busybox",
        "dirname",
        "/tmp/lpr-busybox-dir/file.txt",
    };
    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox dirname", dirname_argv, 3);
    if (status != 0) {
        return status;
    }

    const char *basename_argv[] = {
        "busybox",
        "basename",
        "/tmp/lpr-busybox-dir/file.txt",
    };
    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox basename", basename_argv, 3);
    if (status != 0) {
        return status;
    }

    const char *mkdir_argv[] = {
        "busybox",
        "mkdir",
        "/tmp/lpr-busybox-dir",
    };
    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox mkdir", mkdir_argv, 3);
    if (status != 0) {
        return status;
    }

    const char *touch_argv[] = {
        "busybox",
        "touch",
        "/tmp/lpr-busybox-dir/touched.txt",
    };
    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox touch", touch_argv, 3);
    if (status != 0) {
        return status;
    }

    const char *touch_time_argv[] = {
        "busybox",
        "touch",
        "-t",
        "197001020304.05",
        "/tmp/lpr-busybox-dir/touched.txt",
    };
    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox touch time", touch_time_argv, 5);
    if (status != 0) {
        return status;
    }

    const char *stat_touched_argv[] = {
        "busybox",
        "stat",
        "/tmp/lpr-busybox-dir/touched.txt",
    };
    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox stat touched", stat_touched_argv, 3);
    if (status != 0) {
        return status;
    }

    const char *chmod_argv[] = {
        "busybox",
        "chmod",
        "600",
        "/tmp/lpr-busybox-dir/touched.txt",
    };
    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox chmod", chmod_argv, 4);
    if (status != 0) {
        return status;
    }

    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox stat chmod", stat_touched_argv, 3);
    if (status != 0) {
        return status;
    }

    const char *test_touched_argv[] = {
        "busybox",
        "test",
        "-f",
        "/tmp/lpr-busybox-dir/touched.txt",
    };
    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox test touched", test_touched_argv, 4);
    if (status != 0) {
        return status;
    }

    const char *cp_argv[] = {
        "busybox",
        "cp",
        "/etc/pacha_boot_profile",
        "/tmp/lpr-busybox-dir/copy.txt",
    };
    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox cp", cp_argv, 4);
    if (status != 0) {
        return status;
    }

    const char *test_file_argv[] = {
        "busybox",
        "test",
        "-f",
        "/tmp/lpr-busybox-dir/copy.txt",
    };
    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox test file", test_file_argv, 4);
    if (status != 0) {
        return status;
    }

    const char *mv_argv[] = {
        "busybox",
        "mv",
        "/tmp/lpr-busybox-dir/copy.txt",
        "/tmp/lpr-busybox-dir/moved.txt",
    };
    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox mv", mv_argv, 4);
    if (status != 0) {
        return status;
    }

    const char *stat_argv[] = {
        "busybox",
        "stat",
        "/tmp/lpr-busybox-dir/moved.txt",
    };
    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox stat", stat_argv, 3);
    if (status != 0) {
        return status;
    }

    const char *rm_dir_argv[] = {
        "busybox",
        "rm",
        "-r",
        "/tmp/lpr-busybox-dir",
    };
    status = seed0root_run_lpr_busybox_command(filed_endpoint_fd, "lpr busybox rm dir", rm_dir_argv, 4);
    return status;
}

static int seed0root_run_lua_cli_bench(int filed_endpoint_fd)
{
    const char *argv[] = {
        "/cmd/lua.elf",
        "/cmd/lua_workload.lua",
    };
    return seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/lua.elf",
        argv,
        2,
        "PACHA_LUA_CLI_BENCH=1",
        "lua cli bench",
        FILED_EXEC_LINUX_LPR);
}

static int seed0root_run_lpr_dyn_needed_smoke(int filed_endpoint_fd)
{
    const char *argv[] = {
        "/cmd/lpr_dyn_needed.elf",
    };
    return seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/lpr_dyn_needed.elf",
        argv,
        1,
        "LD_LIBRARY_PATH=/lib:/lib/linux:/usr/lib",
        "lpr dyn needed smoke",
        FILED_EXEC_LINUX_LPR);
}

static int seed0root_run_curl_example(int filed_endpoint_fd)
{
    const char *argv[] = {
        "curl",
        "-sS",
        "--connect-timeout",
        "10",
        "--max-time",
        "20",
        "http://example.com/",
    };
    return seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/curl.elf",
        argv,
        7,
        "LD_LIBRARY_PATH=/opt/curl/lib:/lib:/usr/lib",
        "curl example.com workload",
        FILED_EXEC_LINUX_LPR);
}

static void seed0root_log_curl_timing(int filed_endpoint_fd, const char *path)
{
    char timing[512];
    const int status = seed0root_read_filed_text(filed_endpoint_fd, path, timing, sizeof(timing));
    if (status != 0) {
        printf("[seed0root] curl timing read failed path=%s status=%d\n", path, status);
        return;
    }
    size_t len = strlen(timing);
    if (len == 0 || timing[len - 1] != '\n') {
        if (len + 1 < sizeof(timing)) {
            timing[len++] = '\n';
            timing[len] = '\0';
        }
    }
    printf("[seed0root] curl timing %s", timing);
}

static int seed0root_run_curl_https_expected_failure(
    int filed_endpoint_fd,
    const char *url,
    const char *cacert_path,
    const char *body_path,
    const char *timing_path,
    const char *timing_format,
    const char *label,
    const uint64_t *expected_exits,
    uint64_t expected_count,
    const char *connect_timeout,
    const char *max_time)
{
    const char *argv[] = {
        "curl",
        "-sS",
        "--http1.1",
        "--connect-timeout",
        connect_timeout,
        "--max-time",
        max_time,
        "--cacert",
        cacert_path,
        "--output",
        body_path,
        "--write-out",
        timing_format,
        url,
    };
    const int status = seed0root_run_exec_path_smoke_expect_any(
        filed_endpoint_fd,
        "/cmd/curl.elf",
        argv,
        14,
        "LD_LIBRARY_PATH=/opt/curl/lib:/lib:/usr/lib",
        label,
        FILED_EXEC_LINUX_LPR,
        expected_exits,
        expected_count);
    seed0root_log_curl_timing(filed_endpoint_fd, timing_path);
    return status;
}

static int seed0root_run_curl_https_example(int filed_endpoint_fd)
{
    const char *head_timing_format =
        "%output{/tmp/curl-https-head.timing}"
        "curl_phase=HEAD url=%{url_effective} http=%{http_code} "
        "dns=%{time_namelookup} tcp=%{time_connect} tls=%{time_appconnect} "
        "pretransfer=%{time_pretransfer} first_byte=%{time_starttransfer} "
        "total=%{time_total} exit=%{exitcode} error=%{errormsg}\n";
    const char *get_timing_format =
        "%output{/tmp/curl-https-get.timing}"
        "curl_phase=GET url=%{url_effective} http=%{http_code} "
        "dns=%{time_namelookup} tcp=%{time_connect} tls=%{time_appconnect} "
        "pretransfer=%{time_pretransfer} first_byte=%{time_starttransfer} "
        "total=%{time_total} exit=%{exitcode} error=%{errormsg}\n";
    const char *cert_failure_timing_format =
        "%output{/tmp/curl-https-cert-failure.timing}"
        "curl_phase=CERT_FAILURE url=%{url_effective} http=%{http_code} "
        "dns=%{time_namelookup} tcp=%{time_connect} tls=%{time_appconnect} "
        "pretransfer=%{time_pretransfer} first_byte=%{time_starttransfer} "
        "total=%{time_total} exit=%{exitcode} error=%{errormsg}\n";
    const char *dns_failure_timing_format =
        "%output{/tmp/curl-https-dns-failure.timing}"
        "curl_phase=DNS_FAILURE url=%{url_effective} http=%{http_code} "
        "dns=%{time_namelookup} tcp=%{time_connect} tls=%{time_appconnect} "
        "pretransfer=%{time_pretransfer} first_byte=%{time_starttransfer} "
        "total=%{time_total} exit=%{exitcode} error=%{errormsg}\n";
    const char *timeout_timing_format =
        "%output{/tmp/curl-https-connect-timeout.timing}"
        "curl_phase=CONNECT_TIMEOUT url=%{url_effective} http=%{http_code} "
        "dns=%{time_namelookup} tcp=%{time_connect} tls=%{time_appconnect} "
        "pretransfer=%{time_pretransfer} first_byte=%{time_starttransfer} "
        "total=%{time_total} exit=%{exitcode} error=%{errormsg}\n";
    const char *head_argv[] = {
        "curl",
        "-sS",
        "--fail",
        "--http1.1",
        "-I",
        "--connect-timeout",
        "10",
        "--max-time",
        "30",
        "--cacert",
        "/etc/ssl/certs/ca-certificates.crt",
        "--output",
        "/tmp/curl-https-head.headers",
        "--write-out",
        head_timing_format,
        "https://example.com/",
    };
    int status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/curl.elf",
        head_argv,
        16,
        "LD_LIBRARY_PATH=/opt/curl/lib:/lib:/usr/lib",
        "curl https example.com HEAD workload",
        FILED_EXEC_LINUX_LPR);
    seed0root_log_curl_timing(filed_endpoint_fd, "/tmp/curl-https-head.timing");
    if (status != 0) {
        return status;
    }

    const char *get_argv[] = {
        "curl",
        "-sS",
        "--fail",
        "--http1.1",
        "--connect-timeout",
        "10",
        "--max-time",
        "30",
        "--cacert",
        "/etc/ssl/certs/ca-certificates.crt",
        "--output",
        "/tmp/curl-https-get.body",
        "--write-out",
        get_timing_format,
        "https://example.com/",
    };
    status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/curl.elf",
        get_argv,
        15,
        "LD_LIBRARY_PATH=/opt/curl/lib:/lib:/usr/lib",
        "curl https example.com GET workload",
        FILED_EXEC_LINUX_LPR);
    seed0root_log_curl_timing(filed_endpoint_fd, "/tmp/curl-https-get.timing");
    if (status != 0) {
        return status;
    }

    const uint64_t cert_failure_exits[] = {60, 77};
    status = seed0root_run_curl_https_expected_failure(
        filed_endpoint_fd,
        "https://example.com/",
        "/tmp/curl-https-get.body",
        "/tmp/curl-https-cert-failure.body",
        "/tmp/curl-https-cert-failure.timing",
        cert_failure_timing_format,
        "curl https cert failure workload",
        cert_failure_exits,
        2,
        "10",
        "30");
    if (status != 0) {
        return status;
    }

    const uint64_t dns_failure_exits[] = {6};
    status = seed0root_run_curl_https_expected_failure(
        filed_endpoint_fd,
        "https://pachaos-invalid.invalid/",
        "/etc/ssl/certs/ca-certificates.crt",
        "/tmp/curl-https-dns-failure.body",
        "/tmp/curl-https-dns-failure.timing",
        dns_failure_timing_format,
        "curl https dns failure workload",
        dns_failure_exits,
        1,
        "3",
        "6");
    if (status != 0) {
        return status;
    }

    const uint64_t timeout_exits[] = {7, 28};
    status = seed0root_run_curl_https_expected_failure(
        filed_endpoint_fd,
        "https://10.255.255.1/",
        "/etc/ssl/certs/ca-certificates.crt",
        "/tmp/curl-https-connect-timeout.body",
        "/tmp/curl-https-connect-timeout.timing",
        timeout_timing_format,
        "curl https connect timeout workload",
        timeout_exits,
        2,
        "2",
        "4");
    if (status != 0) {
        return status;
    }

    if (status == 0) {
        printf("[seed0root] curl https example.com profile completed status=0\n");
    }
    return status;
}

static int seed0root_run_chibicc_cli_bench(int filed_endpoint_fd)
{
    const char *cc1_argv[] = {
        "/cmd/chibicc.elf",
        "-cc1",
        "-cc1-input",
        "/cmd/chibicc_workload.c",
        "-cc1-output",
        "/tmp/chibicc_workload.s",
        "/cmd/chibicc_workload.c",
    };
    int status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/chibicc.elf",
        cc1_argv,
        7,
        "PACHA_CHIBICC_CLI_BENCH=1",
        "chibicc cc1 workload",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }

    const char *as_argv[] = {
        "/usr/bin/as",
        "-o",
        "/tmp/chibicc_workload.o",
        "/tmp/chibicc_workload.s",
    };
    status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/usr/bin/as",
        as_argv,
        4,
        "PACHA_CHIBICC_AS_BENCH=1",
        "chibicc as workload",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }

    const char *ld_argv[] = {
        "/usr/bin/ld",
        "-static",
        "-o",
        "/tmp/chibicc_workload.elf",
        "/usr/lib/crt1.o",
        "/usr/lib/crti.o",
        "/tmp/chibicc_workload.o",
        "-L/usr/lib",
        "-lc",
        "/usr/lib/crtn.o",
    };
    status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/usr/bin/ld",
        ld_argv,
        10,
        "PACHA_CHIBICC_LD_BENCH=1",
        "chibicc ld workload",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }

    const char *run_argv[] = {
        "/tmp/chibicc_workload.elf",
    };
    return seed0root_run_exec_path_smoke_expect(
        filed_endpoint_fd,
        "/tmp/chibicc_workload.elf",
        run_argv,
        1,
        "PACHA_CHIBICC_RUN_BENCH=1",
        "chibicc linked workload",
        FILED_EXEC_LINUX_LPR,
        191);
}

static int seed0root_run_apk_offline_bench(int filed_endpoint_fd)
{
    const char *mkdir_root_argv[] = {
        "busybox", "mkdir", "/tmp/apk-root",
    };
    int status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/alpine-busybox.elf",
        mkdir_root_argv,
        3,
        "PACHA_APK_OFFLINE_PREP_ROOT=1",
        "apk offline prep mkdir root",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }
    const char *mkdir_var_argv[] = {
        "busybox", "mkdir", "/tmp/apk-root/var",
    };
    status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/alpine-busybox.elf",
        mkdir_var_argv,
        3,
        "PACHA_APK_OFFLINE_PREP_VAR=1",
        "apk offline prep mkdir var",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }
    const char *mkdir_cache_parent_argv[] = {
        "busybox", "mkdir", "/tmp/apk-root/var/cache",
    };
    status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/alpine-busybox.elf",
        mkdir_cache_parent_argv,
        3,
        "PACHA_APK_OFFLINE_PREP_CACHE_PARENT=1",
        "apk offline prep mkdir cache parent",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }
    const char *mkdir_cache_argv[] = {
        "busybox", "mkdir", "/tmp/apk-root/var/cache/apk",
    };
    status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/alpine-busybox.elf",
        mkdir_cache_argv,
        3,
        "PACHA_APK_OFFLINE_PREP_CACHE=1",
        "apk offline prep mkdir cache",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }

    const char *apk_env = "LD_LIBRARY_PATH=/tmp/apk-root/lib:/tmp/apk-root/usr/lib:/opt/apk-offline/lib:/lib:/lib/linux:/usr/lib";
    const char *apk_argv[] = {
        "/cmd/apk-offline.elf",
        "--root",
        "/tmp/apk-root",
        "--initdb",
        "--no-network",
        "--no-scripts",
        "--allow-untrusted",
        "--timeout",
        "10",
        "--repository",
        "/var/cache/apk/offline",
        "--cache-dir",
        "/tmp/apk-root/var/cache/apk",
        "add",
        "zstd",
        "grep",
        "sed",
        "tar",
        "xz",
    };
    status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/apk-offline.elf",
        apk_argv,
        17,
        apk_env,
        "apk offline add workload",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }

    const char *grep_argv[] = {
        "grep", "--version",
    };
    status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/tmp/apk-root/bin/grep",
        grep_argv,
        2,
        apk_env,
        "apk installed grep version",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }
    const char *sed_argv[] = {
        "sed", "--version",
    };
    status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/tmp/apk-root/bin/sed",
        sed_argv,
        2,
        apk_env,
        "apk installed sed version",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }
    const char *tar_create_argv[] = {
        "tar", "-cf", "/tmp/apk-root/apk-world.tar", "-C", "/tmp/apk-root", "etc/apk/world",
    };
    status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/tmp/apk-root/bin/tar",
        tar_create_argv,
        6,
        apk_env,
        "apk installed tar create",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }
    const char *tar_list_argv[] = {
        "tar", "-tf", "/tmp/apk-root/apk-world.tar",
    };
    status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/tmp/apk-root/bin/tar",
        tar_list_argv,
        3,
        apk_env,
        "apk installed tar list",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }
    const char *xz_argv[] = {
        "xz", "--version",
    };
    status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/tmp/apk-root/usr/bin/xz",
        xz_argv,
        2,
        apk_env,
        "apk installed xz version",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }
    const char *zstd_argv[] = {
        "zstd", "--version",
    };
    return seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/tmp/apk-root/usr/bin/zstd",
        zstd_argv,
        2,
        apk_env,
        "apk installed zstd version",
        FILED_EXEC_LINUX_LPR);
}

static int seed0root_run_apk_update_smoke(int filed_endpoint_fd)
{
    const char *dirs[] = {
        "/tmp/apk-update-root",
        "/tmp/apk-update-root/var",
        "/tmp/apk-update-root/var/cache",
        "/tmp/apk-update-root/var/cache/apk",
        "/tmp/apk-update-root/var/lib",
        "/tmp/apk-update-root/var/lib/apk",
        "/tmp/apk-update-root/var/log",
        "/tmp/apk-update-root/etc",
        "/tmp/apk-update-root/etc/apk",
        "/tmp/apk-update-root/lib",
        "/tmp/apk-update-root/lib/apk",
        "/tmp/apk-update-root/lib/apk/db",
    };
    for (uint64_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i += 1) {
        const char *mkdir_argv[] = {
            "busybox", "mkdir", dirs[i],
        };
        int status = seed0root_run_exec_path_smoke(
            filed_endpoint_fd,
            "/cmd/alpine-busybox.elf",
            mkdir_argv,
            3,
            "PACHA_APK_UPDATE_PREP=1",
            "apk update prep mkdir",
            FILED_EXEC_LINUX_LPR);
        if (status != 0) {
            return status;
        }
    }

    const char *resolv_cp_argv[] = {
        "busybox", "cp", "/etc/resolv.conf", "/tmp/apk-update-root/etc/resolv.conf",
    };
    int status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/alpine-busybox.elf",
        resolv_cp_argv,
        sizeof(resolv_cp_argv) / sizeof(resolv_cp_argv[0]),
        "PACHA_APK_UPDATE_PREP=1",
        "apk update prep resolv.conf",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }

    const char *apk_env = "LD_LIBRARY_PATH=/opt/apk-offline/lib:/lib:/lib/linux:/usr/lib";
    const char *initdb_argv[] = {
        "/cmd/apk-offline.elf",
        "--root",
        "/tmp/apk-update-root",
        "--no-network",
        "--cache-dir",
        "/tmp/apk-update-root/var/cache/apk",
        "add",
        "--initdb",
    };
    status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/apk-offline.elf",
        initdb_argv,
        sizeof(initdb_argv) / sizeof(initdb_argv[0]),
        apk_env,
        "apk update initdb workload",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }

    const char *apk_argv[] = {
        "/cmd/apk-offline.elf",
        "--root",
        "/tmp/apk-update-root",
        "--allow-untrusted",
        "--progress=no",
        "--logfile=no",
        "--sync=no",
        "--repository",
        "http://dl-cdn.alpinelinux.org/alpine/edge/main",
        "--cache-dir",
        "/tmp/apk-update-root/var/cache/apk",
        "update",
    };
    status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/apk-offline.elf",
        apk_argv,
        sizeof(apk_argv) / sizeof(apk_argv[0]),
        apk_env,
        "apk update workload",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }

    const char *apk_add_argv[] = {
        "/cmd/apk-offline.elf",
        "--root",
        "/tmp/apk-update-root",
        "--allow-untrusted",
        "--progress=no",
        "--logfile=no",
        "--sync=no",
        "--repository",
        "http://dl-cdn.alpinelinux.org/alpine/edge/main",
        "--cache-dir",
        "/tmp/apk-update-root/var/cache/apk",
        "add",
        "zlib",
    };
    status = seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/apk-offline.elf",
        apk_add_argv,
        sizeof(apk_add_argv) / sizeof(apk_add_argv[0]),
        apk_env,
        "apk add zlib workload",
        FILED_EXEC_LINUX_LPR);
    if (status != 0) {
        return status;
    }

    const char *zlib_stat_argv[] = {
        "busybox", "test", "-e", "/tmp/apk-update-root/usr/lib/libz.so.1",
    };
    return seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/alpine-busybox.elf",
        zlib_stat_argv,
        sizeof(zlib_stat_argv) / sizeof(zlib_stat_argv[0]),
        "PACHA_APK_ADD_VERIFY=1",
        "apk add zlib verify",
        FILED_EXEC_LINUX_LPR);
}

static int seed0root_run_storage_services(int filed_endpoint_fd)
{
    int status = 0;
    if (filed_endpoint_fd >= 16) {
        printf("[seed0root] filed ready\n");
    } else {
        return -1;
    }
    const unsigned boot_profile = seed0root_boot_profile_flags(filed_endpoint_fd);
    if ((boot_profile & SEED0ROOT_BOOT_PROFILE_FS_WRITE) != 0 && status == 0 && filed_endpoint_fd >= 16) {
        status = seed0root_run_filed_no_cache_probe(filed_endpoint_fd);
    }
    if ((boot_profile & SEED0ROOT_BOOT_PROFILE_FS_WRITE) != 0 && status == 0 && filed_endpoint_fd >= 16) {
        status = seed0root_run_libc_vfs_exec_smoke(filed_endpoint_fd);
    }
    if ((boot_profile & SEED0ROOT_BOOT_PROFILE_LPR) != 0 && status == 0 && filed_endpoint_fd >= 16) {
        status = seed0root_run_lpr_busybox_cold_echo_smoke(filed_endpoint_fd);
    }
    if ((boot_profile & SEED0ROOT_BOOT_PROFILE_LPR) != 0 && status == 0 && filed_endpoint_fd >= 16) {
        status = seed0root_run_lpr_minimal_smoke(filed_endpoint_fd);
    }
    if ((boot_profile & SEED0ROOT_BOOT_PROFILE_LPR) != 0 && status == 0 && filed_endpoint_fd >= 16) {
        status = seed0root_run_lpr_ldmusl_smoke(filed_endpoint_fd);
    }
    if ((boot_profile & SEED0ROOT_BOOT_PROFILE_LPR) != 0 && status == 0 && filed_endpoint_fd >= 16) {
        status = seed0root_run_lpr_pty_probe(filed_endpoint_fd);
    }
    if ((boot_profile & SEED0ROOT_BOOT_PROFILE_LPR) != 0 && status == 0 && filed_endpoint_fd >= 16) {
        status = seed0root_run_lpr_busybox_dynamic_smoke(filed_endpoint_fd);
    }
    if ((boot_profile & SEED0ROOT_BOOT_PROFILE_MEMORY) != 0 && status == 0 && filed_endpoint_fd >= 16) {
        status = seed0root_run_libc_alloc_probe(filed_endpoint_fd);
    }
    if ((boot_profile & SEED0ROOT_BOOT_PROFILE_BENCH) != 0 && status == 0 && filed_endpoint_fd >= 16) {
        status = seed0root_run_libc_mix_bench(filed_endpoint_fd);
    }
    if ((boot_profile & SEED0ROOT_BOOT_PROFILE_DYN_NEEDED) != 0 && status == 0 && filed_endpoint_fd >= 16) {
        status = seed0root_run_lpr_dyn_needed_smoke(filed_endpoint_fd);
    }
    if ((boot_profile & SEED0ROOT_BOOT_PROFILE_CURL) != 0 && status == 0 && filed_endpoint_fd >= 16) {
        status = seed0root_run_curl_example(filed_endpoint_fd);
    }
    if ((boot_profile & SEED0ROOT_BOOT_PROFILE_HTTPS) != 0 && status == 0 && filed_endpoint_fd >= 16) {
        status = seed0root_run_curl_https_example(filed_endpoint_fd);
    }
    if ((boot_profile & (SEED0ROOT_BOOT_PROFILE_BENCH | SEED0ROOT_BOOT_PROFILE_LUA)) != 0 &&
        status == 0 &&
        filed_endpoint_fd >= 16) {
        status = seed0root_run_lua_cli_bench(filed_endpoint_fd);
    }
    if ((boot_profile & (SEED0ROOT_BOOT_PROFILE_BENCH | SEED0ROOT_BOOT_PROFILE_CHIBICC)) != 0 &&
        status == 0 &&
        filed_endpoint_fd >= 16) {
        status = seed0root_run_chibicc_cli_bench(filed_endpoint_fd);
    }
    if ((boot_profile & SEED0ROOT_BOOT_PROFILE_APK) != 0 && status == 0 && filed_endpoint_fd >= 16) {
        status = seed0root_run_apk_offline_bench(filed_endpoint_fd);
    }
    if ((boot_profile & SEED0ROOT_BOOT_PROFILE_APK_UPDATE) != 0 && status == 0 && filed_endpoint_fd >= 16) {
        status = seed0root_run_apk_update_smoke(filed_endpoint_fd);
    }
    const unsigned metrics_profile =
        boot_profile & (SEED0ROOT_BOOT_PROFILE_FS_WRITE |
                        SEED0ROOT_BOOT_PROFILE_MEMORY |
                        SEED0ROOT_BOOT_PROFILE_BENCH |
                        SEED0ROOT_BOOT_PROFILE_LPR |
                        SEED0ROOT_BOOT_PROFILE_LUA |
                        SEED0ROOT_BOOT_PROFILE_DYN_NEEDED |
                        SEED0ROOT_BOOT_PROFILE_CHIBICC |
                        SEED0ROOT_BOOT_PROFILE_APK |
                        SEED0ROOT_BOOT_PROFILE_APK_UPDATE |
                        SEED0ROOT_BOOT_PROFILE_CURL |
                        SEED0ROOT_BOOT_PROFILE_HTTPS);
    const unsigned sync_profile =
        boot_profile & (SEED0ROOT_BOOT_PROFILE_FS_WRITE |
                        SEED0ROOT_BOOT_PROFILE_MEMORY |
                        SEED0ROOT_BOOT_PROFILE_BENCH |
                        SEED0ROOT_BOOT_PROFILE_LPR |
                        SEED0ROOT_BOOT_PROFILE_LUA |
                        SEED0ROOT_BOOT_PROFILE_DYN_NEEDED |
                        SEED0ROOT_BOOT_PROFILE_CHIBICC |
                        SEED0ROOT_BOOT_PROFILE_APK |
                        SEED0ROOT_BOOT_PROFILE_APK_UPDATE |
                        SEED0ROOT_BOOT_PROFILE_CURL |
                        SEED0ROOT_BOOT_PROFILE_HTTPS);
    if (metrics_profile != 0 && status == 0 && filed_endpoint_fd >= 16) {
        const int metrics_status = seed0root_dump_filed_metrics(filed_endpoint_fd);
        printf("[seed0root] filed metrics dump status=%d\n", metrics_status);
    }
    if (sync_profile != 0 && status == 0) {
        const uint64_t sync_start_ns = seed0root_now_ns();
        status = seed0root_filed_sync_all(filed_endpoint_fd);
        const uint64_t sync_end_ns = seed0root_now_ns();
        const uint64_t sync_elapsed_ns =
            (sync_start_ns != 0 && sync_end_ns >= sync_start_ns) ? sync_end_ns - sync_start_ns : 0;
        printf("[seed0root] storage clean checkpoint status=%d ns=%llu us=%llu\n",
            status,
            (unsigned long long)sync_elapsed_ns,
            (unsigned long long)(sync_elapsed_ns / 1000ull));
    }
    return status;
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
    uint64_t load_bias = 0;
    const int use_aslr = e_type == SEED0ROOT_ELF_TYPE_DYN;
    const uint64_t process_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL |
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
        }
        load_count++;
    }
    if (load_count == 0) {
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
    return 0;
}

static int push_u64(unsigned char *stack, uint64_t *sp, uint64_t value)
{
    if (*sp < 8) return -1;
    *sp -= 8;
    wr64(stack + *sp, value);
    return 0;
}

static int start_loaded_process(
    const struct seed0root_loaded_process *loaded,
    const char *argv0,
    int bootstrap_fd,
    struct seed0root_started_process *out_started)
{
    if (loaded == NULL || argv0 == NULL) {
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
    if (stack_fd < 16) return -2;
    unsigned char *stack = pacha_mmap(
        stack_fd,
        PACHA_PROCESS_DEFAULT_STACK_SIZE,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
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
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        0);
    if (stack_map < 4096) {
        (void)pacha_munmap(stack, PACHA_PROCESS_DEFAULT_STACK_SIZE);
        (void)pacha_fd_close(stack_fd);
        return -6;
    }
    const uint64_t stack_base = (uint64_t)stack_map;

    uint64_t sp = PACHA_PROCESS_DEFAULT_STACK_SIZE;
    const uint64_t argv0_len = (uint64_t)strlen(argv0) + 1;
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

    if (push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_NULL) != 0 ||
        (bootstrap_fd >= 16 && (push_u64(stack, &sp, (uint64_t)(uint32_t)bootstrap_fd) != 0 || push_u64(stack, &sp, PACHA_AT_BOOTSTRAP_FD) != 0)) ||
        push_u64(stack, &sp, argv0_va) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_EXECFN) != 0 ||
        push_u64(stack, &sp, random_va) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_RANDOM) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_SECURE) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_EGID) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_GID) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_EUID) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_UID) != 0 ||
        push_u64(stack, &sp, loaded->runtime_entry) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_ENTRY) != 0 ||
        push_u64(stack, &sp, 0) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_BASE) != 0 ||
        push_u64(stack, &sp, SEED0ROOT_PAGE_SIZE) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_PAGESZ) != 0 ||
        push_u64(stack, &sp, loaded->phnum) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_PHNUM) != 0 ||
        push_u64(stack, &sp, loaded->phent) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_PHENT) != 0 ||
        push_u64(stack, &sp, loaded->phdr_va) != 0 || push_u64(stack, &sp, SEED0ROOT_AT_PHDR) != 0 ||
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
    if (thread_fd < 16) return -7;
    const int start_status = pacha_thread_start(thread_fd);
    if (start_status != 0) {
        (void)pacha_fd_close(thread_fd);
        return -8;
    }
    if (out_started != NULL) {
        out_started->process_fd = process_fd;
        out_started->thread_fd = thread_fd;
    }
    return 0;
}

static int prepare_filed_storage_bootstrap(
    const struct seed0root_bootstrap *bootstrap,
    int filed_endpoint_fd,
    struct seed0root_filed_storage_bootstrap *out_bootstrap)
{
    if (bootstrap == NULL ||
        out_bootstrap == NULL ||
        filed_endpoint_fd < 16 ||
        bootstrap->module_count == 0 ||
        bootstrap->module_count > SEED0ROOT_BOOTSTRAP_MAX_MODULES ||
        bootstrap->modules[0].name[0] == '\0')
    {
        return -1;
    }

    memset(out_bootstrap, 0, sizeof(*out_bootstrap));
    out_bootstrap->magic = SEED0ROOT_FILED_STORAGE_BOOTSTRAP_MAGIC;
    out_bootstrap->device_fd = bootstrap->device_fd;
    out_bootstrap->filed_endpoint_fd = (uint64_t)(uint32_t)filed_endpoint_fd;
    out_bootstrap->module_count = bootstrap->module_count;
    for (uint64_t i = 0; i < bootstrap->module_count; i++) {
        const struct seed0root_bootstrap_module *src = &bootstrap->modules[i];
        if (src->name[0] == '\0' || src->image_fd < 16 || src->image_size < 4) {
            return -2;
        }
        snprintf(out_bootstrap->modules[i].name, sizeof(out_bootstrap->modules[i].name), "%s", src->name);
        out_bootstrap->modules[i].image_fd = src->image_fd;
        out_bootstrap->modules[i].image_size = src->image_size;
    }

    return 0;
}

static int seed0root_send_storage_ready(int ready_channel_fd, int filed_client_fd)
{
    if (ready_channel_fd < 16 || filed_client_fd < 16) {
        return -1;
    }

    struct pacha_ipc_fd fd_item = {
        .fd = (uint64_t)(uint32_t)filed_client_fd,
        .rights = seed0root_channel_rights,
        .flags = 0,
        .transfer_flags = 0,
    };
    struct pacha_ipc_msg msg = {
        .word0 = SEED0ROOT_STORAGE_READY_MAGIC,
        .word1 = 0,
        .word2 = 0,
        .word3 = 0,
        .fds = &fd_item,
        .fd_count = 1,
    };
    return pacha_ipc_send(ready_channel_fd, &msg);
}

static int seed0root_wait_services_ready(int service_ready_channel_fd)
{
    if (service_ready_channel_fd < 16) {
        return -22;
    }
    struct pacha_ipc_msg msg;
    memset(&msg, 0, sizeof(msg));
    const int status = recv_ipc_wait(service_ready_channel_fd, &msg);
    (void)pacha_fd_close(service_ready_channel_fd);
    if (status != 0 ||
        msg.word0 != SEED0ROOT_SERVICES_READY_MAGIC ||
        msg.word1 != 0)
    {
        fprintf(stderr,
            "[seed0root] services ready wait failed status=%d word0=0x%llx word1=%llu\n",
            status,
            (unsigned long long)msg.word0,
            (unsigned long long)msg.word1);
        return status != 0 ? status : -5;
    }
    printf("[seed0root] services ready signal received\n");
    fflush(stdout);
    return 0;
}

static int seed0root_start_lpr_supervisor(int filed_endpoint_fd, int *out_endpoint_fd)
{
    if (filed_endpoint_fd < 16 || out_endpoint_fd == NULL) {
        return -22;
    }
    *out_endpoint_fd = -1;
    const int endpoint_fd = pacha_ipc_endpoint_create(seed0root_channel_rights, 0);
    if (endpoint_fd < 16) {
        return endpoint_fd < 0 ? endpoint_fd : -5;
    }

    struct lprs_boot_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = LPRS_BOOT_CONFIG_MAGIC;
    cfg.endpoint_fd = LPR_SUPERVISOR_ENDPOINT_FD;
    const int bootstrap_fd = create_inherited_vmo_from_bytes_with_extra_rights(
        &cfg,
        sizeof(cfg),
        "lpr supervisor bootstrap fd",
        PACHA_FD_RIGHT_DUP);
    if (bootstrap_fd < 16) {
        (void)pacha_fd_close(endpoint_fd);
        return bootstrap_fd;
    }

    int page_fd = -1;
    void *page = NULL;
    int status = seed0root_create_filed_page(&page_fd, &page);
    if (status != 0) {
        (void)pacha_fd_close(bootstrap_fd);
        (void)pacha_fd_close(endpoint_fd);
        return status;
    }
    filed_exec_path_t *exec = (filed_exec_path_t *)page;
    exec->dir_handle = 0;
    exec->flags = FILED_EXEC_INHERIT_FDS;
    exec->inherit_fd_count = 2;
    exec->inherit_fd_targets[0] = LPR_SUPERVISOR_ENDPOINT_FD;
    exec->inherit_fd_targets[1] = LPRS_BOOT_CONFIG_FD;
    exec->argc = 2;
    snprintf(exec->path, sizeof(exec->path), "%s", "/sbin/lpr_supervisor.elf");
    status = seed0root_exec_add_string(exec, &exec->argv[0], "/sbin/lpr_supervisor.elf");
    if (status != 0) {
        seed0root_destroy_filed_page(page_fd, page);
        (void)pacha_fd_close(bootstrap_fd);
        (void)pacha_fd_close(endpoint_fd);
        return status;
    }
    char boot_arg[32];
    snprintf(boot_arg, sizeof(boot_arg), "--boot-fd=%u", (unsigned)LPRS_BOOT_CONFIG_FD);
    status = seed0root_exec_add_string(exec, &exec->argv[1], boot_arg);
    if (status != 0) {
        seed0root_destroy_filed_page(page_fd, page);
        (void)pacha_fd_close(bootstrap_fd);
        (void)pacha_fd_close(endpoint_fd);
        return status;
    }

    struct pacha_ipc_fd fds[3];
    memset(fds, 0, sizeof(fds));
    fds[0].fd = (uint64_t)(uint32_t)page_fd;
    fds[0].rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    fds[1].fd = (uint64_t)(uint32_t)endpoint_fd;
    fds[1].rights = seed0root_channel_rights;
    fds[2].fd = (uint64_t)(uint32_t)bootstrap_fd;
    fds[2].rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_READ |
        PACHA_FD_RIGHT_MAP_READ;

    struct pacha_ipc_fd reply_fds[2];
    memset(reply_fds, 0, sizeof(reply_fds));
    struct pacha_ipc_msg reply;
    status = seed0root_filed_page_call_fdv(
        filed_endpoint_fd,
        FILED_OP_EXEC_PATH,
        0x5eed1001u,
        fds,
        3,
        0,
        &reply,
        reply_fds,
        2);
    seed0root_destroy_filed_page(page_fd, page);
    (void)pacha_fd_close(bootstrap_fd);
    if (status != 0) {
        (void)pacha_fd_close(endpoint_fd);
        return status;
    }
    if (reply.fd_count < 2 || reply_fds[0].fd < 16 || reply_fds[1].fd < 16) {
        if (reply_fds[1].fd >= 16) {
            (void)pacha_fd_close((int)reply_fds[1].fd);
        }
        if (reply_fds[0].fd >= 16) {
            (void)pacha_fd_close((int)reply_fds[0].fd);
        }
        (void)pacha_fd_close(endpoint_fd);
        return -5;
    }
    (void)pacha_fd_close((int)reply_fds[1].fd);
    (void)pacha_fd_close((int)reply_fds[0].fd);

    memset(&reply, 0, sizeof(reply));
    status = seed0root_lprs_call(
        endpoint_fd,
        LPRS_OP_HELLO,
        0x5eed1002u,
        -1,
        NULL,
        0,
        -1,
        &reply);
    if (status != 0) {
        (void)pacha_fd_close(endpoint_fd);
        return status;
    }
    printf("[seed0root] lpr supervisor started endpoint_fd=%d\n", endpoint_fd);
    fflush(stdout);
    *out_endpoint_fd = endpoint_fd;
    return 0;
}

static int seed0root_register_termd_signal_supervisor(
    int filed_endpoint_fd,
    int supervisor_endpoint_fd)
{
    if (filed_endpoint_fd < 16 || supervisor_endpoint_fd < 16) {
        return -22;
    }

    struct pacha_ipc_msg reply;
    pacha_service_envelope_t reply_header;
    memset(&reply, 0, sizeof(reply));
    memset(&reply_header, 0, sizeof(reply_header));
    const int status = seed0root_filed_service_call(
        filed_endpoint_fd,
        FILED_OP_SERVICE_REGISTER_TERMD_SIGNAL_SUPERVISOR,
        0x5eed1005u,
        sizeof(filed_service_endpoint_request_t),
        supervisor_endpoint_fd,
        &reply,
        &reply_header);
    if (status != 0) {
        return status;
    }
    printf("[seed0root] termd signal supervisor registered result=%llu\n",
        (unsigned long long)reply_header.result);
    fflush(stdout);
    return 0;
}

static int seed0root_register_lpr_session(
    int supervisor_endpoint_fd,
    lprs_process_state_t *out_state)
{
    if (supervisor_endpoint_fd < 16 || out_state == NULL) {
        return -22;
    }
    int page_fd = -1;
    void *page = NULL;
    int status = seed0root_create_filed_page(&page_fd, &page);
    if (status != 0) {
        return status;
    }
    lprs_register_exec_t *reg =
        (lprs_register_exec_t *)((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES);
    memset(reg, 0, sizeof(*reg));
    snprintf(reg->state.ctty, sizeof(reg->state.ctty), "%s", "/dev/hvc0");
    snprintf(reg->state.cwd, sizeof(reg->state.cwd), "%s", "/");
    struct pacha_ipc_msg reply;
    status = seed0root_lprs_call(
        supervisor_endpoint_fd,
        LPRS_OP_PROCESS_REGISTER_EXEC,
        0x5eed1003u,
        page_fd,
        page,
        sizeof(*reg),
        -1,
        &reply);
    if (status == 0) {
        memcpy(out_state, &reg->state, sizeof(*out_state));
    }
    seed0root_destroy_filed_page(page_fd, page);
    return status;
}

static int seed0root_spawn_lpr_session(
    int filed_endpoint_fd,
    int supervisor_endpoint_fd)
{
    static const char *const argv[] = {
        "/usr/libexec/pacha-user-session",
    };
    static const char *const envp[] = {
        "PATH=/bin:/usr/bin:/cmd",
        "TERM=linux",
        "HOME=/home",
        "LD_LIBRARY_PATH=/lib/linux:/usr/lib",
    };
    if (filed_endpoint_fd < 16 || supervisor_endpoint_fd < 16) {
        return -22;
    }
    lprs_process_state_t session_state;
    memset(&session_state, 0, sizeof(session_state));
    int status = seed0root_register_lpr_session(
        supervisor_endpoint_fd,
        &session_state);
    if (status != 0 || session_state.token == 0 ||
        session_state.generation == 0 || session_state.pid == 0)
    {
        return status != 0 ? status : -5;
    }

    lpr_manifest_layout_t manifest_layout;
    memset(&manifest_layout, 0, sizeof(manifest_layout));
    status = lpr_manifest_layout(0, 0, 0, 0, &manifest_layout);
    if (status != 0) {
        return -22;
    }
    uint64_t manifest_map_bytes = 0;
    if (align_up(manifest_layout.byte_size, &manifest_map_bytes) != 0) {
        return -22;
    }
    const uint64_t manifest_rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    int manifest_fd = pacha_vmo_create(manifest_map_bytes, manifest_rights, 0);
    if (manifest_fd < 16) {
        return manifest_fd < 0 ? manifest_fd : -12;
    }
    lpr_manifest_t *manifest = pacha_mmap(
        manifest_fd,
        manifest_map_bytes,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    if (manifest == NULL) {
        (void)pacha_fd_close(manifest_fd);
        return -12;
    }
    status = lpr_manifest_begin(
        manifest,
        manifest_map_bytes,
        &manifest_layout,
        0,
        0,
        0,
        0);
    if (status != 0) {
        seed0root_destroy_wire_page(manifest_map_bytes, manifest_fd, manifest);
        return -22;
    }
    manifest->transaction_id = session_state.token;
    manifest->generation = session_state.generation;
    manifest->flags = LPR_MANIFEST_FLAG_DEFAULT_STDIO | LPR_MANIFEST_FLAG_SUPERVISOR;
    manifest->linux_pid = session_state.pid;
    manifest->linux_ppid = session_state.ppid;
    manifest->linux_sid = session_state.sid;
    manifest->linux_pgrp = session_state.pgrp;
    manifest->linux_next_pid = 0;
    manifest->cwd_handle = session_state.cwd_handle;
    manifest->supervisor_token = session_state.token;
    manifest->supervisor_endpoint_fd = LPR_SUPERVISOR_ENDPOINT_FD;
    manifest->owner_generation = session_state.generation;
    snprintf(manifest->ctty, sizeof(manifest->ctty), "%s", session_state.ctty);
    snprintf(manifest->cwd, sizeof(manifest->cwd), "%s", session_state.cwd);
    if (lpr_manifest_seal(manifest, manifest_map_bytes) != 0) {
        seed0root_destroy_wire_page(manifest_map_bytes, manifest_fd, manifest);
        return -22;
    }

    int page_fd = -1;
    void *page = NULL;
    status = seed0root_create_filed_page(&page_fd, &page);
    if (status != 0) {
        seed0root_destroy_wire_page(manifest_map_bytes, manifest_fd, manifest);
        return status;
    }
    filed_exec_path_t *exec = (filed_exec_path_t *)page;
    exec->dir_handle = 0;
    exec->flags =
        FILED_EXEC_LINUX_LPR |
        FILED_EXEC_BOOTSTRAP_FD |
        FILED_EXEC_INHERIT_FDS |
        FILED_EXEC_TRANSFER_PROCESS_FD |
        FILED_EXEC_DEFER_START;
    exec->inherit_fd_count = 1;
    exec->inherit_fd_targets[0] = LPR_SUPERVISOR_ENDPOINT_FD;
    exec->argc = sizeof(argv) / sizeof(argv[0]);
    exec->envc = sizeof(envp) / sizeof(envp[0]);
    snprintf(exec->path, sizeof(exec->path), "%s", "/usr/libexec/pacha-user-session");
    for (uint64_t i = 0; i < exec->argc; i++) {
        status = seed0root_exec_add_string(exec, &exec->argv[i], argv[i]);
        if (status != 0) {
            seed0root_destroy_filed_page(page_fd, page);
            seed0root_destroy_wire_page(manifest_map_bytes, manifest_fd, manifest);
            return status;
        }
    }
    for (uint64_t i = 0; i < exec->envc; i++) {
        status = seed0root_exec_add_string(exec, &exec->envp[i], envp[i]);
        if (status != 0) {
            seed0root_destroy_filed_page(page_fd, page);
            seed0root_destroy_wire_page(manifest_map_bytes, manifest_fd, manifest);
            return status;
        }
    }

    struct pacha_ipc_fd fds[3];
    memset(fds, 0, sizeof(fds));
    fds[0].fd = (uint64_t)(uint32_t)page_fd;
    fds[0].rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    fds[1].fd = (uint64_t)(uint32_t)supervisor_endpoint_fd;
    fds[1].rights = seed0root_channel_rights;
    fds[2].fd = (uint64_t)(uint32_t)manifest_fd;
    fds[2].rights =
        PACHA_FD_RIGHT_INSPECT |
        PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_SET_FLAGS |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ;

    struct pacha_ipc_fd reply_fds[2];
    memset(reply_fds, 0, sizeof(reply_fds));
    struct pacha_ipc_msg reply;
    status = seed0root_filed_page_call_fdv(
        filed_endpoint_fd,
        FILED_OP_EXEC_PATH,
        0x5eed0ba5u,
        fds,
        3,
        0,
        &reply,
        reply_fds,
        2);
    seed0root_destroy_filed_page(page_fd, page);
    seed0root_destroy_wire_page(manifest_map_bytes, manifest_fd, manifest);
    if (status != 0) {
        fprintf(stderr, "[seed0root] user session exec failed status=%d\n", status);
        return status;
    }
    if (reply.fd_count < 2 || reply_fds[0].fd < 16 || reply_fds[1].fd < 16) {
        if (reply_fds[1].fd >= 16) {
            (void)pacha_fd_close((int)reply_fds[1].fd);
        }
        if (reply_fds[0].fd >= 16) {
            (void)pacha_fd_close((int)reply_fds[0].fd);
        }
        return -5;
    }
    memset(&reply, 0, sizeof(reply));
    int lprs_page_fd = -1;
    void *lprs_page = NULL;
    status = seed0root_create_filed_page(&lprs_page_fd, &lprs_page);
    if (status != 0) {
        (void)pacha_fd_close((int)reply_fds[1].fd);
        (void)pacha_syscall2(PACHA_PROCESS_SYSCALL_KILL, reply_fds[0].fd, 1);
        (void)pacha_fd_close((int)reply_fds[0].fd);
        return status;
    }
    lprs_token_request_t *token_req =
        (lprs_token_request_t *)((uint8_t *)lprs_page + PACHA_SERVICE_HEADER_BYTES);
    memset(token_req, 0, sizeof(*token_req));
    token_req->token = session_state.token;
    status = seed0root_lprs_call(
        supervisor_endpoint_fd,
        LPRS_OP_PROCESS_EXEC_COMMIT_BEGIN,
        0x5eed1004u,
        lprs_page_fd,
        lprs_page,
        sizeof(*token_req),
        (int)reply_fds[0].fd,
        &reply);
    seed0root_destroy_filed_page(lprs_page_fd, lprs_page);
    if (status != 0) {
        (void)pacha_fd_close((int)reply_fds[1].fd);
        (void)pacha_syscall2(PACHA_PROCESS_SYSCALL_KILL, reply_fds[0].fd, 1);
        (void)pacha_fd_close((int)reply_fds[0].fd);
        fprintf(stderr,
            "[seed0root] user session transaction begin failed status=%d\n",
            status);
        return status;
    }
    const int start_status = pacha_thread_start((int)reply_fds[1].fd);
    (void)pacha_fd_close((int)reply_fds[1].fd);
    if (start_status != 0) {
        (void)pacha_syscall2(PACHA_PROCESS_SYSCALL_KILL, reply_fds[0].fd, 1);
        (void)pacha_fd_close((int)reply_fds[0].fd);
        fprintf(stderr,
            "[seed0root] user session thread start failed status=%d\n",
            start_status);
        return start_status;
    }
    (void)pacha_fd_close((int)reply_fds[0].fd);
    printf("[seed0root] user session started\n");
    fflush(stdout);
    return 0;
}

static int launch_filed(const struct seed0root_bootstrap *bootstrap)
{
    if (bootstrap->magic != SEED0ROOT_BOOTSTRAP_MAGIC ||
        bootstrap->filed_image_fd < 16 ||
        bootstrap->filed_image_size == 0 ||
        bootstrap->device_fd < 16 ||
        bootstrap->ready_channel_fd < 16 ||
        bootstrap->service_ready_channel_fd < 16 ||
        bootstrap->module_count == 0 ||
        bootstrap->module_count > SEED0ROOT_BOOTSTRAP_MAX_MODULES) {
        fprintf(stderr,
            "[seed0root] bootstrap unavailable magic=0x%llx device_fd=%llu ready_fd=%llu service_ready_fd=%llu filed_fd=%llu size=%llu modules=%llu\n",
            (unsigned long long)bootstrap->magic,
            (unsigned long long)bootstrap->device_fd,
            (unsigned long long)bootstrap->ready_channel_fd,
            (unsigned long long)bootstrap->service_ready_channel_fd,
            (unsigned long long)bootstrap->filed_image_fd,
            (unsigned long long)bootstrap->filed_image_size,
            (unsigned long long)bootstrap->module_count);
        return -1;
    }

    uint64_t filed_map_size = 0;
    if (align_up(bootstrap->filed_image_size, &filed_map_size) != 0) {
        return -1;
    }
    printf("[seed0root] filed image mmap begin fd=%llu size=%llu map=%llu\n",
        (unsigned long long)bootstrap->filed_image_fd,
        (unsigned long long)bootstrap->filed_image_size,
        (unsigned long long)filed_map_size);
    fflush(stdout);
    unsigned char *image = pacha_mmap(
        (int)bootstrap->filed_image_fd,
        filed_map_size,
        PACHA_PROT_READ,
        PACHA_MMAP_SHARED,
        0);
    printf("[seed0root] filed image mmap returned ptr=%p\n", (void *)image);
    fflush(stdout);
    if (image == NULL) {
        fprintf(stderr, "[seed0root] filed image mmap failed fd=%llu\n",
            (unsigned long long)bootstrap->filed_image_fd);
        return -1;
    }
    printf("[seed0root] filed image ready\n");
    fflush(stdout);
    const int filed_endpoint_fd =
        pacha_ipc_endpoint_create(seed0root_channel_rights, PACHA_FD_FLAG_INHERIT);
    if (filed_endpoint_fd < 16) {
        fprintf(stderr,
            "[seed0root] filed endpoint create failed status=%d\n",
            filed_endpoint_fd);
        (void)pacha_munmap(image, filed_map_size);
        return filed_endpoint_fd < 0 ? filed_endpoint_fd : -2;
    }
    const long filed_client_dup =
        pacha_fd_fcntl(filed_endpoint_fd, PACHA_FD_FCNTL_DUP, 16, seed0root_channel_rights);
    if (filed_client_dup < 16) {
        fprintf(stderr,
            "[seed0root] filed endpoint dup failed status=%ld endpoint_fd=%d\n",
            filed_client_dup,
            filed_endpoint_fd);
        (void)pacha_fd_close(filed_endpoint_fd);
        (void)pacha_munmap(image, filed_map_size);
        return filed_client_dup < 0 ? (int)filed_client_dup : -2;
    }
    const int filed_client_fd = (int)filed_client_dup;
    printf("[seed0root] filed endpoint ready\n");
    fflush(stdout);
    int status = mark_fd_inherit((int)bootstrap->device_fd, "filed device fd");
    if (status != 0) {
        fprintf(stderr, "[seed0root] filed device fd inherit failed status=%d fd=%llu\n",
            status,
            (unsigned long long)bootstrap->device_fd);
        (void)pacha_fd_close(filed_client_fd);
        (void)pacha_fd_close(filed_endpoint_fd);
        (void)pacha_munmap(image, filed_map_size);
        return status;
    }
    printf("[seed0root] filed device fd ready\n");
    fflush(stdout);
    struct seed0root_filed_storage_bootstrap filed_bootstrap;
    status = prepare_filed_storage_bootstrap(bootstrap, filed_endpoint_fd, &filed_bootstrap);
    if (status != 0) {
        (void)pacha_fd_close(filed_client_fd);
        (void)pacha_fd_close(filed_endpoint_fd);
        (void)pacha_munmap(image, filed_map_size);
        fprintf(stderr, "[seed0root] filed bootstrap package failed status=%d\n", status);
        return status;
    }
    printf("[seed0root] filed bootstrap ready\n");
    fflush(stdout);
    const int bootstrap_fd = create_inherited_vmo_from_bytes(&filed_bootstrap, sizeof(filed_bootstrap), "filed bootstrap fd");
    if (bootstrap_fd < 16) {
        (void)pacha_fd_close(filed_client_fd);
        (void)pacha_fd_close(filed_endpoint_fd);
        (void)pacha_munmap(image, filed_map_size);
        fprintf(stderr, "[seed0root] filed bootstrap fd create failed status=%d\n", bootstrap_fd);
        return bootstrap_fd;
    }
    printf("[seed0root] filed bootstrap fd=%d\n", bootstrap_fd);
    fflush(stdout);
    struct seed0root_loaded_process loaded;
    status = load_elf_process("/sbin/filed.elf", image, bootstrap->filed_image_size, &loaded);
    (void)pacha_munmap(image, filed_map_size);
    if (status != 0) {
        (void)pacha_fd_close(bootstrap_fd);
        (void)pacha_fd_close(filed_client_fd);
        (void)pacha_fd_close(filed_endpoint_fd);
        fprintf(stderr, "[seed0root] filed load failed status=%d\n", status);
        return status;
    }
    printf("[seed0root] filed image loaded\n");
    fflush(stdout);
    status = start_loaded_process(&loaded, "/sbin/filed.elf", bootstrap_fd, NULL);
    (void)pacha_fd_close(bootstrap_fd);
    if (status != 0) {
        (void)pacha_fd_close(filed_client_fd);
        (void)pacha_fd_close(filed_endpoint_fd);
        fprintf(stderr, "[seed0root] filed start failed status=%d\n", status);
        return status;
    }
    (void)pacha_fd_close(filed_endpoint_fd);
    printf("[seed0root] filed started\n");
    fflush(stdout);
    const long boot_client_dup =
        pacha_fd_fcntl(filed_client_fd, PACHA_FD_FCNTL_DUP, 16, seed0root_channel_rights);
    if (boot_client_dup < 16) {
        (void)pacha_fd_close(filed_client_fd);
        fprintf(stderr,
            "[seed0root] storage ready filed endpoint dup failed status=%ld fd=%d\n",
            boot_client_dup,
            filed_client_fd);
        return boot_client_dup < 0 ? (int)boot_client_dup : -2;
    }
    const int boot_client_fd = (int)boot_client_dup;
    status = seed0root_send_storage_ready((int)bootstrap->ready_channel_fd, boot_client_fd);
    (void)pacha_fd_close(boot_client_fd);
    if (status != 0) {
        (void)pacha_fd_close(filed_client_fd);
        fprintf(stderr, "[seed0root] storage ready send failed status=%d fd=%llu\n",
            status,
            (unsigned long long)bootstrap->ready_channel_fd);
        return status;
    }
    printf("[seed0root] storage ready signal sent\n");
    fflush(stdout);
    status = seed0root_wait_services_ready((int)bootstrap->service_ready_channel_fd);
    if (status != 0) {
        (void)pacha_fd_close(filed_client_fd);
        return status;
    }
    status = seed0root_run_storage_services(filed_client_fd);
    if (status != 0) {
        (void)pacha_fd_close(filed_client_fd);
        fprintf(stderr, "[seed0root] filed service failed status=%d\n", status);
        return status;
    }
    int lpr_supervisor_endpoint_fd = -1;
    status = seed0root_start_lpr_supervisor(filed_client_fd, &lpr_supervisor_endpoint_fd);
    if (status != 0) {
        (void)pacha_fd_close(filed_client_fd);
        fprintf(stderr, "[seed0root] lpr supervisor launch failed status=%d\n", status);
        return status;
    }
    status = seed0root_register_termd_signal_supervisor(
        filed_client_fd,
        lpr_supervisor_endpoint_fd);
    if (status != 0) {
        (void)pacha_fd_close(lpr_supervisor_endpoint_fd);
        (void)pacha_fd_close(filed_client_fd);
        fprintf(stderr,
            "[seed0root] termd signal supervisor register failed status=%d\n",
            status);
        return status;
    }
    status = seed0root_spawn_lpr_session(
        filed_client_fd,
        lpr_supervisor_endpoint_fd);
    (void)pacha_fd_close(lpr_supervisor_endpoint_fd);
    (void)pacha_fd_close(filed_client_fd);
    if (status != 0) {
        fprintf(stderr, "[seed0root] user session launch failed status=%d\n", status);
        return status;
    }
    printf("[seed0root] storage ready\n");
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    printf("[seed0root] start\n");
    fflush(stdout);
    int bootstrap_fd = -1;
    int bootstrap_status = find_seed0root_bootstrap_fd(argv, &bootstrap_fd);
    if (bootstrap_status != 0) {
        fprintf(stderr, "[seed0root] bootstrap lookup failed status=%d\n", bootstrap_status);
        return 4;
    }
    printf("[seed0root] bootstrap fd=%d\n", bootstrap_fd);
    fflush(stdout);
    struct seed0root_bootstrap bootstrap;
    bootstrap_status = read_bootstrap_fd(bootstrap_fd, &bootstrap, sizeof(bootstrap), "seed0root");
    printf("[seed0root] bootstrap read status=%d magic=0x%llx device_fd=%llu ready_fd=%llu service_ready_fd=%llu filed_fd=%llu filed_size=%llu modules=%llu\n",
        bootstrap_status,
        (unsigned long long)bootstrap.magic,
        (unsigned long long)bootstrap.device_fd,
        (unsigned long long)bootstrap.ready_channel_fd,
        (unsigned long long)bootstrap.service_ready_channel_fd,
        (unsigned long long)bootstrap.filed_image_fd,
        (unsigned long long)bootstrap.filed_image_size,
        (unsigned long long)bootstrap.module_count);
    fflush(stdout);
    if (bootstrap_status != 0 ||
        bootstrap.magic != SEED0ROOT_BOOTSTRAP_MAGIC ||
        bootstrap.device_fd < 16 ||
        bootstrap.ready_channel_fd < 16 ||
        bootstrap.service_ready_channel_fd < 16 ||
        bootstrap.filed_image_fd < 16 ||
        bootstrap.filed_image_size == 0 ||
        bootstrap.module_count == 0 ||
        bootstrap.module_count > SEED0ROOT_BOOTSTRAP_MAX_MODULES)
    {
        fprintf(stderr, "[seed0root] bootstrap invalid status=%d\n", bootstrap_status);
        fflush(stderr);
        return 4;
    }
    printf("[seed0root] filed launching\n");
    fflush(stdout);
    int launch_status = launch_filed(&bootstrap);
    if (launch_status != 0) {
        fprintf(stderr, "[seed0root] filed launch failed status=%d\n", launch_status);
        return 5;
    }
    printf("[seed0root] ready\n");
    fflush(stdout);
    fflush(stderr);
    return 0;
}
