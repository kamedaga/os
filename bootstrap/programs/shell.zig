/// Bootstrap shell displayed on the primary framebuffer until the windowed UI takes over.
const std = @import("std");
const block_client = @import("support_root").block_client;
const fs_abi = @import("support_root").fs_abi;
const fs_client = @import("support_root").fs_client;
const fs_protocol = @import("support_root").fs_protocol;
const input_bootstrap = @import("support_root").input_driver_bootstrap_abi;
const init_bootstrap_abi = @import("support_root").init_bootstrap_abi;
const process_abi = @import("support_root").process_abi;
const service_registry_abi = @import("support_root").service_registry_abi;

const syscall_log: u64 = 0x9;
const syscall_map_mmio: u64 = 0xB;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_queue_submit: u64 = 0xE;
const syscall_queue_notify: u64 = 0xF;
const syscall_map_pages_batch: u64 = 0x15;
const syscall_wait_event: u64 = 0x17;
const syscall_get_process_slot: u64 = 0x2E;
const syscall_map_vm_object: u64 = 0x28;
const config_framebuffer_paddr_index: usize = @intCast(init_bootstrap_abi.boot_display_config_fb_paddr_index);
const config_framebuffer_size_bytes_index: usize = @intCast(init_bootstrap_abi.boot_display_config_fb_size_bytes_index);
const config_framebuffer_vm_token_index: usize = @intCast(init_bootstrap_abi.boot_display_config_fb_vm_token_index);
const syscall_batch_max_pages: usize = 64;

const syscall_ok: u64 = 0;
const framebuffer_va: usize = 0x3C00_5000;
const config_page_va: usize = @intCast(process_abi.standard_config_target_va);
const runtime_page_base_va: usize = 0x2100_0000;
const common_page_va: usize = runtime_page_base_va + 0x4000;
const notify_page_va: usize = runtime_page_base_va + 0x5000;
const isr_page_va: usize = runtime_page_base_va + 0x6000;
const queue_page0_va: usize = runtime_page_base_va + 0x8000;
const queue_page1_va: usize = runtime_page_base_va + 0x9000;

const fb_width: usize = 832;
const fb_height: usize = @intCast(init_bootstrap_abi.boot_display_shell_height);
const fb_pitch: usize = 832;
const glyph_w: usize = 5;
const glyph_h: usize = 7;
const glyph_scale: usize = 2;
const cell_w: usize = glyph_w * glyph_scale + 6;
const cell_h: usize = glyph_h * glyph_scale + 6;
const cols: usize = fb_width / cell_w;
const rows: usize = fb_height / cell_h;

const bg_color: u32 = 0x000B_0E12;
const panel_color: u32 = 0x0011_1820;
const prompt_color: u32 = 0x0015_1E28;
const border_color: u32 = 0x0022_3444;
const fg_color: u32 = 0x00F4_F1E8;
const title_color: u32 = 0x0079_D7FF;
const prompt_fg_color: u32 = 0x00FF_D166;
const warn_color: u32 = 0x00FF_8A65;
const splash_bar_color: u32 = 0x0035_C2FF;
const splash_core_color: u32 = 0x00FF_F1BF;
const splash_sub_color: u32 = 0x0095_B7CC;

const common_device_feature_select: usize = 0x00;
const common_driver_feature_select: usize = 0x08;
const common_driver_feature: usize = 0x0C;
const common_device_status: usize = 0x14;
const common_queue_select: usize = 0x16;
const common_queue_size: usize = 0x18;
const common_queue_enable: usize = 0x1C;
const common_queue_notify_off: usize = 0x1E;
const common_queue_desc: usize = 0x20;
const common_queue_avail: usize = 0x28;
const common_queue_used: usize = 0x30;

const status_acknowledge: u8 = 0x01;
const status_driver: u8 = 0x02;
const status_driver_ok: u8 = 0x04;

const event_type_syn: u16 = 0x00;
const event_type_key: u16 = 0x01;
const syn_report: u16 = 0x00;
const key_left_shift: u16 = 0x2A;
const key_right_shift: u16 = 0x36;

const queue_index_event: u16 = 0;
const queue_size: u16 = 8;
const queue_used_offset: usize = 4096;
const queue_buffers_offset: usize = 4176;
const desc_flag_write: u16 = 1 << 1;
const shell_idle_poll_ticks: u64 = 4;
const service_registry_page_va: u64 = process_abi.service_registry_shadow_va;
const shell_stack_extension_pages: u64 = 8;
const shell_stack_extension_base_va: u64 = process_abi.aux_base_va - ((shell_stack_extension_pages + 1) * 4096);
const block_request_va: u64 = 0x3C10_4000;
const block_response_va: u64 = 0x3C10_5000;
const persistent_fs_request_va: u64 = 0x3C10_6000;
const persistent_fs_response_va: u64 = 0x3C10_7000;
const block_demo_magic: u64 = 0x424C_4B44_454D_4F31;
const shell_cat_display_limit_bytes: usize = 2048;
const pie_user_rootfs_name = "pie_user.elf";
var fs_demo_scratch: [1536]u8 = [_]u8{0} ** 1536;

const VirtqDesc = extern struct {
    addr: u64,
    len: u32,
    flags: u16,
    next: u16,
};

const VirtqUsedElem = extern struct {
    id: u32,
    len: u32,
};

const VirtioInputEvent = extern struct {
    event_type: u16,
    code: u16,
    value: u32,
};

const RenderAction = enum(u2) {
    none = 0,
    prompt = 1,
    full = 2,

    fn merge(current: RenderAction, next: RenderAction) RenderAction {
        return if (@intFromEnum(next) > @intFromEnum(current)) next else current;
    }
};

const KeyboardState = struct {
    notify_addr: usize,
    isr_base: usize,
    last_used_idx: u16 = 0,
    pending_code: u16 = 0,
    pending_value: u32 = 0,
    has_pending_key: bool = false,
    pending_ascii: u8 = 0,
    has_pending_ascii: bool = false,
    shift_down: bool = false,
};

const ShellState = struct {
    lines: [rows - 2][cols]u8 = [_][cols]u8{[_]u8{' '} ** cols} ** (rows - 2),
    line_len: [rows - 2]usize = [_]usize{0} ** (rows - 2),
    cur_row: usize = 0,
    splash_line_count: usize = 0,
    rendered_rows: usize = 0,
    frame_initialized: bool = false,
    cmd: [128]u8 = undefined,
    cmd_len: usize = 0,
    keyboard_ready: bool = false,
    block_client_ready: bool = false,
    block_client_state: block_client.Client = undefined,
    persistent_fs_client_ready: bool = false,
    persistent_fs_client_state: fs_client.Client = undefined,
    cwd_path_buf: [fs_protocol.max_path_bytes]u8 = [_]u8{0} ** fs_protocol.max_path_bytes,
    cwd_path_len: usize = 0,

    fn clearBody(self: *ShellState) void {
        var r: usize = 0;
        while (r < self.lines.len) : (r += 1) {
            self.lines[r] = [_]u8{' '} ** cols;
            self.line_len[r] = 0;
        }
        self.cur_row = 0;
        self.splash_line_count = 0;
    }

    fn scroll(self: *ShellState) void {
        var r: usize = 1;
        while (r < self.lines.len) : (r += 1) {
            self.lines[r - 1] = self.lines[r];
            self.line_len[r - 1] = self.line_len[r];
        }
        self.lines[self.lines.len - 1] = [_]u8{' '} ** cols;
        self.line_len[self.lines.len - 1] = 0;
        self.cur_row = self.lines.len - 1;
    }

    fn writeLine(self: *ShellState, text: []const u8) void {
        if (self.cur_row >= self.lines.len) self.scroll();
        self.lines[self.cur_row] = [_]u8{' '} ** cols;
        const copy_len = if (text.len < cols) text.len else cols;
        if (copy_len > 0) @memcpy(self.lines[self.cur_row][0..copy_len], text[0..copy_len]);
        self.line_len[self.cur_row] = copy_len;
        self.cur_row += 1;
        if (self.cur_row >= self.lines.len) self.scroll();
    }

    fn writeSplashLine(self: *ShellState, text: []const u8) void {
        self.writeLine(text);
        if (self.splash_line_count < self.lines.len) {
            self.splash_line_count += 1;
        }
    }
};

