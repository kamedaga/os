const std = @import("std");
const boot_status_abi = @import("abi_root").boot_status_abi;
const boot_status_client = @import("abi_root").boot_status_client;
const bootfs_format = @import("abi_root").bootfs_format;
const cap_transfer_abi = @import("abi_root").cap_transfer_abi;
const capctl_protocol = @import("abi_root").capctl_protocol;
const image_abi = @import("abi_root").image_abi;
const startup_plan_abi = @import("abi_root").startup_plan_abi;
const queue_abi = @import("abi_root").queue_abi;
const init_bootstrap_abi = @import("abi_root").init_bootstrap_abi;
const input_bootstrap = @import("abi_root").input_driver_bootstrap_abi;
const manager_init_bootstrap_abi = @import("abi_root").manager_init_bootstrap_abi;
const block_bootstrap = @import("abi_root").block_bootstrap_abi;
const console_bootstrap = @import("abi_root").console_bootstrap_abi;
const gpu_bootstrap = @import("abi_root").gpu_bootstrap_abi;
const gpu_protocol = @import("abi_root").gpu_protocol;
const persistent_fs_bootstrap = @import("abi_root").persistent_fs_bootstrap_abi;
const process_abi = @import("abi_root").process_abi;
const process_args_env_bootstrap_abi = @import("abi_root").process_args_env_bootstrap_abi;
const process_exit_bootstrap_abi = @import("abi_root").process_exit_bootstrap_abi;
const service_registry_abi = @import("abi_root").service_registry_abi;
const stdio_bootstrap_abi = @import("abi_root").stdio_bootstrap_abi;
const rootfs_core = @import("abi_root").rootfs_core;

const syscall_log: u64 = 0x9;
const syscall_map_page: u64 = 0x2;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_wait_event: u64 = 0x17;
const syscall_grant_caps_batch: u64 = 0x14;
const syscall_install_caps_batch: u64 = 0x32;
const syscall_install_endpoint: u64 = 0x26;
const syscall_signal_endpoint: u64 = 0x2C;
const syscall_install_mmio_cap: u64 = 0x2F;
const syscall_get_process_slot: u64 = 0x2E;

const dynamic_bootstrap_source_base_va: u64 = 0x3C10_6000;
const inspect_mmio_base_va: u64 = 0x3F00_0000;
const virtio_vendor_id: u64 = 0x1AF4;
const virtio_input_device_modern: u64 = 0x1052;
const virtio_input_subsystem_id: u64 = 0x0012;
const virtio_blk_device_modern: u64 = 0x1042;
const virtio_blk_subsystem_id: u64 = 0x0002;
const virtio_gpu_device_modern: u64 = 0x1050;
const virtio_gpu_subsystem_id: u64 = 0x0010;
const virtio_console_device_modern: u64 = 0x1043;
const virtio_console_subsystem_id: u64 = 0x0003;
const input_cfg_select: u64 = 0;
const input_cfg_subsel: u64 = 1;
const input_cfg_size: u64 = 2;
const input_cfg_payload: u64 = 8;
const virtio_input_cfg_select_ev_bits: u8 = 0x11;
const virtio_input_ev_key: u8 = 0x01;
const virtio_input_ev_rel: u8 = 0x02;
const virtio_input_ev_abs: u8 = 0x03;
const input_code_rel_x: u16 = 0x00;
const input_code_rel_y: u16 = 0x01;
const input_code_abs_x: u16 = 0x00;
const input_code_abs_y: u16 = 0x01;
const input_code_key_a: u16 = 0x1E;
const input_code_btn_left: u16 = 0x110;
const virtio_blk_capacity_offset: u64 = 0x00;
const virtio_blk_block_size_offset: u64 = 0x14;
const rootfs_startup_manifest_path = "/sys/startup_manifest.txt";
const rootfs_shell_path = "/cmd/shell.elf";
const rootfs_gpu_driver_path = "/srv/virtio_gpu_gl.elf";
const persistent_fs_start_block: u64 = 395264;
const manager_stack_extension_pages: u64 = 8;
const manager_stack_extension_base_va: u64 = process_abi.aux_base_va - ((manager_stack_extension_pages + 1) * 4096);
const startup_manifest_max_bytes: usize = 4096;
const startup_named_dependency_max: usize = 8;
const capctl_request_va: u64 = 0x3C0E_0000;
const capctl_response_va: u64 = 0x3C0E_1000;
const startup_ready_name_max: usize = startup_plan_abi.max_startup_program_descriptors * (startup_named_dependency_max + 1);

const InputDeviceKind = startup_plan_abi.StartupInputSelector;
const BlockDeviceKind = startup_plan_abi.StartupBlockSelector;
const StartupDeviceKind = startup_plan_abi.StartupDeviceSelector;

const BlockGeometry = struct {
    capacity_sectors: u64,
    logical_block_size: u64,
};

const DeviceMmioPageSet = struct {
    paddrs: [4]u64 = [_]u64{0} ** 4,
    rights: [4]u64 = [_]u64{0} ** 4,
    count: usize = 0,
};

const MmioCapCache = struct {
    paddrs: [init_bootstrap_abi.max_device_descriptors * 4]u64 = [_]u64{0} ** (init_bootstrap_abi.max_device_descriptors * 4),
    rights: [init_bootstrap_abi.max_device_descriptors * 4]u64 = [_]u64{0} ** (init_bootstrap_abi.max_device_descriptors * 4),
    count: usize = 0,
};

const StartupManifest = struct {
    bytes: [startup_manifest_max_bytes]u8,
    len: usize,

    fn slice(self: *const StartupManifest) []const u8 {
        return self.bytes[0..self.len];
    }
};

const StartupPolicy = struct {
    name: []const u8 = "",
    label: []const u8 = "",
    path: []const u8 = "",
    action: startup_plan_abi.StartupAction = .block_driver,
    exec_source: startup_plan_abi.StartupExecSource = .startup_path,
    ensure_flags: u64 = 0,
    require_flags: u64 = 0,
    after_count: usize = 0,
    after_names: [startup_named_dependency_max][]const u8 = [_][]const u8{""} ** startup_named_dependency_max,
    provide_count: usize = 0,
    provide_names: [startup_named_dependency_max][]const u8 = [_][]const u8{""} ** startup_named_dependency_max,
    block_kind: ?BlockDeviceKind = null,
    device_kind: ?StartupDeviceKind = null,
    input_kind: ?InputDeviceKind = null,
};

const StartupNode = struct {
    policy: StartupPolicy,
    launched: bool = false,
};

const CachedStartupExec = struct {
    valid: bool = false,
    exec_source: startup_plan_abi.StartupExecSource = .startup_path,
    path_len: u16 = 0,
    path_bytes: [startup_plan_abi.path_max_bytes]u8 = [_]u8{0} ** startup_plan_abi.path_max_bytes,
    result: rootfs_core.OpenExecResult = .{ .token = 0, .file_bytes = 0 },

    fn path(self: *const CachedStartupExec) []const u8 {
        return self.path_bytes[0..self.path_len];
    }
};

var startup_manifest_storage = StartupManifest{
    .bytes = [_]u8{0} ** startup_manifest_max_bytes,
    .len = 0,
};
var next_dynamic_service_endpoint_id: u64 = service_registry_abi.dynamic_endpoint_id_base + 1;
var next_dynamic_bootstrap_source_va: u64 = dynamic_bootstrap_source_base_va;
var next_inspect_mmio_page_va: u64 = inspect_mmio_base_va;
var mmio_cap_cache = MmioCapCache{};
var block_bootstrap_pages_storage: [process_abi.max_bootstrap_page_descriptors]process_abi.BootstrapPageDescriptor = undefined;
var block_bootstrap_table_storage = process_abi.BootstrapDescriptorTable{};
var console_bootstrap_pages_storage: [process_abi.max_bootstrap_page_descriptors]process_abi.BootstrapPageDescriptor = undefined;
var console_bootstrap_table_storage = process_abi.BootstrapDescriptorTable{};
var gpu_bootstrap_pages_storage: [process_abi.max_bootstrap_page_descriptors]process_abi.BootstrapPageDescriptor = undefined;
var gpu_bootstrap_table_storage = process_abi.BootstrapDescriptorTable{};
var persistent_fs_bootstrap_pages_storage: [process_abi.max_bootstrap_page_descriptors]process_abi.BootstrapPageDescriptor = undefined;
var persistent_fs_bootstrap_table_storage = process_abi.BootstrapDescriptorTable{};
var generic_service_bootstrap_table_storage = process_abi.BootstrapDescriptorTable{};
var boot_display_bootstrap_table_storage = process_abi.BootstrapDescriptorTable{};

const seed_log_prefix = "[seed] ";
const legacy_manager_log_prefix = "ManagerInit: ";

