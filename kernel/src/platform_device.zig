//! Native non-PCI resource ownership. This module performs no keyboard
//! protocol, decoding, controller reset, or driver timeout policy.
const std = @import("std");
const abi = @import("kernel_abi_root").capsule_abi;
const acpi = @import("acpi_tables.zig");
const arch = @import("arch/x86_64/platform.zig");
const smp = @import("smp.zig");
const lapic = @import("lapic.zig");
const mmio = @import("memory/mmio.zig");

// 0x40=timer, 0x41=unrouted device, 0x42=scheduler/maintenance IPI.
pub const interrupt_vector: u8 = 0x43;
pub const ResourceId = enum(u64) { i8042 = 1 };
pub const Grant = struct { resource: ResourceId = .i8042, generation: u64 = 0 };
pub const Descriptor = extern struct {
    resource_id: u64 = @intFromEnum(ResourceId.i8042),
    kind: u64 = abi.platform_device_i8042,
    data_port: u64 = 0x60,
    command_port: u64 = 0x64,
    irq: u64 = 1,
    gsi: u64 = 1,
    irq_flags: u64 = 0,
    flags: u64 = abi.platform_flag_fadt_8042 | abi.platform_flag_controller_write,
};
pub const IrqGrant = struct { device: Grant, epoch: u64 };

const Route = struct { gsi: u32 = 1, flags: u64 = 0 };
const Controller = struct { base: u32, gsi_base: u32, id: u8 };
const Geometry = struct { end: u32, last_pin: u8 };
fn legacyDestination(id: u32) ?u8 {
    return if (id <= 255) @intCast(id) else null;
}
fn controllerGeometry(ctl: Controller, version: u32, id_register: u32) !Geometry {
    const last_pin = (version >> 16) & 255;
    // IOREGSEL is 8 bits: pin 119 ends at high selector 0xff. Never wrap a
    // firmware/controller pin count into another register or truncate APIC ID.
    if (version == 0xffffffff or last_pin > 119 or ((id_register >> 24) & 15) != ctl.id) return error.Invalid;
    return .{ .end = std.math.add(u32, ctl.gsi_base, last_pin + 1) catch return error.Invalid, .last_pin = @intCast(last_pin) };
}

fn read32(bytes: []const u8, offset: usize) u32 {
    return std.mem.readInt(u32, bytes[offset..][0..4], .little);
}

fn checkedTable(bytes: []const u8, signature: *const [4]u8, minimum: usize) bool {
    if (bytes.len < minimum or !std.mem.eql(u8, bytes[0..4], signature) or read32(bytes, 4) != bytes.len) return false;
    var sum: u8 = 0;
    for (bytes) |byte| sum +%= byte;
    return sum == 0;
}

pub fn has8042(fadt: []const u8) bool {
    // IA-PC_BOOT_ARCH was reserved before FADT revision 3. A set bit in an
    // older reserved field cannot confer access to the reset-capable port.
    return checkedTable(fadt, "FACP", 111) and fadt[8] >= 3 and
        (std.mem.readInt(u16, fadt[109..111], .little) & 2) != 0;
}

const Entries = struct {
    bytes: []const u8,
    offset: usize = 44,
    fn next(self: *Entries) !?[]const u8 {
        if (self.offset == self.bytes.len) return null;
        if (self.offset > self.bytes.len or self.bytes.len - self.offset < 2) return error.Invalid;
        const len = self.bytes[self.offset + 1];
        if (len < 2 or len > self.bytes.len - self.offset) return error.Invalid;
        const entry = self.bytes[self.offset..][0..len];
        self.offset += len;
        return entry;
    }
};

fn controller(entry: []const u8) !Controller {
    if (entry.len != 12 or entry[3] != 0) return error.Invalid;
    const base = read32(entry, 4);
    if (base < 0x100000 or base & 4095 != 0) return error.Invalid;
    return .{ .base = base, .gsi_base = read32(entry, 8), .id = entry[2] };
}

