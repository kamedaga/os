/// UEFI boot services: framebuffer acquisition, memory map, boot scratch allocator,
/// disk file loading, and ExitBootServices.  All code in this file runs before or
/// during the UEFI boot phase and is safe to call after ExitBootServices completes.
const std = @import("std");
const uefi = std.os.uefi;
const boot_static = @import("main_static.zig");
const boot_images = @import("boot_images.zig");
const kernel = @import("../kernel.zig");
const kernel_vm = @import("../memory/kernel_vm.zig");
const pmm = @import("../memory/pmm.zig");
const halt = @import("../halt.zig");
const kernel_log = @import("../kernel_log.zig");

const serialWrite = kernel_log.write;

// ---------------------------------------------------------------------------
// Module-private globals
// ---------------------------------------------------------------------------

var boot_services_cache: ?*uefi.tables.BootServices = null;
// Public refs used by pmm.init() in boot/entry.zig.
pub var kernel_image_base_paddr_ref: u64 = 0;
pub var kernel_image_size_bytes_ref: usize = 0;
var uefi_mmap_buffer: [64 * 1024]u8 align(@alignOf(uefi.tables.MemoryDescriptor)) = undefined;
var uefi_exitbs_mmap_buffer: [64 * 1024]u8 align(@alignOf(uefi.tables.MemoryDescriptor)) = undefined;
// Scratch region used for ELF loading after ExitBootServices.
var post_exit_load_scratch: [8 * 1024 * 1024]u8 align(4096) = [_]u8{0} ** (8 * 1024 * 1024);
var post_exit_load_scratch_used: usize = 0;

pub fn postExitLoadScratchEndAddr() usize {
    return @intFromPtr(&post_exit_load_scratch) + post_exit_load_scratch.len;
}

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn staticStorageStart(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

fn minStaticStart(a: usize, b: usize) usize {
    return if (a < b) a else b;
}

pub fn kernelStaticStorageStartAddr() usize {
    var start = staticStorageStart(@TypeOf(boot_services_cache), &boot_services_cache);
    start = minStaticStart(start, staticStorageStart(@TypeOf(kernel_image_base_paddr_ref), &kernel_image_base_paddr_ref));
    start = minStaticStart(start, staticStorageStart(@TypeOf(kernel_image_size_bytes_ref), &kernel_image_size_bytes_ref));
    start = minStaticStart(start, staticStorageStart(@TypeOf(uefi_mmap_buffer), &uefi_mmap_buffer));
    start = minStaticStart(start, staticStorageStart(@TypeOf(uefi_exitbs_mmap_buffer), &uefi_exitbs_mmap_buffer));
    start = minStaticStart(start, staticStorageStart(@TypeOf(post_exit_load_scratch), &post_exit_load_scratch));
    start = minStaticStart(start, staticStorageStart(@TypeOf(post_exit_load_scratch_used), &post_exit_load_scratch_used));
    return start;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end = postExitLoadScratchEndAddr();
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(boot_services_cache), &boot_services_cache));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_image_base_paddr_ref), &kernel_image_base_paddr_ref));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(kernel_image_size_bytes_ref), &kernel_image_size_bytes_ref));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(uefi_mmap_buffer), &uefi_mmap_buffer));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(uefi_exitbs_mmap_buffer), &uefi_exitbs_mmap_buffer));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(post_exit_load_scratch), &post_exit_load_scratch));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(post_exit_load_scratch_used), &post_exit_load_scratch_used));
    return end;
}

// ---------------------------------------------------------------------------
// Early UEFI console output (before serial is ready)
// ---------------------------------------------------------------------------

pub fn earlyUefiWrite(msg: [*:0]const u16) void {
    const st = uefi.system_table;
    const con_out = st.con_out orelse return;
    _ = con_out.outputString(msg) catch {};
}

pub fn earlyUefiWriteAscii(msg: []const u8) void {
    var buf: [160:0]u16 = [_:0]u16{0} ** 160;
    var i: usize = 0;
    while (i < msg.len and i + 1 < buf.len) : (i += 1) {
        const ch = msg[i];
        buf[i] = if (ch == '\n') '\r' else ch;
        if (ch == '\n' and i + 2 < buf.len) {
            buf[i + 1] = '\n';
            i += 1;
        }
    }
    buf[@min(i, buf.len - 1)] = 0;
    earlyUefiWrite(&buf);
}

// ---------------------------------------------------------------------------
// Boot services lifecycle
// ---------------------------------------------------------------------------

