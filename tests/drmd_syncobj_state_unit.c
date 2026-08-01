#include "drm_syncobj_state.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { TEST_FD_MAX = 4096 };

typedef struct fake_fds {
    uint8_t readable[TEST_FD_MAX];
    uint8_t failed[TEST_FD_MAX];
    uint8_t closed[TEST_FD_MAX];
    int peer[TEST_FD_MAX];
    uint8_t signal_fails[TEST_FD_MAX];
    unsigned signals;
    unsigned closes;
} fake_fds_t;

static int fake_poll(void *context, int fd)
{
    fake_fds_t *fds = context;
    assert(fd >= 0 && fd < TEST_FD_MAX);
    assert(!fds->closed[fd]);
    return fds->failed[fd] ? -5 : fds->readable[fd] != 0;
}

static int fake_signal(void *context, int fd)
{
    fake_fds_t *fds = context;
    assert(fd >= 0 && fd < TEST_FD_MAX);
    assert(!fds->closed[fd]);
    if (fds->signal_fails[fd]) return -5;
    assert(fds->peer[fd] >= 0 && fds->peer[fd] < TEST_FD_MAX);
    fds->readable[fds->peer[fd]] = 1;
    fds->signals++;
    return 0;
}

static void fake_close(void *context, int fd)
{
    fake_fds_t *fds = context;
    assert(fd >= 0 && fd < TEST_FD_MAX);
    assert(!fds->closed[fd]);
    fds->closed[fd] = 1;
    fds->closes++;
}

static void reopen(fake_fds_t *fds, int fd)
{
    assert(fd >= 0 && fd < TEST_FD_MAX);
    fds->closed[fd] = 0;
    fds->readable[fd] = 0;
    fds->failed[fd] = 0;
    fds->signal_fails[fd] = 0;
}

static void pair(fake_fds_t *fds, int notify, int wait)
{
    reopen(fds, notify);
    reopen(fds, wait);
    fds->peer[notify] = wait;
    fds->peer[wait] = notify;
}

