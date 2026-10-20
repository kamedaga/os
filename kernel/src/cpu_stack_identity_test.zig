const platform = @import("arch/x86_64/platform.zig");
comptime {
    // Keep sibling imports inside the src module, without pulling privileged
    // production entry points into the host test's linker reachability.
    _ = platform.cpuSlotForStackPointer;
}
