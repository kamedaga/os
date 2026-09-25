#include "live_console.h"

#include "inputd/ipc_protocol.h"
#include "netd/boot_config.h"
#include "pacha/capsule.h"
#include "pacha/ipc.h"
#include "pacha/service_abi.h"
#include "pacha/syscall.h"
#include "termd/ipc_protocol.h"
#include "vterm.h"
#include "font_psf.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    FONT_WIDTH = 8,
    FONT_HEIGHT = 16,
    SCROLLBACK_LINES = 512,
};

struct live_client {
    int endpoint, page_fd;
    void *page;
    uint32_t service;
    uint64_t request_id;
};

struct live_input_slot {
    uint64_t handle;
    int notify_fd;
    uint16_t bus, vendor, product;
    char name[64];
};

struct live_console {
    struct live_client termd, inputd;
    struct live_input_slot *slots;
    size_t slot_count;
    int mmio_fd, master_notify_fd;
    void *framebuffer_reservation;
    size_t framebuffer_reservation_size;
    volatile uint32_t *pixels;
    uint64_t width, height, pitch;
    uint64_t framebuffer_bar_start, framebuffer_bar_size;
    uint64_t framebuffer_device_index;
    unsigned framebuffer_bar;
    int rows, cols;
    uint64_t master;
    char ctty[32];
    VTerm *vt;
    VTermScreen *screen;
    VTermPos cursor;
    int cursor_visible;
    unsigned modifiers;
    char pending[4096];
    size_t pending_len;
    VTermScreenCell *scrollback[SCROLLBACK_LINES];
    size_t scroll_start, scroll_count, scroll_offset;
    int redraw;
};

static int client_init(struct live_client *client, int endpoint, uint32_t service)
{
    memset(client, 0, sizeof(*client));
    client->endpoint = endpoint;
    client->service = service;
    client->page_fd = pacha_vmo_create(PACHA_SERVICE_PAGE_BYTES,
        PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
        PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE, 0);
    if (client->page_fd < 16) return client->page_fd;
    client->page = pacha_mmap(client->page_fd, PACHA_SERVICE_PAGE_BYTES,
        PACHA_PROT_READ | PACHA_PROT_WRITE, PACHA_MMAP_SHARED, 0);
    return client->page == NULL ? -5 : 0;
}

static int client_call(struct live_client *client, uint32_t op,
    uint32_t payload_size, int notify_fd, uint64_t *out_result)
{
    if (payload_size > PACHA_SERVICE_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES)
        return -22;
    const uint64_t id = ++client->request_id;
    pacha_service_envelope_t *header = client->page;
    *header = (pacha_service_envelope_t){
        .magic = PACHA_SERVICE_REQUEST_MAGIC,
        .abi_version = PACHA_SERVICE_ABI_VERSION,
        .service_id = client->service,
        .op = op,
        .flags = PACHA_SERVICE_FLAG_PAGE_PAYLOAD,
        .request_id = id,
        .trace_id = id,
        .payload_size = payload_size,
    };
    struct pacha_ipc_fd fds[2] = {{
        .fd = (uint64_t)client->page_fd,
        .rights = PACHA_FD_RIGHT_CLOSE | PACHA_FD_RIGHT_TRANSFER |
            PACHA_FD_RIGHT_MAP_READ | PACHA_FD_RIGHT_MAP_WRITE,
    }};
    unsigned count = 1;
    if (notify_fd >= 16) {
        fds[1] = (struct pacha_ipc_fd){
            .fd = (uint64_t)notify_fd,
            .rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_TRANSFER |
                PACHA_FD_RIGHT_DUP | PACHA_FD_RIGHT_CLOSE |
                PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV |
                PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL,
        };
        count = 2;
    }
    const struct pacha_ipc_msg request = {
        .word0 = PACHA_SERVICE_REQUEST_MAGIC, .word3 = id,
        .fds = fds, .fd_count = count,
    };
    const int reply_fd = pacha_ipc_call(client->endpoint, &request);
    if (reply_fd < 16) return reply_fd < 0 ? reply_fd : -5;
    struct pacha_ipc_msg reply = {0};
    const int received = pacha_ipc_recv_wait(reply_fd, &reply, PACHA_FD_WAIT_FOREVER);
    (void)pacha_fd_close(reply_fd);
    if (received != 0) return received;
    if (reply.word0 != PACHA_SERVICE_REPLY_MAGIC || reply.word3 != id ||
        header->magic != PACHA_SERVICE_REPLY_MAGIC ||
        header->service_id != client->service || header->op != op ||
        header->request_id != id) return -5;
    if (out_result != NULL) *out_result = header->result;
    return (int)header->status;
}

static int channel_pair(struct pacha_ipc_channel_pair *pair)
{
    const uint64_t rights = PACHA_FD_RIGHT_INSPECT | PACHA_FD_RIGHT_CLOSE |
        PACHA_FD_RIGHT_TRANSFER | PACHA_FD_RIGHT_DUP |
        PACHA_FD_RIGHT_SEND | PACHA_FD_RIGHT_RECV |
        PACHA_FD_RIGHT_WAIT | PACHA_FD_RIGHT_POLL;
    pair->a = pair->b = -1;
    return pacha_ipc_channel_create(pair, rights, 0);
}

static int termd_ioctl(struct live_console *console, uint64_t request,
    uint64_t arg0, uint64_t arg1, uint64_t *result0)
{
    termd_ioctl_request_t *body = (void *)((uint8_t *)console->termd.page +
        PACHA_SERVICE_HEADER_BYTES);
    memset(body, 0, sizeof(*body));
    body->handle = console->master;
    body->request = request;
    body->arg0 = arg0;
    body->arg1 = arg1;
    const int status = client_call(&console->termd, TERMD_OP_HANDLE_IOCTL,
        sizeof(*body), -1, NULL);
    if (status == 0 && result0 != NULL) *result0 = body->result0;
    return status;
}