pub fn keyboardRoute(madt: []const u8) !Route {
    if (!checkedTable(madt, "APIC", 44)) return error.Invalid;
    var routes: [16]Route = undefined;
    var overrides: u16 = 0;
    for (&routes, 0..) |*route, i| route.* = .{ .gsi = @intCast(i) };
    var entries = Entries{ .bytes = madt };
    while (try entries.next()) |entry| {
        switch (entry[0]) {
            1 => {
                _ = try controller(entry);
            },
            2 => {
                if (entry.len != 10 or entry[2] != 0 or entry[3] >= 16) return error.Invalid;
                const source: u4 = @intCast(entry[3]);
                if (overrides & (@as(u16, 1) << source) != 0) return error.Invalid;
                overrides |= @as(u16, 1) << source;
                const flags = std.mem.readInt(u16, entry[8..10], .little);
                const polarity = flags & 3;
                const trigger = (flags >> 2) & 3;
                if (flags & ~@as(u16, 15) != 0 or polarity == 2 or trigger == 2) return error.Invalid;
                routes[source] = .{ .gsi = read32(entry, 4), .flags = (if (polarity == 3) abi.platform_irq_active_low else @as(u64, 0)) |
                    (if (trigger == 3) abi.platform_irq_level else @as(u64, 0)) };
            },
            else => {},
        }
    }
    // A dedicated capability cannot own a shared ISA pin: neither mask nor
    // retirement may affect an unrelated device on that pin.
    for (routes, 0..) |route, i| if (i != 1 and route.gsi == routes[1].gsi) return error.Invalid;
    entries = .{ .bytes = madt };
    while (try entries.next()) |entry| {
        if (entry[0] == 3 and (entry.len != 8 or read32(entry, 4) == routes[1].gsi)) return error.Invalid;
    }
    return routes[1];
}

/// K serializes ownership, PIO and IOREGSEL/window transactions. The interrupt
/// path reads only the published atomic epoch; it never acquires K or accesses
/// IOAPIC registers. Drain runs without a leaf/VM lock and may service IPIs.
const Ownership = struct {
    generation: u64 = 0,
    references: u32 = 0,
    next_epoch: u64 = 0,
    active_epoch: u64 = 0,
    poisoned: bool = false,

    fn valid(self: *const Ownership, grant: Grant) bool {
        return grant.resource == .i8042 and grant.generation != 0 and
            grant.generation == self.generation and self.references != 0 and !self.poisoned;
    }
    fn claim(self: *Ownership) ?Grant {
        if (self.poisoned or self.references != 0 or self.generation == std.math.maxInt(u64)) return null;
        self.generation += 1;
        self.references = 1;
        return .{ .generation = self.generation };
    }
    fn release(self: *Ownership, grant: Grant) void {
        // Poisoned ownership still has to discharge its references; poison
        // blocks reuse rather than leaking object storage or granting a lease.
        if (grant.generation != self.generation or self.references == 0) return;
        self.references -= 1;
    }
    fn irqValid(self: *const Ownership, irq: IrqGrant) bool {
        return self.valid(irq.device) and irq.epoch != 0 and irq.epoch == @atomicLoad(u64, &self.active_epoch, .acquire);
    }
    fn acquireIrq(self: *Ownership, grant: Grant, io: anytype) ?IrqGrant {
        if (!self.valid(grant) or self.active_epoch != 0 or self.references == std.math.maxInt(u32) or self.next_epoch == std.math.maxInt(u64)) return null;
        if (!io.maskAndDrain()) {
            self.poisoned = true;
            return null;
        }
        self.next_epoch += 1;
        self.references += 1;
        @atomicStore(u64, &self.active_epoch, self.next_epoch, .release);
        return .{ .device = grant, .epoch = self.next_epoch };
    }
    fn retire(self: *Ownership, irq: IrqGrant, io: anytype) bool {
        if (!self.irqValid(irq) or !io.maskAndDrain()) return false;
        @atomicStore(u64, &self.active_epoch, 0, .release);
        self.release(irq.device);
        return true;
    }
};

