/* SPDX-License-Identifier: MIT */
#include "drm_service.h"
#include "drm_control.h"
#include "drm_reply.h"
#include "../kobox2_adapter/gpu_event_message.h"
#include "../kobox2_adapter/gpu_query_message.h"

#include <errno.h>
#include <pacha/ipc.h>
#include <pacha/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(GPUD_DRM_RPC_PROFILE) && GPUD_DRM_RPC_PROFILE
static uint64_t rpc_profile_now(void) {
    uint32_t low, high;
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high) :: "memory");
    return ((uint64_t)high << 32) | low;
}

static void rpc_profile_record(uint64_t start, uint64_t map_cycles,
    uint64_t dispatch_cycles, uint64_t unmap_cycles, int mapped, int inlined,
    uint64_t key) {
    /* The service has one dispatch owner. Diagnostic-only cumulative counters
     * distinguish mapping churn from backend waits without per-request logs.
     * Cycle totals include descheduling and must not be called CPU time. */
    static uint64_t calls, maps, inline_calls, total, map_total, dispatch_total, unmap_total;
    static struct { uint64_t key, calls, cycles, dispatch; } commands[64];
    static uint64_t overflow_calls, overflow_cycles;
    const uint64_t elapsed = rpc_profile_now() - start;
    ++calls;
    maps += mapped != 0;
    inline_calls += inlined != 0;
    total += elapsed;
    map_total += map_cycles;
    dispatch_total += dispatch_cycles;
    unmap_total += unmap_cycles;
    unsigned slot;
    for (slot = 0; slot < 64; ++slot) {
        if (commands[slot].calls && commands[slot].key != key) continue;
        commands[slot].key = key;
        ++commands[slot].calls;
        commands[slot].cycles += elapsed;
        commands[slot].dispatch += dispatch_cycles;
        break;
    }
    if (slot == 64) { ++overflow_calls; overflow_cycles += elapsed; }
    if (calls % 4096 == 0 && calls <= 262144) {
        printf("GPUD_RPC_PROFILE principal=%llu calls=%llu maps=%llu inline=%llu "
            "cycles=%llu map_cycles=%llu dispatch_cycles=%llu unmap_cycles=%llu\n",
            (unsigned long long)pacha_syscall0(PACHA_RUNTIME_SYSCALL_GETPID),
            (unsigned long long)calls, (unsigned long long)maps,
            (unsigned long long)inline_calls, (unsigned long long)total,
            (unsigned long long)map_total, (unsigned long long)dispatch_total,
            (unsigned long long)unmap_total);
        /* Only numeric operation/ioctl identifiers are retained, never caller
         * buffers. Explicit overflow and top-four totals prevent a truncated
         * diagnostic table being mistaken for complete command attribution. */
        uint64_t selected = 0;
        for (unsigned rank = 0; rank < 4; ++rank) {
            unsigned best = 64;
            for (unsigned i = 0; i < 64; ++i)
                if (!(selected & (UINT64_C(1) << i)) && commands[i].calls &&
                    (best == 64 || commands[i].cycles > commands[best].cycles))
                    best = i;
            if (best == 64) break;
            selected |= UINT64_C(1) << best;
            printf("GPUD_RPC_COMMAND through=%llu key=%llx calls=%llu cycles=%llu dispatch_cycles=%llu\n",
                (unsigned long long)calls, (unsigned long long)commands[best].key,
                (unsigned long long)commands[best].calls,
                (unsigned long long)commands[best].cycles,
                (unsigned long long)commands[best].dispatch);
        }
        printf("GPUD_RPC_OVERFLOW through=%llu calls=%llu cycles=%llu\n",
            (unsigned long long)calls, (unsigned long long)overflow_calls,
            (unsigned long long)overflow_cycles);
    }
}
#else
static inline uint64_t rpc_profile_now(void) { return 0; }
static inline void rpc_profile_record(uint64_t start, uint64_t map_cycles,
    uint64_t dispatch_cycles, uint64_t unmap_cycles, int mapped, int inlined,
    uint64_t key) {
    (void)start; (void)map_cycles; (void)dispatch_cycles; (void)unmap_cycles;
    (void)mapped; (void)inlined; (void)key;
}
#endif

enum {
    GPUD_LINUX_POLLIN = 0x0001u,
    GPUD_LINUX_POLLRDNORM = 0x0040u,
};

static int valid_fd(uint64_t fd, uint64_t kind, uint64_t rights, uint64_t size) {
    struct pacha_fd_info info = {0};
    return fd >= 16 && fd < PACHA_FD_TABLE_LIMIT &&
        !pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, fd, (uintptr_t)&info) &&
        info.kind == kind && (info.rights & rights) == rights && (!size || info.size == size);
}

static int valid_aux_fd(const struct pacha_ipc_fd *fd, uint64_t size) {
    const uint64_t rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ |
        PACHA_FD_RIGHT_MAP_WRITE;
    struct pacha_fd_info info = {0};
    return fd && fd->fd >= 16 && fd->fd < PACHA_FD_TABLE_LIMIT &&
        fd->rights == rights && !fd->flags && !fd->transfer_flags &&
        !pacha_syscall2(PACHA_FD_SYSCALL_GET_INFO, fd->fd, (uintptr_t)&info) &&
        info.kind == PACHA_FD_KIND_VMO && info.rights == rights &&
        !info.flags && info.size == size;
}

