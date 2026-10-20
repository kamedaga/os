const scheduler = @import("scheduler_connection.zig");
comptime {
    if (!@import("sync/lock_diagnostics.zig").enabled) @compileError("ranked scheduler tests require the diagnostic runner");
    // Import the module's tests, not every privileged production entry point.
    _ = scheduler.initializeStaticStorage;
}