const InitPhase = enum { firmware_evidence, madt, boot_uc_mapping, prepared, interrupt_remapping, destination, controller_geometry, firmware_route, firmware_drain, native_route, ready };
const State = struct {
    prepared_madt: ?[]const u8 = null,
    registers: ?mmio.Borrow = null,
    descriptor: Descriptor = .{},
    pin: u8 = 0,
    destination: u8 = 0,
    ready: bool = false,
    phase: InitPhase = .firmware_evidence,
    ownership: Ownership = .{},
    test_io: if (@import("builtin").is_test) struct { masked: bool = true, drain_ok: bool = true, operations: usize = 0 } else void = if (@import("builtin").is_test) .{} else {},
};
var state: State = .{};
pub fn staticStart() usize {
    return @intFromPtr(&state);
}
pub fn staticEnd() usize {
    return @intFromPtr(&state) + @sizeOf(State);
}

pub fn prepareBootMmio(rsdp: u64) void {
    state = .{};
    if (!has8042(acpi.find(rsdp, "FACP", 111) orelse return)) return;
    state.phase = .madt;
    const madt = acpi.find(rsdp, "APIC", 44) orelse return;
    _ = keyboardRoute(madt) catch return;
    state.phase = .boot_uc_mapping;
    var snapshot: @import("arch/x86_64/mtrr.zig").CacheSnapshot = .{};
    if (!@import("arch/x86_64/mtrr.zig").captureCacheSnapshot(&snapshot)) return;
    var entries = Entries{ .bytes = madt };
    while (entries.next() catch return) |entry| {
        if (entry[0] != 1) continue;
        const ctl = controller(entry) catch return;
        // This is the serial pre-AP mapping phase, identical to HPET's UC
        // admission. No late cache change or identity RAM alias is permitted.
        if (!snapshot.rangeSupportsUncachedMapping(ctl.base, 4096) or !arch.mapBootMmioRange(ctl.base, 4096)) return;
    }
    state.prepared_madt = madt;
    state.phase = .prepared;
}

fn regRead(registers: mmio.Borrow, index: u8) u32 {
    registers.write(u32, 0, index);
    return registers.read(u32, 0x10);
}
fn regWrite(registers: mmio.Borrow, index: u8, value: u32) void {
    registers.write(u32, 0, index);
    registers.write(u32, 0x10, value);
}
fn lowIndex() u8 {
    return 0x10 + state.pin * 2;
}
fn routeLow(masked: bool) u32 {
    return interrupt_vector |
        (if (state.descriptor.irq_flags & abi.platform_irq_active_low != 0) @as(u32, 1 << 13) else 0) |
        (if (state.descriptor.irq_flags & abi.platform_irq_level != 0) @as(u32, 1 << 15) else 0) |
        (if (masked) @as(u32, 1 << 16) else 0);
}

