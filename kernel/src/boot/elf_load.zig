/// ELF loading into user process pages.
/// Uses uefi_services for scratch allocation, so safe to call both before and
/// after ExitBootServices.
const std = @import("std");
const kernel = @import("../kernel.zig");
const boot_static = @import("main_static.zig");
const elf_loader = @import("../elf_loader.zig");
const user_vm = @import("../memory/user_vm.zig");
const kernel_vm = @import("../memory/kernel_vm.zig");
const uefi_services = @import("uefi_services.zig");
const log_util = @import("../log_util.zig");

// ---------------------------------------------------------------------------
// Single-page ELF load (for simple programs that fit in one page)
// ---------------------------------------------------------------------------

pub fn loadUserElfIntoUserPage(user_page_paddr: u64, image_bytes: []const u8) ?elf_loader.Image {
    if ((user_page_paddr & 0xFFF) != 0) return null;
    const page: [*]u8 = @ptrFromInt(user_page_paddr);
    return elf_loader.loadToSinglePage(image_bytes, boot_static.user_elf_base_va, page[0..4096]) catch null;
}

pub fn loadUserElfIntoUserPageOrHalt(user_page_paddr: u64, image_bytes: []const u8, fail_message: []const u8) elf_loader.Image {
    return loadUserElfIntoUserPage(user_page_paddr, image_bytes) orelse {
        @import("../kernel_log.zig").write(fail_message);
        @import("../halt.zig").haltLoop();
    };
}

// ---------------------------------------------------------------------------
// Multi-page ELF load (allocates extra pages as needed from the free list)
// ---------------------------------------------------------------------------

pub fn loadUserElfIntoProcessPages(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    page0_paddr: u64,
    page1_paddr: u64,
    image_bytes: []const u8,
    free_list: *kernel.FreePageList,
) ?elf_loader.Image {
    _ = page1_paddr;
    if ((page0_paddr & 0xFFF) != 0) {
        log_util.logMessage("loadUserElfIntoProcessPages: unaligned base pages");
        return null;
    }
    const required_bytes = computeUserElfRequiredBytes(image_bytes) orelse {
        log_util.logMessage("loadUserElfIntoProcessPages: required_bytes failed");
        return null;
    };
    if (required_bytes > boot_static.user_program_max_load_bytes) {
        log_util.logRequiredMax("loadUserElfIntoProcessPages: image too large required=", required_bytes, boot_static.user_program_max_load_bytes);
        return null;
    }

    const load_window = uefi_services.allocateBootScratch(required_bytes) orelse {
        log_util.logRequiredBytes("loadUserElfIntoProcessPages: scratch alloc failed bytes=", required_bytes);
        return null;
    };
    defer uefi_services.freeBootScratch(load_window);

    @memset(load_window[0..required_bytes], 0);
    const loaded = elf_loader.loadToSinglePage(image_bytes, boot_static.user_elf_base_va, load_window[0..required_bytes]) catch |err| {
        log_util.logError("loadUserElfIntoProcessPages: loadToSinglePage failed: ", err);
        return null;
    };

    const required_pages = required_bytes / 4096;
    const page0: [*]u8 = @ptrFromInt(page0_paddr);
    @memcpy(page0[0..4096], load_window[0..4096]);

    var page_index: usize = 1;
    while (page_index < required_pages) : (page_index += 1) {
        const extra_page = state.allocPageTo(principal, free_list) catch |err| {
            log_util.logIndexedError("loadUserElfIntoProcessPages: allocPageTo failed idx=", page_index, err);
            return null;
        };
        const map_va = boot_static.user_va + (@as(u64, @intCast(page_index)) * 4096);
        if (!user_vm.mapUserLinearRegion(principal, map_va, extra_page.paddr, 4096, true)) {
            log_util.logIndexedMapFailure("loadUserElfIntoProcessPages: map failed idx=", page_index, map_va, extra_page.paddr);
            return null;
        }
        const page_bytes: [*]u8 = @ptrFromInt(extra_page.paddr);
        const off = page_index * 4096;
        @memcpy(page_bytes[0..4096], load_window[off .. off + 4096]);
    }

    return loaded;
}

