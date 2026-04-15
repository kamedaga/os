const std = @import("std");
const boot_logo = @import("generated_pacha_logo.zig");
const boot_manifest_abi = @import("support_root").boot_manifest_abi;
const boot_status_abi = @import("support_root").boot_status_abi;
const boot_status_client = @import("support_root").boot_status_client;
const font = @import("support_root").font;
const image_abi = @import("support_root").image_abi;
const init_bootstrap_abi = @import("support_root").init_bootstrap_abi;
const manager_init_bootstrap_abi = @import("support_root").manager_init_bootstrap_abi;
const bootfs_format = @import("support_root").bootfs_format;
const process_abi = @import("support_root").process_abi;
const queue_abi = @import("support_root").queue_abi;
const rootfs_core = @import("support_root").rootfs_core;

const syscall_log: u64 = 0x9;
const syscall_map_page: u64 = 0x2;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_grant_caps_batch: u64 = 0x14;
const syscall_install_endpoint: u64 = 0x26;
const syscall_signal_endpoint: u64 = 0x2C;
const syscall_install_mmio_cap: u64 = 0x2F;
const syscall_get_process_slot: u64 = 0x2E;

const virtio_vendor_id: u64 = 0x1AF4;
const virtio_input_device_modern: u64 = 0x1052;
const virtio_input_subsystem_id: u64 = 0x0012;
const virtio_blk_device_modern: u64 = 0x1042;
const virtio_blk_subsystem_id: u64 = 0x0002;
const input_cfg_select: u64 = 0;
const input_cfg_subsel: u64 = 1;
const input_cfg_size: u64 = 2;
const input_cfg_payload: u64 = 8;
const virtio_input_cfg_select_ev_bits: u8 = 0x11;
const virtio_input_ev_key: u8 = 0x01;
const virtio_input_ev_rel: u8 = 0x02;
const virtio_input_ev_abs: u8 = 0x03;
const input_code_rel_x: u16 = 0x00;
const input_code_rel_y: u16 = 0x01;
const input_code_abs_x: u16 = 0x00;
const input_code_abs_y: u16 = 0x01;
const input_code_key_a: u16 = 0x1E;
const input_code_btn_left: u16 = 0x110;
const virtio_blk_capacity_offset: u64 = 0x00;
const virtio_blk_block_size_offset: u64 = 0x14;
const inspect_mmio_base_va: u64 = 0x3F00_0000;
const init_stack_extension_pages: u64 = 8;
const init_stack_extension_base_va: u64 = process_abi.aux_base_va - ((init_stack_extension_pages + 1) * 4096);
const boot_log_framebuffer_base_va: u64 = 0x3D00_0000;

const seed_exec_path = "/srv/seed.elf";
const persistent_fs_start_block: u64 = 395264;
const boot_log_bg_color: u32 = 0x0000_0000;
const boot_log_label_color: u32 = 0x0032_FF5A;
const boot_log_text_color: u32 = 0x00C4_CCC4;
const boot_log_margin_x: i32 = 18;
const boot_log_margin_y: i32 = 18;
const boot_log_logo_gap_y: i32 = 16;
const boot_log_min_rows_after_logo: i32 = 6;
const boot_log_scale: i32 = 1;
const boot_banner_title = "MICROKERNEL";
const boot_banner_subtitle = "ENTER BARE-METAL CAPABILITY BASED KERNEL";
const seed_exec_bootfs_path = "/srv/seed.elf";

const BlockGeometry = struct {
    capacity_sectors: u64,
    logical_block_size: u64,
};

const InputDeviceKind = enum {
    pointer,
    keyboard,
};

fn inputDeviceHintValue(kind: InputDeviceKind) u64 {
    return switch (kind) {
        .pointer => @intFromEnum(manager_init_bootstrap_abi.InputDeviceHint.pointer),
        .keyboard => @intFromEnum(manager_init_bootstrap_abi.InputDeviceHint.keyboard),
    };
}

const DeviceMmioPageSet = struct {
    paddrs: [4]u64 = [_]u64{0} ** 4,
    rights: [4]u64 = [_]u64{0} ** 4,
    count: usize = 0,
};

const MmioCapCache = struct {
    paddrs: [init_bootstrap_abi.max_device_descriptors * 4]u64 = [_]u64{0} ** (init_bootstrap_abi.max_device_descriptors * 4),
    rights: [init_bootstrap_abi.max_device_descriptors * 4]u64 = [_]u64{0} ** (init_bootstrap_abi.max_device_descriptors * 4),
    count: usize = 0,
};

var next_inspect_mmio_page_va: u64 = inspect_mmio_base_va;
var manager_bootstrap_pages_storage: [process_abi.max_bootstrap_page_descriptors]process_abi.BootstrapPageDescriptor = undefined;
var manager_bootstrap_table_storage = process_abi.BootstrapDescriptorTable{};
var manager_bootstrap_handoff_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var manager_bootstrap_handoff_source_va: u64 = 0;
var manager_device_needed_cache: [init_bootstrap_abi.max_device_descriptors]bool = [_]bool{false} ** init_bootstrap_abi.max_device_descriptors;
var manager_device_input_hint_cache: [init_bootstrap_abi.max_device_descriptors]u64 = [_]u64{0} ** init_bootstrap_abi.max_device_descriptors;
var mmio_cap_cache = MmioCapCache{};

const BootLogFramebuffer = struct {
    enabled: bool = false,
    width: usize = 0,
    height: usize = 0,
    pitch: usize = 0,
    max_cols: usize = 0,
    cursor_row: usize = 0,
    cursor_len: usize = 0,
    max_rows: usize = 0,
    line_height: i32 = 0,
    text_origin_y: i32 = boot_log_margin_y,
    current_line: [160]u8 = [_]u8{0} ** 160,

    fn pixels(self: *const BootLogFramebuffer) [*]volatile u32 {
        _ = self;
        return @ptrFromInt(boot_log_framebuffer_base_va);
    }
};

var boot_log_framebuffer = BootLogFramebuffer{};

const bootseed_log_prefix = "[bootseed] ";
const legacy_boot_init_log_prefix = "BootInit: ";

