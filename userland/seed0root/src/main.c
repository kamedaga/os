#include "pacha/ipc.h"
#include "pacha/syscall.h"
#include "filed/ipc_protocol.h"
#include "koboxd/ipc_protocol.h"

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
    SEED0ROOT_BOOTSTRAP_MAX_MODULES = 8,
    SEED0ROOT_BOOTSTRAP_NAME_BYTES = 64,
    SEED0ROOT_KOBOXD_BOOTSTRAP_MAGIC = 0x3150474b42584f4bull,
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
};

struct seed0root_bootstrap_module {
    char name[SEED0ROOT_BOOTSTRAP_NAME_BYTES];
    uint64_t image_fd;
    uint64_t image_size;
};

struct seed0root_bootstrap {
    uint64_t magic;
    uint64_t device_fd;
    uint64_t koboxd_image_fd;
    uint64_t koboxd_image_size;
    uint64_t module_count;
    struct seed0root_bootstrap_module modules[SEED0ROOT_BOOTSTRAP_MAX_MODULES];
};

struct seed0root_koboxd_bootstrap {
    uint64_t magic;
    uint64_t device_fd;
    uint64_t control_fd;
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

static int create_inherited_vmo_from_bytes(const void *data, uint64_t size, const char *label)
{
    if (data == NULL || size == 0) {
        return -1;
    }
    const int trace = label != NULL && strcmp(label, "koboxd bootstrap fd") == 0;
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
        PACHA_FD_RIGHT_MAP_WRITE;
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
    for (unsigned i = 0; i < 262144; i++) {
        const int status = pacha_ipc_recv(fd, msg);
        if (status == 0) {
            return 0;
        }
        if (status != PACHA_ERR_EMPTY && status != PACHA_ERR_NOT_READY && status != -2) {
            return status;
        }
        struct pacha_pollfd pollfd = {
            .fd = fd,
            .events = PACHA_FD_EVENT_READABLE,
            .revents = 0,
        };
        (void)pacha_fd_wait_many(&pollfd, 1, 1);
    }
    return -2;
}

static int send_ipc_wait(int fd, const struct pacha_ipc_msg *msg)
{
    if (fd < 16 || msg == NULL) {
        return -1;
    }
    for (unsigned i = 0; i < 262144; i++) {
        const int status = pacha_ipc_send(fd, msg);
        if (status == 0) {
            return 0;
        }
        if (status != PACHA_ERR_EMPTY && status != PACHA_ERR_NOT_READY && status != -2) {
            return status;
        }
        struct pacha_pollfd pollfd = {
            .fd = fd,
            .events = PACHA_FD_EVENT_WRITABLE,
            .revents = 0,
        };
        (void)pacha_fd_wait_many(&pollfd, 1, 1);
    }
    return -2;
}

static int seed0root_get_koboxd_endpoint(int control_fd, uint64_t endpoint_kind, int *out_fd)
{
    if (control_fd < 16 || out_fd == NULL) {
        return -1;
    }
    *out_fd = -1;
    const struct pacha_ipc_msg request = {
        .word0 = KOBOXD_WIRE_CONTROL_MAGIC,
        .word1 = KOBOXD_WIRE_CONTROL_GET_ENDPOINT,
        .word2 = endpoint_kind,
        .word3 = KOBOXD_WIRE_VERSION,
    };
    int status = send_ipc_wait(control_fd, &request);
    if (status != 0) {
        fprintf(stderr,
            "[seed0root] koboxd control send failed kind=%llu status=%d\n",
            (unsigned long long)endpoint_kind,
            status);
        return status;
    }

    struct pacha_ipc_fd fds[1];
    struct pacha_ipc_msg reply;
    for (unsigned attempt = 0; attempt < 128; attempt++) {
        memset(fds, 0, sizeof(fds));
        memset(&reply, 0, sizeof(reply));
        reply.fds = fds;
        reply.fd_capacity = 1;
        status = recv_ipc_wait(control_fd, &reply);
        if (status != 0) {
            return status;
        }
        if (reply.word0 == KOBOXD_WIRE_REPLY_MAGIC &&
            reply.word1 == 0 &&
            reply.word2 == endpoint_kind &&
            reply.fd_count == 1 &&
            fds[0].fd >= 16)
        {
            break;
        }
        if (attempt == 127) {
            fprintf(stderr,
                "[seed0root] koboxd control reply invalid kind=%llu word0=0x%llx word1=%llu word2=%llu fd_count=%llu fd=%llu\n",
                (unsigned long long)endpoint_kind,
                (unsigned long long)reply.word0,
                (unsigned long long)reply.word1,
                (unsigned long long)reply.word2,
                (unsigned long long)reply.fd_count,
                (unsigned long long)fds[0].fd);
            return -2;
        }
    }
    *out_fd = (int)fds[0].fd;
    return 0;
}

static int seed0root_koboxd_endpoint_call_with_fd(
    int endpoint_fd,
    uint64_t op,
    uint64_t object_id,
    int transfer_fd,
    uint64_t *out_word2)
{
    if (endpoint_fd < 16 || out_word2 == NULL) {
        return -1;
    }
    struct pacha_ipc_fd fd_item;
    memset(&fd_item, 0, sizeof(fd_item));
    if (transfer_fd >= 16) {
        fd_item.fd = (uint64_t)(uint32_t)transfer_fd;
        fd_item.rights =
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE;
        fd_item.flags = 0;
        fd_item.transfer_flags = 0;
    }
    const struct pacha_ipc_msg request = {
        .word0 = KOBOXD_WIRE_ENDPOINT_MAGIC,
        .word1 = op,
        .word2 = object_id,
        .word3 = KOBOXD_WIRE_VERSION,
        .fds = transfer_fd >= 16 ? &fd_item : NULL,
        .fd_count = transfer_fd >= 16 ? 1 : 0,
    };
    int status = send_ipc_wait(endpoint_fd, &request);
    if (status != 0) {
        fprintf(stderr,
            "[seed0root] koboxd endpoint send failed fd=%d op=%llu status=%d transfer_fd=%d\n",
            endpoint_fd,
            (unsigned long long)op,
            status,
            transfer_fd);
        return status;
    }
    struct pacha_ipc_msg reply;
    for (unsigned attempt = 0; attempt < 128; attempt++) {
        memset(&reply, 0, sizeof(reply));
        status = recv_ipc_wait(endpoint_fd, &reply);
        if (status != 0) {
            return status;
        }
        if (reply.word0 == KOBOXD_WIRE_REPLY_MAGIC &&
            reply.word3 == KOBOXD_WIRE_VERSION)
        {
            break;
        }
        if (attempt == 127) {
            fprintf(stderr,
                "[seed0root] koboxd endpoint reply invalid fd=%d word0=0x%llx word1=%llu word2=%llu word3=%llu\n",
                endpoint_fd,
                (unsigned long long)reply.word0,
                (unsigned long long)reply.word1,
                (unsigned long long)reply.word2,
                (unsigned long long)reply.word3);
            return -2;
        }
    }
    if ((int64_t)reply.word1 < 0) {
        return (int)(int64_t)reply.word1;
    }
    *out_word2 = reply.word2;
    return 0;
}

static int seed0root_koboxd_endpoint_call(int endpoint_fd, uint64_t op, uint64_t *out_word2)
{
    return seed0root_koboxd_endpoint_call_with_fd(endpoint_fd, op, 0, -1, out_word2);
}

static int seed0root_fs_sync_all(int fs_fd)
{
    uint64_t ignored = 0;
    return seed0root_koboxd_endpoint_call(fs_fd, KOBOXD_WIRE_FS_SYNC_ALL, &ignored);
}

static int seed0root_filed_call(
    int endpoint_fd,
    uint64_t op,
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

    struct pacha_ipc_fd fd_item;
    memset(&fd_item, 0, sizeof(fd_item));
    if (transfer_fd >= 16) {
        fd_item.fd = (uint64_t)(uint32_t)transfer_fd;
        fd_item.rights =
            PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE;
        fd_item.flags = 0;
        fd_item.transfer_flags = 0;
    }

    const struct pacha_ipc_msg request = {
        .word0 = FILED_WIRE_REQUEST_MAGIC,
        .word1 = op,
        .word2 = word2,
        .word3 = request_id,
        .fds = transfer_fd >= 16 ? &fd_item : NULL,
        .fd_count = transfer_fd >= 16 ? 1 : 0,
    };
    const int reply_fd = pacha_ipc_call(endpoint_fd, &request);
    if (reply_fd < 16) {
        return reply_fd;
    }

    memset(out_reply, 0, sizeof(*out_reply));
    out_reply->fds = reply_fds;
    out_reply->fd_capacity = reply_fd_capacity;
    const int recv_status = recv_ipc_wait(reply_fd, out_reply);
    (void)pacha_fd_close(reply_fd);
    if (recv_status != 0) {
        return recv_status;
    }
    if (out_reply->word0 != FILED_WIRE_REPLY_MAGIC || out_reply->word3 != request_id) {
        return -2;
    }
    if ((int64_t)out_reply->word1 < 0) {
        return (int)(int64_t)out_reply->word1;
    }
    return 0;
}

static int seed0root_create_filed_wire_page(int *out_fd, void **out_mapped);
static void seed0root_destroy_filed_wire_page(int fd, void *mapped);

static int seed0root_dump_filed_metrics(int filed_endpoint_fd)
{
    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    return seed0root_filed_call(
        filed_endpoint_fd,
        FILED_WIRE_OP_DUMP_METRICS,
        0x5eed0f12u,
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
    const int status = seed0root_filed_call(
        filed_endpoint_fd,
        FILED_WIRE_OP_SET_CACHE_SLOTS,
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

    status = seed0root_create_filed_wire_page(&page_fd, &page);
    if (status != 0) {
        return status;
    }

    filed_wire_openat_t *openat = (filed_wire_openat_t *)page;
    openat->dir_handle = 0;
    openat->rights =
        FILED_WIRE_RIGHT_READ |
        FILED_WIRE_RIGHT_STAT |
        FILED_WIRE_RIGHT_EXEC;
    openat->open_flags = FILED_WIRE_OPEN_CLOEXEC;
    snprintf(openat->name, sizeof(openat->name), "%s", "/cmd/libc_vfs_exec_smoke.elf");

    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    status = seed0root_filed_call(
        filed_endpoint_fd,
        FILED_WIRE_OP_OPENAT,
        0x5eed0f13u,
        page_fd,
        0,
        &reply,
        NULL,
        0);
    seed0root_destroy_filed_wire_page(page_fd, page);
    page_fd = -1;
    page = NULL;
    if (status != 0) {
        fprintf(stderr, "[seed0root] filed no-cache open failed status=%d\n", status);
        (void)seed0root_set_filed_cache_slots(filed_endpoint_fd, 64);
        return status;
    }

    const uint64_t handle = reply.word2;
    status = seed0root_create_filed_wire_page(&page_fd, &page);
    if (status != 0) {
        (void)seed0root_filed_call(
            filed_endpoint_fd,
            FILED_WIRE_OP_CLOSE,
            0x5eed0f15u,
            -1,
            handle,
            &reply,
            NULL,
            0);
        (void)seed0root_set_filed_cache_slots(filed_endpoint_fd, 64);
        return status;
    }

    filed_wire_io_t *io = (filed_wire_io_t *)page;
    io->handle = handle;
    io->offset = 0;
    io->length = 4;
    memset(&reply, 0, sizeof(reply));
    status = seed0root_filed_call(
        filed_endpoint_fd,
        FILED_WIRE_OP_PREAD,
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
    seed0root_destroy_filed_wire_page(page_fd, page);

    memset(&reply, 0, sizeof(reply));
    const int close_status = seed0root_filed_call(
        filed_endpoint_fd,
        FILED_WIRE_OP_CLOSE,
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
    int status = seed0root_create_filed_wire_page(&page_fd, &page);
    if (status != 0) {
        return status;
    }

    filed_wire_openat_t *openat = (filed_wire_openat_t *)page;
    openat->dir_handle = 0;
    openat->rights = FILED_WIRE_RIGHT_READ | FILED_WIRE_RIGHT_STAT;
    openat->open_flags = FILED_WIRE_OPEN_CLOEXEC;
    snprintf(openat->name, sizeof(openat->name), "%s", path);

    struct pacha_ipc_msg reply;
    memset(&reply, 0, sizeof(reply));
    status = seed0root_filed_call(
        filed_endpoint_fd,
        FILED_WIRE_OP_OPENAT,
        0x5eed0f21u,
        page_fd,
        0,
        &reply,
        NULL,
        0);
    seed0root_destroy_filed_wire_page(page_fd, page);
    page_fd = -1;
    page = NULL;
    if (status != 0) {
        return status;
    }

    const uint64_t handle = reply.word2;
    status = seed0root_create_filed_wire_page(&page_fd, &page);
    if (status != 0) {
        (void)seed0root_filed_call(
            filed_endpoint_fd,
            FILED_WIRE_OP_CLOSE,
            0x5eed0f23u,
            -1,
            handle,
            &reply,
            NULL,
            0);
        return status;
    }

    filed_wire_io_t *io = (filed_wire_io_t *)page;
    io->handle = handle;
    io->offset = 0;
    io->length = out_capacity - 1;
    memset(&reply, 0, sizeof(reply));
    status = seed0root_filed_call(
        filed_endpoint_fd,
        FILED_WIRE_OP_PREAD,
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
    seed0root_destroy_filed_wire_page(page_fd, page);

    memset(&reply, 0, sizeof(reply));
    const int close_status = seed0root_filed_call(
        filed_endpoint_fd,
        FILED_WIRE_OP_CLOSE,
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
    return profile != NULL && token != NULL && strstr(profile, token) != NULL;
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
        (void)pacha_fd_wait_many(&pollfd, 1, 1);
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

static int seed0root_create_filed_wire_page(int *out_fd, void **out_mapped)
{
    return seed0root_create_wire_page(FILED_WIRE_PAGE_BYTES, out_fd, out_mapped);
}

static void seed0root_destroy_filed_wire_page(int fd, void *mapped)
{
    seed0root_destroy_wire_page(FILED_WIRE_PAGE_BYTES, fd, mapped);
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
    if (filed_endpoint_fd < 16 || path == NULL || label == NULL ||
        argc > FILED_WIRE_EXEC_MAX_ARGS)
    {
        return -1;
    }

    int page_fd = -1;
    void *page = NULL;
    int status = seed0root_create_filed_wire_page(&page_fd, &page);
    if (status != 0) {
        return status;
    }

    filed_wire_exec_path_t *exec = (filed_wire_exec_path_t *)page;
    exec->dir_handle = 0;
    exec->flags = exec_flags;
    exec->argc = argc == 0 ? 1 : argc;
    exec->envc = env != NULL ? 1 : 0;
    snprintf(exec->path, sizeof(exec->path), "%s", path);
    snprintf(exec->argv0, sizeof(exec->argv0), "%s", argc > 0 && argv != NULL ? argv[0] : path);
    for (uint64_t i = 0; i < exec->argc; i++) {
        const char *arg = (argc > 0 && argv != NULL && argv[i] != NULL) ? argv[i] : path;
        snprintf(exec->argv[i], sizeof(exec->argv[i]), "%s", arg);
    }
    if (env != NULL) {
        snprintf(exec->envp[0], sizeof(exec->envp[0]), "%s", env);
    }

    const uint64_t exec_start_ns = seed0root_now_ns();
    const uint64_t exec_start_cycles = seed0root_read_tsc();
    struct pacha_ipc_fd reply_fds[2];
    memset(reply_fds, 0, sizeof(reply_fds));
    struct pacha_ipc_msg reply;
    status = seed0root_filed_call(
        filed_endpoint_fd,
        FILED_WIRE_OP_EXEC_PATH,
        0x5eed0f11u,
        page_fd,
        0,
        &reply,
        reply_fds,
        2);
    const uint64_t exec_reply_ns = seed0root_now_ns();
    const uint64_t exec_reply_cycles = seed0root_read_tsc();
    seed0root_destroy_filed_wire_page(page_fd, page);
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
        FILED_WIRE_EXEC_LINUX_LPR);
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
        FILED_WIRE_EXEC_LINUX_LPR);
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
        FILED_WIRE_EXEC_LINUX_LPR);
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
        FILED_WIRE_EXEC_LINUX_LPR);
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
        FILED_WIRE_EXEC_LINUX_LPR);
}

static int seed0root_run_chibicc_cli_bench(int filed_endpoint_fd)
{
    const char *argv[] = {
        "/cmd/chibicc.elf",
        "-cc1",
        "-cc1-input",
        "/cmd/chibicc_workload.c",
        "-cc1-output",
        "/tmp/chibicc_workload.s",
        "/cmd/chibicc_workload.c",
    };
    return seed0root_run_exec_path_smoke(
        filed_endpoint_fd,
        "/cmd/chibicc.elf",
        argv,
        7,
        "PACHA_CHIBICC_CLI_BENCH=1",
        "chibicc cli bench",
        0);
}

static int seed0root_connect_storage_services(int control_fd)
{
    int block_fd = -1;
    int fs_fd = -1;
    int filed_endpoint_fd = -1;
    int status = seed0root_get_koboxd_endpoint(control_fd, KOBOXD_WIRE_ENDPOINT_BLOCK, &block_fd);
    if (status != 0) {
        return status;
    }
    uint64_t block_size = 0;
    status = seed0root_koboxd_endpoint_call(block_fd, KOBOXD_WIRE_BLOCK_IDENTIFY, &block_size);
    if (status != 0 || block_size != 512) {
        fprintf(stderr,
            "[seed0root] koboxd block identify failed status=%d block_size=%llu\n",
            status,
            (unsigned long long)block_size);
        return status != 0 ? status : -2;
    }
    status = seed0root_get_koboxd_endpoint(control_fd, KOBOXD_WIRE_ENDPOINT_FS_BACKEND, &fs_fd);
    if (status != 0) {
        return status;
    }
    status = seed0root_get_koboxd_endpoint(control_fd, KOBOXD_WIRE_ENDPOINT_FILED, &filed_endpoint_fd);
    if (status == 0 && filed_endpoint_fd >= 16) {
        printf("[seed0root] filed ready\n");
    }
    const unsigned boot_profile = (status == 0 && filed_endpoint_fd >= 16) ?
        seed0root_boot_profile_flags(filed_endpoint_fd) :
        SEED0ROOT_DEFAULT_BOOT_PROFILE;
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
    if ((boot_profile & (SEED0ROOT_BOOT_PROFILE_BENCH | SEED0ROOT_BOOT_PROFILE_LUA)) != 0 &&
        status == 0 &&
        filed_endpoint_fd >= 16) {
        status = seed0root_run_lua_cli_bench(filed_endpoint_fd);
    }
    if ((boot_profile & SEED0ROOT_BOOT_PROFILE_BENCH) != 0 && status == 0 && filed_endpoint_fd >= 16) {
        status = seed0root_run_chibicc_cli_bench(filed_endpoint_fd);
    }
    const unsigned metrics_profile =
        boot_profile & (SEED0ROOT_BOOT_PROFILE_FS_WRITE |
                        SEED0ROOT_BOOT_PROFILE_MEMORY |
                        SEED0ROOT_BOOT_PROFILE_BENCH |
                        SEED0ROOT_BOOT_PROFILE_LPR |
                        SEED0ROOT_BOOT_PROFILE_LUA |
                        SEED0ROOT_BOOT_PROFILE_DYN_NEEDED);
    const unsigned sync_profile =
        boot_profile & (SEED0ROOT_BOOT_PROFILE_FS_WRITE |
                        SEED0ROOT_BOOT_PROFILE_MEMORY |
                        SEED0ROOT_BOOT_PROFILE_BENCH |
                        SEED0ROOT_BOOT_PROFILE_LPR |
                        SEED0ROOT_BOOT_PROFILE_LUA |
                        SEED0ROOT_BOOT_PROFILE_DYN_NEEDED);
    if (metrics_profile != 0 && status == 0 && filed_endpoint_fd >= 16) {
        const int metrics_status = seed0root_dump_filed_metrics(filed_endpoint_fd);
        printf("[seed0root] filed metrics dump status=%d\n", metrics_status);
    }
    if (filed_endpoint_fd >= 16) {
        (void)pacha_fd_close(filed_endpoint_fd);
    }
    if (sync_profile != 0 && status == 0) {
        const uint64_t sync_start_ns = seed0root_now_ns();
        status = seed0root_fs_sync_all(fs_fd);
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

static int prepare_koboxd_bootstrap(
    const struct seed0root_bootstrap *bootstrap,
    int control_fd,
    struct seed0root_koboxd_bootstrap *out_bootstrap)
{
    if (bootstrap == NULL ||
        out_bootstrap == NULL ||
        control_fd < 16 ||
        bootstrap->module_count == 0 ||
        bootstrap->module_count > SEED0ROOT_BOOTSTRAP_MAX_MODULES ||
        bootstrap->modules[0].name[0] == '\0')
    {
        return -1;
    }

    memset(out_bootstrap, 0, sizeof(*out_bootstrap));
    out_bootstrap->magic = SEED0ROOT_KOBOXD_BOOTSTRAP_MAGIC;
    out_bootstrap->device_fd = bootstrap->device_fd;
    out_bootstrap->control_fd = (uint64_t)(uint32_t)control_fd;
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

static int launch_koboxd(const struct seed0root_bootstrap *bootstrap)
{
    if (bootstrap->magic != SEED0ROOT_BOOTSTRAP_MAGIC ||
        bootstrap->koboxd_image_fd < 16 ||
        bootstrap->koboxd_image_size == 0 ||
        bootstrap->device_fd < 16 ||
        bootstrap->module_count == 0 ||
        bootstrap->module_count > SEED0ROOT_BOOTSTRAP_MAX_MODULES) {
        fprintf(stderr,
            "[seed0root] bootstrap unavailable magic=0x%llx device_fd=%llu koboxd_fd=%llu size=%llu modules=%llu\n",
            (unsigned long long)bootstrap->magic,
            (unsigned long long)bootstrap->device_fd,
            (unsigned long long)bootstrap->koboxd_image_fd,
            (unsigned long long)bootstrap->koboxd_image_size,
            (unsigned long long)bootstrap->module_count);
        return -1;
    }

    uint64_t koboxd_map_size = 0;
    if (align_up(bootstrap->koboxd_image_size, &koboxd_map_size) != 0) {
        return -1;
    }
    printf("[seed0root] koboxd image mmap begin fd=%llu size=%llu map=%llu\n",
        (unsigned long long)bootstrap->koboxd_image_fd,
        (unsigned long long)bootstrap->koboxd_image_size,
        (unsigned long long)koboxd_map_size);
    fflush(stdout);
    unsigned char *image = pacha_mmap(
        (int)bootstrap->koboxd_image_fd,
        koboxd_map_size,
        PACHA_PROT_READ,
        PACHA_MMAP_SHARED,
        0);
    printf("[seed0root] koboxd image mmap returned ptr=%p\n", (void *)image);
    fflush(stdout);
    if (image == NULL) {
        fprintf(stderr, "[seed0root] koboxd image mmap failed fd=%llu\n",
            (unsigned long long)bootstrap->koboxd_image_fd);
        return -1;
    }
    printf("[seed0root] koboxd image ready\n");
    fflush(stdout);
    struct pacha_ipc_channel_pair control_pair = { .a = -1, .b = -1 };
    int control_status = pacha_ipc_channel_create(&control_pair, seed0root_channel_rights, PACHA_FD_FLAG_INHERIT);
    if (control_status != 0 || control_pair.a < 16 || control_pair.b < 16) {
        fprintf(stderr,
            "[seed0root] koboxd control channel create failed status=%d a=%d b=%d\n",
            control_status,
            control_pair.a,
            control_pair.b);
        (void)pacha_munmap(image, koboxd_map_size);
        return control_status != 0 ? control_status : -2;
    }
    printf("[seed0root] koboxd control ready\n");
    fflush(stdout);
    int status = mark_fd_inherit((int)bootstrap->device_fd, "koboxd device fd");
    if (status != 0) {
        fprintf(stderr, "[seed0root] koboxd device fd inherit failed status=%d fd=%llu\n",
            status,
            (unsigned long long)bootstrap->device_fd);
        (void)pacha_munmap(image, koboxd_map_size);
        return status;
    }
    printf("[seed0root] koboxd device fd ready\n");
    fflush(stdout);
    struct seed0root_koboxd_bootstrap koboxd_bootstrap;
    status = prepare_koboxd_bootstrap(bootstrap, control_pair.b, &koboxd_bootstrap);
    if (status != 0) {
        (void)pacha_munmap(image, koboxd_map_size);
        fprintf(stderr, "[seed0root] koboxd bootstrap package failed status=%d\n", status);
        return status;
    }
    printf("[seed0root] koboxd bootstrap ready\n");
    fflush(stdout);
    const int bootstrap_fd = create_inherited_vmo_from_bytes(&koboxd_bootstrap, sizeof(koboxd_bootstrap), "koboxd bootstrap fd");
    if (bootstrap_fd < 16) {
        (void)pacha_munmap(image, koboxd_map_size);
        fprintf(stderr, "[seed0root] koboxd bootstrap fd create failed status=%d\n", bootstrap_fd);
        return bootstrap_fd;
    }
    printf("[seed0root] koboxd bootstrap fd=%d\n", bootstrap_fd);
    fflush(stdout);
    struct seed0root_loaded_process loaded;
    status = load_elf_process("/sbin/koboxd.elf", image, bootstrap->koboxd_image_size, &loaded);
    (void)pacha_munmap(image, koboxd_map_size);
    if (status != 0) {
        (void)pacha_fd_close(bootstrap_fd);
        fprintf(stderr, "[seed0root] koboxd load failed status=%d\n", status);
        return status;
    }
    printf("[seed0root] koboxd image loaded\n");
    fflush(stdout);
    status = start_loaded_process(&loaded, "/sbin/koboxd.elf", bootstrap_fd, NULL);
    (void)pacha_fd_close(bootstrap_fd);
    if (status != 0) {
        fprintf(stderr, "[seed0root] koboxd start failed status=%d\n", status);
        return status;
    }
    printf("[seed0root] koboxd started\n");
    fflush(stdout);
    status = seed0root_connect_storage_services(control_pair.a);
    if (status != 0) {
        fprintf(stderr, "[seed0root] koboxd connect failed status=%d\n", status);
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
    printf("[seed0root] bootstrap read status=%d magic=0x%llx device_fd=%llu koboxd_fd=%llu koboxd_size=%llu modules=%llu\n",
        bootstrap_status,
        (unsigned long long)bootstrap.magic,
        (unsigned long long)bootstrap.device_fd,
        (unsigned long long)bootstrap.koboxd_image_fd,
        (unsigned long long)bootstrap.koboxd_image_size,
        (unsigned long long)bootstrap.module_count);
    fflush(stdout);
    if (bootstrap_status != 0 ||
        bootstrap.magic != SEED0ROOT_BOOTSTRAP_MAGIC ||
        bootstrap.device_fd < 16 ||
        bootstrap.koboxd_image_fd < 16 ||
        bootstrap.koboxd_image_size == 0 ||
        bootstrap.module_count == 0 ||
        bootstrap.module_count > SEED0ROOT_BOOTSTRAP_MAX_MODULES)
    {
        fprintf(stderr, "[seed0root] bootstrap invalid status=%d\n", bootstrap_status);
        fflush(stderr);
        return 4;
    }
    printf("[seed0root] koboxd launching\n");
    fflush(stdout);
    int launch_status = launch_koboxd(&bootstrap);
    if (launch_status != 0) {
        fprintf(stderr, "[seed0root] koboxd launch failed status=%d\n", launch_status);
        return 5;
    }
    printf("[seed0root] ready\n");
    fflush(stdout);
    fflush(stderr);
    return 0;
}
