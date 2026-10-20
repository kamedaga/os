/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "input_namespace.h"
#include "input_island.h"
#include "linux_subsystem/input/input.h"
#include <filed_client/client.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

/* These buffers format inputd's fixed namespace plus a UINT16_MAX index;
 * they are not limits on paths accepted by the filesystem service. */
enum { INPUT_NAMESPACE_PATH_BYTES = 256 };
struct namespace_client { pacha_fd_t endpoint; };

static int begin(struct namespace_client *client, pacha_fd_t endpoint)
{
    client->endpoint = endpoint;
    return endpoint < 16 ? -EINVAL : 0;
}
static void end(struct namespace_client *client) { (void)client; }

/* Native inputd uses its granted endpoint, not personality libc or a
 * kernel-owned registry, so Linux remains the sole namespace authority. */
static int call(struct namespace_client *client, kb2_fs_request_t *request,
    const char *path, const char *second_path, const void *data, size_t bytes,
    kb2_fs_response_t *response)
{
    int status = filedfs_call(client->endpoint, request, path, second_path,
        data, bytes, NULL, 0, response, NULL, 0, NULL, NULL);
    return status ? status : response->result < 0 ? (int)response->result : 0;
}
static int mkdir_path(struct namespace_client *c, const char *path)
{
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_MKDIRAT, .mode = 0755};
    kb2_fs_response_t response;
    int status = call(c, &request, path, NULL, NULL, 0, &response);
    return status == -EEXIST ? 0 : status;
}
static int remove_path(struct namespace_client *c, const char *path, int directory)
{
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_UNLINKAT,
        .flags = directory ? AT_REMOVEDIR : 0};
    kb2_fs_response_t response;
    int status = call(c, &request, path, NULL, NULL, 0, &response);
    return status == -ENOENT ? 0 : status;
}
static int link_path(struct namespace_client *c, const char *path, const char *target)
{
    /* A source generation refresh replaces only inputd-owned links. */
    int status = remove_path(c, path, 0);
    if (status) return status;
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_SYMLINKAT};
    kb2_fs_response_t response;
    return call(c, &request, target, path, NULL, 0, &response);
}
static int write_path(struct namespace_client *c, const char *path, const char *text)
{
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_OPENAT2,
        .flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, .mode = 0644};
    kb2_fs_response_t response;
    int status = call(c, &request, path, NULL, NULL, 0, &response);
    if (status) return status;
    const uint64_t handle = response.handle;
    const size_t length = strlen(text);
    for (size_t offset = 0; !status && offset < length;) {
        request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_PWRITE,
            .handle = handle, .offset = (int64_t)offset, .length = length - offset};
        status = call(c, &request, NULL, NULL, text + offset, length - offset, &response);
        if (!status && (response.result <= 0 ||
            (uint64_t)response.result > length - offset)) status = -EIO;
        if (!status) offset += (size_t)response.result;
    }
    request = (kb2_fs_request_t){.opcode = KB2_FILESYSTEM_OP_CLOSE, .handle = handle};
    int closed = call(c, &request, NULL, NULL, NULL, 0, &response);
    return status ? status : closed;
}

static int publish_udev_database(struct namespace_client *c, unsigned minor,
    const struct inputd_public_device *device)
{
    char path[INPUT_NAMESPACE_PATH_BYTES], temporary[INPUT_NAMESPACE_PATH_BYTES], text[256];
    snprintf(path, sizeof(path), "/run/udev/data/c13:%u", minor);
    snprintf(temporary, sizeof(temporary), "/run/udev/data/.c13:%u", minor);
    snprintf(text, sizeof(text), "E:ID_INPUT=1\n%s%s",
        device->capabilities & INPUTD_INPUT_CAP_KEYBOARD ? "E:ID_INPUT_KEYBOARD=1\n" : "",
        device->capabilities & (INPUTD_INPUT_CAP_RELATIVE | INPUTD_INPUT_CAP_ABSOLUTE) ?
            "E:ID_INPUT_MOUSE=1\n" : "");
    /* libudev's initialized-device enumeration requires its standard RAM
     * database entry, not just properties in sysfs/uevent. Publish it last,
     * atomically, after the real evdev node and attributes are ready; Xorg
     * must never observe a half-written initialization record. */
    int status = write_path(c, temporary, text);
    if (!status) {
        kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_RENAMEAT2};
        kb2_fs_response_t response;
        status = call(c, &request, temporary, path, NULL, 0, &response);
    }
    if (status) (void)remove_path(c, temporary, 0);
    return status;
}

