const init_bootstrap_abi = @import("init_bootstrap_abi.zig");
const process_abi = @import("process_abi.zig");

pub const magic: u64 = 0x4D49_4248; // "MIBH"
pub const version: u64 = 4;
pub const config_target_va: u64 = process_abi.auxPageVa(33);
pub const max_device_grants: usize = init_bootstrap_abi.max_device_descriptors;
pub const max_device_queue_grants: usize = init_bootstrap_abi.max_device_queue_grants;

pub const InputDeviceHint = enum(u64) {
    unknown = 0,
    pointer = 1,
    keyboard = 2,
};

pub fn inputDeviceHintFromRaw(raw: u64) InputDeviceHint {
    return switch (raw) {
        @intFromEnum(InputDeviceHint.pointer) => .pointer,
        @intFromEnum(InputDeviceHint.keyboard) => .keyboard,
        else => .unknown,
    };
}

pub const DeviceGrant = extern struct {
    device_page_paddr: u64 = 0,
    iommu_token: u64 = 0,
    queue_grant_count: u64 = 0,
    queue_grants: [max_device_queue_grants]init_bootstrap_abi.DeviceQueueGrant =
        [_]init_bootstrap_abi.DeviceQueueGrant{.{}} ** max_device_queue_grants,
    command_token: u64 = 0,
    input_kind_hint: u64 = 0,
};

pub const ConfigPage = extern struct {
    magic: u64,
    version: u64,
    ready: u64,
    device_count: u64,
    framebuffer_vm_token: u64,
    bootfs_vm_token: u64,
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
        .bootfs_vm_token = 0,
        .device_grants = [_]DeviceGrant{.{}} ** max_device_grants,
    };
    compilerBarrier();
}

pub fn writeDeviceGrant(
    config_source_va: u64,
    index: usize,
    device_page_paddr: u64,
    iommu_token: u64,
    queue_grant_count: u64,
    queue_grants: [max_device_queue_grants]init_bootstrap_abi.DeviceQueueGrant,
    command_token: u64,
    input_kind_hint: u64,
) void {
    if (index >= max_device_grants) return;
    const page: *volatile ConfigPage = @ptrFromInt(config_source_va);
    page.device_grants[index] = .{
        .device_page_paddr = device_page_paddr,
        .iommu_token = iommu_token,
        .queue_grant_count = queue_grant_count,
        .queue_grants = queue_grants,
        .command_token = command_token,
        .input_kind_hint = input_kind_hint,
    };
}

pub fn markReady(config_source_va: u64, device_count: u64) void {
    const page: *volatile ConfigPage = @ptrFromInt(config_source_va);
    page.device_count = device_count;
    compilerBarrier();
    page.ready = 1;
}