static int wait_fence(struct gpud_drm_service *service, int fd) {
    for (;;) {
        int error = gpud_drm_service_pump_events(service);
        if (error)
            return error;
        struct pacha_pollfd events[2] = {
            {.fd = fd, .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
            {.fd = service->gpu.ipc->fd,
             .events = PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP},
        };
        /* The producer's completion arrives through this same service.
         * Keep draining it while waiting for an imported input fence. */
        long result = pacha_syscall4(PACHA_FD_SYSCALL_WAIT_MANY,
            (uintptr_t)events, 2, UINT64_MAX, 0);
        if (result > 0 && (events[0].revents & PACHA_FD_EVENT_READABLE))
            return 0;
        if (result > 0 && (events[0].revents & PACHA_FD_EVENT_HANGUP))
            return -EPIPE;
        if (result > 0 && (events[1].revents & PACHA_FD_EVENT_READABLE))
            continue;
        if (result > 0 && (events[1].revents & PACHA_FD_EVENT_HANGUP))
            return -EPIPE;
        if (result != PACHA_SYSCALL_ERR_NOT_READY &&
            result != -PACHA_SYSCALL_ERR_NOT_READY)
            return -EIO;
    }
}

struct gpud_drm_pending_fence {
    struct gpud_drm_pending_fence *next;
    uint64_t session, correlation;
    int fd;
};

static int retire_fence(struct gpud_drm_service *service,
    struct gpud_drm_pending_fence *entry, int signal) {
    struct gpud_drm_pending_fence **link = &service->fences;
    while (*link && *link != entry)
        link = &(*link)->next;
    if (!*link)
        return -EPROTO;
    if (signal) {
        const struct pacha_ipc_msg message = {0};
        int result = pacha_ipc_send(entry->fd, &message);
        /* Closing an unused sync-file before the GPU finishes is normal. */
        if (result && result != -PACHA_SYSCALL_ERR_CLOSED)
            return -EIO;
    }
    if (pacha_fd_close(entry->fd))
        return -EIO;
    *link = entry->next;
    free(entry);
    return 0;
}

static int complete_fences(struct gpud_drm_service *service,
    const unsigned char *data, size_t size) {
    if (size % PH_GPU_FENCE_RECORD_BYTES)
        return -EPROTO;
    for (size_t offset = 0; offset < size; offset += PH_GPU_FENCE_RECORD_BYTES) {
        const unsigned char *record = data + offset;
        uint64_t session = ph_gpu_event_load_u64(record);
        uint64_t correlation = ph_gpu_event_load_u64(record + 8);
        int32_t status = (int32_t)ph_gpu_event_load_u32(record + 16);
        if (!session || !correlation || !status || status > 1 || status < -4095 ||
            ph_gpu_event_load_u32(record + 20))
            return -EPROTO;
        struct gpud_drm_pending_fence *entry = service->fences;
        while (entry && (entry->session != session || entry->correlation != correlation))
            entry = entry->next;
        if (!entry)
            return -EPROTO;
        /* Failure closes the producer without a success message. Pollers see
         * hangup/error, not a falsely signaled successful GPU completion. */
        int error = retire_fence(service, entry, status == 1);
        if (error)
            return error;
    }
    return 0;
}

int gpud_drm_service_bind(struct gpud_drm_service *service, int endpoint_fd) {
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL;
    if (!service || service->endpoint_fd || !service->files.generation ||
        endpoint_fd < 16 || endpoint_fd >= PACHA_FD_TABLE_LIMIT)
        return -EINVAL;
    if (!valid_fd(endpoint_fd, PACHA_FD_KIND_ENDPOINT, rights, 0))
        return -EACCES;
    /* Reserve capability metadata for the bounded object/lease pools plus
     * control and in-flight transfers. Otherwise the larger mapping ledger
     * would merely move exhaustion to the default 256-entry native FD table. */
    struct pacha_fd_table_info capacity;
    const uint64_t needed = GPUD_DRM_MAPPINGS_MAX + GPUD_DRM_PRIMES_MAX +
        GPUD_DRM_OBJECT_LEASES_MAX + GPUD_DRM_REFERENCES_MAX + 64;
    if (pacha_fd_table(needed, &capacity) != 0 || capacity.capacity < needed)
        return -ENOMEM;
    service->endpoint_fd = endpoint_fd;
    return 0;
}

static struct gpud_drm_watch *empty_watch(struct gpud_drm_service *service) {
    for (size_t i = 0; i < GPUD_DRM_REFERENCES_MAX; ++i)
        if (!service->watches[i].handle)
            return &service->watches[i];
    return NULL;
}

static struct gpud_drm_watch *find_watch(
    struct gpud_drm_service *service, uint64_t handle) {
    for (size_t i = 0; i < GPUD_DRM_REFERENCES_MAX; ++i)
        if (service->watches[i].handle == handle)
            return &service->watches[i];
    return NULL;
}

static const struct gpud_drm_file *find_file(
    const struct gpud_drm_service *service, uint64_t handle) {
    for (size_t i = 0; i < service->files.limit; ++i)
        if (service->files.files[i].handle == handle)
            return &service->files.files[i];
    return NULL;
}

static struct gpud_drm_file *find_file_by_session(
    struct gpud_drm_service *service, uint64_t session, size_t *index_out) {
    if (!session)
        return NULL;
    for (size_t i = 0; i < service->files.limit; ++i) {
        struct gpud_drm_file *file = &service->files.files[i];
        if (file->session != session || file->state == GPUD_DRM_FILE_FREE)
            continue;
        if (index_out)
            *index_out = i;
        return file;
    }
    return NULL;
}

static struct gpud_drm_event_buffer *event_buffer(
    struct gpud_drm_service *service, uint64_t handle) {
    for (size_t i = 0; i < service->files.limit; ++i)
        if (service->files.files[i].handle == handle)
            return &service->events[i];
    return NULL;
}

static int signal_event_hint(struct gpud_drm_service *service,
    uint64_t handle) {
    for (size_t i = 0; i < GPUD_DRM_REFERENCES_MAX; ++i) {
        struct gpud_drm_watch *watch = &service->watches[i];
        if (watch->handle != handle || watch->transferred || watch->fd < 16)
            continue;
        const struct pacha_ipc_msg message = {0};
        int status = pacha_ipc_send(watch->fd, &message);
        /* A queued hint already makes every duplicate of the LPR endpoint
         * readable, so channel backpressure is successful coalescing. */
        if (!status || status == PACHA_ERR_ALLOC)
            return 0;
        return -EIO;
    }
    return -ENOENT;
}

static int validate_event_records(const unsigned char *data, size_t bytes) {
    size_t offset = 0;
    while (offset < bytes) {
        uint32_t header[2];
        if (bytes - offset < sizeof(header))
            return -EPROTO;
        memcpy(header, data + offset, sizeof(header));
        if (header[1] < sizeof(header) || header[1] > bytes - offset)
            return -EPROTO;
        offset += header[1];
    }
    return 0;
}

int gpud_drm_service_pump_events(struct gpud_drm_service *service) {
    if (!service || service->error || service->files.terminal_error)
        return service ? (service->error ? service->error :
            service->files.terminal_error) : -EINVAL;
    unsigned char bytes[GPUD_GPU_CHANNEL_PAGE];
    for (;;) {
        size_t size = 0;
        int received = gpud_gpu_rpc_next_event(
            &service->gpu, bytes, sizeof(bytes), &size);
        if (received <= 0)
            return received < 0 ? (service->error = received) : 0;
        struct ph_gpu_drm_event_message event;
        if (ph_gpu_drm_event_decode(bytes, size, service->files.generation,
                &event) || event.sequence <= service->event_sequence ||
            (!event.fences && validate_event_records(event.data, event.data_size)))
            return service->error = gpud_drm_files_fault(
                &service->files, service->files.generation, -EPROTO);
        service->event_sequence = event.sequence;
        if (event.fences) {
            int error = complete_fences(service, event.data, event.data_size);
            if (error)
                return service->error = gpud_drm_files_fault(
                    &service->files, service->files.generation, error);
            continue;
        }
        size_t index = 0;
        struct gpud_drm_file *file = find_file_by_session(
            service, event.session_id, &index);
        if (!file || file->state != GPUD_DRM_FILE_OPEN || !file->references ||
            !event.data_size)
            continue;
        struct gpud_drm_event_buffer *buffer = &service->events[index];
        if (buffer->handle != file->handle) {
            memset(buffer, 0, sizeof(*buffer));
            buffer->handle = file->handle;
        }
        if (event.data_size > sizeof(buffer->data) - buffer->bytes)
            return service->error = gpud_drm_files_fault(
                &service->files, service->files.generation, -ENOSPC);
        const int was_empty = !buffer->bytes;
        memcpy(buffer->data + buffer->bytes, event.data, event.data_size);
        buffer->bytes += event.data_size;
        if (was_empty) {
            int error = signal_event_hint(service, file->handle);
            if (error)
                return service->error = gpud_drm_files_fault(
                    &service->files, service->files.generation, error);
        }
    }
}

struct gpud_drm_reply_transfer {
    struct pacha_ipc_fd fds[2];
    struct gpud_drm_object_lease *lease;
    uint64_t object_id;
    int owner_fd, client_fd;
    size_t count;
    unsigned int prime_owner;
};

static struct gpud_drm_mapping *find_mapping(
    struct gpud_drm_service *service, uint64_t id) {
    if (id)
        for (size_t i = 0; i < GPUD_DRM_MAPPINGS_MAX; ++i)
            if (service->mappings[i].id == id)
                return &service->mappings[i];
    return NULL;
}

static struct gpud_drm_mapping *empty_mapping(
    struct gpud_drm_service *service) {
    for (size_t i = 0; i < GPUD_DRM_MAPPINGS_MAX; ++i)
        if (!service->mappings[i].id)
            return &service->mappings[i];
    return NULL;
}

static struct gpud_drm_prime *find_prime(
    struct gpud_drm_service *service, uint64_t token) {
    if (token)
        for (size_t i = 0; i < GPUD_DRM_PRIMES_MAX; ++i)
            if (service->primes[i].token == token)
                return &service->primes[i];
    return NULL;
}

static struct gpud_drm_prime *empty_prime(struct gpud_drm_service *service) {
    for (size_t i = 0; i < GPUD_DRM_PRIMES_MAX; ++i)
        if (!service->primes[i].token)
            return &service->primes[i];
    return NULL;
}

static struct gpud_drm_object_lease *empty_object_lease(
    struct gpud_drm_service *service) {
    for (size_t i = 0; i < GPUD_DRM_OBJECT_LEASES_MAX; ++i)
        if (!service->object_leases[i].object_id)
            return &service->object_leases[i];
    size_t unique = 0, closed = 0, mappings = 0, primes = 0;
    for (size_t i = 0; i < GPUD_DRM_OBJECT_LEASES_MAX; ++i) {
        const uint64_t id = service->object_leases[i].object_id;
        size_t first = 0;
        while (first < i && service->object_leases[first].object_id != id) ++first;
        if (first != i) continue;
        ++unique;
        const struct gpud_drm_mapping *mapping = find_mapping(service, id);
        const struct gpud_drm_prime *prime = find_prime(service, id);
        mappings += mapping != NULL;
        primes += prime != NULL;
        closed += (mapping && mapping->owner_closed) || (prime && prime->owner_closed);
    }
    printf("[gpud] object lease capacity limit=%u unique=%zu mappings=%zu primes=%zu owner_closed=%zu\n",
        GPUD_DRM_OBJECT_LEASES_MAX, unique, mappings, primes, closed);
    return NULL;
}

static int object_has_leases(
    const struct gpud_drm_service *service, uint64_t id) {
    for (size_t i = 0; i < GPUD_DRM_OBJECT_LEASES_MAX; ++i)
        if (service->object_leases[i].object_id == id)
            return 1;
    return 0;
}

/* Failure-only accounting: view bytes can alias and are not physical usage. */
static void report_retained_objects(const struct gpud_drm_service *service)
{
    size_t mappings = 0, closed_mappings = 0, primes = 0, closed_primes = 0;
    size_t leases = 0, owner_leases = 0, watches = 0;
    uint64_t mapping_bytes = 0, prime_bytes = 0;
    for (size_t i = 0; i < GPUD_DRM_MAPPINGS_MAX; ++i) {
        const struct gpud_drm_mapping *mapping = &service->mappings[i];
        if (!mapping->id) continue;
        ++mappings;
        closed_mappings += !!mapping->owner_closed;
        mapping_bytes += mapping->length;
    }
    for (size_t i = 0; i < GPUD_DRM_PRIMES_MAX; ++i) {
        const struct gpud_drm_prime *prime = &service->primes[i];
        if (!prime->token) continue;
        ++primes;
        closed_primes += !!prime->owner_closed;
        prime_bytes += prime->length;
    }
    for (size_t i = 0; i < GPUD_DRM_OBJECT_LEASES_MAX; ++i) {
        if (!service->object_leases[i].object_id) continue;
        ++leases;
        owner_leases += !!service->object_leases[i].prime_owner;
    }
    for (size_t i = 0; i < GPUD_DRM_REFERENCES_MAX; ++i)
        watches += !!service->watches[i].handle;
    printf("[gpud] retained views mappings=%zu closed=%zu bytes=%llu "
        "primes=%zu closed=%zu bytes=%llu leases=%zu owners=%zu watches=%zu\n",
        mappings, closed_mappings, (unsigned long long)mapping_bytes,
        primes, closed_primes, (unsigned long long)prime_bytes,
        leases, owner_leases, watches);
}

static uint64_t mapping_root_rights(uint32_t rights) {
    uint64_t native = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_REVOKE;
    if (rights & KB2_GPU_SPAN_RIGHT_READ)
        native |= PACHA_FD_RIGHT_MAP_READ;
    if (rights & KB2_GPU_SPAN_RIGHT_WRITE)
        native |= PACHA_FD_RIGHT_MAP_WRITE;
    return native;
}

static int adopt_mapping(struct gpud_drm_service *service,
    uint64_t handle, uint64_t correlation,
    const struct gpud_drm_translation *translation) {
    struct gpud_drm_mapping *slot = empty_mapping(service);
    struct pacha_ipc_fd received = {0};
    if (!slot)
        return -ENOSPC;
    if (!translation->mapping_id || !translation->mapping_exchange ||
        !translation->mapping_length || find_mapping(service, translation->mapping_id))
        return -EPROTO;
    int error = gpud_gpu_rpc_take_mapping(&service->gpu, correlation,
        translation->mapping_exchange, &received);
    if (error)
        return error;
    const uint64_t rights = mapping_root_rights(translation->mapping_rights);
    struct pacha_fd_info info = {0};
    int valid = received.fd >= 16 && received.fd < PACHA_FD_TABLE_LIMIT &&
        received.rights == rights && received.flags == PACHA_FD_FLAG_CLOEXEC &&
        !received.transfer_flags &&
        !pacha_fd_get_info((int)received.fd, &info) &&
        info.kind == PACHA_FD_KIND_VMO && info.size == translation->mapping_length &&
        info.rights == rights && info.flags == PACHA_FD_FLAG_CLOEXEC;
    if (!valid) {
        if (received.fd >= 16 && received.fd < PACHA_FD_TABLE_LIMIT)
            (void)pacha_fd_close((int)received.fd);
        return -EPROTO;
    }
    *slot = (struct gpud_drm_mapping){
        .id = translation->mapping_id,
        .exchange = translation->mapping_exchange,
        .handle = handle,
        .length = translation->mapping_length,
        .gem_handle = translation->mapping_handle,
        .rights = translation->mapping_rights,
        .cache_policy = translation->mapping_cache_policy,
        .view_fd = (int)received.fd,
    };
    const size_t occupied_prefix = (size_t)(slot - service->mappings) + 1;
    if (occupied_prefix > service->mapping_high_water) {
        service->mapping_high_water = occupied_prefix;
        if (occupied_prefix == 65 || occupied_prefix == 129)
            printf("[gpud] drm mapping high-water=%zu limit=%u\n",
                occupied_prefix, GPUD_DRM_MAPPINGS_MAX);
    }
    return 0;
}

static int release_mapping(struct gpud_drm_service *service,
    struct gpud_drm_mapping *mapping) {
    if (!mapping || !mapping->id || mapping->view_fd < 16 ||
        service->correlation == UINT64_MAX)
        return -EINVAL;
    int error = pacha_vmo_revoke(mapping->view_fd);
    if (!error) {
        /* Revoke removes every capability for this VMO, including view_fd. */
        mapping->view_fd = -1;
        error = gpud_gpu_rpc_release_mapping(&service->gpu,
            ++service->correlation, mapping->id);
    }
    if (!error)
        memset(mapping, 0, sizeof(*mapping));
    return error;
}

static int release_prime(struct gpud_drm_service *service,
    struct gpud_drm_prime *prime) {
    if (!prime || !prime->token || prime->view_fd < 16 ||
        service->correlation == UINT64_MAX)
        return -EINVAL;
    int error = pacha_vmo_revoke(prime->view_fd);
    if (!error) {
        prime->view_fd = -1;
        error = gpud_gpu_rpc_release_mapping(&service->gpu,
            ++service->correlation, prime->token);
    }
    if (!error)
        memset(prime, 0, sizeof(*prime));
    return error;
}

static int release_closed_objects(struct gpud_drm_service *service) {
    for (size_t i = 0; i < GPUD_DRM_MAPPINGS_MAX; ++i) {
        struct gpud_drm_mapping *mapping = &service->mappings[i];
        if (!mapping->id || !mapping->owner_closed ||
            object_has_leases(service, mapping->id))
            continue;
        int error = release_mapping(service, mapping);
        if (error)
            return gpud_drm_files_fault(
                &service->files, service->files.generation, error);
    }
    for (size_t i = 0; i < GPUD_DRM_PRIMES_MAX; ++i) {
        struct gpud_drm_prime *prime = &service->primes[i];
        if (!prime->token || !prime->owner_closed ||
            object_has_leases(service, prime->token))
            continue;
        int error = release_prime(service, prime);
        if (error)
            return gpud_drm_files_fault(
                &service->files, service->files.generation, error);
    }
    return 0;
}

static void close_mapping_owner(
    struct gpud_drm_service *service, uint64_t handle, uint32_t gem_handle) {
    for (size_t i = 0; i < GPUD_DRM_MAPPINGS_MAX; ++i)
        if (service->mappings[i].handle == handle &&
            (!gem_handle || service->mappings[i].gem_handle == gem_handle))
            service->mappings[i].owner_closed = 1;
}

static int retire_owner_watch(struct gpud_drm_service *service, uint64_t handle) {
    for (size_t i = 0; i < GPUD_DRM_REFERENCES_MAX; ++i) {
        struct gpud_drm_watch *watch = &service->watches[i];
        if (watch->handle != handle || watch->transferred)
            continue;
        if (pacha_fd_close(watch->fd))
            return gpud_drm_files_fault(&service->files, service->files.generation, -EIO);
        memset(watch, 0, sizeof(*watch));
        return 0;
    }
    return 0;
}

static int control(struct gpud_drm_service *service, struct gpud_drm_control *pending,
    size_t size, uint64_t *handle) {
    unsigned char reply[GPUD_GPU_CHANNEL_PAGE];
    size_t reply_size;
    int error = gpud_gpu_rpc_call(&service->gpu, GPUD_GPU_QUEUE_CONTROL,
        service->gpu.mapping + GPUD_GPU_CONTROL_REQUEST_OFFSET, size,
        reply, sizeof(reply), &reply_size);
    if (error)
        return gpud_drm_files_fault(&service->files, service->files.generation, error);
    if (service->gpu.attachment.fd_count)
        return gpud_drm_files_fault(
            &service->files, service->files.generation, -EPROTO);
    return gpud_drm_control_complete(&service->files, pending, reply, reply_size, handle);
}

static int close_draining(struct gpud_drm_service *service) {
    for (;;) {
        struct gpud_drm_control pending = {0};
        size_t size;
        if (service->correlation == UINT64_MAX)
            return -EOVERFLOW;
        int result = gpud_drm_close_prepare(&service->files, service->files.generation,
            ++service->correlation, &pending,
            service->gpu.mapping + GPUD_GPU_CONTROL_REQUEST_OFFSET, 2048, &size);
        if (result != 1)
            return result;
        uint64_t handle = 0;
        result = control(service, &pending, size, &handle);
        if (result)
            return result;
        for (size_t i = 0; i < GPUD_DRM_REFERENCES_MAX; ++i) {
            if (service->watches[i].handle != handle)
                continue;
            if (pacha_fd_close(service->watches[i].fd))
                return -EIO;
            memset(&service->watches[i], 0, sizeof(service->watches[i]));
        }
    }
}

static int ioctl_request(
    struct gpud_drm_service *service, gpud_drm_ioctl_request_t *request) {
    struct gpud_drm_binding binding;
    uint64_t generation = service->files.generation;
    int error = gpud_drm_file_acquire(
        &service->files, generation, request->handle, &binding);
    if (error)
        return error;
    struct gpud_drm_translation translation;
    error = gpud_drm_ioctl_encode(&translation, &binding, request, PH_GPU_QUERY_OUTPUT_REGION);
    /* A MAP ioctl names the existing GEM object's mmap offset. Repeated
     * queries must not create additional pins/slots. The validated request
     * has no ancillary FDs, and a closed/reused GEM handle never matches. */
    if (!error && translation.mapping_handle) {
        for (size_t i = 0; i < GPUD_DRM_MAPPINGS_MAX; ++i) {
            const struct gpud_drm_mapping *mapping = &service->mappings[i];
            if (!mapping->id || mapping->owner_closed ||
                mapping->handle != request->handle ||
                mapping->gem_handle != translation.mapping_handle)
                continue;
            const size_t offset = request->request == GPUD_DRM_IOCTL_MODE_MAP_DUMB ?
                offsetof(gpud_drm_mode_map_dumb_t, offset) : 0;
            memcpy(request->data + offset, &mapping->id, sizeof(mapping->id));
            return gpud_drm_file_release(&service->files, generation, request->handle);
        }
    }
    size_t fd_index = 1 + !!request->aux_size;
    int input_fence = -1, output_fence = -1;
    size_t output_fence_index = 0;
    struct gpud_drm_pending_fence *pending_fence = NULL;
    if (!error && (request->fd_flags & GPUD_DRM_IOCTL_FD_INPUT_WAIT)) {
        input_fence = service->received.fds[fd_index++].fd;
        if (!valid_fd(input_fence, PACHA_FD_KIND_CHANNEL,
                PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL |
                    PACHA_FD_RIGHT_CLOSE, 0))
            error = -EACCES;
    }
    if (!error && (request->fd_flags & GPUD_DRM_IOCTL_FD_OUTPUT_NOTIFY)) {
        output_fence_index = fd_index;
        output_fence = service->received.fds[fd_index++].fd;
        if (!valid_fd(output_fence, PACHA_FD_KIND_CHANNEL,
                PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_CLOSE, 0))
            error = -EACCES;
    }
    if (!error && input_fence >= 16)
        error = wait_fence(service, input_fence);
    void *aux = NULL;
    size_t aux_mapping_size = 0;
    int temporary_aux = 0;
    if (!error && request->aux_size) {
        aux_mapping_size = (request->aux_size + GPUD_GPU_CHANNEL_PAGE - 1) &
            ~(size_t)(GPUD_GPU_CHANNEL_PAGE - 1);
        const struct pacha_ipc_fd *received = &service->received.fds[1];
        uint64_t aux_fd = received->fd;
        if (request->aux_size > GPUD_GPU_AUX_CAPACITY ||
            aux_mapping_size < request->aux_size) {
            error = -EACCES;
        } else if (service->request_aux) {
            if (aux_mapping_size > GPUD_DRM_AUX_REUSE_BYTES || received->fd != PH_IPC_NO_FD)
                error = -EACCES;
            else aux = service->request_aux;
        } else if (!valid_aux_fd(received, aux_mapping_size)) {
            error = -EACCES;
        } else {
            long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, aux_fd, 0,
                aux_mapping_size, PACHA_PROT_READ | PACHA_PROT_WRITE,
                PACHA_MMAP_SHARED, 0);
            if (address < GPUD_GPU_CHANNEL_PAGE)
                error = -ENOMEM;
            else {
                aux = (void *)(uintptr_t)address;
                temporary_aux = 1;
            }
        }
        if (!error) {
            if (translation.region.region_id != PH_GPU_QUERY_OUTPUT_REGION ||
                translation.region.length != request->aux_size) {
                error = -EPROTO;
            } else if (translation.region.rights == KB2_GPU_SPAN_RIGHT_READ) {
                memcpy(service->gpu.mapping + GPUD_GPU_AUX_OFFSET, aux, request->aux_size);
            } else if (translation.region.rights == KB2_GPU_SPAN_RIGHT_WRITE) {
                /* Output-only ioctls may write fewer bytes than requested. */
                memset(service->gpu.mapping + GPUD_GPU_AUX_OFFSET, 0, request->aux_size);
            } else error = -EPROTO;
        }
    }
    if (!error && translation.staged_input_size) {
        if (request->aux_size ||
            translation.region.region_id != PH_GPU_QUERY_OUTPUT_REGION ||
            translation.region.rights != KB2_GPU_SPAN_RIGHT_READ ||
            translation.region.length != translation.staged_input_size ||
            translation.staged_input_size > GPUD_GPU_AUX_CAPACITY) {
            error = -EPROTO;
        } else {
            memcpy(service->gpu.mapping + GPUD_GPU_AUX_OFFSET,
                translation.staged_input, translation.staged_input_size);
        }
    }
    if (!error && output_fence >= 16) {
        pending_fence = calloc(1, sizeof(*pending_fence));
        if (!pending_fence) {
            error = -ENOMEM;
        } else {
            *pending_fence = (struct gpud_drm_pending_fence) {
                .next = service->fences, .session = binding.session_id,
                .correlation = service->correlation, .fd = output_fence,
            };
            service->fences = pending_fence;
            service->received.fds[output_fence_index].fd = PH_IPC_NO_FD;
        }
    }
    if (!error) {
        unsigned char bytes[KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + GPUD_DRM_COMMAND_BYTES];
        unsigned char reply[GPUD_GPU_CHANNEL_PAGE], output[GPUD_GPU_CHANNEL_PAGE];
        size_t size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + translation.command_size, reply_size;
        kb2_protocol_message_envelope_t envelope = {.protocol_id = KB2_GPU_PROTOCOL_ID,
            .opcode = KB2_GPU_OPCODE_COMMAND, .generation = generation,
            .correlation_id = service->correlation, .payload_length = translation.command_size};
        error = kb2_protocol_message_envelope_encode(bytes, size, &envelope) ? -EPROTO : 0;
        if (!error) {
            memcpy(bytes + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                translation.command, translation.command_size);
            error = gpud_gpu_rpc_call(&service->gpu,
                translation.queue_class == KB2_GPU_QUEUE_DISPLAY ?
                    GPUD_GPU_QUEUE_DISPLAY : GPUD_GPU_QUEUE_EXECUTION,
                bytes, size, reply, sizeof(reply), &reply_size);
            if (error) {
                printf("[gpud] drm transport-fault generation=%llu correlation=%llu "
                    "session=%llu handle=%llu ioctl=0x%llx set=%u command=%u "
                    "queue=%u aux=%llu status=%d creates=%llu closes=%llu "
                    "submits=%llu\n",
                    (unsigned long long)generation,
                    (unsigned long long)service->correlation,
                    (unsigned long long)binding.session_id,
                    (unsigned long long)request->handle,
                    (unsigned long long)request->request,
                    translation.command_set_id,
                    translation.command_id,
                    translation.queue_class,
                    (unsigned long long)request->aux_size,
                    error,
                    (unsigned long long)service->resource_creates,
                    (unsigned long long)service->gem_closes,
                    (unsigned long long)service->exec_submits);
                gpud_drm_files_fault(&service->files, generation, error);
            } else {
                memcpy(output, service->gpu.mapping + GPUD_GPU_OUTPUT_OFFSET, sizeof(output));
                error = gpud_drm_ioctl_reply(request, &translation, service->correlation,
                    reply, reply_size, output, sizeof(output));
                if (error && request->request == GPUD_DRM_IOCTL_VIRTGPU_RESOURCE_CREATE) {
                    printf("[gpud] drm resource-create-failed handle=%llu status=%d creates=%llu closes=%llu\n",
                        (unsigned long long)request->handle, error,
                        (unsigned long long)service->resource_creates,
                        (unsigned long long)service->gem_closes);
                    report_retained_objects(service);
                }
                if (error && request->request == GPUD_DRM_IOCTL_MODE_SETCRTC &&
                    request->data_size == sizeof(gpud_drm_kms_crtc_wire_t)) {
                    gpud_drm_kms_crtc_wire_t wire;
                    memcpy(&wire, request->data, sizeof(wire));
                    printf("[gpud] drm modeset-failed generation=%llu "
                        "handle=%llu session=%llu crtc=%u fb=%u "
                        "mode=%u connectors=%u connector0=%u status=%d\n",
                        (unsigned long long)generation,
                        (unsigned long long)request->handle,
                        (unsigned long long)binding.session_id,
                        wire.value.crtc_id, wire.value.fb_id,
                        wire.value.mode_valid, wire.value.count_connectors,
                        wire.value.count_connectors ? wire.connectors[0] : 0,
                        error);
                }
                if (!error && request->request == GPUD_DRM_IOCTL_VIRTGPU_RESOURCE_CREATE)
                    ++service->resource_creates;
                else if (!error && request->request == GPUD_DRM_IOCTL_GEM_CLOSE) {
                    ++service->gem_closes;
                    uint32_t gem_handle;
                    memcpy(&gem_handle, request->data, sizeof(gem_handle));
                    /* Existing VMAs retain their lease after GEM_CLOSE;
                     * only unleased mappings can release the backend pin. */
                    close_mapping_owner(service, request->handle, gem_handle);
                    error = release_closed_objects(service);
                }
                else if (!error &&
                    request->request == GPUD_DRM_IOCTL_VIRTGPU_EXECBUFFER)
                    ++service->exec_submits;
                if (!error && request->request == GPUD_DRM_IOCTL_VIRTGPU_EXECBUFFER) {
                    struct gpud_drm_watch *watch = find_watch(
                        service, request->handle);
                    if (watch && !watch->submit_reported) {
                        watch->submit_reported = 1;
                        printf("[gpud] drm submit generation=%llu node=%llu "
                            "handle=%llu session=%llu\n",
                            (unsigned long long)generation,
                            (unsigned long long)watch->device_minor,
                            (unsigned long long)request->handle,
                            (unsigned long long)binding.session_id);
                    }
                }
                if (!error && request->request == GPUD_DRM_IOCTL_MODE_SETCRTC &&
                    request->data_size == sizeof(gpud_drm_kms_crtc_wire_t)) {
                    gpud_drm_kms_crtc_wire_t wire;
                    memcpy(&wire, request->data, sizeof(wire));
                    struct gpud_drm_watch *watch = find_watch(
                        service, request->handle);
                    if (watch && !watch->modeset_reported && wire.value.fb_id &&
                        wire.value.mode_valid && wire.value.count_connectors) {
                        watch->modeset_reported = 1;
                        printf("[gpud] drm modeset generation=%llu node=%llu "
                            "handle=%llu session=%llu crtc=%u fb=%u connectors=%u\n",
                            (unsigned long long)generation,
                            (unsigned long long)watch->device_minor,
                            (unsigned long long)request->handle,
                            (unsigned long long)binding.session_id,
                            wire.value.crtc_id, wire.value.fb_id,
                            wire.value.count_connectors);
                    }
                }
                if (!error && translation.mapping_id) {
                    error = adopt_mapping(service, request->handle,
                        service->correlation, &translation);
                } else if (!error && service->gpu.attachment.fd_count)
                    error = -EPROTO;
                if (!error && request->aux_size &&
                    translation.region.rights == KB2_GPU_SPAN_RIGHT_WRITE)
                    memcpy(aux, service->gpu.mapping + GPUD_GPU_AUX_OFFSET,
                        request->aux_size);
                if (error == -EPROTO)
                    gpud_drm_files_fault(&service->files, generation, error);
            }
        }
    }
    if (error && pending_fence) {
        int retired = retire_fence(service, pending_fence, 0);
        if (retired)
            error = gpud_drm_files_fault(&service->files, generation, retired);
    }
    if (temporary_aux && aux && pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP,
            (uintptr_t)aux, aux_mapping_size)) {
        error = -EIO;
        gpud_drm_files_fault(&service->files, generation, error);
    }
    if (request->request == GPUD_DRM_IOCTL_MODE_DIRTYFB &&
        request->data_size == sizeof(gpud_drm_mode_fb_dirty_t)) {
        gpud_drm_mode_fb_dirty_t dirty;
        struct gpud_drm_watch *watch = find_watch(service, request->handle);
        memcpy(&dirty, request->data, sizeof(dirty));
        unsigned int *reported = dirty.num_clips ?
            (watch ? &watch->dirty_update_reported : NULL) :
            (watch ? &watch->dirty_probe_reported : NULL);
        if (reported && !*reported) {
            *reported = 1;
            printf("[gpud] drm dirtyfb generation=%llu node=%llu "
                "handle=%llu session=%llu clips=%u status=%d\n",
                (unsigned long long)generation,
                (unsigned long long)watch->device_minor,
                (unsigned long long)request->handle,
                (unsigned long long)binding.session_id,
                dirty.num_clips, error);
        }
    }
    int release = gpud_drm_file_release(&service->files, generation, request->handle);
    if (error && (request->request == GPUD_DRM_IOCTL_VIRTGPU_MAP ||
            request->request == GPUD_DRM_IOCTL_MODE_MAP_DUMB)) {
        size_t used = 0, closed = 0;
        uint64_t bytes = 0;
        for (size_t i = 0; i < GPUD_DRM_MAPPINGS_MAX; ++i) {
            const struct gpud_drm_mapping *mapping = &service->mappings[i];
            if (!mapping->id) continue;
            ++used;
            closed += !!mapping->owner_closed;
            bytes += mapping->length;
        }
        printf("[gpud] drm map-failed handle=%llu ioctl=0x%llx status=%d "
            "mappings=%zu limit=%u owner_closed=%zu bytes=%llu\n",
            (unsigned long long)request->handle,
            (unsigned long long)request->request, error, used,
            GPUD_DRM_MAPPINGS_MAX, closed, (unsigned long long)bytes);
    }
    return release ? release : error;
}

