const std = @import("std");
const kernel_api = @import("kernel_boot_api");
const boot_resources = kernel_api.boot_resources;
const entry = kernel_api.entry;
const halt = kernel_api.halt;
const kernel = kernel_api.kernel;
const pmm = kernel_api.pmm;
const kernel_vm = kernel_api.kernel_vm;
const boot_static = kernel_api.boot_static;
const limine = @import("protocol.zig");
const image_range = kernel_api.image_range;

pub const Requests = struct {
    hhdm: *limine.HhdmRequest,
    framebuffer: *limine.FramebufferRequest,
    memmap: *limine.MemmapRequest,
    module: *limine.ModuleRequest,
    executable_address: *limine.ExecutableAddressRequest,
};

fn cstrEq(value: [*:0]const u8, expected: []const u8) bool {
    return std.mem.eql(u8, std.mem.span(value), expected);
}

fn hhdmOffsetOrHalt(requests: Requests) u64 {
    const response = requests.hhdm.response orelse {
        halt.haltWithMessage("Limine HHDM response missing");
    };
    return response.offset;
}

fn hhdmPtrToPaddr(requests: Requests, ptr: ?*anyopaque) u64 {
    const raw = @intFromPtr(ptr orelse {
        halt.haltWithMessage("Limine null HHDM pointer");
    });
    const hhdm_offset = hhdmOffsetOrHalt(requests);
    if (raw < hhdm_offset) {
        halt.haltWithMessage("Limine pointer below HHDM");
    }
    return raw - hhdm_offset;
}

pub fn captureExecutableRangeOrHalt(requests: Requests) void {
    const response = requests.executable_address.response orelse {
        halt.haltWithMessage("Limine executable address response missing");
    };
    image_range.base_paddr = response.physical_base;
    image_range.virtual_base = response.virtual_base;
    image_range.size_bytes = 0;
    const memmap_response = requests.memmap.response orelse return;
    var i: u64 = 0;
    while (i < memmap_response.entry_count) : (i += 1) {
        const item = memmap_response.entries[@intCast(i)] orelse continue;
        if (item.kind != .executable_and_modules) continue;
        if (response.physical_base < item.base or response.physical_base >= item.base + item.length) continue;
        const offset = response.physical_base - item.base;
        image_range.size_bytes = @intCast(item.length - offset);
        return;
    }
}

pub fn framebufferInfoOrHalt(requests: Requests) boot_resources.FramebufferInfo {
    const response = requests.framebuffer.response orelse {
        halt.haltWithMessage("Limine framebuffer response missing");
    };
    if (response.framebuffer_count == 0) {
        halt.haltWithMessage("Limine framebuffer unavailable");
    }
    const framebuffer = response.framebuffers[0] orelse {
        halt.haltWithMessage("Limine primary framebuffer missing");
    };
    if (framebuffer.memory_model != limine.framebuffer_rgb or framebuffer.bpp != 32) {
        halt.haltWithMessage("Limine framebuffer mode unsupported");
    }
    const paddr = hhdmPtrToPaddr(requests, framebuffer.address);
    const size_bytes_u64 = framebuffer.pitch * framebuffer.height;
    if (size_bytes_u64 == 0 or size_bytes_u64 > boot_static.framebuffer_window_bytes) {
        halt.haltWithMessage("Limine framebuffer too large");
    }
    if (paddr >= boot_static.four_gib or (paddr & 0xFFF) != 0) {
        halt.haltWithMessage("Limine framebuffer physical address unsupported");
    }
    return .{
        .paddr = paddr,
        .size_bytes = @intCast(size_bytes_u64),
        .width = @intCast(framebuffer.width),
        .height = @intCast(framebuffer.height),
        .pixels_per_scan_line = @intCast(framebuffer.pitch / 4),
        .pixel_format = 1,
        .mode = 0,
    };
}

fn moduleFileByTagOrHalt(requests: Requests, tag: []const u8) *limine.File {
    const response = requests.module.response orelse {
        halt.haltWithMessage("Limine module response missing");
    };
    var i: u64 = 0;
    while (i < response.module_count) : (i += 1) {
        const file = response.modules[@intCast(i)] orelse continue;
        if (cstrEq(file.string, tag)) return file;
    }
    halt.haltWithMessage("Limine required module missing");
}

pub fn moduleBytesOrHalt(requests: Requests, tag: []const u8) []const u8 {
    const file = moduleFileByTagOrHalt(requests, tag);
    const ptr: [*]const u8 = @ptrCast(file.address orelse {
        halt.haltWithMessage("Limine module address missing");
    });
    return ptr[0..@intCast(file.size)];
}

fn pmmRegionKind(kind: limine.MemmapEntryKind) pmm.MemoryRegionKind {
    return switch (kind) {
        .usable => .usable,
        .reserved => .reserved,
        .acpi_reclaimable => .acpi_reclaimable,
        .acpi_nvs => .acpi_nvs,
        .bad_memory => .bad_memory,
        .bootloader_reclaimable => .bootloader_reclaimable,
        .executable_and_modules => .executable_and_modules,
        .framebuffer => .framebuffer,
    };
}

pub fn collectMemoryStatsOrHalt(
    requests: Requests,
    free_list: *kernel.FreePageList,
    user_spaces: []const boot_static.UserAddressSpace,
) boot_static.MemoryStats {
    const response = requests.memmap.response orelse {
        halt.haltWithMessage("Limine memory map response missing");
    };
    const user_spaces_start = if (user_spaces.len == 0) 0 else kernel_vm.pageAlignDown(@intFromPtr(user_spaces.ptr));
    const user_spaces_end = if (user_spaces.len == 0) 0 else kernel_vm.pageAlignUp(@intFromPtr(user_spaces.ptr) + (user_spaces.len * @sizeOf(boot_static.UserAddressSpace)));
    var collector = pmm.MemoryCollector.init(free_list, user_spaces_start, user_spaces_end) orelse {
        halt.haltWithMessage("Limine memory map parse failed");
    };
    var i: u64 = 0;
    while (i < response.entry_count) : (i += 1) {
        const item = response.entries[@intCast(i)] orelse continue;
        collector.addRegion(.{
            .base = item.base,
            .length = item.length,
            .kind = pmmRegionKind(item.kind),
        }) catch {
            halt.haltWithMessage("Limine memory map parse failed");
        };
    }
    return collector.finish();
}

pub fn probeBootResourcesOrHalt(requests: Requests) void {
    captureExecutableRangeOrHalt(requests);
    _ = framebufferInfoOrHalt(requests);
    _ = moduleBytesOrHalt(requests, "init");
    _ = moduleBytesOrHalt(requests, "bootfs");
    const memmap = requests.memmap.response orelse {
        halt.haltWithMessage("Limine memory map response missing");
    };
    if (memmap.entry_count == 0) {
        halt.haltWithMessage("Limine memory map empty");
    }
}

pub fn buildBootResourcesOrHalt(
    requests: Requests,
    free_list: *kernel.FreePageList,
    user_spaces: []const boot_static.UserAddressSpace,
) entry.BootResources {
    return .{
        .framebuffer_info = framebufferInfoOrHalt(requests),
        .init_elf = moduleBytesOrHalt(requests, "init"),
        .bootfs_image = moduleBytesOrHalt(requests, "bootfs"),
        .memory_stats = collectMemoryStatsOrHalt(requests, free_list, user_spaces),
    };
}
