const std = @import("std");
const bootfs_format = @import("support_root").bootfs_format;
const cap_transfer_abi = @import("support_root").cap_transfer_abi;
const boot_status_abi = @import("support_root").boot_status_abi;
const boot_status_client = @import("support_root").boot_status_client;
const mouse_shared_abi = @import("support_root").mouse_shared_abi;
const process_abi = @import("support_root").process_abi;
const boot_manifest_abi = @import("support_root").boot_manifest_abi;
const image_abi = @import("support_root").image_abi;
const startup_plan_abi = @import("support_root").startup_plan_abi;
const queue_abi = @import("support_root").queue_abi;
const init_bootstrap_abi = @import("support_root").init_bootstrap_abi;
const input_bootstrap = @import("support_root").input_driver_bootstrap_abi;
const bootstrap_fs_bootstrap = @import("support_root").bootstrap_fs_bootstrap_abi;
const block_bootstrap = @import("support_root").block_bootstrap_abi;
const block_demo_bootstrap = @import("support_root").block_demo_bootstrap_abi;
const fs_client = @import("support_root").fs_client;
const persistent_fs_bootstrap = @import("support_root").persistent_fs_bootstrap_abi;
const terminal_bootstrap = @import("support_root").terminal_bootstrap_abi;
const mouse_demo_bootstrap = @import("support_root").mouse_demo_bootstrap_abi;
const service_registry_abi = @import("support_root").service_registry_abi;
const taskbar_bootstrap = @import("support_root").taskbar_bootstrap_abi;
const vfs_client = @import("support_root").vfs_client;

const syscall_log: u64 = 0x9;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_send_cap: u64 = 0x6;
const syscall_share_cap: u64 = 0x2B;
const syscall_install_mmio_cap: u64 = 0x2F;
const syscall_switch_thread: u64 = 0x5;
const syscall_install_endpoint: u64 = 0x26;
const syscall_signal_endpoint: u64 = 0x2C;
const syscall_get_process_slot: u64 = 0x2E;
const bootstrap_fs_request_va: u64 = 0x3C18_4000;
const bootstrap_fs_response_va: u64 = 0x3C18_5000;
const vfs_request_va: u64 = 0x3C10_4000;
const vfs_response_va: u64 = 0x3C10_5000;
const dynamic_bootstrap_source_base_va: u64 = 0x3C10_6000;
const keyboard_shared_magic: u64 = 0x4B534852; // "KSHR"
const mouse_shared_magic: u64 = mouse_shared_abi.magic;
const startup_manifest_path = "/boot/startup_manifest.txt";
const rootfs_startup_manifest_path = "/startup_manifest.txt";
const startup_manifest_max_bytes: usize = 1024;
const vfs_boot_config_magic: u64 = 0x5646_5343; // "VFSC"
const persistent_fs_start_block: u64 = 395264;
const vfs_boot_config_version: u64 = 2;
const vfs_boot_config_flag_bootfs_present: u64 = 1 << 0;
const input_shared_page_paddr_index: usize = 10;
const bootfs_root_object_id: u64 = 1;
const raw_block_root_object_id: u64 = 2;
const inspect_mmio_base_va: u64 = 0x3F00_0000;
const syscall_batch_max_pages: usize = 64;
const virtio_vendor_id: u64 = 0x1AF4;
const virtio_input_device_modern: u64 = 0x1052;
const virtio_input_subsystem_id: u64 = 0x0012;
const virtio_blk_device_modern: u64 = 0x1042;
const virtio_blk_subsystem_id: u64 = 0x0002;
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

const InputDeviceKind = startup_plan_abi.StartupInputSelector;
const BlockDeviceKind = startup_plan_abi.StartupBlockSelector;

const BlockGeometry = struct {
    capacity_sectors: u64,
    logical_block_size: u64,
};

const ClassifiedBootstrapDevices = struct {
    keyboard: init_bootstrap_abi.DeviceDescriptor,
    pointer: init_bootstrap_abi.DeviceDescriptor,
    block: init_bootstrap_abi.DeviceDescriptor,
};

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn userLogHex(label: []const u8, value: u64) void {
    var buf: [96]u8 = undefined;
    var len: usize = 0;
    while (len < label.len and len < buf.len) : (len += 1) {
        buf[len] = label[len];
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
    _ = userLog(buf[0..len]);
}

fn noteBootStatus(status_bits: u32) void {
    _ = boot_status_client.set(status_bits);
}

fn switchThread(thread_index: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_switch_thread),
          [arg0] "{rdi}" (thread_index),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn getProcessSlot() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_get_process_slot),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn requireProcessSlot() u64 {
    return getProcessSlot();
}

