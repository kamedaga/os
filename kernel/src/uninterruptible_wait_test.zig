const scheduler = @import("scheduler_connection.zig");
const kernel = @import("kernel.zig");
const TrapFrame = @import("interrupts.zig").TrapFrame;
// Hardware trap entry must never be reached by the host frontend fixture.
export fn saveCurrentThreadXState() callconv(.winapi) void {
    @panic("unexpected host trap");
}
export fn restoreCurrentThreadXState() callconv(.winapi) void {
    @panic("unexpected host trap");
}
export fn resumeAfterFatalUserException(_: kernel.PrincipalId, _: u8, _: *TrapFrame) callconv(.winapi) void {
    @panic("unexpected host fatal trap");
}
test "uninterruptible wait actual five WAIT frontends BSP AP flags pending and invalid matrix" {
    try scheduler.testUninterruptibleWaitFrontendMatrix();
}
