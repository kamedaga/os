/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "../userland/kobox2_adapter/fs_queue.h"
#include "boot/module_launch.h"
#include <kobox2/virtqueue_x86_64.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>

static void *vmos[5];
static size_t vmo_bytes[5];
static struct ph_ipc_packet client_packets[2];
static atomic_uint client_sends;
static unsigned client_bound, client_released, client_closed, wait_calls;
static unsigned snapshot_polls, retained_replaces, retained_reuses;
static int many_clients;
static unsigned data_calls, owner_calls, sends, allocations;
static unsigned released;
static uint64_t routed_client;
struct ph_image ph_core = {.ram_size = 512u << 20};
static unsigned private_wakes;
static uint64_t blocked_sequence;
static unsigned mock_limit = 100, mock_outstanding;
static int client_receive_error[2];
static int client_readable[2]; /* WAIT_MANY reports this doorbell readable. */
static int client_latched[2]; /* The kernel notify latch of this doorbell. */
static kobox_linux_fs_publish_fn mock_publish;
static struct kobox_linux_fs_shared_ops mock_shared;
static uint64_t shared_extra = UINT64_C(1) | (UINT64_C(1) << 32);
static unsigned shared_queries, shared_revokes;
static int shared_revoke_failure;
static int shared_refresh_failure;
static struct ph_ipc_packet shared_attachment;
static int defer_collect;
static _Thread_local uint64_t notification_mask;
static _Thread_local unsigned lock_depth;
static pthread_mutex_t notice_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t notice_condition = PTHREAD_COND_INITIALIZER;
static int pause_notice, notice_entered, notice_release;
int ph_notifications_save(uint64_t *mask) {
    *mask = notification_mask; notification_mask = PH_NOTIFICATION_MASK; return 0;
}
int ph_notifications_restore(uint64_t mask) {
    assert(!lock_depth); notification_mask = mask; return 0;
}
void ph_lock(atomic_uint *lock) {
    assert(notification_mask == PH_NOTIFICATION_MASK && !lock_depth++);
    while (atomic_exchange_explicit(lock, 1, memory_order_acquire)) sched_yield();
}
void ph_unlock(atomic_uint *lock) {
    assert(lock_depth-- == 1 && atomic_exchange(lock, 0) == 1);
}
struct fake_work {
    struct fake_work *next;
    uint64_t client;
    int owner;
    void *input, *response, *cookie;
    size_t input_size, capacity, used;
    int error;
    int executing, completed;
};
static struct fake_work *mock_jobs;
#ifdef PH_FS_STORAGE_PROFILE
static unsigned profile_lines;
#endif

void *ph_alloc(size_t bytes) {
    void *memory = calloc(1, bytes);
    assert(memory); allocations++; return memory;
}
void ph_free(void *memory, size_t bytes) {
    (void)bytes; assert(allocations); allocations--; free(memory);
}

static long mock_wait_events(uint64_t a, uint64_t b, int probe) {
    assert(b && b <= 256);
    struct pacha_pollfd *events = (void *)(uintptr_t)a;
    assert(events[0].fd == 40);
    assert(b >= 2 && events[1].fd == 31);
    long ready = 0;
    for (size_t i = 2; i < b; ++i) {
        assert(many_clients ? events[i].fd == 100 : events[i].fd == 100 || events[i].fd == 101);
        events[i].revents = !many_clients && client_readable[events[i].fd - 100] ?
            PACHA_FD_EVENT_READABLE : 0;
        ready += events[i].revents != 0;
    }
    ++wait_calls;
    return ready ? ready : probe ? 0 : PACHA_SYSCALL_ERR_NOT_READY;
}

long pacha_syscall2(uint64_t op, uint64_t a, uint64_t b) {
    if (op == PACHA_FD_SYSCALL_POLL) {
        ++snapshot_polls;
        return mock_wait_events(a, b, 1);
    }
#ifdef PH_FS_STORAGE_PROFILE
    if (op == 1) {
        const char *line = (void *)(uintptr_t)a;
        assert(b == 121 && !memcmp(line, "[fs-queue-profile]", 18) && line[b-1]=='\n');
        for (unsigned column=0;column<6;++column) {
            assert(line[18+column*17]==' ');
            for(unsigned digit=0;digit<16;++digit)
                assert(strchr("0123456789abcdef",line[19+column*17+digit]));
        }
        ++profile_lines;return 0;
    }
#endif
    if (op == PACHA_FD_SYSCALL_GET_INFO) {
        if (a == 200) {
            struct pacha_fd_info *info = (void *)(uintptr_t)b;
            *info = (struct pacha_fd_info){.kind = PACHA_FD_KIND_VMO,
                .extra = shared_extra};
            ++shared_queries;
            return 0;
        }
        if (a == 100 || a == 101 || a == 30 || a == 31) {
            struct pacha_fd_info *info = (void *)(uintptr_t)b;
            *info = (struct pacha_fd_info){.kind = PACHA_FD_KIND_CHANNEL,
                .rights = PH_FS_CLIENT_DOORBELL_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC};
            return 0;
        }
        assert(a >= 20 && a <= 24);
        struct pacha_fd_info *info = (void *)(uintptr_t)b;
        *info = (struct pacha_fd_info){.kind = PACHA_FD_KIND_VMO,
            .rights = PH_FS_QUEUE_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC,
            .size = vmo_bytes[a - 20]};
    } else {
        assert(op == PACHA_VM_SYSCALL_MUNMAP);
        unsigned found = 0;
        for (unsigned i = 0; i < 5; ++i) found |= (void *)(uintptr_t)a == vmos[i];
        assert(found);
    }
    return 0;
}

long pacha_syscall1(uint64_t op, uint64_t fd) {
    assert(fd == 200 && (op == PACHA_FD_SYSCALL_CLOSE || op == PACHA_FD_SYSCALL_VMO_REVOKE));
    if (op == PACHA_FD_SYSCALL_VMO_REVOKE) {
        ++shared_revokes;
        return shared_revoke_failure ? PACHA_SYSCALL_ERR_INVALID : 0;
    }
    return 0;
}

long pacha_syscall5(uint64_t op, uint64_t parent, uint64_t pages,
    uint64_t count, uint64_t rights, uint64_t flags) {
    assert(op == PACHA_FD_SYSCALL_VMO_CREATE_PAGE_VIEW && parent == (uint64_t)ph_core.ram_fd);
    assert(pages && count && (rights & PACHA_FD_RIGHT_REVOKE));
    assert(flags == (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC | PACHA_VMO_PAGE_VIEW_SPARSE));
    return 200;
}

long pacha_syscall3(uint64_t op, uint64_t a, uint64_t b, uint64_t c) {
    assert(op == PACHA_IPC_SYSCALL_CHANNEL_CREATE);
    assert(b == (PH_IPC_CHANNEL_RIGHTS | PACHA_FD_RIGHT_CLOSE));
    assert(c == (PACHA_FD_FLAG_PRIVATE | PACHA_FD_FLAG_CLOEXEC));
    uint64_t *pair = (void *)(uintptr_t)a;
    pair[0] = 30; pair[1] = 31;
    return 0;
}
_Noreturn void ph_fail(const char *file, unsigned line, uint64_t error) {
    fprintf(stderr, "%s:%u: %llu\n", file, line, (unsigned long long)error);
    abort();
}