fn signalEndpoint(endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_signal_endpoint),
          [arg0] "{rdi}" (endpoint_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn spawnExec(exec_token: u64, bootstrap_source_va: u64, bootstrap_target_va: u64, bootstrap_flags: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (process_abi.syscall_spawn_exec),
          [arg0] "{rdi}" (exec_token),
          [arg1] "{rsi}" (bootstrap_source_va),
          [arg2] "{rdx}" (bootstrap_target_va),
          [arg3] "{rcx}" (bootstrap_flags),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn spawnExecWithBootstrapPages(exec_token: u64, descriptors: []const process_abi.BootstrapPageDescriptor) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (process_abi.syscall_spawn_exec),
          [arg0] "{rdi}" (exec_token),
          [arg1] "{rsi}" (@as(u64, @intFromPtr(descriptors.ptr))),
          [arg2] "{rdx}" (@as(u64, @intCast(descriptors.len))),
          [arg3] "{rcx}" (process_abi.spawn_flag_bootstrap_descriptor_table),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn spawnExecWithExtendedBootstrapTable(exec_token: u64, table: *const process_abi.BootstrapDescriptorTable) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (process_abi.syscall_spawn_exec),
          [arg0] "{rdi}" (exec_token),
          [arg1] "{rsi}" (@as(u64, @intFromPtr(table))),
          [arg2] "{rdx}" (@as(u64, 0)),
          [arg3] "{rcx}" (process_abi.spawn_flag_bootstrap_extended_descriptor_table),
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

fn findInputDeviceDescriptor(kind: InputDeviceKind) ?init_bootstrap_abi.DeviceDescriptor {
    const page = descriptorPage() orelse return null;
    var i: usize = 0;
    while (i < page.device_count and i < init_bootstrap_abi.max_device_descriptors) : (i += 1) {
        const descriptor = page.devices[i];
        if (!isBootstrapDeviceDescriptorPresent(descriptor)) continue;
        if (!isVirtioInputDeviceDescriptor(descriptor)) continue;
        const classified = classifyInputDeviceDescriptor(descriptor) orelse continue;
        if (classified != kind) continue;
        return descriptor;
    }
    return null;
}

fn findBlockDeviceDescriptor(kind: BlockDeviceKind) ?init_bootstrap_abi.DeviceDescriptor {
    _ = kind;
    const page = descriptorPage() orelse return null;
    var i: usize = 0;
    while (i < page.device_count and i < init_bootstrap_abi.max_device_descriptors) : (i += 1) {
        const descriptor = page.devices[i];
        if (!isBootstrapDeviceDescriptorPresent(descriptor)) continue;
        if (!isVirtioBlockDeviceDescriptor(descriptor)) continue;
        return descriptor;
    }
    return null;
}

fn findBootImageDescriptor(kind: boot_manifest_abi.ImageKind) ?boot_manifest_abi.BootImageDescriptor {
    const page = descriptorPage() orelse return null;
    var i: usize = 0;
    while (i < page.boot_image_count and i < boot_manifest_abi.max_boot_image_descriptors) : (i += 1) {
        const descriptor = page.boot_images[i];
        if (descriptor.kind != @intFromEnum(kind)) continue;
        return descriptor;
    }
    return null;
}

fn bootImagePresent(kind: boot_manifest_abi.ImageKind) bool {
    const descriptor = findBootImageDescriptor(kind) orelse return false;
    return (descriptor.flags & boot_manifest_abi.image_flag_present) != 0;
}

fn requireBootImage(kind: boot_manifest_abi.ImageKind, failure_message: []const u8) void {
    if (bootImagePresent(kind)) return;
    _ = userLog(failure_message);
    while (true) asm volatile ("pause");
}

fn requireSpawnPageDescriptor(
    kind: init_bootstrap_abi.SpawnPageKind,
    subject: init_bootstrap_abi.SpawnPageSubject,
    failure_message: []const u8,
) init_bootstrap_abi.SpawnPageDescriptor {
    return findSpawnPageDescriptor(kind, subject) orelse {
        _ = userLog(failure_message);
        while (true) asm volatile ("pause");
    };
}

fn requireInputDeviceDescriptor(
    kind: InputDeviceKind,
    failure_message: []const u8,
) init_bootstrap_abi.DeviceDescriptor {
    return findInputDeviceDescriptor(kind) orelse {
        _ = userLog(failure_message);
        while (true) asm volatile ("pause");
    };
}

fn requireBlockDeviceDescriptor(
    kind: BlockDeviceKind,
    failure_message: []const u8,
) init_bootstrap_abi.DeviceDescriptor {
    return findBlockDeviceDescriptor(kind) orelse {
        _ = userLog(failure_message);
        while (true) asm volatile ("pause");
    };
}

fn requireClassifiedBootstrapDevices() ClassifiedBootstrapDevices {
    return .{
        .keyboard = requireInputDeviceDescriptor(.keyboard, "Init: keyboard input descriptor missing\n"),
        .pointer = requireInputDeviceDescriptor(.pointer, "Init: pointer input descriptor missing\n"),
        .block = requireBlockDeviceDescriptor(.virtio_blk, "Init: block device descriptor missing\n"),
    };
}

fn requirePrimaryDisplayDescriptor() init_bootstrap_abi.DisplayDescriptor {
    const page = descriptorPage() orelse {
        _ = userLog("Init: bootstrap descriptor page missing\n");
        while (true) asm volatile ("pause");
    };
    if ((page.primary_display.flags & init_bootstrap_abi.display_flag_present) == 0) {
        _ = userLog("Init: primary display descriptor missing\n");
        while (true) asm volatile ("pause");
    }
    return page.primary_display;
}

fn requireBootFsArchive() init_bootstrap_abi.BootArchiveDescriptor {
    const page = descriptorPage() orelse {
        _ = userLog("Init: bootstrap descriptor page missing\n");
        while (true) asm volatile ("pause");
    };
    if ((page.bootfs_archive.flags & init_bootstrap_abi.boot_archive_flag_present) == 0) {
        _ = userLog("Init: bootfs archive descriptor missing\n");
        while (true) asm volatile ("pause");
    }
    if (page.bootfs_archive.image_va == 0 or page.bootfs_archive.size_bytes == 0) {
        _ = userLog("Init: bootfs archive descriptor invalid\n");
        while (true) asm volatile ("pause");
    }
    return page.bootfs_archive;
}

fn requireBootFsArchiveView() *const BootFsArchiveView {
    const page = descriptorPage() orelse {
        _ = userLog("Init: bootstrap descriptor page missing\n");
        while (true) asm volatile ("pause");
    };
    const archive = requireBootFsArchive();
    if (archive.page_count == 0 or archive.page_count > init_bootstrap_abi.max_boot_archive_pages) {
        _ = userLog("Init: bootfs archive page count invalid\n");
        while (true) asm volatile ("pause");
    }
    var i: usize = 0;
    while (i < archive.page_count) : (i += 1) {
        const paddr = page.bootfs_page_paddrs[i];
        if (paddr < 0x1000) {
            _ = userLog("Init: bootfs archive page paddr invalid\n");
            while (true) asm volatile ("pause");
        }
        bootfs_archive_view_storage.page_paddrs[i] = paddr;
    }
    bootfs_archive_view_storage.base_va = archive.image_va;
    bootfs_archive_view_storage.size_bytes = @intCast(archive.size_bytes);
    bootfs_archive_view_storage.page_count = @intCast(archive.page_count);
    return &bootfs_archive_view_storage;
}

fn requireWindowServiceEntry(base_va: u64) service_registry_abi.ServiceEntry {
    return service_registry_abi.findService(base_va, .window) orelse {
        _ = userLog("Init: window service entry missing\n");
        while (true) asm volatile ("pause");
    };
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

fn sendCap(paddr: u64, endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_send_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (endpoint_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn shareCap(paddr: u64, endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_share_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (endpoint_id),
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

fn recvCap() u64 {
    const transfer = recvCapTransfer();
    if (transfer < cap_transfer_abi.transfer_id_min) return transfer;
    return acceptCapTransfer(transfer);
}

fn recvCapTransfer() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (@as(u64, 0xA)),
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

fn waitEvent(wait_mailbox: bool, timeout_ticks: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (@as(u64, 0x17)),
          [arg0] "{rdi}" (@as(u64, if (wait_mailbox) 1 else 0)),
          [arg1] "{rsi}" (timeout_ticks),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (@as(u64, 0x2)),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

/// Install an MMIO capability for a physical page (init-only bootstrap syscall).
/// rights_bits: 0x1=read, 0x2=write, 0x4=dma, 0x8=grant
fn installMmioCap(paddr: u64, rights_bits: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_install_mmio_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (rights_bits),
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

fn grantQueueCap(token: u64, to_process_slot: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (queue_abi.syscall_grant_cap),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (to_process_slot),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn armDeferredCompositor(classic_exec_token: u64, gpu_exec_token: u64, target_process_slot: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (process_abi.syscall_arm_deferred_compositor),
          [arg0] "{rdi}" (classic_exec_token),
          [arg1] "{rsi}" (gpu_exec_token),
          [arg2] "{rdx}" (target_process_slot),
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

fn grantVmObject(token: u64, to_process_slot: u64, rights: image_abi.VmObjectRights) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (image_abi.syscall_grant_vm_object),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (to_process_slot),
          [arg2] "{rdx}" (image_abi.vmObjectRightsToBits(rights)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn ensureBootFsVmObjectCache(bootfs: *const BootFsArchiveView) u64 {
    if (bootfs_vm_token_cache != 0) return bootfs_vm_token_cache;
    bootfs_vm_token_cache = installVmObject(bootfs.base_va, bootfs.size_bytes, .{
        .read = true,
        .map = true,
        .grant = true,
    });
    if (image_abi.decodeVmObjectToken(bootfs_vm_token_cache) == null) {
        _ = userLog("Init: install VFS bootfs vm object failed\n");
        while (true) asm volatile ("pause");
    }
    return bootfs_vm_token_cache;
}

fn pauseLoop(iterations: usize) void {
    var i: usize = 0;
    while (i < iterations) : (i += 1) asm volatile ("pause");
}

fn sendCapRetry(paddr: u64, endpoint_id: u64) bool {
    var attempt: usize = 0;
    while (attempt < 1_000_000) : (attempt += 1) {
        if (sendCap(paddr, endpoint_id) == 0) return true;
        _ = waitEvent(false, 1);
        pauseLoop(256);
    }
    return false;
}


fn waitRecvCap() u64 {
    while (true) {
        const received = waitEvent(true, 0);
        if (received >= cap_transfer_abi.transfer_id_min) return acceptCapTransfer(received);
        const cap = if (received != 0) received else recvCap();
        if (cap >= 0x1000) return cap;
        pauseLoop(1024);
    }
}

const DeviceMmioPageSet = struct {
    paddrs: [4]u64 = [_]u64{0} ** 4,
    rights: [4]u64 = [_]u64{0} ** 4,
    count: usize = 0,
};

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

fn ensureDeviceMmioCapsInstalled(descriptor: init_bootstrap_abi.DeviceDescriptor) bool {
    const page_right_grant: u64 = 0x8;
    const set = collectDeviceMmioPages(descriptor, true) orelse return false;
    var i: usize = 0;
    while (i < set.count) : (i += 1) {
        if (installMmioCap(set.paddrs[i], set.rights[i] | page_right_grant) != 0) return false;
    }
    return true;
}

fn grantDeviceMmioPages(descriptor: init_bootstrap_abi.DeviceDescriptor, child_process_slot: u64) bool {
    const set = collectDeviceMmioPages(descriptor, false) orelse return false;
    var i: usize = 0;
    while (i < set.count) : (i += 1) {
        if (grantCap(child_process_slot, set.paddrs[i], set.rights[i]) != 0) return false;
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

fn grantInputResources(
    config_source_va: u64,
    descriptor: init_bootstrap_abi.DeviceDescriptor,
    child_process_slot: u64,
) bool {
    const submit_source_token = descriptor.init_queue_submit_token;
    const notify_source_token = descriptor.init_queue_notify_token;
    if (submit_source_token == 0 or notify_source_token == 0) return false;
    if (!ensureDeviceMmioCapsInstalled(descriptor)) return false;
    if (!grantDeviceMmioPages(descriptor, child_process_slot)) return false;

    const submit_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(submit_source_token), child_process_slot);
    const notify_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(notify_source_token), child_process_slot);
    const submit_child = queue_abi.decodeQueueCapToken(submit_child_encoded) orelse return false;
    const notify_child = queue_abi.decodeQueueCapToken(notify_child_encoded) orelse return false;
    input_bootstrap.writeGrantedQueueTokens(config_source_va, submit_child, notify_child);
    return true;
}

fn grantInputDriverResources(descriptor: init_bootstrap_abi.DeviceDescriptor, child_process_slot: u64) bool {
    return grantInputResources(descriptor.bootstrap_source_va, descriptor, child_process_slot);
}

fn grantBlockDriverResources(config_source_va: u64, descriptor: init_bootstrap_abi.DeviceDescriptor, child_process_slot: u64) bool {
    const submit_source_token = descriptor.init_queue_submit_token;
    const notify_source_token = descriptor.init_queue_notify_token;
    if (submit_source_token == 0 or notify_source_token == 0) return false;
    if (!ensureDeviceMmioCapsInstalled(descriptor)) return false;
    if (!grantDeviceMmioPages(descriptor, child_process_slot)) return false;

    const submit_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(submit_source_token), child_process_slot);
    const notify_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(notify_source_token), child_process_slot);
    const submit_child = queue_abi.decodeQueueCapToken(submit_child_encoded) orelse return false;
    const notify_child = queue_abi.decodeQueueCapToken(notify_child_encoded) orelse return false;
    block_bootstrap.writeGrantedQueueTokens(config_source_va, submit_child, notify_child);
    return true;
}

fn grantBootstrapFsBlockResources(config_source_va: u64, descriptor: init_bootstrap_abi.DeviceDescriptor, child_process_slot: u64) bool {
    const submit_source_token = descriptor.init_queue_submit_token;
    const notify_source_token = descriptor.init_queue_notify_token;
    if (submit_source_token == 0 or notify_source_token == 0) return false;
    if (!ensureDeviceMmioCapsInstalled(descriptor)) return false;
    if (!grantDeviceMmioPages(descriptor, child_process_slot)) return false;

    const submit_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(submit_source_token), child_process_slot);
    const notify_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(notify_source_token), child_process_slot);
    const submit_child = queue_abi.decodeQueueCapToken(submit_child_encoded) orelse return false;
    const notify_child = queue_abi.decodeQueueCapToken(notify_child_encoded) orelse return false;
    bootstrap_fs_bootstrap.writeGrantedQueueTokens(config_source_va, submit_child, notify_child);
    return true;
}

fn lookupFileWithRetry(client: *vfs_client.Client, path: []const u8) ?vfs_client.LookupResult {
    var attempt: usize = 0;
    while (attempt < 4096) : (attempt += 1) {
        const result = client.lookup(0, path) catch {
            pauseLoop(1024);
            continue;
        };
        if (result.object_kind == .vnode_file) return result;
        pauseLoop(1024);
    }
    return null;
}

const StartupManifest = struct {
    bytes: [startup_manifest_max_bytes]u8,
    len: usize,

    fn slice(self: *const StartupManifest) []const u8 {
        return self.bytes[0..self.len];
    }
};

const CompositorVariant = startup_plan_abi.StartupCompositorVariant;
const WindowBootstrapConfigKind = startup_plan_abi.StartupWindowConfig;

const StartupPolicy = struct {
    label: []const u8 = "",
    path: []const u8 = "",
    action: startup_plan_abi.StartupAction = .vfs,
    exec_source: startup_plan_abi.StartupExecSource = .startup_path,
    ensure_flags: u64 = 0,
    require_flags: u64 = 0,
    input_kind: ?InputDeviceKind = null,
    block_kind: ?BlockDeviceKind = null,
    window_flags: u64 = 0,
    window_config_kind: ?WindowBootstrapConfigKind = null,
    compositor_variant: ?CompositorVariant = null,
};

const StartupNode = struct {
    policy: StartupPolicy,
    launched: bool = false,
};

const BootFsArchiveView = struct {
    base_va: u64,
    size_bytes: usize,
    page_count: usize,
    page_paddrs: [init_bootstrap_abi.max_boot_archive_pages]u64,

    fn getHeader(self: *const BootFsArchiveView) ?*const bootfs_format.BootFsHeader {
        if (self.size_bytes < @sizeOf(bootfs_format.BootFsHeader)) return null;
        const hdr: *const bootfs_format.BootFsHeader = @ptrFromInt(self.base_va);
        if (hdr.magic != bootfs_format.magic) return null;
        if (hdr.version != bootfs_format.version) return null;
        if (hdr.header_bytes != @sizeOf(bootfs_format.BootFsHeader)) return null;
        if (hdr.image_bytes == 0 or hdr.image_bytes > self.size_bytes) return null;
        if (hdr.entry_table_offset + hdr.entry_bytes > hdr.image_bytes) return null;
        if (hdr.string_table_offset + hdr.string_table_bytes > hdr.image_bytes) return null;
        if (hdr.data_offset + hdr.data_bytes > hdr.image_bytes) return null;
        return hdr;
    }

    fn entries(self: *const BootFsArchiveView, hdr: *const bootfs_format.BootFsHeader) []const bootfs_format.BootFsEntry {
        const ptr: [*]const bootfs_format.BootFsEntry = @ptrFromInt(self.base_va + @as(u64, @intCast(hdr.entry_table_offset)));
        return ptr[0..hdr.entry_count];
    }

    fn stringTable(self: *const BootFsArchiveView, hdr: *const bootfs_format.BootFsHeader) []const u8 {
        const ptr: [*]const u8 = @ptrFromInt(self.base_va + @as(u64, @intCast(hdr.string_table_offset)));
        return ptr[0..@intCast(hdr.string_table_bytes)];
    }

    fn pathForEntry(self: *const BootFsArchiveView, hdr: *const bootfs_format.BootFsHeader, entry: bootfs_format.BootFsEntry) ?[]const u8 {
        const table = self.stringTable(hdr);
        const begin: usize = entry.path_offset;
        const end = begin + entry.path_bytes;
        if (entry.kind != bootfs_format.kind_regular) return null;
        if (end > table.len) return null;
        return table[begin..end];
    }

    fn findRegularFile(self: *const BootFsArchiveView, abs_path: []const u8) ?[]const u8 {
        const hdr = self.getHeader() orelse return null;
        const boot_entries = self.entries(hdr);
        for (boot_entries) |entry| {
            const path = self.pathForEntry(hdr, entry) orelse continue;
            if (!std.mem.eql(u8, path, abs_path)) continue;
            if (entry.data_offset > hdr.image_bytes) return null;
            if (entry.data_bytes > hdr.image_bytes - entry.data_offset) return null;
            const ptr: [*]const u8 = @ptrFromInt(self.base_va + @as(u64, @intCast(entry.data_offset)));
            return ptr[0..@intCast(entry.data_bytes)];
        }
        return null;
    }
};

var bootfs_archive_view_storage = BootFsArchiveView{
    .base_va = 0,
    .size_bytes = 0,
    .page_count = 0,
    .page_paddrs = [_]u64{0} ** init_bootstrap_abi.max_boot_archive_pages,
};

var startup_manifest_storage = StartupManifest{
    .bytes = [_]u8{0} ** startup_manifest_max_bytes,
    .len = 0,
};
var vfs_bootstrap_pages_storage: [process_abi.max_bootstrap_page_descriptors]process_abi.BootstrapPageDescriptor = undefined;
var vfs_bootstrap_table_storage = process_abi.BootstrapDescriptorTable{};
var bootstrap_fs_bootstrap_pages_storage: [process_abi.max_bootstrap_page_descriptors]process_abi.BootstrapPageDescriptor = undefined;
var bootstrap_fs_bootstrap_table_storage = process_abi.BootstrapDescriptorTable{};
var block_bootstrap_pages_storage: [process_abi.max_bootstrap_page_descriptors]process_abi.BootstrapPageDescriptor = undefined;
var block_bootstrap_table_storage = process_abi.BootstrapDescriptorTable{};
var persistent_fs_bootstrap_pages_storage: [process_abi.max_bootstrap_page_descriptors]process_abi.BootstrapPageDescriptor = undefined;
var persistent_fs_bootstrap_table_storage = process_abi.BootstrapDescriptorTable{};
var boot_display_bootstrap_table_storage = process_abi.BootstrapDescriptorTable{};
var bootfs_vm_token_cache: u64 = 0;

fn loadStartupManifestFromBootFs(bootfs: *const BootFsArchiveView) ?*const StartupManifest {
    const bytes = bootfs.findRegularFile(startup_manifest_path) orelse return null;
    @memset(startup_manifest_storage.bytes[0..], 0);
    startup_manifest_storage.len = 0;
    const copy_len: usize = @min(bytes.len, startup_manifest_storage.bytes.len);
    @memcpy(startup_manifest_storage.bytes[0..copy_len], bytes[0..copy_len]);
    startup_manifest_storage.len = copy_len;
    return &startup_manifest_storage;
}

fn loadStartupManifestFromFs(client: *fs_client.Client, path: []const u8) ?*const StartupManifest {
    const file = client.lookup(client.mount_token, path) catch return null;
    if (file.object_kind != .vnode_file) return null;
    if (file.file_bytes > startup_manifest_storage.bytes.len) return null;
    const opened = client.open(file.token) catch {
        client.close(file.token) catch {};
        return null;
    };
    defer client.close(opened.token) catch {};
    defer client.close(file.token) catch {};

    @memset(startup_manifest_storage.bytes[0..], 0);
    startup_manifest_storage.len = 0;

    var offset: u64 = 0;
    while (startup_manifest_storage.len < file.file_bytes) {
        const dst = startup_manifest_storage.bytes[startup_manifest_storage.len..@intCast(file.file_bytes)];
        const read_result = client.read(opened.token, offset, dst) catch return null;
        if (read_result.bytes_read == 0) break;
        startup_manifest_storage.len += read_result.bytes_read;
        offset = read_result.next_offset;
    }

    if (startup_manifest_storage.len != file.file_bytes) return null;
    return &startup_manifest_storage;
}

fn startupManifestFail(message: []const u8) noreturn {
    _ = userLog(message);
    while (true) asm volatile ("pause");
}

fn windowBootstrapFlagsForConfig(kind: WindowBootstrapConfigKind) u64 {
    return switch (kind) {
        .terminal => window_bootstrap_flag_service_registry | window_bootstrap_flag_keyboard_shared,
        .taskbar => window_bootstrap_flag_service_registry | window_bootstrap_flag_pointer_shared | window_bootstrap_flag_taskbar_panel,
        .mouse_demo => window_bootstrap_flag_service_registry | window_bootstrap_flag_pointer_shared,
    };
}

fn defaultStartupPolicyForLegacyRole(role: startup_plan_abi.StartupProgramRole, path: []const u8) StartupPolicy {
    return switch (role) {
        .vfs => .{
            .label = "VFS",
            .path = path,
            .action = .vfs,
        },
        .keyboard_driver => .{
            .label = "keyboard driver",
            .path = path,
            .action = .input_driver,
            .input_kind = .keyboard,
        },
        .mouse_driver => .{
            .label = "mouse driver",
            .path = path,
            .action = .input_driver,
            .ensure_flags = startup_plan_abi.ensure_flag_boot_display,
            .input_kind = .pointer,
        },
        .bootstrap_fs => .{
            .label = "bootstrap fs",
            .path = path,
            .action = .bootstrap_fs_server,
            .exec_source = .bootfs,
        },
        .block_driver => .{
            .label = "block driver",
            .path = path,
            .action = .block_driver,
            .block_kind = .virtio_blk,
        },
        .persistent_fs => .{
            .label = "persistent fs",
            .path = path,
            .action = .persistent_fs_server,
            .require_flags = startup_plan_abi.require_flag_block_service,
        },
        .block_demo => .{
            .label = "block demo",
            .path = path,
            .action = .block_client,
            .require_flags = startup_plan_abi.require_flag_block_service,
        },
        .terminal_window => .{
            .label = "terminal",
            .path = path,
            .action = .window_client,
            .ensure_flags = startup_plan_abi.ensure_flag_boot_display,
            .require_flags = startup_plan_abi.require_flag_keyboard_shared | startup_plan_abi.require_flag_compositor_armed,
            .window_flags = windowBootstrapFlagsForConfig(.terminal),
            .window_config_kind = .terminal,
        },
        .taskbar => .{
            .label = "taskbar",
            .path = path,
            .action = .window_client,
            .ensure_flags = startup_plan_abi.ensure_flag_boot_display,
            .require_flags = startup_plan_abi.require_flag_pointer_shared | startup_plan_abi.require_flag_compositor_armed,
            .window_flags = windowBootstrapFlagsForConfig(.taskbar),
            .window_config_kind = .taskbar,
        },
        .mouse_button_demo => .{
            .label = "mouse demo",
            .path = path,
            .action = .window_client,
            .ensure_flags = startup_plan_abi.ensure_flag_boot_display,
            .require_flags = startup_plan_abi.require_flag_pointer_shared | startup_plan_abi.require_flag_compositor_armed,
            .window_flags = windowBootstrapFlagsForConfig(.mouse_demo),
            .window_config_kind = .mouse_demo,
        },
        .compositor => .{
            .label = "compositor",
            .path = path,
            .action = .deferred_compositor,
            .ensure_flags = startup_plan_abi.ensure_flag_boot_display,
            .compositor_variant = .classic,
        },
        .gpu_compositor => .{
            .label = "gpu compositor",
            .path = path,
            .action = .deferred_compositor,
            .ensure_flags = startup_plan_abi.ensure_flag_boot_display,
            .compositor_variant = .gpu,
        },
    };
}

fn parseStartupEnsureFlags(value: []const u8) u64 {
    var flags: u64 = 0;
    var it = std.mem.tokenizeScalar(u8, value, ',');
    while (it.next()) |item_raw| {
        const item = std.mem.trim(u8, item_raw, " \t");
        if (item.len == 0) continue;
        flags |= startup_plan_abi.ensureBitFromKey(item) orelse startupManifestFail("Init: unknown startup ensure flag\n");
    }
    return flags;
}

fn parseStartupRequirementFlags(value: []const u8) u64 {
    var flags: u64 = 0;
    var it = std.mem.tokenizeScalar(u8, value, ',');
    while (it.next()) |item_raw| {
        const item = std.mem.trim(u8, item_raw, " \t");
        if (item.len == 0) continue;
        flags |= startup_plan_abi.requirementBitFromKey(item) orelse startupManifestFail("Init: unknown startup require flag\n");
    }
    return flags;
}

fn defaultStartupPolicyLabel(policy: *const StartupPolicy) []const u8 {
    if (policy.window_config_kind) |kind| {
        return switch (kind) {
            .terminal => "terminal",
            .taskbar => "taskbar",
            .mouse_demo => "mouse demo",
        };
    }
    if (policy.compositor_variant) |variant| {
        return switch (variant) {
            .classic => "compositor",
            .gpu => "gpu compositor",
        };
    }
    return switch (policy.action) {
        .vfs => "VFS",
        .bootstrap_fs_server => "bootstrap fs",
        .input_driver => switch (policy.input_kind orelse .keyboard) {
            .keyboard => "keyboard driver",
            .pointer => "pointer driver",
        },
        .block_driver => "block driver",
        .persistent_fs_server => "persistent fs",
        .block_client => "block client",
        .window_client => "window client",
        .deferred_compositor => "compositor",
    };
}

fn normalizeStartupPolicy(policy: *StartupPolicy) void {
    switch (policy.action) {
        .window_client => {
            policy.ensure_flags |= startup_plan_abi.ensure_flag_boot_display;
            policy.require_flags |= startup_plan_abi.require_flag_compositor_armed;
            if (policy.window_config_kind) |kind| {
                if (policy.window_flags == 0) policy.window_flags = windowBootstrapFlagsForConfig(kind);
            }
            if ((policy.window_flags & window_bootstrap_flag_keyboard_shared) != 0) {
                policy.require_flags |= startup_plan_abi.require_flag_keyboard_shared;
            }
            if ((policy.window_flags & window_bootstrap_flag_pointer_shared) != 0) {
                policy.require_flags |= startup_plan_abi.require_flag_pointer_shared;
            }
        },
        .persistent_fs_server, .block_client => {
            policy.require_flags |= startup_plan_abi.require_flag_block_service;
        },
        .deferred_compositor => {
            policy.ensure_flags |= startup_plan_abi.ensure_flag_boot_display;
        },
        else => {},
    }
}

fn validateStartupPolicy(policy: *StartupPolicy) void {
    normalizeStartupPolicy(policy);
    switch (policy.action) {
        .input_driver => {
            if (policy.input_kind == null) startupManifestFail("Init: startup input driver missing selector\n");
        },
        .block_driver => {
            if (policy.block_kind == null) startupManifestFail("Init: startup block driver missing selector\n");
        },
        .window_client => {
            if (policy.window_config_kind == null) startupManifestFail("Init: startup window client missing window kind\n");
        },
        .deferred_compositor => {
            if (policy.compositor_variant == null) startupManifestFail("Init: startup compositor missing variant\n");
        },
        else => {},
    }
    if (policy.label.len == 0) policy.label = defaultStartupPolicyLabel(policy);
}

fn parseStartupPolicyToken(policy: *StartupPolicy, token: []const u8, has_action: *bool, has_path: *bool) void {
    const eq_index = std.mem.indexOfScalar(u8, token, '=') orelse startupManifestFail("Init: malformed startup manifest token\n");
    const key = token[0..eq_index];
    const value = token[eq_index + 1 ..];
    if (key.len == 0 or value.len == 0) startupManifestFail("Init: malformed startup manifest token\n");

    if (std.mem.eql(u8, key, "action")) {
        policy.action = startup_plan_abi.actionFromKey(value) orelse startupManifestFail("Init: unknown startup action\n");
        has_action.* = true;
        return;
    }
    if (std.mem.eql(u8, key, "path")) {
        policy.path = value;
        has_path.* = true;
        return;
    }
    if (std.mem.eql(u8, key, "label")) {
        policy.label = value;
        return;
    }
    if (std.mem.eql(u8, key, "load")) {
        policy.exec_source = startup_plan_abi.execSourceFromKey(value) orelse startupManifestFail("Init: unknown startup exec source\n");
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
    if (std.mem.eql(u8, key, "input")) {
        policy.input_kind = startup_plan_abi.inputSelectorFromKey(value) orelse startupManifestFail("Init: unknown startup input selector\n");
        return;
    }
    if (std.mem.eql(u8, key, "block")) {
        policy.block_kind = startup_plan_abi.blockSelectorFromKey(value) orelse startupManifestFail("Init: unknown startup block selector\n");
        return;
    }
    if (std.mem.eql(u8, key, "window")) {
        const kind = startup_plan_abi.windowConfigFromKey(value) orelse startupManifestFail("Init: unknown startup window kind\n");
        policy.window_config_kind = kind;
        policy.window_flags = windowBootstrapFlagsForConfig(kind);
        return;
    }
    if (std.mem.eql(u8, key, "compositor")) {
        policy.compositor_variant = startup_plan_abi.compositorVariantFromKey(value) orelse startupManifestFail("Init: unknown startup compositor variant\n");
        return;
    }
    startupManifestFail("Init: unknown startup manifest key\n");
}

fn parseStartupManifestLine(raw_line: []const u8) ?StartupPolicy {
    const line = std.mem.trim(u8, raw_line, " \t\r");
    if (line.len == 0 or line[0] == '#') return null;

    var tokens = std.mem.tokenizeAny(u8, line, " \t");
    const first = tokens.next() orelse return null;

    var policy = StartupPolicy{};
    var has_action = false;
    var has_path = false;

    if (std.mem.indexOfScalar(u8, first, '=') == null) {
        const role = startup_plan_abi.roleFromKey(first) orelse startupManifestFail("Init: unknown startup manifest role\n");
        const path = tokens.next() orelse startupManifestFail("Init: empty startup manifest path\n");
        policy = defaultStartupPolicyForLegacyRole(role, path);
        has_action = true;
        has_path = true;
    } else {
        parseStartupPolicyToken(&policy, first, &has_action, &has_path);
    }

    while (tokens.next()) |token| {
        parseStartupPolicyToken(&policy, token, &has_action, &has_path);
    }
    if (!has_action) startupManifestFail("Init: startup manifest action missing\n");
    if (!has_path or policy.path.len == 0) startupManifestFail("Init: empty startup manifest path\n");
    validateStartupPolicy(&policy);
    return policy;
}

var next_dynamic_service_endpoint_id: u64 = service_registry_abi.dynamic_endpoint_id_base;
var next_dynamic_bootstrap_source_va: u64 = dynamic_bootstrap_source_base_va;
var next_inspect_mmio_page_va: u64 = inspect_mmio_base_va;

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

fn allocChildServiceRegistryPage(
    window_service: service_registry_abi.ServiceEntry,
    vfs_process_slot: ?u64,
    vfs_endpoint_id: ?u64,
    bootstrap_fs_process_slot: ?u64,
    bootstrap_fs_endpoint_id: ?u64,
    block_process_slot: ?u64,
    block_endpoint_id: ?u64,
    persistent_fs_process_slot: ?u64,
    persistent_fs_endpoint_id: ?u64,
) ?u64 {
    const source_va = allocDynamicBootstrapSourceVa();
    var registry_paddr: u64 = 0;
    if (allocMapPages(source_va, 1, true, @intFromPtr(&registry_paddr)) != 0) return null;
    if (registry_paddr < 0x1000) return null;
    const window_endpoint_id = allocDynamicServiceEndpointId();
    service_registry_abi.initPage(source_va);
    service_registry_abi.addService(source_va, .window, window_service.process_slot, window_endpoint_id);
    if (vfs_process_slot) |slot| {
        if (vfs_endpoint_id) |endpoint_id| {
            service_registry_abi.addService(source_va, .vfs, slot, endpoint_id);
        }
    }
    if (bootstrap_fs_process_slot) |slot| {
        if (bootstrap_fs_endpoint_id) |endpoint_id| {
            service_registry_abi.addService(source_va, .bootstrap_fs, slot, endpoint_id);
        }
    }
    if (block_process_slot) |slot| {
        if (block_endpoint_id) |endpoint_id| {
            service_registry_abi.addService(source_va, .block, slot, endpoint_id);
        }
    }
    if (persistent_fs_process_slot) |slot| {
        if (persistent_fs_endpoint_id) |endpoint_id| {
            service_registry_abi.addService(source_va, .persistent_fs, slot, endpoint_id);
        }
    }
    return source_va;
}

const window_bootstrap_flag_service_registry: u64 = 1 << 0;
const window_bootstrap_flag_keyboard_shared: u64 = 1 << 1;
const window_bootstrap_flag_pointer_shared: u64 = 1 << 2;
const window_bootstrap_flag_taskbar_panel: u64 = 1 << 3;

const CachedStartupExec = struct {
    valid: bool = false,
    exec_source: startup_plan_abi.StartupExecSource = .startup_path,
    path_len: u16 = 0,
    path_bytes: [startup_plan_abi.path_max_bytes]u8 = [_]u8{0} ** startup_plan_abi.path_max_bytes,
    result: fs_client.OpenResult = .{
        .token = 0,
        .file_bytes = 0,
    },

    fn path(self: *const CachedStartupExec) []const u8 {
        return self.path_bytes[0..self.path_len];
    }
};

const LaunchContext = struct {
    client: ?vfs_client.Client = null,
    bootstrap_client: ?fs_client.Client = null,
    has_boot_display: bool,
    bootfs: BootFsArchiveView,
    primary_display: init_bootstrap_abi.DisplayDescriptor,
    window_service: service_registry_abi.ServiceEntry,
    keyboard_input: init_bootstrap_abi.DeviceDescriptor,
    keyboard_shared_page: init_bootstrap_abi.SpawnPageDescriptor,
    pointer_input: init_bootstrap_abi.DeviceDescriptor,
    pointer_shared_page: init_bootstrap_abi.SpawnPageDescriptor,
    block_device: init_bootstrap_abi.DeviceDescriptor,
    primary_panel_config_page: init_bootstrap_abi.SpawnPageDescriptor,
    primary_panel_state_page: init_bootstrap_abi.SpawnPageDescriptor,
    primary_panel_command_page: init_bootstrap_abi.SpawnPageDescriptor,
    window_service_page: init_bootstrap_abi.SpawnPageDescriptor,
    vfs_process_slot: ?u64 = null,
    vfs_endpoint_id: ?u64 = null,
    bootstrap_fs_process_slot: ?u64 = null,
    bootstrap_fs_endpoint_id: ?u64 = null,
    block_process_slot: ?u64 = null,
    block_endpoint_id: ?u64 = null,
    persistent_fs_process_slot: ?u64 = null,
    persistent_fs_endpoint_id: ?u64 = null,
    keyboard_spawned: bool = false,
    keyboard_shared_ready: bool = false,
    pointer_spawned: bool = false,
    pointer_shared_ready: bool = false,
    compositor_armed: bool = false,
    first_window_spawn_logged: bool = false,
    cached_execs: [startup_plan_abi.max_startup_program_descriptors]CachedStartupExec = [_]CachedStartupExec{.{}} ** startup_plan_abi.max_startup_program_descriptors,

    fn logRoleLine(_: *LaunchContext, action: []const u8, label: []const u8, result: []const u8) void {
        var buf: [96]u8 = undefined;
        const message = std.fmt.bufPrint(&buf, "Init: {s} {s} {s}\n", .{ action, label, result }) catch return;
        _ = userLog(message);
    }

    fn logRoleHex(_: *LaunchContext, label: []const u8, suffix: []const u8, value: u64) void {
        var buf: [96]u8 = undefined;
        const prefix = std.fmt.bufPrint(&buf, "Init: {s} {s}", .{ label, suffix }) catch return;
        userLogHex(prefix, value);
    }

    fn logClientError(_: *LaunchContext, prefix: []const u8, err: anyerror) void {
        var buf: [128]u8 = undefined;
        const message = std.fmt.bufPrint(&buf, "{s}{s}\n", .{ prefix, @errorName(err) }) catch return;
        _ = userLog(message);
    }

    fn ensureVfsClient(self: *LaunchContext) *vfs_client.Client {
        if (self.client) |*client| return client;
        const child_slot = self.vfs_process_slot orelse {
            _ = userLog("Init: VFS process slot unavailable\n");
            while (true) asm volatile ("pause");
        };
        const endpoint_id = self.vfs_endpoint_id orelse {
            _ = userLog("Init: VFS endpoint unavailable\n");
            while (true) asm volatile ("pause");
        };
        const client = vfs_client.Client.connect(.{
            .request_va = vfs_request_va,
            .response_va = vfs_response_va,
            .client_process_slot = requireProcessSlot(),
            .endpoint_id = endpoint_id,
            .server_process_slot = child_slot,
            .response_poll_limit = 8192,
        }) catch {
            _ = userLog("Init: VFS connect failed\n");
            while (true) asm volatile ("pause");
        };
        self.client = client;
        _ = userLog("Init: VFS connect done\n");
        return &(self.client.?);
    }

    fn ensureBootstrapFsClient(self: *LaunchContext) ?*fs_client.Client {
        if (self.bootstrap_client) |*client| return client;
        const child_slot = self.bootstrap_fs_process_slot orelse return null;
        const endpoint_id = self.bootstrap_fs_endpoint_id orelse return null;
        const client = fs_client.Client.connect(.{
            .request_va = bootstrap_fs_request_va,
            .response_va = bootstrap_fs_response_va,
            .client_process_slot = requireProcessSlot(),
            .endpoint_id = endpoint_id,
            .server_process_slot = child_slot,
            .response_poll_limit = 8192,
        }) catch return null;
        self.bootstrap_client = client;
        _ = userLog("Init: bootstrap fs connect done\n");
        return &(self.bootstrap_client.?);
    }

    fn requireExecFromBootFs(self: *LaunchContext, path: []const u8, label: []const u8) fs_client.OpenResult {
        const image = self.bootfs.findRegularFile(path) orelse {
            var buf: [96]u8 = undefined;
            const msg = std.fmt.bufPrint(&buf, "Init: bootfs exec {s} missing\n", .{label}) catch "Init: bootfs exec missing\n";
            _ = userLog(msg);
            while (true) asm volatile ("pause");
        };
        const vm_token = installVmObject(@intFromPtr(image.ptr), image.len, .{ .read = true });
        if (image_abi.decodeVmObjectToken(vm_token) == null) {
            var buf: [96]u8 = undefined;
            const msg = std.fmt.bufPrint(&buf, "Init: bootfs vm install {s} failed\n", .{label}) catch "Init: bootfs vm install failed\n";
            _ = userLog(msg);
            while (true) asm volatile ("pause");
        }
        const exec_token = installExecImage(vm_token, .{ .exec = true });
        if (image_abi.decodeExecImageToken(exec_token) == null) {
            var buf: [96]u8 = undefined;
            const msg = std.fmt.bufPrint(&buf, "Init: bootfs exec install {s} failed\n", .{label}) catch "Init: bootfs exec install failed\n";
            _ = userLog(msg);
            while (true) asm volatile ("pause");
        }
        return .{
            .token = exec_token,
            .file_bytes = image.len,
        };
    }

    fn requireExecFromBootstrapFs(self: *LaunchContext, path: []const u8, label: []const u8) ?fs_client.OpenResult {
        const client = self.ensureBootstrapFsClient() orelse return null;
        const file = client.lookup(client.mount_token, path) catch |err| {
            self.logRoleLine("lookup", label, "failed");
            self.logClientError("Init: bootstrap fs lookup err=", err);
            return null;
        };
        self.logRoleLine("lookup", label, "ok");
        const exec = client.openExec(file.token) catch |err| {
            self.logRoleLine("open_exec", label, "failed");
            self.logClientError("Init: bootstrap fs open_exec err=", err);
            return null;
        };
        self.logRoleLine("open_exec", label, "ok");
        return exec;
    }

    fn findCachedExec(self: *LaunchContext, exec_source: startup_plan_abi.StartupExecSource, path: []const u8) ?fs_client.OpenResult {
        for (&self.cached_execs) |*entry| {
            if (!entry.valid) continue;
            if (entry.exec_source != exec_source) continue;
            if (std.mem.eql(u8, entry.path(), path)) return entry.result;
        }
        return null;
    }

    fn storeCachedExec(self: *LaunchContext, exec_source: startup_plan_abi.StartupExecSource, path: []const u8, result: fs_client.OpenResult) void {
        if (path.len > startup_plan_abi.path_max_bytes) {
            _ = userLog("Init: cached exec path too long\n");
            while (true) asm volatile ("pause");
        }
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
        _ = userLog("Init: cached exec table full\n");
        while (true) asm volatile ("pause");
    }

    fn fetchExecForStartupSource(self: *LaunchContext, exec_source: startup_plan_abi.StartupExecSource, path: []const u8, label: []const u8) fs_client.OpenResult {
        return switch (exec_source) {
            .bootfs => self.requireExecFromBootFs(path, label),
            .startup_path => blk: {
                if (self.bootstrap_fs_process_slot != null and self.bootstrap_fs_endpoint_id != null) {
                    break :blk self.requireExecFromBootstrapFs(path, label) orelse {
                        _ = userLog("Init: bootstrap fs exec failed\n");
                        while (true) asm volatile ("pause");
                    };
                }
                break :blk self.requireExecFromBootFs(path, label);
            },
        };
    }

    fn cacheExecForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        if (self.findCachedExec(policy.exec_source, policy.path) != null) return;
        const exec = self.fetchExecForStartupSource(policy.exec_source, policy.path, policy.label);
        self.storeCachedExec(policy.exec_source, policy.path, exec);
    }

    fn requireExecForPolicy(self: *LaunchContext, policy: StartupPolicy) fs_client.OpenResult {
        if (self.findCachedExec(policy.exec_source, policy.path)) |exec| return exec;
        return self.fetchExecForStartupSource(policy.exec_source, policy.path, policy.label);
    }

    fn requireLookup(self: *LaunchContext, path: []const u8, failed_message: []const u8, ok_message: []const u8) vfs_client.LookupResult {
        const client = self.ensureVfsClient();
        const file = lookupFileWithRetry(client, path) orelse {
            _ = userLog(failed_message);
            while (true) asm volatile ("pause");
        };
        _ = userLog(ok_message);
        return file;
    }

    fn requireOpenExec(self: *LaunchContext, token: u64, failed_message: []const u8, ok_message: []const u8) vfs_client.OpenResult {
        const client = self.ensureVfsClient();
        const exec = client.openExec(token) catch |err| {
            _ = userLog(failed_message);
            self.logClientError("Init: open_exec err=", err);
            while (true) asm volatile ("pause");
        };
        _ = userLog(ok_message);
        return exec;
    }

    fn requireBootDisplay(self: *LaunchContext) void {
        if (self.has_boot_display) return;

        _ = userLog("Init: spawning shell from bootfs\n");

        const fb_paddr = self.primary_display.framebuffer_paddr;
        const fb_size = self.primary_display.framebuffer_size_bytes;
        if (fb_paddr == 0 or fb_size == 0) {
            _ = userLog("Init: framebuffer paddr or size missing\n");
            while (true) asm volatile ("pause");
        }
        const shell_fb_height = @min(self.primary_display.height, init_bootstrap_abi.boot_display_shell_height);
        const shell_fb_size = @min(fb_size, self.primary_display.pitch * shell_fb_height * 4);

        // Get exec from bootfs.
        const exec = self.requireExecFromBootFs("/bin/shell.elf", "shell");

        // Alloc a writable config page for boot_display.
        const config_source_va = self.allocWritableBootstrapPage("Init: alloc shell config page failed\n");
        const keyboard_desc = self.inputDescriptorForKind(.keyboard);
        input_bootstrap.writeKeyboardConfigPage(config_source_va, .{
            .common_page_paddr = keyboard_desc.common_page_paddr,
            .notify_page_paddr = keyboard_desc.notify_page_paddr,
            .isr_page_paddr = keyboard_desc.isr_page_paddr,
            .device_page_paddr = keyboard_desc.device_page_paddr,
            .common_page_offset = keyboard_desc.common_page_offset,
            .notify_page_offset = keyboard_desc.notify_page_offset,
            .isr_page_offset = keyboard_desc.isr_page_offset,
            .device_page_offset = keyboard_desc.device_page_offset,
            .notify_off_multiplier = keyboard_desc.notify_off_multiplier,
        });
        const config_words: [*]volatile u64 = @ptrFromInt(config_source_va);
        config_words[init_bootstrap_abi.boot_display_config_fb_paddr_index] = fb_paddr;
        config_words[init_bootstrap_abi.boot_display_config_fb_size_bytes_index] = shell_fb_size;
        const fb_vm_token = installVmObjectMmioRange(fb_paddr, shell_fb_size, .{ .read = true, .write = true, .map = true, .grant = true });
        if (image_abi.decodeVmObjectToken(fb_vm_token) == null) {
            _ = userLog("Init: framebuffer vm object install failed\n");
            while (true) asm volatile ("pause");
        }
        config_words[init_bootstrap_abi.boot_display_config_fb_vm_token_index] = fb_vm_token;

        // Build extended bootstrap descriptor table.
        boot_display_bootstrap_table_storage = .{};
        boot_display_bootstrap_table_storage.page_descriptors[0] = .{
            .source_va = config_source_va,
            .target_va = process_abi.standard_config_target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        boot_display_bootstrap_table_storage.page_descriptors[1] = .{
            .source_va = self.window_service_page.source_va,
            .target_va = process_abi.service_registry_shadow_va,
            .flags = 0,
        };
        boot_display_bootstrap_table_storage.page_count = 2;
        boot_display_bootstrap_table_storage.cap_descriptors[0] = .{
            .source_token = fb_vm_token,
            .target_token_va = process_abi.standard_config_target_va +
                init_bootstrap_abi.boot_display_config_fb_vm_token_index * 8,
            .rights_bits = image_abi.vmObjectRightsToBits(.{ .read = true, .write = true, .map = true }),
            .kind = .vm_object,
        };
        boot_display_bootstrap_table_storage.cap_count = 1;

        const result = spawnExecWithExtendedBootstrapTable(exec.token, &boot_display_bootstrap_table_storage);
        const boot_display_slot = process_abi.decodeSpawnedProcessSlot(result) orelse {
            _ = userLog("Init: shell spawn failed\n");
            while (true) asm volatile ("pause");
        };
        if (!grantInputResources(config_source_va, keyboard_desc, boot_display_slot)) {
            _ = userLog("Init: shell keyboard grant failed\n");
            while (true) asm volatile ("pause");
        }
        const endpoint_id = allocDynamicServiceEndpointId();

        // Record boot_display's process slot and dynamically assigned endpoint
        // so window clients can be given a service registry pointing to it.
        self.window_service = .{
            .kind = @intFromEnum(service_registry_abi.ServiceKind.window),
            .process_slot = boot_display_slot,
            .endpoint_id = endpoint_id,
            .flags = 0,
        };
        self.has_boot_display = true;
        _ = userLog("Init: shell spawned\n");
    }

    fn inputDescriptorForKind(self: *LaunchContext, kind: InputDeviceKind) init_bootstrap_abi.DeviceDescriptor {
        return switch (kind) {
            .keyboard => self.keyboard_input,
            .pointer => self.pointer_input,
        };
    }

    fn blockDescriptorForKind(self: *LaunchContext, kind: BlockDeviceKind) init_bootstrap_abi.DeviceDescriptor {
        _ = kind;
        return self.block_device;
    }

    fn inputSharedPageForKind(self: *LaunchContext, kind: InputDeviceKind) init_bootstrap_abi.SpawnPageDescriptor {
        return switch (kind) {
            .keyboard => self.keyboard_shared_page,
            .pointer => self.pointer_shared_page,
        };
    }

    fn writeInputConfigForKind(self: *LaunchContext, kind: InputDeviceKind, descriptor: init_bootstrap_abi.DeviceDescriptor) void {
        const shared_desc = self.inputSharedPageForKind(kind);
        switch (kind) {
            .keyboard => input_bootstrap.writeKeyboardConfigPage(descriptor.bootstrap_source_va, .{
                .common_page_paddr = descriptor.common_page_paddr,
                .notify_page_paddr = descriptor.notify_page_paddr,
                .isr_page_paddr = descriptor.isr_page_paddr,
                .device_page_paddr = descriptor.device_page_paddr,
                .common_page_offset = descriptor.common_page_offset,
                .notify_page_offset = descriptor.notify_page_offset,
                .isr_page_offset = descriptor.isr_page_offset,
                .device_page_offset = descriptor.device_page_offset,
                .notify_off_multiplier = descriptor.notify_off_multiplier,
                .shared_target_va = shared_desc.target_va,
            }),
            .pointer => input_bootstrap.writeMouseConfigPage(descriptor.bootstrap_source_va, .{
                .common_page_paddr = descriptor.common_page_paddr,
                .notify_page_paddr = descriptor.notify_page_paddr,
                .isr_page_paddr = descriptor.isr_page_paddr,
                .device_page_paddr = descriptor.device_page_paddr,
                .common_page_offset = descriptor.common_page_offset,
                .notify_page_offset = descriptor.notify_page_offset,
                .isr_page_offset = descriptor.isr_page_offset,
                .device_page_offset = descriptor.device_page_offset,
                .notify_off_multiplier = descriptor.notify_off_multiplier,
                .screen_width = self.primary_display.width,
                .screen_height = self.primary_display.height,
                .screen_pitch = self.primary_display.pitch,
                .shared_target_va = shared_desc.target_va,
            }),
        }
    }

    fn allocWritableBootstrapPage(self: *LaunchContext, label: []const u8) u64 {
        _ = self;
        const source_va = allocDynamicBootstrapSourceVa();
        var paddr: u64 = 0;
        if (allocMapPages(source_va, 1, true, @intFromPtr(&paddr)) != 0 or paddr < 0x1000) {
            _ = userLog(label);
            while (true) asm volatile ("pause");
        }
        return source_va;
    }

    fn validateInputSharedPage(self: *LaunchContext, kind: InputDeviceKind, shared_desc: init_bootstrap_abi.SpawnPageDescriptor) void {
        _ = self;
        const words: [*]const volatile u64 = @ptrFromInt(shared_desc.source_va);
        switch (kind) {
            .keyboard => if (words[0] != keyboard_shared_magic) {
                _ = userLog("Init: keyboard shared magic mismatch\n");
                while (true) asm volatile ("pause");
            },
            .pointer => if (words[0] != mouse_shared_magic) {
                _ = userLog("Init: mouse shared magic mismatch\n");
                while (true) asm volatile ("pause");
            },
        }
    }

    fn inputSharedPaddrFromConfig(self: *LaunchContext, kind: InputDeviceKind) u64 {
        const descriptor = self.inputDescriptorForKind(kind);
        const words: [*]const volatile u64 = @ptrFromInt(descriptor.bootstrap_source_va);
        return words[input_shared_page_paddr_index];
    }

    fn finishInputSharedPage(self: *LaunchContext, kind: InputDeviceKind, shared_paddr: u64) void {
        const shared_desc = self.inputSharedPageForKind(kind);
        if (mapPage(shared_desc.source_va, shared_paddr, false) != 0) {
            _ = userLog("Init: input shared page map failed\n");
            while (true) asm volatile ("pause");
        }
        self.validateInputSharedPage(kind, shared_desc);
        switch (kind) {
            .keyboard => {
                self.keyboard_shared_ready = true;
                noteBootStatus(boot_status_abi.status_init_keyboard_shared_ready);
            },
            .pointer => {
                self.pointer_shared_ready = true;
                noteBootStatus(boot_status_abi.status_init_mouse_shared_ready);
                if (installEndpoint(self.window_service.endpoint_id, self.window_service.process_slot) != 0) {
                    _ = userLog("Init: install boot display endpoint failed\n");
                    while (true) asm volatile ("pause");
                }
                if (shareCap(shared_paddr, self.window_service.endpoint_id) != 0) {
                    _ = userLog("Init: send mouse shared page to boot display failed\n");
                    while (true) asm volatile ("pause");
                }
                _ = userLog("Init: send mouse shared page to boot display ok\n");
            },
        }
    }

    fn tryFinishInputSharedFromPaddr(self: *LaunchContext, shared_paddr: u64) bool {
        if (self.keyboard_spawned and !self.keyboard_shared_ready and self.inputSharedPaddrFromConfig(.keyboard) == shared_paddr) {
            self.finishInputSharedPage(.keyboard, shared_paddr);
            return true;
        }
        if (self.pointer_spawned and !self.pointer_shared_ready and self.inputSharedPaddrFromConfig(.pointer) == shared_paddr) {
            self.finishInputSharedPage(.pointer, shared_paddr);
            return true;
        }
        return false;
    }

    fn finishPendingInputSharedCaps(self: *LaunchContext) void {
        while ((self.keyboard_spawned and !self.keyboard_shared_ready) or
            (self.pointer_spawned and !self.pointer_shared_ready))
        {
            const shared_paddr = waitRecvCap();
            if (shared_paddr < cap_transfer_abi.transfer_id_min) continue;
            if (!self.tryFinishInputSharedFromPaddr(shared_paddr)) {
                _ = userLog("Init: unexpected input shared page\n");
                while (true) asm volatile ("pause");
            }
        }
    }

    fn appendBootstrapPage(
        _: *LaunchContext,
        out: []process_abi.BootstrapPageDescriptor,
        count: *usize,
        source_va: u64,
        target_va: u64,
        flags: u64,
    ) void {
        out[count.*] = .{
            .source_va = source_va,
            .target_va = target_va,
            .flags = flags,
        };
        count.* += 1;
    }

    fn ensurePolicyResources(self: *LaunchContext, policy: StartupPolicy) void {
        if ((policy.ensure_flags & startup_plan_abi.ensure_flag_boot_display) != 0) {
            self.requireBootDisplay();
        }
    }

    fn currentReadyFlags(self: *const LaunchContext) u64 {
        var flags: u64 = 0;
        if (self.keyboard_shared_ready) flags |= startup_plan_abi.require_flag_keyboard_shared;
        if (self.pointer_shared_ready) flags |= startup_plan_abi.require_flag_pointer_shared;
        if (self.block_process_slot != null and self.block_endpoint_id != null) {
            flags |= startup_plan_abi.require_flag_block_service;
        }
        if (self.bootstrap_fs_process_slot != null and self.bootstrap_fs_endpoint_id != null) {
            flags |= startup_plan_abi.require_flag_bootstrap_fs_service;
        }
        if (self.persistent_fs_process_slot != null and self.persistent_fs_endpoint_id != null) {
            flags |= startup_plan_abi.require_flag_persistent_fs_service;
        }
        if (self.compositor_armed) flags |= startup_plan_abi.require_flag_compositor_armed;
        return flags;
    }

    fn policyReady(self: *const LaunchContext, policy: StartupPolicy) bool {
        const ready_flags = self.currentReadyFlags();
        return (ready_flags & policy.require_flags) == policy.require_flags;
    }

    fn pendingInputSharedWait(self: *const LaunchContext) bool {
        return (self.keyboard_spawned and !self.keyboard_shared_ready) or
            (self.pointer_spawned and !self.pointer_shared_ready);
    }

    fn prepareWindowBootstrapForPolicy(
        self: *LaunchContext,
        policy: StartupPolicy,
        out: []process_abi.BootstrapPageDescriptor,
    ) []const process_abi.BootstrapPageDescriptor {
        var count: usize = 0;
        const registry_target_va = self.window_service_page.target_va;
        var registry_source_va: u64 = 0;
        if ((policy.window_flags & window_bootstrap_flag_taskbar_panel) != 0) {
            if ((policy.window_flags & window_bootstrap_flag_service_registry) != 0 and registry_source_va == 0) {
                registry_source_va = allocChildServiceRegistryPage(
                    self.window_service,
                    self.vfs_process_slot,
                    self.vfs_endpoint_id,
                    self.bootstrap_fs_process_slot,
                    self.bootstrap_fs_endpoint_id,
                    self.block_process_slot,
                    self.block_endpoint_id,
                    self.persistent_fs_process_slot,
                    self.persistent_fs_endpoint_id,
                ) orelse {
                    self.logRoleLine("alloc", policy.label, "window service registry failed");
                    while (true) asm volatile ("pause");
                };
            }
            taskbar_bootstrap.writeConfigPage(
                self.primary_panel_config_page.source_va,
                self.primary_display.width,
                self.primary_display.height,
                0,
                registry_target_va,
                self.pointer_shared_page.target_va,
                self.primary_panel_state_page.target_va,
                self.primary_panel_command_page.target_va,
            );
            taskbar_bootstrap.initStatePage(self.primary_panel_state_page.source_va);
            taskbar_bootstrap.initCommandPage(self.primary_panel_command_page.source_va);
            self.appendBootstrapPage(out, &count, self.primary_panel_config_page.source_va, self.primary_panel_config_page.target_va, self.primary_panel_config_page.spawn_flags);
            self.appendBootstrapPage(out, &count, self.primary_panel_state_page.source_va, self.primary_panel_state_page.target_va, self.primary_panel_state_page.spawn_flags);
            self.appendBootstrapPage(out, &count, self.primary_panel_command_page.source_va, self.primary_panel_command_page.target_va, self.primary_panel_command_page.spawn_flags);
        }
        if ((policy.window_flags & window_bootstrap_flag_keyboard_shared) != 0) {
            self.appendBootstrapPage(out, &count, self.keyboard_shared_page.source_va, self.keyboard_shared_page.target_va, self.keyboard_shared_page.spawn_flags);
        }
        if ((policy.window_flags & window_bootstrap_flag_pointer_shared) != 0) {
            self.appendBootstrapPage(out, &count, self.pointer_shared_page.source_va, self.pointer_shared_page.target_va, self.pointer_shared_page.spawn_flags);
        }
        if ((policy.window_flags & window_bootstrap_flag_service_registry) != 0) {
            if (registry_source_va == 0) {
                registry_source_va = allocChildServiceRegistryPage(
                    self.window_service,
                    self.vfs_process_slot,
                    self.vfs_endpoint_id,
                    self.bootstrap_fs_process_slot,
                    self.bootstrap_fs_endpoint_id,
                    self.block_process_slot,
                    self.block_endpoint_id,
                    self.persistent_fs_process_slot,
                    self.persistent_fs_endpoint_id,
                ) orelse {
                    self.logRoleLine("alloc", policy.label, "window service registry failed");
                    while (true) asm volatile ("pause");
                };
            }
            const config_kind = policy.window_config_kind orelse unreachable;
            switch (config_kind) {
                .terminal => {
                    const config_source_va = self.allocWritableBootstrapPage("Init: alloc terminal config page failed\n");
                    terminal_bootstrap.writeConfigPage(
                        config_source_va,
                        registry_target_va,
                        self.keyboard_shared_page.target_va,
                    );
                    self.appendBootstrapPage(
                        out,
                        &count,
                        config_source_va,
                        process_abi.standard_config_target_va,
                        process_abi.spawn_flag_bootstrap_page_writable,
                    );
                },
                .taskbar => {},
                .mouse_demo => {
                    const config_source_va = self.allocWritableBootstrapPage("Init: alloc mouse demo config page failed\n");
                    mouse_demo_bootstrap.writeConfigPage(
                        config_source_va,
                        registry_target_va,
                        self.pointer_shared_page.target_va,
                    );
                    self.appendBootstrapPage(
                        out,
                        &count,
                        config_source_va,
                        process_abi.standard_config_target_va,
                        process_abi.spawn_flag_bootstrap_page_writable,
                    );
                },
            }
            self.appendBootstrapPage(out, &count, registry_source_va, self.window_service_page.target_va, self.window_service_page.spawn_flags);
        }
        return out[0..count];
    }

    fn launchVfsForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        const exec = self.requireExecForPolicy(policy);
        const exec_token = exec.token;

        const vfs_config_source_va = allocDynamicBootstrapSourceVa();
        var cfg_paddr: u64 = 0;
        if (allocMapPages(vfs_config_source_va, 1, true, @intFromPtr(&cfg_paddr)) != 0 or cfg_paddr < 0x1000) {
            self.logRoleLine("alloc", policy.label, "config page failed");
            while (true) asm volatile ("pause");
        }
        const vfs_endpoint_id = allocDynamicServiceEndpointId();
        const words: [*]volatile u64 = @ptrFromInt(vfs_config_source_va);
        var i: usize = 0;
        while (i < 512) : (i += 1) words[i] = 0;
        words[0] = vfs_boot_config_magic;
        words[1] = vfs_boot_config_version;
        words[2] = vfs_endpoint_id;
        words[3] = 0;
        words[4] = 0;
        words[5] = self.bootfs.size_bytes;
        words[6] = vfs_boot_config_flag_bootfs_present;

        var bootstrap_count: usize = 0;
        vfs_bootstrap_pages_storage[bootstrap_count] = .{
            .source_va = vfs_config_source_va,
            .target_va = process_abi.standard_config_target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        bootstrap_count += 1;
        vfs_bootstrap_table_storage = .{};
        vfs_bootstrap_table_storage.page_count = @intCast(bootstrap_count);
        vfs_bootstrap_table_storage.cap_count = 1;
        vfs_bootstrap_table_storage.page_descriptors[0] = vfs_bootstrap_pages_storage[0];
        vfs_bootstrap_table_storage.cap_descriptors[0] = .{
            .source_token = ensureBootFsVmObjectCache(&self.bootfs),
            .target_token_va = process_abi.standard_config_target_va + 4 * 8,
            .rights_bits = image_abi.vmObjectRightsToBits(.{
                .read = true,
                .map = true,
            }),
            .kind = .vm_object,
        };
        const spawned = spawnExecWithExtendedBootstrapTable(exec_token, &vfs_bootstrap_table_storage);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", policy.label, "failed");
            self.logRoleHex(policy.label, " spawn ret=", spawned);
            while (true) asm volatile ("pause");
        };
        self.logRoleLine("spawn", policy.label, "ok");
        _ = userLog("Init: VFS spawn done\n");
        self.vfs_process_slot = child_slot;
        self.vfs_endpoint_id = vfs_endpoint_id;
        if (installEndpoint(vfs_endpoint_id, child_slot) != 0) {
            _ = userLog("Init: install VFS endpoint failed\n");
            while (true) asm volatile ("pause");
        }
    }

    fn launchInputDriverForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        const input_kind = policy.input_kind orelse unreachable;
        const exec = self.requireExecForPolicy(policy);

        const input_desc = self.inputDescriptorForKind(input_kind);
        self.writeInputConfigForKind(input_kind, input_desc);

        const spawned = spawnExec(
            exec.token,
            input_desc.bootstrap_source_va,
            process_abi.standard_config_target_va,
            process_abi.spawn_flag_bootstrap_page_writable,
        );
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", policy.label, "failed");
            self.logRoleHex(policy.label, " spawn ret=", spawned);
            while (true) asm volatile ("pause");
        };
        self.logRoleLine("spawn", policy.label, "ok");
        switch (input_kind) {
            .keyboard => {
                self.keyboard_spawned = true;
                noteBootStatus(boot_status_abi.status_init_keyboard_spawn_done);
            },
            .pointer => {
                self.pointer_spawned = true;
                noteBootStatus(boot_status_abi.status_init_mouse_spawn_done);
            },
        }
        if (!grantInputDriverResources(input_desc, child_slot)) {
            self.logRoleLine("grant", policy.label, "resources failed");
            while (true) asm volatile ("pause");
        }
        self.logRoleLine("grant", policy.label, "resources ok");
    }

    fn launchBlockDriverForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        const block_kind = policy.block_kind orelse unreachable;
        const exec = self.requireExecForPolicy(policy);
        const block_desc = self.blockDescriptorForKind(block_kind);
        const block_geometry = readBlockGeometry(block_desc) orelse {
            self.logRoleLine("inspect", policy.label, "block geometry failed");
            while (true) asm volatile ("pause");
        };
        const config_source_va = self.allocWritableBootstrapPage("Init: alloc block driver config page failed\n");

        const endpoint_id = allocDynamicServiceEndpointId();
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
        });

        block_bootstrap_pages_storage[0] = .{
            .source_va = config_source_va,
            .target_va = process_abi.standard_config_target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        block_bootstrap_table_storage = .{};
        block_bootstrap_table_storage.page_count = 1;
        block_bootstrap_table_storage.cap_count = 0;
        block_bootstrap_table_storage.page_descriptors[0] = block_bootstrap_pages_storage[0];

        const spawned = spawnExecWithExtendedBootstrapTable(exec.token, &block_bootstrap_table_storage);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", policy.label, "failed");
            self.logRoleHex(policy.label, " spawn ret=", spawned);
            while (true) asm volatile ("pause");
        };
        self.logRoleLine("spawn", policy.label, "ok");
        if (installEndpoint(endpoint_id, child_slot) != 0) {
            self.logRoleLine("endpoint", policy.label, "install failed");
            while (true) asm volatile ("pause");
        }
        if (!grantBlockDriverResources(config_source_va, block_desc, child_slot)) {
            self.logRoleLine("grant", policy.label, "resources failed");
            while (true) asm volatile ("pause");
        }
        self.logRoleLine("grant", policy.label, "resources ok");
        _ = signalEndpoint(endpoint_id);
        service_registry_abi.addService(self.window_service_page.source_va, .block, child_slot, endpoint_id);
        self.block_process_slot = child_slot;
        self.block_endpoint_id = endpoint_id;
    }

    fn launchBootstrapFsForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        const exec = self.requireExecForPolicy(policy);
        const block_desc = self.blockDescriptorForKind(.virtio_blk);
        const block_geometry = readBlockGeometry(block_desc) orelse {
            self.logRoleLine("inspect", policy.label, "block geometry failed");
            while (true) asm volatile ("pause");
        };
        const endpoint_id = allocDynamicServiceEndpointId();
        const config_source_va = self.allocWritableBootstrapPage("Init: alloc bootstrap fs config page failed\n");
        bootstrap_fs_bootstrap.writeConfigPage(
            config_source_va,
            endpoint_id,
            .{
                .rootfs_start_block = persistent_fs_start_block,
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
            },
        );

        bootstrap_fs_bootstrap_table_storage = .{};
        bootstrap_fs_bootstrap_table_storage.page_count = 1;
        bootstrap_fs_bootstrap_table_storage.cap_count = 0;
        bootstrap_fs_bootstrap_pages_storage[0] = .{
            .source_va = config_source_va,
            .target_va = process_abi.standard_config_target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        bootstrap_fs_bootstrap_table_storage.page_descriptors[0] = bootstrap_fs_bootstrap_pages_storage[0];

        const spawned = spawnExecWithExtendedBootstrapTable(exec.token, &bootstrap_fs_bootstrap_table_storage);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", policy.label, "failed");
            self.logRoleHex(policy.label, " spawn ret=", spawned);
            while (true) asm volatile ("pause");
        };
        self.logRoleLine("spawn", policy.label, "ok");
        if (installEndpoint(endpoint_id, child_slot) != 0) {
            self.logRoleLine("endpoint", policy.label, "install failed");
            while (true) asm volatile ("pause");
        }
        if (!grantBootstrapFsBlockResources(config_source_va, block_desc, child_slot)) {
            self.logRoleLine("grant", policy.label, "block resources failed");
            while (true) asm volatile ("pause");
        }
        _ = signalEndpoint(endpoint_id);
        service_registry_abi.addService(self.window_service_page.source_va, .bootstrap_fs, child_slot, endpoint_id);
        self.bootstrap_fs_process_slot = child_slot;
        self.bootstrap_fs_endpoint_id = endpoint_id;
    }

    fn launchPersistentFsForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        if (self.block_process_slot == null or self.block_endpoint_id == null) {
            self.logRoleLine("bootstrap", policy.label, "block service missing");
            while (true) asm volatile ("pause");
        }
        const exec = self.requireExecForPolicy(policy);
        const endpoint_id = allocDynamicServiceEndpointId();
        const registry_source_va = allocChildServiceRegistryPage(
            self.window_service,
            self.vfs_process_slot,
            self.vfs_endpoint_id,
            self.bootstrap_fs_process_slot,
            self.bootstrap_fs_endpoint_id,
            self.block_process_slot,
            self.block_endpoint_id,
            null,
            null,
        ) orelse {
            self.logRoleLine("alloc", policy.label, "service registry failed");
            while (true) asm volatile ("pause");
        };
        const config_source_va = self.allocWritableBootstrapPage("Init: alloc persistent fs config page failed\n");
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
        persistent_fs_bootstrap_table_storage = .{};
        persistent_fs_bootstrap_table_storage.page_count = 2;
        persistent_fs_bootstrap_table_storage.cap_count = 0;
        persistent_fs_bootstrap_table_storage.page_descriptors[0] = persistent_fs_bootstrap_pages_storage[0];
        persistent_fs_bootstrap_table_storage.page_descriptors[1] = persistent_fs_bootstrap_pages_storage[1];
        const spawned = spawnExecWithExtendedBootstrapTable(exec.token, &persistent_fs_bootstrap_table_storage);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", policy.label, "failed");
            self.logRoleHex(policy.label, " spawn ret=", spawned);
            while (true) asm volatile ("pause");
        };
        self.logRoleLine("spawn", policy.label, "ok");
        if (installEndpoint(endpoint_id, child_slot) != 0) {
            self.logRoleLine("endpoint", policy.label, "install failed");
            while (true) asm volatile ("pause");
        }
        service_registry_abi.addService(self.window_service_page.source_va, .persistent_fs, child_slot, endpoint_id);
        self.persistent_fs_process_slot = child_slot;
        self.persistent_fs_endpoint_id = endpoint_id;
    }

    fn launchBlockDemoForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        if (self.block_process_slot == null or self.block_endpoint_id == null) {
            self.logRoleLine("bootstrap", policy.label, "block service missing");
            while (true) asm volatile ("pause");
        }
        const exec = self.requireExecForPolicy(policy);
        const registry_source_va = allocChildServiceRegistryPage(
            self.window_service,
            self.vfs_process_slot,
            self.vfs_endpoint_id,
            self.bootstrap_fs_process_slot,
            self.bootstrap_fs_endpoint_id,
            self.block_process_slot,
            self.block_endpoint_id,
            self.persistent_fs_process_slot,
            self.persistent_fs_endpoint_id,
        ) orelse {
            self.logRoleLine("alloc", policy.label, "service registry failed");
            while (true) asm volatile ("pause");
        };
        const config_source_va = self.allocWritableBootstrapPage("Init: alloc block demo config page failed\n");
        block_demo_bootstrap.writeConfigPage(config_source_va, 0);

        var bootstrap_pages: [2]process_abi.BootstrapPageDescriptor = undefined;
        bootstrap_pages[0] = .{
            .source_va = config_source_va,
            .target_va = process_abi.standard_config_target_va,
            .flags = process_abi.spawn_flag_bootstrap_page_writable,
        };
        bootstrap_pages[1] = .{
            .source_va = registry_source_va,
            .target_va = self.window_service_page.target_va,
            .flags = self.window_service_page.spawn_flags,
        };
        const spawned = spawnExecWithBootstrapPages(exec.token, bootstrap_pages[0..]);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", policy.label, "failed");
            self.logRoleHex(policy.label, " spawn ret=", spawned);
            while (true) asm volatile ("pause");
        };
        block_demo_bootstrap.writeProcessSlot(config_source_va, child_slot);
        self.logRoleLine("spawn", policy.label, "ok");
    }

    fn launchWindowClientForPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        const exec = self.requireExecForPolicy(policy);

        var bootstrap_pages: [5]process_abi.BootstrapPageDescriptor = undefined;
        const bootstrap_slice = self.prepareWindowBootstrapForPolicy(policy, bootstrap_pages[0..]);
        const spawned = spawnExecWithBootstrapPages(exec.token, bootstrap_slice);
        if (process_abi.decodeSpawnedProcessSlot(spawned) == null) {
            self.logRoleLine("spawn", policy.label, "failed");
            self.logRoleHex(policy.label, " spawn ret=", spawned);
            while (true) asm volatile ("pause");
        }
        const child_thread = process_abi.decodeSpawnedThreadSlot(spawned);
        const is_first_window = !self.first_window_spawn_logged;
        self.logRoleLine("spawn", policy.label, "ok");
        if (is_first_window) {
            self.first_window_spawn_logged = true;
            noteBootStatus(boot_status_abi.status_init_first_window_spawn_done);
            return;
        }
        if (child_thread) |thread_index| {
            _ = switchThread(thread_index);
        }
    }

    fn armCompositor(self: *LaunchContext, classic: StartupPolicy, gpu: StartupPolicy) void {
        const compositor_exec = self.requireExecForPolicy(classic);
        const gpu_compositor_exec = self.requireExecForPolicy(gpu);

        const ret = armDeferredCompositor(compositor_exec.token, gpu_compositor_exec.token, self.window_service.process_slot);
        if (ret != 0) {
            _ = userLog("Init: arm deferred compositor failed\n");
            userLogHex("Init: arm deferred compositor ret=", ret);
            while (true) asm volatile ("pause");
        }
        self.compositor_armed = true;
        noteBootStatus(boot_status_abi.status_init_compositor_arm_done);
    }

    fn launchPolicy(self: *LaunchContext, policy: StartupPolicy) void {
        self.ensurePolicyResources(policy);
        switch (policy.action) {
            .vfs => self.launchVfsForPolicy(policy),
            .bootstrap_fs_server => self.launchBootstrapFsForPolicy(policy),
            .input_driver => self.launchInputDriverForPolicy(policy),
            .block_driver => self.launchBlockDriverForPolicy(policy),
            .persistent_fs_server => self.launchPersistentFsForPolicy(policy),
            .block_client => self.launchBlockDemoForPolicy(policy),
            .window_client => self.launchWindowClientForPolicy(policy),
            .deferred_compositor => unreachable,
        }
    }
};

