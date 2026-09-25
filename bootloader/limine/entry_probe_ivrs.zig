//! One-shot, read-only IVRS diagnostic. The normal kernel never links this
//! executable; it does not initialise an IOMMU or enter userland.
const std = @import("std");
const limine = @import("protocol.zig");
const ivrs = @import("acpi_ivrs");
const font = @import("ivrs_probe_font").bytes;

const max_table_size: usize = 1024 * 1024;
const acpi_header_size: usize = 36;
const max_memmap_entries: u64 = 16384;

// Revision 4 is needed: unlike the production revision 3 executable, its
// RSDP is an HHDM address and ACPI table regions are guaranteed HHDM-mapped.
pub export var limine_requests_start align(8) linksection(".limine_requests_start") = limine.requests_start_marker;
pub export var limine_base_revision align(8) linksection(".limine_requests") = [3]u64{
    limine.base_revision_magic0,
    limine.base_revision_magic1,
    4,
};

const init_module_path: [*:0]const u8 = "INITAPP.ELF";
const init_module_tag: [*:0]const u8 = "init";
const bootfs_module_path: [*:0]const u8 = "BOOTFS.IMG";
const bootfs_module_tag: [*:0]const u8 = "bootfs";
pub export var init_internal_module align(8) linksection(".limine_requests") = limine.InternalModule{
    .path = init_module_path,
    .string = init_module_tag,
    .flags = limine.internal_module_required,
};
pub export var bootfs_internal_module align(8) linksection(".limine_requests") = limine.InternalModule{
    .path = bootfs_module_path,
    .string = bootfs_module_tag,
    .flags = limine.internal_module_required,
};
pub export var internal_modules align(8) linksection(".limine_requests") = [2]?*limine.InternalModule{
    &init_internal_module,
    &bootfs_internal_module,
};
pub export var hhdm_request align(8) linksection(".limine_requests") = limine.HhdmRequest{};
pub export var framebuffer_request align(8) linksection(".limine_requests") = limine.FramebufferRequest{};
pub export var memmap_request align(8) linksection(".limine_requests") = limine.MemmapRequest{};
pub export var module_request align(8) linksection(".limine_requests") = limine.ModuleRequest{
    .internal_module_count = internal_modules.len,
    .internal_modules = &internal_modules,
};
pub export var rsdp_request align(8) linksection(".limine_requests") = limine.RsdpRequest{};
pub export var executable_address_request align(8) linksection(".limine_requests") = limine.ExecutableAddressRequest{};
pub export var limine_requests_end align(8) linksection(".limine_requests_end") = limine.requests_end_marker;

// The production protocol type uses a closed enum ending at 7. Revision 4
// adds type 8, RESERVED_MAPPED, so inspect just this field as raw u64 here.
const RawMemmapEntry = extern struct { base: u64, length: u64, kind: u64 };
// Keep the two results independent; a no-PAD rejection must not leave
// partially populated state for the PAD-enabled parser.
var legacy_info: ivrs.Info = .{};
var candidate_info: ivrs.Info = .{};

pub fn panic(_: []const u8, _: ?*std.builtin.StackTrace, _: ?usize) noreturn {
    haltForever();
}

comptime {
    // Host unit tests need Zig's normal test runner entry instead of this
    // privileged Limine entry. The freestanding ELF still exports `_start`.
    if (!@import("builtin").is_test) @export(&probeEntry, .{ .name = "_start" });
}

fn probeEntry() callconv(.naked) noreturn {
    // Limine base revision 4 does not require CR4.OSFXSR. Zig's x86-64 ABI
    // emits XMM moves even for plain struct copies, so enable SSE before any
    // compiler-generated instruction; this is CPU-local and touches no IOMMU.
    asm volatile (
        \\mov %cr0, %rax
        \\and $-5, %rax
        \\or $2, %rax
        \\mov %rax, %cr0
        \\clts
        \\mov %cr4, %rax
        \\or $0x600, %rax
        \\mov %rax, %cr4
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp ivrsMain
    );
}

pub export fn ivrsMain() callconv(.c) noreturn {
    const response = framebuffer_request.response orelse haltForever();
    if (response.framebuffer_count == 0) haltForever();
    const primary = response.framebuffers[0] orelse haltForever();
    var screen = Screen.init(primary) orelse haltForever();
    screen.clear();
    screen.write(0, "ENTRYIVRS  read-only ACPI / parser diagnostic");
    inspect(&screen);
    // GOP may be write-combining on the real GPU. Complete every store
    // before the permanent halt so a photographed screen is definitive.
    asm volatile ("sfence" ::: .{ .memory = true });
    haltForever();
}