static int event_request(struct gpud_drm_service *service, uint32_t op,
    void *payload, uint64_t *result) {
    gpud_drm_read_request_t *read = payload;
    gpud_drm_handle_request_t *poll = payload;
    uint64_t handle = op == GPUD_DRM_OP_HANDLE_READ ? read->handle : poll->handle;
    uint64_t generation = service->files.generation;
    struct gpud_drm_binding binding;
    int error = gpud_drm_file_acquire(&service->files, generation, handle, &binding);
    if (error)
        return error;
    struct gpud_drm_event_buffer *buffer = event_buffer(service, handle);
    if (!buffer || (buffer->handle && buffer->handle != handle)) {
        error = -EPROTO;
    } else if (op == GPUD_DRM_OP_HANDLE_POLL) {
        *result = buffer->bytes ? poll->arg0 &
            (GPUD_LINUX_POLLIN | GPUD_LINUX_POLLRDNORM) : 0;
    } else {
        size_t copied = 0;
        while (copied < buffer->bytes) {
            uint32_t header[2];
            memcpy(header, buffer->data + copied, sizeof(header));
            if (header[1] > read->capacity - copied)
                break;
            copied += header[1];
        }
        if (!copied && buffer->bytes) {
            error = -EMSGSIZE;
        } else {
            read->data_size = copied;
            if (copied)
                memcpy(read->data, buffer->data, copied);
            buffer->bytes -= copied;
            if (buffer->bytes)
                memmove(buffer->data, buffer->data + copied, buffer->bytes);
            if (buffer->bytes)
                error = signal_event_hint(service, handle);
        }
    }
    int release = gpud_drm_file_release(&service->files, generation, handle);
    if (error == -EPROTO)
        gpud_drm_files_fault(&service->files, generation, error);
    return release ? release : error;
}

