// Zig's test executable root is the runner, not the test source module.
// Publish diagnostic CPU identity here so actual callers cannot silently run off.
pub const main = @import("standard_test_runner").main;
pub const std_options = @import("standard_test_runner").std_options;
pub const kernel_lock_checks_enabled = true;
pub const kernel_lock_cpu_count = 2;
threadlocal var diagnostic_cpu: usize = 0;
pub fn setTestLockDiagnosticsCpu(cpu: usize) void {
    diagnostic_cpu = cpu;
}
pub fn lockDiagnosticsCpuSlot() ?usize {
    return diagnostic_cpu;
}

pub var ipi_admissions: usize = 0;
pub fn lockDiagnosticsIpiWaitReached() void {
    ipi_admissions += 1;
}