fn inspect(screen: *Screen) void {
    var row: usize = 2;
    if (limine_base_revision[2] != 0 or limine_base_revision[1] < 4) {
        line(screen, &row, "ERROR: Limine base revision 4 unsupported (tag={x},{x})", .{
            limine_base_revision[1], limine_base_revision[2],
        });
        return;
    }
    line(screen, &row, "Limine base revision: {d}", .{limine_base_revision[1]});

    const hhdm = hhdm_request.response orelse {
        line(screen, &row, "ERROR: no HHDM response", .{});
        return;
    };
    const map = memmap_request.response orelse {
        line(screen, &row, "ERROR: no memory-map response", .{});
        return;
    };
    if (map.entry_count == 0 or map.entry_count > max_memmap_entries) {
        line(screen, &row, "ERROR: invalid memory-map entry count {d}", .{map.entry_count});
        return;
    }
    const rsdp_response = rsdp_request.response orelse {
        line(screen, &row, "ERROR: no RSDP response", .{});
        return;
    };
    const rsdp_physical = std.math.sub(u64, rsdp_response.address, hhdm.offset) catch {
        line(screen, &row, "ERROR: RSDP not in HHDM", .{});
        return;
    };
    line(screen, &row, "RSDP phys={x:0>16} HHDM={x:0>16}", .{ rsdp_physical, hhdm.offset });

    const access = Access{ .map = map, .hhdm_offset = hhdm.offset };
    const rsdp = readRsdp(&access, rsdp_physical) catch |err| {
        line(screen, &row, "RSDP ERROR: {s}", .{@errorName(err)});
        return;
    };
    line(screen, &row, "ACPI revision={d} XSDT={x:0>16} RSDT={x:0>8}", .{
        rsdp.revision, rsdp.xsdt, rsdp.rsdt,
    });
    line(screen, &row, "Extended RSDP: {s}", .{rsdp.extended_status});

    var table: ?[]const u8 = null;
    var skipped_ivrs: usize = 0;
    if (rsdp.xsdt != 0) {
        table = findInRoot(&access, rsdp.xsdt, true, &skipped_ivrs) catch |err| blk: {
            line(screen, &row, "XSDT ERROR: {s}", .{@errorName(err)});
            break :blk null;
        };
    }
    if (table == null and rsdp.rsdt != 0) {
        table = findInRoot(&access, rsdp.rsdt, false, &skipped_ivrs) catch |err| blk: {
            line(screen, &row, "RSDT ERROR: {s}", .{@errorName(err)});
            break :blk null;
        };
    }
    if (skipped_ivrs != 0) line(screen, &row, "ACPI: skipped {d} invalid IVRS table(s)", .{skipped_ivrs});
    const bytes = table orelse {
        line(screen, &row, "IVRS ERROR: not found in valid ACPI root", .{});
        return;
    };
    const fingerprint = fnv1a32(bytes);
    line(screen, &row, "IVRS length={d} revision={d}  IVRS-FNV32={x:0>8}", .{
        bytes.len, bytes[8], fingerprint,
    });
    line(screen, &row, "IVRS ACPI checksum byte=0x{x:0>2} sum=0x00 (verified)", .{bytes[9]});
    const focus = summarizeBlocks(screen, &row, bytes);
    line(screen, &row, "NO-PAD START", .{});
    asm volatile ("sfence" ::: .{ .memory = true });
    const legacy_error: ?[]const u8 = blk: {
        ivrs.parseLegacy(bytes, &legacy_info) catch |err| break :blk @errorName(err);
        break :blk null;
    };
    if (legacy_error) |err| {
        line(screen, &row, "NO-PAD ERROR: {s}", .{err});
    } else {
        line(screen, &row, "NO-PAD OK: units={d} memory ranges={d}", .{
            legacy_info.unit_count, legacy_info.memory_range_count,
        });
    }
    line(screen, &row, "PAD-ONLY START", .{});
    asm volatile ("sfence" ::: .{ .memory = true });
    const candidate_error: ?[]const u8 = blk: {
        ivrs.parse(bytes, &candidate_info) catch |err| break :blk @errorName(err);
        break :blk null;
    };
    if (candidate_error) |err| {
        line(screen, &row, "PAD-ONLY ERROR: {s}", .{err});
        dump(screen, &row, bytes, focus);
    } else {
        line(screen, &row, "PAD-ONLY OK: units={d} memory ranges={d}", .{
            candidate_info.unit_count, candidate_info.memory_range_count,
        });
        scanPci(screen, &row, &candidate_info);
    }
}