static int open_master(struct live_console *console)
{
    struct pacha_ipc_channel_pair pair;
    int status = channel_pair(&pair);
    if (status != 0) return status;
    termd_open_request_t *body = (void *)((uint8_t *)console->termd.page +
        PACHA_SERVICE_HEADER_BYTES);
    memset(body, 0, sizeof(*body));
    uint64_t handle = 0;
    status = client_call(&console->termd, TERMD_OP_OPEN_PTMX,
        sizeof(*body), pair.b, &handle);
    (void)pacha_fd_close(pair.b);
    if (status != 0) {
        (void)pacha_fd_close(pair.a);
        return status;
    }
    console->master = handle;
    console->master_notify_fd = pair.a;
    uint64_t pts_index = 0;
    status = termd_ioctl(console, 0x80045430u, 0, 0, &pts_index);
    if (status == 0) status = termd_ioctl(console, 0x40045431u, 0, 0, NULL);
    if (status == 0) status = termd_ioctl(console, 0x5414u,
        (uint64_t)console->rows, (uint64_t)console->cols, NULL);
    if (status == 0) {
        if (pts_index > 65535) return -22;
        (void)snprintf(console->ctty, sizeof(console->ctty),
            "/dev/pts/%llu", (unsigned long long)pts_index);
    }
    return status;
}

static int map_framebuffer(struct live_console *console,
    const pacha_live_root_bootstrap_t *boot,
    const struct pacha_root_device_record *devices, const int *device_fds,
    uint64_t device_count)
{
    if (!boot->framebuffer_paddr || !boot->framebuffer_size ||
        !boot->width || !boot->height || boot->pitch < boot->width ||
        boot->pitch > UINT64_MAX / boot->height / 4 ||
        boot->pitch * boot->height * 4 > boot->framebuffer_size)
        return -22;
    const uint64_t base = boot->framebuffer_paddr & ~UINT64_C(4095);
    const uint64_t offset = boot->framebuffer_paddr - base;
    if (boot->framebuffer_size > UINT64_MAX - offset - 4095) return -22;
    const uint64_t map_size = (offset + boot->framebuffer_size + 4095) & ~UINT64_C(4095);
    for (uint64_t i = 0; i < device_count; i++) {
        if ((devices[i].class_code >> 16) != 3) continue;
        for (unsigned bar = 0; bar < 6; bar++) {
            struct pacha_capsule_bar_info info;
            if (pacha_capsule_pci_bar_info(device_fds[i], bar, &info) != 0 ||
                !(info.flags & PACHA_CAPSULE_BAR_MEM) || !info.size ||
                boot->framebuffer_paddr < info.start ||
                boot->framebuffer_paddr > info.end ||
                boot->framebuffer_size - 1 > info.end - boot->framebuffer_paddr ||
                base < (info.start & ~UINT64_C(4095))) continue;
            const uint64_t bar_base = info.start & ~UINT64_C(4095);
            const uint64_t page_offset = base - bar_base;
            if (info.size > UINT64_MAX - (info.start - bar_base) - 4095)
                continue;
            const uint64_t aperture =
                (info.start - bar_base + info.size + 4095) & ~UINT64_C(4095);
            if (page_offset > aperture || map_size > aperture - page_offset ||
                map_size > SIZE_MAX) continue;
            /* Reserve a kernel-selected, still-unbacked VMA. The capsule
             * overlay atomically replaces only that reservation; closing the
             * lease restores it before we finally unmap it. This avoids a
             * hard-coded VA and races with the process allocator. */
            const long reserved = pacha_syscall6(PACHA_VM_SYSCALL_MMAP,
                0, 0, map_size, 0,
                PACHA_MMAP_PRIVATE | PACHA_MMAP_ANONYMOUS |
                    PACHA_MMAP_NORESERVE, 0);
            if (reserved < 4096) return reserved < 0 ? (int)reserved : -5;
            void *reservation = (void *)(uintptr_t)reserved;
            const int fd = pacha_capsule_derive_mmio_range(device_fds[i], bar,
                reservation, (size_t)map_size,
                PACHA_CAPSULE_MMIO_REPLACE_EXISTING |
                    PACHA_CAPSULE_MMIO_CACHE_UC, page_offset);
            if (fd < 16) {
                (void)pacha_munmap(reservation, map_size);
                continue;
            }
            void *mapping = NULL;
            size_t mapped_size = 0;
            if (pacha_capsule_mmio_mapping(fd, &mapping, &mapped_size) != 0 ||
                mapped_size < map_size) {
                (void)pacha_fd_close(fd);
                (void)pacha_munmap(reservation, map_size);
                continue;
            }
            console->mmio_fd = fd;
            console->framebuffer_reservation = reservation;
            console->framebuffer_reservation_size = (size_t)map_size;
            console->pixels = (volatile uint32_t *)((uint8_t *)mapping + offset);
            console->width = boot->width;
            console->height = boot->height;
            console->pitch = boot->pitch;
            console->framebuffer_bar_start = info.start;
            console->framebuffer_bar_size = info.size;
            console->framebuffer_device_index = i;
            console->framebuffer_bar = bar;
            console->rows = (int)(boot->height / FONT_HEIGHT);
            console->cols = (int)(boot->width / FONT_WIDTH);
            return console->rows && console->cols ? 0 : -22;
        }
    }
    return -19;
}

static int cursor_moved(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    (void)oldpos;
    struct live_console *console = user;
    console->cursor = pos;
    console->cursor_visible = visible;
    return 1;
}