long pacha_syscall6(uint64_t op, uint64_t a, uint64_t b, uint64_t c,
    uint64_t d, uint64_t e, uint64_t f) {
    assert(op == PACHA_VM_SYSCALL_MMAP && a >= 20 && a <= 24 && !b && !f);
    assert(c == vmo_bytes[a - 20] && d == (PACHA_PROT_READ | PACHA_PROT_WRITE) &&
        e == PACHA_MMAP_SHARED);
    return (long)(uintptr_t)vmos[a - 20];
}

long pacha_syscall4(uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    if (op == PACHA_FD_SYSCALL_VMO_COLLECT_DIRTY) {
        assert(a == 200 && b && c == 1 && d == 0);
        *(uint64_t *)(uintptr_t)b = 2;
        return shared_refresh_failure ? PACHA_SYSCALL_ERR_INVALID : 0;
    }
    if (op == PACHA_FD_SYSCALL_VMO_REFRESH_PAGE_VIEW) {
        assert(a == 200 && b == (uint64_t)ph_core.ram_fd && c && d == 2);
        return shared_refresh_failure ? PACHA_SYSCALL_ERR_INVALID : 0;
    }
    assert(op == PACHA_FD_SYSCALL_WAIT_MANY && c == 1);
    assert(d == PACHA_FD_WAIT_SET_REPLACE || d == PACHA_FD_WAIT_SET_REUSE);
    if (d == PACHA_FD_WAIT_SET_REPLACE) ++retained_replaces;
    else ++retained_reuses;
    return mock_wait_events(a, b, 0);
}

int ph_ipc_init(struct ph_ipc *ipc, pacha_fd_t fd, uint64_t generation) {
    assert(!ipc->generation && (fd == 100 || fd == 101 || fd == 30 || fd == 31) && generation == 7);
    *ipc = (struct ph_ipc){.generation = generation, .fd = fd, .admitted = 1};
    return 0;
}
int ph_ipc_receive(struct ph_ipc *ipc, uint64_t generation, struct ph_ipc_packet *packet) {
    if (ipc->fd == 31) return -EAGAIN;
    assert(ipc->fd == 100 || ipc->fd == 101);
    assert(generation == 7 && ipc->admitted);
    if (client_receive_error[ipc->fd - 100]) return client_receive_error[ipc->fd - 100];
    struct ph_ipc_packet *queued = &client_packets[ipc->fd - 100];
    if (!queued->operation) return -EAGAIN;
    *packet = *queued;
    memset(queued, 0, sizeof(*queued));
    return 0;
}
int ph_ipc_notify_consume(pacha_fd_t fd) {
    assert(fd == 100 || fd == 101);
    if (client_receive_error[fd - 100]) return client_receive_error[fd - 100];
    if (!client_latched[fd - 100]) return -EAGAIN;
    client_latched[fd - 100] = 0;
    return 0;
}
int ph_ipc_packet_release(struct ph_ipc_packet *packet) {
    for (size_t i = 0; i < packet->fd_count; ++i) assert(packet->fds[i].fd == PH_IPC_NO_FD);
    memset(packet, 0, sizeof(*packet));
    return 0;
}
int ph_ipc_destroy(struct ph_ipc *ipc, uint64_t generation) {
    assert(generation == 7);
    if (!ipc->fd) return 0;
    if (ipc->fd == 30 || ipc->fd == 31) { ipc->fd = ipc->admitted = 0; return 0; }
    assert(ipc->fd >= 100 && ipc->fd <= 101);
    ipc->fd = ipc->admitted = 0;
    ++client_closed;
    return 0;
}

int ph_ipc_send(struct ph_ipc *ipc, struct ph_ipc_packet *packet) {
    if (packet->operation == PH_FS_SHARED_ATTACHMENT) {
        assert(lock_depth == 1 && packet->fd_count == 1 && packet->fds[0].fd == 200);
        assert(packet->generation == 7 && !(packet->fds[0].rights & PACHA_FD_RIGHT_REVOKE));
        shared_attachment = *packet;
        return 0;
    }
    assert(!lock_depth);
    if (ipc && ipc->fd == 30) {
        assert(packet->operation == 1 && packet->generation == 7 && !packet->fd_count);
        ++private_wakes; return 0;
    }
    if (packet->operation == PH_FS_CLIENT_BOUND || packet->operation == PH_FS_CLIENT_RELEASED) {
        assert(packet->generation == 7 && packet->correlation >= 20 &&
            !packet->fd_count && !packet->value);
        if (packet->operation == PH_FS_CLIENT_BOUND) ++client_bound;
        else ++client_released;
        return 0;
    }
    /* Client doorbells are latches; a packet send on one is a regression. */
    assert(!ipc || ipc->fd < 100 || ipc->fd > 101);
    if (packet->operation == PH_FS_EXCHANGE_RELEASED) {
        assert(packet->generation == 7 && packet->correlation >= 3 &&
            !packet->value && !packet->fd_count);
        released++; return 0;
    }
    assert((packet->operation == PH_FS_QUEUE_NOTIFY ||
        packet->operation == PH_FS_OWNER_NOTIFY || packet->operation == PH_FS_EXCHANGE_NOTIFY) &&
        packet->generation == 7 &&
        (packet->operation == PH_FS_EXCHANGE_NOTIFY ? packet->correlation >= 3 : !packet->correlation) &&
        packet->value == 2 && !packet->fd_count);
    sends++; return 0;
}

int ph_ipc_notify(const struct ph_ipc_notice *notice) {
    assert(!lock_depth && notice->fd >= 100 && notice->fd <= 101);
    ++client_sends;
    int result = 0;
    if (pause_notice) {
        assert(!pthread_mutex_lock(&notice_mutex));
        notice_entered = 1;
        assert(!pthread_cond_broadcast(&notice_condition));
        while (!notice_release) assert(!pthread_cond_wait(&notice_condition, &notice_mutex));
        assert(!pthread_mutex_unlock(&notice_mutex));
    }
    return result;
}

static int reply(const void *bytes, size_t size, void *out, size_t capacity,
    size_t *used, int owner) {
    kb2_fs_request_t r;
    assert(!kb2_fs_request_decode(bytes, size, size, &r));
    kb2_fs_response_t response = {.generation = r.generation,
        .sequence = r.sequence, .opcode = r.opcode};
    if (owner) {
        assert(r.opcode == KB2_FILESYSTEM_OP_HELLO ||
            r.opcode == KB2_FILESYSTEM_OP_EXEC_OPEN ||
            r.opcode == KB2_FILESYSTEM_OP_PREAD ||
            r.opcode == KB2_FILESYSTEM_OP_STATX ||
            r.opcode == KB2_FILESYSTEM_OP_CLOSE ||
            r.opcode == KB2_FILESYSTEM_OP_CLIENT_CREDENTIALS ||
            r.opcode == KB2_FILESYSTEM_OP_SHARED_FILE_VMO);
        owner_calls++;
        if (r.opcode == KB2_FILESYSTEM_OP_SHARED_FILE_VMO) {
            uint64_t pages[] = {1, PACHA_VMO_PAGE_VIEW_HOLE};
            assert(!mock_shared.create(mock_shared.context, r.client, r.sequence,
                pages, 2, r.flags, &response.handle));
            response.result = 4096;
        }
        if (r.opcode == KB2_FILESYSTEM_OP_EXEC_OPEN) response.handle = 88;
        if (r.opcode == KB2_FILESYSTEM_OP_PREAD) {
            response.result = (int64_t)r.length;
            response.data_length = r.length;
            assert(r.length <= capacity - KB2_FILESYSTEM_RESPONSE_SIZE);
            memset((unsigned char *)out + KB2_FILESYSTEM_RESPONSE_SIZE, 0xe7, (size_t)r.length);
        }
    } else if (r.opcode == KB2_FILESYSTEM_OP_CLIENT_REGISTER) {
        response.result = -EPERM;
    } else {
        assert(r.opcode == KB2_FILESYSTEM_OP_READ ||
            r.opcode == KB2_FILESYSTEM_OP_PREAD ||
            r.opcode == KB2_FILESYSTEM_OP_SYNC || r.opcode == KB2_FILESYSTEM_OP_SYNCFS);
        if (r.opcode == KB2_FILESYSTEM_OP_SYNC) assert(!r.handle && !r.length);
        if (r.opcode == KB2_FILESYSTEM_OP_SYNCFS) assert(r.handle == 42 && !r.length);
        assert((!r.client || r.client == routed_client) && (routed_client == 7 || routed_client == 9));
        data_calls++;
        response.result = (int64_t)r.length;
        response.data_length = r.length;
        assert(r.length <= capacity - KB2_FILESYSTEM_RESPONSE_SIZE);
        memset((unsigned char *)out + KB2_FILESYSTEM_RESPONSE_SIZE, 0xa7, (size_t)r.length);
    }
    assert(!kb2_fs_response_encode(out, capacity, &response));
    *used = KB2_FILESYSTEM_RESPONSE_SIZE + (size_t)response.data_length;
    return 0;
}

