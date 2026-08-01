#include <stdint.h>
#include <string.h>

#include "../userland/inputd/src/input_island.c"

enum {
    TEST_EV_KEY = 1,
    TEST_EV_REL = 2,
    TEST_REL_X = 0,
    TEST_REL_Y = 1,
};

static inputd_registry_entry_t test_registry[2];
static kb_input_device_snapshot_t ioctl_snapshot;

int kb_input_subsystem_for_each_device(
    int (*callback)(const kb_input_device_snapshot_t *device, void *ctx),
    void *ctx)
{
    return callback(&ioctl_snapshot, ctx);
}

static void reset_inputd(size_t device_count)
{
    memset(handles, 0, sizeof(handles));
    memset(transfer_leases, 0, sizeof(transfer_leases));
    memset(event_ring, 0, sizeof(event_ring));
    memset(test_registry, 0, sizeof(test_registry));
    memset(&ioctl_snapshot, 0, sizeof(ioctl_snapshot));
    event_head = 0;
    event_count = 0;
    next_handle = 1;
    next_sequence = 1;
    next_frame_sequence = 1;
    next_source_generation = 1;
    wait_generation = 1;
    notify_ready_mask = 0;
    static struct inputd_input_island island;
    island = (struct inputd_input_island){
        .registry = test_registry,
        .device_count = device_count,
        .ready = 1,
    };
    active_island = &island;
}

static void setup_device(size_t index, uint32_t device_id)
{
    test_registry[index].kobox_device_id = device_id;
    test_registry[index].public_device.event_index = (uint32_t)index;
}

static void setup_handle(uint32_t device_id, uint64_t cursor)
{
    handles[0] = (inputd_handle_t){
        .active = 1,
        .refs = 1,
        .device_id = device_id,
        .clock_id = INPUTD_CLOCK_MONOTONIC,
        .id = 1,
        .cursor = cursor,
        .readable = 1,
    };
    inputd_registry_entry_t *entry = lookup_registry_by_device_id(device_id);
    entry->handle_mask = 1;
}

static void append_test_event(
    uint32_t device_id,
    uint16_t type,
    uint16_t code,
    int32_t value)
{
    const kb_input_event_t event = {
        .device_id = device_id,
        .type = type,
        .code = code,
        .value = value,
    };
    append_event(&event, next_frame_sequence++, UINT64_C(1000000000));
    inputd_registry_entry_t *entry = lookup_registry_by_device_id(device_id);
    entry->latest_sequence = next_sequence - 1u;
}

static void install_recovery_ring(void)
{
    static const inputd_raw_event_t events[] = {
        { .sequence = 20, .device_id = 7, .type = TEST_EV_REL,
          .code = TEST_REL_X, .value = 10, .monotonic_seconds = 1 },
        { .sequence = 21, .device_id = 7, .type = INPUTD_EV_SYN,
          .code = INPUTD_SYN_REPORT, .monotonic_seconds = 1 },
        { .sequence = 22, .device_id = 7, .type = TEST_EV_REL,
          .code = TEST_REL_X, .value = 77, .monotonic_seconds = 2 },
        { .sequence = 23, .device_id = 7, .type = TEST_EV_REL,
          .code = TEST_REL_Y, .value = -3, .monotonic_seconds = 2 },
        { .sequence = 24, .device_id = 7, .type = INPUTD_EV_SYN,
          .code = INPUTD_SYN_REPORT, .monotonic_seconds = 2 },
    };
    memcpy(event_ring, events, sizeof(events));
    event_head = 0;
    event_count = sizeof(events) / sizeof(events[0]);
    next_sequence = 25;
    test_registry[0].overwritten_sequence = 19;
    test_registry[0].latest_sequence = 24;
}

static int test_other_device_overflow_does_not_drop(void)
{
    reset_inputd(2);
    setup_device(0, 7);
    setup_device(1, 8);

    append_test_event(7, INPUTD_EV_SYN, INPUTD_SYN_REPORT, 0);
    setup_handle(7, 2);
    handles[1] = (inputd_handle_t){
        .active = 1,
        .refs = 1,
        .device_id = 7,
        .clock_id = INPUTD_CLOCK_MONOTONIC,
        .id = 2,
        .cursor = 1,
        .readable = 1,
    };
    test_registry[0].handle_mask |= UINT32_C(1) << 1;
    for (size_t i = 0; i < INPUTD_RING_MAX; i++)
        append_test_event(8, TEST_EV_REL, TEST_REL_X, (int32_t)i);
    append_test_event(7, TEST_EV_REL, TEST_REL_X, 41);
    append_test_event(7, INPUTD_EV_SYN, INPUTD_SYN_REPORT, 0);

    if (test_registry[0].overwritten_sequence != 1) return 1;
    inputd_read_request_t request = {
        .handle = 1,
        .event_capacity = INPUTD_EVENT_CAPACITY,
    };
    if (inputd_input_read(&request) != 0) return 2;
    if (request.event_count != 2) return 3;
    if (request.events[0].type != TEST_EV_REL ||
        request.events[0].code != TEST_REL_X ||
        request.events[0].value != 41) return 4;
    if (request.events[1].type != INPUTD_EV_SYN ||
        request.events[1].code != INPUTD_SYN_REPORT) return 5;

    request.handle = 2;
    if (inputd_input_read(&request) != 0) return 6;
    if (request.event_count != 2) return 7;
    if (request.events[0].type != INPUTD_EV_SYN ||
        request.events[0].code != INPUTD_SYN_DROPPED) return 8;
    if (request.events[1].type != INPUTD_EV_SYN ||
        request.events[1].code != INPUTD_SYN_REPORT) return 9;
    return 0;
}

