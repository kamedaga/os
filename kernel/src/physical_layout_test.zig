// Keep the test root at src/: the architecture mapping boundary now depends
// on shared address-domain types, not a second architecture-local type copy.
comptime {
    _ = @import("arch/x86_64/physical_layout.zig");
}