static int scroll_push(int cols, const VTermScreenCell *cells, void *user)
{
    struct live_console *console = user;
    if (cols != console->cols || cells == NULL) return 0;
    VTermScreenCell *line = malloc((size_t)cols * sizeof(*line));
    if (line == NULL) return 0;
    memcpy(line, cells, (size_t)cols * sizeof(*line));
    if (console->scroll_count == SCROLLBACK_LINES) {
        free(console->scrollback[console->scroll_start]);
        console->scrollback[console->scroll_start] = NULL;
        console->scroll_start = (console->scroll_start + 1) % SCROLLBACK_LINES;
        console->scroll_count--;
    }
    const size_t slot = (console->scroll_start + console->scroll_count) %
        SCROLLBACK_LINES;
    console->scrollback[slot] = line;
    console->scroll_count++;
    if (console->scroll_offset && console->scroll_offset < console->scroll_count)
        console->scroll_offset++;
    return 1;
}

static int scroll_pop(int cols, VTermScreenCell *cells, void *user)
{
    struct live_console *console = user;
    if (cols != console->cols || cells == NULL || !console->scroll_count)
        return 0;
    const size_t slot = (console->scroll_start + console->scroll_count - 1) %
        SCROLLBACK_LINES;
    memcpy(cells, console->scrollback[slot], (size_t)cols * sizeof(*cells));
    free(console->scrollback[slot]);
    console->scrollback[slot] = NULL;
    console->scroll_count--;
    if (console->scroll_offset > console->scroll_count)
        console->scroll_offset = console->scroll_count;
    return 1;
}

static int scroll_clear(void *user)
{
    struct live_console *console = user;
    for (size_t i = 0; i < console->scroll_count; i++) {
        const size_t slot = (console->scroll_start + i) % SCROLLBACK_LINES;
        free(console->scrollback[slot]);
        console->scrollback[slot] = NULL;
    }
    console->scroll_start = console->scroll_count = console->scroll_offset = 0;
    return 1;
}

static void scroll_view(struct live_console *console, int lines)
{
    if (lines > 0) {
        const size_t amount = (size_t)lines;
        console->scroll_offset = amount > console->scroll_count - console->scroll_offset ?
            console->scroll_count : console->scroll_offset + amount;
    } else if (lines < 0) {
        const size_t amount = (size_t)(-lines);
        console->scroll_offset = amount >= console->scroll_offset ?
            0 : console->scroll_offset - amount;
    }
    console->redraw = 1;
}

static uint32_t cell_color(VTermScreen *screen, VTermColor color, uint32_t fallback)
{
    if (VTERM_COLOR_IS_DEFAULT_FG(&color) || VTERM_COLOR_IS_DEFAULT_BG(&color))
        return fallback;
    vterm_screen_convert_color_to_rgb(screen, &color);
    return ((uint32_t)color.rgb.red << 16) |
        ((uint32_t)color.rgb.green << 8) | color.rgb.blue;
}

static void render(struct live_console *console)
{
    vterm_screen_flush_damage(console->screen);
    for (int row = 0; row < console->rows; row++) {
        for (int col = 0; col < console->cols; col++) {
            VTermScreenCell cell;
            const size_t line = console->scroll_count + (size_t)row -
                console->scroll_offset;
            if (line < console->scroll_count) {
                const size_t slot = (console->scroll_start + line) %
                    SCROLLBACK_LINES;
                cell = console->scrollback[slot][col];
            } else if (!vterm_screen_get_cell(console->screen,
                    (VTermPos){.row = (int)(line - console->scroll_count),
                        .col = col}, &cell)) continue;
            uint32_t fg = cell_color(console->screen, cell.fg, 0x00d8e4f0);
            uint32_t bg = cell_color(console->screen, cell.bg, 0x00000000);
            if (cell.attrs.reverse) { const uint32_t tmp = fg; fg = bg; bg = tmp; }
            uint32_t ch = cell.chars[0];
            if (ch > 255) ch = '?';
            if (ch == 0) ch = ' ';
            const uint8_t *glyph = font_psf + 4 + ch * FONT_HEIGHT;
            const int cursor = console->scroll_offset == 0 &&
                console->cursor_visible &&
                console->cursor.row == row && console->cursor.col == col;
            for (int y = 0; y < FONT_HEIGHT; y++) {
                volatile uint32_t *line = console->pixels +
                    (uint64_t)(row * FONT_HEIGHT + y) * console->pitch +
                    (uint64_t)col * FONT_WIDTH;
                for (int x = 0; x < FONT_WIDTH; x++) {
                    const int ink = (glyph[y] >> (7 - x)) & 1;
                    line[x] = cursor ? (ink ? bg : fg) : (ink ? fg : bg);
                }
            }
        }
    }
    console->redraw = 0;
}

static void keyboard_output(const char *bytes, size_t len, void *user)
{
    struct live_console *console = user;
    if (len > sizeof(console->pending) - console->pending_len) return;
    memcpy(console->pending + console->pending_len, bytes, len);
    console->pending_len += len;
}

static int flush_keyboard(struct live_console *console)
{
    while (console->pending_len) {
        termd_io_request_t *body = (void *)((uint8_t *)console->termd.page +
            PACHA_SERVICE_HEADER_BYTES);
        memset(body, 0, sizeof(*body));
        body->handle = console->master;
        body->flags = TERMD_IO_F_NOWAIT;
        body->length = console->pending_len;
        memcpy(body->data, console->pending, console->pending_len);
        uint64_t written = 0;
        const int status = client_call(&console->termd, TERMD_OP_HANDLE_WRITE,
            sizeof(*body), -1, &written);
        if (status == -11) return 0;
        if (status != 0) return status;
        if (!written || written > console->pending_len) return -5;
        console->pending_len -= (size_t)written;
        memmove(console->pending, console->pending + written, console->pending_len);
    }
    return 0;
}

