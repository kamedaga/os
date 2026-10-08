/* SPDX-License-Identifier: MIT */
#include "block_service.h"
#include "host.h"
#include "boot/module_launch.h"

#include <pacha/syscall.h>
#include <errno.h>
#include <string.h>

static uint32_t block_status(int error) {
    switch (error) {
    case 0: return KB2_BLOCK_DEVICE_STATUS_OK;
    case -EINVAL: return KB2_BLOCK_DEVICE_STATUS_INVALID;
    case -ESTALE: return KB2_BLOCK_DEVICE_STATUS_STALE;
    case -ENODEV:
    case -ENXIO: return KB2_BLOCK_DEVICE_STATUS_NO_DEVICE;
    case -EACCES:
    case -EPERM:
    case -EROFS: return KB2_BLOCK_DEVICE_STATUS_READ_ONLY;
    case -ENOSPC: return KB2_BLOCK_DEVICE_STATUS_NO_SPACE;
    case -EOPNOTSUPP: return KB2_BLOCK_DEVICE_STATUS_UNSUPPORTED;
    default: return KB2_BLOCK_DEVICE_STATUS_IO;
    }
}

static int prepare(void *context, struct ph_ipc_packet *packet) {
    struct ph_block_service *block = context;
    kb2_block_header_t header;
    uint32_t opcode;
    if (!packet->correlation || packet->generation != block->generation ||
        block->operation) return -EPROTO;
    if (packet->operation == PH_BLOCK_ATTACH) {
        struct pacha_fd_info info = {0};
        if (block->shared || packet->fd_count != 1 ||
            packet->value != PH_BLOCK_SHARED_BYTES ||
            pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO,
                packet->fds[0].fd, (uintptr_t)&info) ||
            info.kind != PACHA_FD_KIND_VMO || info.size < PH_BLOCK_SHARED_BYTES ||
            (info.rights & (PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE)) !=
                (PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE))
            return -EPROTO;
        long mapped = pacha_syscall6(PACHA_VM_SYSCALL_MMAP,
            packet->fds[0].fd, 0, PH_BLOCK_SHARED_BYTES,
            PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
        if (mapped < 4096) return -ENOMEM;
        block->shared = (void *)(uintptr_t)mapped;
    } else {
        if (!block->shared || packet->fd_count || packet->value ||
            packet->operation < PH_BLOCK_SNAPSHOT ||
            packet->operation > PH_BLOCK_FLUSH ||
            kb2_block_header_decode(block->shared, PH_BLOCK_SHARED_BYTES,
                &header)) return -EPROTO;
        opcode = packet->operation - PH_BLOCK_ATTACH;
        if (header.opcode != opcode || header.status ||
            header.generation != block->generation ||
            header.sequence != packet->correlation ||
            header.sequence <= block->sequence) return -EPROTO;
        if (opcode == KB2_BLOCK_DEVICE_OP_SNAPSHOT) {
            if (header.diskseq || header.major || header.minor ||
                header.length || !header.count) return -EPROTO;
        } else if (opcode == KB2_BLOCK_DEVICE_OP_FLUSH) {
            if (!header.diskseq || header.count || header.length || header.offset)
                return -EPROTO;
        } else if (!header.diskseq || header.count || !header.length ||
                   header.length > PH_BLOCK_SHARED_BYTES - KB2_BLOCK_DEVICE_HEADER_SIZE)
            return -EPROTO;
        block->sequence = header.sequence;
    }
    block->operation = packet->operation;
    block->correlation = packet->correlation;
    return 0;
}