const Access = struct {
    map: *const limine.MemmapResponse,
    hhdm_offset: u64,

    fn covered(self: *const Access, paddr: u64, size: usize) bool {
        if (paddr == 0 or size == 0) return false;
        const end = std.math.add(u64, paddr, size) catch return false;
        var cursor = paddr;
        var spans: u64 = 0;
        while (cursor < end and spans < self.map.entry_count) : (spans += 1) {
            var next = cursor;
            for (0..@intCast(self.map.entry_count)) |index| {
                const ptr = self.map.entries[index] orelse continue;
                const item: *const RawMemmapEntry = @ptrCast(ptr);
                if (item.kind != 2 and item.kind != 3 and item.kind != 8) continue;
                const item_end = std.math.add(u64, item.base, item.length) catch continue;
                if (item.base <= cursor and cursor < item_end)
                    next = @max(next, @min(item_end, end));
            }
            if (next == cursor) return false;
            cursor = next;
        }
        return cursor == end;
    }

    fn bytes(self: *const Access, paddr: u64, size: usize) ?[]const u8 {
        if (!self.covered(paddr, size)) return null;
        const vaddr = std.math.add(u64, self.hhdm_offset, paddr) catch return null;
        if (vaddr > std.math.maxInt(usize)) return null;
        return @as([*]const u8, @ptrFromInt(@as(usize, @intCast(vaddr))))[0..size];
    }
};

const Rsdp = struct {
    revision: u8,
    rsdt: u32,
    xsdt: u64,
    extended_status: []const u8,
};

fn readRsdp(access: *const Access, paddr: u64) !Rsdp {
    const legacy = access.bytes(paddr, 20) orelse return error.RsdpOutsideAcpiMap;
    if (!std.mem.eql(u8, legacy[0..8], "RSD PTR ")) return error.InvalidRsdpSignature;
    if (!checksumOk(legacy)) return error.InvalidRsdpChecksum;
    const revision = legacy[15];
    const rsdt = read32(legacy, 16);
    if (revision < 2) return .{
        .revision = revision, .rsdt = rsdt, .xsdt = 0, .extended_status = "legacy RSDT only",
    };

    // Match the production ACPI walker: a valid legacy RSDP may still use
    // its RSDT if the optional extended portion is malformed or inaccessible.
    const header = access.bytes(paddr, 36) orelse return .{
        .revision = revision, .rsdt = rsdt, .xsdt = 0, .extended_status = "unmapped; RSDT fallback",
    };
    const length = read32(header, 20);
    if (length < 36 or length > 4096) return .{
        .revision = revision, .rsdt = rsdt, .xsdt = 0, .extended_status = "bad length; RSDT fallback",
    };
    const extended = access.bytes(paddr, length) orelse return .{
        .revision = revision, .rsdt = rsdt, .xsdt = 0, .extended_status = "unmapped span; RSDT fallback",
    };
    if (!checksumOk(extended)) return .{
        .revision = revision, .rsdt = rsdt, .xsdt = 0, .extended_status = "bad checksum; RSDT fallback",
    };
    return .{ .revision = revision, .rsdt = rsdt, .xsdt = read64(extended, 24), .extended_status = "valid" };
}

fn checkedTable(access: *const Access, paddr: u64, signature: []const u8, minimum: usize) ![]const u8 {
    const header = access.bytes(paddr, acpi_header_size) orelse return error.TableOutsideAcpiMap;
    if (!std.mem.eql(u8, header[0..4], signature)) return error.InvalidTableSignature;
    const length = read32(header, 4);
    if (length < minimum or length > max_table_size) return error.InvalidTableLength;
    const table = access.bytes(paddr, length) orelse return error.TableOutsideAcpiMap;
    if (!checksumOk(table)) return error.InvalidTableChecksum;
    return table;
}

fn findInRoot(access: *const Access, paddr: u64, xsdt: bool, skipped_ivrs: *usize) !?[]const u8 {
    const root = try checkedTable(access, paddr, if (xsdt) "XSDT" else "RSDT", acpi_header_size);
    const stride: usize = if (xsdt) 8 else 4;
    if ((root.len - acpi_header_size) % stride != 0) return error.InvalidRootLength;
    var offset: usize = acpi_header_size;
    while (offset < root.len) : (offset += stride) {
        const child_paddr: u64 = if (xsdt) read64(root, offset) else read32(root, offset);
        // Do not use a child address until its header itself is fully inside
        // a revision-4 ACPI mapped region. Non-IVRS children are not parsed.
        const header = access.bytes(child_paddr, acpi_header_size) orelse continue;
        if (!std.mem.eql(u8, header[0..4], "IVRS")) continue;
        // Production skips malformed matching children and keeps searching.
        // The diagnostic reports the count instead of masking a later valid IVRS.
        const child = checkedTable(access, child_paddr, "IVRS", 48) catch {
            skipped_ivrs.* += 1;
            continue;
        };
        return child;
    }
    return null;
}

fn checksumOk(bytes: []const u8) bool {
    var sum: u8 = 0;
    for (bytes) |byte| sum +%= byte;
    return sum == 0;
}

fn read32(bytes: []const u8, offset: usize) u32 {
    return std.mem.readInt(u32, bytes[offset..][0..4], .little);
}

fn read16(bytes: []const u8, offset: usize) u16 {
    return std.mem.readInt(u16, bytes[offset..][0..2], .little);
}

fn read64(bytes: []const u8, offset: usize) u64 {
    return std.mem.readInt(u64, bytes[offset..][0..8], .little);
}