pub fn loadUserElfIntoProcessPagesOrHalt(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    page0_paddr: u64,
    page1_paddr: u64,
    image_bytes: []const u8,
    fail_message: []const u8,
    free_list: *kernel.FreePageList,
) elf_loader.Image {
    return loadUserElfIntoProcessPages(state, principal, page0_paddr, page1_paddr, image_bytes, free_list) orelse {
        @import("../kernel_log.zig").write(fail_message);
        @import("../halt.zig").haltLoop();
    };
}

// ---------------------------------------------------------------------------
// Streaming ELF load (from ImageBacking, for deferred compositor / spawn exec)
// ---------------------------------------------------------------------------

const ImageBackingReaderContext = struct {
    backing: kernel.ImageBacking,
};

fn readImageBackingAt(context: *anyopaque, offset: u64, out: []u8) elf_loader.StreamReadError!void {
    const ctx: *const ImageBackingReaderContext = @ptrCast(@alignCast(context));
    if (offset > ctx.backing.size_bytes) return error.SourceOutOfRange;
    if (@as(u64, @intCast(out.len)) > ctx.backing.size_bytes - offset) return error.SourceOutOfRange;

    var copied: usize = 0;
    const start_absolute = @as(u64, ctx.backing.page_offset_bytes) + offset;
    while (copied < out.len) {
        const absolute = start_absolute + @as(u64, @intCast(copied));
        const page_index: usize = @intCast(absolute / 4096);
        if (page_index >= ctx.backing.page_count) return error.SourceOutOfRange;
        const page_offset: usize = @intCast(absolute % 4096);
        const page_paddr = ctx.backing.page_paddrs[page_index];
        if ((page_paddr & 0xFFF) != 0) return error.SourceOutOfRange;
        const page: [*]const u8 = @ptrFromInt(page_paddr);
        const available = 4096 - page_offset;
        const remaining = out.len - copied;
        const chunk_len: usize = if (remaining < available) remaining else available;
        @memcpy(out[copied .. copied + chunk_len], page[page_offset .. page_offset + chunk_len]);
        copied += chunk_len;
    }
}

fn checkedAddU64(a: u64, b: u64) elf_loader.LoadToPageError!u64 {
    const sum, const overflow = @addWithOverflow(a, b);
    if (overflow != 0) return error.AddressOverflow;
    return sum;
}

const ProcessImagePages = struct {
    scratch: []align(8) u8,
    page_paddrs: []u64,
};

fn allocProcessImagePages(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    page0_paddr: u64,
    required_pages: usize,
    free_list: *kernel.FreePageList,
) ?ProcessImagePages {
    if (required_pages == 0) return null;
    const scratch = uefi_services.allocateBootScratch(required_pages * @sizeOf(u64)) orelse {
        log_util.logRequiredBytes("loadUserElfIntoProcessPagesFromBacking: page table alloc failed bytes=", required_pages * @sizeOf(u64));
        return null;
    };
    const page_paddrs = std.mem.bytesAsSlice(u64, scratch);
    page_paddrs[0] = page0_paddr;

    var page_index: usize = 1;
    while (page_index < required_pages) : (page_index += 1) {
        const extra_page = state.allocPageTo(principal, free_list) catch |err| {
            log_util.logIndexedError("loadUserElfIntoProcessPagesFromBacking: allocPageTo failed idx=", page_index, err);
            uefi_services.freeBootScratch(scratch);
            return null;
        };
        const map_va = boot_static.user_va + (@as(u64, @intCast(page_index)) * 4096);
        if (!user_vm.mapUserLinearRegion(principal, map_va, extra_page.paddr, 4096, true)) {
            log_util.logIndexedMapFailure("loadUserElfIntoProcessPagesFromBacking: map failed idx=", page_index, map_va, extra_page.paddr);
            uefi_services.freeBootScratch(scratch);
            return null;
        }
        page_paddrs[page_index] = extra_page.paddr;
    }

    return .{
        .scratch = scratch,
        .page_paddrs = page_paddrs[0..required_pages],
    };
}