static int drain_master(struct live_console *console, int *changed)
{
    for (unsigned pass = 0; pass < 32; pass++) {
        termd_io_request_t *body = (void *)((uint8_t *)console->termd.page +
            PACHA_SERVICE_HEADER_BYTES);
        memset(body, 0, sizeof(*body));
        body->handle = console->master;
        body->length = TERMD_IO_BYTES;
        body->flags = TERMD_IO_F_NOWAIT;
        uint64_t received = 0;
        const int status = client_call(&console->termd, TERMD_OP_HANDLE_READ,
            sizeof(*body), -1, &received);
        if (status == -11 || status == -5) return 0;
        if (status != 0) return status;
        if (!received || received > TERMD_IO_BYTES) return 0;
        vterm_input_write(console->vt, (const char *)body->data, (size_t)received);
        *changed = 1;
    }
    return 0;
}

static void input_slot_describe(struct live_console *console, size_t index)
{
    struct live_input_slot *slot = &console->slots[index];
    /* Names and IDs come from the published evdev device, since one xHCI
     * controller can expose several unrelated HID functions. */
    inputd_ioctl_request_t *body = (void *)((uint8_t *)console->inputd.page +
        PACHA_SERVICE_HEADER_BYTES);
    memset(body, 0, sizeof(*body));
    body->handle = slot->handle;
    body->request = 0x80084502u;
    body->data_size = 8;
    if (client_call(&console->inputd, INPUTD_OP_IOCTL,
            sizeof(*body), -1, NULL) == 0 && body->result_size >= 8) {
        uint16_t id[4];
        memcpy(id, body->data, sizeof(id));
        slot->bus = id[0];
        slot->vendor = id[1];
        slot->product = id[2];
    }
    memset(body, 0, sizeof(*body));
    body->handle = slot->handle;
    body->request = 0x80404506u;
    body->data_size = sizeof(slot->name);
    if (client_call(&console->inputd, INPUTD_OP_IOCTL,
            sizeof(*body), -1, NULL) != 0 || body->result_size == 0) return;
    size_t length = body->result_size;
    if (length >= sizeof(slot->name)) length = sizeof(slot->name) - 1;
    for (size_t i = 0; i < length && body->data[i] != 0; i++) {
        const unsigned char ch = body->data[i];
        slot->name[i] = ch >= 0x20 && ch <= 0x7e ? ch : '?';
    }
}

static int input_slot_open(struct live_console *console, size_t index)
{
    struct pacha_ipc_channel_pair pair;
    int status = channel_pair(&pair);
    if (status != 0) return status;
    inputd_open_request_t *body = (void *)((uint8_t *)console->inputd.page +
        PACHA_SERVICE_HEADER_BYTES);
    *body = (inputd_open_request_t){.event_index = (uint32_t)index};
    uint64_t handle = 0;
    status = client_call(&console->inputd, INPUTD_OP_OPEN, sizeof(*body),
        pair.b, &handle);
    (void)pacha_fd_close(pair.b);
    if (status != 0) {
        (void)pacha_fd_close(pair.a);
        return status;
    }
    console->slots[index].handle = handle;
    console->slots[index].notify_fd = pair.a;
    input_slot_describe(console, index);
    const struct live_input_slot *slot = &console->slots[index];
    printf("[live_console] input event%zu opened handle=%llu bus=%04x id=%04x:%04x name=%s\n",
        index, (unsigned long long)handle, slot->bus, slot->vendor,
        slot->product, slot->name[0] ? slot->name : "unknown");
    fflush(stdout);
    return 0;
}

static void input_slot_close(struct live_console *console, size_t index)
{
    struct live_input_slot *slot = &console->slots[index];
    if (slot->handle) {
        inputd_handle_request_t *body = (void *)((uint8_t *)console->inputd.page +
            PACHA_SERVICE_HEADER_BYTES);
        body->handle = slot->handle;
        (void)client_call(&console->inputd, INPUTD_OP_CLOSE, sizeof(*body), -1, NULL);
    }
    if (slot->notify_fd >= 16) (void)pacha_fd_close(slot->notify_fd);
    *slot = (struct live_input_slot){.notify_fd = -1};
}

static void key_event(struct live_console *console, uint16_t code, int32_t value)
{
    if ((code == 104 || code == 109) && value != 0 &&
        (console->modifiers & VTERM_MOD_SHIFT)) {
        scroll_view(console, code == 104 ? console->rows / 2 :
            -(console->rows / 2));
        return;
    }
    if (code == 42 || code == 54) {
        if (value) console->modifiers |= VTERM_MOD_SHIFT;
        else console->modifiers &= ~VTERM_MOD_SHIFT;
        return;
    }
    if (code == 29 || code == 97) {
        if (value) console->modifiers |= VTERM_MOD_CTRL;
        else console->modifiers &= ~VTERM_MOD_CTRL;
        return;
    }
    if (code == 56 || code == 100) {
        if (value) console->modifiers |= VTERM_MOD_ALT;
        else console->modifiers &= ~VTERM_MOD_ALT;
        return;
    }
    if (value != 1 && value != 2) return;
    if (console->scroll_offset) {
        console->scroll_offset = 0;
        console->redraw = 1;
    }
    VTermKey key = VTERM_KEY_NONE;
    switch (code) {
    case 1: key = VTERM_KEY_ESCAPE; break;
    case 14: key = VTERM_KEY_BACKSPACE; break;
    case 15: key = VTERM_KEY_TAB; break;
    case 28: key = VTERM_KEY_ENTER; break;
    case 103: key = VTERM_KEY_UP; break;
    case 108: key = VTERM_KEY_DOWN; break;
    case 105: key = VTERM_KEY_LEFT; break;
    case 106: key = VTERM_KEY_RIGHT; break;
    case 102: key = VTERM_KEY_HOME; break;
    case 107: key = VTERM_KEY_END; break;
    case 110: key = VTERM_KEY_INS; break;
    case 111: key = VTERM_KEY_DEL; break;
    case 104: key = VTERM_KEY_PAGEUP; break;
    case 109: key = VTERM_KEY_PAGEDOWN; break;
    default: break;
    }
    if (key != VTERM_KEY_NONE) {
        vterm_keyboard_key(console->vt, key, (VTermModifier)console->modifiers);
        return;
    }
    static const char plain[58] = {
        [2]='1',[3]='2',[4]='3',[5]='4',[6]='5',[7]='6',[8]='7',[9]='8',
        [10]='9',[11]='0',[12]='-',[13]='=',[16]='q',[17]='w',[18]='e',
        [19]='r',[20]='t',[21]='y',[22]='u',[23]='i',[24]='o',[25]='p',
        [26]='[',[27]=']',[30]='a',[31]='s',[32]='d',[33]='f',[34]='g',
        [35]='h',[36]='j',[37]='k',[38]='l',[39]=';',[40]='\'',
        [41]='`',[43]='\\',[44]='z',[45]='x',[46]='c',[47]='v',[48]='b',
        [49]='n',[50]='m',[51]=',',[52]='.',[53]='/',[57]=' '
    };
    static const char shifted[58] = {
        [2]='!',[3]='@',[4]='#',[5]='$',[6]='%',[7]='^',[8]='&',[9]='*',
        [10]='(',[11]=')',[12]='_',[13]='+',[16]='Q',[17]='W',[18]='E',
        [19]='R',[20]='T',[21]='Y',[22]='U',[23]='I',[24]='O',[25]='P',
        [26]='{',[27]='}',[30]='A',[31]='S',[32]='D',[33]='F',[34]='G',
        [35]='H',[36]='J',[37]='K',[38]='L',[39]=':',[40]='"',
        [41]='~',[43]='|',[44]='Z',[45]='X',[46]='C',[47]='V',[48]='B',
        [49]='N',[50]='M',[51]='<',[52]='>',[53]='?',[57]=' '
    };
    if (code < sizeof(plain) && plain[code]) {
        const int shift = (console->modifiers & VTERM_MOD_SHIFT) != 0;
        vterm_keyboard_unichar(console->vt,
            (uint8_t)(shift ? shifted[code] : plain[code]),
            (VTermModifier)(console->modifiers & ~VTERM_MOD_SHIFT));
    }
}