fn fnv1a32(bytes: []const u8) u32 {
    var hash: u32 = 0x811c9dc5;
    for (bytes) |byte| hash = (hash ^ byte) *% 0x01000193;
    return hash;
}

const PciStatus = enum { platform, matched, unmatched, ambiguous };
const PciDecision = struct {
    status: PciStatus,
    source: u16 = 0,
    unit_index: u8 = 0,
};
const PciFunction = struct {
    bus: u8,
    device: u8,
    function: u8,
    vendor: u16,
    product: u16,
    class: u8,
    subclass: u8,
    decision: PciDecision,
};
const PciInventory = struct {
    original_cf8: u32 = 0,
    total: u32 = 0,
    platform: u32 = 0,
    candidates: u32 = 0,
    matched: u32 = 0,
    unmatched: u32 = 0,
    ambiguous: u32 = 0,
    first_unmatched: ?PciFunction = null,
    first_ambiguous: ?PciFunction = null,
    shared_source_groups: u32 = 0,
    first_shared_source: ?struct { unit: u8, source: u16, first_bdf: u16, second_bdf: u16 } = null,
    source_group_count: usize = 0,
    source_groups: [256]struct { unit: u8, source: u16, first_bdf: u16, count: u32 } = undefined,
    source_tracking_incomplete: bool = false,
    root_multifunction: bool = false,
    root_count: usize = 0,
    root: [8]PciFunction = undefined,
    sample_count: usize = 0,
    samples: [6]PciFunction = undefined,
};

fn noteSource(result: *PciInventory, decision: PciDecision, packed_bdf: u16) void {
    for (result.source_groups[0..result.source_group_count]) |*group| {
        if (group.unit != decision.unit_index or group.source != decision.source) continue;
        if (group.count == 1) {
            result.shared_source_groups += 1;
            if (result.first_shared_source == null) result.first_shared_source = .{
                .unit = group.unit,
                .source = group.source,
                .first_bdf = group.first_bdf,
                .second_bdf = packed_bdf,
            };
        }
        group.count += 1;
        return;
    }
    if (result.source_group_count == result.source_groups.len) {
        result.source_tracking_incomplete = true;
        return;
    }
    result.source_groups[result.source_group_count] = .{
        .unit = decision.unit_index,
        .source = decision.source,
        .first_bdf = packed_bdf,
        .count = 1,
    };
    result.source_group_count += 1;
}

/// Mirror `platformOnlyPciFunction` then `ruleForPci` from amd_iommu.zig.
/// IVHD `device_id` is a packed BDF, not the PCI vendor/product ID.
fn pciDecision(info: *const ivrs.Info, packed_bdf: u16, class: u8) PciDecision {
    if (class == 0x06) return .{ .status = .platform };
    for (info.units[0..info.unit_count]) |unit| {
        if (unit.segment == 0 and unit.device_id == packed_bdf) return .{ .status = .platform };
    }
    var found: ?PciDecision = null;
    for (info.units[0..info.unit_count], 0..) |*unit, index| {
        if (unit.segment != 0) continue;
        const rule = unit.ruleFor(packed_bdf) orelse continue;
        if (found != null) return .{ .status = .ambiguous };
        found = .{
            .status = .matched,
            .source = rule.sourceFor(packed_bdf) orelse unreachable,
            .unit_index = @intCast(index),
        };
    }
    return found orelse .{ .status = .unmatched };
}

fn inl(port: u16) u32 {
    return asm volatile ("inl %[port], %[value]"
        : [value] "={eax}" (-> u32),
        : [port] "{dx}" (port),
    );
}

fn outl(port: u16, value: u32) void {
    asm volatile ("outl %[value], %[port]"
        :
        : [value] "{eax}" (value),
          [port] "{dx}" (port),
    );
}

const NativePciIo = struct {
    fn read(_: *@This(), port: u16) u32 {
        if (port != 0xcf8 and port != 0xcfc) unreachable;
        return inl(port);
    }
    fn write(_: *@This(), port: u16, value: u32) void {
        // Only CF8 address-selector writes are made; never CFC data writes.
        if (port != 0xcf8) unreachable;
        outl(0xcf8, value);
    }
};

fn pciDword(io: anytype, bus: u8, device: u8, function: u8, comptime offset: u8) u32 {
    comptime {
        if (offset != 0x00 and offset != 0x08 and offset != 0x0c)
            @compileError("diagnostic PCI read outside allowed config dwords");
    }
    const selector: u32 = 0x8000_0000 |
        (@as(u32, bus) << 16) | (@as(u32, device) << 11) |
        (@as(u32, function) << 8) | offset;
    io.write(0xcf8, selector);
    return io.read(0xcfc);
}

