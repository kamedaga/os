//! Privileged user XState transitions, shared by boot and trap entry/return.
//! The scheduler owns initial/per-thread images; this module owns neither
//! storage nor a lock. Keeping CPU transitions out of boot lets their live
//! register constraints be audited independently of boot orchestration.
//!
//! Context access uses this CPU's pinned stable scheduler-owned context
//! without taking the thread-table lock or acquiring a directory snapshot.
//! Growth does not move the save target. Directory/context reclamation still
//! requires the pending scoped-reader migration; a CPU pin alone is
//! not a permission to free storage or concurrently edit context fields.
//! Before-save/after-restore code must not clobber live user XState: even an
//! aggregate CPU-identity result can introduce compiler-generated SIMD moves.
const scheduler = @import("scheduler_connection.zig");
const kernel_runtime = @import("kernel_runtime.zig");
const x86_platform = @import("arch/x86_64/platform.zig");
const halt = @import("halt.zig");

// Only the local syscall entry owns this flag. Kernel IRQs preserve live
// registers and never change its ownership; a source save clears it BEFORE
// activation can make another context current. Never infer this from final ID.
const Ownership = @import("ipc_xstate_transition.zig").Ownership;
var ipc_live: []Ownership = &.{};
pub fn staticEnd() usize {
    return @intFromPtr(&ipc_live) + @sizeOf(@TypeOf(ipc_live));
}
pub fn initCpuStorage(count: usize) !void {
    ipc_live = try @import("arch/x86_64/cpu_runtime.zig").alloc(Ownership, count);
    @memset(ipc_live, .{});
}
pub fn beginIpcLive() void {
    ipc_live[scheduler.currentCpu()].begin();
    if (scheduler.currentContextForXState()) |ctx| ctx.pkru = x86_platform.readPkru();
}
pub fn saveIpcLiveIfNeeded() void {
    if (@import("builtin").is_test) return;
    if (ipc_live.len != 0 and ipc_live[scheduler.currentCpu()].live) saveCurrentThreadXState();
}
pub fn finishIpcLive() void {
    const slot = scheduler.currentCpu();
    if (!ipc_live[slot].finish()) {
        // No activation and no signal image access took place. The original
        // user image is still in hardware, including its upper YMM lanes.
    } else restoreCurrentThreadXState();
}

pub fn initXStateSupport() void {
    if (!x86_platform.enableAvxXStateForCurrentCpu()) {
        halt.haltWithMessage("AVX xstate unavailable");
    }
    x86_platform.configureXStateOptimizationForCpu(0);
    @memset(&scheduler.initial_x_state, 0);
    x86_platform.saveXState(&scheduler.initial_x_state);
}

pub export fn saveCurrentThreadXState() callconv(.winapi) void {
    const trace = @import("ipc_path_trace.zig");
    const start = trace.now();
    defer trace.end(501, start);
    if (ipc_live.len != 0) ipc_live[scheduler.currentCpu()].saved();
    if (!kernel_runtime.kernel_state_ready) return;
    const ctx = scheduler.currentContextForXState() orelse return;
    // A remote stop clears ready before the target CPU reaches its IPI safe
    // point.  The still-current allocated context must nevertheless retain
    // the user FX image before scheduler ownership is severed.
    ctx.pkru = x86_platform.readPkru();
    const save_start = @import("ipc_metric.zig").timestamp();
    x86_platform.saveThreadXState(&ctx.x_state);
    @import("ipc_metric.zig").record(.xstate_save, save_start);
}

pub export fn restoreCurrentThreadXState() callconv(.c) void {
    if (!kernel_runtime.kernel_state_ready) return;
    const ctx = scheduler.currentContextForXState() orelse return;
    // Gate on allocation, as the save does, not on ready. A remote stop clears
    // ready for a thread that is still current here (e.g. installed by a
    // handoff or a direct block switch, then stopped before this stub runs).
    // Skipping the XRSTOR would return to its user frame with the previous
    // thread's live registers, and the stop IPI's save would then store those
    // into this thread's image. Its own image is always the right one to load.
    // The accessor checks allocation/generation on the CPU-owned identity;
    // another index lookup would provide no extra exclusion.
    x86_platform.writePkru(ctx.pkru);
    const restore_start = @import("ipc_metric.zig").timestamp();
    x86_platform.restoreXState(&ctx.x_state);
    @import("ipc_metric.zig").record(.xstate_restore, restore_start);
}