static int drain_input_slot(struct live_console *console, size_t index)
{
    struct live_input_slot *slot = &console->slots[index];
    if (!slot->handle) return 0;
    for (unsigned pass = 0; pass < 8; pass++) {
        inputd_read_request_t *body = (void *)((uint8_t *)console->inputd.page +
            PACHA_SERVICE_HEADER_BYTES);
        memset(body, 0, sizeof(*body));
        body->handle = slot->handle;
        body->event_capacity = INPUTD_EVENT_CAPACITY;
        uint64_t count = 0;
        const int status = client_call(&console->inputd, INPUTD_OP_READ,
            sizeof(*body), -1, &count);
        if (status == -11) return 0;
        if (status == -19 || status == -9) {
            printf("[live_console] input event%zu detached\n", index);
            input_slot_close(console, index);
            return 0;
        }
        if (status != 0) return status;
        if (count != body->event_count || count > INPUTD_EVENT_CAPACITY) return -5;
        for (size_t i = 0; i < count; i++) {
            if (body->events[i].type == 1)
                key_event(console, body->events[i].code, body->events[i].value);
            else if (body->events[i].type == 2 && body->events[i].code == 8 &&
                body->events[i].value != 0)
                scroll_view(console, body->events[i].value * 3);
        }
        if (count < INPUTD_EVENT_CAPACITY) return 0;
    }
    return 0;
}

static int scan_inputs(struct live_console *console)
{
    for (size_t i = 0; i <= console->slot_count; i++) {
        if (i == console->slot_count) {
            if (i >= UINT16_MAX) break;
            struct live_input_slot *slots = realloc(console->slots,
                (i + 1) * sizeof(*slots));
            if (slots == NULL) return -12;
            console->slots = slots;
            console->slots[i] = (struct live_input_slot){.notify_fd = -1};
            console->slot_count++;
        }
        if (console->slots[i].handle) continue;
        const int status = input_slot_open(console, i);
        if (status != 0 && status != -19) return status;
        if (i + 1 == console->slot_count && status == -19) break;
    }
    return 0;
}