/// Read only the same segment-0 standard config fields used by production.
/// The original CF8 selector is restored before returning. No BAR sizing,
/// command-register writes, ECAM accesses, MMIO, or IOMMU enable occurs here.
fn collectPci(info: *const ivrs.Info, io: anytype) PciInventory {
    var result = PciInventory{};
    result.original_cf8 = io.read(0xcf8);
    defer io.write(0xcf8, result.original_cf8);
    for (0..256) |bus_index| {
        const bus: u8 = @intCast(bus_index);
        for (0..32) |device_index| {
            const device: u8 = @intCast(device_index);
            const first_identity = pciDword(io, bus, device, 0, 0x00);
            if (@as(u16, @truncate(first_identity)) == 0xffff) continue;
            const first_header = pciDword(io, bus, device, 0, 0x0c);
            const multifunction = ((first_header >> 16) & 0x80) != 0;
            if (bus == 0 and device == 0) result.root_multifunction = multifunction;
            const function_count: usize = if (multifunction) 8 else 1;
            for (0..function_count) |function_index| {
                const function: u8 = @intCast(function_index);
                const identity = if (function == 0) first_identity else pciDword(io, bus, device, function, 0x00);
                const vendor: u16 = @truncate(identity);
                if (vendor == 0xffff) continue;
                const class_dword = pciDword(io, bus, device, function, 0x08);
                const class: u8 = @truncate(class_dword >> 24);
                const packed_bdf: u16 = (@as(u16, bus) << 8) |
                    (@as(u16, device) << 3) | function;
                const entry = PciFunction{
                    .bus = bus,
                    .device = device,
                    .function = function,
                    .vendor = vendor,
                    .product = @truncate(identity >> 16),
                    .class = class,
                    .subclass = @truncate(class_dword >> 16),
                    .decision = pciDecision(info, packed_bdf, class),
                };
                result.total += 1;
                if (bus == 0 and device == 0) {
                    result.root[result.root_count] = entry;
                    result.root_count += 1;
                }
                switch (entry.decision.status) {
                    .platform => result.platform += 1,
                    .matched, .unmatched, .ambiguous => {
                        result.candidates += 1;
                        if (result.sample_count < result.samples.len) {
                            result.samples[result.sample_count] = entry;
                            result.sample_count += 1;
                        }
                        switch (entry.decision.status) {
                            .matched => {
                                result.matched += 1;
                                noteSource(&result, entry.decision, packed_bdf);
                            },
                            .unmatched => {
                                result.unmatched += 1;
                                if (result.first_unmatched == null) result.first_unmatched = entry;
                            },
                            .ambiguous => {
                                result.ambiguous += 1;
                                if (result.first_ambiguous == null) result.first_ambiguous = entry;
                            },
                            .platform => unreachable,
                        }
                    },
                }
            }
        }
    }
    return result;
}

fn pciStatusName(status: PciStatus) []const u8 {
    return switch (status) {
        .platform => "SKIP",
        .matched => "MATCH",
        .unmatched => "MISS",
        .ambiguous => "AMBIG",
    };
}

fn pciLine(screen: *Screen, row: *usize, prefix: []const u8, entry: PciFunction) void {
    if (entry.decision.status == .matched) {
        line(screen, row, "{s} {x:0>2}:{x:0>2}.{d} {x:0>4}:{x:0>4} cls={x:0>2}/{x:0>2} src={x:0>4} U{d}", .{
            prefix, entry.bus, entry.device, entry.function, entry.vendor, entry.product,
            entry.class, entry.subclass, entry.decision.source, entry.decision.unit_index,
        });
    } else {
        line(screen, row, "{s} {x:0>2}:{x:0>2}.{d} {x:0>4}:{x:0>4} cls={x:0>2}/{x:0>2} {s}", .{
            prefix, entry.bus, entry.device, entry.function, entry.vendor, entry.product,
            entry.class, entry.subclass, pciStatusName(entry.decision.status),
        });
    }
}

fn scanPci(screen: *Screen, row: *usize, info: *const ivrs.Info) void {
    line(screen, row, "PCI SCAN START: CF8/CFC read-only", .{});
    asm volatile ("sfence" ::: .{ .memory = true });
    var io = NativePciIo{};
    const result = collectPci(info, &io);
    line(screen, row, "PCI CF8 standard 00/08/0C only; no ECAM extended/BAR/MMIO", .{});
    line(screen, row, "PCI total={d} platform={d} candidate={d} (limit 256)", .{
        result.total, result.platform, result.candidates,
    });
    line(screen, row, "PCI matched={d} missing={d} ambiguous={d} CF8 restored", .{
        result.matched, result.unmatched, result.ambiguous,
    });
    if (result.first_shared_source) |shared| {
        line(screen, row, "PCI shared source groups={d} first U{d}/src={x:0>4} BDF={x:0>4}+{x:0>4}", .{
            result.shared_source_groups, shared.unit, shared.source, shared.first_bdf, shared.second_bdf,
        });
    } else {
        line(screen, row, "PCI shared source groups={d} tracking={s}", .{
            result.shared_source_groups, if (result.source_tracking_incomplete) "INCOMPLETE" else "complete",
        });
    }
    if (result.first_unmatched) |entry| pciLine(screen, row, "FIRST MISS", entry);
    if (result.first_ambiguous) |entry| pciLine(screen, row, "FIRST AMBIG", entry);
    line(screen, row, "00:00.x present={d} multifunction={s}", .{
        result.root_count, if (result.root_multifunction) "yes" else "no",
    });
    const root_shown = @min(result.root_count, 4, screen.rows -| row.* -| 2);
    for (result.root[0..root_shown]) |entry| pciLine(screen, row, "ROOT", entry);
    const sample_shown = @min(result.sample_count, screen.rows -| row.* -| 1);
    line(screen, row, "Candidates shown={d}/{d} (first in BDF order)", .{
        sample_shown, result.candidates,
    });
    for (result.samples[0..sample_shown]) |entry| pciLine(screen, row, "CAND", entry);
}

