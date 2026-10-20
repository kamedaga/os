#include "lpr_ipc_mock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/personality/linux/runtime/lpr_drm/client.c"

lpr_state_t lpr_state;
static unsigned char page[4096];
static unsigned char auxiliary[GPUD_DRM_AUX_REUSE_BYTES], temporary[2 * GPUD_DRM_AUX_REUSE_BYTES];
static int vmo_creates, maps, unmaps, allocation_failure, mapping_failure;
static uint64_t allocation_size, allocation_flags, request_marker;
static int creates, registrations, requests, closed[128], fail_recv, fail_call, bad_reply, bind_error;
static uint64_t last_magic, last_id;
static char diagnostic[256];
int64_t lpr_linux_write(uint64_t fd, uint64_t data, uint64_t bytes, unsigned *wait_not_ready __attribute__((unused)))
{
    assert(fd == 2 && bytes < sizeof(diagnostic));
    memcpy(diagnostic, (const void *)(uintptr_t)data, bytes);
    diagnostic[bytes] = 0;
    return (int64_t)bytes;
}
void *lpr_memset(void *p, int c, size_t n) { return memset(p, c, n); }
void *lpr_memcpy(void *p, const void *s, size_t n) { return memcpy(p, s, n); }
uint64_t lpr_next_request_id(volatile uint64_t *counter) { return ++*counter; }
int64_t lpr_pacha_status_to_errno(int64_t status) { return status > 0 ? -status : status; }
int64_t pacha_kernel_status_to_errno(int64_t status) { return lpr_pacha_status_to_errno(status); }
int lpr_native_fd_info(uint64_t fd, struct pacha_fd_info *info)
{
    assert(fd == 91 || fd == 92);
    *info = (struct pacha_fd_info){.kind = PACHA_FD_KIND_VMO, .rights = UINT64_MAX};
    return 1;
}
int64_t lpr_pacha_syscall1(uint64_t op, uint64_t fd)
{ assert(op == PACHAOS_SYSCALL_FD_CLOSE && fd < 128 && fd >= 16); ++closed[fd]; return 0; }
int64_t lpr_pacha_syscall3(uint64_t op, uint64_t address, uint64_t rights, uint64_t flags)
{
    if (op == PACHAOS_SYSCALL_VMO_CREATE) {
        ++vmo_creates; allocation_size = address; allocation_flags = flags;
        if (allocation_failure) return PACHA_SYSCALL_ERR_ALLOC;
        return flags ? 91 : 92;
    }
    assert(op == PACHAOS_SYSCALL_IPC_CHANNEL_CREATE);
    assert((rights & (PACHA_FD_RIGHT_CALL | PACHA_FD_RIGHT_RECV)) ==
        (PACHA_FD_RIGHT_CALL | PACHA_FD_RIGHT_RECV));
    assert(flags == (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC));
    uint64_t *pair = (void *)(uintptr_t)address;
    pair[0] = 80; pair[1] = 81; ++creates; return 0;
}
static int64_t fixture_prepare_request(uint64_t op, uint64_t fd, uint64_t address)
{
    if (op == PACHAOS_SYSCALL_MUNMAP) {
        assert((fd == (uintptr_t)auxiliary && address == sizeof(auxiliary)) ||
            (fd == (uintptr_t)temporary && address == allocation_size));
        ++unmaps; return 0;
    }
    assert(op == PACHAOS_SYSCALL_IPC_CALL);
    const struct pacha_ipc_msg *message = (void *)(uintptr_t)address;
    last_magic = message->word0; last_id = message->word3;
    if (last_magic == GPUD_DRM_BIND_PAGE_REQUEST_MAGIC) {
        ++registrations;
        assert(fd == LPR_GPUD_DRM_ENDPOINT_FD && message->fd_count == (lpr_drm_aux_cache ? 3u : 2u));
        assert(message->fds[0].fd == 90 && message->fds[1].fd == 81);
        if (lpr_drm_aux_cache) {
            assert(message->word1 == sizeof(auxiliary) && message->fds[2].fd == 91);
            assert(message->fds[2].rights == (PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE));
        }
    } else {
        ++requests;
        assert(fd == 80 && last_magic == PACHA_SERVICE_REQUEST_MAGIC && !message->fd_count);
        assert(message->word1 == request_marker);
        if (fail_call) return PACHA_SYSCALL_ERR_CLOSED;
    }
    return 82;
}
int64_t lpr_pacha_syscall6(uint64_t op, uint64_t fd, uint64_t address,
    uint64_t bytes, uint64_t prot, uint64_t flags, uint64_t offset)
{
    assert(op == PACHAOS_SYSCALL_MMAP && !address && bytes == allocation_size && !offset);
    assert(prot == (PACHAOS_PROT_READ | PACHAOS_PROT_WRITE) && flags == PACHAOS_MMAP_SHARED);
    assert(fd == 91 || fd == 92);
    ++maps;
    return mapping_failure ? PACHA_SYSCALL_ERR_ALLOC :
        (int64_t)(uintptr_t)(fd == 91 ? auxiliary : temporary);
}
static int64_t fixture_finish_request(uint64_t op, uint64_t fd, uint64_t address, uint64_t ticks, uint64_t flags)
{
    assert(op == PACHAOS_SYSCALL_IPC_RECV_WAIT && fd == 82 && ticks == UINT64_MAX && flags == PACHA_WAIT_UNINTERRUPTIBLE);
    if (fail_recv && last_magic != GPUD_DRM_BIND_PAGE_REQUEST_MAGIC) return PACHA_SYSCALL_ERR_CLOSED;
    struct pacha_ipc_msg *reply = (void *)(uintptr_t)address;
    if (last_magic == GPUD_DRM_BIND_PAGE_REQUEST_MAGIC) {
        reply->word0 = GPUD_DRM_BIND_PAGE_REPLY_MAGIC;
        reply->word1 = (uint64_t)bind_error;
    } else {
        reply->word0 = bad_reply ? 0 : PACHA_SERVICE_REPLY_MAGIC;
        reply->word3 = last_id;
        pacha_service_envelope_t *header = (void *)page;
        header->magic = PACHA_SERVICE_REPLY_MAGIC;
        header->status = 0; header->result = 1;
    }
    return 0;
}
static int64_t hello(void)
{
    uint64_t result = 0;
    int64_t error = lpr_gpud_drm_call(GPUD_DRM_OP_HELLO, 90, page, 0, &result, NULL);
    if (!error) assert(result == 1);
    return error;
}
static void kms_query_marshalling(void)
{
    const uint32_t commands[] = {GPUD_DRM_IOCTL_MODE_GETRESOURCES,
        GPUD_DRM_IOCTL_MODE_GETPLANERESOURCES, GPUD_DRM_IOCTL_MODE_GETPLANE,
        GPUD_DRM_IOCTL_MODE_GETPROPERTY, GPUD_DRM_IOCTL_MODE_OBJ_GETPROPERTIES};
    for (unsigned int command = 0; command < 5; ++command) {
        struct gpud_kms_query_shape shape;
        assert(gpud_kms_query_shape(commands[command], &shape));
        for (unsigned int large = 0; large < 2; ++large) {
            unsigned char snapshot[64] = {0}, scalar[64] = {0};
            uint64_t arrays[4][256], values[256];
            memset(arrays, 0xcc, sizeof(arrays)); memset(values, 0xcc, sizeof(values));
            gpud_drm_ioctl_request_t request = {0};
            uint32_t capacities[4], offset = 0;
            for (unsigned int i = 0; i < shape.vectors; ++i) {
                const struct gpud_kms_vector *v = &shape.vector[i];
                capacities[i] = large ? v->limit + 3 : 2;
                uint64_t pointer = (uintptr_t)arrays[i], second_pointer = (uintptr_t)values;
                memcpy(snapshot + v->count, &capacities[i], 4);
                memcpy(snapshot + v->pointer, &pointer, 8);
                if (v->second_pointer) memcpy(snapshot + v->second_pointer, &second_pointer, 8);
            }
            pacha_fd_t fd = -1; void *mapping = NULL; uint64_t map_size = 0;
            assert(!lpr_kms_query_prepare(&request, &shape, snapshot, 0, &fd, &mapping, &map_size));
            assert(request.data_size == (large ? shape.scalar_size : shape.inline_size));
            memcpy(scalar, request.data, shape.scalar_size);
            for (unsigned int i = 0; i < shape.vectors; ++i) {
                const struct gpud_kms_vector *v = &shape.vector[i];
                uint64_t pointer;
                memcpy(&pointer, scalar + v->pointer, 8); assert(!pointer);
                if (v->second_pointer) {
                    memcpy(&pointer, scalar + v->second_pointer, 8); assert(!pointer);
                }
                unsigned char *target = large ? (unsigned char *)mapping + offset : request.data + v->inline_array;
                if (v->second_pointer) {
                    for (uint32_t n = 0; n < capacities[i]; ++n) {
                        uint32_t id = 100 + n; uint64_t value = 200 + n;
                        memcpy(target + n * (large ? 16 : 4), &id, 4);
                        memcpy(large ? target + n * 16 + 8 : request.data + v->second_array + n * 8, &value, 8);
                    }
                } else memset(target, 0x5a, capacities[i] * v->stride);
                offset += capacities[i] * v->stride;
            }
            /* Insufficient whole vectors stay untouched; partial vectors
             * expose only capacity, matching their individual Linux ioctl. */
            for (unsigned int i = 0; i < shape.vectors; ++i) {
                uint32_t required = capacities[i] + 1;
                memcpy(request.data + shape.vector[i].count, &required, 4);
            }
            assert(!lpr_kms_query_reply(&request, &shape, snapshot, mapping, (uintptr_t)scalar));
            for (unsigned int i = 0; i < shape.vectors; ++i) {
                const struct gpud_kms_vector *v = &shape.vector[i];
                assert(!memcmp(scalar + v->pointer, snapshot + v->pointer, 8));
                const unsigned char *bytes = (const unsigned char *)arrays[i];
                assert(bytes[0] == (v->partial ? v->second_pointer ? 100 : 0x5a : 0xcc));
                assert(bytes[capacities[i] * (v->second_pointer ? 4 : v->stride)] == 0xcc);
            }
            /* Exact output copies every element but never overwrites tails. */
            for (unsigned int i = 0; i < shape.vectors; ++i)
                memcpy(request.data + shape.vector[i].count, &capacities[i], 4);
            assert(!lpr_kms_query_reply(&request, &shape, snapshot, mapping, (uintptr_t)scalar));
            for (unsigned int i = 0; i < shape.vectors; ++i)
                assert(((unsigned char *)arrays[i])[0] == (shape.vector[i].second_pointer ? 100 : 0x5a));
            if (large && shape.vector[0].second_pointer) {
                uint32_t reserved = 1;
                memcpy((unsigned char *)mapping + 4, &reserved, 4);
                memset(arrays, 0xcc, sizeof(arrays));
                assert(lpr_kms_query_reply(&request, &shape, snapshot, mapping, (uintptr_t)scalar) == -LPR_LINUX_EIO);
                assert(((unsigned char *)arrays)[0] == 0xcc);
            }
            if (large) lpr_drm_aux_destroy(fd, mapping, map_size);
            uint32_t excessive = 65537;
            memcpy(snapshot + shape.vector[0].count, &excessive, 4);
            assert(lpr_kms_query_prepare(&request, &shape, snapshot, 0,
                &fd, &mapping, &map_size) == -LPR_LINUX_EINVAL);
        }
    }
    puts("LPR KMS queries: pointer stripping/restoration, dynamic vectors, partial/exact copies and malformed records PASS");
}