fn userLogRaw(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn stripLegacySeedPrefix(message: []const u8) []const u8 {
    if (std.mem.startsWith(u8, message, seed_log_prefix)) return message;
    if (std.mem.startsWith(u8, message, legacy_manager_log_prefix)) return message[legacy_manager_log_prefix.len..];
    return message;
}

fn userLog(message: []const u8) u64 {
    const stripped = stripLegacySeedPrefix(message);
    var buf: [512]u8 = undefined;
    var len: usize = 0;
    while (len < seed_log_prefix.len and len < buf.len) : (len += 1) buf[len] = seed_log_prefix[len];
    var msg_index: usize = 0;
    while (msg_index < stripped.len and len < buf.len) : ({
        msg_index += 1;
        len += 1;
    }) {
        buf[len] = stripped[msg_index];
    }
    return userLogRaw(buf[0..len]);
}

fn userLogHex(label: []const u8, value: u64) void {
    const stripped_label = stripLegacySeedPrefix(label);
    var buf: [128]u8 = undefined;
    var len: usize = 0;
    while (len < seed_log_prefix.len and len < buf.len) : (len += 1) buf[len] = seed_log_prefix[len];
    var label_index: usize = 0;
    while (label_index < stripped_label.len and len < buf.len) : ({
        label_index += 1;
        len += 1;
    }) {
        buf[len] = stripped_label[label_index];
    }
    if (len + 19 >= buf.len) return;
    buf[len] = '0';
    buf[len + 1] = 'x';
    len += 2;
    var shift: u6 = 60;
    while (true) {
        const nibble: u8 = @intCast((value >> shift) & 0xF);
        buf[len] = if (nibble < 10) '0' + nibble else 'A' + (nibble - 10);
        len += 1;
        if (shift == 0) break;
        shift -= 4;
    }
    buf[len] = '\n';
    len += 1;
    _ = userLogRaw(buf[0..len]);
}

fn fail(message: []const u8) noreturn {
    _ = userLog(message);
    while (true) asm volatile ("pause");
}

fn noteBootStatus(status_bits: u32) void {
    _ = boot_status_client.set(status_bits);
}

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_page),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn allocMapPages(base_va: u64, page_count: u64, writable: bool, out_paddr_list_va: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_alloc_map_pages),
          [arg0] "{rdi}" (base_va),
          [arg1] "{rsi}" (page_count),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
          [arg3] "{rcx}" (out_paddr_list_va),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn waitEvent(wait_mailbox: bool, timeout_ticks: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_wait_event),
          [arg0] "{rdi}" (@as(u64, if (wait_mailbox) 1 else 0)),
          [arg1] "{rsi}" (timeout_ticks),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn acceptCapTransfer(transfer_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (cap_transfer_abi.syscall_accept_cap_transfer),
          [arg0] "{rdi}" (transfer_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn installEndpoint(endpoint_id: u64, target_process_slot: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_install_endpoint),
          [arg0] "{rdi}" (@as(u64, 0)),
          [arg1] "{rsi}" (endpoint_id),
          [arg2] "{rdx}" (target_process_slot),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn publishServiceEndpoint(endpoint_id: u64, target_process_slot: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (service_registry_abi.syscall_publish_service_endpoint),
          [arg0] "{rdi}" (endpoint_id),
          [arg1] "{rsi}" (target_process_slot),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn signalEndpoint(endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_signal_endpoint),
          [arg0] "{rdi}" (endpoint_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn getProcessSlot() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_get_process_slot),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn installMmioCap(paddr: u64, rights_bits: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_install_mmio_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (rights_bits),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantCap(to_process_slot: u64, paddr: u64, rights_bits: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (@as(u64, 0x8)),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (to_process_slot),
          [arg2] "{rdx}" (rights_bits),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantCapsBatch(paddr_list_va: u64, page_count: u64, to_process_slot: u64, rights_bits: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_grant_caps_batch),
          [arg0] "{rdi}" (paddr_list_va),
          [arg1] "{rsi}" (page_count),
          [arg2] "{rdx}" (to_process_slot),
          [arg3] "{rcx}" (rights_bits),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn installCapsBatch(paddr_list_va: u64, page_count: u64, rights_bits: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_install_caps_batch),
          [arg0] "{rdi}" (paddr_list_va),
          [arg1] "{rsi}" (page_count),
          [arg2] "{rdx}" (rights_bits),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantQueueCap(token: u64, to_process_slot: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (queue_abi.syscall_grant_cap),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (to_process_slot),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn revokeQueueCap(token: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (queue_abi.syscall_revoke_cap),
          [arg0] "{rdi}" (token),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn deriveCommandCap(token: u64, opcode_mask: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (queue_abi.syscall_derive_command_cap),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (opcode_mask),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn spawnExecWithExtendedBootstrapTable(exec_token: u64, table: *const process_abi.BootstrapDescriptorTable) u64 {
    return spawnExecWithExtendedBootstrapTableFlags(exec_token, table, process_abi.spawn_flag_bootstrap_extended_descriptor_table);
}

fn spawnExecWithExtendedBootstrapTableFlags(exec_token: u64, table: *const process_abi.BootstrapDescriptorTable, flags: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (process_abi.syscall_spawn_exec),
          [arg0] "{rdi}" (exec_token),
          [arg1] "{rsi}" (@as(u64, @intFromPtr(table))),
          [arg2] "{rdx}" (@as(u64, 0)),
          [arg3] "{rcx}" (flags),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn installVmObjectMmioRange(base_paddr: u64, size_bytes: u64, rights: image_abi.VmObjectRights) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (image_abi.syscall_install_vm_object_mmio_range),
          [arg0] "{rdi}" (base_paddr),
          [arg1] "{rsi}" (size_bytes),
          [arg2] "{rdx}" (image_abi.vmObjectRightsToBits(rights)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn installVmObject(base_va: u64, size_bytes: u64, rights: image_abi.VmObjectRights) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (image_abi.syscall_install_vm_object),
          [arg0] "{rdi}" (base_va),
          [arg1] "{rsi}" (size_bytes),
          [arg2] "{rdx}" (image_abi.vmObjectRightsToBits(rights)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn installExecImage(vm_token: u64, rights: image_abi.ExecImageRights) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (image_abi.syscall_install_exec_image),
          [arg0] "{rdi}" (vm_token),
          [arg1] "{rsi}" (image_abi.execImageRightsToBits(rights)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mapVmObject(token: u64, target_va: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (image_abi.syscall_map_vm_object),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (target_va),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn sliceVmObject(token: u64, offset_bytes: u64, size_bytes: u64, rights: image_abi.VmObjectRights) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (image_abi.syscall_slice_vm_object),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (offset_bytes),
          [arg2] "{rdx}" (size_bytes),
          [arg3] "{rcx}" (image_abi.vmObjectRightsToBits(rights)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn configPage() ?*const volatile init_bootstrap_abi.ConfigPage {
    const page: *const volatile init_bootstrap_abi.ConfigPage = @ptrFromInt(process_abi.standard_config_target_va);
    if (page.magic != init_bootstrap_abi.config_magic) return null;
    if (page.version != init_bootstrap_abi.config_version) return null;
    if (page.descriptor_page_va == 0) return null;
    return page;
}

fn descriptorPage() ?*const volatile init_bootstrap_abi.DescriptorPage {
    const cfg = configPage() orelse return null;
    const page: *const volatile init_bootstrap_abi.DescriptorPage = @ptrFromInt(cfg.descriptor_page_va);
    if (page.magic != init_bootstrap_abi.magic) return null;
    if (page.version != init_bootstrap_abi.version) return null;
    return page;
}

fn findSpawnPageDescriptor(
    kind: init_bootstrap_abi.SpawnPageKind,
    subject: init_bootstrap_abi.SpawnPageSubject,
) ?init_bootstrap_abi.SpawnPageDescriptor {
    const page = descriptorPage() orelse return null;
    var i: usize = 0;
    while (i < page.spawn_page_count and i < init_bootstrap_abi.max_spawn_page_descriptors) : (i += 1) {
        const descriptor = page.spawn_pages[i];
        if (descriptor.kind != @intFromEnum(kind)) continue;
        if (descriptor.subject != @intFromEnum(subject)) continue;
        return descriptor;
    }
    return null;
}

fn requireSpawnPageDescriptor(
    kind: init_bootstrap_abi.SpawnPageKind,
    subject: init_bootstrap_abi.SpawnPageSubject,
    failure_message: []const u8,
) init_bootstrap_abi.SpawnPageDescriptor {
    return findSpawnPageDescriptor(kind, subject) orelse fail(failure_message);
}

fn isBootstrapDeviceDescriptorPresent(descriptor: init_bootstrap_abi.DeviceDescriptor) bool {
    return (descriptor.flags & init_bootstrap_abi.device_flag_present) != 0;
}

fn isVirtioInputDeviceDescriptor(descriptor: init_bootstrap_abi.DeviceDescriptor) bool {
    return descriptor.transport == @intFromEnum(init_bootstrap_abi.DeviceTransport.virtio_pci_modern) and
        descriptor.vendor_id == virtio_vendor_id and
        (descriptor.device_id == virtio_input_device_modern or descriptor.subsystem_id == virtio_input_subsystem_id);
}

fn isVirtioBlockDeviceDescriptor(descriptor: init_bootstrap_abi.DeviceDescriptor) bool {
    return descriptor.transport == @intFromEnum(init_bootstrap_abi.DeviceTransport.virtio_pci_modern) and
        descriptor.vendor_id == virtio_vendor_id and
        (descriptor.device_id == virtio_blk_device_modern or descriptor.subsystem_id == virtio_blk_subsystem_id);
}

fn isVirtioGpuDeviceDescriptor(descriptor: init_bootstrap_abi.DeviceDescriptor) bool {
    return descriptor.transport == @intFromEnum(init_bootstrap_abi.DeviceTransport.virtio_pci_modern) and
        descriptor.vendor_id == virtio_vendor_id and
        (descriptor.device_id == virtio_gpu_device_modern or descriptor.subsystem_id == virtio_gpu_subsystem_id);
}

fn isVirtioConsoleDeviceDescriptor(descriptor: init_bootstrap_abi.DeviceDescriptor) bool {
    return descriptor.transport == @intFromEnum(init_bootstrap_abi.DeviceTransport.virtio_pci_modern) and
        descriptor.vendor_id == virtio_vendor_id and
        (descriptor.device_id == virtio_console_device_modern or descriptor.subsystem_id == virtio_console_subsystem_id);
}

fn appendDeviceMmioPage(set: *DeviceMmioPageSet, paddr: u64, rights_bits: u64) bool {
    if (paddr == 0) return true;
    var i: usize = 0;
    while (i < set.count) : (i += 1) {
        if (set.paddrs[i] != paddr) continue;
        set.rights[i] |= rights_bits;
        return true;
    }
    if (set.count >= set.paddrs.len) return false;
    set.paddrs[set.count] = paddr;
    set.rights[set.count] = rights_bits;
    set.count += 1;
    return true;
}

fn collectDeviceMmioPages(descriptor: init_bootstrap_abi.DeviceDescriptor, writable_device_page: bool) ?DeviceMmioPageSet {
    const page_right_cpu_read: u64 = 0x1;
    const page_right_cpu_write: u64 = 0x2;
    if (descriptor.common_page_paddr == 0 or descriptor.notify_page_paddr == 0) return null;
    var set = DeviceMmioPageSet{};
    if (!appendDeviceMmioPage(&set, descriptor.common_page_paddr, page_right_cpu_read | page_right_cpu_write)) return null;
    if (!appendDeviceMmioPage(&set, descriptor.notify_page_paddr, page_right_cpu_read | page_right_cpu_write)) return null;
    if (!appendDeviceMmioPage(&set, descriptor.isr_page_paddr, page_right_cpu_read)) return null;
    const device_rights = page_right_cpu_read | if (writable_device_page) page_right_cpu_write else @as(u64, 0);
    if (!appendDeviceMmioPage(&set, descriptor.device_page_paddr, device_rights)) return null;
    return set;
}

fn ensureMmioCapInstalled(paddr: u64, rights_bits: u64) bool {
    if (paddr == 0) return true;
    var i: usize = 0;
    while (i < mmio_cap_cache.count) : (i += 1) {
        if (mmio_cap_cache.paddrs[i] != paddr) continue;
        if ((mmio_cap_cache.rights[i] & rights_bits) == rights_bits) return true;
        const merged_rights = mmio_cap_cache.rights[i] | rights_bits;
        if (installMmioCap(paddr, merged_rights) != 0) return false;
        mmio_cap_cache.rights[i] = merged_rights;
        return true;
    }
    if (installMmioCap(paddr, rights_bits) != 0) return false;
    if (mmio_cap_cache.count >= mmio_cap_cache.paddrs.len) return false;
    mmio_cap_cache.paddrs[mmio_cap_cache.count] = paddr;
    mmio_cap_cache.rights[mmio_cap_cache.count] = rights_bits;
    mmio_cap_cache.count += 1;
    return true;
}

fn ensureDeviceMmioCapsInstalled(descriptor: init_bootstrap_abi.DeviceDescriptor) bool {
    const page_right_grant: u64 = 0x8;
    const set = collectDeviceMmioPages(descriptor, true) orelse return false;
    var processed_rights: [4]u64 = undefined;
    var processed_len: usize = 0;
    var grouped_paddrs: [4]u64 = undefined;
    var i: usize = 0;
    while (i < set.count) : (i += 1) {
        const rights_bits = set.rights[i] | page_right_grant;
        var already_processed = false;
        var processed_index: usize = 0;
        while (processed_index < processed_len) : (processed_index += 1) {
            if (processed_rights[processed_index] == rights_bits) {
                already_processed = true;
                break;
            }
        }
        if (already_processed) continue;
        processed_rights[processed_len] = rights_bits;
        processed_len += 1;

        var group_len: usize = 0;
        var j: usize = 0;
        while (j < set.count) : (j += 1) {
            if ((set.rights[j] | page_right_grant) != rights_bits) continue;
            var cache_index: ?usize = null;
            var cache_search: usize = 0;
            while (cache_search < mmio_cap_cache.count) : (cache_search += 1) {
                if (mmio_cap_cache.paddrs[cache_search] == set.paddrs[j]) {
                    cache_index = cache_search;
                    break;
                }
            }
            if (cache_index) |existing_index| {
                if ((mmio_cap_cache.rights[existing_index] & rights_bits) == rights_bits) continue;
                const merged_rights = mmio_cap_cache.rights[existing_index] | rights_bits;
                if (installMmioCap(set.paddrs[j], merged_rights) != 0) return false;
                mmio_cap_cache.rights[existing_index] = merged_rights;
                continue;
            }
            grouped_paddrs[group_len] = set.paddrs[j];
            group_len += 1;
        }
        if (group_len == 0) continue;
        if (group_len == 1) {
            if (!ensureMmioCapInstalled(grouped_paddrs[0], rights_bits)) return false;
            continue;
        }
        if (installCapsBatch(@intFromPtr(&grouped_paddrs), group_len, rights_bits) != 0) return false;
        var group_index: usize = 0;
        while (group_index < group_len) : (group_index += 1) {
            if (mmio_cap_cache.count >= mmio_cap_cache.paddrs.len) return false;
            mmio_cap_cache.paddrs[mmio_cap_cache.count] = grouped_paddrs[group_index];
            mmio_cap_cache.rights[mmio_cap_cache.count] = rights_bits;
            mmio_cap_cache.count += 1;
        }
    }
    return true;
}

fn grantDeviceMmioPages(descriptor: init_bootstrap_abi.DeviceDescriptor, child_process_slot: u64) bool {
    const set = collectDeviceMmioPages(descriptor, false) orelse return false;
    var processed_rights: [4]u64 = undefined;
    var processed_len: usize = 0;
    var grouped_paddrs: [4]u64 = undefined;
    var i: usize = 0;
    while (i < set.count) : (i += 1) {
        var group_len: usize = 0;
        const rights_bits = set.rights[i];
        var already_processed = false;
        var processed_index: usize = 0;
        while (processed_index < processed_len) : (processed_index += 1) {
            if (processed_rights[processed_index] == rights_bits) {
                already_processed = true;
                break;
            }
        }
        if (already_processed) continue;
        var j: usize = 0;
        while (j < set.count) : (j += 1) {
            if (set.rights[j] != rights_bits) continue;
            var duplicate = false;
            var k: usize = 0;
            while (k < group_len) : (k += 1) {
                if (grouped_paddrs[k] == set.paddrs[j]) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            grouped_paddrs[group_len] = set.paddrs[j];
            group_len += 1;
        }
        processed_rights[processed_len] = rights_bits;
        processed_len += 1;
        if (group_len == 1) {
            if (grantCap(child_process_slot, grouped_paddrs[0], rights_bits) != 0) return false;
        } else {
            if (grantCapsBatch(@intFromPtr(&grouped_paddrs), group_len, child_process_slot, rights_bits) != 0) return false;
        }
    }
    return true;
}

fn mapInspectMmioPage(paddr: u64, writable: bool) ?u64 {
    if (paddr == 0) return null;
    const page_va = next_inspect_mmio_page_va;
    next_inspect_mmio_page_va +%= 0x1000;
    if (mapPage(page_va, paddr, writable) != 0) return null;
    return page_va;
}

fn deviceCfgInspectVa(descriptor: init_bootstrap_abi.DeviceDescriptor, writable: bool) ?u64 {
    if (!ensureDeviceMmioCapsInstalled(descriptor)) return null;
    const page_va = mapInspectMmioPage(descriptor.device_page_paddr, writable) orelse return null;
    return page_va + descriptor.device_page_offset;
}

fn mmioReadU8(addr: u64) u8 {
    const ptr: *const volatile u8 = @ptrFromInt(addr);
    return ptr.*;
}

fn mmioReadU32(addr: u64) u32 {
    const ptr: *const volatile u32 = @ptrFromInt(addr);
    return ptr.*;
}

fn mmioReadU64(addr: u64) u64 {
    const ptr: *const volatile u64 = @ptrFromInt(addr);
    return ptr.*;
}

fn mmioWriteU8(addr: u64, value: u8) void {
    const ptr: *volatile u8 = @ptrFromInt(addr);
    ptr.* = value;
}

fn readInputBitmapBit(device_cfg_va: u64, ev_type: u8, code: u16) bool {
    mmioWriteU8(device_cfg_va + input_cfg_select, virtio_input_cfg_select_ev_bits);
    mmioWriteU8(device_cfg_va + input_cfg_subsel, ev_type);
    const size = mmioReadU8(device_cfg_va + input_cfg_size);
    if (size == 0) return false;
    const byte_index: usize = @intCast(code / 8);
    if (byte_index >= size or byte_index >= 128) return false;
    const bit_index: u3 = @intCast(code & 7);
    const bits = mmioReadU8(device_cfg_va + input_cfg_payload + byte_index);
    return ((bits >> bit_index) & 1) != 0;
}

fn classifyInputDeviceDescriptor(descriptor: init_bootstrap_abi.DeviceDescriptor) ?InputDeviceKind {
    const device_cfg_va = deviceCfgInspectVa(descriptor, true) orelse return null;
    const has_rel_x = readInputBitmapBit(device_cfg_va, virtio_input_ev_rel, input_code_rel_x);
    const has_rel_y = readInputBitmapBit(device_cfg_va, virtio_input_ev_rel, input_code_rel_y);
    const has_abs_x = readInputBitmapBit(device_cfg_va, virtio_input_ev_abs, input_code_abs_x);
    const has_abs_y = readInputBitmapBit(device_cfg_va, virtio_input_ev_abs, input_code_abs_y);
    const has_key_a = readInputBitmapBit(device_cfg_va, virtio_input_ev_key, input_code_key_a);
    const has_btn_left = readInputBitmapBit(device_cfg_va, virtio_input_ev_key, input_code_btn_left);
    const pointer_like = (has_rel_x and has_rel_y) or (has_abs_x and has_abs_y and has_btn_left);
    const keyboard_like = has_key_a and !has_rel_x and !has_rel_y and !has_abs_x and !has_abs_y;
    if (pointer_like) return .pointer;
    if (keyboard_like) return .keyboard;
    return null;
}

fn findBootstrapQueueGrant(
    bootstrap_handoff: manager_init_bootstrap_abi.ConfigPage,
    descriptor: init_bootstrap_abi.DeviceDescriptor,
) ?manager_init_bootstrap_abi.DeviceGrant {
    var i: usize = 0;
    while (i < bootstrap_handoff.device_count and i < manager_init_bootstrap_abi.max_device_grants) : (i += 1) {
        const grant = bootstrap_handoff.device_grants[i];
        if (grant.device_page_paddr != descriptor.device_page_paddr) continue;
        if (grant.queue_grant_count == 0) return null;
        return grant;
    }
    return null;
}

fn hintedInputDeviceKind(
    bootstrap_handoff: manager_init_bootstrap_abi.ConfigPage,
    descriptor: init_bootstrap_abi.DeviceDescriptor,
) ?InputDeviceKind {
    const grant = findBootstrapQueueGrant(bootstrap_handoff, descriptor) orelse return null;
    return switch (manager_init_bootstrap_abi.inputDeviceHintFromRaw(grant.input_kind_hint)) {
        .pointer => .pointer,
        .keyboard => .keyboard,
        .unknown => null,
    };
}

fn deviceHasBootstrapQueueGrant(
    bootstrap_handoff: manager_init_bootstrap_abi.ConfigPage,
    descriptor: init_bootstrap_abi.DeviceDescriptor,
) bool {
    return findBootstrapQueueGrant(bootstrap_handoff, descriptor) != null;
}

fn findInputDeviceDescriptor(
    bootstrap_handoff: manager_init_bootstrap_abi.ConfigPage,
    kind: InputDeviceKind,
) ?init_bootstrap_abi.DeviceDescriptor {
    const page = descriptorPage() orelse return null;
    var i: usize = 0;
    while (i < page.device_count and i < init_bootstrap_abi.max_device_descriptors) : (i += 1) {
        const descriptor = page.devices[i];
        if (!isBootstrapDeviceDescriptorPresent(descriptor)) continue;
        if (!isVirtioInputDeviceDescriptor(descriptor)) continue;
        if (!deviceHasBootstrapQueueGrant(bootstrap_handoff, descriptor)) continue;
        const classified = hintedInputDeviceKind(bootstrap_handoff, descriptor) orelse classifyInputDeviceDescriptor(descriptor) orelse null;
        if (classified == null) continue;
        if (classified != kind) continue;
        return descriptor;
    }
    return null;
}

fn findBlockDeviceDescriptor(
    bootstrap_handoff: manager_init_bootstrap_abi.ConfigPage,
    kind: BlockDeviceKind,
) ?init_bootstrap_abi.DeviceDescriptor {
    _ = kind;
    const page = descriptorPage() orelse return null;
    var i: usize = 0;
    while (i < page.device_count and i < init_bootstrap_abi.max_device_descriptors) : (i += 1) {
        const descriptor = page.devices[i];
        if (!isBootstrapDeviceDescriptorPresent(descriptor)) continue;
        if (!isVirtioBlockDeviceDescriptor(descriptor)) continue;
        if (!deviceHasBootstrapQueueGrant(bootstrap_handoff, descriptor)) continue;
        return descriptor;
    }
    return null;
}

fn findGpuDeviceDescriptor(
    bootstrap_handoff: manager_init_bootstrap_abi.ConfigPage,
) ?init_bootstrap_abi.DeviceDescriptor {
    const page = descriptorPage() orelse return null;
    var i: usize = 0;
    while (i < page.device_count and i < init_bootstrap_abi.max_device_descriptors) : (i += 1) {
        const descriptor = page.devices[i];
        if (!isBootstrapDeviceDescriptorPresent(descriptor)) continue;
        if (!isVirtioGpuDeviceDescriptor(descriptor)) continue;
        if (!deviceHasBootstrapQueueGrant(bootstrap_handoff, descriptor)) continue;
        return descriptor;
    }
    return null;
}

fn findConsoleDeviceDescriptor(
    bootstrap_handoff: manager_init_bootstrap_abi.ConfigPage,
) ?init_bootstrap_abi.DeviceDescriptor {
    const page = descriptorPage() orelse return null;
    var i: usize = 0;
    while (i < page.device_count and i < init_bootstrap_abi.max_device_descriptors) : (i += 1) {
        const descriptor = page.devices[i];
        if (!isBootstrapDeviceDescriptorPresent(descriptor)) continue;
        if (!isVirtioConsoleDeviceDescriptor(descriptor)) continue;
        if (!deviceHasBootstrapQueueGrant(bootstrap_handoff, descriptor)) continue;
        return descriptor;
    }
    return null;
}

fn requireInputDeviceDescriptor(
    bootstrap_handoff: manager_init_bootstrap_abi.ConfigPage,
    kind: InputDeviceKind,
    failure_message: []const u8,
) init_bootstrap_abi.DeviceDescriptor {
    return findInputDeviceDescriptor(bootstrap_handoff, kind) orelse fail(failure_message);
}

fn requireBlockDeviceDescriptor(
    bootstrap_handoff: manager_init_bootstrap_abi.ConfigPage,
    kind: BlockDeviceKind,
    failure_message: []const u8,
) init_bootstrap_abi.DeviceDescriptor {
    return findBlockDeviceDescriptor(bootstrap_handoff, kind) orelse fail(failure_message);
}

fn requirePrimaryDisplayDescriptor() init_bootstrap_abi.DisplayDescriptor {
    const page = descriptorPage() orelse fail("ManagerInit: descriptor page missing\n");
    if ((page.primary_display.flags & init_bootstrap_abi.display_flag_present) == 0) {
        fail("ManagerInit: primary display descriptor missing\n");
    }
    return page.primary_display;
}

fn readBlockGeometry(descriptor: init_bootstrap_abi.DeviceDescriptor) ?BlockGeometry {
    const device_cfg_va = deviceCfgInspectVa(descriptor, false) orelse return null;
    const logical_block_size = blk: {
        const reported = mmioReadU32(device_cfg_va + virtio_blk_block_size_offset);
        break :blk if (reported != 0) @as(u64, reported) else @as(u64, 512);
    };
    return .{
        .capacity_sectors = mmioReadU64(device_cfg_va + virtio_blk_capacity_offset),
        .logical_block_size = logical_block_size,
    };
}

fn bootfsArchiveHeader() ?*const bootfs_format.BootFsHeader {
    const page = descriptorPage() orelse return null;
    const archive = page.bootfs_archive;
    if ((archive.flags & init_bootstrap_abi.boot_archive_flag_present) == 0) return null;
    if (archive.image_va == 0 or archive.size_bytes < @sizeOf(bootfs_format.BootFsHeader)) return null;
    const header: *const bootfs_format.BootFsHeader = @ptrFromInt(archive.image_va);
    if (header.magic != bootfs_format.magic or header.version != bootfs_format.version) return null;
    if (header.image_bytes > archive.size_bytes) return null;
    const entry_table_end = header.entry_table_offset + @as(u64, header.entry_count) * @sizeOf(bootfs_format.BootFsEntry);
    if (header.entry_table_offset < header.header_bytes or entry_table_end > header.image_bytes) return null;
    if (header.string_table_offset + header.string_table_bytes > header.image_bytes) return null;
    if (header.data_offset + header.data_bytes > header.image_bytes) return null;
    return header;
}

fn bootfsPathBytes(header: *const bootfs_format.BootFsHeader, entry: bootfs_format.BootFsEntry) ?[]const u8 {
    const path_offset = header.string_table_offset + entry.path_offset;
    const path_end = path_offset + entry.path_bytes;
    if (path_end > header.image_bytes) return null;
    const path_ptr: [*]const u8 = @ptrFromInt(@intFromPtr(header) + path_offset);
    return path_ptr[0..entry.path_bytes];
}

fn loadFileFromBootFs(path: []const u8, out: []u8) ?usize {
    const header = bootfsArchiveHeader() orelse return null;
    const entry_ptr: [*]const bootfs_format.BootFsEntry = @ptrFromInt(@intFromPtr(header) + header.entry_table_offset);
    var entry_index: usize = 0;
    while (entry_index < header.entry_count) : (entry_index += 1) {
        const entry = entry_ptr[entry_index];
        if (entry.kind != bootfs_format.kind_regular) continue;
        const entry_path = bootfsPathBytes(header, entry) orelse continue;
        if (!std.mem.eql(u8, entry_path, path)) continue;
        const data_end = entry.data_offset + entry.data_bytes;
        if (entry.data_offset < header.data_offset or data_end > header.image_bytes) return null;
        if (entry.data_bytes > out.len) return null;
        const src: [*]const u8 = @ptrFromInt(@intFromPtr(header) + entry.data_offset);
        @memcpy(out[0..@intCast(entry.data_bytes)], src[0..@intCast(entry.data_bytes)]);
        return @intCast(entry.data_bytes);
    }
    return null;
}

fn openExecFromBootFs(path: []const u8, bootfs_vm_token: u64) ?rootfs_core.OpenExecResult {
    const header = bootfsArchiveHeader() orelse return null;
    const entry_ptr: [*]const bootfs_format.BootFsEntry = @ptrFromInt(@intFromPtr(header) + header.entry_table_offset);
    var entry_index: usize = 0;
    while (entry_index < header.entry_count) : (entry_index += 1) {
        const entry = entry_ptr[entry_index];
        if (entry.kind != bootfs_format.kind_regular) continue;
        const entry_path = bootfsPathBytes(header, entry) orelse continue;
        if (!std.mem.eql(u8, entry_path, path)) continue;
        const data_end = entry.data_offset + entry.data_bytes;
        if (entry.data_offset < header.data_offset or data_end > header.image_bytes) return null;
        const vm_token = sliceVmObject(bootfs_vm_token, entry.data_offset, entry.data_bytes, .{ .read = true });
        if (image_abi.decodeVmObjectToken(vm_token) == null) return null;
        const exec_token = installExecImage(vm_token, .{ .exec = true });
        if (image_abi.decodeExecImageToken(exec_token) == null) return null;
        return .{
            .token = exec_token,
            .file_bytes = entry.data_bytes,
        };
    }
    return null;
}

fn startupManifestFail(message: []const u8) noreturn {
    _ = userLog(message);
    while (true) asm volatile ("pause");
}

fn loadStartupManifestFromRootFs(path: []const u8) ?*const StartupManifest {
    const file_bytes = rootfs_core.fileSize(path) orelse return null;
    if (file_bytes > startup_manifest_storage.bytes.len) return null;
    @memset(startup_manifest_storage.bytes[0..], 0);
    startup_manifest_storage.len = 0;
    const bytes_read = rootfs_core.loadFile(path, startup_manifest_storage.bytes[0..@intCast(file_bytes)]) orelse return null;
    if (bytes_read != file_bytes) return null;
    startup_manifest_storage.len = @intCast(bytes_read);
    return &startup_manifest_storage;
}

fn loadStartupManifestFromBootFs(path: []const u8) ?*const StartupManifest {
    @memset(startup_manifest_storage.bytes[0..], 0);
    startup_manifest_storage.len = 0;
    const bytes_read = loadFileFromBootFs(path, startup_manifest_storage.bytes[0..]) orelse return null;
    startup_manifest_storage.len = bytes_read;
    return &startup_manifest_storage;
}

fn parseStartupEnsureFlags(value: []const u8) u64 {
    var flags: u64 = 0;
    var it = std.mem.tokenizeScalar(u8, value, ',');
    while (it.next()) |item_raw| {
        const item = std.mem.trim(u8, item_raw, " \t");
        if (item.len == 0) continue;
        flags |= startup_plan_abi.ensureBitFromKey(item) orelse startupManifestFail("ManagerInit: unknown startup ensure flag\n");
    }
    return flags;
}

fn parseStartupRequirementFlags(value: []const u8) u64 {
    var flags: u64 = 0;
    var it = std.mem.tokenizeScalar(u8, value, ',');
    while (it.next()) |item_raw| {
        const item = std.mem.trim(u8, item_raw, " \t");
        if (item.len == 0) continue;
        flags |= startup_plan_abi.requirementBitFromKey(item) orelse startupManifestFail("ManagerInit: unknown startup require flag\n");
    }
    return flags;
}

fn appendStartupNamedDependency(
    out: *[startup_named_dependency_max][]const u8,
    count: *usize,
    value: []const u8,
    failure_message: []const u8,
) void {
    if (value.len == 0) return;
    var i: usize = 0;
    while (i < count.*) : (i += 1) {
        if (std.mem.eql(u8, out[i], value)) return;
    }
    if (count.* >= startup_named_dependency_max) startupManifestFail(failure_message);
    out[count.*] = value;
    count.* += 1;
}

fn parseStartupNamedDependencyList(
    out: *[startup_named_dependency_max][]const u8,
    count: *usize,
    value: []const u8,
    failure_message: []const u8,
) void {
    var it = std.mem.tokenizeScalar(u8, value, ',');
    while (it.next()) |item_raw| {
        const item = std.mem.trim(u8, item_raw, " \t");
        if (item.len == 0) continue;
        appendStartupNamedDependency(out, count, item, failure_message);
    }
}

fn defaultStartupPolicyLabel(policy: *const StartupPolicy) []const u8 {
    return switch (policy.action) {
        .vfs => "vfs",
        .input_driver => "input driver",
        .block_driver => "block driver",
        .console_driver => "console driver",
        .gpu_driver => "gpu driver",
        .persistent_fs_server => "persistent fs",
        .fat_server => "fat server",
        .window_service => "window service",
        .process => "process",
        .process_builder => "process builder",
        else => "service",
    };
}

fn normalizeStartupPolicy(policy: *StartupPolicy) void {
    switch (policy.action) {
        .vfs => policy.require_flags |= startup_plan_abi.require_flag_block_service,
        .persistent_fs_server => policy.require_flags |= startup_plan_abi.require_flag_block_service,
        .fat_server => policy.require_flags |= startup_plan_abi.require_flag_persistent_fs_service,
        else => {},
    }
}

fn validateStartupPolicy(policy: *StartupPolicy) void {
    normalizeStartupPolicy(policy);
    switch (policy.action) {
        .vfs => {},
        .input_driver => if (policy.input_kind == null) startupManifestFail("ManagerInit: startup input driver missing selector\n"),
        .block_driver => if (policy.block_kind == null) startupManifestFail("ManagerInit: startup block driver missing selector\n"),
        .console_driver => if (policy.device_kind == null) startupManifestFail("ManagerInit: startup console driver missing selector\n"),
        .gpu_driver => {},
        .persistent_fs_server => {},
        .fat_server => {},
        .window_service => {},
        .process => {},
        .process_builder => {},
        else => startupManifestFail("ManagerInit: unsupported startup action\n"),
    }
    if (policy.name.len == 0) startupManifestFail("ManagerInit: startup manifest name missing\n");
    if (policy.label.len == 0) policy.label = defaultStartupPolicyLabel(policy);
}

fn parseStartupPolicyToken(policy: *StartupPolicy, token: []const u8, has_action: *bool, has_path: *bool, has_name: *bool) void {
    const eq_index = std.mem.indexOfScalar(u8, token, '=') orelse startupManifestFail("ManagerInit: malformed startup manifest token\n");
    const key = token[0..eq_index];
    const value = token[eq_index + 1 ..];
    if (key.len == 0 or value.len == 0) startupManifestFail("ManagerInit: malformed startup manifest token\n");

    if (std.mem.eql(u8, key, "action")) {
        policy.action = startup_plan_abi.actionFromKey(value) orelse startupManifestFail("ManagerInit: unknown startup action\n");
        has_action.* = true;
        return;
    }
    if (std.mem.eql(u8, key, "path")) {
        policy.path = value;
        has_path.* = true;
        return;
    }
    if (std.mem.eql(u8, key, "name")) {
        policy.name = value;
        has_name.* = true;
        return;
    }
    if (std.mem.eql(u8, key, "label")) {
        policy.label = value;
        return;
    }
    if (std.mem.eql(u8, key, "load")) {
        policy.exec_source = startup_plan_abi.execSourceFromKey(value) orelse startupManifestFail("ManagerInit: unknown startup exec source\n");
        return;
    }
    if (std.mem.eql(u8, key, "ensure")) {
        policy.ensure_flags |= parseStartupEnsureFlags(value);
        return;
    }
    if (std.mem.eql(u8, key, "requires")) {
        policy.require_flags |= parseStartupRequirementFlags(value);
        return;
    }
    if (std.mem.eql(u8, key, "after")) {
        parseStartupNamedDependencyList(&policy.after_names, &policy.after_count, value, "ManagerInit: too many startup after dependencies\n");
        return;
    }
    if (std.mem.eql(u8, key, "provides")) {
        parseStartupNamedDependencyList(&policy.provide_names, &policy.provide_count, value, "ManagerInit: too many startup provides entries\n");
        return;
    }
    if (std.mem.eql(u8, key, "block")) {
        policy.block_kind = startup_plan_abi.blockSelectorFromKey(value) orelse startupManifestFail("ManagerInit: unknown startup block selector\n");
        return;
    }
    if (std.mem.eql(u8, key, "input")) {
        policy.input_kind = startup_plan_abi.inputSelectorFromKey(value) orelse startupManifestFail("ManagerInit: unknown startup input selector\n");
        return;
    }
    if (std.mem.eql(u8, key, "device")) {
        policy.device_kind = startup_plan_abi.deviceSelectorFromKey(value) orelse startupManifestFail("ManagerInit: unknown startup device selector\n");
        return;
    }
    startupManifestFail("ManagerInit: unknown startup manifest key\n");
}

fn parseStartupManifestLine(raw_line: []const u8) ?StartupPolicy {
    const line = std.mem.trim(u8, raw_line, " \t\r");
    if (line.len == 0 or line[0] == '#') return null;

    var tokens = std.mem.tokenizeAny(u8, line, " \t");
    var policy = StartupPolicy{};
    var has_action = false;
    var has_path = false;
    var has_name = false;
    while (tokens.next()) |token| parseStartupPolicyToken(&policy, token, &has_action, &has_path, &has_name);
    if (!has_action) startupManifestFail("ManagerInit: startup manifest action missing\n");
    if (!has_path or policy.path.len == 0) startupManifestFail("ManagerInit: startup manifest path missing\n");
    if (!has_name) startupManifestFail("ManagerInit: startup manifest name missing\n");
    validateStartupPolicy(&policy);
    return policy;
}

fn allocDynamicServiceEndpointId() u64 {
    const endpoint_id = next_dynamic_service_endpoint_id;
    next_dynamic_service_endpoint_id +%= 1;
    return endpoint_id;
}

fn allocDynamicBootstrapSourceVa() u64 {
    const source_va = next_dynamic_bootstrap_source_va;
    next_dynamic_bootstrap_source_va +%= 0x1000;
    return source_va;
}

fn waitForBootstrapHandoff() manager_init_bootstrap_abi.ConfigPage {
    const page: *const volatile manager_init_bootstrap_abi.ConfigPage = @ptrFromInt(manager_init_bootstrap_abi.config_target_va);
    var spin_count: usize = 0;
    while (true) {
        if (page.magic == manager_init_bootstrap_abi.magic and
            page.version == manager_init_bootstrap_abi.version and
            page.ready != 0)
        {
            return page.*;
        }
        if (spin_count < 1024) {
            spin_count += 1;
            asm volatile ("pause");
            continue;
        }
        _ = waitEvent(false, 1);
    }
}

fn allocChildServiceRegistryPage(
    window_service: ?service_registry_abi.ServiceEntry,
    capctl_process_slot: ?u64,
    capctl_endpoint_id: ?u64,
    block_process_slot: ?u64,
    block_endpoint_id: ?u64,
    gpu_process_slot: ?u64,
    gpu_endpoint_id: ?u64,
    console_process_slot: ?u64,
    console_endpoint_id: ?u64,
    persistent_fs_process_slot: ?u64,
    persistent_fs_endpoint_id: ?u64,
) ?u64 {
    const source_va = allocDynamicBootstrapSourceVa();
    var registry_paddr: u64 = 0;
    if (allocMapPages(source_va, 1, true, @intFromPtr(&registry_paddr)) != 0) return null;
    if (registry_paddr < 0x1000) return null;
    service_registry_abi.initPage(source_va);
    if (window_service) |entry| service_registry_abi.addServiceEntry(source_va, entry);
    if (capctl_process_slot != null and capctl_endpoint_id != null) {
        service_registry_abi.addServiceWithProcessSlot(source_va, .capctl, capctl_process_slot.?, capctl_endpoint_id.?);
    }
    if (block_process_slot != null and block_endpoint_id != null) {
        service_registry_abi.addServiceWithProcessSlot(source_va, .block, block_process_slot.?, block_endpoint_id.?);
    }
    if (gpu_process_slot != null and gpu_endpoint_id != null) {
        service_registry_abi.addServiceWithProcessSlot(source_va, .gpu, gpu_process_slot.?, gpu_endpoint_id.?);
    }
    if (console_process_slot != null and console_endpoint_id != null) {
        service_registry_abi.addServiceWithProcessSlot(source_va, .console, console_process_slot.?, console_endpoint_id.?);
    }
    if (persistent_fs_process_slot != null and persistent_fs_endpoint_id != null) {
        service_registry_abi.addServiceWithProcessSlot(source_va, .persistent_fs, persistent_fs_process_slot.?, persistent_fs_endpoint_id.?);
    }
    return source_va;
}

const LaunchContext = struct {
    const QueueGrant = struct {
        iommu_token: u64,
        queue_index: u64,
        submit_token: u64,
        notify_token: u64,
        command_token: u64,
    };

    const BlockProfile = capctl_protocol.BlockProfile;

    has_boot_display: bool = false,
    bootfs_ready: bool = false,
    rootfs_ready: bool = false,
    bootstrap_handoff: manager_init_bootstrap_abi.ConfigPage,
    primary_display: init_bootstrap_abi.DisplayDescriptor,
    keyboard_input: init_bootstrap_abi.DeviceDescriptor,
    pointer_input: ?init_bootstrap_abi.DeviceDescriptor = null,
    block_device: init_bootstrap_abi.DeviceDescriptor,
    console_device: ?init_bootstrap_abi.DeviceDescriptor = null,
    gpu_device: ?init_bootstrap_abi.DeviceDescriptor = null,
    window_service_page: init_bootstrap_abi.SpawnPageDescriptor,
    window_service: ?service_registry_abi.ServiceEntry = null,
    shared_service_registry_source_va: ?u64 = null,
    shared_stdio_bootstrap_source_va: ?u64 = null,
    shared_process_args_env_bootstrap_source_va: ?u64 = null,
    shared_process_exit_bootstrap_source_va: ?u64 = null,
    pointer_shared_source_va: ?u64 = null,
    pointer_shared_paddr: u64 = 0,
    capctl_endpoint_id: ?u64 = null,
    block_process_slot: ?u64 = null,
    block_endpoint_id: ?u64 = null,
    block_iommu_source_token: u64 = 0,
    block_submit_source_token: u64 = 0,
    block_notify_source_token: u64 = 0,
    block_command_source_token: u64 = 0,
    block_iommu_base_token: u64 = 0,
    block_submit_base_token: u64 = 0,
    block_notify_base_token: u64 = 0,
    block_command_base_token: u64 = 0,
    block_profile: BlockProfile = .full,
    block_policy: ?StartupPolicy = null,
    gpu_process_slot: ?u64 = null,
    gpu_endpoint_id: ?u64 = null,
    gpu_policy: ?StartupPolicy = null,
    console_process_slot: ?u64 = null,
    console_endpoint_id: ?u64 = null,
    console_policy: ?StartupPolicy = null,
    persistent_fs_process_slot: ?u64 = null,
    persistent_fs_endpoint_id: ?u64 = null,
    persistent_fs_policy: ?StartupPolicy = null,
    fat_fs_process_slot: ?u64 = null,
    fat_fs_endpoint_id: ?u64 = null,
    fat_fs_policy: ?StartupPolicy = null,
    capctl_request_paddr: u64 = 0,
    capctl_response_paddr: u64 = 0,
    ready_name_count: usize = 0,
    ready_names: [startup_ready_name_max][]const u8 = [_][]const u8{""} ** startup_ready_name_max,
    cached_execs: [startup_plan_abi.max_startup_program_descriptors]CachedStartupExec = [_]CachedStartupExec{.{}} ** startup_plan_abi.max_startup_program_descriptors,

    fn logRoleLine(_: *LaunchContext, action: []const u8, label: []const u8, result: []const u8) void {
        var buf: [96]u8 = undefined;
        const message = std.fmt.bufPrint(&buf, "ManagerInit: {s} {s} {s}\n", .{ action, label, result }) catch return;
        _ = userLog(message);
    }

    fn logRoleHex(_: *LaunchContext, label: []const u8, suffix: []const u8, value: u64) void {
        var buf: [96]u8 = undefined;
        const prefix = std.fmt.bufPrint(&buf, "ManagerInit: {s}{s}", .{ label, suffix }) catch return;
        userLogHex(prefix, value);
    }

    fn ensureRootFsReader(self: *LaunchContext) void {
        if (self.rootfs_ready) return;
        const queue_grant = self.findQueueGrant(self.block_device, 0) orelse fail("ManagerInit: block queue grant missing\n");
        const block_geometry = readBlockGeometry(self.block_device) orelse fail("ManagerInit: block geometry failed\n");
        if (!rootfs_core.init(.{
            .rootfs_start_block = persistent_fs_start_block,
            .capacity_sectors = block_geometry.capacity_sectors,
            .logical_block_size = block_geometry.logical_block_size,
            .common_page_paddr = self.block_device.common_page_paddr,
            .notify_page_paddr = self.block_device.notify_page_paddr,
            .isr_page_paddr = self.block_device.isr_page_paddr,
            .common_page_offset = self.block_device.common_page_offset,
            .notify_page_offset = self.block_device.notify_page_offset,
            .isr_page_offset = self.block_device.isr_page_offset,
            .notify_off_multiplier = self.block_device.notify_off_multiplier,
            .queue_submit_token = queue_grant.submit_token,
            .queue_notify_token = queue_grant.notify_token,
        })) fail("ManagerInit: rootfs init failed\n");
        self.rootfs_ready = true;
        _ = userLog("ManagerInit: rootfs reader ready\n");
    }

    fn ensureBootFsArchive(self: *LaunchContext) void {
        if (self.bootfs_ready) return;
        const bootfs_vm_token = self.bootstrap_handoff.bootfs_vm_token;
        if (image_abi.decodeVmObjectToken(bootfs_vm_token) == null) return;
        const page = descriptorPage() orelse fail("ManagerInit: descriptor page missing\n");
        const archive = page.bootfs_archive;
        if ((archive.flags & init_bootstrap_abi.boot_archive_flag_present) == 0) return;
        if (archive.image_va == 0 or archive.size_bytes == 0) return;
        if (mapVmObject(bootfs_vm_token, archive.image_va) != 0) fail("ManagerInit: bootfs map failed\n");
        self.bootfs_ready = true;
    }

    fn requireExecFromRootFs(self: *LaunchContext, path: []const u8, label: []const u8) rootfs_core.OpenExecResult {
        self.ensureRootFsReader();
        const exec = rootfs_core.openExec(path) orelse {
            self.logRoleLine("open_exec", label, "failed");
            fail("ManagerInit: rootfs open_exec failed\n");
        };
        self.logRoleLine("open_exec", label, "ok");
        return exec;
    }

    fn requireExecFromBootFs(self: *LaunchContext, path: []const u8, label: []const u8) rootfs_core.OpenExecResult {
        self.ensureBootFsArchive();
        const exec = openExecFromBootFs(path, self.bootstrap_handoff.bootfs_vm_token) orelse {
            self.logRoleLine("open_exec", label, "failed");
            fail("ManagerInit: bootfs open_exec failed\n");
        };
        self.logRoleLine("open_exec", label, "ok");
        return exec;
    }

    fn findCachedExec(self: *LaunchContext, exec_source: startup_plan_abi.StartupExecSource, path: []const u8) ?rootfs_core.OpenExecResult {
        for (&self.cached_execs) |*entry| {
            if (!entry.valid) continue;
            if (entry.exec_source != exec_source) continue;
            if (std.mem.eql(u8, entry.path(), path)) return entry.result;
        }
        return null;
    }

    fn storeCachedExec(self: *LaunchContext, exec_source: startup_plan_abi.StartupExecSource, path: []const u8, result: rootfs_core.OpenExecResult) void {
        if (path.len > startup_plan_abi.path_max_bytes) fail("ManagerInit: cached exec path too long\n");
        for (&self.cached_execs) |*entry| {
            if (!entry.valid) continue;
            if (entry.exec_source != exec_source) continue;
            if (!std.mem.eql(u8, entry.path(), path)) continue;
            entry.result = result;
            return;
        }
        for (&self.cached_execs) |*entry| {
            if (entry.valid) continue;
            entry.* = .{
                .valid = true,
                .exec_source = exec_source,
                .path_len = @intCast(path.len),
                .result = result,
            };
            @memcpy(entry.path_bytes[0..path.len], path);
            return;
        }
        fail("ManagerInit: cached exec table full\n");
    }

    fn fetchExecForStartupSource(self: *LaunchContext, exec_source: startup_plan_abi.StartupExecSource, path: []const u8, label: []const u8) rootfs_core.OpenExecResult {
        if (self.findCachedExec(exec_source, path)) |exec| return exec;
        const exec = switch (exec_source) {
            .startup_path => self.requireExecFromRootFs(path, label),
            .bootfs => self.requireExecFromBootFs(path, label),
        };
        self.storeCachedExec(exec_source, path, exec);
        return exec;
    }

    fn requireShellExec(self: *LaunchContext) rootfs_core.OpenExecResult {
        if (self.findCachedExec(.bootfs, rootfs_shell_path)) |exec| return exec;
        if (self.findCachedExec(.startup_path, rootfs_shell_path)) |exec| return exec;
        self.ensureBootFsArchive();
        if (openExecFromBootFs(rootfs_shell_path, self.bootstrap_handoff.bootfs_vm_token)) |exec| {
            self.logRoleLine("open_exec", "shell", "ok");
            self.storeCachedExec(.bootfs, rootfs_shell_path, exec);
            return exec;
        }
        return self.fetchExecForStartupSource(.startup_path, rootfs_shell_path, "shell");
    }

    fn cacheExecForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        _ = self.fetchExecForStartupSource(policy.exec_source, policy.path, policy.label);
    }

    fn requireExecForPolicy(self: *LaunchContext, policy: StartupPolicy) rootfs_core.OpenExecResult {
        return self.fetchExecForStartupSource(policy.exec_source, policy.path, policy.label);
    }

    fn allocWritableBootstrapPage(_: *LaunchContext, label: []const u8) u64 {
        const source_va = allocDynamicBootstrapSourceVa();
        var paddr: u64 = 0;
        if (allocMapPages(source_va, 1, true, @intFromPtr(&paddr)) != 0 or paddr < 0x1000) fail(label);
        return source_va;
    }

    fn ensurePointerSharedPage(self: *LaunchContext) u64 {
        if (self.pointer_shared_source_va) |source_va| return source_va;
        const source_va = allocDynamicBootstrapSourceVa();
        var paddr: u64 = 0;
        if (allocMapPages(source_va, 1, true, @intFromPtr(&paddr)) != 0 or paddr < 0x1000) {
            fail("ManagerInit: alloc pointer shared page failed\n");
        }
        const bytes: [*]volatile u8 = @ptrFromInt(source_va);
        var i: usize = 0;
        while (i < 4096) : (i += 1) bytes[i] = 0;
        self.pointer_shared_source_va = source_va;
        self.pointer_shared_paddr = paddr;
        return source_va;
    }

    fn findQueueGrant(self: *const LaunchContext, descriptor: init_bootstrap_abi.DeviceDescriptor, queue_index: u64) ?QueueGrant {
        var i: usize = 0;
        while (i < self.bootstrap_handoff.device_count and i < manager_init_bootstrap_abi.max_device_grants) : (i += 1) {
            const grant = self.bootstrap_handoff.device_grants[i];
            if (grant.device_page_paddr != descriptor.device_page_paddr) continue;
            if (grant.iommu_token == 0 or grant.command_token == 0) return null;
            var queue_grant_index: usize = 0;
            while (@as(u64, @intCast(queue_grant_index)) < grant.queue_grant_count and queue_grant_index < manager_init_bootstrap_abi.max_device_queue_grants) : (queue_grant_index += 1) {
                const queue_grant = grant.queue_grants[queue_grant_index];
                if (queue_grant.queue_index != queue_index) continue;
                if (queue_grant.submit_token == 0 or queue_grant.notify_token == 0) return null;
                return .{
                    .iommu_token = grant.iommu_token,
                    .queue_index = queue_grant.queue_index,
                    .submit_token = queue_grant.submit_token,
                    .notify_token = queue_grant.notify_token,
                    .command_token = grant.command_token,
                };
            }
        }
        return null;
    }

    fn ensureSharedServiceRegistryPage(self: *LaunchContext) u64 {
        if (self.shared_service_registry_source_va) |source_va| return source_va;
        const source_va = allocChildServiceRegistryPage(
            self.window_service,
            getProcessSlot(),
            self.capctl_endpoint_id,
            self.block_process_slot,
            self.block_endpoint_id,
            self.gpu_process_slot,
            self.gpu_endpoint_id,
            self.console_process_slot,
            self.console_endpoint_id,
            self.persistent_fs_process_slot,
            self.persistent_fs_endpoint_id,
        ) orelse fail("ManagerInit: alloc shared service registry failed\n");
        self.shared_service_registry_source_va = source_va;
        return source_va;
    }

    fn capctlStatusFlags(self: *const LaunchContext) u64 {
        var flags: u64 = 0;
        if (self.block_process_slot != null and self.block_endpoint_id != null) {
            flags |= capctl_protocol.status_flag_block_present;
        }
        if (self.gpu_process_slot != null and self.gpu_endpoint_id != null) {
            flags |= capctl_protocol.status_flag_gpu_present;
        }
        if (self.block_iommu_source_token != 0) flags |= capctl_protocol.status_flag_iommu_active;
        if (self.block_submit_source_token != 0 and self.block_notify_source_token != 0) {
            flags |= capctl_protocol.status_flag_virtqueue_active;
        }
        if (self.block_command_source_token != 0) flags |= capctl_protocol.status_flag_command_active;
        return flags;
    }

    fn commandMaskForProfile(profile: BlockProfile) u64 {
        const read_bit: u64 = @as(u64, 1) << @as(u6, @intCast(@intFromEnum(queue_abi.CommandOpcodeClass.blk_read)));
        const write_bit: u64 = @as(u64, 1) << @as(u6, @intCast(@intFromEnum(queue_abi.CommandOpcodeClass.blk_write)));
        const flush_bit: u64 = @as(u64, 1) << @as(u6, @intCast(@intFromEnum(queue_abi.CommandOpcodeClass.blk_flush)));
        const identify_bit: u64 = @as(u64, 1) << @as(u6, @intCast(@intFromEnum(queue_abi.CommandOpcodeClass.blk_identify)));
        return switch (profile) {
            .full => read_bit | write_bit | flush_bit | identify_bit,
            .read_only => read_bit,
            .no_iommu, .no_virtqueue => read_bit | write_bit | flush_bit | identify_bit,
        };
    }

    fn prepareCommandProfileSource(self: *LaunchContext, profile: BlockProfile) bool {
        if (self.block_command_source_token != 0 and self.block_command_source_token != self.block_command_base_token) {
            const status = revokeQueueCap(queue_abi.encodeCommandCapToken(self.block_command_source_token));
            if (status != 0) return false;
            self.block_command_source_token = 0;
        }
        const mask = commandMaskForProfile(profile);
        const derived_encoded = deriveCommandCap(queue_abi.encodeCommandCapToken(self.block_command_base_token), mask);
        const derived = queue_abi.decodeCommandCapToken(derived_encoded) orelse return false;
        self.block_command_source_token = derived;
        return true;
    }

    fn refreshBlockProfileSources(self: *LaunchContext, profile: BlockProfile) bool {
        switch (profile) {
            .full, .read_only => {
                self.block_iommu_source_token = self.block_iommu_base_token;
                self.block_submit_source_token = self.block_submit_base_token;
                self.block_notify_source_token = self.block_notify_base_token;
            },
            .no_iommu => {
                self.block_iommu_source_token = 0;
                self.block_submit_source_token = self.block_submit_base_token;
                self.block_notify_source_token = self.block_notify_base_token;
            },
            .no_virtqueue => {
                self.block_iommu_source_token = self.block_iommu_base_token;
                self.block_submit_source_token = 0;
                self.block_notify_source_token = 0;
            },
        }
        return self.prepareCommandProfileSource(profile);
    }

    fn publishCapCtlService(self: *LaunchContext) void {
        if (self.capctl_endpoint_id != null) return;
        const endpoint_id = capctl_protocol.endpoint_id;
        const process_slot = getProcessSlot();
        if (installEndpoint(endpoint_id, process_slot) != 0) fail("ManagerInit: capctl endpoint install failed\n");
        if (publishServiceEndpoint(endpoint_id, process_slot) != 0) fail("ManagerInit: capctl endpoint publish failed\n");
        service_registry_abi.setServiceWithProcessSlot(self.window_service_page.source_va, .capctl, process_slot, endpoint_id);
        if (self.shared_service_registry_source_va) |source_va| {
            service_registry_abi.setServiceWithProcessSlot(source_va, .capctl, process_slot, endpoint_id);
        }
        self.capctl_endpoint_id = endpoint_id;
        _ = userLog("ManagerInit: capctl ready\n");
    }

    fn ensureCapCtlMap(self: *LaunchContext, target_va: u64, paddr: u64, writable: bool, last_paddr: *u64) bool {
        _ = self;
        if (paddr < 0x1000) return false;
        if (last_paddr.* == paddr) return true;
        if (mapPage(target_va, paddr, writable) != 0) return false;
        last_paddr.* = paddr;
        return true;
    }

    fn writeCapCtlResponse(
        self: *const LaunchContext,
        response: *volatile capctl_protocol.Response,
        opcode: capctl_protocol.Opcode,
        request_seq: u64,
        status: capctl_protocol.ResponseStatus,
        detail: u64,
    ) void {
        response.magic = capctl_protocol.magic;
        response.version = capctl_protocol.version;
        response.opcode = capctl_protocol.opcodeRaw(opcode);
        response.status = capctl_protocol.responseStatusRaw(status);
        response.detail = detail;
        response.block_process_slot = self.block_process_slot orelse 0;
        response.block_endpoint_id = self.block_endpoint_id orelse 0;
        response.status_flags = self.capctlStatusFlags();
        response.block_profile = capctl_protocol.blockProfileRaw(self.block_profile);
        response.gpu_process_slot = self.gpu_process_slot orelse 0;
        response.gpu_endpoint_id = self.gpu_endpoint_id orelse 0;
        response.service_kind = 0;
        response.service_process_slot = 0;
        response.service_endpoint_id = 0;
        response.response_seq = request_seq;
    }

    fn writeCapCtlServiceResponse(
        self: *const LaunchContext,
        response: *volatile capctl_protocol.Response,
        request_seq: u64,
        status: capctl_protocol.ResponseStatus,
        detail: u64,
        kind: service_registry_abi.ServiceKind,
        process_slot: u64,
        endpoint_id: u64,
    ) void {
        self.writeCapCtlResponse(response, .launch_service, request_seq, status, detail);
        response.service_kind = @intFromEnum(kind);
        response.service_process_slot = process_slot;
        response.service_endpoint_id = endpoint_id;
    }

    fn revokeBlockIommu(self: *LaunchContext) u64 {
        if (self.block_iommu_source_token == 0) return 0;
        const status = revokeQueueCap(queue_abi.encodeIommuCapToken(self.block_iommu_source_token));
        if (status == 0) self.block_iommu_source_token = 0;
        return status;
    }

    fn revokeBlockVirtqueue(self: *LaunchContext) u64 {
        var first_error: u64 = 0;
        if (self.block_submit_source_token != 0) {
            const status = revokeQueueCap(queue_abi.encodeVirtqueueCapToken(self.block_submit_source_token));
            if (status == 0) {
                self.block_submit_source_token = 0;
            } else if (first_error == 0) {
                first_error = status;
            }
        }
        if (self.block_notify_source_token != 0) {
            const status = revokeQueueCap(queue_abi.encodeVirtqueueCapToken(self.block_notify_source_token));
            if (status == 0) {
                self.block_notify_source_token = 0;
            } else if (first_error == 0) {
                first_error = status;
            }
        }
        return first_error;
    }

    fn revokeBlockCommand(self: *LaunchContext) u64 {
        if (self.block_command_source_token == 0) return 0;
        const status = revokeQueueCap(queue_abi.encodeCommandCapToken(self.block_command_source_token));
        if (status == 0) self.block_command_source_token = 0;
        return status;
    }

    fn applyBlockProfile(self: *LaunchContext, profile: BlockProfile) capctl_protocol.ResponseStatus {
        const block_policy = self.block_policy orelse return .unsupported;
        const persistent_fs_policy = self.persistent_fs_policy orelse return .unsupported;
        if (!self.refreshBlockProfileSources(profile)) return .kernel_error;
        self.block_profile = profile;
        self.launchBlockDriverForPolicy(block_policy);
        switch (profile) {
            .full, .read_only => self.launchPersistentFsForPolicy(persistent_fs_policy),
            .no_iommu, .no_virtqueue => {
                service_registry_abi.removeService(self.window_service_page.source_va, .persistent_fs);
                if (self.shared_service_registry_source_va) |source_va| {
                    service_registry_abi.removeService(source_va, .persistent_fs);
                }
                self.persistent_fs_process_slot = null;
                self.persistent_fs_endpoint_id = null;
            },
        }
        return .ok;
    }

    fn launchGpuDriverFromExecRequest(self: *LaunchContext) capctl_protocol.ResponseStatus {
        if (self.gpu_process_slot != null and self.gpu_endpoint_id != null) return .already;
        if (self.gpu_device == null) return .unavailable;
        const policy = StartupPolicy{
            .action = .gpu_driver,
            .path = rootfs_gpu_driver_path,
            .name = "gpu",
            .label = "gpu_driver",
            .exec_source = .startup_path,
        };
        self.launchGpuDriverForPolicy(policy);
        self.markReadyName("gpu");
        self.markReadyName("gpu_service");
        return .ok;
    }

    const LaunchServiceResult = struct {
        status: capctl_protocol.ResponseStatus,
        process_slot: u64 = 0,
        endpoint_id: u64 = 0,
    };

    fn openRootFsExecForCapCtl(self: *LaunchContext, path: []const u8, label: []const u8) ?rootfs_core.OpenExecResult {
        if (self.findCachedExec(.bootfs, path)) |exec| return exec;
        if (self.findCachedExec(.startup_path, path)) |exec| return exec;
        self.ensureBootFsArchive();
        if (openExecFromBootFs(path, self.bootstrap_handoff.bootfs_vm_token)) |exec| {
            self.logRoleLine("open_exec", label, "bootfs");
            self.storeCachedExec(.bootfs, path, exec);
            return exec;
        }
        self.ensureRootFsReader();
        const exec = rootfs_core.openExec(path) orelse {
            self.logRoleLine("open_exec", label, "failed");
            return null;
        };
        self.logRoleLine("open_exec", label, "ok");
        self.storeCachedExec(.startup_path, path, exec);
        return exec;
    }

    fn launchRootFsWindowServiceFromCapCtl(self: *LaunchContext, path: []const u8) LaunchServiceResult {
        if (self.window_service) |entry| {
            return .{
                .status = .already,
                .process_slot = entry.process_slot,
                .endpoint_id = entry.endpoint_id,
            };
        }
        _ = userLog("ManagerInit: capctl launch window service begin\n");
        const exec = self.openRootFsExecForCapCtl(path, "window_service") orelse return .{ .status = .unavailable };
        const endpoint_id = allocDynamicServiceEndpointId();
        const registry_source_va = self.ensureSharedServiceRegistryPage();
        const stdio_source_va = self.ensureSharedStdioBootstrapPage();
        const args_env_source_va = self.ensureSharedProcessArgsEnvPage();
        const exit_status_source_va = self.ensureSharedProcessExitStatusPage();
        const pointer_shared_source_va = self.pointer_shared_source_va;

        generic_service_bootstrap_table_storage = .{};
        generic_service_bootstrap_table_storage.page_count = if (pointer_shared_source_va != null) 5 else 4;
        generic_service_bootstrap_table_storage.page_descriptors[0] = .{
            .source_va = registry_source_va,
            .target_va = process_abi.service_registry_shadow_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[1] = .{
            .source_va = stdio_source_va,
            .target_va = stdio_bootstrap_abi.target_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[2] = .{
            .source_va = args_env_source_va,
            .target_va = process_args_env_bootstrap_abi.target_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[3] = .{
            .source_va = exit_status_source_va,
            .target_va = process_exit_bootstrap_abi.target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        if (pointer_shared_source_va) |source_va| {
            generic_service_bootstrap_table_storage.page_descriptors[4] = .{
                .source_va = source_va,
                .target_va = input_bootstrap.pointer_shared_target_va,
                .flags = 0,
            };
        }

        const spawned = spawnExecWithExtendedBootstrapTable(exec.token, &generic_service_bootstrap_table_storage);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", "window_service", "failed");
            self.logRoleHex("window_service", " spawn ret=", spawned);
            return .{ .status = .kernel_error, .endpoint_id = endpoint_id };
        };
        self.logRoleLine("spawn", "window_service", "ok");
        if (publishServiceEndpoint(endpoint_id, child_slot) != 0) {
            return .{ .status = .kernel_error, .process_slot = child_slot, .endpoint_id = endpoint_id };
        }

        self.window_service = .{
            .kind = @intFromEnum(service_registry_abi.ServiceKind.window),
            .process_slot = child_slot,
            .endpoint_id = endpoint_id,
            .flags = service_registry_abi.service_flag_process_slot_compat,
        };
        service_registry_abi.setServiceWithProcessSlot(self.window_service_page.source_va, .window, child_slot, endpoint_id);
        if (self.shared_service_registry_source_va) |source_va| {
            service_registry_abi.setServiceWithProcessSlot(source_va, .window, child_slot, endpoint_id);
        }
        self.has_boot_display = true;
        _ = userLog("ManagerInit: window service ready\n");
        return .{ .status = .ok, .process_slot = child_slot, .endpoint_id = endpoint_id };
    }

    fn launchRootFsServiceFromCapCtl(self: *LaunchContext, kind: service_registry_abi.ServiceKind, path: []const u8) LaunchServiceResult {
        return switch (kind) {
            .window => self.launchRootFsWindowServiceFromCapCtl(path),
            else => .{ .status = .unsupported },
        };
    }

    fn publishSpawnedServiceFromCapCtl(self: *LaunchContext, kind: service_registry_abi.ServiceKind, child_slot: u64) LaunchServiceResult {
        if (child_slot == 0) return .{ .status = .invalid };
        switch (kind) {
            .window => {
                if (self.window_service) |entry| {
                    return .{
                        .status = .already,
                        .process_slot = entry.process_slot,
                        .endpoint_id = entry.endpoint_id,
                    };
                }
                const endpoint_id = allocDynamicServiceEndpointId();
                if (publishServiceEndpoint(endpoint_id, child_slot) != 0) {
                    return .{ .status = .kernel_error, .process_slot = child_slot, .endpoint_id = endpoint_id };
                }
                self.window_service = .{
                    .kind = @intFromEnum(service_registry_abi.ServiceKind.window),
                    .process_slot = child_slot,
                    .endpoint_id = endpoint_id,
                    .flags = service_registry_abi.service_flag_process_slot_compat,
                };
                service_registry_abi.setServiceWithProcessSlot(self.window_service_page.source_va, .window, child_slot, endpoint_id);
                if (self.shared_service_registry_source_va) |source_va| {
                    service_registry_abi.setServiceWithProcessSlot(source_va, .window, child_slot, endpoint_id);
                }
                self.has_boot_display = true;
                _ = userLog("ManagerInit: window service published\n");
                return .{ .status = .ok, .process_slot = child_slot, .endpoint_id = endpoint_id };
            },
            else => return .{ .status = .unsupported },
        }
    }

    fn copyCapCtlRequestPayloadPath(path_len_raw: u64, out: *[startup_plan_abi.path_max_bytes]u8) ?[]const u8 {
        if (path_len_raw == 0 or path_len_raw > capctl_protocol.request_payload_bytes or path_len_raw > out.len) return null;
        const path_len: usize = @intCast(path_len_raw);
        const payload: [*]const volatile u8 = @ptrFromInt(capctl_request_va + capctl_protocol.request_header_bytes);
        var i: usize = 0;
        while (i < path_len) : (i += 1) out[i] = payload[i];
        return out[0..path_len];
    }

    fn handleCapCtlRequest(self: *LaunchContext, request_paddr: u64) void {
        if (!self.ensureCapCtlMap(capctl_request_va, request_paddr, false, &self.capctl_request_paddr)) return;
        const request: *volatile capctl_protocol.Request = @ptrFromInt(capctl_request_va);
        if (request.magic != capctl_protocol.magic or
            request.version != capctl_protocol.version or
            request.request_seq == 0 or
            request.response_paddr < 0x1000)
        {
            return;
        }
        if (!self.ensureCapCtlMap(capctl_response_va, request.response_paddr, true, &self.capctl_response_paddr)) return;
        const response: *volatile capctl_protocol.Response = @ptrFromInt(capctl_response_va);
        const opcode = capctl_protocol.decodeOpcode(request.opcode) orelse {
            self.writeCapCtlResponse(response, .status, request.request_seq, .invalid, request.opcode);
            return;
        };
        switch (opcode) {
            .status => self.writeCapCtlResponse(response, .status, request.request_seq, .ok, 0),
            .revoke_iommu => {
                if (self.block_process_slot == null) {
                    self.writeCapCtlResponse(response, .revoke_iommu, request.request_seq, .unavailable, 0);
                    return;
                }
                if (self.block_iommu_source_token == 0) {
                    self.writeCapCtlResponse(response, .revoke_iommu, request.request_seq, .already, 0);
                    return;
                }
                const status = self.revokeBlockIommu();
                self.writeCapCtlResponse(response, .revoke_iommu, request.request_seq, if (status == 0) .ok else .kernel_error, status);
            },
            .revoke_virtqueue => {
                if (self.block_process_slot == null) {
                    self.writeCapCtlResponse(response, .revoke_virtqueue, request.request_seq, .unavailable, 0);
                    return;
                }
                if (self.block_submit_source_token == 0 and self.block_notify_source_token == 0) {
                    self.writeCapCtlResponse(response, .revoke_virtqueue, request.request_seq, .already, 0);
                    return;
                }
                const status = self.revokeBlockVirtqueue();
                self.writeCapCtlResponse(response, .revoke_virtqueue, request.request_seq, if (status == 0) .ok else .kernel_error, status);
            },
            .revoke_command => {
                if (self.block_process_slot == null) {
                    self.writeCapCtlResponse(response, .revoke_command, request.request_seq, .unavailable, 0);
                    return;
                }
                if (self.block_command_source_token == 0) {
                    self.writeCapCtlResponse(response, .revoke_command, request.request_seq, .already, 0);
                    return;
                }
                const status = self.revokeBlockCommand();
                self.writeCapCtlResponse(response, .revoke_command, request.request_seq, if (status == 0) .ok else .kernel_error, status);
            },
            .profile_full => {
                const status = self.applyBlockProfile(.full);
                self.writeCapCtlResponse(response, .profile_full, request.request_seq, status, 0);
            },
            .profile_read_only => {
                const status = self.applyBlockProfile(.read_only);
                self.writeCapCtlResponse(response, .profile_read_only, request.request_seq, status, 0);
            },
            .profile_no_iommu => {
                const status = self.applyBlockProfile(.no_iommu);
                self.writeCapCtlResponse(response, .profile_no_iommu, request.request_seq, status, 0);
            },
            .profile_no_virtqueue => {
                const status = self.applyBlockProfile(.no_virtqueue);
                self.writeCapCtlResponse(response, .profile_no_virtqueue, request.request_seq, status, 0);
            },
            .launch_gpu => {
                const status = self.launchGpuDriverFromExecRequest();
                self.writeCapCtlResponse(response, .launch_gpu, request.request_seq, status, 0);
            },
            .launch_service => {
                const kind = std.meta.intToEnum(service_registry_abi.ServiceKind, request.arg0) catch {
                    self.writeCapCtlResponse(response, .launch_service, request.request_seq, .invalid, request.arg0);
                    return;
                };
                var path_buf: [startup_plan_abi.path_max_bytes]u8 = undefined;
                const path = copyCapCtlRequestPayloadPath(request.arg1, &path_buf) orelse {
                    self.writeCapCtlResponse(response, .launch_service, request.request_seq, .invalid, request.arg1);
                    return;
                };
                const result = self.launchRootFsServiceFromCapCtl(kind, path);
                _ = userLog("ManagerInit: capctl launch service response\n");
                self.writeCapCtlServiceResponse(
                    response,
                    request.request_seq,
                    result.status,
                    0,
                    kind,
                    result.process_slot,
                    result.endpoint_id,
                );
            },
            .publish_service => {
                const kind = std.meta.intToEnum(service_registry_abi.ServiceKind, request.arg0) catch {
                    self.writeCapCtlResponse(response, .publish_service, request.request_seq, .invalid, request.arg0);
                    return;
                };
                const result = self.publishSpawnedServiceFromCapCtl(kind, request.arg1);
                self.writeCapCtlResponse(response, .publish_service, request.request_seq, result.status, 0);
                response.service_kind = @intFromEnum(kind);
                response.service_process_slot = result.process_slot;
                response.service_endpoint_id = result.endpoint_id;
            },
        }
    }

    fn ensureSharedStdioBootstrapPage(self: *LaunchContext) u64 {
        if (self.shared_stdio_bootstrap_source_va) |source_va| return source_va;
        const source_va = self.allocWritableBootstrapPage("ManagerInit: alloc stdio bootstrap page failed\n");
        stdio_bootstrap_abi.initZeroPage(source_va);
        self.shared_stdio_bootstrap_source_va = source_va;
        return source_va;
    }

    fn ensureSharedProcessArgsEnvPage(self: *LaunchContext) u64 {
        if (self.shared_process_args_env_bootstrap_source_va) |source_va| return source_va;
        const source_va = self.allocWritableBootstrapPage("ManagerInit: alloc process args env bootstrap page failed\n");
        process_args_env_bootstrap_abi.initZeroPage(source_va);
        self.shared_process_args_env_bootstrap_source_va = source_va;
        return source_va;
    }

    fn ensureSharedProcessExitStatusPage(self: *LaunchContext) u64 {
        if (self.shared_process_exit_bootstrap_source_va) |source_va| return source_va;
        const source_va = self.allocWritableBootstrapPage("ManagerInit: alloc process exit bootstrap page failed\n");
        process_exit_bootstrap_abi.initZeroPage(source_va);
        self.shared_process_exit_bootstrap_source_va = source_va;
        return source_va;
    }

    fn grantInputResources(self: *LaunchContext, config_source_va: u64, descriptor: init_bootstrap_abi.DeviceDescriptor, child_process_slot: u64) bool {
        const queue_grant = self.findQueueGrant(descriptor, 0) orelse return false;
        if (!ensureDeviceMmioCapsInstalled(descriptor)) return false;
        if (!grantDeviceMmioPages(descriptor, child_process_slot)) return false;
        const submit_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(queue_grant.submit_token), child_process_slot);
        const notify_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(queue_grant.notify_token), child_process_slot);
        const submit_child = queue_abi.decodeQueueCapToken(submit_child_encoded) orelse return false;
        const notify_child = queue_abi.decodeQueueCapToken(notify_child_encoded) orelse return false;
        const iommu_child_encoded = grantQueueCap(queue_abi.encodeIommuCapToken(queue_grant.iommu_token), child_process_slot);
        const command_child_encoded = grantQueueCap(queue_abi.encodeCommandCapToken(queue_grant.command_token), child_process_slot);
        const iommu_child = queue_abi.decodeIommuCapToken(iommu_child_encoded) orelse return false;
        const command_child = queue_abi.decodeCommandCapToken(command_child_encoded) orelse return false;
        input_bootstrap.writeGrantedCapabilityTokens(config_source_va, iommu_child, submit_child, notify_child, command_child);
        return true;
    }

    fn requireBootDisplay(self: *LaunchContext) void {
        if (self.has_boot_display) return;
        const exec = self.requireShellExec();
        _ = userLog("ManagerInit: spawning shell\n");

        const fb_paddr = self.primary_display.framebuffer_paddr;
        const fb_size = self.primary_display.framebuffer_size_bytes;
        if (fb_paddr == 0 or fb_size == 0) fail("ManagerInit: framebuffer missing\n");
        const shell_fb_height = @min(self.primary_display.height, init_bootstrap_abi.boot_display_shell_height);
        const shell_fb_size = @min(fb_size, self.primary_display.pitch * shell_fb_height * 4);
        const fb_vm_token = self.bootstrap_handoff.framebuffer_vm_token;
        if (image_abi.decodeVmObjectToken(fb_vm_token) == null) fail("ManagerInit: framebuffer vm object missing\n");
        const registry_source_va = self.ensureSharedServiceRegistryPage();
        const stdio_source_va = self.ensureSharedStdioBootstrapPage();
        const args_env_source_va = self.ensureSharedProcessArgsEnvPage();
        const exit_status_source_va = self.ensureSharedProcessExitStatusPage();
        const pointer_shared_source_va = self.pointer_shared_source_va;

        const config_source_va = self.allocWritableBootstrapPage("ManagerInit: alloc shell config page failed\n");
        input_bootstrap.writeKeyboardConfigPage(config_source_va, .{
            .common_page_paddr = self.keyboard_input.common_page_paddr,
            .notify_page_paddr = self.keyboard_input.notify_page_paddr,
            .isr_page_paddr = self.keyboard_input.isr_page_paddr,
            .device_page_paddr = self.keyboard_input.device_page_paddr,
            .common_page_offset = self.keyboard_input.common_page_offset,
            .notify_page_offset = self.keyboard_input.notify_page_offset,
            .isr_page_offset = self.keyboard_input.isr_page_offset,
            .device_page_offset = self.keyboard_input.device_page_offset,
            .notify_off_multiplier = self.keyboard_input.notify_off_multiplier,
            .resource_id = self.keyboard_input.resource_id,
        });
        const config_words: [*]volatile u64 = @ptrFromInt(config_source_va);
        config_words[init_bootstrap_abi.boot_display_config_width_index] = self.primary_display.width;
        config_words[init_bootstrap_abi.boot_display_config_height_index] = shell_fb_height;
        config_words[init_bootstrap_abi.boot_display_config_pitch_index] = self.primary_display.pitch;
        config_words[init_bootstrap_abi.boot_display_config_fb_paddr_index] = fb_paddr;
        config_words[init_bootstrap_abi.boot_display_config_fb_size_bytes_index] = shell_fb_size;
        config_words[init_bootstrap_abi.boot_display_config_fb_vm_token_index] = fb_vm_token;
        config_words[init_bootstrap_abi.boot_display_config_pointer_shared_va_index] =
            if (pointer_shared_source_va != null) input_bootstrap.pointer_shared_target_va else 0;

        boot_display_bootstrap_table_storage = .{};
        boot_display_bootstrap_table_storage.page_count = if (pointer_shared_source_va != null) 6 else 5;
        boot_display_bootstrap_table_storage.cap_count = 1;
        boot_display_bootstrap_table_storage.page_descriptors[0] = .{
            .source_va = config_source_va,
            .target_va = process_abi.standard_config_target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        boot_display_bootstrap_table_storage.page_descriptors[1] = .{
            .source_va = registry_source_va,
            .target_va = process_abi.service_registry_shadow_va,
            .flags = 0,
        };
        boot_display_bootstrap_table_storage.page_descriptors[2] = .{
            .source_va = stdio_source_va,
            .target_va = stdio_bootstrap_abi.target_va,
            .flags = 0,
        };
        boot_display_bootstrap_table_storage.page_descriptors[3] = .{
            .source_va = args_env_source_va,
            .target_va = process_args_env_bootstrap_abi.target_va,
            .flags = 0,
        };
        boot_display_bootstrap_table_storage.page_descriptors[4] = .{
            .source_va = exit_status_source_va,
            .target_va = process_exit_bootstrap_abi.target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        if (pointer_shared_source_va) |source_va| {
            boot_display_bootstrap_table_storage.page_descriptors[5] = .{
                .source_va = source_va,
                .target_va = input_bootstrap.pointer_shared_target_va,
                .flags = 0,
            };
        }
        boot_display_bootstrap_table_storage.cap_descriptors[0] = .{
            .source_token = fb_vm_token,
            .target_token_va = process_abi.standard_config_target_va + init_bootstrap_abi.boot_display_config_fb_vm_token_index * 8,
            .rights_bits = image_abi.vmObjectRightsToBits(.{ .read = true, .write = true, .map = true }),
            .kind = .vm_object,
        };

        const spawned = spawnExecWithExtendedBootstrapTable(exec.token, &boot_display_bootstrap_table_storage);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", "shell", "failed");
            self.logRoleHex("shell", " spawn ret=", spawned);
            fail("ManagerInit: shell spawn failed\n");
        };
        if (!self.grantInputResources(config_source_va, self.keyboard_input, child_slot)) {
            fail("ManagerInit: shell keyboard grant failed\n");
        }
        self.has_boot_display = true;
        _ = userLog("ManagerInit: shell spawned\n");
        noteBootStatus(boot_status_abi.status_init_first_window_spawn_done);
    }

    fn ensurePolicyResources(self: *LaunchContext, policy: StartupPolicy) void {
        if ((policy.ensure_flags & startup_plan_abi.ensure_flag_boot_display) != 0) self.requireBootDisplay();
    }

    fn currentReadyFlags(self: *const LaunchContext) u64 {
        var flags: u64 = 0;
        if (self.block_process_slot != null and self.block_endpoint_id != null) flags |= startup_plan_abi.require_flag_block_service;
        if (self.console_process_slot != null and self.console_endpoint_id != null) flags |= startup_plan_abi.require_flag_console_service;
        if (self.persistent_fs_process_slot != null and self.persistent_fs_endpoint_id != null) flags |= startup_plan_abi.require_flag_persistent_fs_service;
        if (self.pointer_shared_source_va != null) flags |= startup_plan_abi.require_flag_pointer_shared;
        return flags;
    }

    fn hasReadyName(self: *const LaunchContext, name: []const u8) bool {
        if (name.len == 0) return false;
        var i: usize = 0;
        while (i < self.ready_name_count) : (i += 1) {
            if (std.mem.eql(u8, self.ready_names[i], name)) return true;
        }
        return false;
    }

    fn markReadyName(self: *LaunchContext, name: []const u8) void {
        if (name.len == 0 or self.hasReadyName(name)) return;
        if (self.ready_name_count >= self.ready_names.len) fail("ManagerInit: startup ready-name table full\n");
        self.ready_names[self.ready_name_count] = name;
        self.ready_name_count += 1;
    }

    fn markPolicyReadyNames(self: *LaunchContext, policy: StartupPolicy) void {
        self.markReadyName(policy.name);
        var i: usize = 0;
        while (i < policy.provide_count) : (i += 1) self.markReadyName(policy.provide_names[i]);
    }

    fn waitForConfigWord(self: *LaunchContext, base_va: u64, index: usize, expected: u64) void {
        _ = self;
        const words: [*]volatile u64 = @ptrFromInt(base_va);
        var spin_count: usize = 0;
        while (words[index] != expected) {
            if (spin_count < 4096) {
                spin_count += 1;
                asm volatile ("pause");
                continue;
            }
            _ = waitEvent(false, 1);
        }
    }

    fn namedDependenciesReady(self: *const LaunchContext, policy: StartupPolicy) bool {
        var i: usize = 0;
        while (i < policy.after_count) : (i += 1) {
            if (!self.hasReadyName(policy.after_names[i])) return false;
        }
        return true;
    }

    fn policyReady(self: *const LaunchContext, policy: StartupPolicy) bool {
        const ready_flags = self.currentReadyFlags();
        if ((ready_flags & policy.require_flags) != policy.require_flags) return false;
        return self.namedDependenciesReady(policy);
    }

    fn launchInputDriverForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        const input_kind = policy.input_kind orelse fail("ManagerInit: input selector missing\n");
        const input_desc = switch (input_kind) {
            .keyboard => self.keyboard_input,
            .pointer => self.pointer_input orelse fail("ManagerInit: pointer input descriptor missing\n"),
        };
        const exec = self.requireExecForPolicy(policy);
        const endpoint_id = allocDynamicServiceEndpointId();
        const config_source_va = self.allocWritableBootstrapPage("ManagerInit: alloc input driver config page failed\n");
        const stdio_source_va = self.ensureSharedStdioBootstrapPage();
        const args_env_source_va = self.ensureSharedProcessArgsEnvPage();
        const exit_status_source_va = self.ensureSharedProcessExitStatusPage();
        const pointer_shared_source_va = if (input_kind == .pointer) self.ensurePointerSharedPage() else @as(u64, 0);

        switch (input_kind) {
            .keyboard => input_bootstrap.writeKeyboardConfigPage(config_source_va, .{
                .common_page_paddr = input_desc.common_page_paddr,
                .notify_page_paddr = input_desc.notify_page_paddr,
                .isr_page_paddr = input_desc.isr_page_paddr,
                .device_page_paddr = input_desc.device_page_paddr,
                .common_page_offset = input_desc.common_page_offset,
                .notify_page_offset = input_desc.notify_page_offset,
                .isr_page_offset = input_desc.isr_page_offset,
                .device_page_offset = input_desc.device_page_offset,
                .notify_off_multiplier = input_desc.notify_off_multiplier,
                .resource_id = input_desc.resource_id,
            }),
            .pointer => input_bootstrap.writeMouseConfigPage(config_source_va, .{
                .common_page_paddr = input_desc.common_page_paddr,
                .notify_page_paddr = input_desc.notify_page_paddr,
                .isr_page_paddr = input_desc.isr_page_paddr,
                .device_page_paddr = input_desc.device_page_paddr,
                .common_page_offset = input_desc.common_page_offset,
                .notify_page_offset = input_desc.notify_page_offset,
                .isr_page_offset = input_desc.isr_page_offset,
                .device_page_offset = input_desc.device_page_offset,
                .notify_off_multiplier = input_desc.notify_off_multiplier,
                .shared_page_paddr = self.pointer_shared_paddr,
                .screen_width = self.primary_display.width,
                .screen_height = self.primary_display.height,
                .screen_pitch = self.primary_display.pitch,
                .resource_id = input_desc.resource_id,
            }),
        }

        generic_service_bootstrap_table_storage = .{};
        generic_service_bootstrap_table_storage.page_count = 4;
        generic_service_bootstrap_table_storage.page_descriptors[0] = .{
            .source_va = config_source_va,
            .target_va = process_abi.standard_config_target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        generic_service_bootstrap_table_storage.page_descriptors[1] = .{
            .source_va = stdio_source_va,
            .target_va = stdio_bootstrap_abi.target_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[2] = .{
            .source_va = args_env_source_va,
            .target_va = process_args_env_bootstrap_abi.target_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[3] = .{
            .source_va = exit_status_source_va,
            .target_va = process_exit_bootstrap_abi.target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        _ = pointer_shared_source_va;

        const spawned = spawnExecWithExtendedBootstrapTable(exec.token, &generic_service_bootstrap_table_storage);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", policy.label, "failed");
            self.logRoleHex(policy.label, " spawn ret=", spawned);
            fail("ManagerInit: input driver spawn failed\n");
        };
        self.logRoleLine("spawn", policy.label, "ok");
        if (installEndpoint(endpoint_id, child_slot) != 0) fail("ManagerInit: input endpoint install failed\n");
        if (publishServiceEndpoint(endpoint_id, child_slot) != 0) fail("ManagerInit: input endpoint publish failed\n");
        if (!self.grantInputResources(config_source_va, input_desc, child_slot)) fail("ManagerInit: input grant failed\n");
        if (input_kind == .pointer) {
            service_registry_abi.setServiceWithProcessSlot(self.window_service_page.source_va, .pointer, child_slot, endpoint_id);
            if (self.shared_service_registry_source_va) |source_va| service_registry_abi.setServiceWithProcessSlot(source_va, .pointer, child_slot, endpoint_id);
        }
    }

    fn launchBlockDriverForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        const exec = self.requireExecForPolicy(policy);
        const block_desc = self.block_device;
        const queue_grant = self.findQueueGrant(block_desc, 0) orelse fail("ManagerInit: block queue grant missing\n");
        if (self.block_iommu_base_token == 0) self.block_iommu_base_token = queue_grant.iommu_token;
        if (self.block_submit_base_token == 0) self.block_submit_base_token = queue_grant.submit_token;
        if (self.block_notify_base_token == 0) self.block_notify_base_token = queue_grant.notify_token;
        if (self.block_command_base_token == 0) self.block_command_base_token = queue_grant.command_token;
        if (!self.refreshBlockProfileSources(self.block_profile)) fail("ManagerInit: block profile source prepare failed\n");
        const block_geometry = readBlockGeometry(block_desc) orelse fail("ManagerInit: block geometry failed\n");
        const endpoint_id = allocDynamicServiceEndpointId();
        const config_source_va = self.allocWritableBootstrapPage("ManagerInit: alloc block driver config page failed\n");
        const stdio_source_va = self.ensureSharedStdioBootstrapPage();
        const args_env_source_va = self.ensureSharedProcessArgsEnvPage();
        const exit_status_source_va = self.ensureSharedProcessExitStatusPage();
        block_bootstrap.writeConfigPage(config_source_va, .{
            .endpoint_id = endpoint_id,
            .common_page_paddr = block_desc.common_page_paddr,
            .notify_page_paddr = block_desc.notify_page_paddr,
            .isr_page_paddr = block_desc.isr_page_paddr,
            .device_page_paddr = block_desc.device_page_paddr,
            .common_page_offset = block_desc.common_page_offset,
            .notify_page_offset = block_desc.notify_page_offset,
            .isr_page_offset = block_desc.isr_page_offset,
            .device_page_offset = block_desc.device_page_offset,
            .notify_off_multiplier = block_desc.notify_off_multiplier,
            .capacity_sectors = block_geometry.capacity_sectors,
            .logical_block_size = block_geometry.logical_block_size,
            .resource_id = block_desc.resource_id,
        });
        block_bootstrap_pages_storage[0] = .{
            .source_va = config_source_va,
            .target_va = process_abi.standard_config_target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        block_bootstrap_pages_storage[1] = .{
            .source_va = stdio_source_va,
            .target_va = stdio_bootstrap_abi.target_va,
            .flags = 0,
        };
        block_bootstrap_pages_storage[2] = .{
            .source_va = args_env_source_va,
            .target_va = process_args_env_bootstrap_abi.target_va,
            .flags = 0,
        };
        block_bootstrap_pages_storage[3] = .{
            .source_va = exit_status_source_va,
            .target_va = process_exit_bootstrap_abi.target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        block_bootstrap_table_storage = .{};
        block_bootstrap_table_storage.page_count = 4;
        block_bootstrap_table_storage.page_descriptors[0] = block_bootstrap_pages_storage[0];
        block_bootstrap_table_storage.page_descriptors[1] = block_bootstrap_pages_storage[1];
        block_bootstrap_table_storage.page_descriptors[2] = block_bootstrap_pages_storage[2];
        block_bootstrap_table_storage.page_descriptors[3] = block_bootstrap_pages_storage[3];

        const spawned = spawnExecWithExtendedBootstrapTable(exec.token, &block_bootstrap_table_storage);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", policy.label, "failed");
            self.logRoleHex(policy.label, " spawn ret=", spawned);
            fail("ManagerInit: block driver spawn failed\n");
        };
        self.logRoleLine("spawn", policy.label, "ok");
        if (installEndpoint(endpoint_id, child_slot) != 0) fail("ManagerInit: block endpoint install failed\n");
        if (publishServiceEndpoint(endpoint_id, child_slot) != 0) fail("ManagerInit: block endpoint publish failed\n");
        if (!ensureDeviceMmioCapsInstalled(block_desc)) fail("ManagerInit: block MMIO install failed\n");
        if (!grantDeviceMmioPages(block_desc, child_slot)) fail("ManagerInit: block MMIO grant failed\n");
        const iommu_child_encoded = if (self.block_iommu_source_token != 0)
            grantQueueCap(queue_abi.encodeIommuCapToken(self.block_iommu_source_token), child_slot)
        else
            @as(u64, 0);
        const submit_child_encoded = if (self.block_submit_source_token != 0)
            grantQueueCap(queue_abi.encodeQueueCapToken(self.block_submit_source_token), child_slot)
        else
            @as(u64, 0);
        const notify_child_encoded = if (self.block_notify_source_token != 0)
            grantQueueCap(queue_abi.encodeQueueCapToken(self.block_notify_source_token), child_slot)
        else
            @as(u64, 0);
        const command_child_encoded = grantQueueCap(queue_abi.encodeCommandCapToken(self.block_command_source_token), child_slot);
        const iommu_child = if (self.block_iommu_source_token != 0)
            (queue_abi.decodeIommuCapToken(iommu_child_encoded) orelse fail("ManagerInit: block iommu grant failed\n"))
        else
            @as(u64, 0);
        const submit_child = if (self.block_submit_source_token != 0)
            (queue_abi.decodeQueueCapToken(submit_child_encoded) orelse fail("ManagerInit: block submit grant failed\n"))
        else
            @as(u64, 0);
        const notify_child = if (self.block_notify_source_token != 0)
            (queue_abi.decodeQueueCapToken(notify_child_encoded) orelse fail("ManagerInit: block notify grant failed\n"))
        else
            @as(u64, 0);
        const command_child = queue_abi.decodeCommandCapToken(command_child_encoded) orelse fail("ManagerInit: block command grant failed\n");
        block_bootstrap.writeGrantedCapabilityTokens(config_source_va, iommu_child, submit_child, notify_child, command_child);
        _ = signalEndpoint(endpoint_id);
        self.waitForConfigWord(config_source_va, block_bootstrap.driver_status_index, block_bootstrap.driver_status_ready);
        service_registry_abi.setServiceWithProcessSlot(self.window_service_page.source_va, .block, child_slot, endpoint_id);
        if (self.shared_service_registry_source_va) |source_va| service_registry_abi.setServiceWithProcessSlot(source_va, .block, child_slot, endpoint_id);
        self.block_process_slot = child_slot;
        self.block_endpoint_id = endpoint_id;
        self.block_policy = policy;
    }

    fn launchConsoleDriverForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        _ = policy.device_kind orelse fail("ManagerInit: console device selector missing\n");
        const console_desc = self.console_device orelse fail("ManagerInit: console device descriptor missing\n");
        const exec = self.requireExecForPolicy(policy);
        const rx_queue_grant = self.findQueueGrant(console_desc, console_bootstrap.rx_queue_index) orelse fail("ManagerInit: console rx queue grant missing\n");
        const tx_queue_grant = self.findQueueGrant(console_desc, console_bootstrap.tx_queue_index) orelse fail("ManagerInit: console tx queue grant missing\n");
        const endpoint_id = allocDynamicServiceEndpointId();
        const config_source_va = self.allocWritableBootstrapPage("ManagerInit: alloc console driver config page failed\n");
        const stdio_source_va = self.ensureSharedStdioBootstrapPage();
        const args_env_source_va = self.ensureSharedProcessArgsEnvPage();
        const exit_status_source_va = self.ensureSharedProcessExitStatusPage();
        console_bootstrap.writeConfigPage(config_source_va, .{
            .endpoint_id = endpoint_id,
            .common_page_paddr = console_desc.common_page_paddr,
            .notify_page_paddr = console_desc.notify_page_paddr,
            .isr_page_paddr = console_desc.isr_page_paddr,
            .device_page_paddr = console_desc.device_page_paddr,
            .common_page_offset = console_desc.common_page_offset,
            .notify_page_offset = console_desc.notify_page_offset,
            .isr_page_offset = console_desc.isr_page_offset,
            .device_page_offset = console_desc.device_page_offset,
            .notify_off_multiplier = console_desc.notify_off_multiplier,
            .resource_id = console_desc.resource_id,
        });
        console_bootstrap_pages_storage[0] = .{ .source_va = config_source_va, .target_va = process_abi.standard_config_target_va, .flags = process_abi.spawn_flag_bootstrap_page_writable };
        console_bootstrap_pages_storage[1] = .{ .source_va = stdio_source_va, .target_va = stdio_bootstrap_abi.target_va, .flags = 0 };
        console_bootstrap_pages_storage[2] = .{ .source_va = args_env_source_va, .target_va = process_args_env_bootstrap_abi.target_va, .flags = 0 };
        console_bootstrap_pages_storage[3] = .{ .source_va = exit_status_source_va, .target_va = process_exit_bootstrap_abi.target_va, .flags = process_abi.spawn_flag_bootstrap_page_writable };
        console_bootstrap_table_storage = .{};
        console_bootstrap_table_storage.page_count = 4;
        console_bootstrap_table_storage.page_descriptors[0] = console_bootstrap_pages_storage[0];
        console_bootstrap_table_storage.page_descriptors[1] = console_bootstrap_pages_storage[1];
        console_bootstrap_table_storage.page_descriptors[2] = console_bootstrap_pages_storage[2];
        console_bootstrap_table_storage.page_descriptors[3] = console_bootstrap_pages_storage[3];

        const spawned = spawnExecWithExtendedBootstrapTable(exec.token, &console_bootstrap_table_storage);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", policy.label, "failed");
            self.logRoleHex(policy.label, " spawn ret=", spawned);
            fail("ManagerInit: console driver spawn failed\n");
        };
        self.logRoleLine("spawn", policy.label, "ok");
        if (installEndpoint(endpoint_id, child_slot) != 0) fail("ManagerInit: console endpoint install failed\n");
        if (publishServiceEndpoint(endpoint_id, child_slot) != 0) fail("ManagerInit: console endpoint publish failed\n");
        if (!ensureDeviceMmioCapsInstalled(console_desc)) fail("ManagerInit: console MMIO install failed\n");
        if (!grantDeviceMmioPages(console_desc, child_slot)) fail("ManagerInit: console MMIO grant failed\n");
        const iommu_child_encoded = grantQueueCap(queue_abi.encodeIommuCapToken(rx_queue_grant.iommu_token), child_slot);
        const rx_submit_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(rx_queue_grant.submit_token), child_slot);
        const rx_notify_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(rx_queue_grant.notify_token), child_slot);
        const tx_submit_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(tx_queue_grant.submit_token), child_slot);
        const tx_notify_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(tx_queue_grant.notify_token), child_slot);
        const command_child_encoded = grantQueueCap(queue_abi.encodeCommandCapToken(rx_queue_grant.command_token), child_slot);
        const iommu_child = queue_abi.decodeIommuCapToken(iommu_child_encoded) orelse fail("ManagerInit: console iommu grant failed\n");
        const rx_submit_child = queue_abi.decodeQueueCapToken(rx_submit_child_encoded) orelse fail("ManagerInit: console rx submit grant failed\n");
        const rx_notify_child = queue_abi.decodeQueueCapToken(rx_notify_child_encoded) orelse fail("ManagerInit: console rx notify grant failed\n");
        const tx_submit_child = queue_abi.decodeQueueCapToken(tx_submit_child_encoded) orelse fail("ManagerInit: console tx submit grant failed\n");
        const tx_notify_child = queue_abi.decodeQueueCapToken(tx_notify_child_encoded) orelse fail("ManagerInit: console tx notify grant failed\n");
        const command_child = queue_abi.decodeCommandCapToken(command_child_encoded) orelse fail("ManagerInit: console command grant failed\n");
        console_bootstrap.writeGrantedCapabilityTokens(config_source_va, iommu_child, rx_submit_child, rx_notify_child, tx_submit_child, tx_notify_child, command_child);
        _ = signalEndpoint(endpoint_id);
        self.waitForConfigWord(config_source_va, console_bootstrap.driver_status_index, console_bootstrap.driver_status_ready);
        service_registry_abi.setServiceWithProcessSlot(self.window_service_page.source_va, .console, child_slot, endpoint_id);
        if (self.shared_service_registry_source_va) |source_va| service_registry_abi.setServiceWithProcessSlot(source_va, .console, child_slot, endpoint_id);
        self.console_process_slot = child_slot;
        self.console_endpoint_id = endpoint_id;
        self.console_policy = policy;
    }

    fn launchGpuDriverForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        const gpu_desc = self.gpu_device orelse fail("ManagerInit: gpu device descriptor missing\n");
        const exec = self.requireExecForPolicy(policy);
        const queue_grant = self.findQueueGrant(gpu_desc, 0) orelse fail("ManagerInit: gpu queue grant missing\n");
        const cursor_queue_grant = self.findQueueGrant(gpu_desc, gpu_bootstrap.cursor_queue_index);
        const endpoint_id = gpu_protocol.endpoint_id;
        const config_source_va = self.allocWritableBootstrapPage("ManagerInit: alloc gpu driver config page failed\n");
        const stdio_source_va = self.ensureSharedStdioBootstrapPage();
        const args_env_source_va = self.ensureSharedProcessArgsEnvPage();
        const exit_status_source_va = self.ensureSharedProcessExitStatusPage();
        gpu_bootstrap.writeConfigPage(config_source_va, .{
            .endpoint_id = endpoint_id,
            .common_page_paddr = gpu_desc.common_page_paddr,
            .notify_page_paddr = gpu_desc.notify_page_paddr,
            .isr_page_paddr = gpu_desc.isr_page_paddr,
            .device_page_paddr = gpu_desc.device_page_paddr,
            .common_page_offset = gpu_desc.common_page_offset,
            .notify_page_offset = gpu_desc.notify_page_offset,
            .isr_page_offset = gpu_desc.isr_page_offset,
            .device_page_offset = gpu_desc.device_page_offset,
            .notify_off_multiplier = gpu_desc.notify_off_multiplier,
            .resource_id = gpu_desc.resource_id,
        });
        gpu_bootstrap_pages_storage[0] = .{
            .source_va = config_source_va,
            .target_va = process_abi.standard_config_target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        gpu_bootstrap_pages_storage[1] = .{
            .source_va = stdio_source_va,
            .target_va = stdio_bootstrap_abi.target_va,
            .flags = 0,
        };
        gpu_bootstrap_pages_storage[2] = .{
            .source_va = args_env_source_va,
            .target_va = process_args_env_bootstrap_abi.target_va,
            .flags = 0,
        };
        gpu_bootstrap_pages_storage[3] = .{
            .source_va = exit_status_source_va,
            .target_va = process_exit_bootstrap_abi.target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        gpu_bootstrap_table_storage = .{};
        gpu_bootstrap_table_storage.page_count = 4;
        gpu_bootstrap_table_storage.page_descriptors[0] = gpu_bootstrap_pages_storage[0];
        gpu_bootstrap_table_storage.page_descriptors[1] = gpu_bootstrap_pages_storage[1];
        gpu_bootstrap_table_storage.page_descriptors[2] = gpu_bootstrap_pages_storage[2];
        gpu_bootstrap_table_storage.page_descriptors[3] = gpu_bootstrap_pages_storage[3];

        const spawned = spawnExecWithExtendedBootstrapTable(exec.token, &gpu_bootstrap_table_storage);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", policy.label, "failed");
            self.logRoleHex(policy.label, " spawn ret=", spawned);
            fail("ManagerInit: gpu driver spawn failed\n");
        };
        self.logRoleLine("spawn", policy.label, "ok");
        if (installEndpoint(endpoint_id, child_slot) != 0) fail("ManagerInit: gpu endpoint install failed\n");
        if (publishServiceEndpoint(endpoint_id, child_slot) != 0) fail("ManagerInit: gpu endpoint publish failed\n");
        if (!ensureDeviceMmioCapsInstalled(gpu_desc)) fail("ManagerInit: gpu MMIO install failed\n");
        if (!grantDeviceMmioPages(gpu_desc, child_slot)) fail("ManagerInit: gpu MMIO grant failed\n");
        const iommu_child_encoded = grantQueueCap(queue_abi.encodeIommuCapToken(queue_grant.iommu_token), child_slot);
        const submit_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(queue_grant.submit_token), child_slot);
        const notify_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(queue_grant.notify_token), child_slot);
        const cursor_submit_child_encoded = if (cursor_queue_grant) |grant|
            grantQueueCap(queue_abi.encodeQueueCapToken(grant.submit_token), child_slot)
        else
            @as(u64, 0);
        const cursor_notify_child_encoded = if (cursor_queue_grant) |grant|
            grantQueueCap(queue_abi.encodeQueueCapToken(grant.notify_token), child_slot)
        else
            @as(u64, 0);
        const command_child_encoded = grantQueueCap(queue_abi.encodeCommandCapToken(queue_grant.command_token), child_slot);
        const iommu_child = queue_abi.decodeIommuCapToken(iommu_child_encoded) orelse fail("ManagerInit: gpu iommu grant failed\n");
        const submit_child = queue_abi.decodeQueueCapToken(submit_child_encoded) orelse fail("ManagerInit: gpu submit grant failed\n");
        const notify_child = queue_abi.decodeQueueCapToken(notify_child_encoded) orelse fail("ManagerInit: gpu notify grant failed\n");
        const cursor_submit_child = if (cursor_queue_grant != null)
            (queue_abi.decodeQueueCapToken(cursor_submit_child_encoded) orelse fail("ManagerInit: gpu cursor queue submit grant failed\n"))
        else
            @as(u64, 0);
        const cursor_notify_child = if (cursor_queue_grant != null)
            (queue_abi.decodeQueueCapToken(cursor_notify_child_encoded) orelse fail("ManagerInit: gpu cursor queue notify grant failed\n"))
        else
            @as(u64, 0);
        const command_child = queue_abi.decodeCommandCapToken(command_child_encoded) orelse fail("ManagerInit: gpu command grant failed\n");
        gpu_bootstrap.writeGrantedCapabilityTokens(config_source_va, iommu_child, submit_child, notify_child, cursor_submit_child, cursor_notify_child, command_child);
        _ = signalEndpoint(endpoint_id);
        service_registry_abi.setServiceWithProcessSlot(self.window_service_page.source_va, .gpu, child_slot, endpoint_id);
        if (self.shared_service_registry_source_va) |source_va| service_registry_abi.setServiceWithProcessSlot(source_va, .gpu, child_slot, endpoint_id);
        self.gpu_process_slot = child_slot;
        self.gpu_endpoint_id = endpoint_id;
        self.gpu_policy = policy;
    }

    fn launchWindowServiceForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        const exec = self.requireExecForPolicy(policy);
        const endpoint_id = allocDynamicServiceEndpointId();
        const registry_source_va = self.ensureSharedServiceRegistryPage();
        const stdio_source_va = self.ensureSharedStdioBootstrapPage();
        const args_env_source_va = self.ensureSharedProcessArgsEnvPage();
        const exit_status_source_va = self.ensureSharedProcessExitStatusPage();
        const pointer_shared_source_va = self.pointer_shared_source_va;

        generic_service_bootstrap_table_storage = .{};
        generic_service_bootstrap_table_storage.page_count = if (pointer_shared_source_va != null) 5 else 4;
        generic_service_bootstrap_table_storage.page_descriptors[0] = .{
            .source_va = registry_source_va,
            .target_va = process_abi.service_registry_shadow_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[1] = .{
            .source_va = stdio_source_va,
            .target_va = stdio_bootstrap_abi.target_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[2] = .{
            .source_va = args_env_source_va,
            .target_va = process_args_env_bootstrap_abi.target_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[3] = .{
            .source_va = exit_status_source_va,
            .target_va = process_exit_bootstrap_abi.target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        if (pointer_shared_source_va) |source_va| {
            generic_service_bootstrap_table_storage.page_descriptors[4] = .{
                .source_va = source_va,
                .target_va = input_bootstrap.pointer_shared_target_va,
                .flags = 0,
            };
        }

        const spawned = spawnExecWithExtendedBootstrapTable(exec.token, &generic_service_bootstrap_table_storage);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", policy.label, "failed");
            self.logRoleHex(policy.label, " spawn ret=", spawned);
            fail("ManagerInit: window service spawn failed\n");
        };
        self.logRoleLine("spawn", policy.label, "ok");
        if (publishServiceEndpoint(endpoint_id, child_slot) != 0) fail("ManagerInit: window service endpoint publish failed\n");

        self.window_service = .{
            .kind = @intFromEnum(service_registry_abi.ServiceKind.window),
            .process_slot = child_slot,
            .endpoint_id = endpoint_id,
            .flags = service_registry_abi.service_flag_process_slot_compat,
        };
        service_registry_abi.setServiceWithProcessSlot(self.window_service_page.source_va, .window, child_slot, endpoint_id);
        if (self.shared_service_registry_source_va) |source_va| {
            service_registry_abi.setServiceWithProcessSlot(source_va, .window, child_slot, endpoint_id);
        }
        _ = userLog("ManagerInit: window service ready\n");
    }

    fn launchPersistentFsForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        if (self.block_process_slot == null or self.block_endpoint_id == null) fail("ManagerInit: block service missing\n");
        const exec = self.requireExecForPolicy(policy);
        const endpoint_id = allocDynamicServiceEndpointId();
        const registry_source_va = allocChildServiceRegistryPage(
            self.window_service,
            getProcessSlot(),
            self.capctl_endpoint_id,
            self.block_process_slot,
            self.block_endpoint_id,
            self.gpu_process_slot,
            self.gpu_endpoint_id,
            self.console_process_slot,
            self.console_endpoint_id,
            self.persistent_fs_process_slot,
            self.persistent_fs_endpoint_id,
        ) orelse fail("ManagerInit: alloc persistent fs registry failed\n");
        const config_source_va = self.allocWritableBootstrapPage("ManagerInit: alloc persistent fs config page failed\n");
        const stdio_source_va = self.ensureSharedStdioBootstrapPage();
        const args_env_source_va = self.ensureSharedProcessArgsEnvPage();
        const exit_status_source_va = self.ensureSharedProcessExitStatusPage();
        persistent_fs_bootstrap.writeConfigPage(config_source_va, endpoint_id, persistent_fs_start_block);

        persistent_fs_bootstrap_pages_storage[0] = .{
            .source_va = config_source_va,
            .target_va = process_abi.standard_config_target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        persistent_fs_bootstrap_pages_storage[1] = .{
            .source_va = registry_source_va,
            .target_va = self.window_service_page.target_va,
            .flags = self.window_service_page.spawn_flags,
        };
        persistent_fs_bootstrap_pages_storage[2] = .{
            .source_va = stdio_source_va,
            .target_va = stdio_bootstrap_abi.target_va,
            .flags = 0,
        };
        persistent_fs_bootstrap_pages_storage[3] = .{
            .source_va = args_env_source_va,
            .target_va = process_args_env_bootstrap_abi.target_va,
            .flags = 0,
        };
        persistent_fs_bootstrap_pages_storage[4] = .{
            .source_va = exit_status_source_va,
            .target_va = process_exit_bootstrap_abi.target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        persistent_fs_bootstrap_table_storage = .{};
        persistent_fs_bootstrap_table_storage.page_count = 5;
        persistent_fs_bootstrap_table_storage.page_descriptors[0] = persistent_fs_bootstrap_pages_storage[0];
        persistent_fs_bootstrap_table_storage.page_descriptors[1] = persistent_fs_bootstrap_pages_storage[1];
        persistent_fs_bootstrap_table_storage.page_descriptors[2] = persistent_fs_bootstrap_pages_storage[2];
        persistent_fs_bootstrap_table_storage.page_descriptors[3] = persistent_fs_bootstrap_pages_storage[3];
        persistent_fs_bootstrap_table_storage.page_descriptors[4] = persistent_fs_bootstrap_pages_storage[4];

        const spawned = spawnExecWithExtendedBootstrapTable(exec.token, &persistent_fs_bootstrap_table_storage);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", policy.label, "failed");
            self.logRoleHex(policy.label, " spawn ret=", spawned);
            fail("ManagerInit: persistent fs spawn failed\n");
        };
        self.logRoleLine("spawn", policy.label, "ok");
        if (installEndpoint(endpoint_id, child_slot) != 0) fail("ManagerInit: persistent fs endpoint install failed\n");
        if (publishServiceEndpoint(endpoint_id, child_slot) != 0) fail("ManagerInit: persistent fs endpoint publish failed\n");
        service_registry_abi.setServiceWithProcessSlot(self.window_service_page.source_va, .persistent_fs, child_slot, endpoint_id);
        if (self.shared_service_registry_source_va) |source_va| service_registry_abi.setServiceWithProcessSlot(source_va, .persistent_fs, child_slot, endpoint_id);
        self.waitForConfigWord(config_source_va, persistent_fs_bootstrap.server_status_index, persistent_fs_bootstrap.server_status_ready);
        self.persistent_fs_process_slot = child_slot;
        self.persistent_fs_endpoint_id = endpoint_id;
        self.persistent_fs_policy = policy;
    }

    fn launchProcessForPolicy(self: *LaunchContext, policy: StartupPolicy, grant_process_builder: bool) void {
        const exec = self.requireExecForPolicy(policy);
        const registry_source_va = self.ensureSharedServiceRegistryPage();
        const stdio_source_va = self.ensureSharedStdioBootstrapPage();
        const args_env_source_va = self.ensureSharedProcessArgsEnvPage();
        const exit_status_source_va = self.ensureSharedProcessExitStatusPage();

        generic_service_bootstrap_table_storage = .{};
        generic_service_bootstrap_table_storage.page_count = 4;
        generic_service_bootstrap_table_storage.page_descriptors[0] = .{
            .source_va = registry_source_va,
            .target_va = process_abi.service_registry_shadow_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[1] = .{
            .source_va = stdio_source_va,
            .target_va = stdio_bootstrap_abi.target_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[2] = .{
            .source_va = args_env_source_va,
            .target_va = process_args_env_bootstrap_abi.target_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[3] = .{
            .source_va = exit_status_source_va,
            .target_va = process_exit_bootstrap_abi.target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };

        const spawn_flags = process_abi.spawn_flag_bootstrap_extended_descriptor_table |
            (if (grant_process_builder) process_abi.spawn_flag_child_bootstrap_owner else @as(u64, 0));
        const spawned = spawnExecWithExtendedBootstrapTableFlags(exec.token, &generic_service_bootstrap_table_storage, spawn_flags);
        if (process_abi.decodeSpawnedProcessSlot(spawned) == null) {
            self.logRoleLine("spawn", policy.label, "failed");
            self.logRoleHex(policy.label, " spawn ret=", spawned);
            fail("ManagerInit: process spawn failed\n");
        }
        self.logRoleLine("spawn", policy.label, "ok");
    }

    fn launchVfsForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        const exec = self.requireExecForPolicy(policy);
        const endpoint_id = allocDynamicServiceEndpointId();
        const config_source_va = self.allocWritableBootstrapPage("ManagerInit: alloc vfs config page failed\n");
        const config_words: [*]volatile u64 = @ptrFromInt(config_source_va);
        config_words[0] = endpoint_id;
        config_words[1] = 0x3153_4656; // "VFS1"
        config_words[2] = 0;
        config_words[3] = self.fat_fs_endpoint_id orelse 0;
        config_words[4] = self.fat_fs_process_slot orelse 0;

        const registry_source_va = self.ensureSharedServiceRegistryPage();
        const stdio_source_va = self.ensureSharedStdioBootstrapPage();
        const args_env_source_va = self.ensureSharedProcessArgsEnvPage();
        const exit_status_source_va = self.ensureSharedProcessExitStatusPage();

        generic_service_bootstrap_table_storage = .{};
        generic_service_bootstrap_table_storage.page_count = 5;
        generic_service_bootstrap_table_storage.page_descriptors[0] = .{
            .source_va = config_source_va,
            .target_va = process_abi.standard_config_target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        generic_service_bootstrap_table_storage.page_descriptors[1] = .{
            .source_va = registry_source_va,
            .target_va = process_abi.service_registry_shadow_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[2] = .{
            .source_va = stdio_source_va,
            .target_va = stdio_bootstrap_abi.target_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[3] = .{
            .source_va = args_env_source_va,
            .target_va = process_args_env_bootstrap_abi.target_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[4] = .{
            .source_va = exit_status_source_va,
            .target_va = process_exit_bootstrap_abi.target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };

        const spawned = spawnExecWithExtendedBootstrapTable(exec.token, &generic_service_bootstrap_table_storage);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", policy.label, "failed");
            self.logRoleHex(policy.label, " spawn ret=", spawned);
            fail("ManagerInit: vfs spawn failed\n");
        };
        self.logRoleLine("spawn", policy.label, "ok");
        if (installEndpoint(endpoint_id, child_slot) != 0) fail("ManagerInit: vfs endpoint install failed\n");
        if (publishServiceEndpoint(endpoint_id, child_slot) != 0) fail("ManagerInit: vfs endpoint publish failed\n");
        service_registry_abi.setServiceWithProcessSlot(self.window_service_page.source_va, .vfs, child_slot, endpoint_id);
        if (self.shared_service_registry_source_va) |source_va| service_registry_abi.setServiceWithProcessSlot(source_va, .vfs, child_slot, endpoint_id);
        self.waitForConfigWord(config_source_va, 2, 1);
    }

    fn launchFatServerForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        const exec = self.requireExecForPolicy(policy);
        const endpoint_id = allocDynamicServiceEndpointId();
        const config_source_va = self.allocWritableBootstrapPage("ManagerInit: alloc fat server config page failed\n");
        const config_words: [*]volatile u64 = @ptrFromInt(config_source_va);
        config_words[0] = endpoint_id;
        config_words[1] = 0x3154_4146; // "FAT1"
        config_words[2] = 0;
        config_words[3] = persistent_fs_start_block;

        const registry_source_va = self.ensureSharedServiceRegistryPage();
        const stdio_source_va = self.ensureSharedStdioBootstrapPage();
        const args_env_source_va = self.ensureSharedProcessArgsEnvPage();
        const exit_status_source_va = self.ensureSharedProcessExitStatusPage();

        generic_service_bootstrap_table_storage = .{};
        generic_service_bootstrap_table_storage.page_count = 5;
        generic_service_bootstrap_table_storage.page_descriptors[0] = .{
            .source_va = config_source_va,
            .target_va = process_abi.standard_config_target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        generic_service_bootstrap_table_storage.page_descriptors[1] = .{
            .source_va = registry_source_va,
            .target_va = process_abi.service_registry_shadow_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[2] = .{
            .source_va = stdio_source_va,
            .target_va = stdio_bootstrap_abi.target_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[3] = .{
            .source_va = args_env_source_va,
            .target_va = process_args_env_bootstrap_abi.target_va,
            .flags = 0,
        };
        generic_service_bootstrap_table_storage.page_descriptors[4] = .{
            .source_va = exit_status_source_va,
            .target_va = process_exit_bootstrap_abi.target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };

        const spawned = spawnExecWithExtendedBootstrapTable(exec.token, &generic_service_bootstrap_table_storage);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", policy.label, "failed");
            self.logRoleHex(policy.label, " spawn ret=", spawned);
            fail("ManagerInit: fat server spawn failed\n");
        };
        self.logRoleLine("spawn", policy.label, "ok");
        if (installEndpoint(endpoint_id, child_slot) != 0) fail("ManagerInit: fat server endpoint install failed\n");
        if (publishServiceEndpoint(endpoint_id, child_slot) != 0) fail("ManagerInit: fat server endpoint publish failed\n");
        service_registry_abi.setServiceWithProcessSlot(self.window_service_page.source_va, .fat_fs, child_slot, endpoint_id);
        if (self.shared_service_registry_source_va) |source_va| service_registry_abi.setServiceWithProcessSlot(source_va, .fat_fs, child_slot, endpoint_id);
        self.waitForConfigWord(config_source_va, 2, 1);
        self.fat_fs_process_slot = child_slot;
        self.fat_fs_endpoint_id = endpoint_id;
        self.fat_fs_policy = policy;
    }

    fn launchPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        self.ensurePolicyResources(policy);
        switch (policy.action) {
            .vfs => self.launchVfsForPolicy(policy),
            .input_driver => self.launchInputDriverForPolicy(policy),
            .block_driver => self.launchBlockDriverForPolicy(policy),
            .console_driver => self.launchConsoleDriverForPolicy(policy),
            .gpu_driver => self.launchGpuDriverForPolicy(policy),
            .window_service => self.launchWindowServiceForPolicy(policy),
            .persistent_fs_server => self.launchPersistentFsForPolicy(policy),
            .fat_server => self.launchFatServerForPolicy(policy),
            .process => self.launchProcessForPolicy(policy, false),
            .process_builder => self.launchProcessForPolicy(policy, true),
            else => startupManifestFail("ManagerInit: unsupported policy action\n"),
        }
    }
};

var launch_ctx_storage: LaunchContext = undefined;

fn runStartupManifest(ctx: *LaunchContext, manifest: *const StartupManifest) void {
    noteBootStatus(boot_status_abi.status_init_manifest_begin);
    var nodes: [startup_plan_abi.max_startup_program_descriptors]StartupNode = undefined;
    var node_count: usize = 0;
    var lines = std.mem.tokenizeScalar(u8, manifest.slice(), '\n');
    while (lines.next()) |raw_line| {
        const policy = parseStartupManifestLine(raw_line) orelse continue;
        if (node_count >= nodes.len) fail("ManagerInit: too many startup manifest entries\n");
        nodes[node_count] = .{ .policy = policy };
        node_count += 1;
    }

    var preload_index: usize = 0;
    while (preload_index < node_count) : (preload_index += 1) ctx.cacheExecForPolicy(nodes[preload_index].policy);

    var dispatched_count: usize = 0;
    while (dispatched_count < node_count) {
        var progress = false;
        var node_index: usize = 0;
        while (node_index < node_count) : (node_index += 1) {
            const node = &nodes[node_index];
            if (node.launched) continue;
            if (!ctx.policyReady(node.policy)) continue;
            ctx.launchPolicy(node.policy);
            ctx.markPolicyReadyNames(node.policy);
            node.launched = true;
            dispatched_count += 1;
            progress = true;
        }
        if (dispatched_count >= node_count) break;
        if (!progress) fail("ManagerInit: startup policy stalled\n");
    }
    _ = userLog("ManagerInit: startup manifest done\n");
}

fn managerMain() noreturn {
    noteBootStatus(boot_status_abi.status_init_started);

    const bootstrap_handoff = waitForBootstrapHandoff();
    const primary_display = requirePrimaryDisplayDescriptor();
    const keyboard_input = requireInputDeviceDescriptor(bootstrap_handoff, .keyboard, "ManagerInit: keyboard input descriptor missing\n");
    const pointer_input = findInputDeviceDescriptor(bootstrap_handoff, .pointer);
    const block_device = requireBlockDeviceDescriptor(bootstrap_handoff, .virtio_blk, "ManagerInit: block device descriptor missing\n");
    const console_device = findConsoleDeviceDescriptor(bootstrap_handoff);
    const gpu_device = findGpuDeviceDescriptor(bootstrap_handoff);
    const window_service_page = requireSpawnPageDescriptor(.service_config, .window_service, "ManagerInit: window service descriptor missing\n");

    const window_service = service_registry_abi.findService(window_service_page.source_va, .window);

    launch_ctx_storage = .{
        .bootstrap_handoff = bootstrap_handoff,
        .primary_display = primary_display,
        .keyboard_input = keyboard_input,
        .pointer_input = pointer_input,
        .block_device = block_device,
        .console_device = console_device,
        .gpu_device = gpu_device,
        .window_service_page = window_service_page,
        .window_service = window_service,
        .has_boot_display = window_service != null,
    };
    launch_ctx_storage.ensureBootFsArchive();
    var startup_manifest_from_rootfs = false;
    const startup_manifest = blk: {
        if (loadStartupManifestFromBootFs(rootfs_startup_manifest_path)) |manifest| break :blk manifest;
        launch_ctx_storage.ensureRootFsReader();
        if (loadStartupManifestFromRootFs(rootfs_startup_manifest_path)) |manifest| {
            startup_manifest_from_rootfs = true;
            break :blk manifest;
        }
        fail("ManagerInit: load startup manifest failed\n");
    };
    _ = userLog(if (startup_manifest_from_rootfs)
        "ManagerInit: rootfs startup manifest ready\n"
    else
        "ManagerInit: bootfs startup manifest ready\n");
    runStartupManifest(&launch_ctx_storage, startup_manifest);
    if (!launch_ctx_storage.has_boot_display) {
        launch_ctx_storage.requireBootDisplay();
    }
    launch_ctx_storage.publishCapCtlService();

    while (true) {
        const received = waitEvent(true, 1);
        if (received >= cap_transfer_abi.transfer_id_min) {
            const request_paddr = acceptCapTransfer(received);
            if (request_paddr >= 0x1000) launch_ctx_storage.handleCapCtlRequest(request_paddr);
        }
    }
}

pub export fn _start() noreturn {
    if (allocMapPages(manager_stack_extension_base_va, manager_stack_extension_pages, true, 0) != 0) {
        fail("ManagerInit: stack extend failed\n");
    }
    managerMain();
}
