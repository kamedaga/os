#include "input_island.h"

#include <kobox/device_pachaos_capsule.h>
#include <kobox/module.h>
#include <kobox/platform.h>
#include <kobox/shim.h>
#include "linux_subsystem/input/input.h"
#include "linux_subsystem/kvm/kvm_symbols.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    INPUTD_HANDLE_MAX = 32,
    INPUTD_RING_MAX = 4096,
    INPUTD_EV_SYN = 0,
    INPUTD_SYN_DROPPED = 3,
    INPUTD_CLOCK_REALTIME = 0,
    INPUTD_CLOCK_MONOTONIC = 1,
    INPUTD_EVIOCGVERSION = 0x80044501u,
    INPUTD_EVIOCGID = 0x80084502u,
    INPUTD_EVIOCGREP = 0x80084503u,
    INPUTD_EVIOCGRAB = 0x40044590u,
    INPUTD_EVIOCREVOKE = 0x40044591u,
    INPUTD_EVIOCSCLOCKID = 0x400445a0u,
};

typedef struct inputd_raw_event {
    uint64_t sequence;
    uint32_t device_id;
    uint16_t type;
    uint16_t code;
    int32_t value;
    int64_t monotonic_seconds;
    int64_t monotonic_microseconds;
} inputd_raw_event_t;

typedef struct inputd_handle {
    int active;
    int revoked;
    uint32_t refs;
    uint32_t device_id;
    uint32_t flags;
    int clock_id;
    uint64_t id;
    uint64_t cursor;
} inputd_handle_t;

static inputd_handle_t handles[INPUTD_HANDLE_MAX];
static inputd_raw_event_t event_ring[INPUTD_RING_MAX];
static size_t event_head;
static size_t event_count;
static uint64_t next_handle = 1;
static uint64_t next_sequence = 1;
static uint64_t grabbed_handle_by_device[64];

static inputd_handle_t *find_handle(uint64_t id)
{
    for (size_t i = 0; i < INPUTD_HANDLE_MAX; i++) {
        if (handles[i].active && handles[i].id == id) return &handles[i];
    }
    return NULL;
}

static inputd_handle_t *alloc_handle(void)
{
    for (size_t i = 0; i < INPUTD_HANDLE_MAX; i++) {
        if (!handles[i].active) {
            memset(&handles[i], 0, sizeof(handles[i]));
            handles[i].active = 1;
            handles[i].refs = 1;
            handles[i].clock_id = INPUTD_CLOCK_REALTIME;
            handles[i].id = next_handle++;
            if (next_handle == 0) next_handle = 1;
            return &handles[i];
        }
    }
    return NULL;
}

typedef struct inputd_device_lookup {
    uint32_t wanted_index;
    uint32_t current_index;
    uint32_t wanted_id;
    kb_input_device_snapshot_t snapshot;
    int found;
} inputd_device_lookup_t;

static int lookup_device_callback(const kb_input_device_snapshot_t *device, void *opaque)
{
    inputd_device_lookup_t *lookup = opaque;
    if ((lookup->wanted_id != 0 && device->id == lookup->wanted_id) ||
        (lookup->wanted_id == 0 && lookup->current_index == lookup->wanted_index)) {
        lookup->snapshot = *device;
        lookup->found = 1;
        return 1;
    }
    lookup->current_index++;
    return 0;
}

static int lookup_device_by_index(uint32_t index, kb_input_device_snapshot_t *out)
{
    inputd_device_lookup_t lookup = { .wanted_index = index };
    (void)kb_input_subsystem_for_each_device(lookup_device_callback, &lookup);
    if (!lookup.found) return -19;
    *out = lookup.snapshot;
    return 0;
}

static int lookup_device_by_id(uint32_t id, kb_input_device_snapshot_t *out)
{
    inputd_device_lookup_t lookup = { .wanted_id = id };
    (void)kb_input_subsystem_for_each_device(lookup_device_callback, &lookup);
    if (!lookup.found) return -19;
    *out = lookup.snapshot;
    return 0;
}

static void append_event(const kb_input_event_t *event)
{
    size_t slot = (event_head + event_count) % INPUTD_RING_MAX;
    if (event_count == INPUTD_RING_MAX) {
        slot = event_head;
        event_head = (event_head + 1u) % INPUTD_RING_MAX;
    } else {
        event_count++;
    }
    struct timespec ts = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    event_ring[slot] = (inputd_raw_event_t){
        .sequence = next_sequence++,
        .device_id = event->device_id,
        .type = (uint16_t)event->type,
        .code = (uint16_t)event->code,
        .value = event->value,
        .monotonic_seconds = ts.tv_sec,
        .monotonic_microseconds = ts.tv_nsec / 1000,
    };
}