fn line(screen: *Screen, row: *usize, comptime format: []const u8, args: anytype) void {
    var buffer: [160]u8 = undefined;
    screen.write(row.*, std.fmt.bufPrint(&buffer, format, args) catch "format error");
    row.* += 1;
}

const EntryHit = struct { offset: usize, kind: u8 };

/// Lexical-only synopsis of the IVRS blocks and device-entry type bytes. It
/// intentionally does not judge device IDs, rules, or IVHD semantics: the
/// production parser above remains the sole authority for that result.
fn summarizeBlocks(screen: *Screen, row: *usize, bytes: []const u8) ?usize {
    var block_offset: usize = 48;
    var block_count: usize = 0;
    var pad_count: usize = 0;
    var other_count: usize = 0;
    var first_pad: ?EntryHit = null;
    var first_other: ?EntryHit = null;
    var scan_stop: ?usize = null;
    while (block_offset < bytes.len) {
        if (bytes.len - block_offset < 4) {
            scan_stop = block_offset;
            break;
        }
        const kind = bytes[block_offset];
        const length: usize = read16(bytes, block_offset + 2);
        if (length < 4 or length > bytes.len - block_offset) {
            scan_stop = block_offset;
            break;
        }
        if (block_count < 2) line(screen, row, "BLOCK#{d} @0x{x:0>4} type=0x{x:0>2} length={d}", .{
            block_count, block_offset, kind, length,
        });
        block_count += 1;
        if (kind == 0x10 or kind == 0x11 or kind == 0x40) {
            const header_len: usize = if (kind == 0x10) 24 else 40;
            if (length < header_len) {
                scan_stop = block_offset;
                break;
            }
            const end = block_offset + length;
            var entry_offset = block_offset + header_len;
            while (entry_offset < end) {
                const entry_kind = bytes[entry_offset];
                if (entry_kind == 0) {
                    pad_count += 1;
                    if (first_pad == null) first_pad = .{ .offset = entry_offset, .kind = entry_kind };
                } else if (entry_kind != 0x01 and entry_kind != 0x02 and entry_kind != 0x03 and
                    entry_kind != 0x04 and entry_kind != 0x42 and entry_kind != 0x43 and
                    entry_kind != 0x46 and entry_kind != 0x47 and entry_kind != 0x48 and
                    !(entry_kind == 0xf0 and kind == 0x40))
                {
                    other_count += 1;
                    if (first_other == null) first_other = .{ .offset = entry_offset, .kind = entry_kind };
                }
                const step: usize = if (entry_kind < 0x40) 4 else if (entry_kind < 0x80) 8 else if (
                    entry_kind == 0xf0 and kind == 0x40 and end - entry_offset >= 22
                ) 22 + @as(usize, bytes[entry_offset + 21]) else 0;
                if (step == 0 or step > end - entry_offset) {
                    scan_stop = entry_offset;
                    break;
                }
                entry_offset += step;
            }
        }
        if (scan_stop != null) break;
        block_offset += length;
    }
    line(screen, row, "LEXICAL blocks={d} PAD00={d} other={d}", .{ block_count, pad_count, other_count });
    if (first_pad) |hit| line(screen, row, "First PAD00 entry @IVRS+0x{x:0>4}", .{hit.offset});
    if (first_other) |hit| line(screen, row, "First other entry type=0x{x:0>2} @IVRS+0x{x:0>4}", .{
        hit.kind, hit.offset,
    });
    if (scan_stop) |offset| line(screen, row, "Lexical scan stops @IVRS+0x{x:0>4}", .{offset});
    return if (first_other) |hit| hit.offset else if (first_pad) |hit| hit.offset else scan_stop;
}