int live_console_prepare(const pacha_live_root_bootstrap_t *boot,
    const struct pacha_root_device_record *devices, const int *device_fds,
    uint64_t device_count, int termd_fd, int inputd_fd,
    struct live_console **out)
{
    if (out == NULL) return -22;
    *out = NULL;
    struct live_console *console = calloc(1, sizeof(*console));
    if (console == NULL) return -12;
    console->mmio_fd = console->master_notify_fd = -1;
    console->termd.page_fd = console->inputd.page_fd = -1;
    int status = map_framebuffer(console, boot, devices, device_fds, device_count);
    if (status == 0) status = client_init(&console->termd, termd_fd, TERMD_SERVICE_ID);
    if (status == 0) status = client_init(&console->inputd, inputd_fd, INPUTD_SERVICE_ID);
    if (status == 0) {
        console->vt = vterm_new(console->rows, console->cols);
        if (console->vt == NULL) status = -12;
    }
    if (status == 0) {
        console->screen = vterm_obtain_screen(console->vt);
        if (console->screen == NULL) status = -12;
    }
    if (status == 0) {
        static const VTermScreenCallbacks callbacks = {
            .movecursor = cursor_moved,
            .sb_pushline = scroll_push,
            .sb_popline = scroll_pop,
            .sb_clear = scroll_clear,
        };
        vterm_screen_set_callbacks(console->screen, &callbacks, console);
        vterm_screen_enable_altscreen(console->screen, 1);
        vterm_screen_reset(console->screen, 1);
        vterm_set_utf8(console->vt, 1);
        vterm_output_set_callback(console->vt, keyboard_output, console);
        console->cursor_visible = 1;
        status = open_master(console);
    }
    if (status == 0) {
        char line[192];
        const char *banner = "PachaOS/amd64 (live) (ttyv0)\r\n";
        vterm_input_write(console->vt, banner, strlen(banner));
        pacha_system_info_t system = {0};
        if (pacha_syscall1(PACHA_RUNTIME_SYSCALL_SYSTEM_INFO,
                (uint64_t)(uintptr_t)&system) == 0) {
            (void)snprintf(line, sizeof(line),
                "SMP: %llu logical CPUs online\r\n",
                (unsigned long long)system.online_cpu_count);
            vterm_input_write(console->vt, line, strlen(line));
            (void)snprintf(line, sizeof(line),
                "memory: %llu MiB usable, %llu MiB free now\r\n",
                (unsigned long long)(system.total_usable_memory_bytes >> 20),
                (unsigned long long)(system.free_memory_bytes >> 20));
            vterm_input_write(console->vt, line, strlen(line));
        }
        const char *archive =
            "bootfs0: archive validated; / populated on tmpfs (RAM only)\r\n";
        vterm_input_write(console->vt, archive, strlen(archive));
        (void)snprintf(line, sizeof(line),
            "pci0: %llu device capsules handed to live root\r\n",
            (unsigned long long)device_count);
        vterm_input_write(console->vt, line, strlen(line));
        unsigned translated = 0, quarantined = 0, untranslated = 0, unknown = 0;
        for (uint64_t i = 0; i < device_count; i++) {
            struct pacha_capsule_info info;
            if (device_fds[i] < 16 ||
                pacha_capsule_query(device_fds[i], &info) != 0 ||
                info.kind != PACHA_CAPSULE_KIND_DEVICE) {
                unknown++;
            } else if (info.flags & PACHA_CAPSULE_DMA_QUARANTINED) {
                quarantined++;
            } else if (info.flags & PACHA_CAPSULE_DMA_TRANSLATED) {
                translated++;
            } else untranslated++;
        }
        (void)snprintf(line, sizeof(line),
            "iommu0: DMA translated=%u quarantined=%u untranslated=%u unknown=%u\r\n",
            translated, quarantined, untranslated, unknown);
        vterm_input_write(console->vt, line, strlen(line));
        unsigned display_unit = 0, xhci_unit = 0, network_unit = 0;
        for (uint64_t i = 0; i < device_count; i++) {
            const struct pacha_root_device_record *device = &devices[i];
            struct pacha_capsule_info info;
            const int queried = device_fds[i] >= 16 &&
                pacha_capsule_query(device_fds[i], &info) == 0 &&
                info.kind == PACHA_CAPSULE_KIND_DEVICE;
            const char *dma = !queried ? "unknown" :
                (info.flags & PACHA_CAPSULE_DMA_QUARANTINED) ? "quarantined" :
                (info.flags & PACHA_CAPSULE_DMA_TRANSLATED) ? "translated" :
                "untranslated";
            const char *kind = NULL;
            unsigned unit = 0;
            if ((device->class_code >> 16) == 3) {
                kind = "vgapci";
                unit = display_unit++;
            } else if (device->class_code == 0x0c0330) {
                kind = "xhci";
                unit = xhci_unit++;
            } else if ((device->class_code >> 16) == 2) {
                kind = "pci-net";
                unit = network_unit++;
            }
            if (kind == NULL) continue;
            (void)snprintf(line, sizeof(line),
                "%s%u: <%04llx:%04llx class=%06llx> at pci%04x:%02x:%02x.%u\r\n",
                kind, unit, (unsigned long long)device->vendor_id,
                (unsigned long long)device->device_id,
                (unsigned long long)device->class_code,
                device->pci_segment, device->pci_bus,
                device->pci_device, device->pci_function);
            vterm_input_write(console->vt, line, strlen(line));
            if (queried && info.size != 0) {
                (void)snprintf(line, sizeof(line),
                    "  DMA: %s; IOVA base=0x%llx size=0x%llx\r\n",
                    dma, (unsigned long long)info.iova,
                    (unsigned long long)info.size);
            } else {
                (void)snprintf(line, sizeof(line), "  DMA: %s; IOVA unavailable\r\n", dma);
            }
            vterm_input_write(console->vt, line, strlen(line));
            for (unsigned bar = 0; bar < 6; bar++) {
                struct pacha_capsule_bar_info bar_info;
                if (device_fds[i] < 16 ||
                    pacha_capsule_pci_bar_info(device_fds[i], bar, &bar_info) != 0 ||
                    bar_info.size == 0) continue;
                (void)snprintf(line, sizeof(line),
                    "  BAR%u: %s base=0x%llx size=0x%llx%s\r\n",
                    bar, (bar_info.flags & PACHA_CAPSULE_BAR_MEM) ? "mem" : "io",
                    (unsigned long long)bar_info.start,
                    (unsigned long long)bar_info.size,
                    (bar_info.flags & PACHA_CAPSULE_BAR_PREFETCHABLE) ?
                        " prefetch" : "");
                vterm_input_write(console->vt, line, strlen(line));
            }
        }
        (void)snprintf(line, sizeof(line),
            "fb0: GOP %llux%llu pitch=%llu pixels phys=0x%llx\r\n",
            (unsigned long long)console->width,
            (unsigned long long)console->height,
            (unsigned long long)console->pitch,
            (unsigned long long)boot->framebuffer_paddr);
        vterm_input_write(console->vt, line, strlen(line));
        const struct pacha_root_device_record *fb_device =
            &devices[console->framebuffer_device_index];
        (void)snprintf(line, sizeof(line),
            "fb0: pci%04x:%02x:%02x.%u BAR%u offset=0x%llx size=0x%llx\r\n",
            fb_device->pci_segment, fb_device->pci_bus,
            fb_device->pci_device, fb_device->pci_function,
            console->framebuffer_bar,
            (unsigned long long)(boot->framebuffer_paddr -
                console->framebuffer_bar_start),
            (unsigned long long)console->framebuffer_bar_size);
        vterm_input_write(console->vt, line, strlen(line));
        const char *services =
            "unixd0: <local IPC> ready\r\n"
            "termd0: <Linux TTY and PTY> ready\r\n"
            "inputd0: <evdev input broker> ready\r\n"
            "lprs0: <Linux ABI supervisor> started\r\n";
        vterm_input_write(console->vt, services, strlen(services));
        for (unsigned unit = 0; unit < xhci_unit; unit++) {
            (void)snprintf(line, sizeof(line),
                "usbd%u: host process launched for xhci%u\r\n",
                unit, unit);
            vterm_input_write(console->vt, line, strlen(line));
        }
        (void)snprintf(line, sizeof(line),
            "ttyv0: <framebuffer console> %dx%d on %s\r\n",
            console->cols, console->rows, console->ctty);
        vterm_input_write(console->vt, line, strlen(line));
        render(console);
        printf("[live_console] framebuffer %llux%llu mapped, ctty=%s\n",
            (unsigned long long)console->width,
            (unsigned long long)console->height, console->ctty);
        fflush(stdout);
        *out = console;
        return 0;
    }
    live_console_destroy(console);
    return status;
}