void inputd_input_island_pump(struct inputd_input_island *island)
{
    (void)island;
    for (unsigned pass = 0; pass < 8; pass++) {
        (void)kb_handle_any_irq(0);
        kb_run_deferred_work();
        kb_input_event_t events[128];
        const size_t count = kb_input_subsystem_pop_events(events, 128);
        for (size_t i = 0; i < count; i++) append_event(&events[i]);
        if (count < 128) break;
    }
}

int inputd_input_island_init(
    struct inputd_input_island *island,
    const struct inputd_boot_config *cfg)
{
    if (island == NULL || cfg == NULL || cfg->device_count != INPUTD_DEVICE_COUNT ||
        cfg->module_count != INPUTD_MAX_MODULES) return -22;
    memset(island, 0, sizeof(*island));
    (void)setenv("KOBOX_DEVICE_BACKEND", "pachaos", 1);
    (void)setenv("KOBOX_PCI_LAYOUT", "arch68", 1);
    (void)setenv("KOBOX_VIRTIO_NO_INDIRECT", "1", 1);
    (void)setenv("KOBOX_VIRTIO_NO_EVENT_IDX", "1", 1);
    (void)setenv("KOBOX_INPUT_TRUST_DEVICE_STRINGS", "1", 1);

    kb_device_backend_t *backend = NULL;
    kb_status_t status = kb_pachaos_capsule_devices_create(
        cfg->device_fds, INPUTD_DEVICE_COUNT, &backend);
    if (status != KB_OK || backend == NULL) return -5;
    island->device_backend = backend;
    kb_shim_set_device_backend(backend);
    if (kb_kvm_prepare_dma_arena(backend) != 0) return -5;
    const kb_platform_desc_t platform_desc = {
        .name = "inputd-virtio-input-island",
        .device_backend = backend,
        .interfaces = NULL,
        .interface_count = 0,
    };
    kb_platform_t *platform = NULL;
    if (kb_platform_create(&platform_desc, &platform) != KB_OK) return -5;

    for (uint32_t i = 0; i < INPUTD_MAX_MODULES; i++) {
        const struct inputd_module_config *module = &cfg->modules[i];
        if (module->image_va == 0 || module->image_size == 0 || module->name[0] == '\0') return -22;
        const kb_module_image_t image = {
            .data = (const void *)(uintptr_t)module->image_va,
            .size = (size_t)module->image_size,
            .name = module->name,
        };
        printf("[inputd] module open name=%s bytes=%llu\n", module->name,
            (unsigned long long)module->image_size);
        status = kb_module_open_image(&image, backend, (kb_module_t **)&island->modules[i]);
        if (status != KB_OK || island->modules[i] == NULL) return -5;
        island->loaded_module_count++;
        int init_result = 0;
        status = kb_module_call_init((kb_module_t *)island->modules[i], &init_result);
        if (status != KB_OK && status != KB_ERR_NOT_FOUND) return -5;
        if (status == KB_OK && init_result != 0) return init_result;
        printf("[inputd] module ready name=%s init=%d\n", module->name, init_result);
    }

    for (unsigned i = 0; i < 64; i++) {
        kb_run_deferred_work();
        (void)kb_handle_any_irq(0);
        if (kb_input_subsystem_device_count() == INPUTD_DEVICE_COUNT) break;
    }
    island->device_count = (uint32_t)kb_input_subsystem_device_count();
    if (island->device_count != INPUTD_DEVICE_COUNT) return -19;
    const int opened = kb_input_subsystem_open_registered_devices();
    if (opened != INPUTD_DEVICE_COUNT) return -5;
    kb_input_subsystem_print_summary(stdout);
    island->ready = 1;
    return 0;
}

int inputd_input_open(uint32_t event_index, uint32_t flags, uint64_t *out_handle)
{
    if (out_handle == NULL) return -22;
    kb_input_device_snapshot_t device;
    int status = lookup_device_by_index(event_index, &device);
    if (status != 0) return status;
    inputd_handle_t *handle = alloc_handle();
    if (handle == NULL) return -24;
    handle->device_id = device.id;
    handle->flags = flags;
    handle->cursor = next_sequence;
    *out_handle = handle->id;
    printf("[inputd] open event%u handle=%llu device=%u name=%s\n", event_index,
        (unsigned long long)handle->id, device.id, device.name);
    return 0;
}