static int linux_data(struct kobox_linux_fs_service *service, uint64_t client,
    const void *bytes, size_t size, void *out, size_t capacity, size_t *used) {
    assert(service == (void *)1234); routed_client = client;
    kb2_fs_request_t r;
    assert(!kb2_fs_request_decode(bytes, size, size, &r));
    if (r.client && r.client != client) {
        kb2_fs_response_t denied = {.opcode = r.opcode, .generation = r.generation,
            .sequence = r.sequence, .result = -EACCES};
        assert(!kb2_fs_response_encode(out, capacity, &denied));
        *used = KB2_FILESYSTEM_RESPONSE_SIZE;
        return 0;
    }
    return reply(bytes, size, out, capacity, used, 0);
}
static int linux_owner(struct kobox_linux_fs_service *service,
    const void *bytes, size_t size, void *out, size_t capacity, size_t *used) {
    assert(service == (void *)1234);
    return reply(bytes, size, out, capacity, used, 1);
}

static int mock_served;
static int mock_create(struct kobox_linux_fs_service *service,
    const struct kobox_linux_fs_worker_config *config, struct kobox_linux_fs_workers **pool) {
    assert(service == (void *)1234 && config->workers >= 3 && config->control_bytes);
    assert(!mock_jobs && !mock_outstanding);
    mock_publish = config->publish;
    *pool = (void *)2468; return 0;
}
static int mock_submit_common(struct kobox_linux_fs_workers *pool, uint64_t client,
    const void *input, size_t size, size_t capacity, void *cookie, int owner) {
    assert(pool == (void *)2468 && mock_served);
    if (mock_outstanding >= mock_limit) return -EAGAIN;
    struct fake_work *job = calloc(1, sizeof(*job)); assert(job);
    job->input = malloc(size); job->response = malloc(capacity); assert(job->input && job->response);
    memcpy(job->input, input, size);
    job->input_size = size; job->capacity = capacity; job->client = client; job->cookie = cookie; job->owner = owner;
    struct fake_work **tail = &mock_jobs;
    while (*tail) tail = &(*tail)->next;
    *tail = job; ++mock_outstanding;
    return 0;
}
static int mock_submit(struct kobox_linux_fs_workers *pool, uint64_t client,
    const void *input, size_t size, size_t capacity, void *cookie) {
    return mock_submit_common(pool, client, input, size, capacity, cookie, 0);
}
static int mock_control(struct kobox_linux_fs_workers *pool, const void *input,
    size_t size, size_t capacity, void *cookie) {
    return mock_submit_common(pool, 0, input, size, capacity, cookie, 1);
}
static void mock_complete(struct fake_work *job) {
    assert(!job->completed && !job->executing);
    job->executing = 1;
    job->error = job->owner ? linux_owner((void *)1234, job->input, job->input_size,
        job->response, job->capacity, &job->used) : linux_data((void *)1234, job->client,
        job->input, job->input_size, job->response, job->capacity, &job->used);
    if (!job->owner && mock_publish)
        mock_publish(job->cookie, job->response, job->used, job->error);
    job->executing = 0;
    job->completed = 1;
}
static void *publish_thread(void *job) { mock_complete(job); return NULL; }
static pthread_barrier_t publisher_barrier;
static void *publish_result_thread(void *argument) {
    struct fake_work *job = argument;
    int result = pthread_barrier_wait(&publisher_barrier);
    assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
    mock_publish(job->cookie, job->response, job->used, job->error);
    job->completed = 1;
    return NULL;
}
static int mock_collect(struct kobox_linux_fs_workers *pool, struct kobox_linux_fs_work **work,
    void **cookie, const void **response, size_t *used, int *error) {
    assert(pool == (void *)2468);
    if (defer_collect) return -EAGAIN;
    struct fake_work **cursor = &mock_jobs;
    while (*cursor) {
        kb2_fs_request_t r;
        assert(!kb2_fs_request_decode((*cursor)->input, (*cursor)->input_size, (*cursor)->input_size, &r));
        if ((*cursor)->executing || (blocked_sequence && r.sequence == blocked_sequence)) {
            cursor = &(*cursor)->next; continue;
        }
        break;
    }
    if (!*cursor) return -EAGAIN;
    struct fake_work *job = *cursor; *cursor = job->next;
    if (!job->completed) mock_complete(job);
    *work = (void *)job; *cookie = job->cookie; *response = job->response; *used = job->used; *error = job->error;
    return 0;
}
static int mock_release(struct kobox_linux_fs_workers *pool, struct kobox_linux_fs_work *work) {
    assert(pool == (void *)2468 && mock_outstanding);
    struct fake_work *job = (void *)work;
    free(job->input); free(job->response); free(job); --mock_outstanding; return 0;
}
static int mock_serve(struct kobox_linux_fs_workers *pool) {
    assert(pool == (void *)2468 && !mock_jobs && !mock_outstanding); ++mock_served; return 0;
}
static int mock_close(struct kobox_linux_fs_workers *pool) { assert(pool == (void *)2468); return 0; }
static int mock_shared_bind(struct kobox_linux_fs_service *service,
    const struct kobox_linux_fs_shared_ops *ops) {
    assert(service == (void *)1234 && ops->budget_bytes && ops->context);
    mock_shared = *ops;
    return 0;
}
static int mock_shared_close(struct kobox_linux_fs_service *service) {
    assert(service == (void *)1234);
    return 0;
}
static int mock_destroy(struct kobox_linux_fs_workers *pool) {
    assert(pool == (void *)2468 && !mock_outstanding && !mock_jobs); return 0;
}

