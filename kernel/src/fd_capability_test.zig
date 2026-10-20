threadlocal var cpu_slot: usize = 0;
pub fn setTestLockDiagnosticsCpu(cpu: usize) void {
    cpu_slot = cpu;
}
pub fn lockDiagnosticsCpuSlot() ?usize {
    return cpu_slot;
}
test "FD capability actual state roots" {
    _ = @import("state/fd_capability.zig");
}