static int execute_prime_command(struct gpud_drm_service *service,
    const struct gpud_drm_translation *translation,
    unsigned char *reply, size_t reply_capacity, size_t *reply_size) {
    unsigned char bytes[
        KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE + GPUD_DRM_COMMAND_BYTES];
    const size_t size = KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE +
        translation->command_size;
    const kb2_protocol_message_envelope_t envelope = {
        .protocol_id = KB2_GPU_PROTOCOL_ID,
        .opcode = KB2_GPU_OPCODE_COMMAND,
        .generation = translation->generation,
        .correlation_id = service->correlation,
        .payload_length = translation->command_size,
    };
    if (kb2_protocol_message_envelope_encode(bytes, size, &envelope))
        return -EPROTO;
    memcpy(bytes + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
        translation->command, translation->command_size);
    return gpud_gpu_rpc_call(&service->gpu, GPUD_GPU_QUEUE_EXECUTION,
        bytes, size, reply, reply_capacity, reply_size);
}

static int adopt_prime(struct gpud_drm_service *service, uint64_t token,
    struct gpud_drm_reply_transfer *transfer, uint64_t *result) {
    struct gpud_drm_prime *slot = empty_prime(service);
    struct pacha_ipc_fd received = {0};
    if (!slot)
        return -ENOSPC;
    if (!token || find_prime(service, token) || find_mapping(service, token))
        return -EPROTO;
    struct gpud_drm_object_lease *lease = empty_object_lease(service);
    if (!lease)
        return -EMFILE;
    int error = gpud_gpu_rpc_take_dma_buf(
        &service->gpu, service->correlation, token, &received);
    if (error)
        return error;
    const uint32_t span_rights =
        KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE;
    const uint64_t root_rights = mapping_root_rights(span_rights);
    struct pacha_fd_info info = {0};
    const int valid = received.fd >= 16 && received.fd < PACHA_FD_TABLE_LIMIT &&
        received.rights == root_rights &&
        received.flags == PACHA_FD_FLAG_CLOEXEC && !received.transfer_flags &&
        !pacha_fd_get_info((int)received.fd, &info) &&
        info.kind == PACHA_FD_KIND_VMO && info.size && !(info.size & 4095u) &&
        info.rights == root_rights && info.flags == PACHA_FD_FLAG_CLOEXEC;
    if (!valid) {
        if (received.fd >= 16 && received.fd < PACHA_FD_TABLE_LIMIT)
            (void)pacha_fd_close((int)received.fd);
        return -EPROTO;
    }
    *slot = (struct gpud_drm_prime) {
        .token = token,
        .length = info.size,
        .view_fd = (int)received.fd,
    };
    struct pacha_ipc_channel_pair pair = {.a = -1, .b = -1};
    error = pacha_ipc_channel_create(&pair,
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_CLOSE |
            PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL,
        PACHA_FD_FLAG_CLOEXEC);
    if (error) {
        int released = release_prime(service, slot);
        return released ? released : error;
    }
    transfer->fds[0] = (struct pacha_ipc_fd) {
        .fd = received.fd,
        /* A Linux dma-buf is an FD-transferable object.  Keep revoke and
         * inspect authority in gpud, but let LPR duplicate the narrowed view
         * when the Linux application sends the dma-buf through SCM_RIGHTS. */
        .rights = PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ |
            PACHA_FD_RIGHT_MAP_WRITE,
        .transfer_flags = PACHA_IPC_TRANSFER_CLOEXEC,
    };
    transfer->object_id = token;
    transfer->fds[1] = (struct pacha_ipc_fd) {
        .fd = (uint64_t)(uint32_t)pair.b,
        .rights = PACHA_FD_RIGHT_CLOSE,
        .transfer_flags = PACHA_IPC_TRANSFER_MOVE |
            PACHA_IPC_TRANSFER_CLOEXEC | PACHA_IPC_TRANSFER_INHERIT,
    };
    transfer->lease = lease;
    transfer->owner_fd = pair.a;
    transfer->client_fd = pair.b;
    transfer->prime_owner = 1;
    transfer->count = 2;
    *result = token;
    return 0;
}