void *ph_image_lookup(void *core, const char *name) {
    assert(core == (void *)5678);
    void *symbol = NULL;
#define EXPORT(entry, function) if (!strcmp(name, entry)) { \
    __auto_type address = &function; memcpy(&symbol, &address, sizeof(symbol)); return symbol; }
    EXPORT("kobox_linux_fs_workers_create", mock_create)
    EXPORT("kobox_linux_fs_service_shared_bind", mock_shared_bind)
    EXPORT("kobox_linux_fs_service_shared_close", mock_shared_close)
    EXPORT("kobox_linux_fs_workers_submit", mock_submit)
    EXPORT("kobox_linux_fs_workers_submit_control", mock_control)
    EXPORT("kobox_linux_fs_workers_serve", mock_serve)
    EXPORT("kobox_linux_fs_workers_collect", mock_collect)
    EXPORT("kobox_linux_fs_workers_release", mock_release)
    EXPORT("kobox_linux_fs_workers_close", mock_close)
    EXPORT("kobox_linux_fs_workers_destroy", mock_destroy)
#undef EXPORT
    abort();
    return symbol;
}

struct driver {
    struct ph_fs_channel channel;
    struct ph_lifecycle_service *service;
    unsigned lane;
    int mutate;
};

static int quantum(struct ph_lifecycle_service *service) {
    struct kobox_linux_io_service io = {.filesystem = (void *)1234};
    return service->advance(service->context, &io, NULL, false);
}
static void pump(struct ph_lifecycle_service *service) {
    for (unsigned attempts = 0; attempts < 100; ++attempts) {
        int result = quantum(service);
        assert(result == 0 || result == PH_LIFECYCLE_SERVICE_PROGRESS);
        if (!result) return;
    }
    abort();
}
static void management(struct ph_lifecycle_service *service, int status) {
    if (status == PH_LIFECYCLE_SERVICE_IDLE) return;
    assert(!status);
    assert(!service->complete(service->context, NULL));
    assert(!service->release(service->context));
}

static void finish_request(struct driver *driver, int status) {
    assert(status == PH_LIFECYCLE_SERVICE_IDLE);
    assert(quantum(driver->service) == PH_LIFECYCLE_SERVICE_PROGRESS);
    /* The peer cannot replace the already-admitted opcode/client by changing
     * shared bytes between native preparation and Linux task dispatch. */
    if (driver->mutate) memset((unsigned char *)vmos[driver->lane] +
        driver->channel.arena_offset, 0xcc, KB2_FILESYSTEM_REQUEST_SIZE);
    pump(driver->service);
}

static int notify(void *context, uint32_t id) {
    struct driver *driver = context;
    struct ph_ipc_packet packet = {.operation = driver->lane ?
        driver->lane == 2 ? PH_FS_EXCHANGE_NOTIFY : PH_FS_OWNER_NOTIFY : PH_FS_QUEUE_NOTIFY,
        .generation = 7, .value = id,
        .correlation = driver->lane == 2 ? driver->channel.config.channel_id : 0};
    finish_request(driver, driver->service->prepare(driver->service->context, &packet));
    return 0;
}
static int wait(void *context, uint32_t id) {
    struct driver *driver = context;
    assert(id == 2);
    pump(driver->service);
    return 0;
}

static void rejected_bind(unsigned scenario) {
    struct ph_fs_queue queue;
    struct ph_lifecycle_service service;
    struct ph_fs_channel_config data = {
        .generation = 7, .channel_id = 1, .queue_size = 8, .arena_bytes = 65536};
    struct ph_fs_channel_config owner = data;
    owner.channel_id = 2;
    assert(!ph_fs_queue_init(&queue, (void *)5678, &data, &owner,
        &kb2_vq_x86_64_atomics, &service));
    size_t offset;
    assert(!ph_fs_channel_layout(&data, &vmo_bytes[0], &offset));
    vmos[0] = aligned_alloc(4096, vmo_bytes[0]); assert(vmos[0]);
    memset(vmos[0], 0, vmo_bytes[0]);
    struct ph_fs_channel driver = {0};
    kb2_vq_chain_t *slots[8] = {0};
    uint16_t owners[8] = {0};
    assert(!ph_fs_channel_bind(&driver, vmos[0], vmo_bytes[0], &data,
        KB2_VQ_DRIVER, &kb2_vq_x86_64_atomics, slots, owners));
    struct ph_ipc_packet packet = {.operation = PH_FS_QUEUE_BIND,
        .generation = 7, .value = 1, .fd_count = 1,
        .fds = {{.fd = 20, .rights = PH_FS_QUEUE_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC}}};
    if (!scenario) packet.fds[0].rights &= ~PACHA_FD_RIGHT_MAP_WRITE;
    else if (scenario == 1) packet.generation++;
    else if (scenario == 2)
        ((unsigned char *)vmos[0])[KB2_PROTOCOL_CHANNEL_HEADER_ABI_IDENTITY_OFFSET] ^= 1;
    else {
        size_t encoded;
        assert(!kb2_protocol_channel_encoded_size(1, 1, &encoded));
        ((unsigned char *)vmos[0])[encoded +
            KB2_FILESYSTEM_CHANNEL_PROFILE_SCHEMA_DIGEST_OFFSET] ^= 1;
    }
    assert(service.prepare(service.context, &packet) == (!scenario ? -EACCES : -EPROTO));
    assert(!service.stop(service.context));
    assert(!ph_fs_queue_finish(&queue) && !allocations);
    free(vmos[0]); vmos[0] = NULL;
}

static void temporary_exchange(struct ph_lifecycle_service *service, uint64_t id,
    int owner, int denied) {
    const size_t payload = 1048576;
    struct ph_fs_channel_config config = {.generation = 7, .channel_id = id,
        .queue_size = 8, .arena_bytes = KB2_FILESYSTEM_REQUEST_SIZE +
            KB2_FILESYSTEM_RESPONSE_SIZE + payload};
    size_t offset;
    assert(!ph_fs_channel_layout(&config, &vmo_bytes[2], &offset));
    vmos[2] = aligned_alloc(4096, vmo_bytes[2]); assert(vmos[2]);
    memset(vmos[2], 0, vmo_bytes[2]);
    struct driver driver;
    memset(&driver, 0, sizeof(driver));
    driver.service = service; driver.lane = 2;
    kb2_vq_chain_t *slots[8] = {0};
    uint16_t owners[8] = {0};
    assert(!ph_fs_channel_bind(&driver.channel, vmos[2], vmo_bytes[2], &config,
        KB2_VQ_DRIVER, &kb2_vq_x86_64_atomics, slots, owners));
    struct ph_ipc_packet packet = {.operation = owner ? PH_FS_EXCHANGE_OWNER_PREPARE :
        PH_FS_EXCHANGE_PREPARE,
        .generation = 7, .correlation = id, .value = 7};
    assert(service->prepare(service->context, &packet) == PH_LIFECYCLE_SERVICE_IDLE);
    packet = (struct ph_ipc_packet){.operation = PH_FS_EXCHANGE_BIND,
        .generation = 7, .correlation = id, .value = config.arena_bytes, .fd_count = 1,
        .fds = {{.fd = 22, .rights = PH_FS_QUEUE_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC}}};
    assert(service->prepare(service->context, &packet) == PH_LIFECYCLE_SERVICE_IDLE);
    kb2_fs_request_t r = {.opcode = denied ? KB2_FILESYSTEM_OP_EXEC_OPEN :
        owner ? KB2_FILESYSTEM_OP_PREAD : KB2_FILESYSTEM_OP_READ, .generation = 7,
        .sequence = id, .client = 7, .credential_generation = 1, .length = payload};
    unsigned char request[KB2_FILESYSTEM_REQUEST_SIZE];
    assert(!kb2_fs_request_encode(request, sizeof(request), sizeof(request), &r));
    unsigned char *response = malloc(payload + KB2_FILESYSTEM_RESPONSE_SIZE); assert(response);
    struct ph_fs_channel_wait callbacks = {.context = &driver, .notify = notify, .wait = wait};
    unsigned before = owner ? owner_calls : data_calls;
    size_t used;
    assert(!ph_fs_channel_call(&driver.channel, request, sizeof(request),
        response, payload + KB2_FILESYSTEM_RESPONSE_SIZE, &used, &callbacks));
    if (denied) {
        kb2_fs_response_t rejection;
        assert(!kb2_fs_response_decode(response, used, &rejection));
        assert(rejection.result == -EPERM && data_calls == before);
    } else {
        assert(used == payload + KB2_FILESYSTEM_RESPONSE_SIZE &&
            (owner ? owner_calls : data_calls) == before + 1);
        const unsigned char value = owner ? 0xe7 : 0xa7;
        assert(response[KB2_FILESYSTEM_RESPONSE_SIZE] == value && response[used - 1] == value);
    }
    packet = (struct ph_ipc_packet){.operation = PH_FS_EXCHANGE_RELEASE,
        .generation = 7, .correlation = id};
    assert(service->prepare(service->context, &packet) == PH_LIFECYCLE_SERVICE_IDLE);
    pump(service);
    kb2_vq_channel_fault(&driver.channel.channel);
    free(response); free(vmos[2]); vmos[2] = NULL;
}

