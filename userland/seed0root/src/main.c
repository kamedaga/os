#include "pacha/ipc.h"
#include "pacha/launch.h"
#include "pacha/service_abi.h"
#include "pacha/syscall.h"
#include "filed/payload.h"
#include "filed/ipc_protocol.h"
#include "lpr_supervisor/boot_config.h"
#include "lpr_supervisor/ipc_protocol.h"
#include "personality/linux_lpr.h"
#include "personality/lpr_manifest.h"
#include "pacha/root_handoff.h"
#include "pacha/live_bootstrap.h"
#include "pacha/capsule.h"
#include "live_console.h"
#include "storage/bootstrap.h"
#include "netd/boot_config.h"
#include "termd/boot_config.h"
#include "gpud/boot_config.h"
#include "inputd/boot_config.h"
#include "usbd/boot_config.h"
#include "unixd/ipc_protocol.h"

#ifndef SEED0ROOT_DEFAULT_BOOT_PROFILE
#define SEED0ROOT_DEFAULT_BOOT_PROFILE 0u
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

enum {
    SEED0ROOT_STORAGE_READY_MAGIC = 0x3159445252545330ull,
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
    SEED0ROOT_BOOT_PROFILE_CURL = 1u << 8,
    SEED0ROOT_BOOT_PROFILE_HTTPS = 1u << 9,
    SEED0ROOT_BOOT_PROFILE_APK_UPDATE = 1u << 10,
    SEED0ROOT_FD_KIND_DEVICE = 8,
    SEED0ROOT_SERVICE_DEVICE_FD = 224,
    SEED0ROOT_SERVICE_ENDPOINT_FD = 232,
    SEED0ROOT_SERVICE_READY_FD = 233,
    SEED0ROOT_SERVICE_NETD_FD = 234,
    SEED0ROOT_SERVICE_INPUT_SOURCE_FD = 235,
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
    const struct pacha_process_fd_grant *grants,
    uint64_t grant_count,
    struct seed0root_loaded_process *out);
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
    if (trace) {
        printf("[seed0root] %s ready\n", label);
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
    uint64_t transfer_rights,
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
        fds[fd_count].rights = transfer_rights;
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

static int seed0root_lprs_call_cap(
    int endpoint_fd,
    uint32_t op,
    uint64_t request_id,
    int page_fd,
    void *page,
    uint32_t payload_size,
    int transfer_fd,
    struct pacha_ipc_msg *out_reply,
    int *out_cap)
{
    if (out_cap) *out_cap = -1;
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

    struct pacha_ipc_fd cap = {0};
    memset(out_reply, 0, sizeof(*out_reply));
    out_reply->fds = &cap;
    out_reply->fd_capacity = 1;
    const int recv_status = recv_ipc_wait(reply_fd, out_reply);
    out_reply->fds = NULL;
    out_reply->fd_capacity = 0;
    (void)pacha_fd_close(reply_fd);
    if (recv_status != 0) {
        if (cap.fd >= 16) (void)pacha_fd_close((int)cap.fd);
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
        reply_header->request_id != request_id ||
        reply_header->status != (int64_t)out_reply->word1 ||
        out_reply->fd_count > 1)
    {
        if (cap.fd >= 16) (void)pacha_fd_close((int)cap.fd);
        if (owned_page_fd >= 16) {
            seed0root_destroy_filed_page(owned_page_fd, owned_page);
        }
        return -2;
    }
    if ((int64_t)out_reply->word1 < 0) {
        if (cap.fd >= 16) (void)pacha_fd_close((int)cap.fd);
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
    if (out_reply->fd_count) {
        if (!out_cap || cap.fd < 16 || cap.fd >= PACHA_FD_TABLE_LIMIT) {
            if (cap.fd >= 16) (void)pacha_fd_close((int)cap.fd);
            return -5;
        }
        *out_cap = (int)cap.fd;
    }
    return 0;
}

static int seed0root_lprs_call(int endpoint_fd, uint32_t op, uint64_t request_id,
    int page_fd, void *page, uint32_t payload_size, int transfer_fd,
    struct pacha_ipc_msg *out_reply)
{
    return seed0root_lprs_call_cap(endpoint_fd, op, request_id, page_fd, page,
        payload_size, transfer_fd, out_reply, NULL);
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
    memset(&reply, 0, sizeof(reply));
    struct pacha_ipc_fd exec_fds[2] = {
        { .fd = (uint64_t)page_fd, .rights = PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE },
        { .fd = (uint64_t)filed_endpoint_fd,
            .rights = PACHA_LAUNCH_CLIENT | PACHA_FD_RIGHT_TRANSFER },
    };
    uint64_t exec_fd_count = 1;
    if (!(exec_flags & FILED_EXEC_LINUX_LPR)) {
        exec->flags |= FILED_EXEC_INHERIT_FDS;
        exec->inherit_fd_count = 1;
        exec->fd_grants[0].target = 240;
        exec->fd_grants[0].rights = exec_fds[1].rights;
        exec_fd_count = 2;
    }
    status = seed0root_filed_page_call_fdv(
        filed_endpoint_fd,
        FILED_OP_EXEC_PATH,
        0x5eed0f11u,
        exec_fds,
        exec_fd_count,
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
            (void)pacha_syscall2(
                PACHA_PROCESS_SYSCALL_KILL, reply_fds[0].fd, 1);
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
    const struct pacha_process_fd_grant *grants,
    uint64_t grant_count,
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
    const int process_fd = pacha_process_create(process_rights, 0, grants, grant_count);
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
    if (thread_fd < 16) {
        (void)pacha_fd_close(process_fd);
        return -7;
    }
    const int start_status = pacha_thread_start(thread_fd);
    if (start_status != 0) {
        (void)pacha_fd_close(thread_fd);
        (void)pacha_fd_close(process_fd);
        return -8;
    }
    if (out_started != NULL) {
        out_started->process_fd = process_fd;
        out_started->thread_fd = thread_fd;
    } else {
        (void)pacha_fd_close(thread_fd);
        (void)pacha_fd_close(process_fd);
    }
    return 0;
}

static int prepare_filed_storage_bootstrap(
    const storage_seed0root_bootstrap_t *bootstrap,
    int filed_endpoint_fd,
    storage_filed_bootstrap_t *out_bootstrap)
{
    if (bootstrap == NULL ||
        out_bootstrap == NULL ||
        filed_endpoint_fd < 16 ||
        bootstrap->module_count == 0 ||
        bootstrap->module_count > STORAGE_STACK_MODULE_CAPACITY ||
        !storage_module_table_matches_manifest(
            bootstrap->modules, bootstrap->module_count))
    {
        return -1;
    }

    memset(out_bootstrap, 0, sizeof(*out_bootstrap));
    out_bootstrap->magic = STORAGE_FILED_BOOTSTRAP_MAGIC;
    out_bootstrap->device_fd = bootstrap->device_fd;
    out_bootstrap->control_fd = (uint64_t)(uint32_t)filed_endpoint_fd;
    out_bootstrap->module_count = bootstrap->module_count;
    for (uint64_t i = 0; i < bootstrap->module_count; i++) {
        const storage_module_image_desc_t *src = &bootstrap->modules[i];
        if (src->name[0] == '\0' || src->image_fd < 16 || src->image_size < 4) {
            return -2;
        }
        snprintf(out_bootstrap->modules[i].name, sizeof(out_bootstrap->modules[i].name), "%s", src->name);
        out_bootstrap->modules[i].image_fd = src->image_fd;
        out_bootstrap->modules[i].image_size = src->image_size;
    }

    return 0;
}

struct seed0root_root_devices {
    struct {
        uint64_t device_count;
        struct pacha_root_device_record *devices;
    } metadata;
    int *fds;
};

struct seed0root_live_usb_ready {
    int *fds;
    size_t count;
    size_t capacity;
    int net_fd;
    int net_state; /* 0 absent, 1 pending, 2 ready, 3 failed */
    int net_ipv4_ready;
    int net_status;
    uint64_t net_stage;
    unsigned net_carrier, net_mtu;
    unsigned net_nic_step, net_loaded, net_pci_bound;
    int net_detail;
    uint64_t net_source;
    unsigned net_line;
    unsigned net_fault_vector, net_fault_error_code, net_fault_core_relative;
    uint64_t net_fault_ip, net_fault_address;
};

static void seed0root_close_live_usb_ready(struct seed0root_live_usb_ready *ready)
{
    if (ready == NULL) return;
    for (size_t i = 0; i < ready->count; i++)
        if (ready->fds[i] >= 16) (void)pacha_fd_close(ready->fds[i]);
    if (ready->net_fd >= 16) (void)pacha_fd_close(ready->net_fd);
    free(ready->fds);
    memset(ready, 0, sizeof(*ready));
}

static void seed0root_poll_live_usb_ready(void *context)
{
    struct seed0root_live_usb_ready *ready = context;
    if (ready == NULL) return;
    for (size_t i = 0; i < ready->count; i++) {
        const int fd = ready->fds[i];
        if (fd < 16) continue;
        struct pacha_pollfd event = {
            .fd = fd,
            .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP,
        };
        const long polled = pacha_fd_wait_many(&event, 1, 0);
        if (polled == PACHA_ERR_NOT_READY) continue;
        struct pacha_ipc_msg message = {0};
        const int received = polled == 1 &&
            (event.revents & PACHA_FD_EVENT_READABLE) != 0 ?
            pacha_ipc_recv(fd, &message) : -5;
        const int healthy = received == 0 &&
            message.word0 == USBD_BOOT_READY_MAGIC && message.word1 == 0 &&
            message.word2 == 1 && message.fd_count == 0;
        printf("[seed0root] usbd slot %zu %s receive=%d service=%lld\n",
            i, healthy ? "ready" : "failed", received,
            (long long)message.word1);
        fflush(stdout);
        (void)pacha_fd_close(fd);
        /* Keep the first readiness result for the boot report without
         * waiting for slow real USB hubs before launching ash. */
        ready->fds[i] = healthy ? -1 : -2;
    }
    if (ready->net_fd >= 16) {
        struct pacha_pollfd event = {.fd = ready->net_fd,
            .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
        const long polled = pacha_fd_wait_many(&event, 1, 0);
        if (polled != PACHA_ERR_NOT_READY) {
            struct pacha_ipc_msg message = {0};
            const int received = polled == 1 &&
                (event.revents & PACHA_FD_EVENT_READABLE) != 0 ?
                pacha_ipc_recv(ready->net_fd, &message) : -5;
            const int fault_report = message.word0 == NETD_BOOT_FAULT_MAGIC;
            if (received == 0 && message.fd_count == 0 &&
                (fault_report || (message.word0 == NETD_BOOT_STATUS_MAGIC &&
                (message.word2 & 0xffu) >= NETD_BOOT_STAGE_NIC &&
                (message.word2 & 0xffu) <= NETD_BOOT_STAGE_LINK))) {
                ready->net_status = (int)(int32_t)message.word1;
                ready->net_stage = fault_report ? NETD_BOOT_STAGE_FAULT :
                    message.word2 & 0xffu;
                if (!fault_report && ready->net_status == 0 &&
                    ready->net_stage == NETD_BOOT_STAGE_IPV4)
                    ready->net_ipv4_ready = 1;
                if (fault_report) {
                    const unsigned encoded = (unsigned)(message.word1 >> 32);
                    ready->net_fault_error_code = encoded & 0xffffu;
                    ready->net_fault_vector = (encoded >> 16) & 0x7fffu;
                    ready->net_fault_core_relative = encoded >> 31;
                    ready->net_fault_ip = message.word2;
                    ready->net_fault_address = message.word3;
                } else if (ready->net_status && ready->net_stage == NETD_BOOT_STAGE_NIC) {
                    ready->net_nic_step = (unsigned)((message.word2 >> 8) & 0xffu);
                    ready->net_loaded = (unsigned)((message.word2 >> 16) & 0xffu);
                    ready->net_pci_bound = (unsigned)((message.word2 >> 24) & 1u);
                    ready->net_line = (unsigned)(message.word2 >> 32);
                    ready->net_detail = (int)(int32_t)(message.word1 >> 32);
                    ready->net_source = message.word3;
                } else if (!ready->net_status) {
                    ready->net_carrier = (unsigned)(message.word3 & 0xffu);
                    ready->net_mtu = (unsigned)(message.word3 >> 8);
                }
                ready->net_state = ready->net_status ? 3 : 2;
            } else {
                ready->net_state = 3;
                ready->net_status = received ? received : -22;
                ready->net_stage = 0;
            }
            printf("[seed0root] netd state=%d stage=%llu status=%d nic_step=%u detail=%d loaded=%u pci_bound=%u source=0x%llx line=%u carrier=%u mtu=%u fault_vector=%u fault_code=%u fault_core=%u fault_ip=0x%llx fault_addr=0x%llx\n",
                ready->net_state, (unsigned long long)ready->net_stage,
                ready->net_status, ready->net_nic_step, ready->net_detail,
                ready->net_loaded, ready->net_pci_bound,
                (unsigned long long)ready->net_source, ready->net_line,
                ready->net_carrier, ready->net_mtu,
                ready->net_fault_vector, ready->net_fault_error_code,
                ready->net_fault_core_relative,
                (unsigned long long)ready->net_fault_ip,
                (unsigned long long)ready->net_fault_address);
            fflush(stdout);
            if (ready->net_state == 3) {
                (void)pacha_fd_close(ready->net_fd);
                ready->net_fd = -1;
            }
        }
    }
}

static void seed0root_close_root_devices(struct seed0root_root_devices *devices)
{
    if (devices == NULL) return;
    for (uint64_t i = 0; i < devices->metadata.device_count; i++)
        if (devices->fds[i] >= 16) (void)pacha_fd_close(devices->fds[i]);
    free(devices->fds);
    free(devices->metadata.devices);
    memset(devices, 0, sizeof(*devices));
}

static int seed0root_receive_root_handoff(
    int channel_fd,
    struct seed0root_root_devices *out)
{
    if (channel_fd < 16 || out == NULL) return -22;
    memset(out, 0, sizeof(*out));
    int status = 0;
    for (;;) {
        struct pacha_ipc_fd fds[1 + PACHA_ROOT_HANDOFF_BATCH_DEVICES];
        memset(fds, 0, sizeof(fds));
        struct pacha_ipc_msg msg;
        memset(&msg, 0, sizeof(msg));
        msg.fds = fds;
        msg.fd_capacity = 1 + PACHA_ROOT_HANDOFF_BATCH_DEVICES;
        status = recv_ipc_wait(channel_fd, &msg);
        if (status != 0 || msg.word0 != PACHA_ROOT_HANDOFF_MAGIC ||
            msg.word1 != PACHA_ROOT_HANDOFF_VERSION ||
            msg.word2 > PACHA_ROOT_HANDOFF_BATCH_DEVICES ||
            msg.fd_count != msg.word2 + 1 || fds[0].fd < 16 ||
            (msg.word3 & ~PACHA_ROOT_HANDOFF_FLAG_LAST) != 0) {
            if (status == 0) status = -22;
            goto fail_batch;
        }

        struct pacha_fd_info info;
        memset(&info, 0, sizeof(info));
        if (pacha_fd_get_info((int)fds[0].fd, &info) != 0 ||
            info.kind != PACHA_FD_KIND_VMO ||
            (info.rights & PACHA_FD_RIGHT_MAP_READ) == 0) {
            status = -13;
            goto fail_batch;
        }
        const void *page = pacha_mmap((int)fds[0].fd, 4096, PACHA_PROT_READ,
            PACHA_MMAP_SHARED, 0);
        if (page == NULL) {
            status = -5;
            goto fail_batch;
        }
        struct pacha_root_handoff batch;
        memcpy(&batch, page, sizeof(batch));
        (void)pacha_munmap((void *)page, 4096);
        if (batch.magic != PACHA_ROOT_HANDOFF_MAGIC ||
            batch.version != PACHA_ROOT_HANDOFF_VERSION ||
            batch.device_count != msg.word2 || batch.flags != msg.word3) {
            status = -22;
            goto fail_batch;
        }
        uint32_t seen = 0;
        for (uint64_t i = 0; i < batch.device_count; i++) {
            const struct pacha_root_device_record *record = &batch.devices[i];
            if (record->transfer_index >= batch.device_count ||
                (seen & (1u << record->transfer_index)) != 0) {
                status = -22;
                goto fail_batch;
            }
            seen |= 1u << record->transfer_index;
            const int fd = (int)fds[1 + record->transfer_index].fd;
            memset(&info, 0, sizeof(info));
            if (fd < 16 || pacha_fd_get_info(fd, &info) != 0 ||
                info.kind != SEED0ROOT_FD_KIND_DEVICE ||
                (info.rights & (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE)) !=
                    (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE)) {
                status = -13;
                goto fail_batch;
            }
        }
        if (batch.device_count > (SIZE_MAX / sizeof(*out->fds)) -
                out->metadata.device_count ||
            batch.device_count > (SIZE_MAX / sizeof(*out->metadata.devices)) -
                out->metadata.device_count) {
            status = -7;
            goto fail_batch;
        }
        const size_t total = (size_t)(out->metadata.device_count + batch.device_count);
        if (total != 0) {
            struct pacha_root_device_record *records =
                realloc(out->metadata.devices, total * sizeof(*records));
            if (records == NULL) {
                status = -3;
                goto fail_batch;
            }
            out->metadata.devices = records;
            int *more_fds = realloc(out->fds, total * sizeof(*more_fds));
            if (more_fds == NULL) {
                status = -3;
                goto fail_batch;
            }
            out->fds = more_fds;
        }
        for (uint64_t i = 0; i < batch.device_count; i++) {
            const size_t slot = (size_t)out->metadata.device_count + (size_t)i;
            out->metadata.devices[slot] = batch.devices[i];
            out->fds[slot] = (int)fds[1 + batch.devices[i].transfer_index].fd;
            fds[1 + batch.devices[i].transfer_index].fd = 0;
        }
        out->metadata.device_count += batch.device_count;
        (void)pacha_fd_close((int)fds[0].fd);
        fds[0].fd = 0;
        if ((batch.flags & PACHA_ROOT_HANDOFF_FLAG_LAST) != 0) break;
        continue;

fail_batch:
        for (uint64_t i = 0; i < 1 + PACHA_ROOT_HANDOFF_BATCH_DEVICES; i++)
            if (fds[i].fd >= 16) (void)pacha_fd_close((int)fds[i].fd);
        (void)pacha_fd_close(channel_fd);
        seed0root_close_root_devices(out);
        fprintf(stderr, "[seed0root] invalid root capability handoff status=%d\n", status);
        return status;
    }
    (void)pacha_fd_close(channel_fd);
    printf("[seed0root] root capability handoff received devices=%llu\n",
        (unsigned long long)out->metadata.device_count);
    fflush(stdout);
    return 0;
}

static int seed0root_find_root_device(
    const struct seed0root_root_devices *devices,
    uint64_t vendor_id,
    uint64_t device_id,
    uint64_t alternate_device_id)
{
    if (devices == NULL) return -1;
    for (uint64_t i = 0; i < devices->metadata.device_count; i++) {
        const struct pacha_root_device_record *record = &devices->metadata.devices[i];
        if (record->vendor_id == vendor_id &&
            (record->device_id == device_id || record->device_id == alternate_device_id))
            return (int)i;
    }
    return -1;
}

static int seed0root_find_root_ethernet(const struct seed0root_root_devices *devices)
{
    if (devices == NULL) return -1;
    for (uint64_t i = 0; i < devices->metadata.device_count; ++i)
        if ((devices->metadata.devices[i].class_code >> 8) == 0x0200)
            return (int)i;
    return -1;
}

#if defined(SEED0ROOT_UNIXD_CONTRACT_TEST) && SEED0ROOT_UNIXD_CONTRACT_TEST
/* Test builds retain only the capability returned for this exact launch.
 * No PID lookup, new kernel authority, or service control API is needed. */
static int seed0root_test_netd_process = -1;
static int seed0root_test_native_status;
#endif

static int seed0root_exec_native_service(
    int filed_endpoint_fd,
    const char *path,
    const void *bootstrap,
    uint64_t bootstrap_size,
    const struct pacha_process_fd_grant *grants,
    uint64_t inherit_count,
    uint64_t request_id)
{
    if (filed_endpoint_fd < 16 || path == NULL || bootstrap == NULL ||
        bootstrap_size == 0 || inherit_count > FILED_EXEC_MAX_INHERIT_FDS ||
        (inherit_count && grants == NULL))
        return -22;
    const int bootstrap_fd = create_inherited_vmo_from_bytes(
        bootstrap, bootstrap_size, "service bootstrap fd");
    if (bootstrap_fd < 16) return bootstrap_fd;

    int page_fd = -1;
    void *page = NULL;
    int status = seed0root_create_filed_page(&page_fd, &page);
    if (status != 0) {
        (void)pacha_fd_close(bootstrap_fd);
        return status;
    }
    filed_exec_path_t *exec = (filed_exec_path_t *)page;
    exec->flags = FILED_EXEC_BOOTSTRAP_FD | FILED_EXEC_INHERIT_FDS;
    exec->inherit_fd_count = inherit_count;
    exec->argc = 1;
    snprintf(exec->path, sizeof(exec->path), "%s", path);
    status = seed0root_exec_add_string(exec, &exec->argv[0], path);
    for (uint64_t i = 0; status == 0 && i < inherit_count; i++) {
        if (grants[i].source_fd < 16 || grants[i].target_fd < 16 ||
            grants[i].target_fd >= PACHA_FD_TABLE_LIMIT)
            status = -22;
        exec->fd_grants[i] = (filed_exec_fd_grant_t){ .target = grants[i].target_fd,
            .rights = grants[i].rights, .flags = grants[i].flags };
    }
    struct pacha_ipc_fd fds[2 + FILED_EXEC_MAX_INHERIT_FDS];
    memset(fds, 0, sizeof(fds));
    fds[0].fd = (uint64_t)(uint32_t)page_fd;
    fds[0].rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    for (uint64_t i = 0; status == 0 && i < inherit_count; i++) {
        fds[1 + i].fd = grants[i].source_fd;
        fds[1 + i].rights = grants[i].rights | PACHA_FD_RIGHT_TRANSFER;
    }
    fds[1 + inherit_count].fd = (uint64_t)(uint32_t)bootstrap_fd;
    fds[1 + inherit_count].rights = PACHA_LAUNCH_BLOB | PACHA_FD_RIGHT_TRANSFER;

    struct pacha_ipc_fd reply_fds[2];
    memset(reply_fds, 0, sizeof(reply_fds));
    struct pacha_ipc_msg reply;
    if (status == 0)
        status = seed0root_filed_page_call_fdv(filed_endpoint_fd,
            FILED_OP_EXEC_PATH, request_id, fds, inherit_count + 2, 0,
            &reply, reply_fds, 2);
    seed0root_destroy_filed_page(page_fd, page);
    (void)pacha_fd_close(bootstrap_fd);
    if (status == 0 && reply.fd_count < 2) status = -5;
#if defined(SEED0ROOT_UNIXD_CONTRACT_TEST) && SEED0ROOT_UNIXD_CONTRACT_TEST
    if (status == 0 && strcmp(path, "/srv/netd.elf") == 0) {
        seed0root_test_netd_process = (int)reply_fds[0].fd;
        reply_fds[0].fd = 0;
    }
#endif
    for (uint64_t i = 0; i < 2; i++)
        if (reply_fds[i].fd >= 16) (void)pacha_fd_close((int)reply_fds[i].fd);
    return status;
}

static int seed0root_register_service_endpoint(
    int filed_endpoint_fd,
    uint32_t op,
    int service_endpoint_fd,
    uint64_t request_id)
{
    struct pacha_ipc_msg reply;
    pacha_service_envelope_t header;
    memset(&reply, 0, sizeof(reply));
    memset(&header, 0, sizeof(header));
    return seed0root_filed_service_call(filed_endpoint_fd, op, request_id,
        sizeof(filed_service_endpoint_request_t), service_endpoint_fd,
        PACHA_LAUNCH_CLIENT | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SET_FLAGS,
        &reply, &header);
}

static int seed0root_wait_service_ready(int channel_fd, uint64_t magic, const char *name)
{
    struct pacha_ipc_msg msg;
    memset(&msg, 0, sizeof(msg));
    const int status = recv_ipc_wait(channel_fd, &msg);
    (void)pacha_fd_close(channel_fd);
    if (status != 0 || msg.word0 != magic || msg.word1 != 0 || msg.fd_count != 0) {
        fprintf(stderr, "[seed0root] %s ready failed status=%d service_status=%lld magic=0x%llx\n",
            name, status, (long long)msg.word1, (unsigned long long)msg.word0);
        return status != 0 ? status : -5;
    }
    return 0;
}

#if defined(SEED0ROOT_GPUD_RESTART_TEST) && SEED0ROOT_GPUD_RESTART_TEST
static int seed0root_test_gpud_restart(int filed_endpoint_fd, int control_fd)
{
    const uint64_t deadline = seed0root_now_ns() + 60000000000ull;
    const int timer = pacha_timerfd_create(100000000, 100000000,
        PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL |
            PACHA_FD_RIGHT_CLOSE,
        0);
    int status = timer >= 16 ? 0 : -5;
    int requested = 0;
    while (status == 0 && seed0root_now_ns() < deadline) {
        char marker[8] = {0};
        if (seed0root_read_filed_text(filed_endpoint_fd,
                "/tmp/gpud-restart-ready", marker, sizeof(marker)) == 0 &&
            strcmp(marker, "1\n") == 0) {
            requested = 1;
            break;
        }
        struct pacha_pollfd tick = {
            .fd = timer,
            .events = PACHA_FD_EVENT_READABLE,
        };
        uint64_t expirations = 0;
        if (pacha_fd_wait_many(&tick, 1, PACHA_FD_WAIT_FOREVER) != 1 ||
            pacha_fd_read(timer, &expirations, sizeof(expirations)) !=
                (long)sizeof(expirations))
            status = -5;
    }
    if (timer >= 16)
        (void)pacha_fd_close(timer);
    if (status == 0 && !requested)
        status = -110;

    const struct pacha_ipc_msg request = {
        .word0 = GPUD_CONTROL_FORCE_RESTART_MAGIC,
        .word1 = 1,
    };
    if (status == 0)
        status = pacha_ipc_send(control_fd, &request);
    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    if (status == 0) status = recv_ipc_wait(control_fd, &reply);
    if (status == 0 &&
        (reply.word0 != GPUD_CONTROL_RESTARTED_MAGIC || reply.word1 != 2 ||
         reply.word2 != 1 || reply.word3 != 0 || reply.fd_count != 0))
        status = -5;
    printf("GPUD_SANDBOX_RESTART_TEST=%s old=1 new=%llu\n",
        status == 0 ? "PASS" : "FAIL", (unsigned long long)reply.word1);
    fflush(stdout);
    return status;
}
#endif

static int seed0root_start_unixd(int filed_endpoint_fd, int filed_path_fd, int *out_admin)
{
    *out_admin = -1;
    const int admin = pacha_ipc_endpoint_create(seed0root_channel_rights, 0);
    if (admin < 16) return -5;
    struct pacha_ipc_channel_pair ready = { .a = -1, .b = -1 };
    if (pacha_ipc_channel_create(&ready, seed0root_channel_rights, 0) != 0) {
        (void)pacha_fd_close(admin);
        return -5;
    }
    const struct unix_boot_config config = { .magic = UNIX_BOOT_MAGIC,
        .version = UNIX_SERVICE_VERSION, .admin_endpoint = SEED0ROOT_SERVICE_ENDPOINT_FD,
        .filed_path_channel = 234,
        .ready_channel = SEED0ROOT_SERVICE_READY_FD };
    const struct pacha_process_fd_grant grants[] = {
        PACHA_LAUNCH_GRANT(admin, SEED0ROOT_SERVICE_ENDPOINT_FD, PACHA_LAUNCH_SERVER),
        PACHA_LAUNCH_GRANT(ready.b, SEED0ROOT_SERVICE_READY_FD, PACHA_LAUNCH_SIGNAL),
        { .source_fd = (uint64_t)filed_path_fd, .target_fd = 234,
          .rights = PACHA_LAUNCH_CLIENT, .flags = PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC },
    };
    int status = seed0root_exec_native_service(filed_endpoint_fd, "/srv/unixd.elf",
        &config, sizeof(config), grants, 3, 0x5eed2010u);
    (void)pacha_fd_close(ready.b);
    if (status == 0) status = seed0root_wait_service_ready(ready.a, UNIX_READY_MAGIC, "unixd");
    else (void)pacha_fd_close(ready.a);
    if (status != 0) { (void)pacha_fd_close(admin); return status; }
    *out_admin = admin;
    return 0;
}

static int seed0root_launch_root_services(
    int filed_endpoint_fd,
    struct seed0root_root_devices *devices,
    int *out_gpud_control_fd,
    int live_mode,
    int *out_live_termd_fd,
    int *out_live_inputd_fd,
    struct seed0root_live_usb_ready *out_live_usb_ready)
{
    if (out_gpud_control_fd == NULL ||
        (live_mode && (out_live_termd_fd == NULL ||
            out_live_inputd_fd == NULL || out_live_usb_ready == NULL)))
        return -22;
    *out_gpud_control_fd = -1;
    if (live_mode) {
        *out_live_termd_fd = -1;
        *out_live_inputd_fd = -1;
    }
    enum {
        VIRTIO_VENDOR = 0x1af4,
        CONSOLE_LEGACY = 0x1003, CONSOLE_MODERN = 0x1043,
        GPU_LEGACY = 0x1010, GPU_MODERN = 0x1050,
        INPUT_MODERN = 0x1052,
    };
    int status = 0;
    int termd_endpoint = -1, netd_endpoint = -1, gpud_drm_endpoint = -1;
    int inputd_endpoint = -1, input_source_endpoint = -1;
    struct pacha_ipc_channel_pair ready = { .a = -1, .b = -1 };
    struct pacha_ipc_channel_pair gpud_control = { .a = -1, .b = -1 };

    const int console = seed0root_find_root_device(devices, VIRTIO_VENDOR,
        CONSOLE_LEGACY, CONSOLE_MODERN);
    termd_endpoint = pacha_ipc_endpoint_create(seed0root_channel_rights, 0);
    if (termd_endpoint < 16 ||
        pacha_ipc_channel_create(&ready, seed0root_channel_rights, 0) != 0) {
        status = -5;
        goto out;
    }
    struct termd_boot_config termd_cfg;
    memset(&termd_cfg, 0, sizeof(termd_cfg));
    termd_cfg.magic = TERMD_BOOT_CONFIG_MAGIC;
    termd_cfg.version = TERMD_BOOT_CONFIG_VERSION;
    termd_cfg.tty_endpoint_fd = SEED0ROOT_SERVICE_ENDPOINT_FD;
    termd_cfg.device_fd = console >= 0 ? SEED0ROOT_SERVICE_DEVICE_FD : 0;
    termd_cfg.ready_channel_fd = SEED0ROOT_SERVICE_READY_FD;
    const struct pacha_process_fd_grant termd_grants[] = {
        PACHA_LAUNCH_GRANT(termd_endpoint, SEED0ROOT_SERVICE_ENDPOINT_FD, PACHA_LAUNCH_SERVER),
        PACHA_LAUNCH_GRANT(ready.b, SEED0ROOT_SERVICE_READY_FD, PACHA_LAUNCH_SIGNAL),
        PACHA_LAUNCH_GRANT(filed_endpoint_fd, 240, PACHA_LAUNCH_CLIENT),
        PACHA_LAUNCH_GRANT(console >= 0 ? devices->fds[console] : -1,
            SEED0ROOT_SERVICE_DEVICE_FD, PACHA_LAUNCH_DEVICE_DRIVER),
    };
    const uint64_t termd_count = console >= 0 ? 4 : 3;
    status = seed0root_exec_native_service(filed_endpoint_fd, "/srv/termd.elf",
        &termd_cfg, sizeof(termd_cfg), termd_grants, termd_count,
        0x5eed2001u);
    (void)pacha_fd_close(ready.b); ready.b = -1;
    if (status != 0) goto out;
    status = seed0root_register_service_endpoint(filed_endpoint_fd,
        FILED_OP_SERVICE_SET_TERMD_TTY, termd_endpoint, 0x5eed2002u);
    if (status != 0) goto out;
    { const int ready_fd = ready.a; ready.a = -1;
      status = seed0root_wait_service_ready(ready_fd, TERMD_BOOT_READY_MAGIC, "termd"); }
    if (status != 0) goto out;

    {
        const int net = seed0root_find_root_ethernet(devices);
        if (!live_mode && net < 0) { status = -19; goto out; }
        if (net >= 0) {
        struct pacha_ipc_channel_pair net_ready = {.a = -1, .b = -1};
        if (live_mode) {
            out_live_usb_ready->net_state = 1;
            status = pacha_ipc_channel_create(&net_ready,
                seed0root_channel_rights, 0);
            if (status != 0) goto out;
        }
        netd_endpoint = pacha_ipc_endpoint_create(seed0root_channel_rights, 0);
        if (netd_endpoint < 16) {
            if (net_ready.a >= 16) (void)pacha_fd_close(net_ready.a);
            if (net_ready.b >= 16) (void)pacha_fd_close(net_ready.b);
            status = -5;
            goto out;
        }
        struct netd_boot_config netd_cfg;
        memset(&netd_cfg, 0, sizeof(netd_cfg));
        netd_cfg.magic = NETD_BOOT_CONFIG_MAGIC;
        netd_cfg.version = NETD_BOOT_CONFIG_VERSION;
        netd_cfg.device_fd = SEED0ROOT_SERVICE_DEVICE_FD;
        netd_cfg.filed_endpoint_fd = 240;
        netd_cfg.socket_endpoint_fd = SEED0ROOT_SERVICE_ENDPOINT_FD;
        netd_cfg.status_channel_fd = live_mode ? SEED0ROOT_SERVICE_READY_FD : 0;
        /* netd owns network policy; this handoff grants an Ethernet function. */
        const struct pacha_process_fd_grant netd_grants[] = {
            PACHA_LAUNCH_GRANT(devices->fds[net], SEED0ROOT_SERVICE_DEVICE_FD,
                PACHA_LAUNCH_DEVICE_DRIVER | PACHA_FD_RIGHT_TRANSFER),
            PACHA_LAUNCH_GRANT(netd_endpoint, SEED0ROOT_SERVICE_ENDPOINT_FD, PACHA_LAUNCH_SERVER),
            PACHA_LAUNCH_GRANT(filed_endpoint_fd, 240, PACHA_LAUNCH_CLIENT),
            PACHA_LAUNCH_GRANT(net_ready.b, SEED0ROOT_SERVICE_READY_FD,
                PACHA_LAUNCH_SIGNAL),
        };
        status = seed0root_exec_native_service(filed_endpoint_fd, "/srv/netd.elf",
            &netd_cfg, sizeof(netd_cfg), netd_grants, live_mode ? 4 : 3,
            0x5eed2003u);
        if (net_ready.b >= 16) (void)pacha_fd_close(net_ready.b);
        if (status == 0)
            status = seed0root_register_service_endpoint(filed_endpoint_fd,
                FILED_OP_SERVICE_SET_NETD_SOCKET, netd_endpoint, 0x5eed2004u);
        if (status != 0 && !live_mode) goto out;
        if (status != 0) {
            printf("[seed0root] optional live netd launch failed status=%d\n", status);
            if (net_ready.a >= 16) (void)pacha_fd_close(net_ready.a);
            out_live_usb_ready->net_state = 3;
            out_live_usb_ready->net_status = status;
            out_live_usb_ready->net_stage = 0;
            (void)pacha_fd_close(netd_endpoint);
            netd_endpoint = -1;
            status = 0;
        } else if (live_mode) {
            out_live_usb_ready->net_fd = net_ready.a;
            printf("[seed0root] live Ethernet service started independently of ash\n");
        }
        }
    }

#if defined(SEED0ROOT_LPR_THREAD_SIGNAL_TEST) && SEED0ROOT_LPR_THREAD_SIGNAL_TEST
    /* This LPR-only gate must remain runnable without a DRM core package. */
    goto out;
#endif

    int input_slots[FILED_EXEC_MAX_INHERIT_FDS];
    uint64_t input_count = 0;
    for (uint64_t i = 0; i < devices->metadata.device_count; i++) {
        if (devices->metadata.devices[i].vendor_id == VIRTIO_VENDOR &&
            devices->metadata.devices[i].device_id == INPUT_MODERN) {
            if (input_count == FILED_EXEC_MAX_INHERIT_FDS) {
                status = -7;
                goto out;
            }
            input_slots[input_count++] = (int)i;
        }
    }
    if (input_count + 5 > FILED_EXEC_MAX_INHERIT_FDS ||
        pacha_ipc_channel_create(&ready, seed0root_channel_rights, 0) != 0) {
        status = -7;
        goto out;
    }
    inputd_endpoint = pacha_ipc_endpoint_create(seed0root_channel_rights, 0);
    if (inputd_endpoint < 16) { status = -5; goto out; }
    input_source_endpoint = pacha_ipc_endpoint_create(seed0root_channel_rights, 0);
    if (input_source_endpoint < 16) { status = -5; goto out; }
    unsigned char input_blob[INPUTD_BOOT_CONFIG_MAX_BYTES];
    memset(input_blob, 0, sizeof(input_blob));
    struct inputd_boot_config *input_cfg = (struct inputd_boot_config *)input_blob;
    const uint64_t input_size = sizeof(*input_cfg) +
        input_count * sizeof(struct inputd_device_config);
    input_cfg->magic = INPUTD_BOOT_CONFIG_MAGIC;
    input_cfg->version = INPUTD_BOOT_CONFIG_VERSION;
    input_cfg->header_size = sizeof(*input_cfg);
    input_cfg->total_size = input_size;
    input_cfg->input_endpoint_fd = SEED0ROOT_SERVICE_ENDPOINT_FD;
    input_cfg->ready_channel_fd = SEED0ROOT_SERVICE_READY_FD;
    input_cfg->netd_endpoint_fd = live_mode ? 0 : SEED0ROOT_SERVICE_NETD_FD;
    input_cfg->source_endpoint_fd = SEED0ROOT_SERVICE_INPUT_SOURCE_FD;
    input_cfg->device_count = (uint32_t)input_count;
    input_cfg->device_record_size = sizeof(struct inputd_device_config);
    input_cfg->devices_offset = sizeof(*input_cfg);
    struct inputd_device_config *records =
        (struct inputd_device_config *)(input_blob + input_cfg->devices_offset);
    struct pacha_process_fd_grant input_grants[FILED_EXEC_MAX_INHERIT_FDS];
    for (uint64_t i = 0; i < input_count; i++) {
        const int slot = input_slots[i];
        const struct pacha_root_device_record *src = &devices->metadata.devices[slot];
        records[i] = (struct inputd_device_config) {
            .device_fd = SEED0ROOT_SERVICE_DEVICE_FD + i,
            .resource_id = src->resource_id,
            .pci_segment = src->pci_segment, .pci_bus = src->pci_bus,
            .pci_device = src->pci_device, .pci_function = src->pci_function,
            .vendor_id = (uint32_t)src->vendor_id,
            .device_id = (uint32_t)src->device_id,
            .subsystem_id = (uint32_t)src->subsystem_id,
        };
        input_grants[i] = (struct pacha_process_fd_grant)PACHA_LAUNCH_GRANT(
            devices->fds[slot], SEED0ROOT_SERVICE_DEVICE_FD + i, PACHA_LAUNCH_DEVICE_DRIVER);
    }
    uint64_t input_grant_count = input_count;
    input_grants[input_grant_count++] = (struct pacha_process_fd_grant)PACHA_LAUNCH_GRANT(
        inputd_endpoint, SEED0ROOT_SERVICE_ENDPOINT_FD, PACHA_LAUNCH_SERVER);
    input_grants[input_grant_count++] = (struct pacha_process_fd_grant)PACHA_LAUNCH_GRANT(
        ready.b, SEED0ROOT_SERVICE_READY_FD, PACHA_LAUNCH_SIGNAL);
    if (!live_mode)
        input_grants[input_grant_count++] = (struct pacha_process_fd_grant)PACHA_LAUNCH_GRANT(
            netd_endpoint, SEED0ROOT_SERVICE_NETD_FD, PACHA_LAUNCH_CLIENT);
    input_grants[input_grant_count++] = (struct pacha_process_fd_grant)PACHA_LAUNCH_GRANT(
        filed_endpoint_fd, 240, PACHA_LAUNCH_CLIENT);
    input_grants[input_grant_count++] = (struct pacha_process_fd_grant)PACHA_LAUNCH_GRANT(
        input_source_endpoint, SEED0ROOT_SERVICE_INPUT_SOURCE_FD, PACHA_LAUNCH_SERVER);
    status = seed0root_exec_native_service(filed_endpoint_fd, "/srv/inputd.elf",
        input_blob, input_size, input_grants, input_grant_count,
        0x5eed2007u);
    (void)pacha_fd_close(ready.b); ready.b = -1;
    if (status != 0) goto out;
    status = seed0root_register_service_endpoint(filed_endpoint_fd,
        FILED_OP_SERVICE_SET_INPUTD_INPUT, inputd_endpoint, 0x5eed2008u);
    if (status != 0) goto out;
    { const int ready_fd = ready.a; ready.a = -1;
      status = seed0root_wait_service_ready(ready_fd, INPUTD_BOOT_READY_MAGIC, "inputd"); }
    if (status != 0) goto out;
    /* Inputd owns the evdev names before a USB controller can publish input.
     * Neither its startup nor USB enumeration depends on DRM readiness. */
    for (uint64_t xhci = 0; xhci < devices->metadata.device_count; xhci++) {
        if (devices->metadata.devices[xhci].class_code != 0x0c0330)
            continue;
        if (pacha_ipc_channel_create(&ready, seed0root_channel_rights, 0) != 0) {
            status = -5;
            goto out;
        }
        const struct usbd_boot_config usbd_cfg = {
            .magic = USBD_BOOT_CONFIG_MAGIC,
            .version = USBD_BOOT_CONFIG_VERSION,
            .device_fd = SEED0ROOT_SERVICE_DEVICE_FD,
            .filed_endpoint_fd = 240,
            .ready_channel_fd = SEED0ROOT_SERVICE_READY_FD,
            .input_source_endpoint_fd = SEED0ROOT_SERVICE_INPUT_SOURCE_FD,
            .resource_id = devices->metadata.devices[xhci].resource_id,
            .pci_segment = devices->metadata.devices[xhci].pci_segment,
            .pci_bus = devices->metadata.devices[xhci].pci_bus,
            .pci_device = devices->metadata.devices[xhci].pci_device,
            .pci_function = devices->metadata.devices[xhci].pci_function,
        };
        const struct pacha_process_fd_grant usbd_grants[] = {
            PACHA_LAUNCH_GRANT(devices->fds[xhci], SEED0ROOT_SERVICE_DEVICE_FD,
                PACHA_LAUNCH_DEVICE_DRIVER | PACHA_FD_RIGHT_TRANSFER),
            PACHA_LAUNCH_GRANT(ready.b, SEED0ROOT_SERVICE_READY_FD, PACHA_LAUNCH_SIGNAL),
            PACHA_LAUNCH_GRANT(filed_endpoint_fd, 240, PACHA_LAUNCH_CLIENT),
            PACHA_LAUNCH_GRANT(input_source_endpoint, SEED0ROOT_SERVICE_INPUT_SOURCE_FD,
                PACHA_LAUNCH_CLIENT),
        };
        status = seed0root_exec_native_service(filed_endpoint_fd, "/srv/usbd.elf",
            &usbd_cfg, sizeof(usbd_cfg), usbd_grants, 4, 0x5eed3000u + xhci);
        (void)pacha_fd_close(ready.b); ready.b = -1;
        if (status != 0) goto out;
        if (live_mode) {
            if (out_live_usb_ready->count == out_live_usb_ready->capacity) {
                status = -7;
                goto out;
            }
            const size_t slot = out_live_usb_ready->count++;
            out_live_usb_ready->fds[slot] = ready.a;
            ready.a = -1;
            // USB enumeration may involve real hubs and composite devices.
            // Its ready signal is still checked, but must not gate GOP/ash.
            printf("[seed0root] usbd slot %zu PCI %u:%u.%u started; waiting asynchronously\n",
                slot, devices->metadata.devices[xhci].pci_bus,
                devices->metadata.devices[xhci].pci_device,
                devices->metadata.devices[xhci].pci_function);
            fflush(stdout);
        } else {
            const int ready_fd = ready.a; ready.a = -1;
            status = seed0root_wait_service_ready(ready_fd, USBD_BOOT_READY_MAGIC, "usbd");
            if (status != 0) goto out;
        }
    }
#if defined(SEED0ROOT_USB_HID_INPUT_TEST) && SEED0ROOT_USB_HID_INPUT_TEST
    extern int seed0root_usb_input_smoke(int inputd_endpoint);
    status = seed0root_usb_input_smoke(inputd_endpoint);
    if (status != 0) goto out;
#endif
    if (live_mode) {
        *out_live_termd_fd = termd_endpoint;
        *out_live_inputd_fd = inputd_endpoint;
        termd_endpoint = -1;
        inputd_endpoint = -1;
        printf("[seed0root] live services ready: termd, inputd; usbd/netd independent; no gpud\n");
        fflush(stdout);
        goto out;
    }
    /* Input discovery must not be gated on DRM. A USB-only boot has no
     * legacy virtio-input device at startup, so inputd starts empty. */
    const int gpu = seed0root_find_root_device(devices, VIRTIO_VENDOR,
        GPU_LEGACY, GPU_MODERN);
    if (gpu < 0 || pacha_ipc_channel_create(&ready, seed0root_channel_rights, 0) != 0 ||
        pacha_ipc_channel_create(&gpud_control, seed0root_channel_rights, 0) != 0) {
        status = gpu < 0 ? -19 : -5;
        goto out;
    }
    gpud_drm_endpoint = pacha_ipc_endpoint_create(seed0root_channel_rights, 0);
    if (gpud_drm_endpoint < 16) { status = -5; goto out; }
    struct gpud_boot_config gpud_cfg;
    memset(&gpud_cfg, 0, sizeof(gpud_cfg));
    gpud_cfg.magic = GPUD_BOOT_CONFIG_MAGIC;
    gpud_cfg.version = GPUD_BOOT_CONFIG_VERSION;
    gpud_cfg.drm_endpoint_fd = SEED0ROOT_SERVICE_ENDPOINT_FD;
    gpud_cfg.device_fd = SEED0ROOT_SERVICE_DEVICE_FD;
    gpud_cfg.filed_endpoint_fd = 240;
    gpud_cfg.control_channel_fd = SEED0ROOT_SERVICE_NETD_FD;
    gpud_cfg.ready_channel_fd = SEED0ROOT_SERVICE_READY_FD;
    const struct pacha_process_fd_grant gpud_grants[] = {
        PACHA_LAUNCH_GRANT(devices->fds[gpu], SEED0ROOT_SERVICE_DEVICE_FD,
            PACHA_LAUNCH_DEVICE_DRIVER | PACHA_FD_RIGHT_TRANSFER),
        PACHA_LAUNCH_GRANT(gpud_drm_endpoint, SEED0ROOT_SERVICE_ENDPOINT_FD, PACHA_LAUNCH_SERVER),
        PACHA_LAUNCH_GRANT(ready.b, SEED0ROOT_SERVICE_READY_FD, PACHA_LAUNCH_SIGNAL),
        PACHA_LAUNCH_GRANT(gpud_control.b, SEED0ROOT_SERVICE_NETD_FD,
            PACHA_LAUNCH_SERVER | PACHA_FD_RIGHT_SEND),
        PACHA_LAUNCH_GRANT(filed_endpoint_fd, 240, PACHA_LAUNCH_CLIENT),
    };
    status = seed0root_exec_native_service(filed_endpoint_fd, "/srv/gpud.elf",
        &gpud_cfg, sizeof(gpud_cfg), gpud_grants, 5, 0x5eed2005u);
    (void)pacha_fd_close(ready.b); ready.b = -1;
    (void)pacha_fd_close(gpud_control.b); gpud_control.b = -1;
    if (status != 0) goto out;
    status = seed0root_register_service_endpoint(filed_endpoint_fd,
        FILED_OP_SERVICE_SET_GPUD_DRM, gpud_drm_endpoint, 0x5eed2006u);
    if (status != 0) goto out;
    { const int ready_fd = ready.a; ready.a = -1;
      status = seed0root_wait_service_ready(ready_fd, GPUD_BOOT_READY_MAGIC, "gpud"); }
    if (status != 0) goto out;
    printf("[seed0root] rootfs services ready termd -> netd -> usbd -> inputd -> gpud\n");
    fflush(stdout);
#if defined(SEED0ROOT_LAUNCH_GRANT_TEST) && SEED0ROOT_LAUNCH_GRANT_TEST
    {
        const char *grant_argv[] = { "/cmd/process_create_grants.elf" };
        status = seed0root_run_exec_path_smoke(filed_endpoint_fd, grant_argv[0],
            grant_argv, 1, NULL, "process-create grant transaction", 0);
        if (status != 0) goto out;
    }
#endif
#if defined(SEED0ROOT_NATIVE_PRIMITIVES_TEST) && SEED0ROOT_NATIVE_PRIMITIVES_TEST
    {
        const char *argv[] = { "/cmd/native_host_primitives.elf" };
        status = seed0root_run_exec_path_smoke(filed_endpoint_fd, argv[0],
            argv, 1, NULL, "native host primitives", 0);
        if (status != 0) goto out;
        const int rng = seed0root_find_root_device(devices, VIRTIO_VENDOR,
            0x1044, 0x1005);
        if (rng < 0) { status = -19; goto out; }
        const uint64_t bootstrap = 0;
        const struct pacha_process_fd_grant device_grant = PACHA_LAUNCH_GRANT(
            devices->fds[rng], 224, PACHA_LAUNCH_DEVICE_DRIVER);
        status = seed0root_exec_native_service(filed_endpoint_fd,
            "/cmd/native_device_contract.elf", &bootstrap, sizeof(bootstrap),
            &device_grant, 1, 0x5eed2010u);
        if (status != 0) goto out;
    }
#endif
#if defined(SEED0ROOT_UNIXD_CONTRACT_TEST) && SEED0ROOT_UNIXD_CONTRACT_TEST
    {
        const char *native_argv[] = { "/cmd/unix_native_contract.elf" };
        seed0root_test_native_status = seed0root_run_exec_path_smoke(filed_endpoint_fd, native_argv[0],
            native_argv, 1, "UNIXD_NATIVE_TEST=1", "unix native contract", 0);
        const int netd_process = seed0root_test_netd_process;
        struct pacha_fd_info info = {0};
        status = netd_process < 16 ? -9 : (int)pacha_syscall2(
            PACHA_PROCESS_SYSCALL_STOP, (uint64_t)netd_process, 19);
        if (!status && (pacha_fd_get_info(netd_process, &info) != 0 || info.extra != 4)) status = -5;
        printf("UNIXD_NETD_STOPPED status=%d state=%llu\n", status, (unsigned long long)info.extra);
        fflush(stdout);
    }
#endif

    *out_gpud_control_fd = gpud_control.a;
    gpud_control.a = -1;

out:
    if (ready.a >= 16) (void)pacha_fd_close(ready.a);
    if (ready.b >= 16) (void)pacha_fd_close(ready.b);
    if (gpud_control.a >= 16) (void)pacha_fd_close(gpud_control.a);
    if (gpud_control.b >= 16) (void)pacha_fd_close(gpud_control.b);
    if (inputd_endpoint >= 16) (void)pacha_fd_close(inputd_endpoint);
    if (input_source_endpoint >= 16) (void)pacha_fd_close(input_source_endpoint);
    if (gpud_drm_endpoint >= 16) (void)pacha_fd_close(gpud_drm_endpoint);
    if (netd_endpoint >= 16) (void)pacha_fd_close(netd_endpoint);
    if (termd_endpoint >= 16) (void)pacha_fd_close(termd_endpoint);
    if (!live_mode || status != 0) seed0root_close_root_devices(devices);
    return status;
}

static int seed0root_send_storage_ready(int ready_channel_fd)
{
    if (ready_channel_fd < 16) {
        return -1;
    }
    struct pacha_ipc_msg msg = {
        .word0 = PACHA_ROOT_READY_MAGIC,
        .word1 = 0,
        .word2 = 0,
        .word3 = 0,
        .fds = NULL,
        .fd_count = 0,
    };
    return pacha_ipc_send(ready_channel_fd, &msg);
}

static int seed0root_start_lpr_supervisor(int filed_endpoint_fd, int unix_admin_fd,
    int power_fd, int *out_endpoint_fd)
{
    if (filed_endpoint_fd < 16 || unix_admin_fd < 16 || out_endpoint_fd == NULL) {
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
    cfg.unix_admin_fd = LPRS_UNIX_ADMIN_FD;
    cfg.filed_admin_fd = LPRS_FILED_ADMIN_FD;
    cfg.power_fd = power_fd >= 16 ? LPRS_POWER_FD : 0;
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
    exec->inherit_fd_count = 3;
    exec->fd_grants[0] = (filed_exec_fd_grant_t){
        .target = LPR_SUPERVISOR_ENDPOINT_FD, .rights = PACHA_LAUNCH_SERVER };
    exec->fd_grants[1] = (filed_exec_fd_grant_t){
        .target = LPRS_BOOT_CONFIG_FD, .rights = PACHA_LAUNCH_BLOB };
    exec->fd_grants[2] = (filed_exec_fd_grant_t){
        .target = LPRS_UNIX_ADMIN_FD, .rights = PACHA_LAUNCH_CLIENT,
        .flags = PACHA_FD_FLAG_PRIVATE };
    exec->inherit_fd_count = 4;
    exec->fd_grants[3] = (filed_exec_fd_grant_t){
        .target = LPRS_FILED_ADMIN_FD, .rights = PACHA_LAUNCH_CLIENT,
        .flags = PACHA_FD_FLAG_PRIVATE };
    if (power_fd >= 16) {
        exec->inherit_fd_count = 5;
        exec->fd_grants[4] = (filed_exec_fd_grant_t){
            .target = LPRS_POWER_FD, .rights = PACHA_LAUNCH_CLIENT,
            .flags = PACHA_FD_FLAG_PRIVATE };
    }
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

    struct pacha_ipc_fd fds[6];
    memset(fds, 0, sizeof(fds));
    fds[0].fd = (uint64_t)(uint32_t)page_fd;
    fds[0].rights =
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    fds[1].fd = (uint64_t)(uint32_t)endpoint_fd;
    fds[1].rights = exec->fd_grants[0].rights | PACHA_FD_RIGHT_TRANSFER;
    fds[2].fd = (uint64_t)(uint32_t)bootstrap_fd;
    fds[2].rights = exec->fd_grants[1].rights | PACHA_FD_RIGHT_TRANSFER;
    fds[3].fd = (uint64_t)(uint32_t)unix_admin_fd;
    fds[3].rights = exec->fd_grants[2].rights | PACHA_FD_RIGHT_TRANSFER;
    fds[4].fd = (uint64_t)(uint32_t)filed_endpoint_fd;
    fds[4].rights = exec->fd_grants[3].rights | PACHA_FD_RIGHT_TRANSFER;
    if (power_fd >= 16) {
        fds[5].fd = (uint64_t)(uint32_t)power_fd;
        fds[5].rights = exec->fd_grants[4].rights | PACHA_FD_RIGHT_TRANSFER;
    }

    struct pacha_ipc_fd reply_fds[2];
    memset(reply_fds, 0, sizeof(reply_fds));
    struct pacha_ipc_msg reply;
    status = seed0root_filed_page_call_fdv(
        filed_endpoint_fd,
        FILED_OP_EXEC_PATH,
        0x5eed1001u,
        fds,
        power_fd >= 16 ? 6 : 5,
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
        PACHA_LAUNCH_CLIENT | PACHA_FD_RIGHT_TRANSFER,
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

/* Account and capabilities are a single trusted launch specification. */
#include "linux_services.h"

static int seed0root_register_lpr_service(
    int supervisor_endpoint_fd,
    const seed0root_linux_service_t *service,
    lprs_process_state_t *out_state,
    int *out_bootstrap)
{
    if (supervisor_endpoint_fd < 16 || out_state == NULL || !service ||
        !service->account || !service->account[0] || strlen(service->account) >= 64 ||
        !service->argc || service->argc > 8 || !service->argv[0]) {
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
    snprintf(reg->account, sizeof(reg->account), "%s", service->account);
    reg->filed_rights = service->filed_rights;
    reg->credential_rights = service->credential_rights;
    snprintf(reg->state.ctty, sizeof(reg->state.ctty), "%s", service->ctty ? service->ctty : "");
    snprintf(reg->state.cwd, sizeof(reg->state.cwd), "%s", "/");
    struct pacha_ipc_msg reply;
    status = seed0root_lprs_call_cap(
        supervisor_endpoint_fd,
        LPRS_OP_PROCESS_REGISTER_EXEC,
        0x5eed1003u,
        page_fd,
        page,
        sizeof(*reg),
        -1,
        &reply, out_bootstrap);
    if (status == 0) {
        memcpy(out_state, &reg->state, sizeof(*out_state));
    }
    seed0root_destroy_filed_page(page_fd, page);
    return status;
}

static void seed0root_cancel_lpr_exec(
    int supervisor_endpoint_fd,
    uint64_t token)
{
    int page_fd = -1;
    void *page = NULL;
    if (supervisor_endpoint_fd < 16 || token == 0 ||
        seed0root_create_filed_page(&page_fd, &page) != 0)
    {
        return;
    }
    lprs_token_request_t *request =
        (lprs_token_request_t *)((uint8_t *)page + PACHA_SERVICE_HEADER_BYTES);
    memset(request, 0, sizeof(*request));
    request->token = token;
    struct pacha_ipc_msg reply;
    (void)seed0root_lprs_call(
        supervisor_endpoint_fd,
        LPRS_OP_PROCESS_EXEC_COMMIT_CANCEL,
        0x5eed1006u,
        page_fd,
        page,
        sizeof(*request),
        -1,
        &reply);
    seed0root_destroy_filed_page(page_fd, page);
}

static int seed0root_spawn_registered_lpr_service(
    int filed_endpoint_fd,
    int supervisor_endpoint_fd,
    const seed0root_linux_service_t *service,
    const lprs_process_state_t *registered,
    int bootstrap_fd)
{
    const char *const *argv = service->argv;
    if (filed_endpoint_fd < 16 || supervisor_endpoint_fd < 16) {
        return -22;
    }
    const lprs_process_state_t init_state = *registered;
    int status;

    lpr_manifest_layout_t manifest_layout;
    memset(&manifest_layout, 0, sizeof(manifest_layout));
    status = lpr_manifest_layout(0, 0, 0, 0, &manifest_layout);
    if (status != 0) {
        seed0root_cancel_lpr_exec(
            supervisor_endpoint_fd, init_state.token);
        return -22;
    }
    uint64_t manifest_map_bytes = 0;
    if (align_up(manifest_layout.byte_size, &manifest_map_bytes) != 0) {
        seed0root_cancel_lpr_exec(
            supervisor_endpoint_fd, init_state.token);
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
        seed0root_cancel_lpr_exec(
            supervisor_endpoint_fd, init_state.token);
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
        seed0root_cancel_lpr_exec(
            supervisor_endpoint_fd, init_state.token);
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
        seed0root_cancel_lpr_exec(
            supervisor_endpoint_fd, init_state.token);
        return -22;
    }
    manifest->transaction_id = init_state.token;
    manifest->generation = init_state.generation;
    manifest->flags = LPR_MANIFEST_FLAG_DEFAULT_STDIO | LPR_MANIFEST_FLAG_SUPERVISOR;
    manifest->linux_pid = init_state.pid;
    manifest->linux_ppid = init_state.ppid;
    manifest->linux_sid = init_state.sid;
    manifest->linux_pgrp = init_state.pgrp;
    manifest->linux_next_pid = 0;
    manifest->cwd_handle = init_state.cwd_handle;
    manifest->supervisor_token = init_state.token;
    manifest->supervisor_bootstrap_fd = LPR_SUPERVISOR_ENDPOINT_FD;
    manifest->owner_generation = init_state.generation;
    snprintf(manifest->ctty, sizeof(manifest->ctty), "%s", init_state.ctty);
    snprintf(manifest->cwd, sizeof(manifest->cwd), "%s", init_state.cwd);
    if (lpr_manifest_seal(manifest, manifest_map_bytes) != 0) {
        seed0root_destroy_wire_page(manifest_map_bytes, manifest_fd, manifest);
        seed0root_cancel_lpr_exec(
            supervisor_endpoint_fd, init_state.token);
        return -22;
    }

    int page_fd = -1;
    void *page = NULL;
    status = seed0root_create_filed_page(&page_fd, &page);
    if (status != 0) {
        seed0root_destroy_wire_page(manifest_map_bytes, manifest_fd, manifest);
        seed0root_cancel_lpr_exec(
            supervisor_endpoint_fd, init_state.token);
        return status;
    }
    filed_exec_path_t *exec = (filed_exec_path_t *)page;
    exec->dir_handle = 0;
    exec->flags =
        FILED_EXEC_LINUX_LPR |
        FILED_EXEC_BOOTSTRAP_FD |
        FILED_EXEC_INHERIT_FDS |
        FILED_EXEC_TRANSFER_PROCESS_FD |
        FILED_EXEC_DEFER_START | service->clients;
    exec->inherit_fd_count = 1;
    exec->fd_grants[0] = (filed_exec_fd_grant_t){
        .target = LPR_SUPERVISOR_ENDPOINT_FD,
        .rights = PACHA_LAUNCH_CLIENT | PACHA_FD_RIGHT_DUP |
            PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SET_FLAGS };
    exec->argc = service->argc;
    exec->envc = 0;
    snprintf(exec->path, sizeof(exec->path), "%s", argv[0]);
    for (uint64_t i = 0; i < exec->argc; i++) {
        status = seed0root_exec_add_string(exec, &exec->argv[i], argv[i]);
        if (status != 0) {
            seed0root_destroy_filed_page(page_fd, page);
            seed0root_destroy_wire_page(manifest_map_bytes, manifest_fd, manifest);
            seed0root_cancel_lpr_exec(
                supervisor_endpoint_fd, init_state.token);
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
    fds[1].fd = (uint64_t)(uint32_t)bootstrap_fd;
    fds[1].rights = exec->fd_grants[0].rights | PACHA_FD_RIGHT_TRANSFER;
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
        fprintf(stderr, "[seed0root] Linux service exec failed status=%d\n", status);
        seed0root_cancel_lpr_exec(
            supervisor_endpoint_fd, init_state.token);
        return status;
    }
    if (reply.fd_count < 2 || reply_fds[0].fd < 16 || reply_fds[1].fd < 16) {
        if (reply_fds[1].fd >= 16) {
            (void)pacha_fd_close((int)reply_fds[1].fd);
        }
        if (reply_fds[0].fd >= 16) {
            (void)pacha_syscall2(
                PACHA_PROCESS_SYSCALL_KILL, reply_fds[0].fd, 1);
            (void)pacha_fd_close((int)reply_fds[0].fd);
        }
        seed0root_cancel_lpr_exec(
            supervisor_endpoint_fd, init_state.token);
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
        seed0root_cancel_lpr_exec(
            supervisor_endpoint_fd, init_state.token);
        return status;
    }
    lprs_token_request_t *token_req =
        (lprs_token_request_t *)((uint8_t *)lprs_page + PACHA_SERVICE_HEADER_BYTES);
    memset(token_req, 0, sizeof(*token_req));
    token_req->token = init_state.token;
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
            "[seed0root] Linux service transaction begin failed status=%d\n",
            status);
        seed0root_cancel_lpr_exec(
            supervisor_endpoint_fd, init_state.token);
        return status;
    }
    const int start_status = pacha_thread_start((int)reply_fds[1].fd);
    (void)pacha_fd_close((int)reply_fds[1].fd);
    if (start_status != 0) {
        seed0root_cancel_lpr_exec(
            supervisor_endpoint_fd, init_state.token);
        (void)pacha_syscall2(PACHA_PROCESS_SYSCALL_KILL, reply_fds[0].fd, 1);
        (void)pacha_fd_close((int)reply_fds[0].fd);
        fprintf(stderr,
            "[seed0root] Linux service thread start failed status=%d\n",
            start_status);
        return start_status;
    }
    (void)pacha_fd_close((int)reply_fds[0].fd);
    return 0;
}

static int seed0root_spawn_lpr_services(int filed_endpoint_fd, int supervisor_endpoint_fd)
{
    for (unsigned i = 0; i < sizeof(seed0root_linux_services) / sizeof(seed0root_linux_services[0]); i++) {
        const seed0root_linux_service_t *service = &seed0root_linux_services[i];
        lprs_process_state_t state = {0};
        int bootstrap_fd = -1;
        int status = seed0root_register_lpr_service(supervisor_endpoint_fd, service, &state, &bootstrap_fd);
        if (status == 0 && (!state.token || !state.generation || !state.pid || bootstrap_fd < 16)) status = -5;
        if (status == 0) status = seed0root_spawn_registered_lpr_service(
            filed_endpoint_fd, supervisor_endpoint_fd, service, &state, bootstrap_fd);
        else if (state.token) seed0root_cancel_lpr_exec(supervisor_endpoint_fd, state.token);
        if (bootstrap_fd >= 16) (void)pacha_fd_close(bootstrap_fd);
        if (status) return status;
    }
    return 0;
}

struct seed0root_live_ssh {
    struct seed0root_live_usb_ready *devices;
    int filed_fd;
    int supervisor_fd;
    int enabled;
    int launched;
};

static void seed0root_poll_live_ssh(void *context)
{
    struct seed0root_live_ssh *ssh = context;
    seed0root_poll_live_usb_ready(ssh->devices);
    if (!ssh->enabled || ssh->launched ||
        ssh->devices->net_state != 2 || !ssh->devices->net_ipv4_ready)
        return;

    ssh->launched = 1;
    const seed0root_linux_service_t service = {
        .account = "root",
        .filed_rights = SEED0ROOT_FILE_NAMESPACE,
        /* Dropbear performs setgid/initgroups/setuid after authentication.
         * Keep this identity authority on the daemon, never on live ash. */
        .credential_rights = LPRS_CREDENTIAL_SETUID | LPRS_CREDENTIAL_SETGID,
        .clients = FILED_EXEC_SERVICE_NETD | FILED_EXEC_SERVICE_TERMD,
        .argc = 2,
        .argv = {"/usr/sbin/dropbear", "-FsjkmR"},
    };
    lprs_process_state_t state = {0};
    int bootstrap_fd = -1;
    int status = seed0root_register_lpr_service(ssh->supervisor_fd,
        &service, &state, &bootstrap_fd);
    if (status == 0) status = seed0root_spawn_registered_lpr_service(
        ssh->filed_fd, ssh->supervisor_fd, &service, &state, bootstrap_fd);
    else if (state.token) seed0root_cancel_lpr_exec(ssh->supervisor_fd, state.token);
    if (bootstrap_fd >= 16) (void)pacha_fd_close(bootstrap_fd);
    printf("[seed0root] live SSH %s status=%d (public key only)\n",
        status == 0 ? "launched" : "launch failed", status);
    fflush(stdout);
}

static int launch_filed_with_path(const storage_seed0root_bootstrap_t *bootstrap,
    const struct pacha_ipc_channel_pair *unix_path)
{
    if (bootstrap->magic != STORAGE_SEED0ROOT_BOOTSTRAP_MAGIC ||
        bootstrap->filed_image_fd < 16 ||
        bootstrap->filed_image_size == 0 ||
        bootstrap->device_fd < 16 ||
        bootstrap->ready_channel_fd < 16 ||
        bootstrap->service_ready_channel_fd < 16 ||
        bootstrap->module_count == 0 ||
        !storage_module_table_matches_manifest(
            bootstrap->modules, bootstrap->module_count)) {
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
        pacha_ipc_endpoint_create(seed0root_channel_rights, 0);
    if (filed_endpoint_fd < 16) {
        fprintf(stderr,
            "[seed0root] filed endpoint create failed status=%d\n",
            filed_endpoint_fd);
        (void)pacha_munmap(image, filed_map_size);
        return filed_endpoint_fd < 0 ? filed_endpoint_fd : -2;
    }
    const long filed_client_dup =
        pacha_fd_fcntl(filed_endpoint_fd, PACHA_FD_FCNTL_DUP, 16,
            PACHA_LAUNCH_CLIENT | PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SET_FLAGS);
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
    printf("[seed0root] filed device fd ready\n");
    fflush(stdout);
    storage_filed_bootstrap_t filed_bootstrap;
    int status = prepare_filed_storage_bootstrap(bootstrap, filed_endpoint_fd, &filed_bootstrap);
    if (status != 0) {
        (void)pacha_fd_close(filed_client_fd);
        (void)pacha_fd_close(filed_endpoint_fd);
        (void)pacha_munmap(image, filed_map_size);
        fprintf(stderr, "[seed0root] filed bootstrap package failed status=%d\n", status);
        return status;
    }
    printf("[seed0root] filed bootstrap ready\n");
    filed_bootstrap.unix_path_fd = (uint64_t)unix_path->a;
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
    struct pacha_process_fd_grant grants[6 + STORAGE_STACK_MODULE_CAPACITY] = {
        PACHA_LAUNCH_LOG_GRANTS(PACHA_FD_RIGHT_TRANSFER),
        PACHA_LAUNCH_GRANT(bootstrap->device_fd, bootstrap->device_fd, PACHA_LAUNCH_DEVICE_DRIVER),
        /* filed derives client-only handles for programs it launches. */
        PACHA_LAUNCH_GRANT(filed_endpoint_fd, filed_endpoint_fd,
            PACHA_LAUNCH_SERVER | PACHA_LAUNCH_CLIENT | PACHA_FD_RIGHT_DUP |
                PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_SET_FLAGS),
        { .source_fd = (uint64_t)unix_path->a, .target_fd = (uint64_t)unix_path->a,
          .rights = PACHA_LAUNCH_SERVER, .flags = PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC },
        PACHA_LAUNCH_GRANT(bootstrap_fd, bootstrap_fd, PACHA_LAUNCH_BLOB),
    };
    for (uint64_t i = 0; i < bootstrap->module_count; ++i) {
        grants[6 + i] = (struct pacha_process_fd_grant)PACHA_LAUNCH_GRANT(
            bootstrap->modules[i].image_fd, bootstrap->modules[i].image_fd, PACHA_LAUNCH_BLOB);
    }
    status = load_elf_process("/sbin/filed.elf", image, bootstrap->filed_image_size,
        grants, 6 + bootstrap->module_count, &loaded);
    (void)pacha_munmap(image, filed_map_size);
    (void)pacha_fd_close((int)bootstrap->filed_image_fd);
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
    status = seed0root_send_storage_ready((int)bootstrap->ready_channel_fd);
    (void)pacha_fd_close((int)bootstrap->ready_channel_fd);
    if (status != 0) {
        (void)pacha_fd_close(filed_client_fd);
        fprintf(stderr, "[seed0root] storage ready send failed status=%d fd=%llu\n",
            status,
            (unsigned long long)bootstrap->ready_channel_fd);
        return status;
    }
    printf("[seed0root] storage ready signal sent\n");
    fflush(stdout);
    /* Local IPC and its trusted authority do not depend on NIC/device
     * discovery. The admin capability is not registered as a filed service
     * endpoint and is never part of a Linux exec manifest. */
    int unix_admin_fd = -1;
    int lpr_supervisor_endpoint_fd = -1;
    int gpud_control_fd = -1;
    status = seed0root_start_unixd(filed_client_fd, unix_path->b, &unix_admin_fd);
    if (status == 0) status = seed0root_start_lpr_supervisor(
        filed_client_fd, unix_admin_fd, -1, &lpr_supervisor_endpoint_fd);
    if (unix_admin_fd >= 16) (void)pacha_fd_close(unix_admin_fd);
    if (status != 0) {
        (void)pacha_fd_close(filed_client_fd);
        fprintf(stderr, "[seed0root] local IPC bootstrap failed status=%d\n", status);
        return status;
    }
    struct seed0root_root_devices root_devices;
    status = seed0root_receive_root_handoff(
        (int)bootstrap->service_ready_channel_fd,
        &root_devices);
    if (status != 0) {
        (void)pacha_fd_close(lpr_supervisor_endpoint_fd);
        (void)pacha_fd_close(filed_client_fd);
        return status;
    }
    status = seed0root_launch_root_services(
        filed_client_fd, &root_devices, &gpud_control_fd, 0, NULL, NULL, NULL);
    if (status != 0) {
        (void)pacha_fd_close(lpr_supervisor_endpoint_fd);
        (void)pacha_fd_close(filed_client_fd);
        fprintf(stderr, "[seed0root] rootfs service launch failed status=%d\n", status);
        return status;
    }
    status = seed0root_run_storage_services(filed_client_fd);
    if (status != 0) {
        if (gpud_control_fd >= 16) (void)pacha_fd_close(gpud_control_fd);
        (void)pacha_fd_close(lpr_supervisor_endpoint_fd);
        (void)pacha_fd_close(filed_client_fd);
        fprintf(stderr, "[seed0root] filed service failed status=%d\n", status);
        return status;
    }
    status = seed0root_register_termd_signal_supervisor(
        filed_client_fd,
        lpr_supervisor_endpoint_fd);
    if (status != 0) {
        if (gpud_control_fd >= 16) (void)pacha_fd_close(gpud_control_fd);
        (void)pacha_fd_close(lpr_supervisor_endpoint_fd);
        (void)pacha_fd_close(filed_client_fd);
        fprintf(stderr,
            "[seed0root] termd signal supervisor register failed status=%d\n",
            status);
        return status;
    }
    status = seed0root_spawn_lpr_services(
        filed_client_fd,
        lpr_supervisor_endpoint_fd);
#if defined(SEED0ROOT_GPUD_RESTART_TEST) && SEED0ROOT_GPUD_RESTART_TEST
    if (status == 0)
        status = seed0root_test_gpud_restart(
            filed_client_fd, gpud_control_fd);
#endif
    if (gpud_control_fd >= 16) {
        (void)pacha_fd_close(gpud_control_fd);
        gpud_control_fd = -1;
    }
#if defined(SEED0ROOT_UNIXD_CONTRACT_TEST) && SEED0ROOT_UNIXD_CONTRACT_TEST
    {
        const uint64_t deadline = seed0root_now_ns() + 90000000000ull;
        const int timer = pacha_timerfd_create(100000000, 100000000,
            PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE, 0);
        if (!status && timer < 16) status = -5;
        int completed = 0;
        while (!status && seed0root_now_ns() < deadline) {
            char result[16] = {0};
            if (seed0root_read_filed_text(filed_client_fd,
                    "/tmp/unixd-stop-test.done", result, sizeof(result)) == 0 &&
                strchr(result, '\n') != NULL) {
                completed = 1;
                if (strcmp(result, "0\n") != 0) status = -5;
                break;
            }
            struct pacha_pollfd tick = { .fd = timer, .events = PACHA_FD_EVENT_READABLE };
            uint64_t expirations;
            if (pacha_fd_wait_many(&tick, 1, PACHA_FD_WAIT_FOREVER) != 1 ||
                pacha_fd_read(timer, &expirations, sizeof(expirations)) != sizeof(expirations)) {
                status = -5;
                break;
            }
        }
        if (timer >= 16) (void)pacha_fd_close(timer);
        if (!status && !completed) status = -110;
        struct pacha_fd_info info = {0};
        const int netd_process = seed0root_test_netd_process;
        const int inspected = pacha_fd_get_info(netd_process, &info);
        if (!status && (inspected != 0 || info.extra != 4)) status = -5;
        const int resumed = netd_process < 16 ? -9 : (int)pacha_syscall2(
            PACHA_PROCESS_SYSCALL_CONTINUE, (uint64_t)netd_process, 18);
        if (!status) status = resumed;
        printf("UNIXD_NETD_STOP_TEST status=%d stopped_state=%llu resumed=%d native_status=%d\n",
            status, (unsigned long long)info.extra, resumed, seed0root_test_native_status);
        fflush(stdout);
        if (!status) status = seed0root_test_native_status;
        if (netd_process >= 16) (void)pacha_fd_close(netd_process);
        seed0root_test_netd_process = -1;
    }
#endif
    (void)pacha_fd_close(lpr_supervisor_endpoint_fd);
    (void)pacha_fd_close(filed_client_fd);
    if (status != 0) {
        fprintf(stderr, "[seed0root] Linux init launch failed status=%d\n", status);
        return status;
    }
    printf("[seed0root] storage ready\n");
    return 0;
}

static int launch_filed(const storage_seed0root_bootstrap_t *bootstrap)
{
    struct pacha_ipc_channel_pair path = { .a = -1, .b = -1 };
    int status = pacha_ipc_channel_create(&path, seed0root_channel_rights, 0);
    if (status != 0) return status;
    status = launch_filed_with_path(bootstrap, &path);
    (void)pacha_fd_close(path.a);
    (void)pacha_fd_close(path.b);
    return status;
}

static int launch_live(const pacha_live_root_bootstrap_t *bootstrap)
{
    if (bootstrap == NULL ||
        bootstrap->magic != PACHA_LIVE_ROOT_BOOTSTRAP_MAGIC ||
        bootstrap->version != PACHA_LIVE_ROOT_BOOTSTRAP_VERSION ||
        bootstrap->filed_endpoint_fd < 16 || bootstrap->unix_path_fd < 16 ||
        bootstrap->root_handoff_fd < 16 || bootstrap->power_channel_fd < 16)
        return -22;
    const int filed_fd = (int)bootstrap->filed_endpoint_fd;
    int unix_admin_fd = -1;
    int supervisor_fd = -1;
    int termd_fd = -1;
    int inputd_fd = -1;
    int unused_gpud_fd = -1;
    struct live_console *console = NULL;
    struct seed0root_live_usb_ready usb_ready = {0};
    usb_ready.net_fd = -1;
    int status = seed0root_start_unixd(filed_fd,
        (int)bootstrap->unix_path_fd, &unix_admin_fd);
    if (status == 0) status = seed0root_start_lpr_supervisor(
        filed_fd, unix_admin_fd, (int)bootstrap->power_channel_fd, &supervisor_fd);
    (void)pacha_fd_close((int)bootstrap->power_channel_fd);
    if (unix_admin_fd >= 16) (void)pacha_fd_close(unix_admin_fd);
    if (status != 0) return status;
    struct seed0root_root_devices devices;
    status = seed0root_receive_root_handoff(
        (int)bootstrap->root_handoff_fd, &devices);
    if (status == 0) {
        printf("[seed0root] live framebuffer paddr=0x%llx size=%llu %llux%llu pitch=%llu\n",
            (unsigned long long)bootstrap->framebuffer_paddr,
            (unsigned long long)bootstrap->framebuffer_size,
            (unsigned long long)bootstrap->width,
            (unsigned long long)bootstrap->height,
            (unsigned long long)bootstrap->pitch);
        for (uint64_t i = 0; i < devices.metadata.device_count; i++) {
            const struct pacha_root_device_record *device = &devices.metadata.devices[i];
            if ((device->class_code >> 16) != 3) continue;
            for (unsigned bar = 0; bar < 6; bar++) {
                struct pacha_capsule_bar_info info;
                if (pacha_capsule_pci_bar_info(devices.fds[i], bar, &info) == 0 && info.size)
                    printf("[seed0root] display %04llx:%04llx BAR%u [%llx,%llx] flags=%llx\n",
                        (unsigned long long)device->vendor_id,
                        (unsigned long long)device->device_id, bar,
                        (unsigned long long)info.start,
                        (unsigned long long)info.end,
                        (unsigned long long)info.flags);
            }
        }
        fflush(stdout);
    }
    if (status == 0) {
        usb_ready.capacity = (size_t)devices.metadata.device_count;
        if (usb_ready.capacity != 0) {
            usb_ready.fds = calloc(usb_ready.capacity, sizeof(*usb_ready.fds));
            if (usb_ready.fds == NULL) status = -12;
        }
    }
    if (status == 0) status = seed0root_launch_root_services(
        filed_fd, &devices, &unused_gpud_fd, 1, &termd_fd, &inputd_fd,
        &usb_ready);
    if (status == 0) status = live_console_prepare(bootstrap,
        devices.metadata.devices, devices.fds, devices.metadata.device_count,
        termd_fd, inputd_fd, &console);
    seed0root_close_root_devices(&devices);
    if (status == 0) status = seed0root_register_termd_signal_supervisor(
        filed_fd, supervisor_fd);
    if (status == 0) {
        const seed0root_linux_service_t shell = {
            .account = "root",
            .filed_rights = SEED0ROOT_FILE_NAMESPACE,
            /* The shell can start before DHCP, but its descendants still
             * need the generic netd socket capability once it comes up. */
            .clients = FILED_EXEC_SERVICE_TERMD |
                (usb_ready.net_state == 1 ? FILED_EXEC_SERVICE_NETD : 0),
            .ctty = live_console_ctty(console),
            .argc = 1,
            .argv = {"/bin/ash"},
        };
        lprs_process_state_t state = {0};
        int shell_bootstrap_fd = -1;
        status = seed0root_register_lpr_service(supervisor_fd, &shell,
            &state, &shell_bootstrap_fd);
        if (status == 0) status = seed0root_spawn_registered_lpr_service(
            filed_fd, supervisor_fd, &shell, &state, shell_bootstrap_fd);
        else if (state.token) seed0root_cancel_lpr_exec(supervisor_fd, state.token);
        if (shell_bootstrap_fd >= 16) (void)pacha_fd_close(shell_bootstrap_fd);
    }
    if (status == 0) {
        char authorized_key[256];
        const int key_status = seed0root_read_filed_text(filed_fd,
            "/root/.ssh/authorized_keys", authorized_key,
            sizeof(authorized_key));
        struct seed0root_live_ssh ssh = {
            .devices = &usb_ready,
            .filed_fd = filed_fd,
            .supervisor_fd = supervisor_fd,
            .enabled = key_status == 0 &&
                strncmp(authorized_key, "ssh-ed25519 ", 12) == 0,
        };
        printf("[seed0root] live SSH %s\n",
            ssh.enabled ? "public key configured; waiting for netd" :
                "disabled (no Ed25519 authorized key)");
        fflush(stdout);
        /* Keep asynchronous netd independent of ash, but allow a bounded
         * interval for a useful initial boot report. Never inject later
         * status lines into an interactive TTY behind the shell's back. */
        if (usb_ready.net_state == 1 && usb_ready.net_fd >= 16) {
            struct pacha_pollfd event = {.fd = usb_ready.net_fd,
                .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
            /* FD wait deadlines are scheduler ticks (1 ms), not nanoseconds. */
            (void)pacha_fd_wait_many(&event, 1, 5000);
        }
        seed0root_poll_live_usb_ready(&usb_ready);
        /* Autonegotiation can finish immediately after probe. Sample one
         * further bounded link event before freezing the boot history. */
        if (usb_ready.net_state == 2 && !usb_ready.net_carrier &&
            usb_ready.net_fd >= 16) {
            struct pacha_pollfd event = {.fd = usb_ready.net_fd,
                .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP};
            (void)pacha_fd_wait_many(&event, 1, 2000);
            seed0root_poll_live_usb_ready(&usb_ready);
        }
        live_console_report_usb_boot(console, usb_ready.fds, usb_ready.count);
        live_console_report_net_boot(console, usb_ready.net_state,
            usb_ready.net_status, usb_ready.net_stage,
            usb_ready.net_carrier, usb_ready.net_mtu,
            usb_ready.net_nic_step, usb_ready.net_detail,
            usb_ready.net_loaded, usb_ready.net_pci_bound,
            usb_ready.net_source, usb_ready.net_line,
            usb_ready.net_fault_vector, usb_ready.net_fault_error_code,
            usb_ready.net_fault_core_relative, usb_ready.net_fault_ip,
            usb_ready.net_fault_address);
        printf("[seed0root] live RAM services connected; ash ctty=%s\n",
            live_console_ctty(console));
        fflush(stdout);
        live_console_finish_boot(console);
        status = live_console_run(console, seed0root_poll_live_ssh, &ssh);
    }
    seed0root_close_live_usb_ready(&usb_ready);
    live_console_destroy(console);
    if (termd_fd >= 16) (void)pacha_fd_close(termd_fd);
    if (inputd_fd >= 16) (void)pacha_fd_close(inputd_fd);
    if (supervisor_fd >= 16) (void)pacha_fd_close(supervisor_fd);
    return status;
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
    storage_seed0root_bootstrap_t bootstrap;
    bootstrap_status = read_bootstrap_fd(bootstrap_fd, &bootstrap, sizeof(bootstrap), "seed0root");
    (void)pacha_fd_fcntl(bootstrap_fd, PACHA_FD_FCNTL_SET_FLAGS,
        0, PACHA_FD_FLAG_INHERIT);
    (void)pacha_fd_close(bootstrap_fd);
    if (bootstrap_status == 0 &&
        bootstrap.magic == PACHA_LIVE_ROOT_BOOTSTRAP_MAGIC) {
        pacha_live_root_bootstrap_t live;
        memcpy(&live, &bootstrap, sizeof(live));
        const int live_status = launch_live(&live);
        if (live_status != 0)
            fprintf(stderr, "[seed0root] live launch failed status=%d\n",
                live_status);
        return live_status == 0 ? 0 : 4;
    }
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
        bootstrap.magic != STORAGE_SEED0ROOT_BOOTSTRAP_MAGIC ||
        bootstrap.device_fd < 16 ||
        bootstrap.ready_channel_fd < 16 ||
        bootstrap.service_ready_channel_fd < 16 ||
        bootstrap.filed_image_fd < 16 ||
        bootstrap.filed_image_size == 0 ||
        !storage_module_table_matches_manifest(
            bootstrap.modules, bootstrap.module_count))
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