const DeferredCompositorPolicies = struct {
    classic: ?StartupPolicy = null,
    gpu: ?StartupPolicy = null,
};

var launch_ctx_storage: LaunchContext = undefined;

fn runStartupManifest(ctx: *LaunchContext, manifest: *const StartupManifest) void {
    noteBootStatus(boot_status_abi.status_init_manifest_begin);
    var nodes: [startup_plan_abi.max_startup_program_descriptors]StartupNode = undefined;
    var node_count: usize = 0;
    var lines = std.mem.tokenizeScalar(u8, manifest.slice(), '\n');
    while (lines.next()) |raw_line| {
        const policy = parseStartupManifestLine(raw_line) orelse continue;
        if (node_count >= nodes.len) {
            _ = userLog("Init: too many startup manifest entries\n");
            while (true) asm volatile ("pause");
        }
        nodes[node_count] = .{
            .policy = policy,
            .launched = false,
        };
        node_count += 1;
    }

    // Preload exec images before later services can take over bootstrap-owned
    // resources such as the rootfs block queue.
    var preload_index: usize = 0;
    while (preload_index < node_count) : (preload_index += 1) {
        ctx.cacheExecForPolicy(nodes[preload_index].policy);
    }

    var dispatched_count: usize = 0;
    var compositor_policies = DeferredCompositorPolicies{};
    while (dispatched_count < node_count) {
        var progress = false;
        var node_index: usize = 0;
        while (node_index < node_count) : (node_index += 1) {
            const node = &nodes[node_index];
            if (node.launched) continue;
            if (!ctx.policyReady(node.policy)) continue;
            switch (node.policy.action) {
                .deferred_compositor => {
                    ctx.ensurePolicyResources(node.policy);
                    ctx.cacheExecForPolicy(node.policy);
                    switch (node.policy.compositor_variant orelse unreachable) {
                        .classic => compositor_policies.classic = node.policy,
                        .gpu => compositor_policies.gpu = node.policy,
                    }
                },
                else => ctx.launchPolicy(node.policy),
            }
            node.launched = true;
            dispatched_count += 1;
            progress = true;
        }
        if (!ctx.compositor_armed and compositor_policies.classic != null and compositor_policies.gpu != null) {
            ctx.armCompositor(compositor_policies.classic.?, compositor_policies.gpu.?);
            progress = true;
        }
        if (dispatched_count >= node_count) break;
        if (progress) continue;
        if (!ctx.pendingInputSharedWait()) {
            _ = userLog("Init: startup policy stalled\n");
            while (true) asm volatile ("pause");
        }
        const shared_paddr = waitRecvCap();
        if (shared_paddr < cap_transfer_abi.transfer_id_min) continue;
        if (!ctx.tryFinishInputSharedFromPaddr(shared_paddr)) {
            _ = userLog("Init: unexpected input shared page\n");
            while (true) asm volatile ("pause");
        }
    }
    ctx.finishPendingInputSharedCaps();
    _ = userLog("Init: startup manifest done\n");
}