fn dump(screen: *Screen, row: *usize, bytes: []const u8, focus: ?usize) void {
    if (row.* >= screen.rows) return;
    const available_lines = screen.rows - row.* - 1;
    const prefix_end = @min(bytes.len, available_lines * 16);
    if (focus) |f| {
        if (available_lines >= 8 and f >= prefix_end and f < bytes.len) {
            const first_end: usize = 4 * 16;
            const total_lines = (bytes.len + 15) / 16;
            const window_lines = available_lines - 4;
            const focus_line = f / 16;
            var start_line = focus_line -| (window_lines / 2);
            start_line = @max(start_line, 4);
            if (start_line + window_lines > total_lines) start_line = total_lines - window_lines;
            const window_start = start_line * 16;
            const window_end = @min(bytes.len, (start_line + window_lines) * 16);
            line(screen, row, "RAW [0..0x{x}) + [0x{x}..0x{x}) / 0x{x} TRUNCATED", .{
                first_end, window_start, window_end, bytes.len,
            });
            dumpRange(screen, row, bytes, 0, first_end);
            dumpRange(screen, row, bytes, window_start, window_end);
            return;
        }
    }
    line(screen, row, "RAW [0..0x{x}) / 0x{x}  {s}", .{
        prefix_end, bytes.len, if (prefix_end < bytes.len) "TRUNCATED" else "COMPLETE",
    });
    dumpRange(screen, row, bytes, 0, prefix_end);
}

fn dumpRange(screen: *Screen, row: *usize, bytes: []const u8, start: usize, end: usize) void {
    var offset = start;
    while (offset < end) : (offset += 16) {
        var buffer: [80]u8 = undefined;
        const prefix = std.fmt.bufPrint(&buffer, "{x:0>4}: ", .{offset}) catch unreachable;
        var used: usize = prefix.len;
        for (bytes[offset..@min(offset + 16, end)]) |byte| {
            const item = std.fmt.bufPrint(buffer[used..], "{x:0>2} ", .{byte}) catch unreachable;
            used += item.len;
        }
        screen.write(row.*, buffer[0..used]);
        row.* += 1;
    }
}

const Screen = struct {
    pixels: [*]volatile u8,
    width: usize,
    height: usize,
    pitch: usize,
    scale: usize,
    cols: usize,
    rows: usize,

    fn init(fb: *const limine.Framebuffer) ?Screen {
        if (fb.address == null or fb.memory_model != limine.framebuffer_rgb or
            fb.bpp != 32 or fb.width == 0 or fb.height == 0 or fb.pitch == 0 or
            (fb.pitch & 3) != 0 or fb.width > fb.pitch / 4 or
            fb.width > std.math.maxInt(usize) or fb.height > std.math.maxInt(usize) or
            fb.pitch > std.math.maxInt(usize)) return null;
        _ = std.math.mul(u64, fb.pitch, fb.height) catch return null;
        const scale: usize = if (fb.width >= 3000) 3 else if (fb.width >= 1400) 2 else 1;
        const width: usize = @intCast(fb.width);
        const height: usize = @intCast(fb.height);
        const cols = @min(width / (8 * scale), 110);
        const rows = @min(height / (16 * scale), 48);
        if (cols < 60 or rows < 12) return null;
        if (font.len < 4 + 256 * 16 or font[0] != 0x36 or font[1] != 0x04 or font[3] != 16) return null;
        return .{
            .pixels = @ptrCast(fb.address.?),
            .width = width,
            .height = height,
            .pitch = @intCast(fb.pitch),
            .scale = scale,
            .cols = cols,
            .rows = rows,
        };
    }

    fn clear(self: *Screen) void {
        for (0..self.height) |y| {
            for (0..self.width) |x| {
                const offset = y * self.pitch + x * 4;
                self.pixels[offset] = 0;
                self.pixels[offset + 1] = 0;
                self.pixels[offset + 2] = 0;
                self.pixels[offset + 3] = 0;
            }
        }
    }

    fn write(self: *Screen, row: usize, text: []const u8) void {
        if (row >= self.rows) return;
        for (text[0..@min(text.len, self.cols)], 0..) |char, column| {
            const glyph = if (char >= 32 and char <= 126) char else '?';
            for (0..16) |gy| {
                const bits = font[4 + @as(usize, glyph) * 16 + gy];
                for (0..8) |gx| {
                    if ((bits & (@as(u8, 0x80) >> @intCast(gx))) == 0) continue;
                    for (0..self.scale) |sy| {
                        for (0..self.scale) |sx| {
                            const x = (column * 8 + gx) * self.scale + sx;
                            const y = (row * 16 + gy) * self.scale + sy;
                            if (x >= self.width or y >= self.height) continue;
                            const offset = y * self.pitch + x * 4;
                            self.pixels[offset] = 0xff;
                            self.pixels[offset + 1] = 0xff;
                            self.pixels[offset + 2] = 0xff;
                            self.pixels[offset + 3] = 0;
                        }
                    }
                }
            }
        }
    }
};

fn haltForever() noreturn {
    while (true) asm volatile ("hlt");
}