int main(void)
{
    assert(amdgpu_info_failure(22, 440, "validate", -22, 0) == -22);
    assert(strstr(diagnostic, "phase=validate query=22 bytes=440 errno=22"));
    lpr_tty_wire_page_fd = 90; lpr_tty_wire_page = page;
    assert(!hello() && creates == 1 && registrations == 1 && requests == 1);
    assert(!closed[80] && closed[81] == 1);
    for (int i = 0; i < 7; ++i) assert(!hello());
    assert(creates == 1 && requests == 8);
    fail_recv = 1;
    assert(hello() < 0 && requests == 9 && creates == 1 && closed[80] == 1);
    assert(!lpr_drm_request_channel_fd); /* No automatic request replay. */
    fail_recv = 0;
    assert(!hello() && creates == 2 && requests == 10);
    int before = closed[80];
    lpr_drm_after_fork_child();
    assert(!lpr_drm_request_channel_fd && closed[80] == before);
    assert(!hello() && creates == 3 && requests == 11);
    bad_reply = 1;
    assert(hello() < 0 && requests == 12 && !lpr_drm_request_channel_fd);
    bad_reply = 0; bind_error = -12;
    assert(hello() == -12 && requests == 12 && !lpr_drm_request_channel_fd);
    bind_error = 0; fail_call = 1;
    assert(hello() < 0 && requests == 13 && !lpr_drm_request_channel_fd);
    assert(lpr_gpud_drm_bind_page(91, page) == LPR_GPUD_DRM_ENDPOINT_FD);
    fail_call = 0;
    assert(!hello());
    pacha_fd_t aux_fd; void *mapping; uint64_t bytes;
    allocation_failure = 1;
    assert(lpr_drm_aux_create(5284, 1, &aux_fd, &mapping, &bytes) < 0);
    assert(!lpr_drm_aux_cache);
    allocation_failure = 0; mapping_failure = 1;
    assert(lpr_drm_aux_create(5284, 1, &aux_fd, &mapping, &bytes) < 0);
    assert(!lpr_drm_aux_cache && closed[91] == 1);
    mapping_failure = 0;
    assert(!lpr_drm_aux_create(5284, 1, &aux_fd, &mapping, &bytes));
    assert(aux_fd == 91 && mapping == auxiliary && bytes == sizeof(auxiliary));
    assert(allocation_flags == (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC));
    assert(!lpr_drm_request_channel_fd && lpr_drm_aux_cache_busy);
    request_marker = GPUD_DRM_REQUEST_BOUND_AUX;
    gpud_drm_ioctl_request_t *ioctl = lpr_gpud_drm_payload(page);
    memset(ioctl, 0, sizeof(*ioctl)); ioctl->aux_size = 5284;
    assert(!lpr_gpud_drm_call_transfer(GPUD_DRM_OP_HANDLE_IOCTL, 90, page,
        sizeof(*ioctl), NULL, NULL, aux_fd, 1));
    int saved_maps = maps, saved_unmaps = unmaps, saved_creates = vmo_creates;
    memset(mapping, 0xa5, 5284);
    lpr_drm_aux_destroy(aux_fd, mapping, bytes);
    assert(!lpr_drm_aux_cache_busy && unmaps == saved_unmaps && closed[91] == 1);
    assert(!lpr_drm_aux_create(7276, 1, &aux_fd, &mapping, &bytes));
    for (int i = 0; i < 7276; ++i) assert(auxiliary[i] == 0);
    assert(maps == saved_maps && vmo_creates == saved_creates);
    /* A concurrent owner, a temporary control page, and an oversized payload
     * all keep the exact-size temporary path; the cache budget is not a limit. */
    pacha_fd_t tmp_fd; void *tmp; uint64_t tmp_bytes;
    assert(!lpr_drm_aux_create(5284, 1, &tmp_fd, &tmp, &tmp_bytes));
    assert(tmp_fd == 92 && tmp_bytes == 8192 && !allocation_flags);
    lpr_drm_aux_destroy(tmp_fd, tmp, tmp_bytes);
    lpr_drm_aux_destroy(aux_fd, mapping, bytes);
    assert(!lpr_drm_aux_create(5284, 0, &tmp_fd, &tmp, &tmp_bytes));
    assert(tmp_fd == 92 && tmp_bytes == 8192);
    lpr_drm_aux_destroy(tmp_fd, tmp, tmp_bytes);
    assert(!lpr_drm_aux_create(sizeof(auxiliary) + 1, 1, &tmp_fd, &tmp, &tmp_bytes));
    assert(tmp_fd == 92 && tmp_bytes == sizeof(auxiliary) + 4096);
    lpr_drm_aux_destroy(tmp_fd, tmp, tmp_bytes);
    saved_unmaps = unmaps; before = closed[91];
    lpr_drm_after_fork_child();
    assert(!lpr_drm_aux_cache && !lpr_drm_aux_cache_fd && !lpr_drm_aux_cache_busy);
    assert(unmaps == saved_unmaps + 1 && closed[91] == before);

    /* Mesa poisons ignored BO fields and sends several IB/fence/sync chunks. */
    gpud_drm_amdgpu_cs_ib_t ib={.va_start=0x10000,.ib_bytes=64};
    gpud_drm_amdgpu_bo_entry_t bo={.handle=9};
    gpud_drm_amdgpu_bo_list_in_t list={.operation=UINT32_MAX,.list_handle=UINT32_MAX,
        .bo_number=1,.bo_info_size=8,.bo_info_ptr=(uintptr_t)&bo};
    uint32_t sync=5, fence[2]={9,0};
    gpud_drm_amdgpu_cs_chunk_t chunks[]={
        {6,6,(uintptr_t)&list},{5,1,(uintptr_t)&sync},{2,2,(uintptr_t)fence},
        {1,8,(uintptr_t)&ib},{1,8,(uintptr_t)&ib}};
    uint64_t pointers[5];
    for(unsigned i=0;i<5;++i) pointers[i]=(uintptr_t)&chunks[i];
    gpud_drm_amdgpu_cs_t cs={.ctx_id=7,.num_chunks=5,.chunks=(uintptr_t)pointers};
    gpud_drm_ioctl_request_t request={0};
    assert(!lpr_drm_prepare_amdgpu_cs((uintptr_t)&cs,0,&request,&tmp_fd,&tmp,&tmp_bytes, 0));
    gpud_drm_amdgpu_cs_wire_t wire;
    memcpy(&wire,request.data,sizeof(wire));
    assert(wire.chunk_count==5 && request.aux_size==84);
    assert(wire.parts[3].offset==20 && wire.parts[4].offset==52);
    lpr_drm_aux_destroy(tmp_fd,tmp,tmp_bytes);
    chunks[1].length_dw=2; /* A sem array is valid, not just a single handle. */
    uint32_t sems[2]={5,6}; chunks[1].chunk_data=(uintptr_t)sems;
    assert(!lpr_drm_prepare_amdgpu_cs((uintptr_t)&cs,0,&request,&tmp_fd,&tmp,&tmp_bytes, 0));
    lpr_drm_aux_destroy(tmp_fd,tmp,tmp_bytes);
    chunks[1].chunk_id=99;
    assert(lpr_drm_prepare_amdgpu_cs((uintptr_t)&cs,0,&request,&tmp_fd,&tmp,&tmp_bytes, 0)==-LPR_LINUX_EOPNOTSUPP);
    chunks[1].chunk_id=5; ib.pad=1;
    assert(lpr_drm_prepare_amdgpu_cs((uintptr_t)&cs,0,&request,&tmp_fd,&tmp,&tmp_bytes, 0)==-LPR_LINUX_EINVAL);
    assert(tmp_fd==-1 && !tmp && !tmp_bytes);
    kms_query_marshalling();
    {
        /* Older libdrm owns only the original prefix; ASan must catch any
         * accidental read of the newer num_entries/padding tail. */
        struct { uint32_t handle, op; uint64_t value; } prefix = {
            7, 0, UINT64_C(0x12345000)
        };
        _Static_assert(sizeof(prefix) == 16, "original Linux GEM_OP prefix");
        gpud_drm_ioctl_request_t operation = {0};
        uint64_t pointer = 0, extent = 0;
        pacha_fd_t view_fd = -1; void *view = NULL;
        assert(!lpr_drm_prepare_amdgpu_gem_op(GPUD_DRM_IOCTL_AMDGPU_GEM_OP,
            (uintptr_t)&prefix, 1, &operation, &pointer,
            &view_fd, &view, &extent));
        assert(pointer == prefix.value && operation.arg_size == sizeof(prefix));
        lpr_drm_aux_destroy(view_fd, view, extent);
    }
    for (unsigned extended = 0; extended < 2; ++extended) {
        gpud_drm_amdgpu_gem_op_t user = {.handle = 7, .op = 0,
            .value = UINT64_C(0x12345000)};
        uint64_t pointer = 0;
        gpud_drm_ioctl_request_t operation = {0};
        pacha_fd_t view_fd = -1; void *view = NULL; uint64_t extent = 0;
        const uint32_t command = extended ? GPUD_DRM_IOCTL_AMDGPU_GEM_OP_EXT :
            GPUD_DRM_IOCTL_AMDGPU_GEM_OP;
        assert(!lpr_drm_prepare_amdgpu_gem_op(command, (uintptr_t)&user, 1,
            &operation, &pointer, &view_fd, &view, &extent));
        gpud_drm_amdgpu_gem_op_t wire;
        memcpy(&wire, operation.data, sizeof(wire));
        assert(pointer == user.value && !wire.value && wire.handle == 7 &&
            operation.aux_size == 32 && operation.data_size == 24 &&
            operation.arg_size == (extended ? 24u : 16u));
        lpr_drm_aux_destroy(view_fd, view, extent);
        operation = (gpud_drm_ioctl_request_t){0};
        view_fd = -1; view = NULL; extent = pointer = 0;
        user.op = 1; user.value = 4;
        assert(!lpr_drm_prepare_amdgpu_gem_op(command, (uintptr_t)&user, 1,
            &operation, &pointer, &view_fd, &view, &extent));
        memcpy(&wire, operation.data, sizeof(wire));
        assert(wire.value == 4 && !pointer && !operation.aux_size && !view);
        user.op = 0; user.value = 0;
        assert(lpr_drm_prepare_amdgpu_gem_op(command, (uintptr_t)&user, 1,
            &operation, &pointer, &view_fd, &view, &extent) == -LPR_LINUX_EFAULT);
        user.op = 2;
        assert(lpr_drm_prepare_amdgpu_gem_op(command, (uintptr_t)&user, 1,
            &operation, &pointer, &view_fd, &view, &extent) == -LPR_LINUX_EOPNOTSUPP);
    }
    puts("LPR GEM_OP: both native sizes, pointer stripping, scalar placement and null rejection PASS");
    puts("LPR Mesa CS: multiple IBs, ignored BO fields, sync/fence, rejects and cleanup PASS");
    puts("LPR page connection: one bind, no VMO per call, loss/no replay, fork, errors PASS");
    puts("LPR auxiliary reuse: zeroing, bounded retention, temporary fallback, fork and errors PASS");
}

