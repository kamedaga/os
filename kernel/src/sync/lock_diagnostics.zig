const builtin = @import("builtin");
const root = @import("root");
pub const order = @import("lock_order.zig");
pub const enabled = if (@hasDecl(root, "kernel_lock_checks_enabled")) root.kernel_lock_checks_enabled else false;
pub const Record = if (enabled) order.Record else struct {};
// A single field preserves the parent auto-layout in off builds: even a
// zero-size sibling can change Zig auto-layout field ordering.
pub const Byte = struct { value: u8 = 0, record: Record = .{} };
const boot_count = if (!enabled) 0 else if (builtin.is_test) root.kernel_lock_cpu_count else 1;
var cpus_boot: [boot_count]order.Cpu = @splat(.{});
var cpus: []order.Cpu = &cpus_boot;
pub fn initCpuStorage(count: usize) !void {
    if (comptime !enabled) return;
    assertInterruptsDisabled();
    // Cpu contains lock-owned record pointers, never pointers back into this
    // directory. Copying the BSP head/depth preserves existing lock holds.
    cpus = try @import("../diagnostic_cpu_storage.zig").grow(order.Cpu, cpus, count, .{});
}

pub fn failure(err: anyerror) noreturn {
    if (builtin.is_test) @panic(@errorName(err));
    asm volatile ("cli" ::: .{ .memory = true });
    // No normal panic/GOP/log callback: the rejected path may hold precisely
    // the locks those diagnostics need. Raw UART output neither locks nor allocates.
    const serial = @import("../serial.zig");
    serial.writeRaw("[kernel] lock diagnostic fault: ");
    serial.writeRaw(@errorName(err));
    serial.writeRaw("\n");
    while (true) asm volatile ("hlt");
}

fn assertInterruptsDisabled() void {
    if (builtin.is_test) return;
    var flags: u64 = 0;
    asm volatile ("pushfq; pop %[flags]"
        : [flags] "=r" (flags),
        :
        : .{ .memory = true });
    if ((flags & (1 << 9)) != 0) failure(error.InterruptsEnabled);
}

fn currentCpu() usize {
    const cpu = root.lockDiagnosticsCpuSlot() orelse failure(error.UnknownCpu);
    if (cpu >= cpus.len) failure(error.InvalidCpu);
    return cpu;
}

pub fn beforeAcquire(record: *Record, key: order.Key) void {
    if (comptime enabled) {
        assertInterruptsDisabled();
        cpus[currentCpu()].beforeAcquire(record, key) catch |err| failure(err);
    }
}

pub fn acquired(record: *Record, key: order.Key) void {
    if (comptime enabled) {
        assertInterruptsDisabled();
        cpus[currentCpu()].acquired(record, key) catch |err| failure(err);
    }
}

pub fn releasing(record: *Record) void {
    if (comptime enabled) {
        assertInterruptsDisabled();
        cpus[currentCpu()].releasing(record) catch |err| failure(err);
    }
}

pub fn assertRootEmpty() void {
    if (comptime enabled) {
        assertInterruptsDisabled();
        cpus[currentCpu()].assertQuiescent() catch |err| failure(err);
    }
}

// Boot initialization overwrites embedded lock records. It may not erase
// live holds; this is a quiescent/reset assertion, not a remote sampling API.
pub fn assertAllQuiescent() void {
    if (comptime enabled) {
        assertInterruptsDisabled();
        for (cpus) |*cpu| cpu.assertQuiescent() catch |err| failure(err);
    }
}

pub const InterruptScope = struct {
    cpu: usize = 0,
    depth: ?usize = null,

    pub fn end(self: InterruptScope) void {
        if (comptime enabled) {
            assertInterruptsDisabled();
            if (currentCpu() != self.cpu) failure(error.ContextMovedCpu);
            if (self.depth) |depth| {
                cpus[self.cpu].leaveInterrupt(depth) catch |err| failure(err);
            } else {
                assertRootEmpty();
            }
        }
    }
};

pub fn enterInterrupt(from_user: bool) InterruptScope {
    if (comptime enabled) {
        assertInterruptsDisabled();
        const cpu = currentCpu();
        // A user boundary has no interrupted kernel owner. Do not push a
        // context whose defer could be abandoned by AP noreturn redispatch.
        if (from_user) {
            assertRootEmpty();
            return .{ .cpu = cpu };
        }
        const depth = cpus[cpu].enterInterrupt() catch |err| failure(err);
        return .{ .cpu = cpu, .depth = depth };
    }
    return .{};
}

pub fn staticEnd() usize {
    if (comptime enabled) return @max(@intFromPtr(&cpus) + @sizeOf(@TypeOf(cpus)), @intFromPtr(&cpus_boot) + @sizeOf(@TypeOf(cpus_boot)));
    return 0;
}

// Only the diagnostic build masks IF here. CAS + record publication and
// unlink + release must be indivisible to local IRQ observers, even when
// the underlying lock permits IF=1 outside its acquisition attempt.
pub inline fn maskInterrupts() bool {
    if (comptime !enabled or builtin.is_test) return false;
    const flags = asm volatile ("pushfq; pop %[flags]; cli"
        : [flags] "=r" (-> u64),
        :
        : .{ .memory = true });
    return flags & (1 << 9) != 0;
}

pub inline fn restoreInterrupts(restore: bool) void {
    if (comptime enabled and !builtin.is_test) {
        if (restore) asm volatile ("sti" ::: .{ .memory = true });
    }
}

pub fn acquiredTry(record: *Record, key: order.Key) void {
    if (comptime enabled) {
        assertInterruptsDisabled();
        cpus[currentCpu()].acquiredMode(record, key, .try_once) catch |err| failure(err);
    }
}

pub fn assertOwned(record: *Record) void {
    if (comptime enabled) {
        assertInterruptsDisabled();
        cpus[currentCpu()].assertOwned(record) catch |err| failure(err);
    }
}

pub fn recursive(record: *Record) bool {
    if (comptime enabled) {
        assertInterruptsDisabled();
        return cpus[currentCpu()].recursive(record) catch |err| failure(err);
    }
    return false;
}

pub fn assertCanWaitForIpi() void {
    if (comptime enabled) {
        const restore = maskInterrupts();
        defer restoreInterrupts(restore);
        assertInterruptsDisabled();
        cpus[currentCpu()].assertCanWaitForIpi() catch |err| failure(err);
    }
}

pub fn testCpu() *order.Cpu {
    if (!builtin.is_test or !enabled) @compileError("diagnostic host test only");
    return &cpus[currentCpu()];
}

test "lock ordering CPU migration preserves BSP holds and admits CPU 71" {
    if (comptime !enabled or !@hasDecl(root, "setTestLockDiagnosticsCpu")) return;
    const std = @import("std");
    root.setTestLockDiagnosticsCpu(0);
    var record: Record = .{};
    const key: order.Key = .{ .class = .kernel_state };
    acquired(&record, key);
    try initCpuStorage(72);
    defer {
        std.testing.allocator.free(cpus);
        @memset(&cpus_boot, .{});
        cpus = &cpus_boot;
        root.setTestLockDiagnosticsCpu(0);
    }
    assertOwned(&record);
    releasing(&record);
    root.setTestLockDiagnosticsCpu(71);
    var last_record: Record = .{};
    acquired(&last_record, key);
    assertOwned(&last_record);
    releasing(&last_record);
    assertAllQuiescent();
}