static int prime_export_request(struct gpud_drm_service *service,
    const gpud_drm_prime_export_request_t *request,
    struct gpud_drm_reply_transfer *transfer, uint64_t *result) {
    if (!request->gem_handle ||
        (request->flags & ~(GPUD_DRM_CLOEXEC | GPUD_DRM_RDWR)))
        return -EINVAL;
    if (!empty_prime(service)) {
        printf("[gpud] drm prime-capacity handle=%llu gem=%u limit=%u\n",
            (unsigned long long)request->handle, request->gem_handle,
            GPUD_DRM_PRIMES_MAX);
        return -ENOSPC;
    }
    if (!empty_object_lease(service))
        return -EMFILE;
    const uint64_t generation = service->files.generation;
    struct gpud_drm_binding binding;
    int error = gpud_drm_file_acquire(
        &service->files, generation, request->handle, &binding);
    if (error)
        return error;
    struct gpud_drm_translation translation;
    error = gpud_drm_prime_export_encode(&translation, &binding,
        request->gem_handle, request->flags);
    if (!error) {
        unsigned char reply[GPUD_GPU_CHANNEL_PAGE];
        size_t reply_size;
        error = execute_prime_command(service, &translation,
            reply, sizeof(reply), &reply_size);
        uint64_t token = 0;
        if (!error)
            error = gpud_drm_prime_export_reply(&translation,
                request->gem_handle, request->flags, service->correlation,
                reply, reply_size, &token);
        if (!error)
            error = adopt_prime(service, token, transfer, result);
        if (error)
            printf("[gpud] drm prime-export-failed handle=%llu gem=%u status=%d\n",
                (unsigned long long)request->handle, request->gem_handle, error);
        if (error == -EPROTO)
            gpud_drm_files_fault(&service->files, generation, error);
    }
    int release = gpud_drm_file_release(
        &service->files, generation, request->handle);
    return release ? release : error;
}

static int prime_import_request(struct gpud_drm_service *service,
    const gpud_drm_prime_import_request_t *request, uint64_t *result) {
    if (!request->token || request->size || request->flags ||
        request->reserved0 || !find_prime(service, request->token))
        return -EINVAL;
    const uint64_t generation = service->files.generation;
    struct gpud_drm_binding binding;
    int error = gpud_drm_file_acquire(
        &service->files, generation, request->handle, &binding);
    if (error)
        return error;
    struct gpud_drm_translation translation;
    error = gpud_drm_prime_import_encode(
        &translation, &binding, request->token);
    if (!error) {
        unsigned char reply[GPUD_GPU_CHANNEL_PAGE];
        size_t reply_size;
        error = execute_prime_command(service, &translation,
            reply, sizeof(reply), &reply_size);
        uint32_t handle = 0;
        if (!error && service->gpu.attachment.fd_count)
            error = -EPROTO;
        if (!error)
            error = gpud_drm_prime_import_reply(&translation,
                request->token, service->correlation,
                reply, reply_size, &handle);
        if (!error)
            *result = handle;
        if (error == -EPROTO)
            gpud_drm_files_fault(&service->files, generation, error);
    }
    int release = gpud_drm_file_release(
        &service->files, generation, request->handle);
    return release ? release : error;
}

static int prime_release_request(struct gpud_drm_service *service,
    uint64_t token) {
    struct gpud_drm_prime *prime = find_prime(service, token);
    if (!prime || prime->owner_closed)
        return -ENOENT;
    prime->owner_closed = 1;
    return release_closed_objects(service);
}

static int prime_acquire_request(struct gpud_drm_service *service,
    uint64_t token) {
    struct gpud_drm_prime *prime = find_prime(service, token);
    const uint64_t fd = service->received.fds[1].fd;
    if (!prime || (prime->owner_closed && !object_has_leases(service, token)) ||
        !valid_fd(fd, PACHA_FD_KIND_CHANNEL,
            PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_WAIT |
                PACHA_FD_RIGHT_POLL, 0))
        return -EINVAL;
    struct gpud_drm_object_lease *lease = empty_object_lease(service);
    if (!lease)
        return -EMFILE;
    *lease = (struct gpud_drm_object_lease) {
        .object_id = token,
        .fd = (int)fd,
    };
    service->received.fds[1].fd = PH_IPC_NO_FD;
    return 0;
}

static int mmap_request(struct gpud_drm_service *service,
    const gpud_drm_mmap_request_t *request,
    struct gpud_drm_reply_transfer *transfer, uint64_t *result) {
    const uint64_t known_prot = PACHA_PROT_READ | PACHA_PROT_WRITE;
    const uint64_t known_flags = PACHA_MMAP_FIXED |
        PACHA_MMAP_FIXED_NOREPLACE | PACHA_MMAP_SHARED |
        PACHA_MMAP_NORESERVE;
    struct gpud_drm_mapping *mapping = find_mapping(service, request->offset);
    const struct gpud_drm_file *file = find_file(service, request->handle);
    if (!mapping || mapping->handle != request->handle || mapping->owner_closed ||
        !file || file->state != GPUD_DRM_FILE_OPEN ||
        !request->length || request->length > mapping->length ||
        request->prot & ~known_prot || request->flags & ~known_flags ||
        !(request->flags & PACHA_MMAP_SHARED) ||
        ((request->prot & PACHA_PROT_READ) &&
            !(mapping->rights & KB2_GPU_SPAN_RIGHT_READ)) ||
        ((request->prot & PACHA_PROT_WRITE) &&
            !(mapping->rights & KB2_GPU_SPAN_RIGHT_WRITE)))
        return -EINVAL;
    struct gpud_drm_object_lease *lease = empty_object_lease(service);
    if (!lease)
        return -EMFILE;
    const uint64_t lease_rights = PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL;
    struct pacha_ipc_channel_pair pair = {.a = -1, .b = -1};
    int error = pacha_ipc_channel_create(
        &pair, lease_rights, PACHA_FD_FLAG_CLOEXEC);
    if (error)
        return error;
    uint64_t view_rights = PACHA_FD_RIGHT_CLOSE;
    if (mapping->rights & KB2_GPU_SPAN_RIGHT_READ)
        view_rights |= PACHA_FD_RIGHT_MAP_READ;
    if (mapping->rights & KB2_GPU_SPAN_RIGHT_WRITE)
        view_rights |= PACHA_FD_RIGHT_MAP_WRITE;
    transfer->fds[0] = (struct pacha_ipc_fd){
        .fd = (uint64_t)(uint32_t)mapping->view_fd,
        .rights = view_rights,
        .transfer_flags = PACHA_IPC_TRANSFER_CLOEXEC,
    };
    transfer->fds[1] = (struct pacha_ipc_fd){
        .fd = (uint64_t)(uint32_t)pair.b,
        .rights = PACHA_FD_RIGHT_CLOSE,
        .transfer_flags = PACHA_IPC_TRANSFER_MOVE |
            PACHA_IPC_TRANSFER_CLOEXEC | PACHA_IPC_TRANSFER_INHERIT,
    };
    transfer->lease = lease;
    transfer->object_id = mapping->id;
    transfer->owner_fd = pair.a;
    transfer->client_fd = pair.b;
    transfer->count = 2;
    *result = mapping->length;
    return 0;
}