static int test_recovery_keeps_following_frame(void)
{
    reset_inputd(1);
    setup_device(0, 7);
    setup_handle(7, 19);
    install_recovery_ring();

    inputd_read_request_t request = {
        .handle = 1,
        .event_capacity = INPUTD_EVENT_CAPACITY,
    };
    if (inputd_input_read(&request) != 0) return 1;
    if (request.event_count != 5) return 2;
    if (request.events[0].type != INPUTD_EV_SYN ||
        request.events[0].code != INPUTD_SYN_DROPPED) return 3;
    if (request.events[1].type != INPUTD_EV_SYN ||
        request.events[1].code != INPUTD_SYN_REPORT ||
        request.events[1].seconds != 1) return 4;
    if (request.events[2].type != TEST_EV_REL ||
        request.events[2].code != TEST_REL_X ||
        request.events[2].value != 77) return 5;
    if (request.events[3].type != TEST_EV_REL ||
        request.events[3].code != TEST_REL_Y ||
        request.events[3].value != -3) return 6;
    if (request.events[4].type != INPUTD_EV_SYN ||
        request.events[4].code != INPUTD_SYN_REPORT) return 7;
    if (handles[0].cursor != next_sequence || handles[0].readable ||
        handles[0].resyncing || handles[0].resync_report_pending) return 8;
    return 0;
}

static int test_capacity_one_preserves_recovery_report(void)
{
    reset_inputd(1);
    setup_device(0, 7);
    setup_handle(7, 19);
    install_recovery_ring();

    inputd_read_request_t request = { .handle = 1, .event_capacity = 1 };
    if (inputd_input_read(&request) != 0 || request.event_count != 1) return 1;
    if (request.events[0].type != INPUTD_EV_SYN ||
        request.events[0].code != INPUTD_SYN_DROPPED) return 2;
    if (handles[0].resyncing || !handles[0].resync_report_pending ||
        handles[0].cursor != 22 || !handles[0].readable) return 3;

    if (inputd_input_read(&request) != 0 || request.event_count != 1) return 4;
    if (request.events[0].type != INPUTD_EV_SYN ||
        request.events[0].code != INPUTD_SYN_REPORT ||
        request.events[0].seconds != 1) return 5;
    if (handles[0].resync_report_pending || handles[0].cursor != 22 ||
        !handles[0].readable) return 6;

    if (inputd_input_read(&request) != 0 || request.event_count != 1 ||
        request.events[0].type != TEST_EV_REL || request.events[0].value != 77) return 7;
    if (inputd_input_read(&request) != 0 || request.event_count != 1 ||
        request.events[0].type != TEST_EV_REL || request.events[0].value != -3) return 8;
    if (inputd_input_read(&request) != 0 || request.event_count != 1 ||
        request.events[0].type != INPUTD_EV_SYN ||
        request.events[0].code != INPUTD_SYN_REPORT) return 9;
    if (handles[0].cursor != next_sequence || handles[0].readable) return 10;
    return 0;
}

static int test_pending_report_does_not_hide_next_loss(void)
{
    reset_inputd(2);
    setup_device(0, 7);
    setup_device(1, 8);
    setup_handle(7, 19);
    install_recovery_ring();

    inputd_read_request_t request = { .handle = 1, .event_capacity = 1 };
    if (inputd_input_read(&request) != 0 ||
        !handles[0].resync_report_pending || handles[0].cursor != 22) return 1;

    for (size_t i = 0; i < INPUTD_RING_MAX; i++) {
        event_ring[i] = (inputd_raw_event_t){
            .sequence = 31u + i,
            .device_id = 8,
            .type = TEST_EV_REL,
            .code = TEST_REL_X,
            .value = (int32_t)i,
            .monotonic_seconds = 3,
        };
    }
    event_head = 0;
    event_count = INPUTD_RING_MAX;
    next_sequence = 31u + INPUTD_RING_MAX;
    test_registry[0].overwritten_sequence = 30;
    test_registry[0].latest_sequence = 30;
    test_registry[1].latest_sequence = next_sequence - 1u;

    if (inputd_input_read(&request) != 0 || request.event_count != 1) return 2;
    if (request.events[0].type != INPUTD_EV_SYN ||
        request.events[0].code != INPUTD_SYN_REPORT ||
        !handles[0].resync_drop_pending || handles[0].cursor != 22 ||
        !handles[0].readable) return 3;

    if (inputd_input_read(&request) != 0 || request.event_count != 1) return 4;
    if (request.events[0].type != INPUTD_EV_SYN ||
        request.events[0].code != INPUTD_SYN_DROPPED) return 5;
    if (handles[0].resync_drop_pending || !handles[0].resyncing ||
        handles[0].cursor != next_sequence) return 6;
    return 0;
}