static void direct_clients(struct ph_lifecycle_service *service, struct driver *owner_driver) {
    struct ph_fs_channel drivers[2];
    memset(drivers, 0, sizeof(drivers));
    kb2_vq_chain_t *slots[2][8] = {{0}};
    uint16_t owners[2][8] = {{0}};
    for (unsigned i = 0; i < 2; ++i) {
        struct ph_fs_channel_config config = {.generation = 7, .channel_id = 20 + i,
            .queue_size = 8, .arena_bytes = i ? 65536 : 4096};
        size_t offset;
        assert(!ph_fs_channel_layout(&config, &vmo_bytes[3 + i], &offset));
        vmos[3 + i] = aligned_alloc(4096, vmo_bytes[3 + i]); assert(vmos[3 + i]);
        memset(vmos[3 + i], 0, vmo_bytes[3 + i]);
        assert(!ph_fs_channel_bind(&drivers[i], vmos[3 + i], vmo_bytes[3 + i],
            &config, KB2_VQ_DRIVER, &kb2_vq_x86_64_atomics, slots[i], owners[i]));
        struct ph_ipc_packet packet = {.operation = PH_FS_CLIENT_PREPARE,
            .generation = 7, .correlation = config.channel_id, .value = i ? 9 : 7};
        assert(service->prepare(service->context, &packet) == PH_LIFECYCLE_SERVICE_IDLE);
        packet = (struct ph_ipc_packet){.operation = PH_FS_CLIENT_BIND,
            .generation = 7, .correlation = config.channel_id, .value = config.arena_bytes,
            .fd_count = 2, .fds = {{.fd = 23 + i, .rights = PH_FS_QUEUE_RIGHTS,
                .flags = PACHA_FD_FLAG_CLOEXEC}, {.fd = 100 + i,
                .rights = PH_FS_CLIENT_DOORBELL_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC}}};
        assert(!service->prepare(service->context, &packet));
        assert(packet.fds[1].fd == PH_IPC_NO_FD);
        management(service, 0);
    }
    assert(client_bound == 2);
    assert(!service->wait(service->context, 40, 1) && wait_calls == 2);
    /* Both zero-identity requests are published before execution. Each uses
     * its private registered binding; the second never selects client seven. */
    struct ph_fs_pending pending[2] = {{0}};
    unsigned char request[KB2_FILESYSTEM_REQUEST_SIZE];
    for (unsigned i = 0; i < 2; ++i) {
        kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_PREAD, .generation = 7,
            .sequence = 200 + i, .length = 16};
        assert(!kb2_fs_request_encode(request, sizeof(request), sizeof(request), &r));
        int notify;
        assert(!ph_fs_channel_submit(&drivers[i], &pending[i], request, sizeof(request), 80, &notify));
    }
    pump(service);
    for (unsigned i = 0; i < 2; ++i) {
        struct ph_fs_pending *done = NULL;
        assert(!ph_fs_channel_poll(&drivers[i], &done) && done == &pending[i]);
        kb2_fs_response_t response;
        unsigned char payload[16]; size_t used;
        assert(!ph_fs_channel_collect(done, &response, payload, sizeof(payload), &used));
        assert(response.result == 16 && response.sequence == 200 + i);
    }
    /* A worker publishes before ANY owner collection. Old tickets still pin
     * their resources while the peer consumes used and reuses descriptors. */
    struct ph_fs_queue *queue = service->context;
    defer_collect = 1;
    for (unsigned i = 0; i < 2; ++i) {
        kb2_fs_request_t early = {.opcode = KB2_FILESYSTEM_OP_PREAD,
            .generation = 7, .sequence = 240 + i, .length = 16};
        assert(!kb2_fs_request_encode(request, sizeof(request), sizeof(request), &early));
        int notify;
        assert(!ph_fs_channel_submit(&drivers[0], &pending[i], request, sizeof(request), 80, &notify));
    }
    pump(service);
    assert(mock_outstanding == 2 && queue->clients->lane.inflight == 2);
    int ready;
    assert(!ph_fs_channel_arm(&drivers[0], &ready) && !ready);
    unsigned notifications = client_sends;
    mock_complete(mock_jobs->next);
    assert(client_sends == notifications + 1);
    struct ph_fs_pending *early_done = NULL;
    assert(!ph_fs_channel_poll(&drivers[0], &early_done) && early_done == &pending[1]);
    kb2_fs_response_t early_reply;
    unsigned char early_payload[16]; size_t early_used;
    assert(!ph_fs_channel_collect(early_done, &early_reply, early_payload, sizeof(early_payload), &early_used));
    assert(early_reply.sequence == 241 && mock_outstanding == 2);
    mock_complete(mock_jobs);
    assert(!ph_fs_channel_poll(&drivers[0], &early_done) && early_done == &pending[0]);
    assert(!ph_fs_channel_collect(early_done, &early_reply, early_payload, sizeof(early_payload), &early_used));
    assert(early_reply.sequence == 240 && queue->clients->lane.inflight == 2);
    kb2_fs_request_t reused = {.opcode = KB2_FILESYSTEM_OP_PREAD,
        .generation = 7, .sequence = 242, .length = 16};
    assert(!kb2_fs_request_encode(request, sizeof(request), sizeof(request), &reused));
    int reuse_notify;
    assert(!ph_fs_channel_submit(&drivers[0], &pending[0], request, sizeof(request), 80, &reuse_notify));
    pump(service);
    assert(mock_outstanding == 3 && queue->clients->lane.inflight == 3);
    reused.sequence = 243;
    assert(!kb2_fs_request_encode(request, sizeof(request), sizeof(request), &reused));
    assert(!ph_fs_channel_submit(&drivers[0], &pending[1], request, sizeof(request), 80, &reuse_notify));
    pump(service);
    assert(mock_outstanding == 4 && queue->clients->lane.inflight == 4);
    struct fake_work *jobs[2] = {mock_jobs->next->next, mock_jobs->next->next->next};
    pthread_t publishers[2];
    assert(!pthread_barrier_init(&publisher_barrier, NULL, 2));
    for (unsigned i = 0; i < 2; ++i) {
        jobs[i]->error = linux_data((void *)1234, jobs[i]->client, jobs[i]->input,
            jobs[i]->input_size, jobs[i]->response, jobs[i]->capacity, &jobs[i]->used);
        assert(!pthread_create(&publishers[i], NULL, publish_result_thread, jobs[i]));
    }
    for (unsigned i = 0; i < 2; ++i) assert(!pthread_join(publishers[i], NULL));
    assert(!pthread_barrier_destroy(&publisher_barrier));
    unsigned seen = 0;
    for (unsigned i = 0; i < 2; ++i) {
        assert(!ph_fs_channel_poll(&drivers[0], &early_done));
        assert(!ph_fs_channel_collect(early_done, &early_reply, early_payload, sizeof(early_payload), &early_used));
        assert(early_reply.sequence == 242 || early_reply.sequence == 243);
        seen |= 1u << (early_reply.sequence - 242);
    }
    assert(seen == 3);
    unsigned used_before_reclaim = drivers[0].lane.consumed;
    defer_collect = 0;
    pump(service);
    assert(!mock_outstanding && !queue->clients->lane.inflight);
    assert(drivers[0].lane.consumed == used_before_reclaim);
    assert(ph_fs_channel_poll(&drivers[0], &early_done) == -EAGAIN);
    /* Forged header identity cannot override the registered binding. */
    kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_PREAD, .generation = 7,
        .sequence = 210, .client = 9, .capabilities = UINT64_MAX, .length = 16};
    assert(!kb2_fs_request_encode(request, sizeof(request), sizeof(request), &r));
    /* Idle passes skip re-arming an unchanged ring; the arm already in force
     * must still make the client's next publish request a doorbell. */
    for (unsigned i = 0; i < 3; ++i) assert(!quantum(service));
    int notify;
    assert(!ph_fs_channel_submit(&drivers[0], &pending[0], request, sizeof(request), 80, &notify));
    assert(notify);
    pump(service); assert(routed_client == 7);
    struct ph_fs_pending *done = NULL;
    assert(!ph_fs_channel_poll(&drivers[0], &done));
    kb2_fs_response_t response; size_t used;
    assert(!ph_fs_channel_collect(done, &response, NULL, 0, &used) && response.result == -EACCES);
    /* A pending registry update barriers only its affected client. Other
     * clients execute while that management task is blocked; after its reply,
     * admission resumes. Neither path wakes the native helper per completion. */
    struct ph_fs_pending control_pending = {0};
    r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_CLIENT_CREDENTIALS,
        .generation = 7, .client = 7, .credential_generation = 2, .sequence = 230};
    assert(!kb2_fs_request_encode(request, sizeof(request), sizeof(request), &r));
    blocked_sequence = 230;
    assert(!ph_fs_channel_submit(&owner_driver->channel, &control_pending,
        request, sizeof(request), 80, &notify));
    unsigned wakes = private_wakes;
    pump(service);
    assert(queue->control_active && queue->barrier_client == 7);
    uint16_t before_barrier = queue->clients->lane.channel.lane.consumed;
    for (unsigned i = 0; i < 2; ++i) {
        r = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_PREAD,
            .generation = 7, .sequence = 231 + i, .length = 16};
        assert(!kb2_fs_request_encode(request, sizeof(request), sizeof(request), &r));
        assert(!ph_fs_channel_submit(&drivers[i], &pending[i], request, sizeof(request), 80, &notify));
    }
    pump(service);
    assert(queue->clients->lane.channel.lane.consumed == before_barrier);
    assert(ph_fs_channel_poll(&drivers[0], &done) == -EAGAIN);
    assert(!ph_fs_channel_poll(&drivers[1], &done) && done == &pending[1]);
    unsigned char barrier_payload[16];
    assert(!ph_fs_channel_collect(done, &response, barrier_payload, sizeof(barrier_payload), &used) && response.sequence == 232);
    blocked_sequence = 0;
    pump(service);
    assert(!queue->control_active && !queue->barrier_client && !queue->barrier_target);
    assert(!ph_fs_channel_poll(&owner_driver->channel, &done) && done == &control_pending);
    assert(!ph_fs_channel_collect(done, &response, NULL, 0, &used) && response.sequence == 230);
    assert(!ph_fs_channel_poll(&drivers[0], &done) && done == &pending[0]);
    assert(!ph_fs_channel_collect(done, &response, barrier_payload, sizeof(barrier_payload), &used) && response.sequence == 231);
    assert(private_wakes == wakes);
    /* One queue now holds multiple jobs. Request 220 remains blocked while
     * the same queue's 221 and another client's 222 complete out of order. */
    struct ph_fs_pending parallel[3] = {{0}};
    blocked_sequence = 220;
    for (unsigned i = 0; i < 3; ++i) {
        r.client = 0; r.sequence = 220 + i;
        assert(!kb2_fs_request_encode(request, sizeof(request), sizeof(request), &r));
        assert(!ph_fs_channel_submit(&drivers[i == 2], &parallel[i], request, sizeof(request), 80, &notify));
    }
    pump(service);
    assert(mock_outstanding == 1);
    unsigned char payload[16];
    assert(!ph_fs_channel_poll(&drivers[0], &done) && done == &parallel[1]);
    assert(!ph_fs_channel_collect(done, &response, payload, sizeof(payload), &used) && response.sequence == 221);
    assert(!ph_fs_channel_poll(&drivers[1], &done) && done == &parallel[2]);
    assert(!ph_fs_channel_collect(done, &response, payload, sizeof(payload), &used) && response.sequence == 222);
    assert(ph_fs_channel_poll(&drivers[0], &done) == -EAGAIN);
    /* Suspend a real publisher thread after used/SEND but before returning
     * its endpoint pin. Peer death and detach must not close/unmap it. */
    defer_collect = 1; pause_notice = 1;
    assert(!ph_fs_channel_arm(&drivers[0], &ready) && !ready);
    pthread_t publisher;
    assert(!pthread_create(&publisher, NULL, publish_thread, mock_jobs));
    assert(!pthread_mutex_lock(&notice_mutex));
    while (!notice_entered) assert(!pthread_cond_wait(&notice_condition, &notice_mutex));
    assert(!pthread_mutex_unlock(&notice_mutex));
    r.sequence = 225;
    assert(!kb2_fs_request_encode(request, sizeof(request), sizeof(request), &r));
    assert(!ph_fs_channel_submit(&drivers[0], &pending[0], request, sizeof(request), 80, &notify));
    unsigned before_death = data_calls;
    /* The native helper consumes doorbells; its failed receive marks the
     * client, and the owner faults it on its next pass. */
    client_receive_error[0] = -EPIPE;
    client_readable[0] = 1;
    assert(!service->wait(service->context, 40, 0));
    client_readable[0] = 0;
    assert(queue->clients->doorbell_fault);
    pump(service);
    assert(queue->clients->lane.retiring && queue->clients->lane.mapping &&
        queue->clients->lane.inflight == 1 && !queue->clients->release_requested);
    assert(!client_released && mock_outstanding == 1 && data_calls == before_death);
    assert(!client_closed && queue->clients->ipc.fd == 100);
    client_receive_error[0] = 0;
    struct ph_ipc_packet detach = {.operation = PH_FS_CLIENT_RELEASE, .generation = 7, .correlation = 20};
    assert(service->prepare(service->context, &detach) == PH_LIFECYCLE_SERVICE_IDLE);
    pump(service);
    assert(!client_released && queue->clients->lane.mapping && queue->clients->lane.inflight == 1);
    /* Quota pressure restores the last unexecuted chain and leaves no native
     * input backlog/allocation. A worker release makes it admissible again. */
    mock_limit = 1;
    r.sequence = 223;
    assert(!kb2_fs_request_encode(request, sizeof(request), sizeof(request), &r));
    assert(!ph_fs_channel_submit(&drivers[1], &pending[1], request, sizeof(request), 80, &notify));
    uint16_t consumed = queue->clients->next->lane.channel.lane.consumed;
    unsigned allocated = allocations, calls = data_calls;
    pump(service);
    assert(allocations == allocated && data_calls == calls && mock_outstanding == 1);
    assert(queue->clients->next->lane.channel.lane.consumed == consumed);
    assert(!client_closed && queue->clients->ipc.fd == 100);
    assert(!pthread_mutex_lock(&notice_mutex));
    notice_release = 1;
    assert(!pthread_cond_broadcast(&notice_condition));
    assert(!pthread_mutex_unlock(&notice_mutex));
    assert(!pthread_join(publisher, NULL));
    pause_notice = defer_collect = 0;
    blocked_sequence = 0;
    pump(service);
    assert(client_released == 1 && mock_outstanding == 0 && !queue->clients->next);
    assert(!ph_fs_channel_poll(&drivers[1], &done) && done == &pending[1]);
    assert(!ph_fs_channel_collect(done, &response, payload, sizeof(payload), &used) && response.sequence == 223);
    mock_limit = 100;
    /* Invalid owner packet retires only its client. Unconsumed work is never
     * executed, and queue detach does not revoke the managed client identity. */
    r.sequence = 224;
    assert(!kb2_fs_request_encode(request, sizeof(request), sizeof(request), &r));
    assert(!ph_fs_channel_submit(&drivers[1], &pending[1], request, sizeof(request), 80, &notify));
    calls = data_calls;
    /* A latched doorbell is consumed and leaves the client healthy. */
    client_latched[1] = 1;
    client_readable[1] = 1;
    assert(!service->wait(service->context, 40, 0));
    client_readable[1] = 0;
    assert(!client_latched[1] && !queue->clients->doorbell_fault);
    /* Readable without a latch is a queued packet the client forged. */
    client_packets[1] = (struct ph_ipc_packet){.operation = PH_FS_CLIENT_PREPARE,
        .generation = 7, .correlation = 22, .value = 1};
    client_readable[1] = 1;
    assert(!service->wait(service->context, 40, 0));
    client_readable[1] = 0;
    assert(!client_packets[1].operation && queue->clients->doorbell_fault);
    pump(service);
    assert(!queue->closing && queue->workers && data_calls == calls);
    detach.correlation = 21;
    assert(service->prepare(service->context, &detach) == PH_LIFECYCLE_SERVICE_IDLE);
    pump(service);
    assert(client_released == 2 && client_closed == 2 && !queue->clients && data_calls == calls);
    for (unsigned i = 0; i < 2; ++i) { free(vmos[3 + i]); vmos[3 + i] = NULL; }
}

