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

// Separate selection, CPU synchronization and PMM return costs: samples in
// PT retirement cannot distinguish them. Helper counters include fault
// reclamation; range counters do not. detach_cycles includes range selection
// and helper detachment, not fault-side candidate selection. retired_pts is
// detach success, not PMM return success. No storage/probes in normal builds.
pub const PtRetireCounter = enum(usize) {
    range_calls,
    range_candidate_pts,
    range_multi_batch_calls,
    fault_reclaim_calls,
    flush_calls,
    retired_pts,
    detach_cycles,
    flush_cycles,
    release_cycles,
    range_total_cycles,
};
var pt_retire_perf_counters: [munmap_cpu_rows][16]u64 align(128) =
    std.mem.zeroes([munmap_cpu_rows][16]u64);
comptime {
    if (enabled) @export(&pt_retire_perf_counters, .{ .name = "pt_retire_perf_counters" });
}
pub fn ptRetireAdd(counter: PtRetireCounter, value: u64) void {
    if (!enabled) return;
    const cpu = smp.currentCpuSlot();
    if (cpu >= smp.max_cpus) return;
    _ = @atomicRmw(u64, &pt_retire_perf_counters[cpu][@intFromEnum(counter)], .Add, value, .monotonic);
}
pub fn ptRetireElapsed(counter: PtRetireCounter, start: u64) void {
    if (enabled) ptRetireAdd(counter, timestamp() -% start);
}

// Unlock-time timer scan + deadline publish, not notifyChanged(). Column 3 is
// reserved. Separate from existing layouts; absent from normal releases.
var timer_scan_perf_counters: [munmap_cpu_rows][4]u64 align(128) =
    std.mem.zeroes([munmap_cpu_rows][4]u64);
comptime {
    if (enabled) @export(&timer_scan_perf_counters, .{ .name = "timer_scan_perf_counters" });
}
pub fn timerScanRecord(start: u64, changed: bool) void {
    if (!enabled) return;
    const cycles = timestamp() -% start;
    const cpu = smp.currentCpuSlot();
    if (cpu >= smp.max_cpus) return;
    _ = @atomicRmw(u64, &timer_scan_perf_counters[cpu][0], .Add, 1, .monotonic);
    _ = @atomicRmw(u64, &timer_scan_perf_counters[cpu][1], .Add, cycles, .monotonic);
    if (changed) _ = @atomicRmw(u64, &timer_scan_perf_counters[cpu][2], .Add, 1, .monotonic);
}

// Native hot-path boundaries, classified before a handoff changes the frame.
// Times include suspension and overlap; they are not additive CPU time.
// Futex uses copy for compare, claim for registration/collector, body for block
// or handoff (including its lock-drop callback), reserved for successful handoff.
pub const RuntimePath = enum(usize) { clock, pending_frame, futex_wait, futex_wake, futex_wake_wait, checked_word_copy };
pub const RuntimeCounter = enum(usize) {
    calls,
    total_cycles,
    lock_wait_cycles,
    locked_body_cycles,
    unlock_cycles,
    return_stage_cycles,
    body_calls,
    body_cycles,
    copy_calls,
    copy_cycles,
    claim_calls,
    claim_cycles,
    claimed,
    no_claim,
    invalid,
    reserved,
};
pub const runtime_counter_count = @typeInfo(RuntimeCounter).@"enum".fields.len;
pub const runtime_path_stride = @typeInfo(RuntimePath).@"enum".fields.len * runtime_counter_count;
var runtime_path_perf_counters: [munmap_cpu_rows][runtime_path_stride]u64 align(128) =
    std.mem.zeroes([munmap_cpu_rows][runtime_path_stride]u64);