pub fn acquireBootServicesOrHalt() *uefi.tables.BootServices {
    const bs = uefi.system_table.boot_services orelse {
        halt.haltWithMessage("boot services missing");
    };
    boot_services_cache = bs;
    return bs;
}

pub fn exitBootServicesOrHalt() void {
    if (boot_static.debug_skip_exit_boot_services) {
        return;
    } else {
        switch (exitBootServicesWithRetry()) {
            .success => {},
            .failed => halt.haltWithMessage("ExitBootServices failed"),
        }
    }
    boot_services_cache = null;
}

const ExitBootResult = enum { success, failed };

fn exitBootServicesWithRetry() ExitBootResult {
    const st = uefi.system_table;
    const bs = st.boot_services orelse return .failed;

    var attempt: usize = 0;
    while (attempt < 8) : (attempt += 1) {
        const mmap = bs.getMemoryMap(uefi_exitbs_mmap_buffer[0..]) catch return .failed;
        bs.exitBootServices(uefi.handle, mmap.info.key) catch |err| switch (err) {
            error.InvalidParameter => continue, // map key 競合、再取得で回復可能
            else => return .failed,
        };

        // UEFI仕様: ExitBootServices 後に該当ポインタを null 化し、CRC を再計算する。
        st.console_in_handle = null;
        st.con_in = null;
        st.console_out_handle = null;
        st.con_out = null;
        st.standard_error_handle = null;
        st.std_err = null;
        st.boot_services = null;
        st.hdr.crc32 = 0;
        const st_bytes = @as([*]u8, @ptrCast(st))[0..@as(usize, st.hdr.header_size)];
        st.hdr.crc32 = std.hash.Crc32.hash(st_bytes);

        return .success;
    }
    return .failed;
}

// ---------------------------------------------------------------------------
// Kernel image range capture
// ---------------------------------------------------------------------------

pub fn captureKernelImageRange(bs: *uefi.tables.BootServices) void {
    const loaded_image = (bs.handleProtocol(uefi.protocol.LoadedImage, uefi.handle) catch return) orelse return;
    kernel_image_base_paddr_ref = @intFromPtr(loaded_image.image_base);
    kernel_image_size_bytes_ref = @intCast(loaded_image.image_size);
}

pub fn kernelImageBaseAddr() u64 {
    return kernel_image_base_paddr_ref;
}

pub fn kernelImageSizeBytes() usize {
    return kernel_image_size_bytes_ref;
}

// ---------------------------------------------------------------------------
// Memory statistics collection
// ---------------------------------------------------------------------------

pub fn collectBootMemoryStatsOrHalt(
    bs: *uefi.tables.BootServices,
    free_list: *kernel.FreePageList,
    user_spaces: []const boot_static.UserAddressSpace,
) boot_static.MemoryStats {
    const user_spaces_start = if (user_spaces.len == 0) 0 else kernel_vm.pageAlignDown(@intFromPtr(user_spaces.ptr));
    const user_spaces_end = if (user_spaces.len == 0) 0 else kernel_vm.pageAlignUp(@intFromPtr(user_spaces.ptr) + (user_spaces.len * @sizeOf(boot_static.UserAddressSpace)));
    return pmm.collectMemoryStatsAndFreePages(bs, free_list, uefi_mmap_buffer[0..], user_spaces_start, user_spaces_end) orelse {
        halt.haltWithMessage("memory map parse failed");
    };
}

// ---------------------------------------------------------------------------
// Framebuffer
// ---------------------------------------------------------------------------

pub const FramebufferInfo = struct {
    paddr: u64,
    size_bytes: usize,
    width: u32,
    height: u32,
    pixels_per_scan_line: u32,
    pixel_format: u32,
    mode: u32,
};

pub fn acquireFramebufferInfo(bs: *uefi.tables.BootServices) ?FramebufferInfo {
    const gop = bs.locateProtocol(uefi.protocol.GraphicsOutput, null) catch return null;
    const graphics = gop orelse return null;

    const chosen_mode = selectFramebufferMode(graphics, boot_static.framebuffer_window_bytes) orelse graphics.mode.mode;
    if (chosen_mode != graphics.mode.mode) {
        graphics.setMode(chosen_mode) catch return null;
    }

    const mode = graphics.mode;
    const info = mode.info;
    if (info.pixel_format == .blt_only) return null;

    const size_bytes_u64 = framebufferBytesForModeInfo(info);
    if (size_bytes_u64 == 0 or size_bytes_u64 > boot_static.framebuffer_window_bytes) return null;
    if (mode.frame_buffer_base >= boot_static.four_gib) return null;
    if ((mode.frame_buffer_base & 0xFFF) != 0) return null;

    return .{
        .paddr = mode.frame_buffer_base,
        .size_bytes = @intCast(size_bytes_u64),
        .width = info.horizontal_resolution,
        .height = info.vertical_resolution,
        .pixels_per_scan_line = info.pixels_per_scan_line,
        .pixel_format = @intFromEnum(info.pixel_format),
        .mode = mode.mode,
    };
}