fn zeroProcessImagePages(page_paddrs: []const u64) void {
    for (page_paddrs) |page_paddr| {
        const page: [*]u8 = @ptrFromInt(page_paddr);
        @memset(page[0..4096], 0);
    }
}

fn readProcessImageBytes(page_paddrs: []const u64, image_bytes: usize, offset: u64, out: []u8) elf_loader.LoadToPageError!void {
    if (offset > std.math.maxInt(usize)) return error.SegmentTooLarge;
    const start: usize = @intCast(offset);
    if (start > image_bytes or out.len > image_bytes - start) return error.SegmentOutsideMappedRange;

    var copied: usize = 0;
    while (copied < out.len) {
        const absolute = start + copied;
        const page_index = absolute / 4096;
        if (page_index >= page_paddrs.len) return error.SegmentOutsideMappedRange;
        const page_offset = absolute % 4096;
        const remaining = out.len - copied;
        const step: usize = @min(remaining, 4096 - page_offset);
        const page: [*]const u8 = @ptrFromInt(page_paddrs[page_index]);
        @memcpy(out[copied .. copied + step], page[page_offset .. page_offset + step]);
        copied += step;
    }
}

fn writeProcessImageBytes(page_paddrs: []const u64, image_bytes: usize, offset: u64, bytes: []const u8) elf_loader.LoadToPageError!void {
    if (offset > std.math.maxInt(usize)) return error.RelocationOutOfRange;
    const start: usize = @intCast(offset);
    if (start > image_bytes or bytes.len > image_bytes - start) return error.RelocationOutOfRange;

    var copied: usize = 0;
    while (copied < bytes.len) {
        const absolute = start + copied;
        const page_index = absolute / 4096;
        if (page_index >= page_paddrs.len) return error.RelocationOutOfRange;
        const page_offset = absolute % 4096;
        const remaining = bytes.len - copied;
        const step: usize = @min(remaining, 4096 - page_offset);
        const page: [*]u8 = @ptrFromInt(page_paddrs[page_index]);
        @memcpy(page[page_offset .. page_offset + step], bytes[copied .. copied + step]);
        copied += step;
    }
}

fn readU64Le(bytes: []const u8) u64 {
    var value: u64 = 0;
    var i: usize = 0;
    while (i < 8) : (i += 1) value |= @as(u64, bytes[i]) << @intCast(i * 8);
    return value;
}

fn writeU64Le(bytes: []u8, value: u64) void {
    var i: usize = 0;
    while (i < 8) : (i += 1) bytes[i] = @intCast((value >> @intCast(i * 8)) & 0xFF);
}

const ProcessImageRelocationAccess = struct {
    page_paddrs: []const u64,
    image_bytes: usize,
};

fn readProcessImageU64At(context: *anyopaque, offset: u64) elf_loader.LoadToPageError!u64 {
    const access: *const ProcessImageRelocationAccess = @ptrCast(@alignCast(context));
    var buf: [8]u8 = undefined;
    try readProcessImageBytes(access.page_paddrs, access.image_bytes, offset, buf[0..]);
    return readU64Le(buf[0..]);
}

fn writeProcessImageU64At(context: *anyopaque, offset: u64, value: u64) elf_loader.LoadToPageError!void {
    const access: *const ProcessImageRelocationAccess = @ptrCast(@alignCast(context));
    var buf: [8]u8 = undefined;
    writeU64Le(buf[0..], value);
    try writeProcessImageBytes(access.page_paddrs, access.image_bytes, offset, buf[0..]);
}

