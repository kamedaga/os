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
const x86_platform = kernel_api.x86_platform;
const addresses = kernel_api.addresses;
const dm = kernel_api.direct_map;
const owner = kernel_api.boot_direct_map;

pub const Requests = struct {
    hhdm: *limine.HhdmRequest,
    framebuffer: *limine.FramebufferRequest,
    memmap: *limine.MemmapRequest,
    module: *limine.ModuleRequest,
    rsdp: *limine.RsdpRequest,
    executable_address: *limine.ExecutableAddressRequest,
};

const low_memory_limit: u64 = 0x100000;
const page_bytes: u64 = 4096;
var scratch_memmap: ?*limine.MemmapResponse = null;
fn scratchRegion(index: usize) ?dm.Region {
    const response = scratch_memmap orelse return null;
    if (index >= response.entry_count) return null;
    const item = response.entries[index] orelse halt.haltWithMessage("null boot memmap entry");
    return .{ .base = item.base, .bytes = item.length, .usable = item.kind == .usable };
}

pub fn smpBootResourcesOrHalt(requests: Requests) entry.LimineSmpResources {
    const rsdp = requests.rsdp.response orelse {
        halt.haltWithMessage("Limine RSDP response missing");
    };
    const rsdp_paddr = rsdp.address;
    if (rsdp_paddr == 0 or rsdp_paddr >= (@as(u64, 1) << @as(u6, @intCast(x86_platform.directMapCpuFeatures().bits)))) {
        halt.haltWithMessage("Limine RSDP physical address unsupported");
    }
    const memmap = requests.memmap.response orelse {
        halt.haltWithMessage("Limine memory map response missing");
    };
    // The serial AP handshake permits one shared low-memory trampoline.
    const required_bytes = page_bytes;
    var trampoline_base: u64 = 0;
    var i: u64 = 0;
    while (i < memmap.entry_count) : (i += 1) {
        const item = memmap.entries[@intCast(i)] orelse continue;
        if (item.kind != .usable or item.base >= low_memory_limit) continue;
        const raw_end = item.base +| item.length;
        const region_end = @min(raw_end, low_memory_limit) & ~(page_bytes - 1);
        const region_start = @max(item.base, page_bytes);
        const aligned_start = (region_start + (page_bytes - 1)) & ~(page_bytes - 1);
        if (region_end < required_bytes) continue;
        const candidate = region_end - required_bytes;
        if (candidate >= aligned_start and candidate > trampoline_base) {
            trampoline_base = candidate;
        }
    }
    if (trampoline_base == 0) {
        halt.haltWithMessage("Limine SMP trampoline memory unavailable");
    }
    return .{
        .rsdp_paddr = rsdp_paddr,
        .trampoline_base = trampoline_base,
    };
}

fn cstrEq(value: [*:0]const u8, expected: []const u8) bool {
    return std.mem.eql(u8, std.mem.span(value), expected);
}

pub fn hhdmOffsetOrHalt(requests: Requests) u64 {
    const response = requests.hhdm.response orelse {
        halt.haltWithMessage("Limine HHDM response missing");
    };
    return response.offset;
}