test "covered ACPI spans reject gaps and type-1 reserved areas" {
    var first = RawMemmapEntry{ .base = 0x1000, .length = 0x800, .kind = 2 };
    var second = RawMemmapEntry{ .base = 0x1800, .length = 0x800, .kind = 8 };
    var denied = RawMemmapEntry{ .base = 0x2000, .length = 0x800, .kind = 1 };
    var entries = [_]?*limine.MemmapEntry{ @ptrCast(&first), @ptrCast(&second), @ptrCast(&denied) };
    const map = limine.MemmapResponse{ .revision = 0, .entry_count = entries.len, .entries = &entries };
    const access = Access{ .map = &map, .hhdm_offset = 0 };
    try std.testing.expect(access.covered(0x1700, 0x900));
    try std.testing.expect(!access.covered(0x1700, 0xa00));
    try std.testing.expect(!access.covered(0x2000, 0x10));
}

test "PCI preflight mirrors platform skip unique rules and CF8 restoration" {
    const info = try std.testing.allocator.create(ivrs.Info);
    defer std.testing.allocator.destroy(info);
    info.* = .{};
    info.unit_count = 3;
    info.units[0].segment = 0;
    info.units[0].device_id = 0x0002;
    info.units[0].rule_count = 3;
    info.units[0].rules[0] = .{ .first = 0x0008, .last = 0x0008, .alias = true, .source = 0x0040 };
    info.units[0].rules[1] = .{ .first = 0x0018, .last = 0x0018 };
    info.units[0].rules[2] = .{ .first = 0x0020, .last = 0x0020, .alias = true, .source = 0x0040 };
    info.units[1].segment = 0;
    info.units[1].rule_count = 1;
    info.units[1].rules[0] = .{ .first = 0x0018, .last = 0x0018 };
    info.units[2].segment = 1;
    info.units[2].rule_count = 1;
    info.units[2].rules[0] = .{ .first = 0x0010, .last = 0x0010 };

    const FakeIo = struct {
        selector: u32 = 0x1234_5678,
        forbidden_writes: u32 = 0,
        forbidden_reads: u32 = 0,

        fn write(self: *@This(), port: u16, value: u32) void {
            if (port != 0xcf8) {
                self.forbidden_writes += 1;
                return;
            }
            self.selector = value;
        }

        fn read(self: *@This(), port: u16) u32 {
            if (port == 0xcf8) return self.selector;
            if (port != 0xcfc) {
                self.forbidden_reads += 1;
                return 0xffff_ffff;
            }
            const bdf: u16 = @truncate(self.selector >> 8);
            const offset = self.selector & 0xfc;
            if (offset == 0x00) return switch (bdf) {
                0x0000 => 0x1111_1234, // host bridge, multifunction
                0x0002 => 0x2222_1234, // IOMMU's own function
                0x0008 => 0x3333_1234, // first alias to source 0x40
                0x0010 => 0x4444_1234, // no segment-0 rule
                0x0018 => 0x5555_1234, // two units match
                0x0020 => 0x6666_1234, // second alias to source 0x40
                else => 0xffff_ffff,
            };
            if (offset == 0x08) return switch (bdf) {
                0x0000 => 0x0600_0000,
                0x0002 => 0x0800_0000,
                0x0008 => 0x0200_0000,
                0x0010 => 0x0300_0000,
                0x0018 => 0x0100_0000,
                0x0020 => 0x0200_0000,
                else => 0xffff_ffff,
            };
            if (offset == 0x0c) return if (bdf == 0) 0x0080_0000 else 0;
            self.forbidden_reads += 1;
            return 0xffff_ffff;
        }
    };
    var fake = FakeIo{};
    const result = collectPci(info, &fake);
    try std.testing.expectEqual(@as(u32, 0x1234_5678), fake.selector);
    try std.testing.expectEqual(@as(u32, 0), fake.forbidden_writes);
    try std.testing.expectEqual(@as(u32, 0), fake.forbidden_reads);
    try std.testing.expectEqual(@as(u32, 6), result.total);
    try std.testing.expectEqual(@as(u32, 2), result.platform);
    try std.testing.expectEqual(@as(u32, 4), result.candidates);
    try std.testing.expectEqual(@as(u32, 2), result.matched);
    try std.testing.expectEqual(@as(u32, 1), result.unmatched);
    try std.testing.expectEqual(@as(u32, 1), result.ambiguous);
    try std.testing.expectEqual(@as(u32, 1), result.shared_source_groups);
    try std.testing.expectEqual(@as(u16, 0x0040), result.first_shared_source.?.source);
    try std.testing.expectEqual(@as(usize, 2), result.root_count);
    try std.testing.expect(result.root_multifunction);
    try std.testing.expectEqual(@as(u8, 2), result.root[1].function);
    try std.testing.expectEqual(@as(u16, 0x0010),
        (@as(u16, result.first_unmatched.?.bus) << 8) | (@as(u16, result.first_unmatched.?.device) << 3));
}
