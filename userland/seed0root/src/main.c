#include "pacha/ipc.h"
#include "pacha/syscall.h"
#include "filed/bootstrap.h"
#include "koboxd/ipc_protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

enum {
    SEED0ROOT_BOOTSTRAP_MAGIC = 0x305254424f4f5453ull,
    SEED0ROOT_BOOTSTRAP_MAX_MODULES = 8,
    SEED0ROOT_BOOTSTRAP_NAME_BYTES = 64,
    SEED0ROOT_PATH_COMPONENT_BYTES = 128,
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
        PACHA_FD_SYSCALL_MMAP,
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

static int seed0root_create_wire_page(int *out_fd, void **out_mapped)
{
    if (out_fd == NULL || out_mapped == NULL) {
        return -1;
    }
    *out_fd = -1;
    *out_mapped = NULL;
    const uint64_t rights =
        PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    const int fd = pacha_vmo_create(KOBOXD_WIRE_FS_PAGE_BYTES, rights, 0);
    if (fd < 16) {
        fprintf(stderr,
            "[seed0root] wire page create failed bytes=%u status=%d\n",
            (unsigned)KOBOXD_WIRE_FS_PAGE_BYTES,
            fd);
        return fd;
    }
    const long map_result = pacha_syscall6(
        PACHA_FD_SYSCALL_MMAP,
        (uint64_t)(uint32_t)fd,
        0,
        KOBOXD_WIRE_FS_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE,
        PACHA_MMAP_SHARED,
        0);
    void *mapped = map_result < 4096 ? NULL : (void *)(uintptr_t)map_result;
    if (mapped == NULL) {
        fprintf(stderr,
            "[seed0root] wire page map failed bytes=%u status=%ld\n",
            (unsigned)KOBOXD_WIRE_FS_PAGE_BYTES,
            map_result);
        (void)pacha_fd_close(fd);
        return -2;
    }
    memset(mapped, 0, KOBOXD_WIRE_FS_PAGE_BYTES);
    *out_fd = fd;
    *out_mapped = mapped;
    return 0;
}

static void seed0root_destroy_wire_page(int fd, void *mapped)
{
    if (mapped != NULL) {
        (void)pacha_munmap(mapped, KOBOXD_WIRE_FS_PAGE_BYTES);
    }
    if (fd >= 16) {
        (void)pacha_fd_close(fd);
    }
}

static int seed0root_fs_lookup_object(int fs_fd, uint64_t parent_object_id, const char *name, uint64_t *out_object_id)
{
    if (fs_fd < 16 || name == NULL || out_object_id == NULL) {
        return -1;
    }
    *out_object_id = 0;
    int page_fd = -1;
    void *page = NULL;
    int status = seed0root_create_wire_page(&page_fd, &page);
    if (status != 0) {
        return status;
    }
    koboxd_wire_fs_lookup_t *lookup = (koboxd_wire_fs_lookup_t *)page;
    lookup->parent_object_id = parent_object_id;
    snprintf(lookup->name, sizeof(lookup->name), "%s", name);
    status = seed0root_koboxd_endpoint_call_with_fd(fs_fd, KOBOXD_WIRE_FS_LOOKUP, 0, page_fd, out_object_id);
    seed0root_destroy_wire_page(page_fd, page);
    return status;
}

static int seed0root_fs_stat_object(int fs_fd, uint64_t object_id, koboxd_wire_fs_statx_t *out_stat)
{
    if (fs_fd < 16 || object_id == 0 || out_stat == NULL) {
        return -1;
    }
    int page_fd = -1;
    void *page = NULL;
    int status = seed0root_create_wire_page(&page_fd, &page);
    if (status != 0) {
        return status;
    }
    uint64_t ignored = 0;
    status = seed0root_koboxd_endpoint_call_with_fd(fs_fd, KOBOXD_WIRE_FS_STATX, object_id, page_fd, &ignored);
    if (status == 0) {
        *out_stat = *(koboxd_wire_fs_statx_t *)page;
    }
    seed0root_destroy_wire_page(page_fd, page);
    return status;
}

static int seed0root_fs_pread_object(
    int fs_fd,
    uint64_t object_id,
    uint64_t offset,
    void *buffer,
    uint64_t length,
    uint64_t *out_bytes)
{
    if (fs_fd < 16 || object_id == 0 || buffer == NULL || out_bytes == NULL) {
        return -1;
    }
    *out_bytes = 0;
    int page_fd = -1;
    void *page = NULL;
    int status = seed0root_create_wire_page(&page_fd, &page);
    if (status != 0) {
        return status;
    }
    koboxd_wire_fs_io_t *io = (koboxd_wire_fs_io_t *)page;
    io->object_id = object_id;
    io->offset = offset;
    io->length = length > KOBOXD_WIRE_FS_IO_BYTES ? KOBOXD_WIRE_FS_IO_BYTES : length;
    uint64_t bytes = 0;
    status = seed0root_koboxd_endpoint_call_with_fd(fs_fd, KOBOXD_WIRE_FS_PREAD, 0, page_fd, &bytes);
    if (status == 0 && bytes > 0) {
        if (bytes > length) {
            bytes = length;
        }
        memcpy(buffer, io->data, (size_t)bytes);
        *out_bytes = bytes;
    }
    seed0root_destroy_wire_page(page_fd, page);
    return status;
}

static int seed0root_read_rootfs_file(int fs_fd, const char *path, unsigned char **out_image, uint64_t *out_size)
{
    if (fs_fd < 16 || path == NULL || out_image == NULL || out_size == NULL) {
        return -1;
    }
    *out_image = NULL;
    *out_size = 0;

    uint64_t file_object = 0;
    uint64_t object = KOBOXD_WIRE_FS_ROOT_OBJECT_ID;
    const char *cursor = path;
    if (*cursor != '/') {
        return -2;
    }
    while (*cursor == '/') {
        cursor++;
    }
    while (*cursor != '\0') {
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            cursor++;
        }
        const uint64_t len = (uint64_t)(cursor - start);
        if (len == 0 || len >= SEED0ROOT_PATH_COMPONENT_BYTES) {
            return -2;
        }
        char name[SEED0ROOT_PATH_COMPONENT_BYTES];
        memcpy(name, start, (size_t)len);
        name[len] = '\0';
        const int status = seed0root_fs_lookup_object(fs_fd, object, name, &file_object);
        if (status != 0) {
            return status;
        }
        object = file_object;
        while (*cursor == '/') {
            cursor++;
        }
    }
    if (object == KOBOXD_WIRE_FS_ROOT_OBJECT_ID) {
        return -2;
    }

