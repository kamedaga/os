const kernel_log = @import("kernel_log.zig");
const x86_platform = @import("arch/x86_64/platform.zig");

const report_every: u64 = 4096;

pub const Op = enum(usize) {
    wait_register,
    wait_repoll,
    send_enqueue,
    send_wake,
    wake_to_resume,
    wake_to_resume_same_cpu,
    wake_to_resume_remote_cpu,
    _count,
};

const op_count = @intFromEnum(Op._count);

const Metric = struct {
    count: u64 = 0,
    total_cycles: u64 = 0,
    max_cycles: u64 = 0,
};

const SpinLock = struct {
    value: u8 = 0,

    fn lock(self: *SpinLock) void {
        while (true) {
            if (@cmpxchgWeak(u8, &self.value, 0, 1, .acquire, .monotonic) == null) return;
            while (@atomicLoad(u8, &self.value, .monotonic) != 0) {
                asm volatile ("pause");
            }
        }
    }

    fn unlock(self: *SpinLock) void {
        @atomicStore(u8, &self.value, 0, .release);
    }
};

var lock_state: SpinLock = .{};
var metrics: [op_count]Metric = [_]Metric{.{}} ** op_count;
var next_report: u64 = report_every;

pub fn timestamp() u64 {
    return x86_platform.readTimestampCounter();
}

fn avg(metric: Metric) u64 {
    return if (metric.count == 0) 0 else metric.total_cycles / metric.count;
}

fn addLocked(op: Op, cycles: u64) void {
    const index = @intFromEnum(op);
    var metric = &metrics[index];
    metric.count +%= 1;
    metric.total_cycles +%= cycles;
    if (cycles > metric.max_cycles) metric.max_cycles = cycles;
}

fn totalCountLocked() u64 {
    var total: u64 = 0;
    for (metrics[0..]) |metric| total +%= metric.count;
    return total;
}

fn reportLocked() void {
    const total = totalCountLocked();
    if (total < next_report) return;
    kernel_log.writeFmt(
        "[ipc-metric] wait_register count={} avg={} max={} wait_repoll count={} avg={} max={}\n",
        .{
            metrics[@intFromEnum(Op.wait_register)].count,
            avg(metrics[@intFromEnum(Op.wait_register)]),
            metrics[@intFromEnum(Op.wait_register)].max_cycles,
            metrics[@intFromEnum(Op.wait_repoll)].count,
            avg(metrics[@intFromEnum(Op.wait_repoll)]),
            metrics[@intFromEnum(Op.wait_repoll)].max_cycles,
        },
    );
    kernel_log.writeFmt(
        "[ipc-metric] send_enqueue count={} avg={} max={} send_wake count={} avg={} max={} wake_to_resume count={} avg={} max={}\n",
        .{
            metrics[@intFromEnum(Op.send_enqueue)].count,
            avg(metrics[@intFromEnum(Op.send_enqueue)]),
            metrics[@intFromEnum(Op.send_enqueue)].max_cycles,
            metrics[@intFromEnum(Op.send_wake)].count,
            avg(metrics[@intFromEnum(Op.send_wake)]),
            metrics[@intFromEnum(Op.send_wake)].max_cycles,
            metrics[@intFromEnum(Op.wake_to_resume)].count,
            avg(metrics[@intFromEnum(Op.wake_to_resume)]),
            metrics[@intFromEnum(Op.wake_to_resume)].max_cycles,
        },
    );
    kernel_log.writeFmt(
        "[ipc-metric] wake_resume_by_cpu same count={} avg={} max={} remote count={} avg={} max={}\n",
        .{
            metrics[@intFromEnum(Op.wake_to_resume_same_cpu)].count,
            avg(metrics[@intFromEnum(Op.wake_to_resume_same_cpu)]),
            metrics[@intFromEnum(Op.wake_to_resume_same_cpu)].max_cycles,
            metrics[@intFromEnum(Op.wake_to_resume_remote_cpu)].count,
            avg(metrics[@intFromEnum(Op.wake_to_resume_remote_cpu)]),
            metrics[@intFromEnum(Op.wake_to_resume_remote_cpu)].max_cycles,
        },
    );
    while (next_report <= total) next_report += report_every;
}

pub fn record(op: Op, start_tsc: u64) void {
    const end = timestamp();
    lock_state.lock();
    defer lock_state.unlock();
    addLocked(op, end -% start_tsc);
    reportLocked();
}

pub fn recordElapsed(op: Op, cycles: u64) void {
    lock_state.lock();
    defer lock_state.unlock();
    addLocked(op, cycles);
    reportLocked();
}
