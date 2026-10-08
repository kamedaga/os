/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../userland/kobox2_adapter/usb_input_service.c"

static unsigned char buffer[PH_USB_INPUT_SHARED_BYTES];
static uint64_t doorbell;
static unsigned closes, unmapped;
static int write_error;
static struct kobox_linux_input_notify observer;
static struct ph_ipc_packet sent;

/* The fake port starts with arrivals predating ATTACH, just as USB probing
 * does. Native IPC ownership is tested without booting a kernel fixture. */
static int bind_port(void *port, const struct kobox_linux_input_notify *notify) {
    assert(port == (void *)1 && !observer.notify);
    observer = *notify;
    return observer.notify(observer.context);
}
static int read_port(void *port, struct kobox_linux_input_record *records,
    size_t capacity, size_t *count, uint64_t *overwritten) {
    assert(port == (void *)1 && capacity == PH_USB_INPUT_READ_MAX);
    records[0] = (struct kobox_linux_input_record){.sequence = 1,
        .monotonic_ns = 1000000, .kind = KOBOX_INPUT_EVENT,
        .device_id = 2, .type = 2, .value = -7};
    *count = 1; *overwritten = 0; return 0;
}
static int snapshot_port(void *port, struct kobox_linux_input_device_info *devices,
    size_t capacity, size_t *count, uint64_t *cursor) {
    assert(port == (void *)1 && capacity);
    devices[0] = (struct kobox_linux_input_device_info){.id = 2};
    *count = 1; *cursor = 2; return 0;
}
void *ph_image_lookup(void *core, const char *name) {
    assert(core == (void *)1);
    if (!strcmp(name, "kobox_linux_input_port_read")) return (void *)read_port;
    if (!strcmp(name, "kobox_linux_input_port_snapshot")) return (void *)snapshot_port;
    if (!strcmp(name, "kobox_linux_input_port_bind_notify")) return (void *)bind_port;
    assert(0); return NULL;
}
long pacha_syscall1(uint64_t nr, uint64_t fd) {
    assert(nr == PACHA_FD_SYSCALL_CLOSE && fd == 30); closes++; return 0;
}
long pacha_syscall2(uint64_t nr, uint64_t a, uint64_t b) {
    if (nr == PACHA_FD_SYSCALL_GET_INFO) {
        assert(a == 20);
        *(struct pacha_fd_info *)(uintptr_t)b = (struct pacha_fd_info){
            .kind = PACHA_FD_KIND_VMO, .size = sizeof(buffer),
            .rights = PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE};
    } else {
        assert(nr == PACHA_VM_SYSCALL_MUNMAP && a == (uintptr_t)buffer && b == sizeof(buffer));
        unmapped++;
    }
    return 0;
}
long pacha_syscall3(uint64_t nr, uint64_t a, uint64_t b, uint64_t c) {
    if (nr == PACHA_FD_SYSCALL_EVENTFD_CREATE) {
        assert(!a && (b & PACHA_FD_RIGHT_TRANSFER) && c == PACHA_FD_FLAG_NONBLOCK);
        return 30;
    }
    assert(nr == PACHA_FD_SYSCALL_WRITE && a == 30 && c == 8);
    assert(*(uint64_t *)(uintptr_t)b == 1);
    if (write_error) return -1;
    doorbell++; return 8;
}
long pacha_syscall6(uint64_t nr, uint64_t a, uint64_t b, uint64_t c,
    uint64_t d, uint64_t e, uint64_t f) {
    assert(nr == PACHA_VM_SYSCALL_MMAP && a == 20 && !b && c == sizeof(buffer));
    assert(d == (PACHA_PROT_READ | PACHA_PROT_WRITE) && e == PACHA_MMAP_SHARED && !f);
    return (long)(uintptr_t)buffer;
}
int ph_ipc_send(struct ph_ipc *ipc, struct ph_ipc_packet *packet) {
    assert(ipc == (void *)1); sent = *packet; return 0;
}

int main(void) {
    struct ph_usb_input_service input = {0};
    struct ph_lifecycle_service ops = {0};
    assert(!ph_usb_input_service_init(&input, (void *)1, 7, &ops));
    struct ph_ipc_packet packet = {.operation = PH_USB_INPUT_ATTACH,
        .generation = 7, .correlation = 1, .value = PH_USB_INPUT_SHARED_BYTES,
        .fd_count = 1, .fds = {{.fd = 20}}};
    packet.generation = 6;
    assert(ops.prepare(ops.context, &packet) == -EPROTO);
    packet.generation = 7; packet.fd_count = 0;
    assert(ops.prepare(ops.context, &packet) == -EPROTO);
    packet.fd_count = 1;
    assert(!ops.prepare(ops.context, &packet));
    assert(!ops.dispatch(ops.context, (void *)1) && doorbell == 1);
    assert(!ops.complete(ops.context, (void *)1));
    const uint64_t reader = PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE;
    assert(sent.fd_count == 1 && sent.fds[0].fd == 30 && sent.fds[0].rights == reader);
    assert(sent.fds[0].flags & PACHA_FD_FLAG_NONBLOCK);
    assert(!ops.release(ops.context));
    input.shared->magic = PH_USB_INPUT_SHARED_MAGIC; input.shared->generation = 7;
    packet = (struct ph_ipc_packet){.operation = PH_USB_INPUT_READ,
        .generation = 7, .correlation = 2};
    assert(!ops.prepare(ops.context, &packet));
    assert(!ops.dispatch(ops.context, (void *)1));
    assert(input.shared->count == 1);
    struct kobox_linux_input_record *record = (void *)input.shared->payload;
    assert(record->monotonic_ns == 1000000 && record->value == -7);
    assert(!ops.complete(ops.context, (void *)1) && !sent.fd_count);
    assert(!ops.release(ops.context));
    write_error = 1;
    assert(observer.notify(observer.context) == -EIO);
    /* Module teardown retires callbacks before the native lifecycle stop. */
    observer = (struct kobox_linux_input_notify){0};
    assert(!ops.stop(ops.context) && closes == 1 && unmapped == 1);
    assert(!ops.stop(ops.context) && closes == 1 && unmapped == 1);
    puts("input notification: attach, pending arrivals, attenuated capability, timestamps, failure and teardown passed");
    return 0;
}