pub fn initialize() void {
    defer if (!state.ready) @import("kernel_log.zig").writeFmt("platform: i8042 unavailable phase={s}\n", .{@tagName(state.phase)});
    const madt = state.prepared_madt orelse return;
    defer state.prepared_madt = null;
    // A legacy IOAPIC destination is not an IRTE. Never interpret it as one
    // when firmware or a future IOMMU implementation owns interrupt remapping.
    state.phase = .interrupt_remapping;
    if (!@import("iommu.zig").legacyInterruptRoutingAllowed()) return;
    const route = keyboardRoute(madt) catch return;
    state.phase = .destination;
    const destination = legacyDestination(lapic.localApicId()) orelse return;
    state.phase = .controller_geometry;
    var selected: ?mmio.Borrow = null;
    var selected_pin: u8 = 0;
    var outer = Entries{ .bytes = madt };
    while (outer.next() catch return) |entry| {
        if (entry[0] != 1) continue;
        const ctl = controller(entry) catch return;
        if (smp.explicitUcMmioStatus(ctl.base, 4096) != .ready) return;
        const registers = arch.kernelMmioBorrow(ctl.base, 4096) orelse return;
        const version = regRead(registers, 1);
        const geometry = controllerGeometry(ctl, version, regRead(registers, 0)) catch return;
        const end = geometry.end;
        // Reject duplicate controller identities/apertures and overlapping GSI
        // intervals, even when only one interval contains the keyboard pin.
        var inner = Entries{ .bytes = madt };
        while (inner.next() catch return) |other_entry| {
            if (other_entry.ptr == entry.ptr) break;
            if (other_entry[0] != 1) continue;
            const other = controller(other_entry) catch return;
            if (other.base == ctl.base or other.id == ctl.id) return;
            if (smp.explicitUcMmioStatus(other.base, 4096) != .ready) return;
            const other_registers = arch.kernelMmioBorrow(other.base, 4096) orelse return;
            const other_geometry = controllerGeometry(other, regRead(other_registers, 1), regRead(other_registers, 0)) catch return;
            const other_end = other_geometry.end;
            if (ctl.gsi_base < other_end and other.gsi_base < end) return;
        }
        if (route.gsi >= ctl.gsi_base and route.gsi < end) {
            if (selected != null) return;
            selected = registers;
            selected_pin = @intCast(route.gsi - ctl.gsi_base);
        }
    }
    state.registers = selected orelse return;
    state.pin = selected_pin;
    state.destination = destination;
    state.descriptor.gsi = route.gsi;
    state.descriptor.irq_flags = route.flags;
    // First mask the old route without changing its vector/destination. APs
    // have not entered userland; drain old CPU delivery before replacing it.
    const registers = state.registers.?;
    const old_low = regRead(registers, lowIndex());
    state.phase = .firmware_route;
    @import("kernel_log.zig").writeFmt("platform: i8042 firmware RTE low=0x{x} high=0x{x} GSI={} destination={}\n", .{
        old_low, regRead(registers, lowIndex() + 1), route.gsi, destination,
    });
    regWrite(registers, lowIndex(), old_low | (1 << 16));
    if (regRead(registers, lowIndex()) & (1 << 16) == 0) return;
    // Firmware may have used another vector. Do not assume our vector's drain
    // proves retirement of firmware's pending LAPIC delivery.
    const old_vector: u8 = @truncate(old_low);
    state.phase = .firmware_drain;
    if (old_vector >= 32) {
        // Unknown firmware vectors have no native handler. Servicing one
        // during STI/drain would fault rather than prove safe retirement.
        if (old_vector < interrupt_vector or old_vector >= @import("pci.zig").interrupt_vector_end_exclusive) return;
        if (!smp.drainInterruptRange(old_vector, 1)) return;
    } else if (old_low & (1 << 16) == 0) return;
    if (regRead(registers, lowIndex()) & ((1 << 12) | (1 << 14)) != 0) return;
    state.phase = .native_route;
    regWrite(registers, lowIndex() + 1, @as(u32, state.destination) << 24);
    regWrite(registers, lowIndex(), routeLow(true));
    if (!(NativeIo{}).maskAndDrain()) return;
    state.ready = true;
    state.phase = .ready;
    @import("kernel_log.zig").writeFmt("platform: i8042 ports=60,64 IRQ1 GSI={} flags={} vector=0x{x} masked\n", .{ route.gsi, route.flags, interrupt_vector });
}

const NativeIo = struct {
    fn setMask(_: @This(), masked: bool) bool {
        if (comptime @import("builtin").is_test) {
            state.test_io.masked = masked;
            return true;
        }
        const registers = state.registers orelse return false;
        const before = regRead(registers, lowIndex());
        if ((before & ~@as(u32, (1 << 12) | (1 << 14) | (1 << 16))) != routeLow(false) or
            regRead(registers, lowIndex() + 1) != @as(u32, state.destination) << 24) return false;
        // Preserve status bits even on implementations exposing Remote IRR
        // as writable. Masking must never manufacture the drain's evidence.
        regWrite(registers, lowIndex(), maskWord(before, masked));
        const readback = regRead(registers, lowIndex());
        // Hardware status is not route configuration. Every owned route bit
        // and the physical 8-bit destination must match what we programmed.
        return (readback & ~@as(u32, (1 << 12) | (1 << 14))) == routeLow(masked) and
            regRead(registers, lowIndex() + 1) == @as(u32, state.destination) << 24;
    }
    fn maskAndDrain(self: @This()) bool {
        if (!self.setMask(true)) return false;
        if (comptime @import("builtin").is_test) return state.test_io.drain_ok;
        if (!smp.drainInterruptRange(interrupt_vector, 1)) return false;
        const registers = state.registers.?;
        return regRead(registers, lowIndex()) & ((1 << 12) | (1 << 14)) == 0;
    }
};

