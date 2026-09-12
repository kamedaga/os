/* SPDX-License-Identifier: MIT */
#include "launch_native.h"
#include <pacha/syscall.h>
#include <errno.h>
#include <string.h>

static int status(long result) {
    switch (result) {
    case 0: return 0;
    case PACHA_SYSCALL_ERR_INVALID: return -EINVAL;
    case PACHA_SYSCALL_ERR_NOT_READY: return -EAGAIN;
    case PACHA_SYSCALL_ERR_ALLOC: return -ENOMEM;
    case PACHA_SYSCALL_ERR_MAP: return -EIO;
    default: return -EPROTO;
    }
}

static int valid_fd(long fd) {
    return fd >= 16 && fd < PACHA_FD_TABLE_LIMIT;
}

static int unexpected_result(long result) {
    int error = status(result);
    return error ? error : -EPROTO;
}

static uint64_t page_size(uint64_t size) {
    return (size + GPUD_LAUNCH_PAGE - 1) & ~(uint64_t)(GPUD_LAUNCH_PAGE - 1);
}

static int valid_range(uint64_t address, uint64_t size) {
    return !(address & (GPUD_LAUNCH_PAGE - 1)) &&
        !(size & (GPUD_LAUNCH_PAGE - 1)) && gpud_launch_address_valid(address, size);
}

static int overlaps(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    return a < b + bs && b < a + as;
}

static int validate_request(const struct gpud_launch_request *request,
    const struct gpud_launch_image *image) {
    if (!request->generation || request->blob_count > GPUD_LAUNCH_MAX_BLOBS ||
        (request->blob_count && !request->blobs) ||
        request->grant_count > PACHA_PROCESS_CREATE_MAX_GRANTS ||
        (request->grant_count && !request->grants) ||
        request->stack_size < 4 * GPUD_LAUNCH_PAGE || request->stack_size > 8u * 1024u * 1024u ||
        !valid_range(request->stack_address, request->stack_size)) return -EINVAL;
    for (size_t i = 0; i < image->segment_count; ++i) {
        const struct gpud_launch_segment *segment = &image->segments[i];
        if (overlaps(segment->address, segment->mapping_size,
            request->stack_address, request->stack_size)) return -EINVAL;
    }
    for (size_t i = 0; i < request->blob_count; ++i) {
        const struct gpud_launch_blob *blob = &request->blobs[i];
        if (!blob->bytes || !blob->size || blob->size > GPUD_LAUNCH_IMAGE_LIMIT ||
            !valid_range(blob->address, page_size(blob->size)) ||
            overlaps(blob->address, page_size(blob->size),
                request->stack_address, request->stack_size)) return -EINVAL;
        for (size_t j = 0; j < image->segment_count; ++j)
            if (overlaps(blob->address, page_size(blob->size), image->segments[j].address,
                image->segments[j].mapping_size)) return -EINVAL;
        for (size_t j = 0; j < i; ++j)
            if (overlaps(blob->address, page_size(blob->size), request->blobs[j].address,
                page_size(request->blobs[j].size))) return -EINVAL;
    }
    return 0;
}

static int close_fd(int *fd) {
    if (!*fd) return 0;
    int result = status(pacha_syscall1(PACHA_FD_SYSCALL_CLOSE, (uint64_t)*fd));
    if (!result) *fd = 0;
    return result;
}

static int clear_scratch(struct gpud_native_launch *launch) {
    if (launch->scratch_view) {
        int result = status(pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP,
            (uintptr_t)launch->scratch_view, launch->scratch_size));
        if (result) return result;
        launch->scratch_view = NULL;
        launch->scratch_size = 0;
    }
    return close_fd(&launch->scratch_fd);
}

static int map_into(struct gpud_native_launch *launch, uint64_t fd, uint64_t address,
    uint64_t size, uint64_t protection, uint64_t flags) {
    long result = pacha_syscall6(PACHA_PROCESS_SYSCALL_MAP, (uint64_t)launch->process.fd,
        fd, address, size, protection, flags);
    return (uint64_t)result == address ? 0 : unexpected_result(result);
}

static int copy_mapping(struct gpud_native_launch *launch, const void *bytes,
    size_t byte_count, size_t data_offset, uint64_t address, size_t size, uint64_t protection) {
    uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE;
    if (protection & PACHA_PROT_EXEC) rights |= PACHA_FD_RIGHT_MAP_EXEC;
    long fd = pacha_syscall3(PACHA_FD_SYSCALL_VMO_CREATE, size, rights, 0);
    if (!valid_fd(fd)) return unexpected_result(fd);
    launch->scratch_fd = (int)fd;
    long view = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, (uint64_t)fd, 0, size,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    if (view < GPUD_LAUNCH_PAGE) return unexpected_result(view);
    launch->scratch_view = (void *)(uintptr_t)view;
    launch->scratch_size = size;
    memset(launch->scratch_view, 0, size);
    memcpy((unsigned char *)launch->scratch_view + data_offset, bytes, byte_count);
    int result = map_into(launch, (uint64_t)fd, address, size, protection, PACHA_PROCESS_MAP_SHARED);
    if (result) return result;
    return clear_scratch(launch);
}