fn userLogRaw(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn stripLegacyBootseedPrefix(message: []const u8) []const u8 {
    if (std.mem.startsWith(u8, message, bootseed_log_prefix)) return message;
    if (std.mem.startsWith(u8, message, legacy_boot_init_log_prefix)) return message[legacy_boot_init_log_prefix.len..];
    return message;
}

fn bootLogBlendPixel(ctx: *BootLogFramebuffer, x: i32, y: i32, color: u32, alpha: u8) void {
    if (!ctx.enabled) return;
    if (x < 0 or y < 0) return;
    const ux: usize = @intCast(x);
    const uy: usize = @intCast(y);
    if (ux >= ctx.width or uy >= ctx.height) return;
    const index = uy * ctx.pitch + ux;
    const vfb = ctx.pixels();
    vfb[index] = font.blendColor(vfb[index], color, alpha);
}

fn bootLogFillRect(ctx: *BootLogFramebuffer, x: usize, y: usize, w: usize, h: usize, color: u32) void {
    if (!ctx.enabled or w == 0 or h == 0) return;
    var yy: usize = y;
    while (yy < y + h and yy < ctx.height) : (yy += 1) {
        const row = yy * ctx.pitch;
        var xx: usize = x;
        while (xx < x + w and xx < ctx.width) : (xx += 1) {
            ctx.pixels()[row + xx] = color;
        }
    }
}

fn bootLogClearCurrentRow(ctx: *BootLogFramebuffer) void {
    if (!ctx.enabled) return;
    const y: usize = @intCast(ctx.text_origin_y + @as(i32, @intCast(ctx.cursor_row)) * ctx.line_height);
    const h: usize = @intCast(ctx.line_height);
    const x0: usize = if (boot_log_margin_x > 0) @intCast(boot_log_margin_x) else 0;
    const w = ctx.width -| x0;
    bootLogFillRect(ctx, x0, y, w, h, boot_log_bg_color);
}

fn bootLogScrollUp(ctx: *BootLogFramebuffer) void {
    if (!ctx.enabled or ctx.max_rows == 0) return;

    const scroll_h: usize = @intCast(ctx.line_height);
    const x0: usize = if (boot_log_margin_x > 0) @intCast(boot_log_margin_x) else 0;
    const w = ctx.width -| x0;
    const y0: usize = @intCast(ctx.text_origin_y);
    const text_h = ctx.max_rows * scroll_h;
    if (w == 0 or text_h == 0) return;

    if (text_h > scroll_h) {
        const copy_h = text_h - scroll_h;
        const pixels = ctx.pixels();
        var row: usize = 0;
        while (row < copy_h and y0 + row + scroll_h < ctx.height and y0 + row < ctx.height) : (row += 1) {
            const dst_row = (y0 + row) * ctx.pitch + x0;
            const src_row = (y0 + row + scroll_h) * ctx.pitch + x0;
            var col: usize = 0;
            while (col < w and x0 + col < ctx.width) : (col += 1) {
                pixels[dst_row + col] = pixels[src_row + col];
            }
        }
    }

    const clear_y = y0 + text_h - scroll_h;
    const clear_h = @min(scroll_h, ctx.height -| clear_y);
    if (clear_h != 0) bootLogFillRect(ctx, x0, clear_y, w, clear_h, boot_log_bg_color);
}

fn bootLogRenderCurrentLine(ctx: *BootLogFramebuffer) void {
    if (!ctx.enabled) return;
    bootLogClearCurrentRow(ctx);
    const text = ctx.current_line[0..ctx.cursor_len];
    if (text.len == 0) return;
    const max_x: i32 = @intCast(ctx.width);
    const y = ctx.text_origin_y + @as(i32, @intCast(ctx.cursor_row)) * ctx.line_height;
    if (text[0] == '[') {
        if (std.mem.indexOfScalar(u8, text, ']')) |close_index| {
            const label = text[0 .. close_index + 1];
            font.drawAsciiTextClipped(*BootLogFramebuffer, bootLogBlendPixel, ctx, boot_log_margin_x, y, label, boot_log_label_color, boot_log_scale, max_x - boot_log_margin_x);
            var body = text[close_index + 1 ..];
            if (body.len != 0 and body[0] == ' ') body = body[1..];
            if (body.len != 0) {
                const label_width = font.measureAsciiText(label, boot_log_scale);
                font.drawAsciiTextClipped(*BootLogFramebuffer, bootLogBlendPixel, ctx, boot_log_margin_x + label_width + font.consoleAdvance(boot_log_scale), y, body, boot_log_text_color, boot_log_scale, max_x - boot_log_margin_x);
            }
            return;
        }
    }
    font.drawAsciiTextClipped(*BootLogFramebuffer, bootLogBlendPixel, ctx, boot_log_margin_x, y, text, boot_log_text_color, boot_log_scale, max_x - boot_log_margin_x);
}

fn bootLogAdvanceLine(ctx: *BootLogFramebuffer) void {
    if (!ctx.enabled) return;
    if (ctx.cursor_row + 1 < ctx.max_rows) {
        ctx.cursor_row += 1;
    } else {
        bootLogScrollUp(ctx);
    }
    ctx.cursor_len = 0;
    @memset(ctx.current_line[0..], 0);
}

fn bootLogAppendText(message: []const u8) void {
    if (!boot_log_framebuffer.enabled) return;
    for (message) |ch| {
        switch (ch) {
            '\r' => {},
            '\n' => {
                bootLogRenderCurrentLine(&boot_log_framebuffer);
                bootLogAdvanceLine(&boot_log_framebuffer);
            },
            '\t' => {
                var tab_count: usize = 0;
                while (tab_count < 4) : (tab_count += 1) {
                    bootLogAppendText(" ");
                }
            },
            else => {
                if (ch < 0x20 or ch > 0x7E) continue;
                if (boot_log_framebuffer.cursor_len >= boot_log_framebuffer.max_cols) {
                    bootLogRenderCurrentLine(&boot_log_framebuffer);
                    bootLogAdvanceLine(&boot_log_framebuffer);
                }
                if (boot_log_framebuffer.cursor_len >= boot_log_framebuffer.max_cols) continue;
                boot_log_framebuffer.current_line[boot_log_framebuffer.cursor_len] = ch;
                boot_log_framebuffer.cursor_len += 1;
            },
        }
    }
    bootLogRenderCurrentLine(&boot_log_framebuffer);
}

fn bootLogShouldDrawLogo(width: usize, height: usize, line_height: i32) bool {
    if (boot_logo.width == 0 or boot_logo.height == 0) return false;
    const screen_width: i32 = @intCast(width);
    const screen_height: i32 = @intCast(height);
    const available_width = screen_width - boot_log_margin_x * 2;
    const logo_width: i32 = @intCast(boot_logo.width);
    const logo_height: i32 = @intCast(boot_logo.height);
    const min_required_height = boot_log_margin_y +
        logo_height +
        boot_log_logo_gap_y +
        line_height * boot_log_min_rows_after_logo +
        boot_log_margin_y;
    return available_width >= logo_width and screen_height >= min_required_height;
}

fn bootLogTextOriginY(width: usize, height: usize, line_height: i32) i32 {
    if (!bootLogShouldDrawLogo(width, height, line_height)) return boot_log_margin_y;
    return boot_log_margin_y + @as(i32, @intCast(boot_logo.height)) + boot_log_logo_gap_y;
}

fn bootLogDrawLogo(ctx: *BootLogFramebuffer) void {
    if (!ctx.enabled) return;
    if (ctx.text_origin_y <= boot_log_margin_y) return;

    const x0 = boot_log_margin_x;
    const y0 = boot_log_margin_y;
    var pixel_index: usize = 0;
    var y: usize = 0;
    while (y < boot_logo.height) : (y += 1) {
        var x: usize = 0;
        while (x < boot_logo.width) : (x += 1) {
            const packed_pixel = boot_logo.packedPixel(pixel_index);
            pixel_index += 1;
            const alpha: u8 = @truncate(packed_pixel >> 24);
            if (alpha == 0) continue;
            bootLogBlendPixel(
                ctx,
                x0 + @as(i32, @intCast(x)),
                y0 + @as(i32, @intCast(y)),
                packed_pixel & 0x00FF_FFFF,
                alpha,
            );
        }
    }
}

fn initBootLogFramebuffer() void {
    const page = descriptorPage() orelse return;
    if ((page.primary_display.flags & init_bootstrap_abi.display_flag_present) == 0) return;
    if (page.primary_display.framebuffer_paddr == 0 or page.primary_display.framebuffer_size_bytes == 0) return;

    const width: usize = @intCast(page.primary_display.width);
    const height: usize = @intCast(page.primary_display.height);
    const pitch: usize = @intCast(page.primary_display.pitch);
    if (width == 0 or height == 0 or pitch < width) return;

    const fb_vm_token = installVmObjectMmioRange(
        page.primary_display.framebuffer_paddr,
        page.primary_display.framebuffer_size_bytes,
        .{ .read = true, .write = true, .map = true },
    );
    if (image_abi.decodeVmObjectToken(fb_vm_token) == null) return;
    if (mapVmObject(fb_vm_token, boot_log_framebuffer_base_va) != 0) return;

    const line_height = font.lineHeight(boot_log_scale);
    const advance = font.consoleAdvance(boot_log_scale);
    if (line_height <= 0) return;
    if (advance <= 0) return;
    const text_origin_y = bootLogTextOriginY(width, height, line_height);
    const available_height = @as(i32, @intCast(height)) - text_origin_y - boot_log_margin_y;
    const available_width = @as(i32, @intCast(width)) - boot_log_margin_x * 2;
    const max_rows: usize = if (available_height > line_height)
        @intCast(@divTrunc(available_height, line_height))
    else
        1;
    const max_cols: usize = if (available_width > advance)
        @min(boot_log_framebuffer.current_line.len, @as(usize, @intCast(@divTrunc(available_width, advance))))
    else
        1;

    boot_log_framebuffer = .{
        .enabled = true,
        .width = width,
        .height = height,
        .pitch = pitch,
        .max_cols = max_cols,
        .cursor_row = 0,
        .cursor_len = 0,
        .max_rows = max_rows,
        .line_height = line_height,
        .text_origin_y = text_origin_y,
    };
    bootLogFillRect(&boot_log_framebuffer, 0, 0, width, height, boot_log_bg_color);
    bootLogDrawLogo(&boot_log_framebuffer);
}

fn emitBootBanner() void {
    bootLogAppendText(boot_banner_title ++ "\n");
    bootLogAppendText(boot_banner_subtitle ++ "\n\n");
}

fn emitKernelDebugSnapshot() void {
    const length_ptr: *const volatile u32 = @ptrFromInt(init_bootstrap_abi.boot_log_user_page_va + init_bootstrap_abi.boot_log_page_length_offset);
    const status_ptr: *const volatile u32 = @ptrFromInt(init_bootstrap_abi.boot_log_user_page_va + init_bootstrap_abi.boot_log_page_status_offset);
    if (status_ptr.* == 0) return;
    const text_len: usize = @min(@as(usize, length_ptr.*), init_bootstrap_abi.boot_log_page_payload_bytes);
    if (text_len == 0) return;
    const payload: [*]const volatile u8 = @ptrFromInt(init_bootstrap_abi.boot_log_user_page_va + init_bootstrap_abi.boot_log_page_header_bytes);
    var buf: [init_bootstrap_abi.boot_log_page_payload_bytes]u8 = undefined;
    var i: usize = 0;
    while (i < text_len) : (i += 1) {
        buf[i] = payload[i];
    }
    bootLogAppendText("[debug] pre-bootseed log\n");
    bootLogAppendText(buf[0..text_len]);
    if (buf[text_len - 1] != '\n') bootLogAppendText("\n");
    bootLogAppendText("\n");
}

fn emitUserLog(message: []const u8) u64 {
    bootLogAppendText(message);
    return userLogRaw(message);
}

fn userLog(message: []const u8) u64 {
    const stripped = stripLegacyBootseedPrefix(message);
    var buf: [512]u8 = undefined;
    var len: usize = 0;
    while (len < bootseed_log_prefix.len and len < buf.len) : (len += 1) buf[len] = bootseed_log_prefix[len];
    var msg_index: usize = 0;
    while (msg_index < stripped.len and len < buf.len) : ({
        msg_index += 1;
        len += 1;
    }) {
        buf[len] = stripped[msg_index];
    }
    return emitUserLog(buf[0..len]);
}

fn userLogHex(label: []const u8, value: u64) void {
    const stripped_label = stripLegacyBootseedPrefix(label);
    var buf: [128]u8 = undefined;
    var len: usize = 0;
    while (len < bootseed_log_prefix.len and len < buf.len) : (len += 1) buf[len] = bootseed_log_prefix[len];
    var label_index: usize = 0;
    while (label_index < stripped_label.len and len < buf.len) : ({
        label_index += 1;
        len += 1;
    }) {
        buf[len] = stripped_label[label_index];
    }
    if (len + 19 >= buf.len) return;
    buf[len] = '0';
    buf[len + 1] = 'x';
    len += 2;
    var shift: u6 = 60;
    while (true) {
        const nibble: u8 = @intCast((value >> shift) & 0xF);
        buf[len] = if (nibble < 10) '0' + nibble else 'A' + (nibble - 10);
        len += 1;
        if (shift == 0) break;
        shift -= 4;
    }
    buf[len] = '\n';
    len += 1;
    _ = emitUserLog(buf[0..len]);
}

fn logManagerGrantDeviceStep(
    device_index: usize,
    descriptor: init_bootstrap_abi.DeviceDescriptor,
    step: []const u8,
) void {
    var buf: [160]u8 = undefined;
    const msg = std.fmt.bufPrint(
        &buf,
        "BootInit: manager dev={d} did=0x{X} sid=0x{X} {s}\n",
        .{ device_index, descriptor.device_id, descriptor.subsystem_id, step },
    ) catch return;
    _ = userLog(msg);
}

fn bootFail(message: []const u8) noreturn {
    _ = userLog(message);
    while (true) asm volatile ("pause");
}

fn noteBootStatus(status_bits: u32) void {
    _ = boot_status_client.set(status_bits);
}

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_page),
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