fn framebufferBytesForModeInfo(info: *const uefi.protocol.GraphicsOutput.Mode.Info) u64 {
    return @as(u64, info.pixels_per_scan_line) * @as(u64, info.vertical_resolution) * 4;
}

fn selectFramebufferMode(gop: *uefi.protocol.GraphicsOutput, max_bytes: u64) ?u32 {
    var best_mode: ?u32 = null;
    var best_bytes: u64 = 0;
    var mode_id: u32 = 0;
    while (mode_id < gop.mode.max_mode) : (mode_id += 1) {
        const info = gop.queryMode(mode_id) catch continue;
        if (info.pixel_format == .blt_only) continue;
        const mode_bytes = framebufferBytesForModeInfo(info);
        if (mode_bytes > max_bytes) continue;
        if (mode_bytes >= best_bytes) {
            best_bytes = mode_bytes;
            best_mode = mode_id;
        }
    }
    return best_mode;
}

// ---------------------------------------------------------------------------
// Boot scratch allocator
// Used for ELF load staging buffers both before and after ExitBootServices.
// ---------------------------------------------------------------------------

pub fn allocateBootScratch(bytes: usize) ?[]align(8) u8 {
    if (boot_services_cache) |bs| {
        return bs.allocatePool(.loader_data, bytes) catch null;
    }
    const aligned_bytes = std.mem.alignForward(usize, bytes, 8);
    const start = std.mem.alignForward(usize, post_exit_load_scratch_used, 8);
    if (aligned_bytes > post_exit_load_scratch.len - start) return null;
    post_exit_load_scratch_used = start + aligned_bytes;
    const ptr: [*]align(8) u8 = @ptrCast(@alignCast(&post_exit_load_scratch[start]));
    return ptr[0..bytes];
}

pub fn freeBootScratch(buf: []align(8) u8) void {
    if (boot_services_cache) |bs| {
        bs.freePool(buf.ptr) catch {};
        return;
    }
    const base = @intFromPtr(&post_exit_load_scratch[0]);
    const limit = base + post_exit_load_scratch.len;
    const ptr = @intFromPtr(buf.ptr);
    if (ptr < base or ptr > limit) return;
    const offset = ptr - base;
    const aligned_bytes = std.mem.alignForward(usize, buf.len, 8);
    if (offset + aligned_bytes == post_exit_load_scratch_used) {
        post_exit_load_scratch_used = offset;
    }
}

// ---------------------------------------------------------------------------
// Disk file loading
// ---------------------------------------------------------------------------

pub fn loadFileFromDisk(bs: *uefi.tables.BootServices, path: [*:0]const u16) ?[]const u8 {
    const loaded_image = (bs.handleProtocol(uefi.protocol.LoadedImage, uefi.handle) catch return null) orelse return null;
    const device_handle = loaded_image.device_handle orelse return null;
    const fs = (bs.handleProtocol(uefi.protocol.SimpleFileSystem, device_handle) catch return null) orelse return null;

    var root = fs.openVolume() catch return null;
    defer root.close() catch {};

    var user_file = root.open(path, .read, .{}) catch return null;
    defer user_file.close() catch {};

    var info_buffer: [512]u8 align(8) = undefined;
    const info = user_file.getInfo(.file, info_buffer[0..]) catch return null;
    const read_len: usize = @intCast(info.file_size);
    if (read_len == 0) return null;

    const staging = bs.allocatePool(.loader_data, read_len) catch return null;
    errdefer bs.freePool(staging.ptr) catch {};

    const read_bytes = user_file.read(staging[0..read_len]) catch return null;
    if (read_bytes != read_len) return null;
    return staging[0..read_len];
}

pub fn loadBootDiskFile(bs: *uefi.tables.BootServices, file: boot_images.DiskFile) ?[]const u8 {
    return loadFileFromDisk(bs, file.uefi_path);
}