int gpud_native_launch_prepare(struct gpud_native_launch *launch,
    const struct gpud_launch_request *request) {
    if (!launch || !request) return -EINVAL;
    if (launch->process.fd || launch->thread_fd || launch->scratch_fd || launch->scratch_view)
        return -EBUSY;
    if (!request->generation || request->generation <= launch->process.generation) return -ESTALE;
    struct gpud_launch_image image;
    int result = gpud_launch_image_plan(request->image, request->image_size,
        request->load_bias, &image);
    if (result) return result;
    result = validate_request(request, &image);
    if (result) return result;
    *launch = (struct gpud_native_launch){.process = {.generation = request->generation}};
    uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_KILL | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_INTO | PACHA_FD_RIGHT_SPAWN |
        PACHA_FD_RIGHT_SET_CONTEXT;
    long process = pacha_syscall5(PACHA_PROCESS_SYSCALL_CREATE, 0, rights, 0,
        (uintptr_t)request->grants, request->grant_count);
    if (!valid_fd(process)) {
        result = unexpected_result(process);
        goto failed;
    }
    /* This exact FD was minted by CREATE with our requested kind/rights;
     * unlike adopting a caller FD, no fallible GET_INFO is needed here. */
    launch->process.fd = (int)process;
    for (size_t i = 0; i < image.segment_count; ++i) {
        const struct gpud_launch_segment *segment = &image.segments[i];
        result = copy_mapping(launch, (const unsigned char *)request->image + segment->file_offset,
            segment->file_size, segment->data_offset, segment->address,
            segment->mapping_size, segment->protection);
        if (result) goto failed;
    }
    for (size_t i = 0; i < request->blob_count; ++i) {
        const struct gpud_launch_blob *blob = &request->blobs[i];
        result = copy_mapping(launch, blob->bytes, blob->size, 0, blob->address,
            page_size(blob->size), PACHA_PROT_READ);
        if (result) goto failed;
    }
    result = map_into(launch, 0, request->stack_address, request->stack_size,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_PROCESS_MAP_PRIVATE | PACHA_PROCESS_MAP_ANONYMOUS);
    if (result) goto failed;
    long thread = pacha_syscall6(PACHA_THREAD_SYSCALL_CREATE, (uint64_t)launch->process.fd,
        image.entry, request->stack_address + request->stack_size, 0, 0,
        PACHA_FD_RIGHT_START | PACHA_FD_RIGHT_CLOSE);
    if (!valid_fd(thread)) {
        result = unexpected_result(thread);
        goto failed;
    }
    launch->thread_fd = (int)thread;
    launch->prepared = 1;
    return 0;
failed:
    launch->failed = 1;
    launch->error = result;
    return result;
}

int gpud_native_launch_start(struct gpud_native_launch *launch, uint64_t generation) {
    if (!launch || !generation) return -EINVAL;
    if (launch->process.generation != generation) return -ESTALE;
    if (!launch->prepared || launch->started || launch->failed || !launch->thread_fd ||
        !launch->process.fd || launch->scratch_fd || launch->scratch_view) return -EINVAL;
    int result = status(pacha_syscall1(PACHA_THREAD_SYSCALL_START, (uint64_t)launch->thread_fd));
    if (result) {
        launch->failed = 1;
        launch->error = result;
    } else launch->started = 1;
    return result;
}

int gpud_native_launch_discard(struct gpud_native_launch *launch, uint64_t generation) {
    if (!launch || !generation) return -EINVAL;
    if (launch->process.generation != generation) return -ESTALE;
    if (launch->process.fd && !launch->process.terminal) return -EBUSY;
    int result = clear_scratch(launch);
    if (result) return result;
    result = close_fd(&launch->thread_fd);
    if (result) return result;
    result = gpud_native_process_release(&launch->process, generation);
    if (result) return result;
    *launch = (struct gpud_native_launch){.process = {.generation = generation}};
    return 0;
}

int gpud_native_launch_abort(struct gpud_native_launch *launch, uint64_t generation) {
    if (!launch || !generation) return -EINVAL;
    if (launch->process.generation != generation) return -ESTALE;
    if (launch->process.fd) {
        int result = gpud_native_process_terminate(&launch->process, generation, 9);
        if (result) return result;
    }
    return gpud_native_launch_discard(launch, generation);
}