fn copyReaderIntoProcessImage(
    reader: elf_loader.Reader,
    page_paddrs: []const u64,
    image_bytes: usize,
    src_offset: u64,
    dest_offset: u64,
    byte_count: u64,
) elf_loader.StreamLoadToPageError!void {
    if (byte_count > std.math.maxInt(usize)) return error.SegmentTooLarge;
    const total: usize = @intCast(byte_count);

    var copied: usize = 0;
    while (copied < total) {
        const dst_u64 = try checkedAddU64(dest_offset, @as(u64, @intCast(copied)));
        if (dst_u64 > std.math.maxInt(usize)) return error.SegmentTooLarge;
        const dst: usize = @intCast(dst_u64);
        if (dst > image_bytes) return error.SegmentOutsideMappedRange;
        const page_index = dst / 4096;
        if (page_index >= page_paddrs.len) return error.SegmentOutsideMappedRange;
        const page_offset = dst % 4096;
        const remaining = total - copied;
        const in_page = 4096 - page_offset;
        const in_image = image_bytes - dst;
        const step: usize = @min(remaining, @min(in_page, in_image));
        if (step == 0) return error.SegmentOutsideMappedRange;
        const src = try checkedAddU64(src_offset, @as(u64, @intCast(copied)));
        const page: [*]u8 = @ptrFromInt(page_paddrs[page_index]);
        reader.read_at(reader.context, src, page[page_offset .. page_offset + step]) catch |err| switch (err) {
            error.SourceOutOfRange => return error.SegmentOutOfRange,
            error.SourceReadFailed => return err,
        };
        copied += step;
    }
}

pub fn loadUserElfIntoProcessPagesFromBacking(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    page0_paddr: u64,
    page1_paddr: u64,
    backing: kernel.ImageBacking,
    free_list: *kernel.FreePageList,
) ?elf_loader.Image {
    _ = page1_paddr;
    if ((page0_paddr & 0xFFF) != 0) {
        log_util.logMessage("loadUserElfIntoProcessPagesFromBacking: unaligned base pages");
        return null;
    }

    var reader_ctx = ImageBackingReaderContext{ .backing = backing };
    const reader = elf_loader.Reader{
        .context = @ptrCast(&reader_ctx),
        .read_at = readImageBackingAt,
    };
    const parsed = elf_loader.parseStreaming(reader) catch |err| {
        log_util.logError("loadUserElfIntoProcessPagesFromBacking: parse failed: ", err);
        return null;
    };
    const required_bytes = computeUserElfRequiredBytesFromParsed(parsed) orelse {
        log_util.logMessage("loadUserElfIntoProcessPagesFromBacking: required_bytes failed");
        return null;
    };
    if (required_bytes > boot_static.user_program_max_load_bytes) {
        log_util.logRequiredMax("loadUserElfIntoProcessPagesFromBacking: image too large required=", required_bytes, boot_static.user_program_max_load_bytes);
        return null;
    }

    const required_pages = required_bytes / 4096;
    const process_pages = allocProcessImagePages(state, principal, page0_paddr, required_pages, free_list) orelse return null;
    defer uefi_services.freeBootScratch(process_pages.scratch);
    zeroProcessImagePages(process_pages.page_paddrs);

    var loaded = parsed;
    loaded.entry = checkedAddU64(boot_static.user_elf_base_va, parsed.entry) catch |err| {
        log_util.logError("loadUserElfIntoProcessPagesFromBacking: entry overflow: ", err);
        return null;
    };

    var entry_in_segment = false;
    var segment_index: usize = 0;
    while (segment_index < parsed.load_segment_len) : (segment_index += 1) {
        const seg = parsed.load_segments[segment_index];
        const runtime_start = checkedAddU64(boot_static.user_elf_base_va, seg.vaddr) catch |err| {
            log_util.logError("loadUserElfIntoProcessPagesFromBacking: segment start overflow: ", err);
            return null;
        };
        const runtime_end = checkedAddU64(runtime_start, seg.mem_size) catch |err| {
            log_util.logError("loadUserElfIntoProcessPagesFromBacking: segment end overflow: ", err);
            return null;
        };
        if (seg.file_size > std.math.maxInt(usize) or seg.mem_size > std.math.maxInt(usize)) {
            log_util.logMessage("loadUserElfIntoProcessPagesFromBacking: segment too large");
            return null;
        }
        copyReaderIntoProcessImage(reader, process_pages.page_paddrs, required_bytes, seg.file_offset, seg.vaddr, seg.file_size) catch |err| {
            log_util.logError("loadUserElfIntoProcessPagesFromBacking: segment load failed: ", err);
            return null;
        };
        if (loaded.entry >= runtime_start and loaded.entry < runtime_end) entry_in_segment = true;
    }

    var relocation_access = ProcessImageRelocationAccess{
        .page_paddrs = process_pages.page_paddrs,
        .image_bytes = required_bytes,
    };
    elf_loader.applyRelativeRelocationsWithAccess(parsed, boot_static.user_elf_base_va, .{
        .context = @ptrCast(&relocation_access),
        .max_len = required_bytes,
        .read_u64_at = readProcessImageU64At,
        .write_u64_at = writeProcessImageU64At,
    }) catch |err| {
        log_util.logError("loadUserElfIntoProcessPagesFromBacking: relocation failed: ", err);
        return null;
    };
    if (!entry_in_segment) {
        log_util.logMessage("loadUserElfIntoProcessPagesFromBacking: entry outside mapped range");
        return null;
    }

    return loaded;
}