const char *live_console_ctty(const struct live_console *console)
{
    return console != NULL ? console->ctty : NULL;
}

void live_console_report_usb_boot(struct live_console *console,
    const int *ready_fds, size_t ready_count)
{
    if (console == NULL) return;
    char line[192];
    for (size_t i = 0; i < ready_count; i++) {
        const int state = ready_fds[i];
        if (state == -1) {
            (void)snprintf(line, sizeof(line),
                "usbd%zu: ready; Kobox2 xHCI sandbox and inputd bridge synced\r\n", i);
            vterm_input_write(console->vt, line, strlen(line));
            const char *modules =
                "  Linux: usbcore, xhci-hcd, xhci-pci, usbhid, hid-generic loaded\r\n";
            vterm_input_write(console->vt, modules, strlen(modules));
        } else if (state == -2) {
            (void)snprintf(line, sizeof(line),
                "usbd%zu: startup failed during readiness handshake\r\n", i);
            vterm_input_write(console->vt, line, strlen(line));
        } else if (state >= 16) {
            (void)snprintf(line, sizeof(line),
                "usbd%zu: sandbox readiness pending; USB enumeration is asynchronous\r\n", i);
            vterm_input_write(console->vt, line, strlen(line));
        }
    }
    const int scan_status = scan_inputs(console);
    if (scan_status != 0) {
        (void)snprintf(line, sizeof(line),
            "inputd0: initial evdev scan failed status=%d; retrying\r\n",
            scan_status);
        vterm_input_write(console->vt, line, strlen(line));
        return;
    }
    unsigned visible = 0;
    for (size_t i = 0; i < console->slot_count; i++) {
        const struct live_input_slot *slot = &console->slots[i];
        if (!slot->handle) continue;
        visible++;
        (void)snprintf(line, sizeof(line),
            "inputd0: event%zu <%04x:%04x bus=%04x> %s\r\n",
            i, slot->vendor, slot->product, slot->bus,
            slot->name[0] ? slot->name : "name unavailable");
        vterm_input_write(console->vt, line, strlen(line));
    }
    if (visible == 0) {
        const char *empty =
            "inputd0: no evdev nodes at initial scan; hotplug scan active\r\n";
        vterm_input_write(console->vt, empty, strlen(empty));
    }
}

void live_console_report_net_boot(struct live_console *console,
    int state, int status, uint64_t stage, unsigned carrier, unsigned mtu,
    unsigned nic_step, int detail, unsigned loaded, unsigned pci_bound,
    uint64_t source, unsigned source_line,
    unsigned fault_vector, unsigned fault_error_code,
    unsigned fault_core_relative, uint64_t fault_ip, uint64_t fault_address)
{
    if (console == NULL) return;
    char line[192];
    const char *phase = stage == NETD_BOOT_STAGE_NIC ? "NIC initialization" :
        stage == NETD_BOOT_STAGE_SOCKET ? "socket service" :
        stage == NETD_BOOT_STAGE_TIMER ? "poll timer" : "process launch";
    const char *nic_phase = nic_step == NETD_NIC_STEP_PACKAGE ? "package validation" :
        nic_step == NETD_NIC_STEP_CONTROLLER ? "Kobox2 controller" :
        nic_step == NETD_NIC_STEP_RESOURCE ? "resource grant" :
        nic_step == NETD_NIC_STEP_CHANNEL ? "sandbox channel" :
        nic_step == NETD_NIC_STEP_SANDBOX_LAUNCH ? "sandbox launch" :
        nic_step == NETD_NIC_STEP_PROCESS_WATCH ? "process watch" :
        nic_step == NETD_NIC_STEP_TRANSFER ? "module transfer" :
        nic_step == NETD_NIC_STEP_LIFECYCLE ? "Linux module probe" :
        nic_step == NETD_NIC_STEP_FRAME_ATTACH ? "frame attach" :
        nic_step == NETD_NIC_STEP_FRAME_INFO ? "frame info" :
        nic_step == NETD_NIC_STEP_LINK_ATTACH ? "link attach" :
        "unknown NIC stage";
    if (state == 0)
        (void)snprintf(line, sizeof(line), "netd0: no Ethernet function handed to live root\r\n");
    else if (state == 1)
        (void)snprintf(line, sizeof(line),
            "netd0: <Kobox2 network> initialization pending asynchronously\r\n");
    else if (state == 2)
        (void)snprintf(line, sizeof(line),
            "netd0: <Kobox2 network> %s; carrier=%s mtu=%u\r\n",
            stage == NETD_BOOT_STAGE_LINK ? "link update" : "NIC ready",
            carrier ? "up" : "down", mtu);
    else if (stage == NETD_BOOT_STAGE_FAULT) {
        (void)snprintf(line, sizeof(line),
            "netd0: Linux fault status=%d vector=%u error=0x%x\r\n",
            status, fault_vector, fault_error_code);
        vterm_input_write(console->vt, line, strlen(line));
        (void)snprintf(line, sizeof(line),
            "  %s0x%llx address=0x%llx\r\n",
            fault_core_relative ? "core+" : "ip=",
            (unsigned long long)fault_ip,
            (unsigned long long)fault_address);
    } else if (stage == NETD_BOOT_STAGE_NIC && source) {
        char basename[9] = {0};
        for (unsigned i = 0; i < 8; ++i)
            basename[i] = (char)(source >> (i * 8));
        (void)snprintf(line, sizeof(line),
            "netd0: %s failed status=%d cause=%s:%u value=%d\r\n",
            nic_phase, status, basename, source_line, detail);
    } else if (stage == NETD_BOOT_STAGE_NIC)
        (void)snprintf(line, sizeof(line),
            "netd0: %s failed status=%d detail=%d loaded=%u pci-bound=%u\r\n",
            nic_phase, status, detail, loaded, pci_bound);
    else
        (void)snprintf(line, sizeof(line),
            "netd0: %s failed status=%d\r\n",
            phase, status);
    vterm_input_write(console->vt, line, strlen(line));
    console->redraw = 1;
}