    koboxd_wire_fs_statx_t stat;
    memset(&stat, 0, sizeof(stat));
    int status = seed0root_fs_stat_object(fs_fd, object, &stat);
    if (status != 0 || stat.size == 0 || stat.size > 16ull * 1024ull * 1024ull) {
        return status != 0 ? status : -3;
    }

    unsigned char *image = malloc((size_t)stat.size);
    if (image == NULL) {
        return -4;
    }

    uint64_t offset = 0;
    while (offset < stat.size) {
        uint64_t want = stat.size - offset;
        if (want > KOBOXD_WIRE_FS_IO_BYTES) {
            want = KOBOXD_WIRE_FS_IO_BYTES;
        }
        uint64_t got = 0;
        status = seed0root_fs_pread_object(fs_fd, object, offset, image + offset, want, &got);
        if (status != 0 || got == 0) {
            free(image);
            return status != 0 ? status : -5;
        }
        offset += got;
    }

    *out_image = image;
    *out_size = stat.size;
    return 0;
}

static int launch_filed_from_rootfs(int fs_fd)
{
    if (fs_fd < 16) {
        return -1;
    }
    int status = mark_fd_inherit(fs_fd, "filed fs-backend fd");
    if (status != 0) {
        return status;
    }

    unsigned char *image = NULL;
    uint64_t image_size = 0;
    status = seed0root_read_rootfs_file(fs_fd, "/sbin/filed.elf", &image, &image_size);
    if (status != 0) {
        fprintf(stderr, "[seed0root] filed read failed status=%d\n", status);
        return status;
    }
    const uint64_t filed_endpoint_rights =
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
    const int filed_endpoint_fd = pacha_ipc_endpoint_create(
        filed_endpoint_rights,
        PACHA_FD_FLAG_INHERIT);
    if (filed_endpoint_fd < 16) {
        free(image);
        fprintf(stderr, "[seed0root] filed endpoint create failed status=%d\n", filed_endpoint_fd);
        return filed_endpoint_fd;
    }
    status = mark_fd_inherit(filed_endpoint_fd, "filed public endpoint fd");
    if (status != 0) {
        free(image);
        (void)pacha_fd_close(filed_endpoint_fd);
        return status;
    }

    const filed_bootstrap_t bootstrap = {
        .magic = FILED_BOOTSTRAP_MAGIC,
        .fs_backend_fd = (uint64_t)(uint32_t)fs_fd,
        .public_endpoint_fd = (uint64_t)(uint32_t)filed_endpoint_fd,
        .flags = 0,
    };
    const int bootstrap_fd = create_inherited_vmo_from_bytes(&bootstrap, sizeof(bootstrap), "filed bootstrap fd");
    if (bootstrap_fd < 16) {
        free(image);
        (void)pacha_fd_close(filed_endpoint_fd);
        fprintf(stderr, "[seed0root] filed bootstrap fd create failed status=%d\n", bootstrap_fd);
        return bootstrap_fd;
    }
    struct seed0root_loaded_process loaded;
    status = load_elf_process("/sbin/filed.elf", image, image_size, &loaded);
    free(image);
    if (status != 0) {
        (void)pacha_fd_close(bootstrap_fd);
        (void)pacha_fd_close(filed_endpoint_fd);
        fprintf(stderr, "[seed0root] filed load failed status=%d\n", status);
        return status;
    }
    status = start_loaded_process(&loaded, "/sbin/filed.elf", bootstrap_fd, NULL);
    (void)pacha_fd_close(bootstrap_fd);
    if (status != 0) {
        (void)pacha_fd_close(filed_endpoint_fd);
        fprintf(stderr, "[seed0root] filed start failed status=%d\n", status);
        return status;
    }
    (void)pacha_fd_close(filed_endpoint_fd);
    printf("[seed0root] filed ready\n");
    return 0;
}

static int seed0root_connect_storage_services(int control_fd)
{
    int block_fd = -1;
    int fs_fd = -1;
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

    status = launch_filed_from_rootfs(fs_fd);
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
        const uint64_t requested_va = (use_aslr && load_count == 0) ? 0 : align_down(p_vaddr + load_bias);
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
        0,
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