int main(void)
{
    fake_fds_t fds;
    memset(&fds, 0, sizeof(fds));
    for (int i = 0; i < TEST_FD_MAX; i++) fds.peer[i] = -1;
    const drmd_syncobj_fd_ops_t ops = {
        .poll = fake_poll,
        .signal = fake_signal,
        .close = fake_close,
    };
    drmd_syncobj_state_t state;
    drmd_syncobj_state_init(&state, &ops, &fds);

    uint32_t imported = 0;
    uint32_t scene = 0;
    uint32_t exported = 0;
    assert(drmd_syncobj_create(&state, 7, 0, &imported) == 0);
    assert(drmd_syncobj_create(&state, 7, 0, &scene) == 0);
    assert(drmd_syncobj_create(&state, 7, 0, &exported) == 0);
    reopen(&fds, 20);
    assert(drmd_syncobj_import_sync_file(&state, 7, imported, 1, 20) == 0);
    assert(drmd_syncobj_transfer(&state, 7, imported, 0, scene, 9, 0) == 0);
    assert(drmd_syncobj_destroy(&state, 7, imported) == 0);
    assert(drmd_syncobj_transfer(&state, 7, scene, 9, exported, 0, 0) == 0);
    pair(&fds, 30, 31);
    assert(drmd_syncobj_export_sync_file(&state, 7, exported, 1, 30) == 0);
    assert(drmd_syncobj_destroy(&state, 7, exported) == 0);
    assert(!fds.readable[31]);
    assert(drmd_syncobj_active_fence_count(&state) == 1);
    assert(drmd_syncobj_active_export_count(&state) == 1);

    fds.readable[20] = 1;
    drmd_syncobj_progress(&state);
    assert(fds.readable[31]);
    assert(fds.closed[20]);
    assert(fds.closed[30]);
    assert(drmd_syncobj_active_point_count(&state) == 0);
    assert(drmd_syncobj_active_fence_count(&state) == 0);
    assert(drmd_syncobj_active_export_count(&state) == 0);

    uint32_t failed_source = 0;
    uint32_t failed_destination = 0;
    reopen(&fds, 50);
    pair(&fds, 51, 52);
    assert(drmd_syncobj_create(&state, 12, 0, &failed_source) == 0);
    assert(drmd_syncobj_create(&state, 12, 0, &failed_destination) == 0);
    assert(drmd_syncobj_import_sync_file(&state, 12, failed_source, 1, 50) == 0);
    assert(drmd_syncobj_transfer(
        &state, 12, failed_source, 0, failed_destination, 0, 0) == 0);
    assert(drmd_syncobj_export_sync_file(
        &state, 12, failed_destination, 1, 51) == 0);
    fds.failed[50] = 1;
    drmd_syncobj_progress(&state);
    assert(fds.closed[50]);
    assert(fds.closed[51]);
    assert(!fds.readable[52]);
    assert(fds.signals == 1);
    drmd_syncobj_owner_close(&state, 12);
    assert(drmd_syncobj_active_fence_count(&state) == 0);
    assert(drmd_syncobj_destroy(&state, 7, scene) == 0);

    uint32_t signal_fail_source = 0;
    uint32_t signal_fail_destination = 0;
    reopen(&fds, 60);
    fds.readable[60] = 1;
    pair(&fds, 61, 62);
    fds.signal_fails[61] = 1;
    assert(drmd_syncobj_create(&state, 13, 0, &signal_fail_source) == 0);
    assert(drmd_syncobj_create(&state, 13, 0, &signal_fail_destination) == 0);
    assert(drmd_syncobj_import_sync_file(
        &state, 13, signal_fail_source, 1, 60) == 0);
    assert(drmd_syncobj_transfer(
        &state, 13, signal_fail_source, 0, signal_fail_destination, 0, 0) == 0);
    assert(drmd_syncobj_export_sync_file(
        &state, 13, signal_fail_destination, 1, 61) == -5);
    assert(!fds.closed[61]);
    fake_close(&fds, 61);
    drmd_syncobj_owner_close(&state, 13);

    uint32_t close_source = 0;
    uint32_t close_destination = 0;
    reopen(&fds, 40);
    pair(&fds, 41, 42);
    assert(drmd_syncobj_create(&state, 11, 0, &close_source) == 0);
    assert(drmd_syncobj_create(&state, 11, 0, &close_destination) == 0);
    assert(drmd_syncobj_import_sync_file(&state, 11, close_source, 1, 40) == 0);
    assert(drmd_syncobj_transfer(
        &state, 11, close_source, 0, close_destination, 0, 0) == 0);
    assert(drmd_syncobj_export_sync_file(
        &state, 11, close_destination, 1, 41) == 0);
    drmd_syncobj_owner_close(&state, 11);
    assert(fds.closed[40]);
    assert(fds.closed[41]);
    assert(!fds.readable[42]);
    assert(drmd_syncobj_active_object_count(&state) == 0);
    assert(drmd_syncobj_active_point_count(&state) == 0);
    assert(drmd_syncobj_active_fence_count(&state) == 0);
    assert(drmd_syncobj_active_export_count(&state) == 0);

    for (unsigned iteration = 0; iteration < 10000; iteration++) {
        uint32_t source = 0;
        uint32_t destination = 0;
        const int input_fd = 100 + (int)(iteration % 1000);
        const int notify_fd = 1200 + (int)(iteration % 1000);
        const int wait_fd = 2200 + (int)(iteration % 1000);
        reopen(&fds, input_fd);
        fds.readable[input_fd] = 1;
        pair(&fds, notify_fd, wait_fd);
        assert(drmd_syncobj_create(&state, 9, 0, &source) == 0);
        assert(drmd_syncobj_create(&state, 9, 0, &destination) == 0);
        assert(drmd_syncobj_import_sync_file(&state, 9, source, 1, input_fd) == 0);
        assert(drmd_syncobj_transfer(&state, 9, source, 0, destination, 0, 0) == 0);
        assert(drmd_syncobj_export_sync_file(&state, 9, destination, 1, notify_fd) == 0);
        assert(fds.readable[wait_fd]);
        assert(drmd_syncobj_destroy(&state, 9, source) == 0);
        assert(drmd_syncobj_destroy(&state, 9, destination) == 0);
        assert(drmd_syncobj_active_object_count(&state) == 0);
        assert(drmd_syncobj_active_point_count(&state) == 0);
        assert(drmd_syncobj_active_fence_count(&state) == 0);
        assert(drmd_syncobj_active_export_count(&state) == 0);
    }

    drmd_syncobj_state_finish(&state);
    puts("drmd syncobj state unit: ok");
    return 0;
}