void live_console_finish_boot(struct live_console *console)
{
    if (console == NULL) return;
    const char *shell_line = "init: /bin/ash spawned on ttyv0\r\n";
    vterm_input_write(console->vt, shell_line, strlen(shell_line));
    /* Scroll exactly the visible boot report into libvterm's history. Unlike
     * CSI erase this preserves it for Shift+PageUp, and the required line
     * count follows the actual cursor/log positions and framebuffer height. */
    VTermPos cursor = {0};
    vterm_state_get_cursorpos(vterm_obtain_state(console->vt), &cursor);
    int last_text_row = -1;
    for (int row = 0; row < console->rows; row++) {
        for (int col = 0; col < console->cols; col++) {
            VTermScreenCell cell;
            if (vterm_screen_get_cell(console->screen,
                    (VTermPos){.row = row, .col = col}, &cell) &&
                cell.chars[0] != 0 && cell.chars[0] != ' ') {
                last_text_row = row;
                break;
            }
        }
    }
    const int to_bottom = cursor.row < console->rows - 1 ?
        console->rows - 1 - cursor.row : 0;
    const int blank_lines = last_text_row < 0 ? 0 :
        to_bottom + last_text_row + 1;
    for (int row = 0; row < blank_lines; row++)
        vterm_input_write(console->vt, "\r\n", 2);
    /* A linefeed alone leaves the shell at the bottom row. The viewport is
     * blank now, so home positions its first prompt at the top-left. */
    vterm_input_write(console->vt, "\033[H", 3);
    render(console);
}

int live_console_run(struct live_console *console,
    void (*on_tick)(void *context), void *tick_context)
{
    if (console == NULL) return -22;
    const int timer_fd = pacha_timerfd_create(UINT64_C(20000000),
        UINT64_C(20000000), PACHA_FD_RIGHT_READ | PACHA_FD_RIGHT_WAIT |
        PACHA_FD_RIGHT_POLL | PACHA_FD_RIGHT_CLOSE, 0);
    if (timer_fd < 16) return -5;
    int status = 0;
    for (;;) {
        if (on_tick != NULL) on_tick(tick_context);
        int changed = 0;
        status = scan_inputs(console);
        if (status == 0) {
            for (size_t i = 0; i < console->slot_count; i++) {
                status = drain_input_slot(console, i);
                if (status != 0) break;
            }
        }
        if (status == 0) status = flush_keyboard(console);
        if (status == 0) status = drain_master(console, &changed);
        if (status != 0) break;
        if (changed || console->redraw) render(console);
        struct pacha_pollfd tick = {.fd = timer_fd, .events = PACHA_FD_EVENT_READABLE};
        uint64_t count = 0;
        if (pacha_fd_wait_many(&tick, 1, PACHA_FD_WAIT_FOREVER) != 1 ||
            pacha_fd_read(timer_fd, &count, sizeof(count)) != sizeof(count)) {
            status = -5;
            break;
        }
    }
    (void)pacha_fd_close(timer_fd);
    return status;
}

void live_console_destroy(struct live_console *console)
{
    if (console == NULL) return;
    for (size_t i = 0; i < console->slot_count; i++)
        if (console->slots[i].handle) input_slot_close(console, i);
    free(console->slots);
    if (console->master) {
        termd_handle_request_t *body = (void *)((uint8_t *)console->termd.page +
            PACHA_SERVICE_HEADER_BYTES);
        memset(body, 0, sizeof(*body));
        body->handle = console->master;
        (void)client_call(&console->termd, TERMD_OP_HANDLE_CLOSE, sizeof(*body), -1, NULL);
    }
    if (console->master_notify_fd >= 16) (void)pacha_fd_close(console->master_notify_fd);
    if (console->vt != NULL) vterm_free(console->vt);
    (void)scroll_clear(console);
    if (console->termd.page != NULL) (void)pacha_munmap(console->termd.page, PACHA_SERVICE_PAGE_BYTES);
    if (console->inputd.page != NULL) (void)pacha_munmap(console->inputd.page, PACHA_SERVICE_PAGE_BYTES);
    if (console->termd.page_fd >= 16) (void)pacha_fd_close(console->termd.page_fd);
    if (console->inputd.page_fd >= 16) (void)pacha_fd_close(console->inputd.page_fd);
    if (console->mmio_fd >= 16) (void)pacha_fd_close(console->mmio_fd);
    if (console->framebuffer_reservation != NULL)
        (void)pacha_munmap(console->framebuffer_reservation,
            console->framebuffer_reservation_size);
    free(console);
}
