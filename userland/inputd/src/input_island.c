#include "input_island.h"

#include <kobox/device_pachaos_capsule.h>
#include <kobox/module.h>
#include <kobox/platform.h>
#include <kobox/shim.h>
#include <pacha/ipc.h>
#include "linux_subsystem/input/input.h"
#include "linux_subsystem/kvm/kvm_symbols.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    INPUTD_HANDLE_MAX = 32,
    INPUTD_TRANSFER_LEASE_MAX = 32,
    INPUTD_RING_MAX = 4096,
    INPUTD_FRAME_STAGE_MAX = 256,
    INPUTD_TIMING_RING_MAX = 64,
    INPUTD_STATE_FLUSH_MAX = 4,
    INPUTD_EV_SYN = 0,
    INPUTD_EV_KEY = 1,
    INPUTD_EV_SW = 5,
    INPUTD_EV_LED = 0x11,
    INPUTD_EV_SND = 0x12,
    INPUTD_SYN_REPORT = 0,
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
    uint64_t frame_sequence;
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
    int notify_fd;
    int notify_pending;
    int readable;
    int resyncing;
    int resync_drop_pending;
    int resync_report_pending;
    inputd_raw_event_t resync_drop;
    inputd_raw_event_t resync_report;
    uint64_t state_flush_through[INPUTD_STATE_FLUSH_MAX];
    int state_flush_active;
    int state_flush_frame_visible;
    uint32_t generation;
} inputd_handle_t;

typedef struct inputd_transfer_lease {
    int active;
    int notify_fd;
    uint64_t handle;
    uint32_t generation;
} inputd_transfer_lease_t;

static inputd_handle_t handles[INPUTD_HANDLE_MAX];
static inputd_transfer_lease_t transfer_leases[INPUTD_TRANSFER_LEASE_MAX];
static inputd_raw_event_t event_ring[INPUTD_RING_MAX];
static size_t event_head;
static size_t event_count;
static uint64_t next_handle = 1;
static uint64_t next_sequence = 1;
static uint64_t next_frame_sequence = 1;
static uint32_t next_source_generation = 1;
static uint64_t wait_generation = 1;
static uint32_t notify_ready_mask;

typedef struct inputd_frame_timing {
    uint64_t frame_sequence;
    uint64_t irq_ready_ns;
    uint64_t publish_ns;
} inputd_frame_timing_t;

typedef struct inputd_registry_entry {
    struct inputd_public_device public_device;
    uint32_t kobox_device_id;
    uint64_t grabbed_handle;
    uint32_t handle_mask;
    uint32_t stage_count;
    int stage_dropped;
    kb_input_event_t stage[INPUTD_FRAME_STAGE_MAX];
    inputd_frame_timing_t timing[INPUTD_TIMING_RING_MAX];
    uint32_t timing_head;
    uint64_t latest_sequence;
    uint64_t overwritten_sequence;
    uint64_t pending_irq_ready_ns;
} inputd_registry_entry_t;

static struct inputd_input_island *active_island;

static void bump_wait_generation(void)
{
    wait_generation++;
    if (wait_generation == 0) wait_generation = 1;
}

static uint32_t allocate_source_generation(void)
{
    const uint32_t generation = next_source_generation++;
    if (next_source_generation == 0) next_source_generation = 1;
    return generation == 0 ? allocate_source_generation() : generation;
}

static size_t handle_slot(const inputd_handle_t *handle)
{
    return (size_t)(handle - handles);
}

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
            handles[i].generation = allocate_source_generation();
            if (next_handle == 0) next_handle = 1;
            return &handles[i];
        }
    }
    return NULL;
}

typedef struct inputd_device_lookup {
    int find_by_id;
    uint32_t wanted_id;
    kb_input_device_snapshot_t *snapshots;
    size_t snapshot_capacity;
    size_t snapshot_count;
    int found;
} inputd_device_lookup_t;

static int lookup_device_by_id(uint32_t id, kb_input_device_snapshot_t *out);