fn maskWord(before: u32, masked: bool) u32 {
    return (before & ~@as(u32, 1 << 16)) | (if (masked) @as(u32, 1 << 16) else 0);
}

pub fn count() usize {
    return if (state.ready and !state.ownership.poisoned) 1 else 0;
}
pub fn at(index: usize) ?Descriptor {
    return if (index == 0 and count() != 0) state.descriptor else null;
}
pub fn claim(index: usize) ?Grant {
    if (at(index) == null) return null;
    return state.ownership.claim();
}
pub fn release(grant: Grant) void {
    state.ownership.release(grant);
}
pub fn valid(grant: Grant) bool {
    return state.ready and state.ownership.valid(grant);
}
pub fn acquireIrq(grant: Grant) ?IrqGrant {
    return state.ownership.acquireIrq(grant, NativeIo{});
}
pub fn irqValid(grant: IrqGrant) bool {
    return state.ownership.irqValid(grant);
}
pub fn quiesce(grant: IrqGrant) bool {
    return irqValid(grant) and (NativeIo{}).maskAndDrain();
}
pub fn retire(grant: IrqGrant) bool {
    return state.ownership.retire(grant, NativeIo{});
}
/// Caller holds K and has successfully quiesced this exact epoch. No source
/// unmask or claim can interleave between quiesce and this non-failing commit.
pub fn commitRetirement(grant: IrqGrant) void {
    std.debug.assert(irqValid(grant));
    @atomicStore(u64, &state.ownership.active_epoch, 0, .release);
    state.ownership.release(grant.device);
}
pub fn closeIrq(grant: IrqGrant) void {
    if (!irqValid(grant)) return;
    if (retire(grant)) return;
    // A failed last-close drain cannot retain an FD that is disappearing.
    // Remove publication, poison the physical resource, discharge its strong
    // reference, and prohibit any future owner from reusing the route.
    state.ownership.poisoned = true;
    @atomicStore(u64, &state.ownership.active_epoch, 0, .release);
    state.ownership.release(grant.device);
}
pub fn setMask(grant: IrqGrant, masked: bool) bool {
    if (!irqValid(grant)) return false;
    return if (masked) quiesce(grant) else (NativeIo{}).setMask(false);
}
pub fn interruptSource() ?IrqGrant {
    const epoch = @atomicLoad(u64, &state.ownership.active_epoch, .acquire);
    if (epoch == 0) return null;
    // generation is immutable while an epoch is published; the all-CPU drain
    // precedes both epoch withdrawal and generation reuse.
    return .{ .device = .{ .generation = state.ownership.generation }, .epoch = epoch };
}
pub fn interruptGsi() u64 {
    return state.descriptor.gsi;
}
pub fn ioAllowed(grant: Grant, port: u64, width: u64, write: bool, controller_write: bool, value: u64) bool {
    return valid(grant) and pioBounds(port, width, write, controller_write, value);
}
fn pioBounds(port: u64, width: u64, write: bool, controller_write: bool, value: u64) bool {
    // The controller's command/data state survives FD attenuation and owner
    // handoff. A data-port byte can complete D1/output-port and reset the CPU.
    // Treat every write as controller authority without decoding driver policy.
    return width == 1 and (port == 0x60 or port == 0x64) and
        (!write or (value <= 255 and controller_write));
}
pub fn ioRead(port: u16) u8 {
    if (comptime @import("builtin").is_test) {
        state.test_io.operations += 1;
        return 0x5a;
    }
    return asm volatile ("inb %[port], %[value]"
        : [value] "={al}" (-> u8),
        : [port] "{dx}" (port),
    );
}
pub fn ioWrite(port: u16, value: u8) void {
    if (comptime @import("builtin").is_test) {
        state.test_io.operations += 1;
        return;
    }
    asm volatile ("outb %[value], %[port]"
        :
        : [value] "{al}" (value),
          [port] "{dx}" (port),
        : .{ .memory = true });
}