static uint64_t state_ioctl(uint32_t nr, uint32_t size)
{
    return UINT64_C(0x80000000) | ((uint64_t)size << 16) |
        ((uint64_t)'E' << 8) | nr;
}

static int test_state_ioctls_return_snapshot(void)
{
    reset_inputd(1);
    setup_device(0, 7);
    setup_handle(7, 1);
    ioctl_snapshot.id = 7;
    ioctl_snapshot.key_state[2] = UINT64_C(0x123456789abcdef0);
    ioctl_snapshot.led_state = UINT64_C(0x21);
    ioctl_snapshot.snd_state = UINT64_C(0x42);
    ioctl_snapshot.sw_state = UINT64_C(0x84);

#define TEST_STATE_IOCTL(number, field, error) do { \
    inputd_ioctl_request_t request = { \
        .handle = 1, \
        .request = state_ioctl((number), sizeof(ioctl_snapshot.field)), \
        .data_size = sizeof(ioctl_snapshot.field), \
    }; \
    if (inputd_input_ioctl(&request) != 0 || \
        request.result_size != sizeof(ioctl_snapshot.field) || \
        memcmp(request.data, &ioctl_snapshot.field, sizeof(ioctl_snapshot.field)) != 0) \
        return (error); \
} while (0)
    TEST_STATE_IOCTL(0x18u, key_state, 1);
    TEST_STATE_IOCTL(0x19u, led_state, 2);
    TEST_STATE_IOCTL(0x1au, snd_state, 3);
    TEST_STATE_IOCTL(0x1bu, sw_state, 4);
#undef TEST_STATE_IOCTL
    return 0;
}

static int test_state_ioctl_flushes_pending_type(void)
{
    reset_inputd(1);
    setup_device(0, 7);
    setup_handle(7, 1);
    ioctl_snapshot.id = 7;
    static const inputd_raw_event_t events[] = {
        { .sequence = 1, .device_id = 7, .type = TEST_EV_KEY,
          .code = 30, .value = 1 },
        { .sequence = 2, .device_id = 7, .type = INPUTD_EV_SYN,
          .code = INPUTD_SYN_REPORT },
        { .sequence = 3, .device_id = 7, .type = TEST_EV_KEY,
          .code = 30, .value = 0 },
        { .sequence = 4, .device_id = 7, .type = INPUTD_EV_SYN,
          .code = INPUTD_SYN_REPORT },
        { .sequence = 5, .device_id = 7, .type = TEST_EV_REL,
          .code = TEST_REL_X, .value = 9 },
        { .sequence = 6, .device_id = 7, .type = INPUTD_EV_SYN,
          .code = INPUTD_SYN_REPORT },
    };
    memcpy(event_ring, events, sizeof(events));
    event_count = sizeof(events) / sizeof(events[0]);
    next_sequence = 7;
    test_registry[0].latest_sequence = 6;

    inputd_ioctl_request_t ioctl_request = {
        .handle = 1,
        .request = state_ioctl(0x18u, sizeof(ioctl_snapshot.key_state)),
        .data_size = sizeof(ioctl_snapshot.key_state),
    };
    if (inputd_input_ioctl(&ioctl_request) != 0 ||
        !handles[0].state_flush_active) return 1;

    inputd_read_request_t read_request = {
        .handle = 1,
        .event_capacity = INPUTD_EVENT_CAPACITY,
    };
    if (inputd_input_read(&read_request) != 0 ||
        read_request.event_count != 3) return 2;
    if (read_request.events[0].type != INPUTD_EV_SYN ||
        read_request.events[0].code != INPUTD_SYN_REPORT) return 3;
    if (read_request.events[1].type != TEST_EV_REL ||
        read_request.events[1].value != 9) return 4;
    if (read_request.events[2].type != INPUTD_EV_SYN ||
        read_request.events[2].code != INPUTD_SYN_REPORT) return 5;
    if (handles[0].cursor != next_sequence || handles[0].readable ||
        handles[0].state_flush_active) return 6;
    return 0;
}

int main(void)
{
    int status = test_other_device_overflow_does_not_drop();
    if (status != 0) return 10 + status;
    status = test_recovery_keeps_following_frame();
    if (status != 0) return 20 + status;
    status = test_capacity_one_preserves_recovery_report();
    if (status != 0) return 40 + status;
    status = test_pending_report_does_not_hide_next_loss();
    if (status != 0) return 55 + status;
    status = test_state_ioctls_return_snapshot();
    if (status != 0) return 70 + status;
    status = test_state_ioctl_flushes_pending_type();
    if (status != 0) return 80 + status;
    return 0;
}