int inputd_input_close(uint64_t id)
{
    inputd_handle_t *handle = find_handle(id);
    if (handle == NULL) return -9;
    if (handle->refs > 1) {
        handle->refs--;
        return 0;
    }
    if (handle->device_id < 64 && grabbed_handle_by_device[handle->device_id] == id)
        grabbed_handle_by_device[handle->device_id] = 0;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

int inputd_input_dup(uint64_t id, uint64_t *out_handle)
{
    inputd_handle_t *handle = find_handle(id);
    if (handle == NULL || out_handle == NULL || handle->refs == UINT32_MAX) return -9;
    handle->refs++;
    *out_handle = id;
    return 0;
}

static void copy_event_time(const inputd_handle_t *handle, const inputd_raw_event_t *source,
    inputd_input_event_t *target)
{
    target->type = source->type;
    target->code = source->code;
    target->value = source->value;
    if (handle->clock_id == INPUTD_CLOCK_MONOTONIC) {
        target->seconds = source->monotonic_seconds;
        target->microseconds = source->monotonic_microseconds;
    } else {
        struct timespec ts = {0};
        (void)clock_gettime(CLOCK_REALTIME, &ts);
        target->seconds = ts.tv_sec;
        target->microseconds = ts.tv_nsec / 1000;
    }
}

int inputd_input_read(inputd_read_request_t *request)
{
    if (request == NULL) return -22;
    inputd_handle_t *handle = find_handle(request->handle);
    if (handle == NULL) return -9;
    if (handle->revoked) return -13;
    uint32_t capacity = request->event_capacity;
    if (capacity > INPUTD_EVENT_CAPACITY) capacity = INPUTD_EVENT_CAPACITY;
    request->event_count = 0;
    if (capacity == 0) return -22;

    const uint64_t earliest = event_count == 0 ? next_sequence : event_ring[event_head].sequence;
    if (handle->cursor < earliest && request->event_count < capacity) {
        request->events[request->event_count++] = (inputd_input_event_t){
            .type = INPUTD_EV_SYN, .code = INPUTD_SYN_DROPPED, .value = 0,
        };
        handle->cursor = earliest;
    }
    for (size_t i = 0; i < event_count && request->event_count < capacity; i++) {
        const inputd_raw_event_t *event = &event_ring[(event_head + i) % INPUTD_RING_MAX];
        if (event->sequence < handle->cursor) continue;
        if (event->device_id == handle->device_id) {
            copy_event_time(handle, event, &request->events[request->event_count++]);
        }
    }
    handle->cursor = next_sequence;
    return request->event_count == 0 ? -11 : 0;
}

static uint32_t ioctl_size(uint64_t request) { return (uint32_t)((request >> 16) & 0x3fffu); }
static uint32_t ioctl_type(uint64_t request) { return (uint32_t)((request >> 8) & 0xffu); }
static uint32_t ioctl_nr(uint64_t request) { return (uint32_t)(request & 0xffu); }

static int ioctl_copy_out(inputd_ioctl_request_t *request, const void *data, uint32_t size)
{
    uint32_t capacity = request->data_size;
    const uint32_t encoded = ioctl_size(request->request);
    if (capacity > encoded) capacity = encoded;
    if (capacity > INPUTD_IOCTL_DATA_BYTES) capacity = INPUTD_IOCTL_DATA_BYTES;
    if (size > capacity) size = capacity;
    if (size != 0) memcpy(request->data, data, size);
    request->result_size = size;
    return 0;
}

static int ioctl_copy_string(inputd_ioctl_request_t *request, const char *string)
{
    size_t length = strlen(string) + 1u;
    if (length > UINT32_MAX) return -75;
    return ioctl_copy_out(request, string, (uint32_t)length);
}

int inputd_input_ioctl(inputd_ioctl_request_t *request)
{
    if (request == NULL || request->data_size > INPUTD_IOCTL_DATA_BYTES) return -22;
    inputd_handle_t *handle = find_handle(request->handle);
    if (handle == NULL) return -9;
    if (handle->revoked) return -13;
    kb_input_device_snapshot_t device;
    if (lookup_device_by_id(handle->device_id, &device) != 0) return -19;
    request->result_size = 0;
    const uint32_t command = (uint32_t)request->request;
    if (command == INPUTD_EVIOCGVERSION) {
        const int version = 0x010001;
        return ioctl_copy_out(request, &version, sizeof(version));
    }
    if (command == INPUTD_EVIOCGID)
        return ioctl_copy_out(request, &device.input_id, sizeof(device.input_id));
    if (command == INPUTD_EVIOCGREP) {
        const int repeat[2] = { 250, 33 };
        return ioctl_copy_out(request, repeat, sizeof(repeat));
    }
    if (command == INPUTD_EVIOCSCLOCKID) {
        if (request->data_size < sizeof(int)) return -22;
        int clock_id = -1;
        memcpy(&clock_id, request->data, sizeof(clock_id));
        if (clock_id != INPUTD_CLOCK_REALTIME && clock_id != INPUTD_CLOCK_MONOTONIC) return -22;
        handle->clock_id = clock_id;
        return 0;
    }
    if (command == INPUTD_EVIOCGRAB) {
        if (request->data_size < sizeof(int) || handle->device_id >= 64) return -22;
        int grab = 0;
        memcpy(&grab, request->data, sizeof(grab));
        uint64_t *owner = &grabbed_handle_by_device[handle->device_id];
        if (grab != 0 && *owner != 0 && *owner != handle->id) return -16;
        *owner = grab != 0 ? handle->id : 0;
        return 0;
    }
    if (command == INPUTD_EVIOCREVOKE) {
        handle->revoked = 1;
        if (handle->device_id < 64 && grabbed_handle_by_device[handle->device_id] == handle->id)
            grabbed_handle_by_device[handle->device_id] = 0;
        return 0;
    }
    if (ioctl_type(request->request) != 'E') return -25;
    const uint32_t nr = ioctl_nr(request->request);
    if (nr == 0x06u) return ioctl_copy_string(request, device.name);
    if (nr == 0x07u) return ioctl_copy_string(request, device.phys);
    if (nr == 0x08u) return ioctl_copy_string(request, device.uniq);
    if (nr == 0x09u) return ioctl_copy_out(request, &device.prop_bits, sizeof(device.prop_bits));
    if (nr == 0x18u || nr == 0x19u || nr == 0x1au || nr == 0x1bu) {
        uint8_t zero[INPUTD_IOCTL_DATA_BYTES] = {0};
        return ioctl_copy_out(request, zero, request->data_size);
    }
    if (nr >= 0x20u && nr <= 0x3fu) {
        switch (nr - 0x20u) {
        case 0: return ioctl_copy_out(request, &device.event_bits, sizeof(device.event_bits));
        case 1: return ioctl_copy_out(request, device.key_bits, sizeof(device.key_bits));
        case 2: return ioctl_copy_out(request, &device.rel_bits, sizeof(device.rel_bits));
        case 3: return ioctl_copy_out(request, &device.abs_bits, sizeof(device.abs_bits));
        case 4: return ioctl_copy_out(request, &device.msc_bits, sizeof(device.msc_bits));
        case 5: return ioctl_copy_out(request, &device.sw_bits, sizeof(device.sw_bits));
        case 0x11: return ioctl_copy_out(request, &device.led_bits, sizeof(device.led_bits));
        case 0x12: return ioctl_copy_out(request, &device.snd_bits, sizeof(device.snd_bits));
        case 0x15: return ioctl_copy_out(request, device.ff_bits, sizeof(device.ff_bits));
        default: {
            uint64_t zero = 0;
            return ioctl_copy_out(request, &zero, sizeof(zero));
        }
        }
    }
    if (nr >= 0x40u && nr < 0x40u + KB_INPUT_ABS_MAX) {
        const kb_input_abs_params_t *abs = &device.abs[nr - 0x40u];
        if (!abs->active) return -22;
        const int wire[6] = { abs->value, abs->minimum, abs->maximum,
            abs->fuzz, abs->flat, abs->resolution };
        return ioctl_copy_out(request, wire, sizeof(wire));
    }
    printf("[inputd] ioctl unsupported handle=%llu request=0x%08x size=%u\n",
        (unsigned long long)handle->id, command, request->data_size);
    return -25;
}

int inputd_input_poll(inputd_poll_request_t *request)
{
    if (request == NULL) return -22;
    inputd_handle_t *handle = find_handle(request->handle);
    if (handle == NULL) return -9;
    request->revents = 0;
    for (size_t i = 0; i < event_count; i++) {
        const inputd_raw_event_t *event = &event_ring[(event_head + i) % INPUTD_RING_MAX];
        if (event->sequence >= handle->cursor && event->device_id == handle->device_id) {
            request->revents = request->events & INPUTD_POLLIN;
            break;
        }
    }
    return 0;
}
