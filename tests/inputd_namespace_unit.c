#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../userland/inputd/src/input_namespace.c"

/* Execute the real raw-VMO client and codec so a publisher cannot quietly
 * regress to the retired service envelope or fixed payload layout. */
#include "../userland/libfiled_client/src/client.c"
static unsigned char *page;
static size_t page_bytes;
struct entry { char path[480], data[1024]; uint64_t dev; int kind; };
static struct entry entries[128];
static unsigned entry_count, live_handles, page_fds, replies;
static kb2_fs_response_t last_reply;
static int inject_write_error, inject_bad_reply;

static struct entry *find(const char *path)
{
    for (unsigned i = 0; i < entry_count; i++)
        if (entries[i].kind && !strcmp(entries[i].path, path)) return &entries[i];
    return NULL;
}
static int create(const char *path, int kind, struct entry **out)
{
    struct entry *e = find(path);
    if (e) return -EEXIST;
    assert(entry_count < 128);
    e = &entries[entry_count++];
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->kind = kind;
    if (out) *out = e;
    return 0;
}
pacha_fd_t pacha_vmo_create(uint64_t bytes, uint64_t rights, uint32_t flags)
{
    assert(!page && bytes >= 4096 && !(bytes & 4095) && flags == 0);
    assert(rights == (PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE));
    page = calloc(1, bytes); assert(page); page_bytes = bytes;
    page_fds++;
    return 32;
}
void *pacha_mmap(pacha_fd_t fd, uint64_t length, uint64_t prot, uint64_t flags, uint64_t offset)
{
    assert(fd == 32 && length == page_bytes && offset == 0);
    assert(prot == (PACHA_PROT_READ | PACHA_PROT_WRITE) && flags == PACHA_MMAP_SHARED);
    return page;
}
int pacha_munmap(void *address, uint64_t length)
{
    assert(address == page && length == page_bytes);
    return 0;
}
int pacha_fd_close(pacha_fd_t fd)
{
    if (fd == 32) { assert(page_fds); page_fds--; free(page); page = NULL; }
    else { assert(fd == 33 && replies); replies--; }
    return 0;
}
int pacha_fd_get_info(pacha_fd_t fd, struct pacha_fd_info *info)
{ (void)fd; (void)info; assert(0); return -EIO; }
int pacha_ipc_call(pacha_fd_t fd, const struct pacha_ipc_msg *message, struct pacha_ipc_msg *response, uint64_t timeout, int *sent)
{
    assert(fd == 240 && message->fd_count == 1 && message->fds[0].fd == 32);
    kb2_fs_request_t r;
    assert(message->word1 <= page_bytes && message->word2 <= page_bytes);
    assert(!kb2_fs_request_decode(page, message->word1, message->word1, &r));
    assert(!kb2_fs_request_validate_paths(page, message->word1, &r));
    assert(message->word0 == r.opcode && message->word3 == r.sequence);
    const char *path = r.path.length ? (char *)page + r.path.offset : NULL;
    const char *second = r.second_path.length ? (char *)page + r.second_path.offset : NULL;
    last_reply = (kb2_fs_response_t){.opcode = r.opcode,
        .generation = r.generation, .sequence = r.sequence};
    switch (r.opcode) {
    case KB2_FILESYSTEM_OP_MKDIRAT:
        assert(r.mode == 0755); last_reply.result = create(path, 1, NULL); break;
    case KB2_FILESYSTEM_OP_MKNODAT: {
        struct entry *e;
        assert(r.mode == (0020000u | 0660u));
        last_reply.result = create(path, 2, &e);
        if (!last_reply.result) e->dev = r.length;
        break;
    }
    case KB2_FILESYSTEM_OP_SYMLINKAT: {
        struct entry *e; assert(second);
        last_reply.result = create(second, 3, &e);
        if (!last_reply.result) snprintf(e->data, sizeof(e->data), "%s", path);
        break;
    }
    case KB2_FILESYSTEM_OP_OPENAT2: {
        assert(r.mode == 0644 && r.flags == (O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC));
        struct entry *e = find(path);
        if (!e) assert(create(path, 4, &e) == 0);
        e->data[0] = 0; last_reply.handle = (unsigned)(e - entries) + 1;
        live_handles++; break;
    }
    case KB2_FILESYSTEM_OP_PWRITE: {
        assert(r.offset == 0 && r.handle && r.handle <= entry_count && r.length < 1024);
        assert(r.data.length == r.length);
        if (inject_write_error) { last_reply.result = -EIO; inject_write_error = 0; break; }
        struct entry *e = &entries[r.handle - 1];
        memcpy(e->data, page + r.data.offset, r.length); e->data[r.length] = 0;
        last_reply.result = (int64_t)r.length; break;
    }
    case KB2_FILESYSTEM_OP_CLOSE:
        assert(live_handles); live_handles--; break;
    case KB2_FILESYSTEM_OP_RENAMEAT2: {
        struct entry *source = find(path), *target = find(second);
        assert(source && source->kind == 4);
        if (target) target->kind = 0;
        snprintf(source->path, sizeof(source->path), "%s", second);
        break;
    }
    case KB2_FILESYSTEM_OP_UNLINKAT: {
        struct entry *e = find(path);
        if (!e) last_reply.result = -ENOENT;
        else {
            if (r.flags & AT_REMOVEDIR)
                for (unsigned i = 0; i < entry_count; i++)
                    assert(!entries[i].kind ||
                        strncmp(entries[i].path, path, strlen(path)) ||
                        entries[i].path[strlen(path)] != '/');
            e->kind = 0;
        }
        break;
    }
    default: assert(0);
    }
    assert(!kb2_fs_response_encode(page, page_bytes, &last_reply));
    if (sent) *sent = 1;
    assert(timeout == PACHA_FD_WAIT_FOREVER);
    response->word0 = last_reply.opcode;
    response->word1 = (uint64_t)last_reply.result;
    response->word2 = KB2_FILESYSTEM_RESPONSE_SIZE;
    response->word3 = last_reply.sequence + inject_bad_reply;
    inject_bad_reply = 0;
    return 0;
}

