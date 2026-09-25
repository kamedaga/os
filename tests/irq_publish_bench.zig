//! Host-only benchmark of the actual kernel IRQ publication path. This is not
//! a page-load benchmark and its times must not be presented as browser gains.
const std = @import("std");
const kernel = @import("kernel");

const Timespec = extern struct { sec: c_long, nsec: c_long };
extern "c" fn clock_gettime(clock: c_int, value: *Timespec) c_int;
var state: kernel.KernelState = .{};

fn now() u64 {
    var value: Timespec = undefined;
    std.debug.assert(clock_gettime(1, &value) == 0);
    return @as(u64, @intCast(value.sec)) * 1_000_000_000 + @as(u64, @intCast(value.nsec));
}

pub fn main() !void {
    const owner = kernel.processPrincipalFromIndex(0).?;
    const rounds = 10000;
    var owners: [16]kernel.PrincipalId = undefined;
    for ([_]usize{ 1, 16, 128 }) |occupied| {
        for (0..occupied) |i| {
            const index = (kernel.max_fd_objects - 1) * (i + 1) / occupied;
            state.publishIrqObject(.{ .kind = .irq, .index = @intCast(index), .generation = 1 }, .{
                .owner_principal_raw = @intFromEnum(owner),
                .device = if (i == occupied - 1) 0x1001 else 0x1002,
                .kind = .msix,
                .vector = 1,
            });
        }
        for (0..5) |trial| {
            const start = now();
            var wakes: usize = 0;
            for (0..rounds) |_| wakes += state.recordDeviceInterruptEvent(0x1001, 1, &owners);
            const elapsed = now() - start;
            if (wakes != rounds) return error.WrongWakeCount;
            std.debug.print("IRQ_PUBLISH_BENCH occupied={d} trial={d} rounds={d} ns_per_irq={d}\n", .{
                occupied, trial, rounds, elapsed / rounds,
            });
        }
        for (0..occupied) |i| {
            const index = (kernel.max_fd_objects - 1) * (i + 1) / occupied;
            state.unpublishIrqObject(.{ .kind = .irq, .index = @intCast(index), .generation = 1 });
        }
    }
}