static int cancel_reply_transfer(struct gpud_drm_service *service,
    struct gpud_drm_reply_transfer *transfer) {
    int error = 0;
    if (transfer->owner_fd >= 16)
        error = pacha_fd_close(transfer->owner_fd);
    if (transfer->client_fd >= 16) {
        int closed = pacha_fd_close(transfer->client_fd);
        if (!error)
            error = closed;
    }
    if (transfer->prime_owner && transfer->object_id) {
        struct gpud_drm_prime *prime = find_prime(
            service, transfer->object_id);
        if (prime) {
            int released = release_prime(service, prime);
            if (!error)
                error = released;
        }
    }
    *transfer = (struct gpud_drm_reply_transfer){
        .owner_fd = -1, .client_fd = -1};
    return error;
}

static void finish_reply_transfer(struct gpud_drm_reply_transfer *transfer) {
    if (transfer->lease)
        *transfer->lease = (struct gpud_drm_object_lease){
            .object_id = transfer->object_id,
            .fd = transfer->owner_fd,
            .prime_owner = transfer->prime_owner,
        };
    *transfer = (struct gpud_drm_reply_transfer){
        .owner_fd = -1, .client_fd = -1};
}

static int dispatch(struct gpud_drm_service *service,
    const pacha_service_envelope_t *header, void *payload, uint64_t *result,
    struct gpud_drm_reply_transfer *transfer, int inline_ioctl) {
    uint64_t generation = service->files.generation;
    size_t count = service->received.fd_count;
    if (service->files.terminal_error)
        return service->files.terminal_error;
    if (service->correlation == UINT64_MAX)
        return -EOVERFLOW;
    ++service->correlation;
    if (inline_ioctl && header->op != GPUD_DRM_OP_HANDLE_IOCTL)
        return -EINVAL;
    if (header->op == GPUD_DRM_OP_HELLO)
        return header->payload_size || count != 2 ? -EINVAL : (*result = 1, 0);
    if (header->op == GPUD_DRM_OP_OPEN_NODE) {
        const gpud_drm_open_request_t *request = payload;
        if (header->payload_size != sizeof(gpud_drm_open_request_t) || count != 3 ||
            !valid_fd(service->received.fds[1].fd, PACHA_FD_KIND_CHANNEL,
                PH_IPC_CHANNEL_RIGHTS | PACHA_FD_RIGHT_CLOSE, 0))
            return -EINVAL;
        struct gpud_drm_watch *watch = empty_watch(service);
        if (!watch)
            return -EMFILE;
        struct gpud_drm_control pending = {0};
        size_t size;
        int error = gpud_drm_open_prepare(&service->files, generation,
            service->backend_client, service->correlation, payload, &pending,
            service->gpu.mapping + GPUD_GPU_CONTROL_REQUEST_OFFSET, 2048, &size);
        if (error == -EMFILE) {
            size_t occupied = 0, open = 0;
            for (size_t i = 0; i < service->files.limit; ++i) {
                occupied += service->files.files[i].state != GPUD_DRM_FILE_FREE;
                open += service->files.files[i].state == GPUD_DRM_FILE_OPEN;
            }
            printf("[gpud] drm open capacity occupied=%zu open=%zu limit=%zu\n",
                occupied, open, service->files.limit);
        }
        if (!error)
            error = control(service, &pending, size, result);
        if (!error) {
            *watch = (struct gpud_drm_watch){.handle = *result,
                .device_minor = request->device_minor,
                .fd = (int)service->received.fds[1].fd};
            service->received.fds[1].fd = PH_IPC_NO_FD;
        }
        return error;
    }
    if (header->op == GPUD_DRM_OP_HANDLE_IOCTL) {
        const gpud_drm_ioctl_request_t *request = payload;
        size_t transferred = !!request->aux_size +
            !!(request->fd_flags & GPUD_DRM_IOCTL_FD_INPUT_WAIT) +
            !!(request->fd_flags & GPUD_DRM_IOCTL_FD_OUTPUT_NOTIFY);
        if (header->payload_size != sizeof(*request) ||
            (request->fd_flags & ~GPUD_DRM_IOCTL_FD_MASK) ||
            (inline_ioctl && !gpud_drm_ioctl_can_inline(request)) ||
            count != (inline_ioctl ? 1u : 2u) + transferred)
            return -EINVAL;
        return ioctl_request(service, payload);
    }
    if (header->op == GPUD_DRM_OP_HANDLE_MMAP) {
        const gpud_drm_mmap_request_t *request = payload;
        if (header->payload_size != sizeof(*request) || count != 2)
            return -EINVAL;
        return mmap_request(service, request, transfer, result);
    }
    if (header->op == GPUD_DRM_OP_HANDLE_POLL) {
        const gpud_drm_handle_request_t *request = payload;
        if (header->payload_size != sizeof(*request) || count != 2 ||
            request->arg0 > UINT32_MAX || request->arg1 || request->arg2)
            return -EINVAL;
        return event_request(service, header->op, payload, result);
    }
    if (header->op == GPUD_DRM_OP_HANDLE_READ) {
        const gpud_drm_read_request_t *request = payload;
        if (header->payload_size != sizeof(*request) || count != 2 ||
            !request->capacity || request->capacity > GPUD_DRM_EVENT_READ_BYTES ||
            request->data_size)
            return -EINVAL;
        return event_request(service, header->op, payload, result);
    }
    if (header->op == GPUD_DRM_OP_PRIME_EXPORT) {
        if (header->payload_size != sizeof(gpud_drm_prime_export_request_t) ||
            count != 2)
            return -EINVAL;
        return prime_export_request(service, payload, transfer, result);
    }
    if (header->op == GPUD_DRM_OP_PRIME_IMPORT) {
        const gpud_drm_prime_import_request_t *request = payload;
        if (header->payload_size != sizeof(*request))
            return -EINVAL;
        if (!request->token || count == 3)
            return -EOPNOTSUPP;
        if (count != 2)
            return -EINVAL;
        return prime_import_request(service, request, result);
    }
    if (header->op == GPUD_DRM_OP_PRIME_RELEASE ||
        header->op == GPUD_DRM_OP_PRIME_ACQUIRE) {
        const gpud_drm_prime_token_request_t *request = payload;
        const size_t expected = header->op == GPUD_DRM_OP_PRIME_ACQUIRE ? 3 : 2;
        if (header->payload_size != sizeof(*request) || !request->token ||
            count != expected)
            return -EINVAL;
        return header->op == GPUD_DRM_OP_PRIME_ACQUIRE ?
            prime_acquire_request(service, request->token) :
            prime_release_request(service, request->token);
    }
    if (header->op == GPUD_DRM_OP_HANDLE_CLOSE || header->op == GPUD_DRM_OP_HANDLE_DUP) {
        const gpud_drm_handle_request_t *request = payload;
        if (header->payload_size != sizeof(*request) || request->arg0 || request->arg1 || request->arg2)
            return -EINVAL;
        if (header->op == GPUD_DRM_OP_HANDLE_DUP) {
            if (count != 2 && count != 3)
                return -EOPNOTSUPP;
            struct gpud_drm_watch *watch = NULL;
            struct gpud_drm_watch *source = find_watch(
                service, request->handle);
            if (count == 3) {
                uint64_t fd = service->received.fds[1].fd;
                if (fd == service->received.fds[0].fd ||
                    fd == service->received.fds[2].fd ||
                    !valid_fd(fd, PACHA_FD_KIND_CHANNEL,
                        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL, 0))
                    return -EINVAL;
                watch = empty_watch(service);
                if (!watch)
                    return -EMFILE;
            }
            int error = gpud_drm_file_dup(
                &service->files, generation, request->handle);
            if (!error) {
                *result = request->handle;
                if (watch) {
                    *watch = (struct gpud_drm_watch){.handle = request->handle,
                        .device_minor = source ? source->device_minor : 0,
                        .fd = (int)service->received.fds[1].fd, .transferred = 1};
                    service->received.fds[1].fd = PH_IPC_NO_FD;
                }
            }
            return error;
        }
        if (count != 2)
            return -EOPNOTSUPP;
        int error = gpud_drm_file_close(
            &service->files, generation, request->handle);
        const struct gpud_drm_file *file =
            error ? NULL : find_file(service, request->handle);
        if (!error && file && file->state == GPUD_DRM_FILE_DRAINING) {
            struct gpud_drm_event_buffer *buffer =
                event_buffer(service, request->handle);
            if (buffer)
                memset(buffer, 0, sizeof(*buffer));
            close_mapping_owner(service, request->handle, 0);
        }
        if (!error)
            error = retire_owner_watch(service, request->handle);
        if (!error)
            error = close_draining(service);
        return error ? error : release_closed_objects(service);
    }
    return -EOPNOTSUPP;
}

size_t gpud_drm_service_pollfds(const struct gpud_drm_service *service,
    struct pacha_pollfd *fds, size_t capacity) {
    size_t count = 0;
#define ADD_SOURCE(source, mask) do { \
    if (fds && count < capacity) fds[count] = (struct pacha_pollfd){ \
        .fd = (source), .events = (mask)}; \
    ++count; \
} while (0)
    for (size_t i = 0; i < GPUD_DRM_REFERENCES_MAX; ++i)
        if (service->watches[i].handle && service->watches[i].fd >= 16) {
            ADD_SOURCE(service->watches[i].fd, PACHA_FD_EVENT_HANGUP);
        }
    for (size_t i = 0; i < GPUD_DRM_OBJECT_LEASES_MAX; ++i)
        if (service->object_leases[i].object_id &&
            service->object_leases[i].fd >= 16) {
            ADD_SOURCE(service->object_leases[i].fd, PACHA_FD_EVENT_HANGUP);
        }
    for (const struct gpud_drm_connection *c = service->connections; c; c = c->next) {
        ADD_SOURCE(c->fd, PACHA_FD_EVENT_READABLE | PACHA_FD_EVENT_HANGUP);
    }
#undef ADD_SOURCE
    return count;
}

int gpud_drm_service_reserve_pollfds(struct gpud_drm_service *service, size_t extra) {
    size_t count = gpud_drm_service_pollfds(service, NULL, 0);
    if (extra > SIZE_MAX - count) return -EOVERFLOW;
    count += extra;
    if (count <= service->poll_capacity) return 0;
    if (count > SIZE_MAX / (2 * sizeof(*service->pollfds))) return -EOVERFLOW;
    size_t capacity = count * 2;
    void *grown = realloc(service->pollfds, capacity * sizeof(*service->pollfds));
    if (!grown) return -ENOMEM;
    service->pollfds = grown;
    service->poll_capacity = capacity;
    return 0;
}