pub const TestSupport = if (@import("builtin").is_test) struct {
    pub fn begin() void {
        state = .{ .ready = true };
    }
    pub fn end() void {
        std.debug.assert(state.ownership.references == 0);
        state = .{};
    }
    pub fn masked() bool {
        return state.test_io.masked;
    }
    pub fn failDrain(fail: bool) void {
        state.test_io.drain_ok = !fail;
    }
    pub fn references() u32 {
        return state.ownership.references;
    }
    pub fn ioOperations() usize {
        return state.test_io.operations;
    }
} else struct {};

fn fixtureChecksum(bytes: []u8) void {
    std.mem.writeInt(u32, bytes[4..8], @intCast(bytes.len), .little);
    bytes[9] = 0;
    var sum: u8 = 0;
    for (bytes) |byte| sum +%= byte;
    bytes[9] = 0 -% sum;
}

test "platform FADT presence requires checked explicit 8042 evidence" {
    var fadt: [111]u8 = @splat(0);
    @memcpy(fadt[0..4], "FACP");
    fadt[8] = 6;
    fixtureChecksum(&fadt);
    try std.testing.expect(!has8042(&fadt));
    fadt[109] = 2;
    fixtureChecksum(&fadt);
    try std.testing.expect(has8042(&fadt));
    fadt[8] = 1;
    fixtureChecksum(&fadt);
    try std.testing.expect(!has8042(&fadt));
    fadt[8] = 6;
    fixtureChecksum(&fadt);
    try std.testing.expect(!has8042(fadt[0..110]));
    fadt[110] = 1;
    try std.testing.expect(!has8042(&fadt));
    fixtureChecksum(&fadt);
    try std.testing.expect(has8042(&fadt));
    fadt[109] = 1;
    fixtureChecksum(&fadt);
    try std.testing.expect(!has8042(&fadt));
}

test "platform MADT keyboard ISO admits exact ISA route and rejects malformed shared reserved flags" {
    var madt: [66]u8 = @splat(0);
    @memcpy(madt[0..4], "APIC");
    @memcpy(madt[44..56], &[_]u8{ 1, 12, 2, 0, 0, 0, 0xc0, 0xfe, 0, 0, 0, 0 });
    @memcpy(madt[56..66], &[_]u8{ 2, 10, 0, 1, 17, 0, 0, 0, 15, 0 });
    fixtureChecksum(&madt);
    const route = try keyboardRoute(&madt);
    try std.testing.expectEqual(@as(u32, 17), route.gsi);
    try std.testing.expectEqual(abi.platform_irq_level | abi.platform_irq_active_low, route.flags);
    for ([_]u8{ 2, 8, 16, 255 }) |flags| {
        madt[64] = flags;
        fixtureChecksum(&madt);
        try std.testing.expectError(error.Invalid, keyboardRoute(&madt));
    }
    madt[64] = 0;
    madt[60] = 9;
    fixtureChecksum(&madt);
    try std.testing.expectError(error.Invalid, keyboardRoute(&madt));
    madt[60] = 1;
    fixtureChecksum(&madt);
    try std.testing.expectEqual(@as(u32, 1), (try keyboardRoute(&madt)).gsi);
    madt[58] = 1;
    fixtureChecksum(&madt);
    try std.testing.expectError(error.Invalid, keyboardRoute(&madt));
    madt[58] = 0;
    madt[57] = 11;
    fixtureChecksum(&madt);
    try std.testing.expectError(error.Invalid, keyboardRoute(&madt));
    madt[57] = 0;
    fixtureChecksum(&madt);
    try std.testing.expectError(error.Invalid, keyboardRoute(&madt));
    // NEC's non-keyboard overrides must not steal IRQ1 or impose their flags.
    madt[57] = 10;
    madt[59] = 9;
    madt[60] = 9;
    madt[64] = 13;
    fixtureChecksum(&madt);
    try std.testing.expectEqual(Route{}, try keyboardRoute(&madt));
}

