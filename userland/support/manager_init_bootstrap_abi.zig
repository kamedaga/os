const init_bootstrap_abi = @import("init_bootstrap_abi.zig");
const process_abi = @import("process_abi.zig");

pub const magic: u64 = 0x4D49_4248; // "MIBH"
pub const version: u64 = 1;
pub const config_target_va: u64 = process_abi.auxPageVa(33);
pub const max_device_grants: usize = init_bootstrap_abi.max_device_descriptors;

pub const DeviceGrant = extern struct {
    bootstrap_source_va: u64 = 0,
    submit_token: u64 = 0,
    notify_token: u64 = 0,
};

pub const ConfigPage = extern struct {
    magic: u64,
    version: u64,
    ready: u64,
    device_count: u64,
    framebuffer_vm_token: u64,
    reserved0: u64 = 0,
    device_grants: [max_device_grants]DeviceGrant,
};

fn compilerBarrier() void {
    asm volatile ("" ::: .{ .memory = true });
}

pub fn writePendingConfigPage(config_source_va: u64) void {
    const page: *volatile ConfigPage = @ptrFromInt(config_source_va);
    page.* = .{
        .magic = magic,
        .version = version,
        .ready = 0,
        .device_count = 0,
        .framebuffer_vm_token = 0,
        .device_grants = [_]DeviceGrant{.{}} ** max_device_grants,
    };
    compilerBarrier();
}

pub fn writeDeviceGrant(
    config_source_va: u64,
    index: usize,
    bootstrap_source_va: u64,
    submit_token: u64,
    notify_token: u64,
) void {
    if (index >= max_device_grants) return;
    const page: *volatile ConfigPage = @ptrFromInt(config_source_va);
    page.device_grants[index] = .{
        .bootstrap_source_va = bootstrap_source_va,
        .submit_token = submit_token,
        .notify_token = notify_token,
    };
}

pub fn markReady(config_source_va: u64, device_count: u64) void {
    const page: *volatile ConfigPage = @ptrFromInt(config_source_va);
    page.device_count = device_count;
    compilerBarrier();
    page.ready = 1;
}
