/* SPDX-License-Identifier: MIT */
#pragma once

/* A WAIT completion has its own errno as well as the remaining timeout.
 * A request-sized frame on failure means no validated driver reply arrived. */
static int64_t lpr_i915_wait_copyout(const gpud_drm_ioctl_request_t *reply,
    int64_t status, uint64_t argument)
{
    if (reply->data_size == 16 && status) return status;
    int32_t result;
    uint32_t reserved;
    if (reply->data_size != KB2_GPU_DRM_I915_RECORD_GEM_WAIT_RESULT_SIZE)
        return -LPR_DRM_ERR_PROTOCOL;
    lpr_memcpy(&result, reply->data + 16, 4);
    lpr_memcpy(&reserved, reply->data + 20, 4);
    if (reserved || result > 0 || result < -4095 || result != status)
        return -LPR_DRM_ERR_PROTOCOL;
    lpr_memcpy((unsigned char *)(uintptr_t)argument + 8, reply->data + 8, 8);
    return status;
}

/* Keep pointer snapshots off the general ioctl frame. Neither these pointers
 * nor the Linux query item's data_pointer is sent across the native IPC. */
static __attribute__((noinline)) int64_t lpr_i915_query_ioctl(
    uint64_t handle, uint64_t argument)
{
    gpud_drm_i915_query_t input;
    gpud_drm_i915_query_item_t original[KB2_GPU_DRM_I915_MAX_QUERY_ITEMS];
    gpud_drm_i915_query_item_wire_t items[KB2_GPU_DRM_I915_MAX_QUERY_ITEMS];
    uint64_t offsets[KB2_GPU_DRM_I915_MAX_QUERY_ITEMS];
    lpr_memcpy(&input, (const void *)(uintptr_t)argument, sizeof(input));
    if (input.flags || input.count > KB2_GPU_DRM_I915_MAX_QUERY_ITEMS)
        return -LPR_LINUX_EINVAL;
    if (input.count && !input.items_pointer)
        return -LPR_LINUX_EFAULT;
    const uint64_t prefix = (uint64_t)input.count * sizeof(items[0]);
    uint64_t bytes = prefix;
    if (input.count)
        lpr_memcpy(original, (const void *)(uintptr_t)input.items_pointer,
            (size_t)input.count * sizeof(original[0]));
    for (uint32_t i = 0; i < input.count; ++i) {
        offsets[i] = 0;
        if (original[i].length > 0 && original[i].data_pointer) {
            const uint64_t size = (uint32_t)original[i].length;
            bytes = (bytes + 7) & ~UINT64_C(7);
            if (bytes > KB2_GPU_DRM_I915_MAX_QUERY_BYTES ||
                size > KB2_GPU_DRM_I915_MAX_QUERY_BYTES - bytes)
                return -LPR_LINUX_EINVAL;
            offsets[i] = bytes;
            bytes += size;
        }
        items[i] = (gpud_drm_i915_query_item_wire_t){
            .query_id = original[i].query_id, .length = original[i].length,
            .flags = original[i].flags, .data_offset = offsets[i]};
    }
    void *page = 0, *aux = 0;
    pacha_fd_t aux_fd = -1;
    uint64_t aux_size = 0;
    const pacha_fd_t page_fd = lpr_create_tty_wire_page(&page);
    if (page_fd < 0) return page_fd;
    int64_t status = 0;
    if (bytes) {
        status = lpr_drm_aux_create(bytes, 0, &aux_fd, &aux, &aux_size);
        if (status) goto done;
        lpr_memset(aux, 0, bytes);
        lpr_memcpy(aux, items, prefix);
        for (uint32_t i = 0; i < input.count; ++i)
            if (offsets[i])
                lpr_memcpy((unsigned char *)aux + offsets[i],
                    (const void *)(uintptr_t)original[i].data_pointer,
                    (uint32_t)original[i].length);
    }
    gpud_drm_ioctl_request_t *request = lpr_gpud_drm_payload(page);
    lpr_memset(request, 0, sizeof(*request));
    request->handle = handle;
    request->request = GPUD_DRM_IOCTL_I915_QUERY;
    request->arg_size = sizeof(input);
    request->data_size = sizeof(gpud_drm_i915_query_wire_t);
    request->aux_size = bytes;
    const gpud_drm_i915_query_wire_t wire = {
        .count = input.count, .flags = input.flags, .blob_bytes = bytes - prefix};
    lpr_memcpy(request->data, &wire, sizeof(wire));
    status = lpr_gpud_drm_call_transfer(GPUD_DRM_OP_HANDLE_IOCTL,
        page_fd, page, sizeof(*request), 0, 0, aux_fd, 1);
    if (status) goto done;
    if (prefix) lpr_memcpy(items, aux, prefix);
    /* Validate the entire response before any writes through saved caller
     * pointers. Item errors are signed lengths, not whole-ioctl failures. */
    for (uint32_t i = 0; i < input.count; ++i) {
        if (items[i].query_id != original[i].query_id ||
            items[i].flags != original[i].flags || items[i].data_offset != offsets[i] ||
            (offsets[i] && items[i].length > original[i].length)) {
            status = -LPR_DRM_ERR_PROTOCOL;
            goto done;
        }
    }
    for (uint32_t i = 0; i < input.count; ++i) {
        lpr_memcpy((unsigned char *)(uintptr_t)input.items_pointer +
            (size_t)i * sizeof(original[0]) + sizeof(uint64_t),
            &items[i].length, sizeof(items[i].length));
        if (offsets[i] && items[i].length > 0)
            lpr_memcpy((void *)(uintptr_t)original[i].data_pointer,
                (unsigned char *)aux + offsets[i], (uint32_t)items[i].length);
    }
done:
    lpr_drm_aux_destroy(aux_fd, aux, aux_size);
    lpr_destroy_tty_wire_page(page_fd, page);
    return status;
}