test "platform PIO exact port width and reset authority boundary" {
    try std.testing.expect(!pioBounds(0x60, 1, true, false, 255));
    try std.testing.expect(pioBounds(0x60, 1, true, true, 255));
    try std.testing.expect(pioBounds(0x64, 1, false, false, 0));
    try std.testing.expect(!pioBounds(0x64, 1, true, false, 0));
    try std.testing.expect(pioBounds(0x64, 1, true, true, 0xfe));
    for ([_]u64{ 0, 0x5f, 0x61, 0x63, 0x65, 0x10060, std.math.maxInt(u64) }) |port|
        try std.testing.expect(!pioBounds(port, 1, true, true, 0));
    for ([_]u64{ 0, 2, 4, 8, 256 }) |width|
        try std.testing.expect(!pioBounds(0x60, width, false, false, 0));
    try std.testing.expect(!pioBounds(0x60, 1, true, true, 256));
}

test "platform IOAPIC geometry APIC destination and mask preserve hardware status" {
    try std.testing.expectEqual(@as(?u8, 255), legacyDestination(255));
    try std.testing.expect(legacyDestination(256) == null);
    try std.testing.expect(legacyDestination(std.math.maxInt(u32)) == null);
    const ctl = Controller{ .base = 0xfec00000, .gsi_base = 0, .id = 2 };
    try std.testing.expectEqual(@as(u32, 120), (try controllerGeometry(ctl, (119 << 16) | 0x20, 2 << 24)).end);
    try std.testing.expectError(error.Invalid, controllerGeometry(ctl, (120 << 16) | 0x20, 2 << 24));
    try std.testing.expectError(error.Invalid, controllerGeometry(ctl, 0xffffffff, 2 << 24));
    try std.testing.expectError(error.Invalid, controllerGeometry(ctl, 0x20, 3 << 24));
    var overflow = ctl;
    overflow.gsi_base = std.math.maxInt(u32);
    try std.testing.expectError(error.Invalid, controllerGeometry(overflow, 0x20, 2 << 24));
    const status: u32 = (1 << 12) | (1 << 14);
    const route: u32 = @as(u32, interrupt_vector) | (1 << 13) | (1 << 15);
    try std.testing.expectEqual(route | status | (1 << 16), maskWord(route | status, true));
    try std.testing.expectEqual(route | status, maskWord(route | status | (1 << 16), false));
}

test "platform IRQ lease survives device close, drain failure blocks reuse, epochs reject aliases" {
    const Fake = struct {
        drained: bool,
        fn maskAndDrain(self: @This()) bool {
            return self.drained;
        }
    };
    var owner = Ownership{};
    const grant = owner.claim().?;
    try std.testing.expect(owner.claim() == null);
    const irq = owner.acquireIrq(grant, Fake{ .drained = true }).?;
    try std.testing.expect(owner.acquireIrq(grant, Fake{ .drained = true }) == null);
    owner.release(grant);
    try std.testing.expect(owner.valid(grant));
    try std.testing.expect(!owner.retire(irq, Fake{ .drained = false }));
    try std.testing.expect(owner.irqValid(irq));
    try std.testing.expect(owner.claim() == null);
    try std.testing.expect(owner.retire(irq, Fake{ .drained = true }));
    const next = owner.claim().?;
    try std.testing.expect(!owner.valid(grant));
    const next_irq = owner.acquireIrq(next, Fake{ .drained = true }).?;
    try std.testing.expect(!owner.irqValid(irq));
    try std.testing.expect(next_irq.epoch != irq.epoch);
    try std.testing.expect(!owner.retire(irq, Fake{ .drained = true }));
    try std.testing.expect(owner.irqValid(next_irq));
    try std.testing.expect(owner.retire(next_irq, Fake{ .drained = true }));
    owner.release(next);
    const failing = owner.claim().?;
    try std.testing.expect(owner.acquireIrq(failing, Fake{ .drained = false }) == null);
    owner.release(failing);
    try std.testing.expectEqual(@as(u32, 0), owner.references);
    try std.testing.expect(owner.claim() == null);
}
