const std = @import("std");
const root = @import("root");
const smp = @import("smp.zig");
const platform = @import("arch/x86_64/platform.zig");

pub const enabled = if (@hasDecl(root, "smp_profile_enabled")) root.smp_profile_enabled else false;
pub const Counter = enum(usize) {
    kernel_lock_wait_cycles,
    kernel_lock_contended,
    kernel_lock_hold_cycles,
    kernel_lock_acquires,
    tlb_lock_wait_cycles,
    tlb_requests,
    tlb_targets,
    tlb_send_cycles,
    tlb_ack_wait_cycles,
    tlb_received,
    scheduler_lock_wait_cycles,
    scheduler_lock_acquires,
    steal_cycles,
    steal_attempts,
    wake_scan_cycles,
    wake_scan_calls,
};
// One cacheline-separated row per CPU. No hot-path logging or shared lock.
// Diagnostic host tooling reads these through QMP physical-memory snapshots.
pub export var smp_perf_counters: [smp.max_cpus][16]u64 align(128) = std.mem.zeroes([smp.max_cpus][16]u64);

// Separate diagnostic array: preserve the existing SMP counter layout and
// allocate no additional storage in release builds. Counters aggregate all
// native munmap calls (including LPR and native services), not one Linux PID.
pub const MunmapCounter = enum(usize) {
    calls,
    requested_pages,
    total_cycles,
    transaction_lock_cycles,
    pinned_check_cycles,
    reservation_validate_cycles,
    reservation_count_cycles,
    prepare_cycles,
    unmap_cycles,
    commit_cycles,
    successes,
    errors,
    size_1_page,
    size_2_to_4_pages,
    size_5_to_64_pages,
    size_over_64_pages,
};
const munmap_cpu_rows = if (enabled) smp.max_cpus else 0;
var munmap_perf_counters: [munmap_cpu_rows][16]u64 align(128) =
    std.mem.zeroes([munmap_cpu_rows][16]u64);
comptime {
    if (enabled) @export(&munmap_perf_counters, .{ .name = "munmap_perf_counters" });
}

pub fn munmapAdd(counter: MunmapCounter, value: u64) void {
    if (!enabled) return;
    const cpu = smp.currentCpuSlot();
    if (cpu >= smp.max_cpus) return;
    _ = @atomicRmw(u64, &munmap_perf_counters[cpu][@intFromEnum(counter)], .Add, value, .monotonic);
}
pub fn munmapElapsed(counter: MunmapCounter, start: u64) void {
    if (enabled) munmapAdd(counter, timestamp() -% start);
}

pub const VmoCounter = enum(usize) {
    final_drop_calls,
    final_drop_logical_pages,
    final_owned_cycles,
    final_clear_cycles,
    partial_calls,
    partial_fd_scan_cycles,
    partial_vma_scan_cycles,
    partial_page_cycles,
    partial_requested_pages,
    partial_fd_alias,
    partial_vma_alias,
    partial_no_page_store,
    cow_calls,
    cow_cycles,
    partial_total_cycles,
    final_drop_with_store,
};
var vmo_perf_counters: [munmap_cpu_rows][16]u64 align(128) =
    std.mem.zeroes([munmap_cpu_rows][16]u64);
comptime {
    if (enabled) @export(&vmo_perf_counters, .{ .name = "vmo_perf_counters" });
}
pub fn vmoAdd(counter: VmoCounter, value: u64) void {
    if (!enabled) return;
    const cpu = smp.currentCpuSlot();
    if (cpu >= smp.max_cpus) return;
    _ = @atomicRmw(u64, &vmo_perf_counters[cpu][@intFromEnum(counter)], .Add, value, .monotonic);
}
pub fn vmoElapsed(counter: VmoCounter, start: u64) void {
    if (enabled) vmoAdd(counter, timestamp() -% start);
}

pub fn timestamp() u64 {
    return if (enabled) platform.readTimestampCounter() else 0;
}
pub fn add(counter: Counter, value: u64) void {
    if (!enabled) return;
    const cpu = smp.currentCpuSlot();
    if (cpu >= smp.max_cpus) return;
    _ = @atomicRmw(u64, &smp_perf_counters[cpu][@intFromEnum(counter)], .Add, value, .monotonic);
}
pub fn elapsed(counter: Counter, start: u64) void {
    if (enabled) add(counter, timestamp() -% start);
}
pub fn staticEnd() usize {
    const smp_end = @intFromPtr(&smp_perf_counters) + @sizeOf(@TypeOf(smp_perf_counters));
    if (!enabled) return smp_end;
    return @max(@max(smp_end, @intFromPtr(&munmap_perf_counters) + @sizeOf(@TypeOf(munmap_perf_counters))), @intFromPtr(&vmo_perf_counters) + @sizeOf(@TypeOf(vmo_perf_counters)));
}