static int lookup_device_callback(const kb_input_device_snapshot_t *device, void *opaque)
{
    inputd_device_lookup_t *lookup = opaque;
    if (lookup->find_by_id && device->id == lookup->wanted_id) {
        lookup->snapshots[0] = *device;
        lookup->found = 1;
        return 1;
    }
    if (!lookup->find_by_id && lookup->snapshot_count < lookup->snapshot_capacity)
        lookup->snapshots[lookup->snapshot_count++] = *device;
    return 0;
}

static inputd_registry_entry_t *lookup_registry_by_event(uint32_t event_index)
{
    if (active_island == NULL || active_island->registry == NULL) return NULL;
    inputd_registry_entry_t *registry = active_island->registry;
    for (uint32_t i = 0; i < active_island->device_count; i++)
        if (registry[i].public_device.event_index == event_index) return &registry[i];
    return NULL;
}

static inputd_registry_entry_t *lookup_registry_by_device_id(uint32_t device_id)
{
    if (active_island == NULL || active_island->registry == NULL) return NULL;
    inputd_registry_entry_t *registry = active_island->registry;
    for (uint32_t i = 0; i < active_island->device_count; i++)
        if (registry[i].kobox_device_id == device_id) return &registry[i];
    return NULL;
}

static int lookup_device_by_id(uint32_t id, kb_input_device_snapshot_t *out)
{
    inputd_device_lookup_t lookup = {
        .find_by_id = 1,
        .wanted_id = id,
        .snapshots = out,
        .snapshot_capacity = 1,
    };
    (void)kb_input_subsystem_for_each_device(lookup_device_callback, &lookup);
    if (!lookup.found) return -19;
    return 0;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void flush_notify_edges(void)
{
    uint32_t ready = notify_ready_mask;
    while (ready != 0) {
        const unsigned slot = (unsigned)__builtin_ctz(ready);
        const uint32_t bit = UINT32_C(1) << slot;
        ready &= ~bit;
        inputd_handle_t *handle = &handles[slot];
        if (!handle->active || handle->notify_fd < 16 || !handle->readable) {
            notify_ready_mask &= ~bit;
            continue;
        }
        const struct pacha_ipc_msg message = {
            .word0 = UINT64_C(0x494e505554455654),
            .word1 = handle->id,
        };
        if (pacha_ipc_send(handle->notify_fd, &message) == 0) {
            handle->notify_pending = 1;
            notify_ready_mask &= ~bit;
        }
    }
}

static void mark_device_readable(inputd_registry_entry_t *entry)
{
    uint32_t subscribers = entry->handle_mask;
    while (subscribers != 0) {
        const unsigned slot = (unsigned)__builtin_ctz(subscribers);
        const uint32_t bit = UINT32_C(1) << slot;
        subscribers &= ~bit;
        inputd_handle_t *handle = &handles[slot];
        if (!handle->active || handle->readable || handle->revoked) continue;
        handle->readable = 1;
        notify_ready_mask |= bit;
    }
    flush_notify_edges();
}

static void append_event(
    const kb_input_event_t *event,
    uint64_t frame_sequence,
    uint64_t irq_ready_ns)
{
    size_t slot = (event_head + event_count) % INPUTD_RING_MAX;
    if (event_count == INPUTD_RING_MAX) {
        slot = event_head;
        inputd_registry_entry_t *overwritten_entry =
            lookup_registry_by_device_id(event_ring[slot].device_id);
        if (overwritten_entry != NULL)
            overwritten_entry->overwritten_sequence = event_ring[slot].sequence;
        event_head = (event_head + 1u) % INPUTD_RING_MAX;
    } else {
        event_count++;
    }
    event_ring[slot] = (inputd_raw_event_t){
        .sequence = next_sequence++,
        .frame_sequence = frame_sequence,
        .device_id = event->device_id,
        .type = (uint16_t)event->type,
        .code = (uint16_t)event->code,
        .value = event->value,
        .monotonic_seconds = (int64_t)(irq_ready_ns / UINT64_C(1000000000)),
        .monotonic_microseconds = (int64_t)((irq_ready_ns % UINT64_C(1000000000)) / 1000u),
    };
}

static void commit_staged_frame(inputd_registry_entry_t *entry)
{
    const uint64_t frame_sequence = next_frame_sequence++;
    if (next_frame_sequence == 0) next_frame_sequence = 1;
    uint64_t irq_ready_ns = entry->pending_irq_ready_ns;
    if (irq_ready_ns == 0) irq_ready_ns = monotonic_ns();
    const uint64_t publish_ns = monotonic_ns();
    if (entry->stage_dropped) {
        const kb_input_event_t dropped = {
            .device_id = entry->kobox_device_id,
            .type = INPUTD_EV_SYN,
            .code = INPUTD_SYN_DROPPED,
        };
        const kb_input_event_t report = {
            .device_id = entry->kobox_device_id,
            .type = INPUTD_EV_SYN,
            .code = INPUTD_SYN_REPORT,
        };
        append_event(&dropped, frame_sequence, irq_ready_ns);
        append_event(&report, frame_sequence, irq_ready_ns);
    } else {
        for (uint32_t i = 0; i < entry->stage_count; i++)
            append_event(&entry->stage[i], frame_sequence, irq_ready_ns);
    }
    entry->latest_sequence = next_sequence - 1u;
    entry->timing[entry->timing_head] = (inputd_frame_timing_t){
        .frame_sequence = frame_sequence,
        .irq_ready_ns = irq_ready_ns,
        .publish_ns = publish_ns,
    };
    entry->timing_head = (entry->timing_head + 1u) % INPUTD_TIMING_RING_MAX;
    entry->stage_count = 0;
    entry->stage_dropped = 0;
    entry->pending_irq_ready_ns = 0;
    mark_device_readable(entry);
}

static void stage_event(const kb_input_event_t *event, uint64_t irq_ready_ns)
{
    inputd_registry_entry_t *entry = lookup_registry_by_device_id(event->device_id);
    if (entry == NULL) return;
    if (entry->pending_irq_ready_ns == 0) entry->pending_irq_ready_ns = irq_ready_ns;
    if (!entry->stage_dropped) {
        if (entry->stage_count < INPUTD_FRAME_STAGE_MAX) {
            entry->stage[entry->stage_count++] = *event;
        } else {
            entry->stage_count = 0;
            entry->stage_dropped = 1;
        }
    }
    if (event->type == INPUTD_EV_SYN && event->code == INPUTD_SYN_REPORT)
        commit_staged_frame(entry);
}

int inputd_input_island_drain_device(
    struct inputd_input_island *island,
    size_t device_ordinal,
    uint64_t irq_ready_ns)
{
    if (island == NULL || island->device_backend == NULL ||
        device_ordinal >= island->device_count) return -22;
    const int dispatch_status = kb_handle_device_irqs(
        island->device_backend, device_ordinal, 0);
    if (dispatch_status != 0) return dispatch_status;
    for (unsigned pass = 0; pass < 8; pass++) {
        kb_input_event_t events[128];
        const size_t count = kb_input_subsystem_pop_events(events, 128);
        for (size_t i = 0; i < count; i++) stage_event(&events[i], irq_ready_ns);
        if (count < 128) break;
    }
    return 0;
}

static uint32_t input_capabilities(const kb_input_device_snapshot_t *device)
{
    enum {
        ev_key = 1,
        ev_rel = 2,
        ev_abs = 3,
        key_a = 30,
        rel_x = 0,
        rel_y = 1,
        abs_x = 0,
        abs_y = 1,
    };
    uint32_t capabilities = 0;
    if ((device->event_bits & (UINT64_C(1) << ev_key)) != 0 &&
        (device->key_bits[key_a / 64] & (UINT64_C(1) << (key_a % 64))) != 0)
        capabilities |= INPUTD_INPUT_CAP_KEYBOARD;
    if ((device->event_bits & (UINT64_C(1) << ev_rel)) != 0 &&
        (device->rel_bits & (UINT64_C(1) << rel_x)) != 0 &&
        (device->rel_bits & (UINT64_C(1) << rel_y)) != 0)
        capabilities |= INPUTD_INPUT_CAP_RELATIVE;
    if ((device->event_bits & (UINT64_C(1) << ev_abs)) != 0 &&
        (device->abs_bits & (UINT64_C(1) << abs_x)) != 0 &&
        (device->abs_bits & (UINT64_C(1) << abs_y)) != 0)
        capabilities |= INPUTD_INPUT_CAP_ABSOLUTE;
    return capabilities;
}

static int compare_registry_entry(const void *left, const void *right)
{
    const struct inputd_public_device *a =
        &((const inputd_registry_entry_t *)left)->public_device;
    const struct inputd_public_device *b =
        &((const inputd_registry_entry_t *)right)->public_device;
#define INPUTD_COMPARE_FIELD(field) \
    do { if (a->field != b->field) return a->field < b->field ? -1 : 1; } while (0)
    INPUTD_COMPARE_FIELD(pci_segment);
    INPUTD_COMPARE_FIELD(pci_bus);
    INPUTD_COMPARE_FIELD(pci_device);
    INPUTD_COMPARE_FIELD(pci_function);
    INPUTD_COMPARE_FIELD(stable_id);
#undef INPUTD_COMPARE_FIELD
    return 0;
}

static int build_input_registry(
    struct inputd_input_island *island,
    const struct inputd_boot_config *cfg)
{
    kb_input_device_snapshot_t *snapshots =
        calloc(cfg->device_count, sizeof(*snapshots));
    inputd_registry_entry_t *registry =
        calloc(cfg->device_count, sizeof(*registry));
    if (snapshots == NULL || registry == NULL) {
        free(snapshots);
        free(registry);
        return -12;
    }
    inputd_device_lookup_t collect = {
        .snapshots = snapshots,
        .snapshot_capacity = cfg->device_count,
    };
    (void)kb_input_subsystem_for_each_device(lookup_device_callback, &collect);
    if (collect.snapshot_count != cfg->device_count) {
        free(snapshots);
        free(registry);
        return -19;
    }

    const struct inputd_device_config *devices = inputd_boot_devices(cfg);
    for (uint32_t i = 0; i < cfg->device_count; i++) {
        registry[i].public_device = (struct inputd_public_device){
            .stable_id = devices[i].resource_id,
            .generation = 1,
            .capabilities = input_capabilities(&snapshots[i]),
            .pci_segment = devices[i].pci_segment,
            .pci_bus = devices[i].pci_bus,
            .pci_device = devices[i].pci_device,
            .pci_function = devices[i].pci_function,
        };
        registry[i].kobox_device_id = snapshots[i].id;
    }
    qsort(registry, cfg->device_count, sizeof(*registry), compare_registry_entry);
    for (uint32_t i = 0; i < cfg->device_count; i++) {
        if (i != 0 &&
            registry[i - 1].public_device.pci_segment == registry[i].public_device.pci_segment &&
            registry[i - 1].public_device.pci_bus == registry[i].public_device.pci_bus &&
            registry[i - 1].public_device.pci_device == registry[i].public_device.pci_device &&
            registry[i - 1].public_device.pci_function == registry[i].public_device.pci_function) {
            free(snapshots);
            free(registry);
            return -22;
        }
        registry[i].public_device.event_index = i;
        printf("[inputd] registry event%u stable=%llu generation=%u caps=0x%x pci=%04x:%02x:%02x.%u kobox=%u\n",
            i,
            (unsigned long long)registry[i].public_device.stable_id,
            registry[i].public_device.generation,
            registry[i].public_device.capabilities,
            registry[i].public_device.pci_segment,
            registry[i].public_device.pci_bus,
            registry[i].public_device.pci_device,
            registry[i].public_device.pci_function,
            registry[i].kobox_device_id);
    }
    free(snapshots);
    island->registry = registry;
    return 0;
}

int inputd_input_island_init(
    struct inputd_input_island *island,
    const struct inputd_boot_config *cfg)
{
    if (island == NULL || cfg == NULL || cfg->device_count == 0 ||
        cfg->module_count == 0) return -22;
    memset(island, 0, sizeof(*island));
    (void)setenv("KOBOX_DEVICE_BACKEND", "pachaos", 1);
    (void)setenv("KOBOX_PCI_LAYOUT", "arch68", 1);
    (void)setenv("KOBOX_VIRTIO_NO_INDIRECT", "1", 1);
    (void)setenv("KOBOX_VIRTIO_NO_EVENT_IDX", "1", 1);
    (void)setenv("KOBOX_INPUT_TRUST_DEVICE_STRINGS", "1", 1);

    const struct inputd_device_config *devices = inputd_boot_devices(cfg);
    uint64_t *device_fds = calloc(cfg->device_count, sizeof(*device_fds));
    if (device_fds == NULL) return -12;
    for (uint32_t i = 0; i < cfg->device_count; i++) device_fds[i] = devices[i].device_fd;
    kb_device_backend_t *backend = NULL;
    kb_status_t status = kb_pachaos_capsule_devices_create(
        device_fds, cfg->device_count, &backend);
    free(device_fds);
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

    island->modules = calloc(cfg->module_count, sizeof(*island->modules));
    if (island->modules == NULL) return -12;
    const struct inputd_module_config *modules = inputd_boot_modules(cfg);
    for (uint32_t i = 0; i < cfg->module_count; i++) {
        const struct inputd_module_config *module = &modules[i];
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
        if (kb_input_subsystem_device_count() == cfg->device_count) break;
    }
    island->device_count = (uint32_t)kb_input_subsystem_device_count();
    if (island->device_count != cfg->device_count) return -19;
    const int opened = kb_input_subsystem_open_registered_devices();
    if (opened < 0 || (uint32_t)opened != cfg->device_count) return -5;
    const int registry_status = build_input_registry(island, cfg);
    if (registry_status != 0) return registry_status;
    kb_input_subsystem_print_summary(stdout);
    active_island = island;
    island->ready = 1;
    return 0;
}

size_t inputd_input_public_device_count(const struct inputd_input_island *island)
{
    return island == NULL || island->registry == NULL ? 0 : island->device_count;
}

int inputd_input_public_device(
    const struct inputd_input_island *island,
    size_t ordinal,
    struct inputd_public_device *out_device)
{
    if (island == NULL || island->registry == NULL || out_device == NULL ||
        ordinal >= island->device_count) return -22;
    const inputd_registry_entry_t *registry = island->registry;
    *out_device = registry[ordinal].public_device;
    return 0;
}

int inputd_input_open(uint32_t event_index, uint32_t flags, int notify_fd, uint64_t *out_handle)
{
    if (out_handle == NULL || notify_fd < 16) return -22;
    inputd_registry_entry_t *entry = lookup_registry_by_event(event_index);
    if (entry == NULL) return -19;
    kb_input_device_snapshot_t device;
    int status = lookup_device_by_id(entry->kobox_device_id, &device);
    if (status != 0) return status;
    inputd_handle_t *handle = alloc_handle();
    if (handle == NULL) return -24;
    handle->device_id = device.id;
    handle->flags = flags;
    handle->cursor = next_sequence;
    handle->notify_fd = notify_fd;
    entry->handle_mask |= UINT32_C(1) << handle_slot(handle);
    bump_wait_generation();
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
    if (handle->notify_fd >= 16) (void)pacha_fd_close(handle->notify_fd);
    inputd_registry_entry_t *entry = lookup_registry_by_device_id(handle->device_id);
    const uint32_t bit = UINT32_C(1) << handle_slot(handle);
    if (entry != NULL) {
        entry->handle_mask &= ~bit;
        if (entry->grabbed_handle == id) entry->grabbed_handle = 0;
    }
    notify_ready_mask &= ~bit;
    memset(handle, 0, sizeof(*handle));
    bump_wait_generation();
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

int inputd_input_transfer_dup(uint64_t id, int notify_fd, uint64_t *out_handle)
{
    inputd_handle_t *handle = find_handle(id);
    if (handle == NULL || out_handle == NULL || notify_fd < 16 ||
        handle->refs == UINT32_MAX) return -9;
    inputd_transfer_lease_t *lease = NULL;
    for (size_t i = 0; i < INPUTD_TRANSFER_LEASE_MAX; ++i) {
        if (!transfer_leases[i].active) {
            lease = &transfer_leases[i];
            break;
        }
    }
    if (lease == NULL) return -24;
    memset(lease, 0, sizeof(*lease));
    lease->active = 1;
    lease->notify_fd = notify_fd;
    lease->handle = id;
    lease->generation = allocate_source_generation();
    handle->refs++;
    bump_wait_generation();
    *out_handle = id;
    return 0;
}

static uint32_t inputd_transfer_lease_count(uint64_t handle)
{
    uint32_t count = 0;
    for (size_t i = 0; i < INPUTD_TRANSFER_LEASE_MAX; ++i)
        if (transfer_leases[i].active && transfer_leases[i].handle == handle) count++;
    return count;
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

static void copy_syn_event(const inputd_handle_t *handle,
    const inputd_raw_event_t *source, uint16_t code, inputd_input_event_t *target)
{
    const inputd_raw_event_t synthetic = {
        .type = INPUTD_EV_SYN,
        .code = code,
        .monotonic_seconds = source == NULL ? 0 : source->monotonic_seconds,
        .monotonic_microseconds = source == NULL ? 0 : source->monotonic_microseconds,
    };
    copy_event_time(handle, &synthetic, target);
}

static int state_flush_slot(uint16_t type)
{
    switch (type) {
    case INPUTD_EV_KEY: return 0;
    case INPUTD_EV_LED: return 1;
    case INPUTD_EV_SND: return 2;
    case INPUTD_EV_SW: return 3;
    default: return -1;
    }
}

static uint64_t state_flush_last_sequence(const inputd_handle_t *handle)
{
    uint64_t last = 0;
    for (size_t i = 0; i < INPUTD_STATE_FLUSH_MAX; i++)
        if (handle->state_flush_through[i] > last)
            last = handle->state_flush_through[i];
    return last;
}

static void finish_state_flush_if_consumed(inputd_handle_t *handle)
{
    if (!handle->state_flush_active ||
        handle->cursor <= state_flush_last_sequence(handle)) return;
    memset(handle->state_flush_through, 0, sizeof(handle->state_flush_through));
    handle->state_flush_active = 0;
    handle->state_flush_frame_visible = 0;
}

static void mark_state_events_flushed(
    inputd_handle_t *handle,
    const inputd_registry_entry_t *entry,
    uint16_t type)
{
    const int slot = state_flush_slot(type);
    if (handle == NULL || entry == NULL || slot < 0 ||
        entry->latest_sequence < handle->cursor) return;
    if (entry->latest_sequence > handle->state_flush_through[slot])
        handle->state_flush_through[slot] = entry->latest_sequence;
    handle->state_flush_active = 1;
    /* Linux preserves a leading report while compacting a client queue. */
    handle->state_flush_frame_visible = 1;
}

static int event_was_state_flushed(
    const inputd_handle_t *handle,
    const inputd_raw_event_t *event)
{
    const int slot = state_flush_slot(event->type);
    return handle->state_flush_active && slot >= 0 &&
        event->sequence <= handle->state_flush_through[slot];
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

    inputd_registry_entry_t *entry = lookup_registry_by_device_id(handle->device_id);
    const uint64_t earliest = event_count == 0 ? next_sequence : event_ring[event_head].sequence;
    if (handle->resync_report_pending && request->event_count < capacity) {
        copy_event_time(handle, &handle->resync_report,
            &request->events[request->event_count++]);
        handle->resync_report_pending = 0;
    }

    if (!handle->resyncing && !handle->resync_drop_pending &&
        !handle->resync_report_pending && entry != NULL &&
        handle->cursor <= entry->overwritten_sequence)
    {
        const inputd_raw_event_t *source = event_count == 0 ? NULL : &event_ring[event_head];
        for (size_t i = 0; i < event_count; i++) {
            const inputd_raw_event_t *candidate =
                &event_ring[(event_head + i) % INPUTD_RING_MAX];
            if (candidate->device_id == handle->device_id) {
                source = candidate;
                break;
            }
        }
        handle->resync_drop = source == NULL ? (inputd_raw_event_t){0} : *source;
        handle->resync_drop_pending = 1;
    }
    if (handle->resync_drop_pending && request->event_count < capacity) {
        copy_syn_event(handle, &handle->resync_drop, INPUTD_SYN_DROPPED,
            &request->events[request->event_count++]);
        handle->resync_drop_pending = 0;
        handle->resyncing = 1;
    }

    size_t start = 0;
    if (handle->cursor > earliest) {
        const uint64_t consumed = handle->cursor - earliest;
        start = consumed < event_count ? (size_t)consumed : event_count;
    }
    for (size_t i = start; !handle->resync_drop_pending && i < event_count; i++) {
        const inputd_raw_event_t *event = &event_ring[(event_head + i) % INPUTD_RING_MAX];
        if (event->device_id != handle->device_id) {
            handle->cursor = event->sequence + 1u;
            finish_state_flush_if_consumed(handle);
            continue;
        }
        if (handle->resyncing) {
            handle->cursor = event->sequence + 1u;
            finish_state_flush_if_consumed(handle);
            if (event->type == INPUTD_EV_SYN && event->code == INPUTD_SYN_REPORT) {
                handle->resyncing = 0;
                if (request->event_count < capacity) {
                    copy_event_time(handle, event,
                        &request->events[request->event_count++]);
                } else {
                    handle->resync_report = *event;
                    handle->resync_report_pending = 1;
                    break;
                }
            }
            continue;
        }
        const uint64_t flush_last = state_flush_last_sequence(handle);
        const int in_flush_window = handle->state_flush_active &&
            event->sequence <= flush_last;
        if (event_was_state_flushed(handle, event)) {
            handle->cursor = event->sequence + 1u;
            finish_state_flush_if_consumed(handle);
            continue;
        }
        if (in_flush_window && event->type == INPUTD_EV_SYN &&
            event->code == INPUTD_SYN_REPORT &&
            !handle->state_flush_frame_visible)
        {
            handle->cursor = event->sequence + 1u;
            finish_state_flush_if_consumed(handle);
            continue;
        }
        if (request->event_count >= capacity) break;
        copy_event_time(handle, event, &request->events[request->event_count++]);
        handle->cursor = event->sequence + 1u;
        if (in_flush_window) {
            if (event->type == INPUTD_EV_SYN && event->code == INPUTD_SYN_REPORT)
                handle->state_flush_frame_visible = 0;
            else
                handle->state_flush_frame_visible = 1;
        }
        finish_state_flush_if_consumed(handle);
    }
    handle->readable = handle->resync_drop_pending || handle->resync_report_pending ||
        (entry != NULL && entry->latest_sequence >= handle->cursor);
    if (request->event_count == 0) {
        handle->readable = 0;
        return -11;
    }
    handle->notify_pending = 0;
    if (handle->readable)
        notify_ready_mask |= UINT32_C(1) << handle_slot(handle);
    return 0;
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

static int ioctl_copy_state_and_flush(
    inputd_ioctl_request_t *request,
    inputd_handle_t *handle,
    uint16_t type,
    const void *state,
    uint32_t size)
{
    inputd_registry_entry_t *entry =
        lookup_registry_by_device_id(handle->device_id);
    mark_state_events_flushed(handle, entry, type);
    return ioctl_copy_out(request, state, size);
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
        if (request->data_size < sizeof(int)) return -22;
        int grab = 0;
        memcpy(&grab, request->data, sizeof(grab));
        inputd_registry_entry_t *entry = lookup_registry_by_device_id(handle->device_id);
        if (entry == NULL) return -19;
        uint64_t *owner = &entry->grabbed_handle;
        if (grab != 0 && *owner != 0 && *owner != handle->id) return -16;
        *owner = grab != 0 ? handle->id : 0;
        return 0;
    }
    if (command == INPUTD_EVIOCREVOKE) {
        handle->revoked = 1;
        inputd_registry_entry_t *entry = lookup_registry_by_device_id(handle->device_id);
        if (entry != NULL && entry->grabbed_handle == handle->id)
            entry->grabbed_handle = 0;
        return 0;
    }
    if (ioctl_type(request->request) != 'E') return -25;
    const uint32_t nr = ioctl_nr(request->request);
    if (nr == 0x06u) return ioctl_copy_string(request, device.name);
    if (nr == 0x07u) return ioctl_copy_string(request, device.phys);
    if (nr == 0x08u) return ioctl_copy_string(request, device.uniq);
    if (nr == 0x09u) return ioctl_copy_out(request, &device.prop_bits, sizeof(device.prop_bits));
    if (nr == 0x18u)
        return ioctl_copy_state_and_flush(request, handle, INPUTD_EV_KEY,
            device.key_state, sizeof(device.key_state));
    if (nr == 0x19u)
        return ioctl_copy_state_and_flush(request, handle, INPUTD_EV_LED,
            &device.led_state, sizeof(device.led_state));
    if (nr == 0x1au)
        return ioctl_copy_state_and_flush(request, handle, INPUTD_EV_SND,
            &device.snd_state, sizeof(device.snd_state));
    if (nr == 0x1bu)
        return ioctl_copy_state_and_flush(request, handle, INPUTD_EV_SW,
            &device.sw_state, sizeof(device.sw_state));
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
    request->revents = handle->readable ? request->events & INPUTD_POLLIN : 0;
    return 0;
}

uint64_t inputd_input_wait_generation(void)
{
    return wait_generation;
}

size_t inputd_input_collect_wait_sources(
    struct inputd_wait_source *sources,
    size_t capacity)
{
    if (sources == NULL || capacity == 0) return 0;
    size_t count = 0;
    for (size_t i = 0; i < INPUTD_HANDLE_MAX && count < capacity; i++) {
        const inputd_handle_t *handle = &handles[i];
        if (!handle->active || handle->notify_fd < 16) continue;
        sources[count++] = (struct inputd_wait_source){
            .fd = handle->notify_fd,
            .kind = INPUTD_WAIT_SOURCE_HANDLE,
            .slot = (uint32_t)i,
            .generation = handle->generation,
        };
    }
    for (size_t i = 0; i < INPUTD_TRANSFER_LEASE_MAX && count < capacity; ++i) {
        const inputd_transfer_lease_t *lease = &transfer_leases[i];
        if (!lease->active || lease->notify_fd < 16) continue;
        sources[count++] = (struct inputd_wait_source){
            .fd = lease->notify_fd,
            .kind = INPUTD_WAIT_SOURCE_TRANSFER_LEASE,
            .slot = (uint32_t)i,
            .generation = lease->generation,
        };
    }
    return count;
}

void inputd_input_handle_wait_event(
    const struct inputd_wait_source *source,
    uint64_t revents)
{
    if (source == NULL || (revents & PACHA_FD_EVENT_HANGUP) == 0) return;
    if (source->kind == INPUTD_WAIT_SOURCE_HANDLE && source->slot < INPUTD_HANDLE_MAX) {
        inputd_handle_t *handle = &handles[source->slot];
        if (!handle->active || handle->generation != source->generation ||
            handle->notify_fd != source->fd) return;
        const uint64_t orphan_id = handle->id;
        const uint32_t orphan_refs = handle->refs;
        const int notify_fd = handle->notify_fd;
        handle->notify_fd = -1;
        if (notify_fd >= 16) (void)pacha_fd_close(notify_fd);
        handle->refs = inputd_transfer_lease_count(handle->id) + 1u;
        const int status = inputd_input_close(orphan_id);
        printf("[inputd] orphan reap handle=%llu refs=%u status=%d\n",
            (unsigned long long)orphan_id, orphan_refs, status);
        bump_wait_generation();
        return;
    }
    if (source->kind == INPUTD_WAIT_SOURCE_TRANSFER_LEASE &&
        source->slot < INPUTD_TRANSFER_LEASE_MAX) {
        inputd_transfer_lease_t *lease = &transfer_leases[source->slot];
        if (!lease->active || lease->generation != source->generation ||
            lease->notify_fd != source->fd) return;
        const uint64_t handle = lease->handle;
        (void)pacha_fd_close(lease->notify_fd);
        memset(lease, 0, sizeof(*lease));
        bump_wait_generation();
        (void)inputd_input_close(handle);
    }
}

void inputd_input_flush_notifications(void)
{
    flush_notify_edges();
}