fn hhdmPtrToPaddr(requests: Requests, ptr: ?*anyopaque) addresses.PhysicalAddress {
    const virtual = addresses.KernelVirtualAddress.fromPointer(ptr orelse {
        halt.haltWithMessage("Limine null HHDM pointer");
    });
    const hhdm_offset = hhdmOffsetOrHalt(requests);
    if (virtual.toInt() < hhdm_offset) {
        halt.haltWithMessage("Limine pointer below HHDM");
    }
    // Limine HHDM is valid only before the kernel CR3 switch. Never export
    // its offset as the runtime RAM direct-map base.
    return .fromInt(virtual.toInt() - hhdm_offset);
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
    const paddr = hhdmPtrToPaddr(requests, framebuffer.address).toInt();
    const size_bytes = boot_resources.framebufferBytes(paddr, framebuffer.width, framebuffer.height, framebuffer.pitch, boot_static.physical_map_limit_exclusive) catch |err| {
        halt.haltWithError("Limine framebuffer invalid: ", err);
    };
    return .{
        .paddr = paddr,
        .size_bytes = size_bytes,
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
    entry.installPmmBootAccess(hhdmOffsetOrHalt(requests));
    const storage = userSpaceStoragePhysicalRange(user_spaces);
    const user_spaces_start = storage.start.toInt();
    const user_spaces_end = storage.end.toInt();
    // Publish before init/bootfs temporary staging: a persistent allocation
    // must never pin an older temporary scratch loan. PMM retains this table
    // after all free ranges have been allocated and across the CR3 switch.
    scratch_memmap = response;
    const features = x86_platform.directMapCpuFeatures();
    if (!x86_platform.enableNxForCurrentCpu()) halt.haltWithMessage("scratch requires NX");
    owner.installScratch(scratchRegion, features.bits, hhdmOffsetOrHalt(requests)) catch |err|
        halt.haltWithError("boot scratch backing installation failed: ", err);
    // Snapshot before any PMM seeding. Split image boundaries so
    // no image leaf gains a writable alias, even with overlapping usable input.
    const snapshot_count = std.math.mul(usize, @intCast(response.entry_count), 5) catch
        halt.haltWithMessage("direct map snapshot overflow");
    const snapshot = owner.metadata(dm.Region, snapshot_count) catch
        halt.haltWithMessage("direct map snapshot allocation failed");
    const image_start = image_range.base_paddr & ~@as(u64, 4095);
    const image_bytes_rounded = std.math.add(u64, image_range.size_bytes, 4095) catch
        halt.haltWithMessage("direct map image size overflow");
    const image_end = std.math.add(u64, image_range.base_paddr, image_bytes_rounded) catch
        halt.haltWithMessage("direct map image overflow");
    const image_stop = image_end & ~@as(u64, 4095);
    var used: usize = 0;
    for (response.entries[0..@intCast(response.entry_count)]) |maybe_item| {
        const item = maybe_item orelse continue;
        const end = std.math.add(u64, item.base, item.length) catch halt.haltWithMessage("direct map region overflow");
        var cursor = item.base;
        while (cursor < end) {
            var stop = end;
            for ([_]u64{ image_start, image_stop }) |boundary|
                if (boundary > cursor) {
                    stop = @min(stop, boundary);
                };
            const in_image = cursor >= image_start and cursor < image_stop;
            snapshot[used] = .{ .base = cursor, .bytes = stop - cursor, .usable = item.kind == .usable and !in_image, .prepare_only = false };
            used += 1;
            cursor = stop;
        }
    }
    dm.validateImage(image_range.virtual_base, image_range.size_bytes, 0xffff_ffff_c000_0000, 1 << 30) catch
        halt.haltWithMessage("direct map image/AP stack overlap");
    _ = owner.build(snapshot[0..used], features.bits, features.huge, hhdmOffsetOrHalt(requests), .{ .start = 0, .end = 0 }) catch |err|
        halt.haltWithError("direct map construction failed: ", err);
    var boundary_count: usize = 0;
    var usable_ids: u64 = 0;
    for (response.entries[0..@intCast(response.entry_count)]) |maybe_item| {
        const item = maybe_item orelse continue;
        const region: pmm.MemoryRegion = .{ .base = item.base, .length = item.length, .kind = pmmRegionKind(item.kind) };
        if (region.kind == .usable) usable_ids += 1;
        if ((pmm.regionBoundary(0, region) catch halt.haltWithMessage("PMM region overflow")) != null) boundary_count += 1;
    }
    if (usable_ids == 0) halt.haltWithMessage("PMM has no usable pages");
    const bytes = std.math.mul(usize, boundary_count, @sizeOf(pmm.RegionBoundary)) catch
        halt.haltWithMessage("PMM region table overflow");
    const allocation = kernel_api.boot_scratch.reservePersistent(bytes) catch
        halt.haltWithMessage("PMM region table allocation failed");
    var loan: kernel_api.boot_scratch.Borrow = .{};
    allocation.borrowInto(&loan) catch halt.haltWithMessage("PMM region table borrow failed");
    const boundaries = std.mem.bytesAsSlice(pmm.RegionBoundary, loan.bytes() catch
        halt.haltWithMessage("PMM region table view failed"));
    var boundary_index: usize = 0;
    var region_id: u64 = 0;
    for (response.entries[0..@intCast(response.entry_count)]) |maybe_item| {
        const item = maybe_item orelse continue;
        const region: pmm.MemoryRegion = .{ .base = item.base, .length = item.length, .kind = pmmRegionKind(item.kind) };
        if (region.kind == .usable) {
            if (pmm.regionBoundary(region_id, region) catch halt.haltWithMessage("PMM region overflow")) |boundary| {
                boundaries[boundary_index] = boundary;
                boundary_index += 1;
            }
            region_id += 1;
        }
    }
    // The table stays in the retained scratch prefix. MemoryCollector seeds
    // only firmware usable pages and excludes the complete kernel image plus
    // every scratch backing/table page recorded by the boot reservation owner.
    if (boundary_index != boundaries.len) halt.haltWithMessage("PMM region count changed");
    pmm.sortBoundaries(boundaries);
    loan.end() catch halt.haltWithMessage("PMM region loan end failed");
    const published = allocation.publishPersistent() catch halt.haltWithMessage("PMM region publication failed");
    free_list.installRegions(std.mem.bytesAsSlice(pmm.RegionBoundary, published)) catch
        halt.haltWithMessage("PMM region table invalid");
    pmm.prepareBootAccess(snapshot[0..used]) catch halt.haltWithMessage("boot HHDM deferral preparation failed");
    const boot_pages = owner.handoffScratch(free_list) catch halt.haltWithMessage("boot reservation snapshot failed");
    var collector = pmm.MemoryCollector.init(free_list, user_spaces_start, user_spaces_end) orelse {
        halt.haltWithMessage("Limine memory map parse failed");
    };
    collector.reserveBootPages(boot_pages) catch halt.haltWithMessage("direct map reservation handoff failed");
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
    scratch_memmap = null;
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

// The storage is part of the linked kernel image, not a Limine HHDM loan.
// Translate the two endpoints through image metadata before PMM exclusion;
// treating KVA numerically as physical silently breaks a relocated image.
fn userSpaceStoragePhysicalRange(spaces: []const boot_static.UserAddressSpace) struct { start: addresses.PhysicalAddress, end: addresses.PhysicalAddress } {
    if (spaces.len == 0) return .{ .start = .fromInt(0), .end = .fromInt(0) };
    const first = addresses.KernelVirtualAddress.fromPointer(spaces.ptr);
    const bytes = std.math.mul(usize, spaces.len, @sizeOf(boot_static.UserAddressSpace)) catch
        halt.haltWithMessage("user-space storage extent overflow");
    const last = first.offset(bytes - 1) orelse halt.haltWithMessage("user-space storage KVA overflow");
    const physical = x86_platform.kernelAddressToPhysical(first) orelse halt.haltWithMessage("user-space storage unmapped");
    const physical_last = x86_platform.kernelAddressToPhysical(last) orelse halt.haltWithMessage("user-space storage end unmapped");
    if (physical_last.toInt() < physical.toInt() or physical_last.toInt() - physical.toInt() != bytes - 1)
        halt.haltWithMessage("user-space storage not contiguous");
    return .{ .start = .fromInt(kernel_vm.pageAlignDown(physical.toInt())), .end = .fromInt(kernel_vm.pageAlignUp(physical_last.toInt() + 1)) };
}
