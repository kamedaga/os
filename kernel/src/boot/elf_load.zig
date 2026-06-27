/// ELF loading into user process pages.
/// Uses boot_scratch for load staging, so boot protocols can provide the
/// backing storage independently.
const std = @import("std");
const kernel = @import("../kernel.zig");
const boot_static = @import("main_static.zig");
const elf_loader = @import("../elf_loader.zig");
const user_vm = @import("../memory/user_vm.zig");
const kernel_vm = @import("../memory/kernel_vm.zig");
const boot_scratch = @import("boot_scratch.zig");
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

    const load_window = boot_scratch.allocate(required_bytes) orelse {
        log_util.logRequiredBytes("loadUserElfIntoProcessPages: scratch alloc failed bytes=", required_bytes);
        return null;
    };
    defer boot_scratch.free(load_window);

    @memset(load_window[0..required_bytes], 0);
    const loaded = elf_loader.loadToSinglePage(image_bytes, boot_static.user_elf_base_va, load_window[0..required_bytes]) catch |err| {
        log_util.logError("loadUserElfIntoProcessPages: loadToSinglePage failed: ", err);
        return null;
    };

    const required_pages = required_bytes / 4096;
    const page0: [*]u8 = @ptrFromInt(page0_paddr);
    @memcpy(page0[0..4096], load_window[0..4096]);

    var extra_vmo_fd: ?kernel.Fd = null;
    var extra_vmo_ref: kernel.NativeVmoRef = .{};
    if (required_pages > 1) {
        const extra_size = @as(u64, @intCast(required_pages - 1)) * kernel.native_page_size;
        const fd = state.createAnonymousVmoFd(
            principal,
            extra_size,
            .{
                .map_read = true,
                .map_write = true,
                .map_exec = true,
                .close = true,
            },
            .{ .private = true },
            0,
        ) catch |err| {
            log_util.logError("loadUserElfIntoProcessPages: create extra VMO failed: ", err);
            return null;
        };
        extra_vmo_fd = fd;
        extra_vmo_ref = state.nativeVmoRefForFd(principal, fd) orelse {
            log_util.logMessage("loadUserElfIntoProcessPages: extra VMO fd missing");
            _ = state.closeFdWithFreeList(principal, fd, free_list) catch {};
            return null;
        };
    }

    var page_index: usize = 1;
    while (page_index < required_pages) : (page_index += 1) {
        const extra_page = state.allocPhysicalPage(free_list) catch |err| {
            log_util.logIndexedError("loadUserElfIntoProcessPages: allocPhysicalPage failed idx=", page_index, err);
            if (extra_vmo_fd) |fd| _ = state.closeFdWithFreeList(principal, fd, free_list) catch {};
            return null;
        };
        const map_va = boot_static.user_va + (@as(u64, @intCast(page_index)) * 4096);
        if (!user_vm.mapUserLinearRegion(principal, map_va, extra_page.paddr, 4096, true)) {
            log_util.logIndexedMapFailure("loadUserElfIntoProcessPages: map failed idx=", page_index, map_va, extra_page.paddr);
            if (extra_vmo_fd) |fd| _ = state.closeFdWithFreeList(principal, fd, free_list) catch {};
            return null;
        }
        var paddrs = [_]u64{extra_page.paddr};
        state.installNativeVmoPages(extra_vmo_ref, page_index - 1, paddrs[0..]) catch |err| {
            log_util.logIndexedError("loadUserElfIntoProcessPages: install extra VMO page failed idx=", page_index, err);
            if (extra_vmo_fd) |fd| _ = state.closeFdWithFreeList(principal, fd, free_list) catch {};
            return null;
        };
        const page_bytes: [*]u8 = @ptrFromInt(extra_page.paddr);
        const off = page_index * 4096;
        @memcpy(page_bytes[0..4096], load_window[off .. off + 4096]);
    }

    if (extra_vmo_fd) |fd| {
        const extra_size = @as(u64, @intCast(required_pages - 1)) * kernel.native_page_size;
        _ = state.mmapFd(
            principal,
            fd,
            boot_static.user_va + kernel.native_page_size,
            extra_size,
            .{ .read = true, .write = true, .exec = true },
            .{ .anonymous = true, .private = true, .fixed = true },
            0,
        ) catch |err| {
            log_util.logError("loadUserElfIntoProcessPages: track extra native VMA failed: ", err);
            _ = state.closeFdWithFreeList(principal, fd, free_list) catch {};
            return null;
        };
        state.closeFdWithFreeList(principal, fd, free_list) catch |err| {
            log_util.logError("loadUserElfIntoProcessPages: close extra native VMO fd failed: ", err);
            return null;
        };
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
