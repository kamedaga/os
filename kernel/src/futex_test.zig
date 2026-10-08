const std = @import("std");
const runtime = @import("syscall/runtime.zig");

// A src-root keeps the runtime's sibling imports inside the Zig module.
test "futex runtime coverage" {
    std.testing.refAllDecls(runtime);
}