fn installMmioCap(paddr: u64, rights_bits: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_install_mmio_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (rights_bits),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantCap(to_process_slot: u64, paddr: u64, rights_bits: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (@as(u64, 0x8)),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (to_process_slot),
          [arg2] "{rdx}" (rights_bits),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantCapsBatch(paddr_list_va: u64, page_count: u64, to_process_slot: u64, rights_bits: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_grant_caps_batch),
          [arg0] "{rdi}" (paddr_list_va),
          [arg1] "{rsi}" (page_count),
          [arg2] "{rdx}" (to_process_slot),
          [arg3] "{rcx}" (rights_bits),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantQueueCap(token: u64, to_process_slot: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (queue_abi.syscall_grant_cap),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (to_process_slot),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn spawnExecWithExtendedBootstrapTable(exec_token: u64, table: *const process_abi.BootstrapDescriptorTable) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (process_abi.syscall_spawn_exec),
          [arg0] "{rdi}" (exec_token),
          [arg1] "{rsi}" (@as(u64, @intFromPtr(table))),
          [arg2] "{rdx}" (@as(u64, 0)),
          [arg3] "{rcx}" (process_abi.spawn_flag_bootstrap_extended_descriptor_table | process_abi.spawn_flag_child_bootstrap_owner),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn installVmObjectMmioRange(base_paddr: u64, size_bytes: u64, rights: image_abi.VmObjectRights) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (image_abi.syscall_install_vm_object_mmio_range),
          [arg0] "{rdi}" (base_paddr),
          [arg1] "{rsi}" (size_bytes),
          [arg2] "{rdx}" (image_abi.vmObjectRightsToBits(rights)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn installVmObject(base_va: u64, size_bytes: u64, rights: image_abi.VmObjectRights) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (image_abi.syscall_install_vm_object),
          [arg0] "{rdi}" (base_va),
          [arg1] "{rsi}" (size_bytes),
          [arg2] "{rdx}" (image_abi.vmObjectRightsToBits(rights)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn installExecImage(vm_token: u64, rights: image_abi.ExecImageRights) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (image_abi.syscall_install_exec_image),
          [arg0] "{rdi}" (vm_token),
          [arg1] "{rsi}" (image_abi.execImageRightsToBits(rights)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mapVmObject(token: u64, target_va: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (image_abi.syscall_map_vm_object),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (target_va),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn configPage() ?*const volatile init_bootstrap_abi.ConfigPage {
    const page: *const volatile init_bootstrap_abi.ConfigPage = @ptrFromInt(process_abi.standard_config_target_va);
    if (page.magic != init_bootstrap_abi.config_magic) return null;
    if (page.version != init_bootstrap_abi.config_version) return null;
    if (page.descriptor_page_va == 0) return null;
    return page;
}

fn descriptorPage() ?*const volatile init_bootstrap_abi.DescriptorPage {
    const cfg = configPage() orelse return null;
    const page: *const volatile init_bootstrap_abi.DescriptorPage = @ptrFromInt(cfg.descriptor_page_va);
    if (page.magic != init_bootstrap_abi.magic) return null;
    if (page.version != init_bootstrap_abi.version) return null;
    return page;
}

fn findSpawnPageDescriptor(
    kind: init_bootstrap_abi.SpawnPageKind,
    subject: init_bootstrap_abi.SpawnPageSubject,
) ?init_bootstrap_abi.SpawnPageDescriptor {
    const page = descriptorPage() orelse return null;
    var i: usize = 0;
    while (i < page.spawn_page_count and i < init_bootstrap_abi.max_spawn_page_descriptors) : (i += 1) {
        const descriptor = page.spawn_pages[i];
        if (descriptor.kind != @intFromEnum(kind)) continue;
        if (descriptor.subject != @intFromEnum(subject)) continue;
        return descriptor;
    }
    return null;
}

fn requireSpawnPageDescriptor(
    kind: init_bootstrap_abi.SpawnPageKind,
    subject: init_bootstrap_abi.SpawnPageSubject,
    failure_message: []const u8,
) init_bootstrap_abi.SpawnPageDescriptor {
    return findSpawnPageDescriptor(kind, subject) orelse bootFail(failure_message);
}

fn findBootImageDescriptor(kind: boot_manifest_abi.ImageKind) ?boot_manifest_abi.BootImageDescriptor {
    const page = descriptorPage() orelse return null;
    var i: usize = 0;
    while (i < page.boot_image_count and i < boot_manifest_abi.max_boot_image_descriptors) : (i += 1) {
        const descriptor = page.boot_images[i];
        if (descriptor.kind != @intFromEnum(kind)) continue;
        return descriptor;
    }
    return null;
}

fn requireBootImage(kind: boot_manifest_abi.ImageKind, failure_message: []const u8) void {
    const descriptor = findBootImageDescriptor(kind) orelse bootFail(failure_message);
    if ((descriptor.flags & boot_manifest_abi.image_flag_present) == 0) bootFail(failure_message);
}

fn isBootstrapDeviceDescriptorPresent(descriptor: init_bootstrap_abi.DeviceDescriptor) bool {
    return (descriptor.flags & init_bootstrap_abi.device_flag_present) != 0;
}

fn isVirtioBlockDeviceDescriptor(descriptor: init_bootstrap_abi.DeviceDescriptor) bool {
    return descriptor.transport == @intFromEnum(init_bootstrap_abi.DeviceTransport.virtio_pci_modern) and
        descriptor.vendor_id == virtio_vendor_id and
        (descriptor.device_id == virtio_blk_device_modern or descriptor.subsystem_id == virtio_blk_subsystem_id);
}

fn isVirtioInputDeviceDescriptor(descriptor: init_bootstrap_abi.DeviceDescriptor) bool {
    return descriptor.transport == @intFromEnum(init_bootstrap_abi.DeviceTransport.virtio_pci_modern) and
        descriptor.vendor_id == virtio_vendor_id and
        (descriptor.device_id == virtio_input_device_modern or descriptor.subsystem_id == virtio_input_subsystem_id);
}

fn requireBlockDeviceDescriptor(failure_message: []const u8) init_bootstrap_abi.DeviceDescriptor {
    const page = descriptorPage() orelse bootFail("BootInit: descriptor page missing\n");
    var i: usize = 0;
    while (i < page.device_count and i < init_bootstrap_abi.max_device_descriptors) : (i += 1) {
        const descriptor = page.devices[i];
        if (!isBootstrapDeviceDescriptorPresent(descriptor)) continue;
        if (!isVirtioBlockDeviceDescriptor(descriptor)) continue;
        return descriptor;
    }
    bootFail(failure_message);
}

fn appendDeviceMmioPage(set: *DeviceMmioPageSet, paddr: u64, rights_bits: u64) bool {
    if (paddr == 0) return true;
    var i: usize = 0;
    while (i < set.count) : (i += 1) {
        if (set.paddrs[i] != paddr) continue;
        set.rights[i] |= rights_bits;
        return true;
    }
    if (set.count >= set.paddrs.len) return false;
    set.paddrs[set.count] = paddr;
    set.rights[set.count] = rights_bits;
    set.count += 1;
    return true;
}

fn collectDeviceMmioPages(descriptor: init_bootstrap_abi.DeviceDescriptor, writable_device_page: bool) ?DeviceMmioPageSet {
    const page_right_cpu_read: u64 = 0x1;
    const page_right_cpu_write: u64 = 0x2;
    if (descriptor.common_page_paddr == 0 or descriptor.notify_page_paddr == 0) return null;
    var set = DeviceMmioPageSet{};
    if (!appendDeviceMmioPage(&set, descriptor.common_page_paddr, page_right_cpu_read | page_right_cpu_write)) return null;
    if (!appendDeviceMmioPage(&set, descriptor.notify_page_paddr, page_right_cpu_read | page_right_cpu_write)) return null;
    if (!appendDeviceMmioPage(&set, descriptor.isr_page_paddr, page_right_cpu_read)) return null;
    const device_rights = page_right_cpu_read | if (writable_device_page) page_right_cpu_write else @as(u64, 0);
    if (!appendDeviceMmioPage(&set, descriptor.device_page_paddr, device_rights)) return null;
    return set;
}

fn ensureMmioCapInstalled(paddr: u64, rights_bits: u64) bool {
    if (paddr == 0) return true;
    var i: usize = 0;
    while (i < mmio_cap_cache.count) : (i += 1) {
        if (mmio_cap_cache.paddrs[i] != paddr) continue;
        if ((mmio_cap_cache.rights[i] & rights_bits) == rights_bits) return true;
        const merged_rights = mmio_cap_cache.rights[i] | rights_bits;
        if (installMmioCap(paddr, merged_rights) != 0) return false;
        mmio_cap_cache.rights[i] = merged_rights;
        return true;
    }
    if (installMmioCap(paddr, rights_bits) != 0) return false;
    if (mmio_cap_cache.count >= mmio_cap_cache.paddrs.len) return false;
    mmio_cap_cache.paddrs[mmio_cap_cache.count] = paddr;
    mmio_cap_cache.rights[mmio_cap_cache.count] = rights_bits;
    mmio_cap_cache.count += 1;
    return true;
}

fn ensureDeviceMmioCapsInstalled(descriptor: init_bootstrap_abi.DeviceDescriptor) bool {
    const page_right_grant: u64 = 0x8;
    const set = collectDeviceMmioPages(descriptor, true) orelse return false;
    var i: usize = 0;
    while (i < set.count) : (i += 1) {
        if (!ensureMmioCapInstalled(set.paddrs[i], set.rights[i] | page_right_grant)) return false;
    }
    return true;
}

fn grantDeviceMmioPages(descriptor: init_bootstrap_abi.DeviceDescriptor, child_process_slot: u64) bool {
    const set = collectDeviceMmioPages(descriptor, false) orelse return false;
    var processed_rights: [4]u64 = undefined;
    var processed_len: usize = 0;
    var grouped_paddrs: [4]u64 = undefined;
    var i: usize = 0;
    while (i < set.count) : (i += 1) {
        var group_len: usize = 0;
        const rights_bits = set.rights[i];
        var already_processed = false;
        var processed_index: usize = 0;
        while (processed_index < processed_len) : (processed_index += 1) {
            if (processed_rights[processed_index] == rights_bits) {
                already_processed = true;
                break;
            }
        }
        if (already_processed) continue;
        var j: usize = 0;
        while (j < set.count) : (j += 1) {
            if (set.rights[j] != rights_bits) continue;
            var duplicate = false;
            var k: usize = 0;
            while (k < group_len) : (k += 1) {
                if (grouped_paddrs[k] == set.paddrs[j]) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            grouped_paddrs[group_len] = set.paddrs[j];
            group_len += 1;
        }
        processed_rights[processed_len] = rights_bits;
        processed_len += 1;
        if (group_len == 1) {
            if (grantCap(child_process_slot, grouped_paddrs[0], rights_bits) != 0) return false;
        } else {
            if (grantCapsBatch(@intFromPtr(&grouped_paddrs), group_len, child_process_slot, rights_bits) != 0) return false;
        }
    }
    return true;
}

fn mapInspectMmioPage(paddr: u64, writable: bool) ?u64 {
    if (paddr == 0) return null;
    const page_va = next_inspect_mmio_page_va;
    next_inspect_mmio_page_va +%= 0x1000;
    if (mapPage(page_va, paddr, writable) != 0) return null;
    return page_va;
}

fn deviceCfgInspectVaForClassification(descriptor: init_bootstrap_abi.DeviceDescriptor, writable: bool) ?u64 {
    const page_right_cpu_read: u64 = 0x1;
    const page_right_cpu_write: u64 = 0x2;
    const page_right_grant: u64 = 0x8;
    if (descriptor.device_page_paddr == 0) return null;
    const rights = page_right_cpu_read | if (writable) page_right_cpu_write else @as(u64, 0);
    if (!ensureMmioCapInstalled(descriptor.device_page_paddr, rights | page_right_grant)) return null;
    const page_va = mapInspectMmioPage(descriptor.device_page_paddr, writable) orelse return null;
    return page_va + descriptor.device_page_offset;
}

fn deviceCfgInspectVa(descriptor: init_bootstrap_abi.DeviceDescriptor, writable: bool) ?u64 {
    if (!ensureDeviceMmioCapsInstalled(descriptor)) return null;
    const page_va = mapInspectMmioPage(descriptor.device_page_paddr, writable) orelse return null;
    return page_va + descriptor.device_page_offset;
}

fn mmioReadU8(addr: u64) u8 {
    const ptr: *const volatile u8 = @ptrFromInt(addr);
    return ptr.*;
}

fn mmioWriteU8(addr: u64, value: u8) void {
    const ptr: *volatile u8 = @ptrFromInt(addr);
    ptr.* = value;
}

fn mmioReadU32(addr: u64) u32 {
    const ptr: *const volatile u32 = @ptrFromInt(addr);
    return ptr.*;
}

fn mmioReadU64(addr: u64) u64 {
    const ptr: *const volatile u64 = @ptrFromInt(addr);
    return ptr.*;
}

fn readInputBitmapBit(device_cfg_va: u64, ev_type: u8, code: u16) bool {
    mmioWriteU8(device_cfg_va + input_cfg_select, virtio_input_cfg_select_ev_bits);
    mmioWriteU8(device_cfg_va + input_cfg_subsel, ev_type);
    const size = mmioReadU8(device_cfg_va + input_cfg_size);
    if (size == 0) return false;
    const byte_index: usize = @intCast(code / 8);
    if (byte_index >= size or byte_index >= 128) return false;
    const bit_index: u3 = @intCast(code & 7);
    const bits = mmioReadU8(device_cfg_va + input_cfg_payload + byte_index);
    return ((bits >> bit_index) & 1) != 0;
}

fn classifyInputDeviceDescriptor(descriptor: init_bootstrap_abi.DeviceDescriptor) ?InputDeviceKind {
    const device_cfg_va = deviceCfgInspectVaForClassification(descriptor, true) orelse return null;
    const has_rel_x = readInputBitmapBit(device_cfg_va, virtio_input_ev_rel, input_code_rel_x);
    const has_rel_y = readInputBitmapBit(device_cfg_va, virtio_input_ev_rel, input_code_rel_y);
    const has_abs_x = readInputBitmapBit(device_cfg_va, virtio_input_ev_abs, input_code_abs_x);
    const has_abs_y = readInputBitmapBit(device_cfg_va, virtio_input_ev_abs, input_code_abs_y);
    const has_key_a = readInputBitmapBit(device_cfg_va, virtio_input_ev_key, input_code_key_a);
    const has_btn_left = readInputBitmapBit(device_cfg_va, virtio_input_ev_key, input_code_btn_left);
    const pointer_like = (has_rel_x and has_rel_y) or (has_abs_x and has_abs_y and has_btn_left);
    const keyboard_like = has_key_a and !has_rel_x and !has_rel_y and !has_abs_x and !has_abs_y;
    if (pointer_like) return .pointer;
    if (keyboard_like) return .keyboard;
    return null;
}

fn rebuildManagerDeviceNeedCache(page: *const volatile init_bootstrap_abi.DescriptorPage) void {
    var i: usize = 0;
    while (i < manager_device_needed_cache.len) : (i += 1) {
        manager_device_needed_cache[i] = false;
        manager_device_input_hint_cache[i] = 0;
    }
    i = 0;
    while (i < page.device_count and i < init_bootstrap_abi.max_device_descriptors) : (i += 1) {
        const descriptor = page.devices[i];
        if (!isBootstrapDeviceDescriptorPresent(descriptor)) continue;
        if (isVirtioBlockDeviceDescriptor(descriptor)) {
            manager_device_needed_cache[i] = true;
            continue;
        }
        if (!isVirtioInputDeviceDescriptor(descriptor)) continue;
        const kind = classifyInputDeviceDescriptor(descriptor) orelse continue;
        manager_device_input_hint_cache[i] = inputDeviceHintValue(kind);
        manager_device_needed_cache[i] = kind == .keyboard;
    }
}

fn isManagerBootstrapSpawnPageNeeded(descriptor: init_bootstrap_abi.SpawnPageDescriptor) bool {
    return descriptor.kind == @intFromEnum(init_bootstrap_abi.SpawnPageKind.service_config) and
        descriptor.subject == @intFromEnum(init_bootstrap_abi.SpawnPageSubject.window_service);
}

fn readBlockGeometry(descriptor: init_bootstrap_abi.DeviceDescriptor) ?BlockGeometry {
    const device_cfg_va = deviceCfgInspectVa(descriptor, false) orelse return null;
    const logical_block_size = blk: {
        const reported = mmioReadU32(device_cfg_va + virtio_blk_block_size_offset);
        break :blk if (reported != 0) @as(u64, reported) else @as(u64, 512);
    };
    return .{
        .capacity_sectors = mmioReadU64(device_cfg_va + virtio_blk_capacity_offset),
        .logical_block_size = logical_block_size,
    };
}

fn bootfsArchiveHeader() ?*const bootfs_format.BootFsHeader {
    const page = descriptorPage() orelse return null;
    const archive = page.bootfs_archive;
    if ((archive.flags & init_bootstrap_abi.boot_archive_flag_present) == 0) return null;
    if (archive.image_va == 0 or archive.size_bytes < @sizeOf(bootfs_format.BootFsHeader)) return null;
    const header: *const bootfs_format.BootFsHeader = @ptrFromInt(archive.image_va);
    if (header.magic != bootfs_format.magic or header.version != bootfs_format.version) return null;
    if (header.image_bytes > archive.size_bytes) return null;
    const entry_table_end = header.entry_table_offset + @as(u64, header.entry_count) * @sizeOf(bootfs_format.BootFsEntry);
    if (header.entry_table_offset < header.header_bytes or entry_table_end > header.image_bytes) return null;
    if (header.string_table_offset + header.string_table_bytes > header.image_bytes) return null;
    if (header.data_offset + header.data_bytes > header.image_bytes) return null;
    return header;
}

fn bootfsPathBytes(header: *const bootfs_format.BootFsHeader, entry: bootfs_format.BootFsEntry) ?[]const u8 {
    const path_offset = header.string_table_offset + entry.path_offset;
    const path_end = path_offset + entry.path_bytes;
    if (path_end > header.image_bytes) return null;
    const path_ptr: [*]const u8 = @ptrFromInt(@intFromPtr(header) + path_offset);
    return path_ptr[0..entry.path_bytes];
}

fn openExecFromBootFs(path: []const u8) ?rootfs_core.OpenExecResult {
    const header = bootfsArchiveHeader() orelse return null;
    const entry_ptr: [*]const bootfs_format.BootFsEntry = @ptrFromInt(@intFromPtr(header) + header.entry_table_offset);
    var entry_index: usize = 0;
    while (entry_index < header.entry_count) : (entry_index += 1) {
        const entry = entry_ptr[entry_index];
        if (entry.kind != bootfs_format.kind_regular) continue;
        const entry_path = bootfsPathBytes(header, entry) orelse continue;
        if (!std.mem.eql(u8, entry_path, path)) continue;
        const data_end = entry.data_offset + entry.data_bytes;
        if (entry.data_offset < header.data_offset or data_end > header.image_bytes) return null;
        const data_va = @intFromPtr(header) + entry.data_offset;
        const vm_token = installVmObject(data_va, entry.data_bytes, .{ .read = true });
        if (image_abi.decodeVmObjectToken(vm_token) == null) return null;
        const exec_token = installExecImage(vm_token, .{ .exec = true });
        if (image_abi.decodeExecImageToken(exec_token) == null) return null;
        return .{
            .token = exec_token,
            .file_bytes = entry.data_bytes,
        };
    }
    return null;
}

fn openSeedExec() rootfs_core.OpenExecResult {
    if (openExecFromBootFs(seed_exec_bootfs_path)) |exec| {
        _ = userLog("BootInit: bootfs seed exec ready\n");
        return exec;
    }

    const block_desc = requireBlockDeviceDescriptor("BootInit: block device descriptor missing\n");
    const block_geometry = readBlockGeometry(block_desc) orelse bootFail("BootInit: block geometry failed\n");
    if (!ensureDeviceMmioCapsInstalled(block_desc)) bootFail("BootInit: block MMIO install failed\n");
    if (!rootfs_core.init(.{
        .rootfs_start_block = persistent_fs_start_block,
        .capacity_sectors = block_geometry.capacity_sectors,
        .logical_block_size = block_geometry.logical_block_size,
        .common_page_paddr = block_desc.common_page_paddr,
        .notify_page_paddr = block_desc.notify_page_paddr,
        .isr_page_paddr = block_desc.isr_page_paddr,
        .common_page_offset = block_desc.common_page_offset,
        .notify_page_offset = block_desc.notify_page_offset,
        .isr_page_offset = block_desc.isr_page_offset,
        .notify_off_multiplier = block_desc.notify_off_multiplier,
        .queue_submit_token = block_desc.init_queue_submit_token,
        .queue_notify_token = block_desc.init_queue_notify_token,
    })) bootFail("BootInit: rootfs init failed\n");
    _ = userLog("BootInit: rootfs ready\n");
    return rootfs_core.openExec(seed_exec_path) orelse bootFail("BootInit: seed open_exec failed\n");
}

fn allocWritableBootstrapPageAt(source_va: u64, label: []const u8) u64 {
    var paddr: u64 = 0;
    if (allocMapPages(source_va, 1, true, @intFromPtr(&paddr)) != 0 or paddr < 0x1000) bootFail(label);
    return source_va;
}

fn appendManagerBootstrapPage(source_va: u64, target_va: u64, flags: u64) void {
    if (manager_bootstrap_table_storage.page_count >= process_abi.max_bootstrap_page_descriptors) {
        bootFail("BootInit: manager bootstrap table full\n");
    }
    const index: usize = manager_bootstrap_table_storage.page_count;
    manager_bootstrap_pages_storage[index] = .{
        .source_va = source_va,
        .target_va = target_va,
        .flags = flags,
    };
    manager_bootstrap_table_storage.page_descriptors[index] = manager_bootstrap_pages_storage[index];
    manager_bootstrap_table_storage.page_count += 1;
}

fn prepareManagerBootstrapHandoffPage() u64 {
    const source_va = @intFromPtr(&manager_bootstrap_handoff_page);
    manager_init_bootstrap_abi.writePendingConfigPage(source_va);
    manager_bootstrap_handoff_source_va = source_va;
    return source_va;
}

fn buildManagerBootstrapTable() *const process_abi.BootstrapDescriptorTable {
    manager_bootstrap_table_storage = .{};
    const cfg = configPage() orelse bootFail("BootInit: config page missing\n");
    const page = descriptorPage() orelse bootFail("BootInit: descriptor page missing\n");
    const manager_handoff_source_va = prepareManagerBootstrapHandoffPage();
    rebuildManagerDeviceNeedCache(page);
    _ = userLog("BootInit: manager bootstrap cache ready\n");

    appendManagerBootstrapPage(process_abi.standard_config_target_va, process_abi.standard_config_target_va, 0);
    appendManagerBootstrapPage(cfg.descriptor_page_va, cfg.descriptor_page_va, 0);
    appendManagerBootstrapPage(manager_handoff_source_va, manager_init_bootstrap_abi.config_target_va, 0);

    var spawn_index: usize = 0;
    while (spawn_index < page.spawn_page_count and spawn_index < init_bootstrap_abi.max_spawn_page_descriptors) : (spawn_index += 1) {
        const descriptor = page.spawn_pages[spawn_index];
        if ((descriptor.flags & init_bootstrap_abi.spawn_page_flag_kernel_backed) == 0) continue;
        if (!isManagerBootstrapSpawnPageNeeded(descriptor)) continue;
        const flags = if ((descriptor.flags & init_bootstrap_abi.spawn_page_flag_init_writable) != 0)
            process_abi.spawn_flag_bootstrap_page_writable
        else
            @as(u64, 0);
        appendManagerBootstrapPage(descriptor.source_va, descriptor.source_va, flags);
    }
    _ = userLog("BootInit: manager bootstrap spawn pages ready\n");

    if ((page.primary_display.flags & init_bootstrap_abi.display_flag_present) != 0 and
        page.primary_display.framebuffer_paddr != 0 and
        page.primary_display.framebuffer_size_bytes != 0)
    {
        const fb_vm_token = installVmObjectMmioRange(
            page.primary_display.framebuffer_paddr,
            page.primary_display.framebuffer_size_bytes,
            .{ .read = true, .write = true, .map = true, .grant = true },
        );
        if (image_abi.decodeVmObjectToken(fb_vm_token) == null) bootFail("BootInit: framebuffer vm install failed\n");
        const cap_index = manager_bootstrap_table_storage.cap_count;
        manager_bootstrap_table_storage.cap_count += 1;
        manager_bootstrap_table_storage.cap_descriptors[cap_index] = .{
            .source_token = fb_vm_token,
            .target_token_va = manager_init_bootstrap_abi.config_target_va +
                @offsetOf(manager_init_bootstrap_abi.ConfigPage, "framebuffer_vm_token"),
            .rights_bits = image_abi.vmObjectRightsToBits(.{ .read = true, .write = true, .map = true, .grant = true }),
            .kind = .vm_object,
        };
    }
    _ = userLog("BootInit: manager bootstrap framebuffer ready\n");
    if ((page.bootfs_archive.flags & init_bootstrap_abi.boot_archive_flag_present) != 0 and
        page.bootfs_archive.image_va != 0 and
        page.bootfs_archive.size_bytes != 0)
    {
        const bootfs_vm_token = installVmObject(
            page.bootfs_archive.image_va,
            page.bootfs_archive.size_bytes,
            .{ .read = true, .map = true, .grant = true },
        );
        if (image_abi.decodeVmObjectToken(bootfs_vm_token) == null) bootFail("BootInit: bootfs vm install failed\n");
        const cap_index = manager_bootstrap_table_storage.cap_count;
        if (cap_index >= process_abi.max_bootstrap_cap_descriptors) bootFail("BootInit: manager bootstrap cap table full\n");
        manager_bootstrap_table_storage.cap_count += 1;
        manager_bootstrap_table_storage.cap_descriptors[cap_index] = .{
            .source_token = bootfs_vm_token,
            .target_token_va = manager_init_bootstrap_abi.config_target_va +
                @offsetOf(manager_init_bootstrap_abi.ConfigPage, "bootfs_vm_token"),
            .rights_bits = image_abi.vmObjectRightsToBits(.{ .read = true, .map = true }),
            .kind = .vm_object,
        };
    }
    _ = userLog("BootInit: manager bootstrap bootfs ready\n");
    return &manager_bootstrap_table_storage;
}

fn grantManagerDeviceResources(manager_slot: u64) void {
    const page = descriptorPage() orelse bootFail("BootInit: descriptor page missing\n");
    var handoff_index: usize = 0;
    var device_index: usize = 0;
    while (device_index < page.device_count and device_index < init_bootstrap_abi.max_device_descriptors) : (device_index += 1) {
        const descriptor = page.devices[device_index];
        if (!isBootstrapDeviceDescriptorPresent(descriptor)) continue;
        if (!manager_device_needed_cache[device_index]) continue;
        logManagerGrantDeviceStep(device_index, descriptor, "begin");
        logManagerGrantDeviceStep(device_index, descriptor, "mmio ready");
        const submit_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(descriptor.init_queue_submit_token), manager_slot);
        const notify_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(descriptor.init_queue_notify_token), manager_slot);
        const submit_child = queue_abi.decodeQueueCapToken(submit_child_encoded) orelse bootFail("BootInit: manager submit grant failed\n");
        const notify_child = queue_abi.decodeQueueCapToken(notify_child_encoded) orelse bootFail("BootInit: manager notify grant failed\n");
        logManagerGrantDeviceStep(device_index, descriptor, "queue ready");
        manager_init_bootstrap_abi.writeDeviceGrant(
            manager_bootstrap_handoff_source_va,
            handoff_index,
            descriptor.device_page_paddr,
            submit_child,
            notify_child,
            manager_device_input_hint_cache[device_index],
        );
        handoff_index += 1;
    }
    manager_init_bootstrap_abi.markReady(manager_bootstrap_handoff_source_va, handoff_index);
}

fn bootMain() noreturn {
    noteBootStatus(boot_status_abi.status_init_started);
    requireBootImage(.init_app, "BootInit: boot manifest missing init image\n");
    initBootLogFramebuffer();
    emitBootBanner();
    emitKernelDebugSnapshot();
    _ = userLog("BootInit: boot manifest ok\n");

    _ = requireSpawnPageDescriptor(
        .service_config,
        .window_service,
        "BootInit: window service descriptor missing\n",
    );
    const manager_exec = openSeedExec();
    _ = userLog("BootInit: seed exec ready\n");

    const manager_bootstrap_table = buildManagerBootstrapTable();
    _ = userLog("BootInit: manager bootstrap table ready\n");

    const manager_spawned = spawnExecWithExtendedBootstrapTable(manager_exec.token, manager_bootstrap_table);
    _ = userLog("BootInit: seed spawn returned\n");
    const manager_slot = process_abi.decodeSpawnedProcessSlot(manager_spawned) orelse {
        userLogHex("BootInit: seed spawn ret=", manager_spawned);
        bootFail("BootInit: seed spawn failed\n");
    };
    _ = userLog("BootInit: manager grants begin\n");
    grantManagerDeviceResources(manager_slot);
    _ = userLog("BootInit: manager grants ready\n");
    var line_buf: [96]u8 = undefined;
    const line = std.fmt.bufPrint(&line_buf, "BootInit: seed slot={d}\n", .{manager_slot}) catch "BootInit: seed spawned\n";
    _ = userLog(line);

    while (true) asm volatile ("pause");
}

pub export fn _start() noreturn {
    if (allocMapPages(init_stack_extension_base_va, init_stack_extension_pages, true, 0) != 0) {
        bootFail("BootInit: stack extend failed\n");
    }
    bootMain();
}