static int retire_connection(struct gpud_drm_connection **link) {
    struct gpud_drm_connection *connection = *link;
    /* Keep failed cleanup owned for terminal retirement, never forget a live
     * mapping or accidentally close a descriptor reused by a later client. */
    if (connection->aux) {
        if (pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP,
                (uintptr_t)connection->aux, GPUD_DRM_AUX_REUSE_BYTES)) return -EIO;
        connection->aux = NULL;
    }
    if (connection->page) {
        if (pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP,
                (uintptr_t)connection->page, GPUD_DRM_PAGE_BYTES)) return -EIO;
        connection->page = NULL;
    }
    if (pacha_fd_close(connection->fd)) return -EIO;
    *link = connection->next;
    free(connection);
    return 0;
}

int gpud_drm_service_reap_hangups(struct gpud_drm_service *service) {
    if (!service || service->error || service->files.terminal_error)
        return service ? (service->error ? service->error : service->files.terminal_error) : -EINVAL;
    /* This runs before every DRM request. Poll live references together:
     * the usual no-hangup case needs one syscall, not one per client/lease.
     * On readiness or error retain the individual checks below; closing a
     * file can retire other watches, so do not reuse a stale poll snapshot. */
    int reserve = gpud_drm_service_reserve_pollfds(service, 0);
    if (reserve) return service->error = reserve;
    const size_t count = gpud_drm_service_pollfds(
        service, service->pollfds, service->poll_capacity);
    if (!count)
        return 0;
    long polled = pacha_fd_wait_many_batched(service->pollfds, count, 0);
    if (polled < 0 && polled != PACHA_ERR_NOT_READY) return service->error = -EIO;
    size_t connection_count = 0;
    for (struct gpud_drm_connection *c = service->connections; c; c = c->next)
        ++connection_count;
    size_t index = count - connection_count;
    int hangup = 0;
    for (size_t i = 0; i < index; ++i)
        hangup |= (service->pollfds[i].revents & PACHA_FD_EVENT_HANGUP) != 0;
    struct gpud_drm_connection **link = &service->connections;
    while (*link) {
        const uint64_t events = service->pollfds[index++].revents;
        (*link)->ready = (events & PACHA_FD_EVENT_READABLE) != 0;
        if (events & PACHA_FD_EVENT_HANGUP) {
            int error = retire_connection(link);
            if (error) return service->error = error;
        } else link = &(*link)->next;
    }
    if (!hangup) return 0;
    for (size_t i = 0; i < GPUD_DRM_REFERENCES_MAX; ++i) {
        struct gpud_drm_watch *watch = &service->watches[i];
        if (!watch->handle || watch->fd < 16)
            continue;
        struct pacha_pollfd poll = {.fd = watch->fd, .events = PACHA_FD_EVENT_HANGUP};
        if (pacha_fd_poll(&poll, 1) <= 0 || !(poll.revents & PACHA_FD_EVENT_HANGUP))
            continue;
        uint64_t handle = watch->handle;
        if (pacha_fd_close(watch->fd))
            return service->error =
                gpud_drm_files_fault(&service->files, service->files.generation, -EIO);
        unsigned int transferred = watch->transferred;
        memset(watch, 0, sizeof(*watch));
        int error = gpud_drm_file_close(
            &service->files, service->files.generation, handle);
        const struct gpud_drm_file *file = error ? NULL : find_file(service, handle);
        int last = file && file->state == GPUD_DRM_FILE_DRAINING;
        if (!error && last) {
            struct gpud_drm_event_buffer *buffer = event_buffer(service, handle);
            if (buffer)
                memset(buffer, 0, sizeof(*buffer));
            close_mapping_owner(service, handle, 0);
        }
        if (!error)
            error = close_draining(service);
        if (!error)
            error = release_closed_objects(service);
        if (error)
            return service->error = error;
        if (!transferred && last && !find_file(service, handle))
            printf("[gpud] drm client-hangup handle=%llu generation=%llu backend-close=1\n",
                (unsigned long long)handle,
                (unsigned long long)service->files.generation);
    }
    for (size_t i = 0; i < GPUD_DRM_OBJECT_LEASES_MAX; ++i) {
        struct gpud_drm_object_lease *lease = &service->object_leases[i];
        if (!lease->object_id || lease->fd < 16)
            continue;
        struct pacha_pollfd poll = {
            .fd = lease->fd, .events = PACHA_FD_EVENT_HANGUP};
        if (pacha_fd_poll(&poll, 1) <= 0 ||
            !(poll.revents & PACHA_FD_EVENT_HANGUP))
            continue;
        uint64_t object_id = lease->object_id;
        unsigned int prime_owner = lease->prime_owner;
        int closed = pacha_fd_close(lease->fd);
        if (closed)
            return service->error = gpud_drm_files_fault(
                &service->files, service->files.generation, -EIO);
        memset(lease, 0, sizeof(*lease));
        struct gpud_drm_mapping *mapping = find_mapping(service, object_id);
        struct gpud_drm_prime *prime = find_prime(service, object_id);
        if (prime && prime_owner)
            prime->owner_closed = 1;
        if (!mapping && !prime)
            return service->error = gpud_drm_files_fault(
                &service->files, service->files.generation, -EPROTO);
        if (mapping && mapping->owner_closed &&
            !object_has_leases(service, object_id)) {
            int error = release_mapping(service, mapping);
            if (error)
                return service->error = gpud_drm_files_fault(
                    &service->files, service->files.generation, error);
        }
        if (prime && prime->owner_closed &&
            !object_has_leases(service, object_id)) {
            int error = release_prime(service, prime);
            if (error)
                return service->error = gpud_drm_files_fault(
                    &service->files, service->files.generation, error);
        }
    }
    return 0;
}

int gpud_drm_service_retire_mappings(struct gpud_drm_service *service) {
    if (!service)
        return -EINVAL;
    int first_error = 0;
    while (service->connections) {
        first_error = retire_connection(&service->connections);
        if (first_error) return first_error;
    }
    free(service->pollfds);
    service->pollfds = NULL;
    service->poll_capacity = 0;
    while (service->fences) {
        int error = retire_fence(service, service->fences, 0);
        if (error) {
            first_error = error;
            break;
        }
    }
    for (size_t i = 0; i < GPUD_DRM_OBJECT_LEASES_MAX; ++i) {
        struct gpud_drm_object_lease *lease = &service->object_leases[i];
        if (lease->fd >= 16) {
            int error = pacha_fd_close(lease->fd);
            if (error && !first_error)
                first_error = error;
        }
        memset(lease, 0, sizeof(*lease));
    }
    for (size_t i = 0; i < GPUD_DRM_MAPPINGS_MAX; ++i) {
        struct gpud_drm_mapping *mapping = &service->mappings[i];
        if (mapping->view_fd >= 16) {
            int error = pacha_vmo_revoke(mapping->view_fd);
            if (error && !first_error)
                first_error = error;
            /* A successful revoke already removed the caller's FD. */
            if (error) {
                int closed = pacha_fd_close(mapping->view_fd);
                if (closed && !first_error)
                    first_error = closed;
            }
        }
        memset(mapping, 0, sizeof(*mapping));
    }
    for (size_t i = 0; i < GPUD_DRM_PRIMES_MAX; ++i) {
        struct gpud_drm_prime *prime = &service->primes[i];
        if (prime->view_fd >= 16) {
            int error = pacha_vmo_revoke(prime->view_fd);
            if (error && !first_error)
                first_error = error;
            if (error) {
                int closed = pacha_fd_close(prime->view_fd);
                if (closed && !first_error)
                    first_error = closed;
            }
        }
        memset(prime, 0, sizeof(*prime));
    }
    int error = ph_ipc_packet_release(&service->gpu.attachment);
    if (error && !first_error)
        first_error = error;
    return first_error;
}

static int bind_request_page(struct gpud_drm_service *service,
    const struct pacha_ipc_msg *message) {
    const int auxiliary = message->word1 == GPUD_DRM_AUX_REUSE_BYTES;
    if ((message->word1 && !auxiliary) || message->word2 || message->word3 || message->flags ||
        message->fd_count != 3u + auxiliary ||
        !valid_fd(message->fds[0].fd, PACHA_FD_KIND_VMO,
            PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE,
            GPUD_DRM_PAGE_BYTES) ||
        !valid_fd(message->fds[1].fd, PACHA_FD_KIND_CHANNEL,
            PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_RECV | PACHA_FD_RIGHT_POLL |
                PACHA_FD_RIGHT_WAIT, 0) ||
        (auxiliary && !valid_aux_fd(&message->fds[2], GPUD_DRM_AUX_REUSE_BYTES)) ||
        !valid_fd(message->fds[2u + auxiliary].fd, PACHA_FD_KIND_REPLY,
            PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_SEND, 0)) return -EPROTO;
    struct gpud_drm_connection *connection = calloc(1, sizeof(*connection));
    int status = connection ? 0 : -ENOMEM;
    if (connection) {
        long address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, message->fds[0].fd,
            0, GPUD_DRM_PAGE_BYTES, PACHA_PROT_READ | PACHA_PROT_WRITE,
            PACHA_MMAP_SHARED, 0);
        if (address < 4096) {
            free(connection);
            status = -ENOMEM;
        } else {
            connection->page = (void *)(uintptr_t)address;
            connection->fd = (int)message->fds[1].fd;
            connection->next = service->connections;
            service->connections = connection;
            service->received.fds[1].fd = PH_IPC_NO_FD;
            if (auxiliary) {
                address = pacha_syscall6(PACHA_VM_SYSCALL_MMAP, message->fds[2].fd,
                    0, GPUD_DRM_AUX_REUSE_BYTES, PACHA_PROT_READ | PACHA_PROT_WRITE,
                    PACHA_MMAP_SHARED, 0);
                if (address < 4096) {
                    status = -ENOMEM;
                    /* Publish ownership before the second mapping so failed
                     * cleanup stays reachable for terminal teardown. */
                    int retired = retire_connection(&service->connections);
                    if (retired) service->files.terminal_error = retired;
                } else connection->aux = (void *)(uintptr_t)address;
            }
        }
    }
    const struct pacha_ipc_msg reply = {
        .word0 = GPUD_DRM_BIND_PAGE_REPLY_MAGIC, .word1 = (uint64_t)status};
    return pacha_ipc_reply((int)message->fds[2u + auxiliary].fd, &reply) ? -EIO : 0;
}