pub export fn _start() noreturn {
    noteBootStatus(boot_status_abi.status_init_started);

    requireBootImage(.init_app, "Init: boot manifest missing init image\n");
    requireBootImage(.bootfs_image, "Init: boot manifest missing bootfs image\n");
    _ = userLog("Init: boot manifest ok\n");

    const primary_display = requirePrimaryDisplayDescriptor();
    const bootfs = requireBootFsArchiveView();
    const devices = requireClassifiedBootstrapDevices();
    const keyboard_shared_page = requireSpawnPageDescriptor(.input_shared, .keyboard, "Init: keyboard shared descriptor missing\n");
    const pointer_shared_page = requireSpawnPageDescriptor(.input_shared, .pointer, "Init: pointer shared descriptor missing\n");
    const primary_panel_config_page = requireSpawnPageDescriptor(.ui_config, .primary_panel, "Init: primary panel config descriptor missing\n");
    const primary_panel_state_page = requireSpawnPageDescriptor(.ui_state, .primary_panel, "Init: primary panel state descriptor missing\n");
    const primary_panel_command_page = requireSpawnPageDescriptor(.ui_command, .primary_panel, "Init: primary panel command descriptor missing\n");
    const window_service_page = requireSpawnPageDescriptor(.service_config, .window_service, "Init: window service descriptor missing\n");
    const startup_manifest = loadStartupManifestFromBootFs(bootfs) orelse {
        _ = userLog("Init: load stage1 startup manifest failed\n");
        while (true) asm volatile ("pause");
    };
    _ = userLog("Init: stage1 startup manifest ok\n");
    _ = userLog("Init: bootfs archive ready\n");
    _ = ensureBootFsVmObjectCache(bootfs);
    launch_ctx_storage = .{
        .client = null,
        // boot_display is now spawned by init (not pre-created by kernel).
        // requireBootDisplay() will set has_boot_display = true and populate window_service.
        .has_boot_display = false,
        .bootfs = bootfs.*,
        .primary_display = primary_display,
        .window_service = .{ .kind = 0, .process_slot = 0, .endpoint_id = 0, .flags = 0 },
        .keyboard_input = devices.keyboard,
        .keyboard_shared_page = keyboard_shared_page,
        .pointer_input = devices.pointer,
        .pointer_shared_page = pointer_shared_page,
        .block_device = devices.block,
        .primary_panel_config_page = primary_panel_config_page,
        .primary_panel_state_page = primary_panel_state_page,
        .primary_panel_command_page = primary_panel_command_page,
        .window_service_page = window_service_page,
    };
    runStartupManifest(&launch_ctx_storage, startup_manifest);

    const bootstrap_client = launch_ctx_storage.ensureBootstrapFsClient() orelse {
        _ = userLog("Init: bootstrap fs unavailable for rootfs manifest\n");
        while (true) asm volatile ("pause");
    };
    const rootfs_startup_manifest = loadStartupManifestFromFs(bootstrap_client, rootfs_startup_manifest_path) orelse {
        _ = userLog("Init: load rootfs startup manifest failed\n");
        while (true) asm volatile ("pause");
    };
    _ = userLog("Init: rootfs startup manifest ok\n");
    runStartupManifest(&launch_ctx_storage, rootfs_startup_manifest);

    while (true) asm volatile ("pause");
}