static int attribute(struct namespace_client *c, const char *base,
    const char *name, const char *text)
{
    char path[INPUT_NAMESPACE_PATH_BYTES];
    if (snprintf(path, sizeof(path), "%s/%s", base, name) >= sizeof(path))
        return -ENAMETOOLONG;
    return write_path(c, path, text);
}

static int bitmap(struct namespace_client *c, const char *base,
    const char *name, const uint64_t *words, size_t count)
{
    char text[KB_INPUT_KEY_WORDS * 17 + 2];
    while (count > 1 && words[count - 1] == 0) count--;
    size_t used = 0;
    for (size_t i = count; i > 0; i--) {
        int n = snprintf(text + used, sizeof(text) - used, "%llx%s",
            (unsigned long long)words[i - 1], i == 1 ? "\n" : " ");
        if (n < 0 || (size_t)n >= sizeof(text) - used) return -E2BIG;
        used += (size_t)n;
    }
    return attribute(c, base, name, text);
}

static const char *input_files[] = {"name", "phys", "uniq", "properties", "uevent",
    "subsystem", "id/bustype", "id/vendor", "id/product", "id/version",
    "capabilities/ev", "capabilities/key", "capabilities/rel", "capabilities/abs",
    "capabilities/msc", "capabilities/led", "capabilities/snd",
    "capabilities/ff", "capabilities/sw"};

int inputd_namespace_remove(pacha_fd_t filed_fd, uint32_t index)
{
    if (index > UINT16_MAX) return -EINVAL;
    struct namespace_client c;
    int status = begin(&c, filed_fd);
    char path[INPUT_NAMESPACE_PATH_BYTES], base[160];
    snprintf(base, sizeof(base), "/sys/devices/virtual/input/input%u", index);
#define REMOVE(format, ...) do { \
    snprintf(path, sizeof(path), format, __VA_ARGS__); \
    if (!status) status = remove_path(&c, path, 0); \
} while (0)
    REMOVE("/dev/input/event%u", index);
    REMOVE("/run/udev/data/c13:%u", index + 64);
    REMOVE("/run/udev/data/.c13:%u", index + 64);
    REMOVE("/sys/class/input/event%u", index);
    REMOVE("/sys/class/input/input%u", index);
    REMOVE("/sys/dev/char/13:%u", index + 64);
    const char *event_files[] = {"uevent", "dev", "subsystem", "device"};
    for (size_t i = 0; i < sizeof(event_files)/sizeof(*event_files); i++)
        REMOVE("%s/event%u/%s", base, index, event_files[i]);
    snprintf(path, sizeof(path), "%s/event%u", base, index);
    if (!status) status = remove_path(&c, path, 1);
    for (size_t i = 0; i < sizeof(input_files)/sizeof(*input_files); i++)
        REMOVE("%s/%s", base, input_files[i]);
    const char *dirs[] = {"id", "capabilities"};
    for (size_t i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/%s", base, dirs[i]);
        if (!status) status = remove_path(&c, path, 1);
    }
    if (!status) status = remove_path(&c, base, 1);
#undef REMOVE
    end(&c);
    return status;
}

