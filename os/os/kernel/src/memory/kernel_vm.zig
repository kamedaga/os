const x86_platform = @import("../arch/x86_64/platform.zig");

pub const phys_copy_window_va: u64 = x86_platform.phys_copy_window_va;

pub fn pageAlignDown(addr: u64) u64 {
    return addr & ~@as(u64, 4095);
}

pub fn pageAlignUp(addr: u64) u64 {
    return (addr + 4095) & ~@as(u64, 4095);
}

pub fn writeCr3(value: u64) void {
    x86_platform.writeCr3(value);
}

pub fn readCr3() u64 {
    return x86_platform.readCr3();
}

pub fn invlpg(addr: u64) void {
    x86_platform.invlpg(addr);
}

pub fn installIdentityPageTables0To1GiB() bool {
    return x86_platform.installIdentityPageTables0To1GiB();
}

pub fn hardenKernelMappingsSupervisorOnly() void {
    x86_platform.hardenKernelMappingsSupervisorOnly();
}