/* Request preparation and reply rendering are fixture phases within one
 * synchronous syscall. Their transaction tags never become caller FDs. */
int64_t lpr_pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b)
{
    assert(nr != PACHAOS_SYSCALL_IPC_CALL);
    return fixture_prepare_request(nr, a, b);
}
int64_t lpr_pacha_syscall4(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    return fixture_finish_request(nr, a, b, c, d);
}
int64_t lpr_ipc_message(uint64_t nr, uint64_t endpoint,
    uint64_t request, uint64_t reply, uint64_t timeout, uint64_t flags, int *sent)
{
    if (nr != PACHA_IPC_SYSCALL_CALL) return test_lpr_ipc_noncall(nr, endpoint, request, reply, timeout, flags, sent);

    assert(nr == PACHAOS_SYSCALL_IPC_CALL && request && reply);
    assert(timeout == UINT64_MAX && flags == PACHA_WAIT_UNINTERRUPTIBLE);
    *sent = 0;
    int64_t transaction = fixture_prepare_request(nr, endpoint, request);
    if (transaction < 16) return transaction;
    *sent = 1;
    int64_t status = fixture_finish_request(PACHAOS_SYSCALL_IPC_RECV_WAIT,
        (uint64_t)transaction, reply, timeout, flags);

    return status;
}