static int dispatch(void *context, void *linux_service) {
    struct ph_block_service *block = context;
    struct kobox_linux_io_service *io = linux_service;
    kb2_block_header_t header;
    struct kobox_linux_block_key key;
    int result = 0;
    if (!block->shared || !block->operation || !io || !io->block) return -EPROTO;
    if (block->operation == PH_BLOCK_ATTACH) return 0;
    if (kb2_block_header_decode(block->shared, PH_BLOCK_SHARED_BYTES, &header))
        return -EPROTO;
    key = (struct kobox_linux_block_key){.diskseq = header.diskseq,
        .major = header.major, .minor = header.minor};
    if (block->operation == PH_BLOCK_SNAPSHOT) {
        struct kobox_linux_block_info devices[KB2_BLOCK_DEVICE_MAX_DISKS];
        size_t count = 0, next = 0;
        result = block->snapshot(io->block, header.offset, devices,
            header.count, &count, &next);
        if (!result) {
            for (size_t i = 0; i < count; ++i) {
                kb2_block_disk_t disk = {.diskseq = devices[i].diskseq,
                    .bytes = devices[i].bytes, .major = devices[i].major,
                    .minor = devices[i].minor,
                    .logical_block_size = devices[i].logical_block_size,
                    .physical_block_size = devices[i].physical_block_size,
                    .removable = devices[i].removable,
                    .read_only = devices[i].read_only};
                memcpy(disk.name, devices[i].name, sizeof(disk.name));
                if (kb2_block_disk_encode(block->shared +
                        KB2_BLOCK_DEVICE_HEADER_SIZE + i * KB2_BLOCK_DEVICE_DISK_SIZE,
                        PH_BLOCK_SHARED_BYTES - KB2_BLOCK_DEVICE_HEADER_SIZE -
                            i * KB2_BLOCK_DEVICE_DISK_SIZE, &disk))
                    return -EPROTO;
            }
            header.count = (uint32_t)count;
            header.offset = next;
        }
    } else if (block->operation == PH_BLOCK_READ) {
        result = block->read(io->block, &key, header.offset,
            block->shared + KB2_BLOCK_DEVICE_HEADER_SIZE, header.length);
    } else if (block->operation == PH_BLOCK_WRITE) {
        result = block->write(io->block, &key, header.offset,
            block->shared + KB2_BLOCK_DEVICE_HEADER_SIZE, header.length);
    } else if (block->operation == PH_BLOCK_FLUSH) {
        result = block->flush(io->block, &key);
    } else return -EPROTO;
    header.status = block_status(result);
    if (kb2_block_header_encode(block->shared, PH_BLOCK_SHARED_BYTES, &header))
        return -EPROTO;
    atomic_thread_fence(memory_order_release);
    return 0;
}

static int complete(void *context, struct ph_ipc *ipc) {
    struct ph_block_service *block = context;
    uint64_t status = 0;
    if (block->operation != PH_BLOCK_ATTACH) {
        kb2_block_header_t header;
        if (kb2_block_header_decode(block->shared, PH_BLOCK_SHARED_BYTES, &header))
            return -EPROTO;
        status = header.status;
    }
    struct ph_ipc_packet reply = {.operation = block->operation,
        .generation = block->generation, .correlation = block->correlation,
        .value = status};
    return ph_ipc_send(ipc, &reply);
}

static int release(void *context) {
    struct ph_block_service *block = context;
    block->operation = 0;
    block->correlation = 0;
    return 0;
}

static int stop(void *context) {
    struct ph_block_service *block = context;
    if (!block->shared) return 0;
    if (pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP,
            (uintptr_t)block->shared, PH_BLOCK_SHARED_BYTES)) return -EIO;
    block->shared = NULL;
    return 0;
}

int ph_block_service_init(struct ph_block_service *block, void *core,
    uint64_t generation, struct ph_lifecycle_service *service) {
    if (!block || !core || !generation || !service) return -EINVAL;
    void *snapshot = ph_image_lookup(core, "kobox_linux_block_port_snapshot");
    void *read = ph_image_lookup(core, "kobox_linux_block_port_read");
    void *write = ph_image_lookup(core, "kobox_linux_block_port_write");
    void *flush = ph_image_lookup(core, "kobox_linux_block_port_flush");
    if (!snapshot || !read || !write || !flush) return -ENOENT;
    memset(block, 0, sizeof(*block));
    memcpy(&block->snapshot, &snapshot, sizeof(snapshot));
    memcpy(&block->read, &read, sizeof(read));
    memcpy(&block->write, &write, sizeof(write));
    memcpy(&block->flush, &flush, sizeof(flush));
    block->generation = generation;
    *service = (struct ph_lifecycle_service){.context = block,
        .prepare = prepare, .dispatch = dispatch, .complete = complete,
        .release = release, .stop = stop};
    return 0;
}
