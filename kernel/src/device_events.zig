const kernel = @import("kernel.zig");
const kernel_log = @import("kernel_log.zig");
const scheduler = @import("scheduler.zig");

pub const generic_device_interrupt_vector: u8 = 0x41;
pub const msix_device_interrupt_vector_base: u8 = 0x42;
pub const msix_device_interrupt_vector_count: u8 = 32;
const max_device_event_bindings: usize = kernel.capsule.CapsuleTable.max_capsules;

const DeviceEventBinding = struct {
    valid: bool = false,
    device: kernel.DmaDeviceId = 0,
    owner: kernel.PrincipalId = @enumFromInt(0),
    kind: kernel.CapsuleIrqKind = .auto,
    vector: u32 = 0,
    interrupts: u64 = 0,
    refs: u32 = 0,
};

var bindings: [max_device_event_bindings]DeviceEventBinding = [_]DeviceEventBinding{.{}} ** max_device_event_bindings;
var interrupt_count: u64 = 0;
var trace_bind_count: u32 = 0;
var trace_interrupt_count: u32 = 0;

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn staticStorageStart(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr);
}

pub fn kernelStaticStorageStartAddr() usize {
    var start = staticStorageStart(@TypeOf(bindings), &bindings);
    const counter_start = staticStorageStart(@TypeOf(interrupt_count), &interrupt_count);
    if (counter_start < start) start = counter_start;
    const trace_bind_start = staticStorageStart(@TypeOf(trace_bind_count), &trace_bind_count);
    if (trace_bind_start < start) start = trace_bind_start;
    const trace_interrupt_start = staticStorageStart(@TypeOf(trace_interrupt_count), &trace_interrupt_count);
    if (trace_interrupt_start < start) start = trace_interrupt_start;
    return start;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end = staticStorageEnd(@TypeOf(bindings), &bindings);
    const counter_end = staticStorageEnd(@TypeOf(interrupt_count), &interrupt_count);
    if (counter_end > end) end = counter_end;
    const trace_bind_end = staticStorageEnd(@TypeOf(trace_bind_count), &trace_bind_count);
    if (trace_bind_end > end) end = trace_bind_end;
    const trace_interrupt_end = staticStorageEnd(@TypeOf(trace_interrupt_count), &trace_interrupt_count);
    if (trace_interrupt_end > end) end = trace_interrupt_end;
    return end;
}

fn findBinding(owner: kernel.PrincipalId, device: kernel.DmaDeviceId, kind: kernel.CapsuleIrqKind, vector: u32) ?*DeviceEventBinding {
    if (device == 0) return null;
    for (&bindings) |*entry| {
        if (!entry.valid) continue;
        if (entry.device == device and entry.owner == owner and entry.kind == kind and entry.vector == vector) return entry;
    }
    return null;
}

fn ensureBinding(owner: kernel.PrincipalId, device: kernel.DmaDeviceId, kind: kernel.CapsuleIrqKind, vector: u32) ?*DeviceEventBinding {
    if (device == 0) return null;
    if (findBinding(owner, device, kind, vector)) |entry| return entry;
    for (&bindings) |*entry| {
        if (entry.valid) continue;
        entry.* = .{
            .valid = true,
            .device = device,
            .owner = owner,
            .kind = kind,
            .vector = vector,
            .interrupts = 0,
        };
        return entry;
    }
    return null;
}

pub fn bindDeviceEvent(owner: kernel.PrincipalId, device: kernel.DmaDeviceId, kind: kernel.CapsuleIrqKind, vector: u32) bool {
    const ok = ensureBinding(owner, device, kind, vector) != null;
    if (ok and trace_bind_count < 16) {
        trace_bind_count += 1;
        kernel_log.writeFmt(
            "device-events: bind owner={} device={} kind={} vector={}\n",
            .{ @intFromEnum(owner), device, @intFromEnum(kind), vector },
        );
    }
    return ok;
}

pub fn acquireDeviceEvent(owner: kernel.PrincipalId, device: kernel.DmaDeviceId, kind: kernel.CapsuleIrqKind, vector: u32) bool {
    const entry = ensureBinding(owner, device, kind, vector) orelse return false;
    entry.refs +|= 1;
    if (entry.refs == 0) entry.refs = 1;
    if (trace_bind_count < 16) {
        trace_bind_count += 1;
        kernel_log.writeFmt(
            "device-events: acquire owner={} device={} kind={} vector={} refs={}\n",
            .{ @intFromEnum(owner), device, @intFromEnum(kind), vector, entry.refs },
        );
    }
    return true;
}

pub fn releaseDeviceEvent(owner: kernel.PrincipalId, device: kernel.DmaDeviceId, kind: kernel.CapsuleIrqKind, vector: u32) bool {
    const entry = findBinding(owner, device, kind, vector) orelse return false;
    if (entry.refs > 1) {
        entry.refs -= 1;
    } else {
        entry.* = .{};
    }
    return true;
}

pub fn releaseOwner(owner: kernel.PrincipalId) usize {
    var released: usize = 0;
    for (&bindings) |*entry| {
        if (!entry.valid or entry.owner != owner) continue;
        entry.* = .{};
        released += 1;
    }
    return released;
}

pub fn interruptCountFor(owner: kernel.PrincipalId, device: kernel.DmaDeviceId, kind: kernel.CapsuleIrqKind, vector: u32) ?u64 {
    if (device == 0) return null;
    for (&bindings) |*entry| {
        if (!entry.valid) continue;
        if (entry.device == device and entry.owner == owner and entry.kind == kind and entry.vector == vector) return entry.interrupts;
    }
    return null;
}

pub fn msixEntryForInterruptVector(vector: u8) ?u32 {
    if (vector < msix_device_interrupt_vector_base) return null;
    const entry = vector - msix_device_interrupt_vector_base;
    if (entry >= msix_device_interrupt_vector_count) return null;
    return entry;
}

pub fn wakeBoundDeviceWaiters(kind: kernel.CapsuleIrqKind, vector: u32) usize {
    interrupt_count +%= 1;
    var woke: usize = 0;
    for (&bindings) |*entry| {
        if (!entry.valid) continue;
        if (entry.kind != kind or entry.vector != vector) continue;
        entry.interrupts +%= 1;
        scheduler.wakeBlockedThreadForPrincipal(entry.owner);
        woke += 1;
    }
    if (trace_interrupt_count < 32) {
        trace_interrupt_count += 1;
        kernel_log.writeFmt(
            "device-events: interrupt global={} kind={} vector={} woke={}\n",
            .{ interrupt_count, @intFromEnum(kind), vector, woke },
        );
    }
    return woke;
}

pub fn wakeAllBoundDeviceWaiters() usize {
    interrupt_count +%= 1;
    var woke: usize = 0;
    for (&bindings) |*entry| {
        if (!entry.valid) continue;
        entry.interrupts +%= 1;
        scheduler.wakeBlockedThreadForPrincipal(entry.owner);
        woke += 1;
    }
    if (trace_interrupt_count < 32) {
        trace_interrupt_count += 1;
        kernel_log.writeFmt(
            "device-events: interrupt global={} woke={}\n",
            .{ interrupt_count, woke },
        );
    }
    return woke;
}

pub fn interruptCount() u64 {
    return interrupt_count;
}