int inputd_namespace_publish(pacha_fd_t filed_fd, const struct inputd_public_device *d,
    const struct kb_input_device_snapshot *s)
{
    if (!d || !s || d->event_index > UINT16_MAX ||
        !memchr(s->name, 0, sizeof(s->name)) ||
        !memchr(s->phys, 0, sizeof(s->phys)) ||
        !memchr(s->uniq, 0, sizeof(s->uniq))) return -EINVAL;
    struct namespace_client c;
    int status = begin(&c, filed_fd);
    char base[160], event[180], path[INPUT_NAMESPACE_PATH_BYTES], text[1024];
    const unsigned index = d->event_index, minor = index + 64;
    snprintf(base, sizeof(base), "/sys/devices/virtual/input/input%u", index);
    snprintf(event, sizeof(event), "%s/event%u", base, index);
    const char *dirs[] = {"/dev", "/dev/input", "/sys", "/sys/devices",
        "/sys/devices/virtual", "/sys/devices/virtual/input", "/sys/class",
        "/sys/class/input", "/sys/bus", "/sys/bus/input", "/sys/dev", "/sys/dev/char",
        /* Device enumeration scans both indexes, even without block devices.
         * An absent directory aborts libudev-zero's whole scan before input.
         * Create the empty standard index, never a fabricated block device. */
        "/sys/dev/block",
        "/run", "/run/udev", "/run/udev/data"};
    for (size_t i = 0; !status && i < sizeof(dirs)/sizeof(*dirs); i++)
        status = mkdir_path(&c, dirs[i]);
    if (!status) status = mkdir_path(&c, base);
    if (!status) status = mkdir_path(&c, event);
    const char *subdirs[] = {"id", "capabilities"};
    for (size_t i = 0; !status && i < 2; i++) {
        snprintf(path, sizeof(path), "%s/%s", base, subdirs[i]);
        status = mkdir_path(&c, path);
    }
    const char *strings[] = {s->name, s->phys, s->uniq};
    const char *names[] = {"name", "phys", "uniq"};
    for (size_t i = 0; !status && i < 3; i++) {
        snprintf(text, sizeof(text), "%s\n", strings[i]);
        status = attribute(&c, base, names[i], text);
    }
    const uint16_t ids[] = {s->input_id.bustype, s->input_id.vendor,
        s->input_id.product, s->input_id.version};
    const char *id_names[] = {"id/bustype", "id/vendor", "id/product", "id/version"};
    for (size_t i = 0; !status && i < 4; i++) {
        snprintf(text, sizeof(text), "%04x\n", ids[i]);
        status = attribute(&c, base, id_names[i], text);
    }
    const uint64_t *bits[] = {&s->event_bits, s->key_bits, &s->rel_bits,
        &s->abs_bits, &s->msc_bits, &s->led_bits, &s->snd_bits, s->ff_bits, &s->sw_bits};
    const char *bit_names[] = {"capabilities/ev", "capabilities/key", "capabilities/rel",
        "capabilities/abs", "capabilities/msc", "capabilities/led", "capabilities/snd",
        "capabilities/ff", "capabilities/sw"};
    for (size_t i = 0; !status && i < 9; i++)
        status = bitmap(&c, base, bit_names[i], bits[i],
            i == 1 ? KB_INPUT_KEY_WORDS : i == 7 ? KB_INPUT_FF_WORDS : 1);
    if (!status) status = bitmap(&c, base, "properties", &s->prop_bits, 1);
    snprintf(text, sizeof(text), "PRODUCT=%x/%x/%x/%x\n",
        ids[0], ids[1], ids[2], ids[3]);
    if (!status) status = attribute(&c, base, "uevent", text);
    snprintf(text, sizeof(text), "13:%u\n", minor);
    if (!status) status = attribute(&c, event, "dev", text);
    snprintf(text, sizeof(text),
        "MAJOR=13\nMINOR=%u\nDEVNAME=input/event%u\nID_INPUT=1\n%s%s"
        "PACHA_PCI_SLOT_NAME=%04x:%02x:%02x.%u\n", minor, index,
        d->capabilities & INPUTD_INPUT_CAP_KEYBOARD ? "ID_INPUT_KEYBOARD=1\n" : "",
        d->capabilities & (INPUTD_INPUT_CAP_RELATIVE | INPUTD_INPUT_CAP_ABSOLUTE) ?
            "ID_INPUT_MOUSE=1\n" : "",
        d->pci_segment, d->pci_bus, d->pci_device, d->pci_function);
    if (!status) status = attribute(&c, event, "uevent", text);
#define LINK(format, target, ...) do { \
    snprintf(path, sizeof(path), format, __VA_ARGS__); \
    if (!status) status = link_path(&c, path, target); \
} while (0)
    LINK("%s/subsystem", "/sys/bus/input", base);
    LINK("%s/subsystem", "/sys/bus/input", event);
    LINK("%s/device", base, event);
    LINK("/sys/class/input/input%u", base, index);
    LINK("/sys/class/input/event%u", event, index);
    LINK("/sys/dev/char/13:%u", event, minor);
#undef LINK
    if (!status) {
        /* Event numbers belong to inputd's registry, not to an old rootfs
         * fixture. Replace stale placeholder nodes without retargeting handles
         * already held by evdev clients. */
        snprintf(path, sizeof(path), "/dev/input/event%u", index);
        status = remove_path(&c, path, 0);
    }
    if (!status) {
        kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_MKNODAT,
            .mode = 0020000u | 0660u,
            .length = (13u << 8) | (minor & 255u) | ((uint64_t)(minor & ~255u) << 12)};
        kb2_fs_response_t response;
        snprintf(path, sizeof(path), "/dev/input/event%u", index);
        status = call(&c, &request, path, NULL, NULL, 0, &response);
    }
    if (!status) status = publish_udev_database(&c, minor, d);
    end(&c);
    if (status) {
        /* Never announce a half-published device. A later source sync can
         * retry, but directory enumeration must not retain a dead event node. */
        int cleanup = inputd_namespace_remove(filed_fd, index);
        if (cleanup)
            fprintf(stderr, "[inputd] event%u namespace rollback failed status=%d\n",
                index, cleanup);
    }
    return status;
}
