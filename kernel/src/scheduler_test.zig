const scheduler = @import("scheduler_connection.zig");
// Import tests without forcing every privileged production entry point.
comptime { _ = scheduler.initializeStaticStorage; }
const guards = @import("scheduler_test_trap_guards.zig");
comptime { _ = guards; }