pub fn copyImageBackingBytes(backing: kernel.ImageBacking, dest: []u8) bool {
    if (dest.len < backing.size_bytes) return false;
    if (backing.page_count == 0 or backing.page_count > kernel.max_image_backing_pages) return false;

    var copied: usize = 0;
    var page_index: usize = 0;
    var page_offset: usize = backing.page_offset_bytes;
    while (copied < backing.size_bytes and page_index < backing.page_count) : (page_index += 1) {
        const page_paddr = backing.page_paddrs[page_index];
        if ((page_paddr & 0xFFF) != 0) return false;
        const page: [*]const u8 = @ptrFromInt(page_paddr);
        const page_available = 4096 - page_offset;
        const remaining = @as(usize, @intCast(backing.size_bytes)) - copied;
        const chunk_len: usize = if (remaining < page_available) remaining else page_available;
        @memcpy(dest[copied .. copied + chunk_len], page[page_offset .. page_offset + chunk_len]);
        copied += chunk_len;
        page_offset = 0;
    }
    return copied == backing.size_bytes;
}

pub fn copyImageBackingToBootScratch(backing: kernel.ImageBacking) ?[]align(8) u8 {
    if (backing.size_bytes == 0) return null;
    const buf = uefi_services.allocateBootScratch(backing.size_bytes) orelse return null;
    if (!copyImageBackingBytes(backing, buf[0..backing.size_bytes])) {
        uefi_services.freeBootScratch(buf);
        return null;
    }
    return buf;
}

// ---------------------------------------------------------------------------
// ELF size computation
// ---------------------------------------------------------------------------

pub fn computeUserElfRequiredBytesFromParsed(parsed: elf_loader.Image) ?usize {
    var max_end: u64 = 0;
    var i: usize = 0;
    while (i < parsed.load_segment_len) : (i += 1) {
        const seg = parsed.load_segments[i];
        const seg_end, const overflow = @addWithOverflow(seg.vaddr, seg.mem_size);
        if (overflow != 0) return null;
        if (seg_end > max_end) max_end = seg_end;
    }
    if (max_end == 0) max_end = 4096;
    const aligned_end = kernel_vm.pageAlignUp(max_end);
    if (aligned_end > @import("std").math.maxInt(usize)) return null;
    return @intCast(aligned_end);
}

pub fn computeUserElfRequiredBytes(image_bytes: []const u8) ?usize {
    const parsed = elf_loader.parse(image_bytes) catch return null;
    return computeUserElfRequiredBytesFromParsed(parsed);
}