static void many_client_wait(struct ph_fs_queue *queue, struct ph_lifecycle_service *service) {
    assert(!queue->clients && !queue->wait_after);
    struct ph_fs_client_queue *clients = calloc(513, sizeof(*clients)); assert(clients);
    for (unsigned i = 0; i < 513; ++i) {
        clients[i].next = i == 512 ? NULL : &clients[i + 1];
        clients[i].lane.bound = 1;
        clients[i].ipc.fd = 100;
        clients[i].watch_fd = 100;
        clients[i].config.channel_id = 1000 + i;
    }
    queue->clients = clients; many_clients = 1;
    unsigned before = wait_calls;
    assert(!service->wait(service->context, 40, 2));
    /* Three probes + one rotated sleep per tick, and a final deadline probe. */
    assert(wait_calls == before + 11);
    assert(queue->wait_after == 1507);
    queue->clients = NULL; queue->wait_after = 0;
    many_clients = 0; free(clients);
}

int main(void) {
    struct ph_fs_queue queue;
    struct ph_lifecycle_service service;
    struct ph_fs_channel_config configs[2] = {
        {.generation = 7, .channel_id = 1, .queue_size = 8, .arena_bytes = 65536},
        {.generation = 7, .channel_id = 2, .queue_size = 8, .arena_bytes = 65536}};
    assert(!ph_fs_queue_init(&queue, (void *)5678, &configs[0], &configs[1],
        &kb2_vq_x86_64_atomics, &service));
    struct kobox_linux_io_service io = {.filesystem = (void *)1234};
    assert(!service.dispatch && !service.next && !service.poll_ready);
    assert(!service.start(service.context, &io));
    uint64_t shared_id, shared_pages[] = { 1, PACHA_VMO_PAGE_VIEW_HOLE };
    assert(!mock_shared.create(mock_shared.context, 7, 999, shared_pages, 2, 1, &shared_id));
    // Before publication the owner's 1/1 snapshot is NOT proof of expiry:
    // FileD has not yet received the authority that it is about to transfer.
    assert(mock_shared.retire(mock_shared.context, shared_id, 0) == 0);
    assert(!shared_queries && !shared_revokes);
    shared_revoke_failure = 1;
    assert(mock_shared.retire(mock_shared.context, shared_id, 1) == -EBUSY);
    assert(shared_revokes == 1);
    assert(mock_shared.refresh(mock_shared.context, shared_id, shared_pages, 2) == -ESTALE);
    uint64_t dirty_bitmap = 0;
    assert(!mock_shared.collect_dirty(mock_shared.context, shared_id, &dirty_bitmap, 1));
    assert(dirty_bitmap == 2); // failed revoke must not block collection/retry
    shared_refresh_failure = 1;
    assert(mock_shared.collect_dirty(mock_shared.context, shared_id, &dirty_bitmap, 1) == -EIO);
    shared_refresh_failure = 0;
    shared_revoke_failure = 0;
    assert(mock_shared.retire(mock_shared.context, shared_id, 1) == 1);
    assert(shared_revokes == 2);
    struct driver drivers[2];
    memset(drivers, 0, sizeof(drivers));
    kb2_vq_chain_t *slots[2][8] = {{0}};
    uint16_t owners[2][8] = {{0}};
    for (unsigned i = 0; i < 2; ++i) {
        size_t offset;
        assert(!ph_fs_channel_layout(&configs[i], &vmo_bytes[i], &offset));
        vmos[i] = aligned_alloc(4096, vmo_bytes[i]); assert(vmos[i]);
        memset(vmos[i], 0, vmo_bytes[i]);
        drivers[i].service = &service; drivers[i].lane = i;
        assert(!ph_fs_channel_bind(&drivers[i].channel, vmos[i], vmo_bytes[i],
            &configs[i], KB2_VQ_DRIVER, &kb2_vq_x86_64_atomics, slots[i], owners[i]));
        struct ph_ipc_packet packet = {.operation = i ? PH_FS_OWNER_BIND : PH_FS_QUEUE_BIND,
            .generation = 7, .value = i + 1, .fd_count = 1,
            .fds = {{.fd = i + 20, .rights = PH_FS_QUEUE_RIGHTS, .flags = PACHA_FD_FLAG_CLOEXEC}}};
        assert(service.prepare(service.context, &packet) == PH_LIFECYCLE_SERVICE_IDLE);
    }
    unsigned char request[KB2_FILESYSTEM_REQUEST_SIZE], response[64 + 16384];
    for (unsigned i = 0; i < 3; ++i) {
        unsigned owner = i == 1;
        struct driver *driver = &drivers[owner];
        kb2_fs_request_t r = {.opcode = owner ? KB2_FILESYSTEM_OP_HELLO :
            i == 2 ? KB2_FILESYSTEM_OP_CLIENT_REGISTER : KB2_FILESYSTEM_OP_PREAD,
            .generation = 7, .sequence = i + 1, .client = 7,
            .credential_generation = 1, .length = 16384};
        assert(!kb2_fs_request_encode(request, sizeof(request), sizeof(request), &r));
        struct ph_fs_channel_wait callbacks = {.context = driver, .notify = notify, .wait = wait};
        size_t used;
        driver->mutate = !owner;
        assert(!ph_fs_channel_call(&driver->channel, request, sizeof(request),
            response, sizeof(response), &used, &callbacks));
        kb2_fs_response_t decoded;
        assert(!kb2_fs_response_decode(response, used, &decoded));
        assert(decoded.result == (owner ? 0 : i == 2 ? -EPERM : 16384));
    }
    assert(data_calls == 1 && owner_calls == 1);
    /* The same read/stat/close opcodes select a namespace only through the
     * capability-bound lane; claiming a private handle cannot promote it. */
    const uint32_t owner_ops[] = {KB2_FILESYSTEM_OP_EXEC_OPEN, KB2_FILESYSTEM_OP_PREAD,
        KB2_FILESYSTEM_OP_STATX, KB2_FILESYSTEM_OP_CLOSE};
    for (unsigned i = 0; i < 5; ++i) {
        const unsigned control = i < 4;
        kb2_fs_request_t r = {.opcode = control ? owner_ops[i] : KB2_FILESYSTEM_OP_EXEC_OPEN,
            .generation = 7, .sequence = i + 8, .client = 7,
            .credential_generation = 1, .handle = 88, .length = 16};
        assert(!kb2_fs_request_encode(request, sizeof(request), sizeof(request), &r));
        struct driver *driver = &drivers[control];
        struct ph_fs_channel_wait callbacks = {.context = driver, .notify = notify, .wait = wait};
        size_t used;
        assert(!ph_fs_channel_call(&driver->channel, request, sizeof(request),
            response, sizeof(response), &used, &callbacks));
        kb2_fs_response_t reply;
        assert(!kb2_fs_response_decode(response, used, &reply));
        assert(reply.result == (!control ? -EPERM :
            r.opcode == KB2_FILESYSTEM_OP_PREAD ? 16 : 0));
    }
    assert(data_calls == 1 && owner_calls == 5);
    temporary_exchange(&service, 3, 0, 0);
    temporary_exchange(&service, 4, 0, 0);
    temporary_exchange(&service, 5, 1, 0);
    temporary_exchange(&service, 6, 0, 1);
    assert(data_calls == 3 && owner_calls == 6 && released == 4);
    /* Global sync carries no handle; syncfs carries a real file handle.
     * Both remain ordinary actor requests, never owner-lane authority. */
    const uint32_t sync_ops[] = {KB2_FILESYSTEM_OP_SYNC, KB2_FILESYSTEM_OP_SYNCFS};
    for (unsigned i = 0; i < 2; ++i) {
        kb2_fs_request_t r = {.opcode = sync_ops[i], .generation = 7,
            .sequence = 100 + i, .client = 7, .credential_generation = 1,
            .handle = i ? 42 : 0};
        assert(!kb2_fs_request_encode(request, sizeof(request), sizeof(request), &r));
        struct ph_fs_channel_wait callbacks = {.context = &drivers[0],
            .notify = notify, .wait = wait};
        size_t used;
        assert(!ph_fs_channel_call(&drivers[0].channel, request, sizeof(request),
            response, sizeof(response), &used, &callbacks));
        kb2_fs_response_t decoded;
        assert(!kb2_fs_response_decode(response, used, &decoded) && !decoded.result);
    }
    assert(data_calls == 5 && owner_calls == 6);
    direct_clients(&service, &drivers[1]);
    many_client_wait(&queue, &service);
    {
        kb2_fs_request_t r = {.opcode = KB2_FILESYSTEM_OP_SHARED_FILE_VMO,
            .generation = 7, .sequence = 10000, .client = 7,
            .credential_generation = 1, .handle = 42, .length = 8192, .flags = 3};
        assert(!kb2_fs_request_encode(request, sizeof(request), sizeof(request), &r));
        struct ph_fs_channel_wait callbacks = {.context = &drivers[1], .notify = notify, .wait = wait};
        size_t used;
        assert(!ph_fs_channel_call(&drivers[1].channel, request, sizeof(request),
            response, sizeof(response), &used, &callbacks));
        kb2_fs_response_t reply;
        assert(!kb2_fs_response_decode(response, used, &reply) && reply.result == 4096);
        assert(shared_attachment.value == reply.handle && shared_attachment.correlation == r.sequence);
        shared_extra = UINT64_C(2) | (UINT64_C(1) << 32);
        assert(mock_shared.retire(mock_shared.context, reply.handle, 0) == 0);
        shared_extra = UINT64_C(1) | (UINT64_C(2) << 32);
        assert(mock_shared.retire(mock_shared.context, reply.handle, 0) == 0);
        shared_refresh_failure = 1;
        assert(mock_shared.refresh(mock_shared.context, reply.handle, shared_pages, 2) == -EIO);
        shared_refresh_failure = 0;
        assert(!mock_shared.refresh(mock_shared.context, reply.handle, shared_pages, 2));
        shared_extra = UINT64_C(1) | (UINT64_C(1) << 32);
        assert(mock_shared.retire(mock_shared.context, reply.handle, 0) == 1);
    }
    assert(service.advance(service.context, &io, NULL, true) == 1);
    assert(!service.finish(service.context, &io));
    assert(!service.stop(service.context));
    assert(!ph_fs_queue_finish(&queue) && !allocations);
    free(vmos[0]); free(vmos[1]);
    vmos[0] = vmos[1] = NULL;
    for (unsigned scenario = 0; scenario < 4; ++scenario) rejected_bind(scenario);
    struct ph_wait_retained retained = {0};
    struct pacha_pollfd fds[2] = {
        {.fd = 40, .events = PACHA_FD_EVENT_READABLE},
        {.fd = 31, .events = PACHA_FD_EVENT_READABLE},
    };
    unsigned replace_before = retained_replaces, reuse_before = retained_reuses;
    assert(ph_wait_many_retained(&retained, fds, 2, 1) == PACHA_SYSCALL_ERR_NOT_READY);
    assert(ph_wait_many_retained(&retained, fds, 2, 1) == PACHA_SYSCALL_ERR_NOT_READY);
    fds[1].events |= PACHA_FD_EVENT_HANGUP;
    assert(ph_wait_many_retained(&retained, fds, 2, 1) == PACHA_SYSCALL_ERR_NOT_READY);
    assert(retained_replaces == replace_before + 2 && retained_reuses == reuse_before + 1);
    assert(snapshot_polls > 0);
    puts("kobox2 FS queue: worker publication before collect, descriptor reuse, sender retirement, snapshots, out-of-order completion, client barrier, quota rollback, drain-before-ACK, forgery isolation, 513-client wait, oversized exchange PASS");
    return 0;
}