int main(void)
{
    struct inputd_public_device d = {.event_index = 300,
        .capabilities = INPUTD_INPUT_CAP_KEYBOARD, .pci_bus = 2};
    kb_input_device_snapshot_t s = {.input_id = {3, 0x1234, 0x5678, 1},
        .event_bits = 3, .key_bits = {0x40000000, 1}, .prop_bits = 1};
    strcpy(s.name, "USB keyboard"); strcpy(s.phys, "usb-port/input0");
    assert(inputd_namespace_publish(240, &d, &s) == 0);
    assert(find("/sys/dev/block"));
    assert(!strcmp(find("/sys/devices/virtual/input/input300/name")->data, "USB keyboard\n"));
    assert(!strcmp(find("/sys/devices/virtual/input/input300/capabilities/key")->data,
        "1 40000000\n"));
    assert(!strcmp(find("/sys/devices/virtual/input/input300/id/vendor")->data, "1234\n"));
    assert(strstr(find("/sys/devices/virtual/input/input300/event300/uevent")->data,
        "MINOR=364\nDEVNAME=input/event300\nID_INPUT=1\nID_INPUT_KEYBOARD=1\n"));
    assert(!strcmp(find("/sys/class/input/event300")->data,
        "/sys/devices/virtual/input/input300/event300"));
    assert(find("/dev/input/event300")->dev == ((13u << 8) | 108u | (256ull << 12)));
    assert(!strcmp(find("/run/udev/data/c13:364")->data,
        "E:ID_INPUT=1\nE:ID_INPUT_KEYBOARD=1\n"));
    assert(!find("/run/udev/data/.c13:364"));
    assert(inputd_namespace_publish(240, &d, &s) == 0);
    assert(inputd_namespace_remove(240, 300) == 0);
    assert(!find("/dev/input/event300") && !find("/sys/class/input/event300"));
    assert(!find("/sys/devices/virtual/input/input300"));
    assert(!find("/run/udev/data/c13:364"));
    assert(find("/sys/class/input") && find("/sys/devices/virtual/input"));
    assert(find("/sys/dev/block"));
    assert(inputd_namespace_remove(240, 300) == 0);
    /* Reset the mock namespace for allocation/failure coverage. */
    memset(entries, 0, sizeof(entries)); entry_count = 0;
    d.event_index = 0; d.capabilities = INPUTD_INPUT_CAP_RELATIVE;
    inject_write_error = 1;
    assert(inputd_namespace_publish(240, &d, &s) == -EIO);
    assert(!find("/sys/devices/virtual/input/input0"));
    assert(!live_handles && !page_fds && !replies);
    assert(inputd_namespace_publish(240, &d, &s) == 0);
    assert(strstr(find("/sys/devices/virtual/input/input0/event0/uevent")->data,
        "ID_INPUT_MOUSE=1\n"));
    assert(!strcmp(find("/run/udev/data/c13:64")->data,
        "E:ID_INPUT=1\nE:ID_INPUT_MOUSE=1\n"));
    inject_bad_reply = 1;
    assert(inputd_namespace_remove(240, 0) == -EPROTO);
    assert(!live_handles && !page_fds && !replies);
    d.event_index = UINT16_MAX + 1u;
    assert(inputd_namespace_publish(240, &d, &s) == -EINVAL);
    puts("inputd namespace: publish, refresh, removal, bitmaps and failures passed");
    return 0;
}