var block_storage: [4096]u8 align(16) = [_]u8{0} ** 4096;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn getProcessSlot() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_get_process_slot),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn waitEvent(wait_mailbox: bool, timeout_ticks: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_wait_event),
          [arg0] "{rdi}" (@as(u64, if (wait_mailbox) 1 else 0)),
          [arg1] "{rsi}" (timeout_ticks),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn spawnExec(exec_token: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (process_abi.syscall_spawn_exec),
          [arg0] "{rdi}" (exec_token),
          [arg1] "{rsi}" (@as(u64, 0)),
          [arg2] "{rdx}" (@as(u64, 0)),
          [arg3] "{rcx}" (@as(u64, 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mapPagesBatch(base_va: u64, paddr_list_va: u64, page_count: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_pages_batch),
          [arg0] "{rdi}" (base_va),
          [arg1] "{rsi}" (paddr_list_va),
          [arg2] "{rdx}" (page_count),
          [arg3] "{rcx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mapVmObject(token: u64, target_va: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_vm_object),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (target_va),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mapMmioPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_mmio),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn allocMapPages(base_va: u64, page_count: u64, writable: bool, out_paddr_list_va: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_alloc_map_pages),
          [arg0] "{rdi}" (base_va),
          [arg1] "{rsi}" (page_count),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
          [arg3] "{rcx}" (out_paddr_list_va),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn queueSubmit(token: u64, queue_index: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_queue_submit),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (queue_index),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn queueNotify(token: u64, queue_index: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_queue_notify),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (queue_index),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mmioReadU8(addr: usize) u8 {
    const ptr: *volatile u8 = @ptrFromInt(addr);
    return ptr.*;
}

fn mmioReadU16(addr: usize) u16 {
    const ptr: *volatile u16 = @ptrFromInt(addr);
    return ptr.*;
}

fn mmioWriteU8(addr: usize, value: u8) void {
    const ptr: *volatile u8 = @ptrFromInt(addr);
    ptr.* = value;
}

fn mmioWriteU16(addr: usize, value: u16) void {
    const ptr: *volatile u16 = @ptrFromInt(addr);
    ptr.* = value;
}

fn mmioWriteU32(addr: usize, value: u32) void {
    const ptr: *volatile u32 = @ptrFromInt(addr);
    ptr.* = value;
}

fn mmioWriteU64(addr: usize, value: u64) void {
    const ptr: *volatile u64 = @ptrFromInt(addr);
    ptr.* = value;
}

fn readCfgU64(index: usize) u64 {
    const cfg: [*]const volatile u64 = @ptrFromInt(config_page_va);
    return cfg[index];
}

fn writeCfgU64(index: usize, value: u64) void {
    const cfg: [*]volatile u64 = @ptrFromInt(config_page_va);
    cfg[index] = value;
}

fn queueRegionPhys(queue_paddr0: u64, queue_paddr1: u64, offset: usize) u64 {
    if (offset < 4096) return queue_paddr0 + @as(u64, @intCast(offset));
    return queue_paddr1 + @as(u64, @intCast(offset - 4096));
}

fn queueDescPtr(index: u16) *volatile VirtqDesc {
    return @ptrFromInt(queue_page0_va + @as(usize, index) * @sizeOf(VirtqDesc));
}

fn queueAvailIdxPtr() *volatile u16 {
    return @ptrFromInt(queue_page0_va + @as(usize, queue_size) * @sizeOf(VirtqDesc) + 2);
}

fn queueAvailRingPtr() [*]volatile u16 {
    return @ptrFromInt(queue_page0_va + @as(usize, queue_size) * @sizeOf(VirtqDesc) + 4);
}

fn queueUsedIdxPtr() *volatile u16 {
    return @ptrFromInt(queue_page0_va + queue_used_offset + 2);
}

fn queueUsedRingPtr() [*]volatile VirtqUsedElem {
    return @ptrFromInt(queue_page0_va + queue_used_offset + 4);
}

fn queueEventPtr(desc_id: u16) *volatile VirtioInputEvent {
    return @ptrFromInt(queue_page0_va + queue_buffers_offset + @as(usize, desc_id) * @sizeOf(VirtioInputEvent));
}

fn queuePushAvail(desc_id: u16) void {
    const avail_idx_ptr = queueAvailIdxPtr();
    const avail_idx = avail_idx_ptr.*;
    const slot: usize = @intCast(avail_idx % queue_size);
    queueAvailRingPtr()[slot] = desc_id;
    avail_idx_ptr.* = avail_idx +% 1;
}

fn appendText(buf: []u8, idx: *usize, text: []const u8) void {
    if (idx.* >= buf.len) return;
    const remaining = buf.len - idx.*;
    const copy_len = if (text.len < remaining) text.len else remaining;
    @memcpy(buf[idx.* .. idx.* + copy_len], text[0..copy_len]);
    idx.* += copy_len;
}

fn fillRect(vfb: [*]volatile u32, x: usize, y: usize, w: usize, h: usize, color: u32) void {
    if (w == 0 or h == 0) return;
    var yy: usize = y;
    while (yy < y + h and yy < fb_height) : (yy += 1) {
        const row = yy * fb_pitch;
        var xx: usize = x;
        while (xx < x + w and xx < fb_width) : (xx += 1) {
            vfb[row + xx] = color;
        }
    }
}

fn glyphRows(raw: u8) [glyph_h]u8 {
    const ch: u8 = if (raw >= 'a' and raw <= 'z') raw - 32 else raw;
    return switch (ch) {
        'A' => .{ 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
        'B' => .{ 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
        'C' => .{ 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
        'D' => .{ 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },
        'E' => .{ 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },
        'F' => .{ 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
        'G' => .{ 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E },
        'H' => .{ 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
        'I' => .{ 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F },
        'J' => .{ 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E },
        'K' => .{ 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
        'L' => .{ 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
        'M' => .{ 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },
        'N' => .{ 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },
        'O' => .{ 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
        'P' => .{ 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
        'Q' => .{ 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },
        'R' => .{ 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
        'S' => .{ 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },
        'T' => .{ 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
        'U' => .{ 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
        'V' => .{ 0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04 },
        'W' => .{ 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A },
        'X' => .{ 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
        'Y' => .{ 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 },
        'Z' => .{ 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
        '0' => .{ 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },
        '1' => .{ 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
        '2' => .{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
        '3' => .{ 0x1E, 0x01, 0x01, 0x06, 0x01, 0x01, 0x1E },
        '4' => .{ 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
        '5' => .{ 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E },
        '6' => .{ 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },
        '7' => .{ 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
        '8' => .{ 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
        '9' => .{ 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x1C },
        ' ' => .{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        '.' => .{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 },
        ',' => .{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x08 },
        ':' => .{ 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00 },
        ';' => .{ 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x08 },
        '-' => .{ 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00 },
        '_' => .{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F },
        '/' => .{ 0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00 },
        '\\' => .{ 0x10, 0x08, 0x04, 0x02, 0x01, 0x00, 0x00 },
        '[' => .{ 0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E },
        ']' => .{ 0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E },
        '(' => .{ 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 },
        ')' => .{ 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 },
        '+' => .{ 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 },
        '=' => .{ 0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00 },
        '*' => .{ 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00 },
        '#' => .{ 0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A },
        '!' => .{ 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 },
        '?' => .{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 },
        '|' => .{ 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
        '<' => .{ 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 },
        '>' => .{ 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 },
        '\'' => .{ 0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00 },
        '"' => .{ 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00 },
        '`' => .{ 0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00 },
        '~' => .{ 0x00, 0x09, 0x16, 0x00, 0x00, 0x00, 0x00 },
        else => .{ 0x0E, 0x11, 0x01, 0x06, 0x04, 0x00, 0x04 },
    };
}

fn drawGlyph(vfb: [*]volatile u32, x: usize, y: usize, ch: u8, color: u32) void {
    const glyph = glyphRows(ch);
    var gy: usize = 0;
    while (gy < glyph_h) : (gy += 1) {
        const bits = glyph[gy];
        var gx: usize = 0;
        while (gx < glyph_w) : (gx += 1) {
            if ((bits & (@as(u8, 1) << @intCast((glyph_w - 1) - gx))) == 0) continue;
            const px = x + gx * glyph_scale;
            const py = y + gy * glyph_scale;
            var sy: usize = 0;
            while (sy < glyph_scale and py + sy < fb_height) : (sy += 1) {
                const row = (py + sy) * fb_pitch;
                var sx: usize = 0;
                while (sx < glyph_scale and px + sx < fb_width) : (sx += 1) {
                    vfb[row + px + sx] = color;
                }
            }
        }
    }
}

fn drawLine(vfb: [*]volatile u32, row: usize, text: []const u8, color: u32) void {
    var i: usize = 0;
    while (i < text.len and i < cols) : (i += 1) {
        drawGlyph(vfb, i * cell_w + 4, row * cell_h + 4, text[i], color);
    }
}

fn clearRow(vfb: [*]volatile u32, row: usize, color: u32) void {
    fillRect(vfb, 2, row * cell_h, fb_width - 4, cell_h, color);
}

fn asciiLower(ch: u8) u8 {
    if (ch >= 'A' and ch <= 'Z') return ch + 32;
    return ch;
}

fn trimSpaces(text: []const u8) []const u8 {
    var start: usize = 0;
    var end: usize = text.len;
    while (start < end and (text[start] == ' ' or text[start] == '\t')) : (start += 1) {}
    while (end > start and (text[end - 1] == ' ' or text[end - 1] == '\t')) : (end -= 1) {}
    return text[start..end];
}

fn eqAsciiNoCase(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    var i: usize = 0;
    while (i < a.len) : (i += 1) {
        if (asciiLower(a[i]) != asciiLower(b[i])) return false;
    }
    return true;
}

const CommandSplit = struct {
    head: []const u8,
    tail: []const u8,
};

fn splitCommand(text: []const u8) CommandSplit {
    const trimmed = trimSpaces(text);
    if (trimmed.len == 0) {
        return .{ .head = "", .tail = "" };
    }
    var end: usize = 0;
    while (end < trimmed.len and trimmed[end] != ' ' and trimmed[end] != '\t') : (end += 1) {}
    return .{
        .head = trimmed[0..end],
        .tail = trimSpaces(trimmed[end..]),
    };
}

fn writeWrappedText(st: *ShellState, text: []const u8) void {
    if (text.len == 0) {
        st.writeLine("");
        return;
    }
    var remaining = text;
    while (remaining.len != 0) {
        const newline_index = std.mem.indexOfScalar(u8, remaining, '\n') orelse remaining.len;
        var line = remaining[0..newline_index];
        if (line.len == 0) {
            st.writeLine("");
        } else {
            while (line.len > cols) {
                st.writeLine(line[0..cols]);
                line = line[cols..];
            }
            if (line.len != 0) st.writeLine(line);
        }
        if (newline_index == remaining.len) break;
        remaining = remaining[newline_index + 1 ..];
        if (remaining.len == 0) st.writeLine("");
    }
}

fn rootfsUnavailableReason() []const u8 {
    if (service_registry_abi.findService(service_registry_page_va, .persistent_fs) == null) {
        return "rootfs unavailable: persistent fs not ready";
    }
    return "rootfs unavailable";
}

fn currentRootfsPath(st: *const ShellState) []const u8 {
    return st.cwd_path_buf[0..st.cwd_path_len];
}

fn resetRootfsPath(st: *ShellState) void {
    @memset(st.cwd_path_buf[0..], 0);
    st.cwd_path_buf[0] = '/';
    st.cwd_path_len = 1;
}

fn setRootfsPath(st: *ShellState, path: []const u8) void {
    @memset(st.cwd_path_buf[0..], 0);
    if (path.len != 0) @memcpy(st.cwd_path_buf[0..path.len], path);
    st.cwd_path_len = path.len;
}

fn copyCurrentRootfsPath(st: *const ShellState, out: *[fs_protocol.max_path_bytes]u8) []const u8 {
    @memset(out[0..], 0);
    if (st.cwd_path_len != 0) @memcpy(out[0..st.cwd_path_len], st.cwd_path_buf[0..st.cwd_path_len]);
    return out[0..st.cwd_path_len];
}

fn popNormalizedRootfsPath(path: []u8, len: *usize) void {
    if (len.* <= 1) {
        path[0] = '/';
        len.* = 1;
        return;
    }
    var idx = len.* - 1;
    while (idx > 0 and path[idx] != '/') : (idx -= 1) {}
    len.* = if (idx == 0) 1 else idx;
}

fn normalizeRootfsPath(st: *const ShellState, raw: []const u8, out: *[fs_protocol.max_path_bytes]u8) ?[]const u8 {
    const trimmed = trimSpaces(raw);
    if (trimmed.len == 0) return null;

    @memset(out[0..], 0);
    var out_len: usize = 0;
    if (trimmed[0] == '/') {
        out[0] = '/';
        out_len = 1;
    } else {
        if (st.cwd_path_len == 0) return null;
        @memcpy(out[0..st.cwd_path_len], st.cwd_path_buf[0..st.cwd_path_len]);
        out_len = st.cwd_path_len;
    }

    var pos: usize = 0;
    while (pos < trimmed.len) {
        while (pos < trimmed.len and trimmed[pos] == '/') : (pos += 1) {}
        if (pos >= trimmed.len) break;

        const start = pos;
        while (pos < trimmed.len and trimmed[pos] != '/') : (pos += 1) {}
        const component = trimmed[start..pos];
        if (component.len == 0 or std.mem.eql(u8, component, ".")) continue;
        if (std.mem.eql(u8, component, "..")) {
            popNormalizedRootfsPath(out[0..], &out_len);
            continue;
        }
        if (out_len > 1) {
            if (out_len >= out.len) return null;
            out[out_len] = '/';
            out_len += 1;
        }
        if (out_len + component.len > out.len) return null;
        @memcpy(out[out_len .. out_len + component.len], component);
        out_len += component.len;
    }

    if (out_len == 0) {
        out[0] = '/';
        out_len = 1;
    }
    return out[0..out_len];
}

fn requireRootfsPath(st: *ShellState, raw: []const u8, out: *[fs_protocol.max_path_bytes]u8) ?[]const u8 {
    if (trimSpaces(raw).len == 0) {
        st.writeLine("path required");
        shellLogLine("path required");
        return null;
    }
    return normalizeRootfsPath(st, raw, out) orelse {
        st.writeLine("path too long");
        shellLogLine("path too long");
        return null;
    };
}

fn resolveRootfsPathOrCwd(st: *ShellState, raw: []const u8, out: *[fs_protocol.max_path_bytes]u8) ?[]const u8 {
    if (trimSpaces(raw).len == 0) return copyCurrentRootfsPath(st, out);
    return normalizeRootfsPath(st, raw, out) orelse {
        st.writeLine("path too long");
        shellLogLine("path too long");
        return null;
    };
}

fn keycodeToAscii(code: u16, shift: bool) ?u8 {
    return switch (code) {
        0x02 => if (shift) '!' else '1',
        0x03 => if (shift) '@' else '2',
        0x04 => if (shift) '#' else '3',
        0x05 => if (shift) '$' else '4',
        0x06 => if (shift) '%' else '5',
        0x07 => if (shift) '^' else '6',
        0x08 => if (shift) '&' else '7',
        0x09 => if (shift) '*' else '8',
        0x0A => if (shift) '(' else '9',
        0x0B => if (shift) ')' else '0',
        0x0C => if (shift) '_' else '-',
        0x0D => if (shift) '+' else '=',
        0x0E => '\x08',
        0x0F => '\t',
        0x10 => if (shift) 'Q' else 'q',
        0x11 => if (shift) 'W' else 'w',
        0x12 => if (shift) 'E' else 'e',
        0x13 => if (shift) 'R' else 'r',
        0x14 => if (shift) 'T' else 't',
        0x15 => if (shift) 'Y' else 'y',
        0x16 => if (shift) 'U' else 'u',
        0x17 => if (shift) 'I' else 'i',
        0x18 => if (shift) 'O' else 'o',
        0x19 => if (shift) 'P' else 'p',
        0x1A => if (shift) '{' else '[',
        0x1B => if (shift) '}' else ']',
        0x1C => '\n',
        0x1E => if (shift) 'A' else 'a',
        0x1F => if (shift) 'S' else 's',
        0x20 => if (shift) 'D' else 'd',
        0x21 => if (shift) 'F' else 'f',
        0x22 => if (shift) 'G' else 'g',
        0x23 => if (shift) 'H' else 'h',
        0x24 => if (shift) 'J' else 'j',
        0x25 => if (shift) 'K' else 'k',
        0x26 => if (shift) 'L' else 'l',
        0x27 => if (shift) ':' else ';',
        0x28 => if (shift) '"' else '\'',
        0x29 => if (shift) '~' else '`',
        0x2B => if (shift) '|' else '\\',
        0x2C => if (shift) 'Z' else 'z',
        0x2D => if (shift) 'X' else 'x',
        0x2E => if (shift) 'C' else 'c',
        0x2F => if (shift) 'V' else 'v',
        0x30 => if (shift) 'B' else 'b',
        0x31 => if (shift) 'N' else 'n',
        0x32 => if (shift) 'M' else 'm',
        0x33 => if (shift) '<' else ',',
        0x34 => if (shift) '>' else '.',
        0x35 => if (shift) '?' else '/',
        0x39 => ' ',
        0x53 => '\x7F',
        0x6F => '\x7F',
        else => null,
    };
}

fn renderHeader(vfb: [*]volatile u32, st: *const ShellState) void {
    clearRow(vfb, 0, panel_color);
    fillRect(vfb, 0, 0, fb_width, 2, border_color);
    fillRect(vfb, 0, cell_h, fb_width, 1, border_color);
    var header_buf: [cols]u8 = [_]u8{' '} ** cols;
    var idx: usize = 0;
    appendText(header_buf[0..], &idx, "shell ");
    appendText(header_buf[0..], &idx, currentRootfsPath(st));
    appendText(
        header_buf[0..],
        &idx,
        if (st.keyboard_ready) "  help ls cd mkdir" else "  keyboard offline",
    );
    drawLine(vfb, 0, header_buf[0..idx], if (st.keyboard_ready) title_color else warn_color);
}

fn renderBody(vfb: [*]volatile u32, st: *ShellState) void {
    const rows_to_clear = if (st.rendered_rows == 0)
        st.lines.len
    else if (st.rendered_rows > st.cur_row)
        st.rendered_rows
    else
        st.cur_row;
    var row: usize = 0;
    while (row < rows_to_clear and row < st.lines.len) : (row += 1) {
        clearRow(vfb, row + 1, bg_color);
        if (row >= st.cur_row) continue;
        const color = if (row < st.splash_line_count)
            switch (row) {
                0, 4 => splash_bar_color,
                1 => splash_sub_color,
                2 => splash_core_color,
                3 => title_color,
                else => fg_color,
            }
        else
            fg_color;
        drawLine(vfb, row + 1, st.lines[row][0..st.line_len[row]], color);
    }
    st.rendered_rows = st.cur_row;
}

fn renderPrompt(vfb: [*]volatile u32, st: *const ShellState) void {
    clearRow(vfb, rows - 1, prompt_color);
    fillRect(vfb, 0, (rows - 1) * cell_h, fb_width, 1, border_color);
    fillRect(vfb, 0, fb_height - 2, fb_width, 2, border_color);
    var prompt_buf: [cols]u8 = [_]u8{' '} ** cols;
    prompt_buf[0] = '>';
    prompt_buf[1] = ' ';
    const cmd_slice = if (st.cmd_len + 2 <= cols)
        st.cmd[0..st.cmd_len]
    else
        st.cmd[st.cmd_len - (cols - 2) .. st.cmd_len];
    if (cmd_slice.len > 0) @memcpy(prompt_buf[2 .. 2 + cmd_slice.len], cmd_slice);
    drawLine(vfb, rows - 1, prompt_buf[0..], prompt_fg_color);
}

fn renderFull(vfb: [*]volatile u32, st: *ShellState) void {
    if (!st.frame_initialized) {
        fillRect(vfb, 0, 0, fb_width, fb_height, bg_color);
        fillRect(vfb, 0, 0, fb_width, 2, border_color);
        fillRect(vfb, 0, fb_height - 2, fb_width, 2, border_color);
        fillRect(vfb, 0, 0, 2, fb_height, border_color);
        fillRect(vfb, fb_width - 2, 0, 2, fb_height, border_color);
        st.frame_initialized = true;
    }
    renderHeader(vfb, st);
    renderBody(vfb, st);
    renderPrompt(vfb, st);
}

fn shellLogLine(text: []const u8) void {
    var buf: [160]u8 = undefined;
    var idx: usize = 0;
    appendText(buf[0..], &idx, "Shell: ");
    appendText(buf[0..], &idx, text);
    appendText(buf[0..], &idx, "\n");
    _ = userLog(buf[0..idx]);
}

fn seedBootSplash(st: *ShellState) void {
    st.writeSplashLine("ELF loader PIE+RELATIVE ready");
    st.writeSplashLine("========================================");
    st.writeSplashLine("  MicroKernel");
    st.writeSplashLine("  enter bare-metal capability kernel");
    st.writeSplashLine("========================================");
    st.writeLine("shell fast path online");
}

fn blockLaunchUnavailableReason() []const u8 {
    if (service_registry_abi.findService(service_registry_page_va, .block) == null) {
        return "block demo unavailable: block service not ready";
    }
    return "block demo unavailable";
}

fn persistentFsUnavailableReason() []const u8 {
    if (service_registry_abi.findService(service_registry_page_va, .persistent_fs) == null) {
        return "fs demo unavailable: persistent fs not ready";
    }
    return "fs demo unavailable";
}

fn ensureBlockClient(st: *ShellState) ?*block_client.Client {
    if (st.block_client_ready) return &st.block_client_state;
    const process_slot = getProcessSlot();
    if (process_slot == 0) {
        st.writeLine("block process slot unavailable");
        shellLogLine("block process slot unavailable");
        return null;
    }
    st.block_client_state = block_client.Client.connectFromRegistryPageOptions(
        service_registry_page_va,
        block_request_va,
        block_response_va,
        process_slot,
        .{
            .response_poll_limit = 65536,
        },
    ) catch |err| {
        st.writeLine("block connect failed");
        shellLogLine("block connect failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        return null;
    };
    st.block_client_ready = true;
    return &st.block_client_state;
}

fn ensurePersistentFsClient(st: *ShellState) ?*fs_client.Client {
    if (st.persistent_fs_client_ready) return &st.persistent_fs_client_state;
    const process_slot = getProcessSlot();
    if (process_slot == 0) {
        st.writeLine("fs process slot unavailable");
        shellLogLine("fs process slot unavailable");
        return null;
    }
    st.persistent_fs_client_state = fs_client.Client.connectFromRegistryPage(
        service_registry_page_va,
        persistent_fs_request_va,
        persistent_fs_response_va,
        process_slot,
    ) catch |err| {
        st.writeLine("fs connect failed");
        shellLogLine("fs connect failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        return null;
    };
    st.persistent_fs_client_ready = true;
    return &st.persistent_fs_client_state;
}

fn reportPersistentFsError(st: *ShellState, message: []const u8, err: anyerror, reset_client: bool) void {
    st.writeLine(message);
    shellLogLine(message);
    _ = userLog(@errorName(err));
    _ = userLog("\n");
    if (reset_client) st.persistent_fs_client_ready = false;
}

fn lookupPersistentFsRoot(st: *ShellState, client: *fs_client.Client) ?fs_client.LookupResult {
    const root = client.lookup(client.mount_token, ".") catch |err| {
        reportPersistentFsError(st, "fs root lookup failed", err, true);
        return null;
    };
    if (root.object_kind != .vnode_dir or !fs_abi.isCapToken(root.token)) {
        st.writeLine("fs root invalid");
        shellLogLine("fs root invalid");
        st.persistent_fs_client_ready = false;
        return null;
    }
    return root;
}

fn runPersistentFsList(st: *ShellState, path: []const u8) bool {
    if (service_registry_abi.findService(service_registry_page_va, .persistent_fs) == null) {
        st.writeLine(rootfsUnavailableReason());
        shellLogLine(rootfsUnavailableReason());
        return false;
    }
    const client = ensurePersistentFsClient(st) orelse return false;
    const root = lookupPersistentFsRoot(st, client) orelse return false;
    const target = client.lookup(root.token, path) catch |err| {
        reportPersistentFsError(st, "ls lookup failed", err, false);
        return false;
    };
    defer client.close(target.token) catch {};
    if (target.object_kind != .vnode_dir) {
        st.writeLine(path);
        return true;
    }
    var cursor: u64 = 0;
    var name_buf: [48]u8 = undefined;
    var count: usize = 0;
    while (true) {
        const entry = client.readdirOne(target.token, cursor, name_buf[0..]) catch |err| {
            reportPersistentFsError(st, "ls failed", err, true);
            return false;
        };
        switch (entry) {
            .end => break,
            .entry => |dirent| {
                if (dirent.object_kind == .vnode_dir) {
                    var line_buf: [64]u8 = undefined;
                    const line = std.fmt.bufPrint(&line_buf, "{s}/", .{dirent.name}) catch {
                        st.writeLine(dirent.name);
                        cursor = dirent.next_cursor;
                        count += 1;
                        continue;
                    };
                    st.writeLine(line);
                } else {
                    st.writeLine(dirent.name);
                }
                cursor = dirent.next_cursor;
                count += 1;
            },
        }
    }
    if (count == 0) st.writeLine("directory empty");
    return true;
}

fn runPersistentFsStat(st: *ShellState, path: []const u8) bool {
    const client = ensurePersistentFsClient(st) orelse return false;
    const root = lookupPersistentFsRoot(st, client) orelse return false;
    const file = client.lookup(root.token, path) catch |err| {
        reportPersistentFsError(st, "stat lookup failed", err, false);
        return false;
    };
    defer client.close(file.token) catch {};
    const stat = client.stat(file.token) catch |err| {
        reportPersistentFsError(st, "stat failed", err, true);
        return false;
    };
    var line_buf: [160]u8 = undefined;
    const line = std.fmt.bufPrint(
        &line_buf,
        "{s} type={s} size={d}",
        .{
            path,
            switch (stat.object_kind) {
                .mount => "mount",
                .vnode_dir => "dir",
                .vnode_file => "file",
                .open_file => "open",
                .exec => "exec",
                else => "unknown",
            },
            stat.size_bytes,
        },
    ) catch return false;
    st.writeLine(line);
    return true;
}

fn runPersistentFsCat(st: *ShellState, path: []const u8) bool {
    const client = ensurePersistentFsClient(st) orelse return false;
    const root = lookupPersistentFsRoot(st, client) orelse return false;
    const file = client.lookup(root.token, path) catch |err| {
        reportPersistentFsError(st, "cat lookup failed", err, false);
        return false;
    };
    defer client.close(file.token) catch {};
    const open_file = client.open(file.token) catch |err| {
        reportPersistentFsError(st, "cat open failed", err, true);
        return false;
    };
    defer client.close(open_file.token) catch {};

    if (file.file_bytes == 0) {
        st.writeLine("empty file");
        return true;
    }

    var read_buf: [256]u8 = undefined;
    var print_buf: [256]u8 = undefined;
    var offset: u64 = 0;
    var displayed: usize = 0;
    while (displayed < shell_cat_display_limit_bytes) {
        const max_chunk = @min(read_buf.len, shell_cat_display_limit_bytes - displayed);
        const read_result = client.read(open_file.token, offset, read_buf[0..max_chunk]) catch |err| {
            reportPersistentFsError(st, "cat read failed", err, true);
            return false;
        };
        if (read_result.bytes_read == 0) break;
        var out_len: usize = 0;
        for (read_buf[0..read_result.bytes_read]) |byte| {
            if (byte == '\r') continue;
            print_buf[out_len] = switch (byte) {
                '\n' => '\n',
                '\t' => ' ',
                0x20...0x7E => byte,
                else => '.',
            };
            out_len += 1;
        }
        if (out_len != 0) writeWrappedText(st, print_buf[0..out_len]);
        displayed += read_result.bytes_read;
        offset = read_result.next_offset;
        if (offset >= read_result.file_bytes) break;
    }
    if (file.file_bytes > shell_cat_display_limit_bytes) st.writeLine("cat truncated");
    return true;
}

fn runPersistentFsTouch(st: *ShellState, path: []const u8) bool {
    const client = ensurePersistentFsClient(st) orelse return false;
    const root = lookupPersistentFsRoot(st, client) orelse return false;
    const existing = client.lookup(root.token, path) catch |err| switch (err) {
        error.NotFound => {
            const file = client.create(root.token, path) catch |create_err| {
                reportPersistentFsError(st, "touch create failed", create_err, true);
                return false;
            };
            defer client.close(file.token) catch {};
            var line_buf: [80]u8 = undefined;
            const line = std.fmt.bufPrint(&line_buf, "created {s}", .{path}) catch return false;
            st.writeLine(line);
            return true;
        },
        else => {
            reportPersistentFsError(st, "touch lookup failed", err, false);
            return false;
        },
    };
    defer client.close(existing.token) catch {};
    if (existing.object_kind == .vnode_dir) {
        st.writeLine("touch target is a directory");
        shellLogLine("touch target is a directory");
        return false;
    }
    var line_buf: [80]u8 = undefined;
    const line = std.fmt.bufPrint(&line_buf, "{s} already exists", .{path}) catch return false;
    st.writeLine(line);
    return true;
}

fn runPersistentFsWrite(st: *ShellState, path: []const u8, text: []const u8) bool {
    const client = ensurePersistentFsClient(st) orelse return false;
    const root = lookupPersistentFsRoot(st, client) orelse return false;
    if (!removePersistentFsFileIfPresent(st, client, root.token, path)) return false;
    const file = client.create(root.token, path) catch |err| {
        reportPersistentFsError(st, "write create failed", err, true);
        return false;
    };
    defer client.close(file.token) catch {};
    const open_file = client.open(file.token) catch |err| {
        reportPersistentFsError(st, "write open failed", err, true);
        return false;
    };
    defer client.close(open_file.token) catch {};

    if (text.len != 0) {
        _ = client.write(open_file.token, 0, text) catch |err| {
            reportPersistentFsError(st, "write failed", err, true);
            return false;
        };
    }
    var line_buf: [160]u8 = undefined;
    const line = std.fmt.bufPrint(&line_buf, "wrote {d} bytes to {s}", .{ text.len, path }) catch return false;
    st.writeLine(line);
    return true;
}

fn runPersistentFsRename(st: *ShellState, old_path: []const u8, new_path: []const u8) bool {
    const client = ensurePersistentFsClient(st) orelse return false;
    const root = lookupPersistentFsRoot(st, client) orelse return false;
    client.rename(root.token, old_path, new_path) catch |err| {
        reportPersistentFsError(st, "rename failed", err, true);
        return false;
    };
    var line_buf: [160]u8 = undefined;
    const line = std.fmt.bufPrint(&line_buf, "renamed {s} to {s}", .{ old_path, new_path }) catch return false;
    st.writeLine(line);
    return true;
}

fn runPersistentFsUnlink(st: *ShellState, path: []const u8) bool {
    const client = ensurePersistentFsClient(st) orelse return false;
    const root = lookupPersistentFsRoot(st, client) orelse return false;
    client.unlink(root.token, path) catch |err| {
        reportPersistentFsError(st, "rm failed", err, false);
        return false;
    };
    var line_buf: [160]u8 = undefined;
    const line = std.fmt.bufPrint(&line_buf, "removed {s}", .{path}) catch return false;
    st.writeLine(line);
    return true;
}

fn runPersistentFsExecFile(st: *ShellState, path: []const u8) bool {
    if (service_registry_abi.findService(service_registry_page_va, .persistent_fs) == null) {
        st.writeLine(rootfsUnavailableReason());
        shellLogLine(rootfsUnavailableReason());
        return false;
    }
    const client = ensurePersistentFsClient(st) orelse return false;
    const root = lookupPersistentFsRoot(st, client) orelse return false;
    const file = client.lookup(root.token, path) catch |err| {
        reportPersistentFsError(st, "exec lookup failed", err, false);
        return false;
    };
    defer client.close(file.token) catch {};
    const exec = client.openExec(file.token) catch |err| {
        reportPersistentFsError(st, "open_exec failed", err, true);
        return false;
    };
    const spawned = spawnExec(exec.token);
    const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
        st.writeLine("spawn failed");
        shellLogLine("spawn failed");
        return false;
    };
    var line_buf: [160]u8 = undefined;
    const line = std.fmt.bufPrint(&line_buf, "spawned {s} slot={d}", .{ path, child_slot }) catch return false;
    st.writeLine(line);
    return true;
}

fn removePersistentFsFileIfPresent(st: *ShellState, client: *fs_client.Client, root_token: u64, path: []const u8) bool {
    const file = client.lookup(root_token, path) catch |err| switch (err) {
        error.NotFound => return true,
        else => {
            st.writeLine("fs cleanup lookup failed");
            shellLogLine("fs cleanup lookup failed");
            _ = userLog(@errorName(err));
            _ = userLog("\n");
            st.persistent_fs_client_ready = false;
            return false;
        },
    };
    if (file.object_kind == .vnode_dir) {
        client.close(file.token) catch {};
        st.writeLine("write target is a directory");
        shellLogLine("write target is a directory");
        return false;
    }
    client.close(file.token) catch {};
    client.unlink(root_token, path) catch |err| {
        st.writeLine("fs cleanup unlink failed");
        shellLogLine("fs cleanup unlink failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    return true;
}

fn runPersistentFsMkdir(st: *ShellState, path: []const u8) bool {
    const client = ensurePersistentFsClient(st) orelse return false;
    const root = lookupPersistentFsRoot(st, client) orelse return false;
    const existing = client.lookup(root.token, path) catch |err| switch (err) {
        error.NotFound => {
            const dir = client.createDir(root.token, path) catch |create_err| {
                reportPersistentFsError(st, "mkdir failed", create_err, true);
                return false;
            };
            defer client.close(dir.token) catch {};
            var line_buf: [160]u8 = undefined;
            const line = std.fmt.bufPrint(&line_buf, "created dir {s}", .{path}) catch return false;
            st.writeLine(line);
            return true;
        },
        else => {
            reportPersistentFsError(st, "mkdir lookup failed", err, false);
            return false;
        },
    };
    defer client.close(existing.token) catch {};
    if (existing.object_kind != .vnode_dir) {
        st.writeLine("mkdir target already exists as file");
        shellLogLine("mkdir target already exists as file");
        return false;
    }
    var line_buf: [160]u8 = undefined;
    const line = std.fmt.bufPrint(&line_buf, "{s} already exists", .{path}) catch return false;
    st.writeLine(line);
    return true;
}

fn runPersistentFsCd(st: *ShellState, path: []const u8) bool {
    if (service_registry_abi.findService(service_registry_page_va, .persistent_fs) == null) {
        st.writeLine(rootfsUnavailableReason());
        shellLogLine(rootfsUnavailableReason());
        return false;
    }
    const client = ensurePersistentFsClient(st) orelse return false;
    const root = lookupPersistentFsRoot(st, client) orelse return false;
    const dir = client.lookup(root.token, path) catch |err| {
        reportPersistentFsError(st, "cd lookup failed", err, false);
        return false;
    };
    defer client.close(dir.token) catch {};
    if (dir.object_kind != .vnode_dir) {
        st.writeLine("cd target is not a directory");
        shellLogLine("cd target is not a directory");
        return false;
    }
    setRootfsPath(st, path);
    return true;
}

fn runPersistentFsPwd(st: *ShellState) bool {
    st.writeLine(currentRootfsPath(st));
    return true;
}

fn runBlockDemo(st: *ShellState) bool {
    if (service_registry_abi.findService(service_registry_page_va, .block) == null) {
        st.writeLine("block service unavailable");
        shellLogLine("block service unavailable");
        st.block_client_ready = false;
        return false;
    }
    shellLogLine("block demo start");
    const client = ensureBlockClient(st) orelse return false;
    if (client.block_size == 0 or client.block_size > block_storage.len or client.capacity_blocks <= 256) {
        st.writeLine("unsupported block geometry");
        shellLogLine("unsupported block geometry");
        st.block_client_ready = false;
        return false;
    }

    const target_block = client.capacity_blocks - 128;
    @memset(block_storage[0..], 0);
    const block_bytes = block_storage[0..@intCast(client.block_size)];
    _ = client.readBlocks(target_block, block_bytes) catch |err| {
        st.writeLine("block read failed");
        shellLogLine("block read failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.block_client_ready = false;
        return false;
    };
    const previous_magic = std.mem.readInt(u64, block_bytes[0..8], .little);
    const previous_generation = std.mem.readInt(u64, block_bytes[8..16], .little);
    const next_generation: u64 = if (previous_magic == block_demo_magic) previous_generation + 1 else 1;

    @memset(block_bytes, 0);
    std.mem.writeInt(u64, block_bytes[0..8], block_demo_magic, .little);
    std.mem.writeInt(u64, block_bytes[8..16], next_generation, .little);
    std.mem.writeInt(u64, block_bytes[16..24], target_block, .little);
    client.writeBlocks(target_block, block_bytes) catch |err| {
        st.writeLine("block write failed");
        shellLogLine("block write failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.block_client_ready = false;
        return false;
    };
    client.flush() catch |err| {
        st.writeLine("block flush failed");
        shellLogLine("block flush failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.block_client_ready = false;
        return false;
    };
    @memset(block_bytes, 0);
    _ = client.readBlocks(target_block, block_bytes) catch |err| {
        st.writeLine("block verify read failed");
        shellLogLine("block verify read failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.block_client_ready = false;
        return false;
    };
    const verify_magic = std.mem.readInt(u64, block_bytes[0..8], .little);
    const verify_generation = std.mem.readInt(u64, block_bytes[8..16], .little);
    if (verify_magic != block_demo_magic or verify_generation != next_generation) {
        st.writeLine("block verify mismatch");
        shellLogLine("block verify mismatch");
        st.block_client_ready = false;
        return false;
    }
    var line_buf: [64]u8 = undefined;
    const line = std.fmt.bufPrint(&line_buf, "persisted generation={d}", .{next_generation}) catch return false;
    st.writeLine(line);
    shellLogLine("block demo done");
    return true;
}

fn runPersistentFsDemo(st: *ShellState) bool {
    const file_name = "counter.bin";
    const temp_name = "scratch.bin";
    const renamed_name = "scratch2.bin";
    if (service_registry_abi.findService(service_registry_page_va, .persistent_fs) == null) {
        st.writeLine("persistent fs unavailable");
        shellLogLine("persistent fs unavailable");
        st.persistent_fs_client_ready = false;
        return false;
    }
    shellLogLine("fs demo start");
    const client = ensurePersistentFsClient(st) orelse return false;

    const mount_stat = client.stat(client.mount_token) catch |err| {
        st.writeLine("fs mount stat failed");
        shellLogLine("fs mount stat failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    if (mount_stat.object_kind != .mount) {
        st.writeLine("fs mount kind mismatch");
        shellLogLine("fs mount kind mismatch");
        st.persistent_fs_client_ready = false;
        return false;
    }

    const root = client.lookup(client.mount_token, ".") catch |err| {
        st.writeLine("fs root lookup failed");
        shellLogLine("fs root lookup failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    if (root.object_kind != .vnode_dir or !fs_abi.isCapToken(root.token)) {
        st.writeLine("fs root invalid");
        shellLogLine("fs root invalid");
        st.persistent_fs_client_ready = false;
        return false;
    }

    const root_stat = client.stat(root.token) catch |err| {
        st.writeLine("fs root stat failed");
        shellLogLine("fs root stat failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    if (root_stat.object_kind != .vnode_dir) {
        st.writeLine("fs root stat mismatch");
        shellLogLine("fs root stat mismatch");
        st.persistent_fs_client_ready = false;
        return false;
    }

    const file = client.lookup(root.token, file_name) catch |err| switch (err) {
        error.NotFound => client.create(root.token, file_name) catch |create_err| {
            st.writeLine("fs create failed");
            shellLogLine("fs create failed");
            _ = userLog(@errorName(create_err));
            _ = userLog("\n");
            st.persistent_fs_client_ready = false;
            return false;
        },
        else => {
            st.writeLine("fs file lookup failed");
            shellLogLine("fs file lookup failed");
            _ = userLog(@errorName(err));
            _ = userLog("\n");
            st.persistent_fs_client_ready = false;
            return false;
        },
    };
    if (file.object_kind != .vnode_file or !fs_abi.isCapToken(file.token)) {
        st.writeLine("fs file invalid");
        shellLogLine("fs file invalid");
        st.persistent_fs_client_ready = false;
        return false;
    }

    const open_file = client.open(file.token) catch |err| {
        st.writeLine("fs open failed");
        shellLogLine("fs open failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };

    var read_buf: [16]u8 = [_]u8{0} ** 16;
    const initial_read = client.read(open_file.token, 0, read_buf[0..8]) catch |err| {
        st.writeLine("fs read failed");
        shellLogLine("fs read failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    const previous_generation: u64 = if (initial_read.bytes_read >= 8) std.mem.readInt(u64, read_buf[0..8], .little) else 0;
    const next_generation = previous_generation + 1;
    std.mem.writeInt(u64, read_buf[0..8], next_generation, .little);
    _ = client.write(open_file.token, 0, read_buf[0..8]) catch |err| {
        st.writeLine("fs write failed");
        shellLogLine("fs write failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    client.close(open_file.token) catch |err| {
        st.writeLine("fs close failed");
        shellLogLine("fs close failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };

    const verify_open = client.open(file.token) catch |err| {
        st.writeLine("fs reopen failed");
        shellLogLine("fs reopen failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    @memset(read_buf[0..], 0);
    const verify_read = client.read(verify_open.token, 0, read_buf[0..8]) catch |err| {
        st.writeLine("fs verify read failed");
        shellLogLine("fs verify read failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    client.close(verify_open.token) catch {};
    if (verify_read.bytes_read < 8 or std.mem.readInt(u64, read_buf[0..8], .little) != next_generation) {
        st.writeLine("fs verify mismatch");
        shellLogLine("fs verify mismatch");
        st.persistent_fs_client_ready = false;
        return false;
    }

    if (!removePersistentFsFileIfPresent(st, client, root.token, temp_name)) return false;
    if (!removePersistentFsFileIfPresent(st, client, root.token, renamed_name)) return false;

    const temp_file = client.create(root.token, temp_name) catch |err| {
        st.writeLine("fs temp create failed");
        shellLogLine("fs temp create failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    const temp_open = client.open(temp_file.token) catch |err| {
        st.writeLine("fs temp open failed");
        shellLogLine("fs temp open failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    for (fs_demo_scratch[0..], 0..) |*byte, index| {
        byte.* = @truncate((index * 13 + 7) & 0xFF);
    }
    _ = client.write(temp_open.token, 0, fs_demo_scratch[0..]) catch |err| {
        st.writeLine("fs temp write failed");
        shellLogLine("fs temp write failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    client.close(temp_open.token) catch |err| {
        st.writeLine("fs temp close failed");
        shellLogLine("fs temp close failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    client.rename(root.token, temp_name, renamed_name) catch |err| {
        st.writeLine("fs rename failed");
        shellLogLine("fs rename failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    client.close(temp_file.token) catch {};

    const renamed_file = client.lookup(root.token, renamed_name) catch |err| {
        st.writeLine("fs renamed lookup failed");
        shellLogLine("fs renamed lookup failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    const renamed_stat = client.stat(renamed_file.token) catch |err| {
        st.writeLine("fs renamed stat failed");
        shellLogLine("fs renamed stat failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    if (renamed_stat.size_bytes != fs_demo_scratch.len) {
        st.writeLine("fs renamed size mismatch");
        shellLogLine("fs renamed size mismatch");
        st.persistent_fs_client_ready = false;
        return false;
    }
    client.close(renamed_file.token) catch {};
    client.unlink(root.token, renamed_name) catch |err| {
        st.writeLine("fs unlink failed");
        shellLogLine("fs unlink failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    _ = client.lookup(root.token, renamed_name) catch |err| switch (err) {
        error.NotFound => {},
        else => {
            st.writeLine("fs unlink verify failed");
            shellLogLine("fs unlink verify failed");
            _ = userLog(@errorName(err));
            _ = userLog("\n");
            st.persistent_fs_client_ready = false;
            return false;
        },
    };

    var name_buf: [48]u8 = undefined;
    const entry = client.readdirOne(root.token, 0, name_buf[0..]) catch |err| {
        st.writeLine("fs readdir failed");
        shellLogLine("fs readdir failed");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        st.persistent_fs_client_ready = false;
        return false;
    };
    switch (entry) {
        .end => st.writeLine("fs root empty"),
        .entry => |dirent| {
            var line_buf: [80]u8 = undefined;
            const line = std.fmt.bufPrint(&line_buf, "fs root entry={s}", .{dirent.name}) catch return false;
            st.writeLine(line);
        },
    }
    var gen_buf: [64]u8 = undefined;
    const gen_line = std.fmt.bufPrint(&gen_buf, "fs generation={d}", .{next_generation}) catch return false;
    st.writeLine(gen_line);
    st.writeLine("fs rename/unlink ok");
    shellLogLine("fs demo done");
    return true;
}

fn runPersistentFsPieUser(st: *ShellState) bool {
    shellLogLine("pie_user start");
    const ok = runPersistentFsExecFile(st, pie_user_rootfs_name);
    if (ok) shellLogLine("pie_user done");
    return ok;
}

fn executeCommand(st: *ShellState) RenderAction {
    const cmd = trimSpaces(st.cmd[0..st.cmd_len]);
    if (cmd.len == 0) return .prompt;
    const parsed = splitCommand(cmd);

    if (eqAsciiNoCase(parsed.head, "help")) {
        st.writeLine("commands: help clear pwd cd mkdir ls stat");
        st.writeLine("cat exec touch write rm mv block_demo");
        st.writeLine("fs_demo pie_user  exec <path>  write <path> <text>");
        return .full;
    }
    if (eqAsciiNoCase(parsed.head, "clear")) {
        st.clearBody();
        st.writeLine("screen cleared");
        return .full;
    }
    if (eqAsciiNoCase(parsed.head, "pwd")) {
        _ = runPersistentFsPwd(st);
        return .full;
    }
    if (eqAsciiNoCase(parsed.head, "cd")) {
        var path_buf: [fs_protocol.max_path_bytes]u8 = undefined;
        const target_path = if (trimSpaces(parsed.tail).len == 0)
            blk: {
                path_buf[0] = '/';
                break :blk path_buf[0..1];
            }
        else
            requireRootfsPath(st, parsed.tail, &path_buf) orelse return .full;
        _ = runPersistentFsCd(st, target_path);
        return .full;
    }
    if (eqAsciiNoCase(parsed.head, "mkdir")) {
        var path_buf: [fs_protocol.max_path_bytes]u8 = undefined;
        const path = requireRootfsPath(st, parsed.tail, &path_buf) orelse return .full;
        _ = runPersistentFsMkdir(st, path);
        return .full;
    }
    if (eqAsciiNoCase(parsed.head, "ls")) {
        var path_buf: [fs_protocol.max_path_bytes]u8 = undefined;
        const path = resolveRootfsPathOrCwd(st, parsed.tail, &path_buf) orelse return .full;
        _ = runPersistentFsList(st, path);
        return .full;
    }
    if (eqAsciiNoCase(parsed.head, "stat")) {
        var path_buf: [fs_protocol.max_path_bytes]u8 = undefined;
        const path = requireRootfsPath(st, parsed.tail, &path_buf) orelse return .full;
        _ = runPersistentFsStat(st, path);
        return .full;
    }
    if (eqAsciiNoCase(parsed.head, "cat")) {
        var path_buf: [fs_protocol.max_path_bytes]u8 = undefined;
        const path = requireRootfsPath(st, parsed.tail, &path_buf) orelse return .full;
        _ = runPersistentFsCat(st, path);
        return .full;
    }
    if (eqAsciiNoCase(parsed.head, "touch")) {
        var path_buf: [fs_protocol.max_path_bytes]u8 = undefined;
        const path = requireRootfsPath(st, parsed.tail, &path_buf) orelse return .full;
        _ = runPersistentFsTouch(st, path);
        return .full;
    }
    if (eqAsciiNoCase(parsed.head, "write")) {
        const args = splitCommand(parsed.tail);
        var path_buf: [fs_protocol.max_path_bytes]u8 = undefined;
        const path = requireRootfsPath(st, args.head, &path_buf) orelse return .full;
        _ = runPersistentFsWrite(st, path, args.tail);
        return .full;
    }
    if (eqAsciiNoCase(parsed.head, "rm")) {
        var path_buf: [fs_protocol.max_path_bytes]u8 = undefined;
        const path = requireRootfsPath(st, parsed.tail, &path_buf) orelse return .full;
        _ = runPersistentFsUnlink(st, path);
        return .full;
    }
    if (eqAsciiNoCase(parsed.head, "mv")) {
        const args = splitCommand(parsed.tail);
        var old_path_buf: [fs_protocol.max_path_bytes]u8 = undefined;
        var new_path_buf: [fs_protocol.max_path_bytes]u8 = undefined;
        const old_path = requireRootfsPath(st, args.head, &old_path_buf) orelse return .full;
        const new_path = requireRootfsPath(st, args.tail, &new_path_buf) orelse return .full;
        _ = runPersistentFsRename(st, old_path, new_path);
        return .full;
    }
    if (eqAsciiNoCase(parsed.head, "exec") or eqAsciiNoCase(parsed.head, "run")) {
        var path_buf: [fs_protocol.max_path_bytes]u8 = undefined;
        const path = requireRootfsPath(st, parsed.tail, &path_buf) orelse return .full;
        _ = runPersistentFsExecFile(st, path);
        return .full;
    }
    if (eqAsciiNoCase(parsed.head, "block") or eqAsciiNoCase(parsed.head, "block_demo")) {
        if (service_registry_abi.findService(service_registry_page_va, .block) == null) {
            st.writeLine(blockLaunchUnavailableReason());
            shellLogLine(blockLaunchUnavailableReason());
            return .full;
        }
        if (runBlockDemo(st)) {
            st.writeLine("block demo ok");
        } else {
            st.writeLine("block demo failed");
        }
        return .full;
    }
    if (eqAsciiNoCase(parsed.head, "fs") or eqAsciiNoCase(parsed.head, "fs_demo")) {
        if (service_registry_abi.findService(service_registry_page_va, .persistent_fs) == null) {
            st.writeLine(persistentFsUnavailableReason());
            shellLogLine(persistentFsUnavailableReason());
            return .full;
        }
        if (runPersistentFsDemo(st)) {
            st.writeLine("fs demo ok");
        } else {
            st.writeLine("fs demo failed");
        }
        return .full;
    }
    if (eqAsciiNoCase(parsed.head, "pie") or eqAsciiNoCase(parsed.head, "pie_user")) {
        if (service_registry_abi.findService(service_registry_page_va, .persistent_fs) == null) {
            st.writeLine(persistentFsUnavailableReason());
            shellLogLine(persistentFsUnavailableReason());
            return .full;
        }
        if (runPersistentFsPieUser(st)) {
            st.writeLine("pie_user ok");
        } else {
            st.writeLine("pie_user failed");
        }
        return .full;
    }

    st.writeLine("unknown command");
    shellLogLine("unknown command");
    return .full;
}

fn initKeyboard() ?KeyboardState {
    if (readCfgU64(0) != input_bootstrap.keyboard_config_magic) return null;

    const common_page_paddr = readCfgU64(1);
    const notify_page_paddr = readCfgU64(2);
    const isr_page_paddr = readCfgU64(3);
    const common_off: usize = @intCast(readCfgU64(5));
    const notify_off: usize = @intCast(readCfgU64(6));
    const isr_off: usize = @intCast(readCfgU64(7));
    const notify_off_multiplier: usize = @intCast(readCfgU64(9));
    var queue_paddr0 = readCfgU64(11);
    var queue_paddr1 = readCfgU64(12);
    var queue_submit_token = readCfgU64(13);
    var queue_notify_token = readCfgU64(14);

    if (common_page_paddr < 0x1000 or notify_page_paddr < 0x1000) return null;
    while (mapMmioPage(common_page_va, common_page_paddr, true) != syscall_ok) {
        _ = waitEvent(false, 1);
    }
    while (mapMmioPage(notify_page_va, notify_page_paddr, true) != syscall_ok) {
        _ = waitEvent(false, 1);
    }
    if (isr_page_paddr != 0) {
        while (mapMmioPage(isr_page_va, isr_page_paddr, false) != syscall_ok) {
            _ = waitEvent(false, 1);
        }
    }

    if (queue_paddr0 < 0x1000 or queue_paddr1 < 0x1000) {
        var queue_paddrs: [2]u64 = .{ 0, 0 };
        if (allocMapPages(queue_page0_va, 2, true, @intFromPtr(&queue_paddrs)) != syscall_ok) return null;
        queue_paddr0 = queue_paddrs[0];
        queue_paddr1 = queue_paddrs[1];
        writeCfgU64(11, queue_paddr0);
        writeCfgU64(12, queue_paddr1);
    }

    while (queue_submit_token == 0 or queue_notify_token == 0) {
        _ = waitEvent(false, 1);
        queue_submit_token = readCfgU64(13);
        queue_notify_token = readCfgU64(14);
    }

    const common_base = common_page_va + common_off;
    const notify_base = notify_page_va + notify_off;
    const isr_base = if (isr_page_paddr != 0) isr_page_va + isr_off else 0;

    mmioWriteU8(common_base + common_device_status, 0);
    mmioWriteU8(common_base + common_device_status, status_acknowledge | status_driver);
    mmioWriteU32(common_base + common_device_feature_select, 0);
    mmioWriteU32(common_base + common_driver_feature_select, 0);
    mmioWriteU32(common_base + common_driver_feature, 0);
    mmioWriteU32(common_base + common_driver_feature_select, 1);
    mmioWriteU32(common_base + common_driver_feature, 0);

    mmioWriteU16(common_base + common_queue_select, queue_index_event);
    const max_size = mmioReadU16(common_base + common_queue_size);
    if (max_size == 0 or max_size < queue_size) return null;

    mmioWriteU16(common_base + common_queue_size, queue_size);
    mmioWriteU64(common_base + common_queue_desc, queue_paddr0);
    mmioWriteU64(common_base + common_queue_avail, queue_paddr0 + (@as(u64, queue_size) * @sizeOf(VirtqDesc)));
    mmioWriteU64(common_base + common_queue_used, queueRegionPhys(queue_paddr0, queue_paddr1, queue_used_offset));

    const queue_notify_off = mmioReadU16(common_base + common_queue_notify_off);
    const notify_addr = notify_base + @as(usize, queue_notify_off) * notify_off_multiplier;
    mmioWriteU16(common_base + common_queue_enable, 1);

    var d: u16 = 0;
    const queue_buffers_base_paddr = queueRegionPhys(queue_paddr0, queue_paddr1, queue_buffers_offset);
    while (d < queue_size) : (d += 1) {
        const event_addr = queue_buffers_base_paddr + @as(u64, d) * @sizeOf(VirtioInputEvent);
        queueDescPtr(d).* = .{
            .addr = event_addr,
            .len = @sizeOf(VirtioInputEvent),
            .flags = desc_flag_write,
            .next = 0,
        };
        queuePushAvail(d);
    }

    if (queueSubmit(queue_submit_token, queue_index_event) != syscall_ok) return null;
    if (queueNotify(queue_notify_token, queue_index_event) != syscall_ok) return null;
    mmioWriteU16(notify_addr, queue_index_event);
    mmioWriteU8(common_base + common_device_status, mmioReadU8(common_base + common_device_status) | status_driver_ok);
    return .{ .notify_addr = notify_addr, .isr_base = isr_base };
}

fn handleAscii(st: *ShellState, ascii: u8) RenderAction {
    switch (ascii) {
        '\n', '\r' => {
            const action = executeCommand(st);
            st.cmd_len = 0;
            return RenderAction.merge(action, .prompt);
        },
        '\x08', '\x7F' => {
            if (st.cmd_len > 0) {
                st.cmd_len -= 1;
                return .prompt;
            }
            return .none;
        },
        else => {
            if (ascii >= 0x20 and ascii <= 0x7E and st.cmd_len < st.cmd.len) {
                st.cmd[st.cmd_len] = ascii;
                st.cmd_len += 1;
                return .prompt;
            }
            return .none;
        },
    }
}

fn pumpKeyboard(keyboard: *KeyboardState, st: *ShellState) RenderAction {
    if (keyboard.isr_base != 0) _ = mmioReadU8(keyboard.isr_base);

    var action: RenderAction = .none;
    var needs_notify = false;
    while (keyboard.last_used_idx != queueUsedIdxPtr().*) {
        const slot: usize = @intCast(keyboard.last_used_idx % queue_size);
        const used_elem = queueUsedRingPtr()[slot];
        const desc_id: u16 = @intCast(used_elem.id & 0xFFFF);
        if (desc_id >= queue_size) {
            keyboard.last_used_idx +%= 1;
            continue;
        }

        const ev = queueEventPtr(desc_id).*;
        switch (ev.event_type) {
            event_type_key => {
                if (ev.code == key_left_shift or ev.code == key_right_shift) {
                    keyboard.shift_down = ev.value != 0;
                }
                keyboard.pending_code = ev.code;
                keyboard.pending_value = ev.value;
                keyboard.has_pending_key = true;
                if (ev.value != 0) {
                    if (keycodeToAscii(ev.code, keyboard.shift_down)) |ascii| {
                        keyboard.pending_ascii = ascii;
                        keyboard.has_pending_ascii = true;
                    } else {
                        keyboard.has_pending_ascii = false;
                    }
                }
            },
            event_type_syn => {
                if (ev.code == syn_report and keyboard.has_pending_key) {
                    if (keyboard.pending_value != 0 and keyboard.has_pending_ascii) {
                        action = RenderAction.merge(action, handleAscii(st, keyboard.pending_ascii));
                    }
                    keyboard.has_pending_key = false;
                    keyboard.has_pending_ascii = false;
                }
            },
            else => {},
        }

        queuePushAvail(desc_id);
        keyboard.last_used_idx +%= 1;
        needs_notify = true;
    }

    if (needs_notify) mmioWriteU16(keyboard.notify_addr, queue_index_event);
    return action;
}

pub export fn _start() noreturn {
    if (allocMapPages(shell_stack_extension_base_va, shell_stack_extension_pages, true, 0) != syscall_ok) {
        _ = userLog("Shell: stack extend failed\n");
        while (true) {
            _ = waitEvent(false, 1);
        }
    }
    var shell = ShellState{};
    resetRootfsPath(&shell);
    const fb_vm_token = readCfgU64(config_framebuffer_vm_token_index);
    if (fb_vm_token != 0) {
        if (mapVmObject(fb_vm_token, framebuffer_va) != syscall_ok) {
            _ = userLog("Shell: framebuffer map failed\n");
            while (true) {
                _ = waitEvent(false, 1);
            }
        }
    } else {
    // Map framebuffer MMIO pages granted by init in batches.
        const fb_paddr = readCfgU64(config_framebuffer_paddr_index);
        const fb_size = readCfgU64(config_framebuffer_size_bytes_index);
        if (fb_paddr == 0 or fb_size == 0) {
            _ = userLog("Shell: framebuffer config missing\n");
            while (true) {
                _ = waitEvent(false, 1);
            }
        }
        const fb_page_count: usize = @intCast((fb_size + 4095) / 4096);
        var fb_paddrs: [syscall_batch_max_pages]u64 = undefined;
        var fb_page_base: usize = 0;
        while (fb_page_base < fb_page_count) : (fb_page_base += syscall_batch_max_pages) {
            const batch_count = @min(fb_page_count - fb_page_base, syscall_batch_max_pages);
            var batch_i: usize = 0;
            while (batch_i < batch_count) : (batch_i += 1) {
                fb_paddrs[batch_i] = fb_paddr + (@as(u64, @intCast(fb_page_base + batch_i)) * 4096);
            }
            const target_va = framebuffer_va + fb_page_base * 4096;
            while (mapPagesBatch(target_va, @intFromPtr(&fb_paddrs), batch_count, true) != syscall_ok) {
                _ = waitEvent(false, 1);
            }
        }
    }

    const vfb: [*]volatile u32 = @ptrFromInt(framebuffer_va);

    _ = userLog("Shell: started\n");
    seedBootSplash(&shell);
    var keyboard = initKeyboard();
    shell.keyboard_ready = keyboard != null;
    if (shell.keyboard_ready) {
        shell.writeLine("ready");
        shell.writeLine("type help");
        _ = userLog("Shell: keyboard ready\n");
    } else {
        shell.writeLine("keyboard unavailable");
        _ = userLog("Shell: keyboard unavailable\n");
    }
    renderFull(vfb, &shell);
    _ = userLog("Shell: ui ready\n");

    while (true) {
        var action: RenderAction = .none;
        if (keyboard) |*kbd| {
            action = pumpKeyboard(kbd, &shell);
        }
        switch (action) {
            .none => {},
            .prompt => renderPrompt(vfb, &shell),
            .full => renderFull(vfb, &shell),
        }
        _ = waitEvent(false, shell_idle_poll_ticks);
    }
}