static int receive_request(struct gpud_drm_service *service, int endpoint_fd,
    struct gpud_drm_connection *connection) {
    void *bound_page = connection ? connection->page : NULL;
    if (!service || service->endpoint_fd < 16 || !service->backend_client)
        return -EINVAL;
    if (service->error)
        return service->error;
    struct pacha_ipc_msg message = {.fds = service->received.fds,
        .fd_capacity = PACHA_IPC_MAX_TRANSFER_FDS};
    long native = pacha_syscall2(
        PACHA_IPC_SYSCALL_RECV, endpoint_fd, (uintptr_t)&message);
    if (native)
        return native == PACHA_SYSCALL_ERR_EMPTY || native == PACHA_SYSCALL_ERR_NOT_READY ?
            -EAGAIN : (service->error = -EIO);
    service->received.fd_count = message.fd_count;
    const uint64_t profile_start = rpc_profile_now();
    uint64_t profile_map = 0, profile_dispatch = 0, profile_unmap = 0;
    int profile_mapped = 0;
    uint64_t profile_key = UINT64_MAX;
    int error = -EPROTO;
    struct gpud_drm_reply_transfer transfer = {
        .owner_fd = -1, .client_fd = -1};
    if (!bound_page && message.word0 == GPUD_DRM_BIND_PAGE_REQUEST_MAGIC) {
        error = bind_request_page(service, &message);
        goto cleanup;
    }
    if (message.word0 == GPUD_DRM_INLINE_IOCTL_REQUEST_MAGIC) {
        /* Only the kernel-created reply capability is accepted. In particular,
         * this path cannot smuggle auxiliary memory or fence capabilities. */
        if (bound_page || message.fd_count != 1 || message.flags ||
            !valid_fd(message.fds[0].fd, PACHA_FD_KIND_REPLY,
                PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_SEND, 0))
            goto cleanup;
        gpud_drm_ioctl_request_t request = {
            .handle = message.word1, .request = message.word2,
            .arg_size = (message.word2 >> 16) & 0x3fffu,
            .data_size = (message.word2 >> 16) & 0x3fffu,
        };
        memcpy(request.data, &message.word3, sizeof(message.word3));
        const pacha_service_envelope_t header = {
            .op = GPUD_DRM_OP_HANDLE_IOCTL, .payload_size = sizeof(request),
        };
        uint64_t result = 0;
        const uint64_t dispatch_start = rpc_profile_now();
#if defined(GPUD_DRM_RPC_PROFILE) && GPUD_DRM_RPC_PROFILE
        profile_key = ((uint64_t)GPUD_DRM_OP_HANDLE_IOCTL << 32) | (uint32_t)request.request;
#endif
        int status = dispatch(service, &header, &request, &result, &transfer, 1);
        profile_dispatch = rpc_profile_now() - dispatch_start;
        if (!status && !gpud_drm_ioctl_can_inline(&request))
            status = -EPROTO;
        struct pacha_ipc_msg reply = {
            .word0 = GPUD_DRM_INLINE_IOCTL_REPLY_MAGIC,
            .word1 = (uint64_t)status, .word2 = message.word2,
        };
        if (!status)
            memcpy(&reply.word3, request.data, request.data_size);
        error = pacha_ipc_reply((int)message.fds[0].fd, &reply) ? -EIO : 0;
        goto cleanup;
    }
    const int bound_aux = message.word1 == GPUD_DRM_REQUEST_BOUND_AUX;
    if (bound_aux && (!connection || !connection->aux)) goto cleanup;
    if (bound_page) {
        const size_t placeholders = 1u + bound_aux;
        if (!message.fd_count || message.fd_count > PACHA_IPC_MAX_TRANSFER_FDS - placeholders)
            goto cleanup;
        /* Preserve attachment indices for ioctl/fence validation. These slots
         * borrow connection-owned mappings, not untrusted numeric tokens. */
        memmove(message.fds + placeholders, message.fds, message.fd_count * sizeof(*message.fds));
        message.fds[0] = (struct pacha_ipc_fd){.fd = PH_IPC_NO_FD};
        if (bound_aux) message.fds[1] = (struct pacha_ipc_fd){.fd = PH_IPC_NO_FD};
        message.fd_count += placeholders;
        service->received.fd_count = message.fd_count;
        service->request_aux = bound_aux ? connection->aux : NULL;
    }
    if (message.fd_count < 2)
        goto cleanup;
    uint64_t page_fd = message.fds[0].fd, reply_fd = message.fds[message.fd_count - 1].fd;
    if ((!bound_page && !valid_fd(page_fd, PACHA_FD_KIND_VMO,
            PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE, GPUD_DRM_PAGE_BYTES)) ||
        !valid_fd(reply_fd, PACHA_FD_KIND_REPLY, PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_SEND, 0))
        goto cleanup;
    const uint64_t map_start = rpc_profile_now();
    long address = bound_page ? (long)(uintptr_t)bound_page :
        pacha_syscall6(PACHA_VM_SYSCALL_MMAP, page_fd, 0, GPUD_DRM_PAGE_BYTES,
            PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    profile_map = rpc_profile_now() - map_start;
    if (address < 4096)
        goto cleanup;
    service->page = (void *)(uintptr_t)address;
    profile_mapped = !bound_page;
    pacha_service_envelope_t header, response;
    union {
        gpud_drm_ioctl_request_t ioctl;
        gpud_drm_open_request_t open;
        gpud_drm_handle_request_t handle;
        gpud_drm_mmap_request_t mmap;
        gpud_drm_read_request_t read;
    } payload;
    memcpy(&header, service->page, sizeof(header));
    int status = -EINVAL;
    uint64_t result = 0;
    uint32_t response_size = 0;
    if (message.word0 == PACHA_SERVICE_REQUEST_MAGIC && !message.flags &&
        (!message.word1 || bound_aux) && !message.word2 &&
        message.word3 == header.request_id && pacha_service_request_is_valid(&header, GPUD_DRM_SERVICE_ID) &&
        header.flags == (header.payload_size ? PACHA_SERVICE_FLAG_PAGE_PAYLOAD : 0) &&
        !header.fd_count && !header.reserved0 && !header.reserved1 && header.payload_size <= sizeof(payload)) {
        memcpy(&payload, (unsigned char *)service->page + sizeof(header), header.payload_size);
        if (bound_aux && (header.op != GPUD_DRM_OP_HANDLE_IOCTL ||
                header.payload_size != sizeof(payload.ioctl) || !payload.ioctl.aux_size ||
                payload.ioctl.aux_size > GPUD_DRM_AUX_REUSE_BYTES)) goto cleanup;
#if defined(GPUD_DRM_RPC_PROFILE) && GPUD_DRM_RPC_PROFILE
        profile_key = (uint64_t)header.op << 32;
        if (header.op == GPUD_DRM_OP_HANDLE_IOCTL && header.payload_size == sizeof(payload.ioctl))
            profile_key |= (uint32_t)payload.ioctl.request;
#endif
        const uint64_t dispatch_start = rpc_profile_now();
        status = dispatch(service, &header, &payload, &result, &transfer, 0);
        profile_dispatch = rpc_profile_now() - dispatch_start;
        if (status && header.op == GPUD_DRM_OP_HANDLE_MMAP &&
                header.payload_size == sizeof(payload.mmap))
            printf("[gpud] drm mmap-failed handle=%llu offset=%llu length=%llu status=%d\n",
                (unsigned long long)payload.mmap.handle,
                (unsigned long long)payload.mmap.offset,
                (unsigned long long)payload.mmap.length, status);
        if (!status && (header.op == GPUD_DRM_OP_HANDLE_IOCTL ||
                header.op == GPUD_DRM_OP_HANDLE_READ)) {
            response_size = header.op == GPUD_DRM_OP_HANDLE_IOCTL ?
                sizeof(payload.ioctl) : sizeof(payload.read);
            memcpy((unsigned char *)service->page + sizeof(header), &payload, response_size);
        }
    }
    pacha_service_reply_init(&response, &header, status, PACHA_SERVICE_ERROR_GPUD_DRM,
        status ? 0 : result, response_size);
    memcpy(service->page, &response, sizeof(response));
    struct pacha_ipc_msg reply = {.word0 = PACHA_SERVICE_REPLY_MAGIC,
        .word1 = (uint64_t)status, .word2 = status ? 0 : result,
        .word3 = header.request_id,
        .fds = transfer.count ? transfer.fds : NULL,
        .fd_count = transfer.count};
    error = pacha_ipc_reply((int)reply_fd, &reply) ? -EIO : 0;
    if (!error && transfer.count)
        finish_reply_transfer(&transfer);
cleanup:
    service->request_aux = NULL;
    if (transfer.count) {
        int canceled = cancel_reply_transfer(service, &transfer);
        if (canceled)
            error = canceled;
    }
    if (bound_page) service->page = NULL;
    if (service->page) {
        const uint64_t unmap_start = rpc_profile_now();
        if (pacha_syscall2(PACHA_VM_SYSCALL_MUNMAP, (uintptr_t)service->page, GPUD_DRM_PAGE_BYTES))
            error = -EIO;
        else
            service->page = NULL;
        profile_unmap = rpc_profile_now() - unmap_start;
    }
    int release = ph_ipc_packet_release(&service->received);
    if (release)
        error = release;
    if (service->files.terminal_error)
        error = service->files.terminal_error;
    service->error = error;
    rpc_profile_record(profile_start, profile_map, profile_dispatch, profile_unmap,
        profile_mapped, message.word0 == GPUD_DRM_INLINE_IOCTL_REQUEST_MAGIC, profile_key);
    return error;
}

static int receive_connection(struct gpud_drm_service *service) {
    struct gpud_drm_connection **link = &service->connections;
    while (*link && !(*link)->ready) link = &(*link)->next;
    if (!*link) return -EAGAIN;
    struct gpud_drm_connection *connection = *link;
    connection->ready = 0;
    int result = receive_request(service, connection->fd, connection);
    /* Rotate live clients after one request so a busy producer cannot starve
     * another connection. The public endpoint also gets alternating priority. */
    *link = connection->next;
    while (*link) link = &(*link)->next;
    *link = connection;
    connection->next = NULL;
    return result;
}

int gpud_drm_service_receive(struct gpud_drm_service *service) {
    if (!service || service->error) return service ? service->error : -EINVAL;
    int prefer = service->prefer_connection;
    service->prefer_connection = !prefer;
    int result = prefer ? receive_connection(service) :
        receive_request(service, service->endpoint_fd, NULL);
    if (result != -EAGAIN) return result;
    return prefer ? receive_request(service, service->endpoint_fd, NULL) :
        receive_connection(service);
}
