pub const ipc_profile_enabled = true;
test {
    _ = @import("ipc_metric.zig");
    _ = @import("wake_trace.zig");
}
