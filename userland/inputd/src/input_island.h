#pragma once

#include <stddef.h>
#include <stdint.h>

#include <inputd/boot_config.h>
#include <inputd/ipc_protocol.h>
#include <inputd/source_protocol.h>

enum {
    INPUTD_INPUT_CAP_KEYBOARD = 1u << 0,
    INPUTD_INPUT_CAP_RELATIVE = 1u << 1,
    INPUTD_INPUT_CAP_ABSOLUTE = 1u << 2,
};

struct inputd_public_device {
    uint64_t stable_id;
    uint32_t event_index;
    uint32_t generation;
    uint32_t capabilities;
    uint32_t pci_segment;
    uint32_t pci_bus;
    uint32_t pci_device;
    uint32_t pci_function;
};

struct inputd_input_island {
    void *device_backend;
    void **modules;
    void *registry;
    uint32_t loaded_module_count;
    uint32_t device_count;
    int ready;
};

enum inputd_wait_source_kind {
    INPUTD_WAIT_SOURCE_HANDLE = 1,
    INPUTD_WAIT_SOURCE_TRANSFER_LEASE = 2,
};

struct inputd_wait_source {
    int fd;
    uint32_t kind;
    uint32_t slot;
    uint32_t generation;
};

int inputd_input_island_init(
    struct inputd_input_island *island,
    const struct inputd_boot_config *cfg);
size_t inputd_input_public_device_count(const struct inputd_input_island *island);
int inputd_input_public_device(
    const struct inputd_input_island *island,
    size_t ordinal,
    struct inputd_public_device *out_device);
int inputd_input_island_drain_device(
    struct inputd_input_island *island,
    size_t device_ordinal,
    uint64_t irq_ready_ns);
int inputd_input_source_begin(struct inputd_input_island *island,
    uint64_t source_id, uint64_t generation, int lost_events);
int inputd_input_source_device(struct inputd_input_island *island,
    uint64_t source_id, uint64_t generation,
    uint16_t pci_segment, uint8_t pci_bus, uint8_t pci_device, uint8_t pci_function,
    const struct kobox_linux_input_device_info *info,
    struct inputd_public_device *published);
int inputd_input_source_mark_published(struct inputd_input_island *island,
    uint64_t source_id, uint64_t generation, uint32_t local_id);
int inputd_input_source_end(struct inputd_input_island *island,
    uint64_t source_id, uint64_t generation,
    int (*removed)(const struct inputd_public_device *, void *), void *context);
int inputd_input_source_events(struct inputd_input_island *island,
    uint64_t source_id, uint64_t generation,
    const struct kobox_linux_input_record *records, size_t count);
int inputd_input_open(uint32_t event_index, uint32_t flags, int notify_fd, uint64_t *out_handle);
int inputd_input_close(uint64_t handle);
int inputd_input_dup(uint64_t handle, uint64_t *out_handle);
int inputd_input_transfer_dup(uint64_t handle, int notify_fd, uint64_t *out_handle);
int inputd_input_read(inputd_read_request_t *request);
int inputd_input_ioctl(inputd_ioctl_request_t *request);
int inputd_input_poll(inputd_poll_request_t *request);
uint64_t inputd_input_wait_generation(void);
size_t inputd_input_collect_wait_sources(
    struct inputd_wait_source *sources,
    size_t capacity);
void inputd_input_handle_wait_event(
    const struct inputd_wait_source *source,
    uint64_t revents);
void inputd_input_flush_notifications(void);
