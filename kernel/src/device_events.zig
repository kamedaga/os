const kernel = @import("kernel.zig");
const scheduler = @import("scheduler.zig");

pub const generic_device_interrupt_vector: u8 = 0x41;
const max_device_event_bindings: usize = 16;

const DeviceEventBinding = struct {
    valid: bool = false,
    device: kernel.DmaDeviceId = 0,
    owner: kernel.PrincipalId = @enumFromInt(0),
    interrupts: u64 = 0,
    refs: u32 = 0,
};

var bindings: [max_device_event_bindings]DeviceEventBinding = [_]DeviceEventBinding{.{}} ** max_device_event_bindings;
var interrupt_count: u64 = 0;

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
    return start;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end = staticStorageEnd(@TypeOf(bindings), &bindings);
    const counter_end = staticStorageEnd(@TypeOf(interrupt_count), &interrupt_count);
    if (counter_end > end) end = counter_end;
    return end;
}

fn findBinding(owner: kernel.PrincipalId, device: kernel.DmaDeviceId) ?*DeviceEventBinding {
    if (device == 0) return null;
    for (&bindings) |*entry| {
        if (!entry.valid) continue;
        if (entry.device == device and entry.owner == owner) return entry;
    }
    return null;
}

fn ensureBinding(owner: kernel.PrincipalId, device: kernel.DmaDeviceId) ?*DeviceEventBinding {
    if (device == 0) return null;
    if (findBinding(owner, device)) |entry| return entry;
    for (&bindings) |*entry| {
        if (entry.valid) continue;
        entry.* = .{
            .valid = true,
            .device = device,
            .owner = owner,
            .interrupts = 0,
        };
        return entry;
    }
    return null;
}

pub fn bindDeviceEvent(owner: kernel.PrincipalId, device: kernel.DmaDeviceId) bool {
    return ensureBinding(owner, device) != null;
}

pub fn acquireDeviceEvent(owner: kernel.PrincipalId, device: kernel.DmaDeviceId) bool {
    const entry = ensureBinding(owner, device) orelse return false;
    entry.refs +|= 1;
    if (entry.refs == 0) entry.refs = 1;
    return true;
}

pub fn releaseDeviceEvent(owner: kernel.PrincipalId, device: kernel.DmaDeviceId) bool {
    const entry = findBinding(owner, device) orelse return false;
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

pub fn interruptCountFor(owner: kernel.PrincipalId, device: kernel.DmaDeviceId) ?u64 {
    if (device == 0) return null;
    for (&bindings) |*entry| {
        if (!entry.valid) continue;
        if (entry.device == device and entry.owner == owner) return entry.interrupts;
    }
    return null;
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
    return woke;
}

pub fn interruptCount() u64 {
    return interrupt_count;
}
