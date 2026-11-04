// Dispatch/handoff host tests can link trap stubs, but must never execute
// hardware save/restore or fatal-return paths. Fail rather than emulate them.
export fn saveCurrentThreadXState() callconv(.winapi) void {
    @panic("unexpected host XState save");
}
export fn restoreCurrentThreadXState() callconv(.c) void {
    @panic("unexpected host XState restore");
}
export fn resumeAfterFatalUserException(_: @import("kernel.zig").PrincipalId, _: u8, _: *@import("interrupts.zig").TrapFrame) callconv(.winapi) void {
    @panic("unexpected host fatal exception");
}
