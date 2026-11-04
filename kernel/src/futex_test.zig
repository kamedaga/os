const std = @import("std");
const runtime = @import("syscall/runtime.zig");

// A src-root keeps the runtime's sibling imports inside the Zig module.
test "futex runtime coverage" {
    std.testing.refAllDecls(runtime);
}

// Real dispatch references trap entry symbols through the hook type. Entering
// hardware context/trap code in these host regressions is always a failure.
export fn saveCurrentThreadXState() callconv(.winapi) void {
    @panic("unexpected host XState save");
}
export fn restoreCurrentThreadXState() callconv(.c) void {
    @panic("unexpected host XState restore");
}
export fn resumeAfterFatalUserException(_: @import("kernel.zig").PrincipalId, _: u8, _: *@import("interrupts.zig").TrapFrame) callconv(.winapi) void {
    @panic("unexpected host fatal exception");
}