comptime {
    if (enabled) @export(&runtime_path_perf_counters, .{ .name = "runtime_path_perf_counters" });
}
pub fn runtimePath(nr: u64, op: u64) ?RuntimePath {
    if (!enabled) return null;
    const abi = @import("kernel_abi_root");
    if (nr == abi.runtime_abi.syscall_clock_gettime or nr == abi.runtime_abi.syscall_clock_read) return .clock;
    if (nr == abi.runtime_abi.syscall_futex_wait) return .futex_wait;
    if (nr == abi.runtime_abi.syscall_futex_wake) return .futex_wake;
    if (nr == abi.runtime_abi.syscall_futex_wake_wait) return .futex_wake_wait;
    if (nr == abi.process_abi.syscall_process_signal_ctl and
        op == abi.process_abi.signal_ctl_deliver_pending_frame) return .pending_frame;
    return null;
}
pub fn runtimeTimestamp(path: ?RuntimePath) u64 {
    return if (enabled and path != null) timestamp() else 0;
}
pub fn runtimeAdd(path: ?RuntimePath, counter: RuntimeCounter, value: u64) void {
    if (!enabled) return;
    const selected = path orelse return;
    const cpu = smp.currentCpuSlot();
    if (cpu >= smp.max_cpus) return;
    const index = @intFromEnum(selected) * runtime_counter_count + @intFromEnum(counter);
    _ = @atomicRmw(u64, &runtime_path_perf_counters[cpu][index], .Add, value, .monotonic);
}
pub fn runtimeElapsed(path: ?RuntimePath, counter: RuntimeCounter, start: u64) void {
    if (enabled and path != null) runtimeAdd(path, counter, timestamp() -% start);
}

// The transfer's saved frame, hardware bases and ownership locks cannot be
// observed at userland boundaries. Keep this diagnostic separate from the
// syscall rows so their existing denominators and snapshot stride do not move.
pub const HandoffCounter = enum(usize) {
    attempts,
    table_wait_cycles,
    lock_pairs,
    cpu_wait_cycles,
    rq_wait_cycles,
    scalar_calls,
    scalar_cycles,
    capture_calls,
    capture_cycles,
    publication_calls,
    publication_cycles,
    install_calls,
    install_cycles,
    installed,
    rejected,
    install_failed,
};
pub const handoff_counter_count = @typeInfo(HandoffCounter).@"enum".fields.len;
var wake_wait_handoff_perf_counters: [munmap_cpu_rows][handoff_counter_count]u64 align(128) =
    std.mem.zeroes([munmap_cpu_rows][handoff_counter_count]u64);
comptime {
    if (enabled) @export(&wake_wait_handoff_perf_counters, .{ .name = "wake_wait_handoff_perf_counters" });
}
pub fn handoffAdd(counter: HandoffCounter, value: u64) void {
    if (!enabled) return;
    const cpu = smp.currentCpuSlot();
    if (cpu >= smp.max_cpus) return;
    _ = @atomicRmw(u64, &wake_wait_handoff_perf_counters[cpu][@intFromEnum(counter)], .Add, value, .monotonic);
}
pub fn handoffElapsed(counter: HandoffCounter, start: u64) void {
    if (enabled) handoffAdd(counter, timestamp() -% start);
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
    const existing_end = @max(@max(@max(smp_end, @intFromPtr(&munmap_perf_counters) + @sizeOf(@TypeOf(munmap_perf_counters))), @intFromPtr(&vmo_perf_counters) + @sizeOf(@TypeOf(vmo_perf_counters))), @intFromPtr(&timer_scan_perf_counters) + @sizeOf(@TypeOf(timer_scan_perf_counters)));
    const runtime_end = @max(existing_end, @intFromPtr(&runtime_path_perf_counters) + @sizeOf(@TypeOf(runtime_path_perf_counters)));
    const retire_end = @max(runtime_end, @intFromPtr(&pt_retire_perf_counters) + @sizeOf(@TypeOf(pt_retire_perf_counters)));
    return @max(retire_end, @intFromPtr(&wake_wait_handoff_perf_counters) + @sizeOf(@TypeOf(wake_wait_handoff_perf_counters)));
}
