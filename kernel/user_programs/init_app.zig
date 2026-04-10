const std = @import("std");
const bootfs_format = @import("bootfs_format.zig");
const cap_transfer_abi = @import("cap_transfer_abi.zig");
const mouse_shared_abi = @import("mouse_shared_abi.zig");
const process_abi = @import("process_abi.zig");
const boot_manifest_abi = @import("boot_manifest_abi.zig");
const fs_abi = @import("fs_abi.zig");
const image_abi = @import("image_abi.zig");
const startup_plan_abi = @import("startup_plan_abi.zig");
const queue_abi = @import("queue_abi.zig");
const init_bootstrap_abi = @import("init_bootstrap_abi.zig");
const input_bootstrap = @import("input_driver_bootstrap_abi.zig");
const block_bootstrap = @import("block_bootstrap_abi.zig");
const block_demo_bootstrap = @import("block_demo_bootstrap_abi.zig");
const persistent_fs_bootstrap = @import("persistent_fs_bootstrap_abi.zig");
const terminal_bootstrap = @import("terminal_bootstrap_abi.zig");
const mouse_demo_bootstrap = @import("mouse_demo_bootstrap_abi.zig");
const service_registry_abi = @import("service_registry_abi.zig");
const taskbar_bootstrap = @import("taskbar_bootstrap_abi.zig");
const vfs_client = @import("vfs_client.zig");

const syscall_log: u64 = 0x9;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_send_cap: u64 = 0x6;
const syscall_share_cap: u64 = 0x2B;
const syscall_switch_thread: u64 = 0x5;
const syscall_install_endpoint: u64 = 0x26;
const syscall_signal_endpoint: u64 = 0x2C;
const syscall_fs_send_cap: u64 = 0x1B;
const init_process_slot: u64 = 9;
const vfs_request_va: u64 = 0x3C10_4000;
const vfs_response_va: u64 = 0x3C10_5000;
const dynamic_bootstrap_source_base_va: u64 = 0x3C10_6000;
const keyboard_shared_magic: u64 = 0x4B534852; // "KSHR"
const mouse_shared_magic: u64 = mouse_shared_abi.magic;
const startup_manifest_path = "/boot/startup_manifest.txt";
const startup_manifest_max_bytes: usize = 1024;
const vfs_boot_config_magic: u64 = 0x5646_5343; // "VFSC"
const persistent_fs_start_block: u64 = 395264;
const vfs_boot_config_version: u64 = 2;
const vfs_boot_config_flag_bootfs_present: u64 = 1 << 0;
const input_shared_page_paddr_index: usize = 10;

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

fn switchThread(thread_index: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_switch_thread),
          [arg0] "{rdi}" (thread_index),
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

fn findInputDeviceDescriptor(kind: init_bootstrap_abi.InputDeviceKind) ?init_bootstrap_abi.InputDeviceDescriptor {
    const page = descriptorPage() orelse return null;
    var i: usize = 0;
    while (i < page.input_device_count and i < init_bootstrap_abi.max_input_device_descriptors) : (i += 1) {
        const descriptor = page.input_devices[i];
        if (descriptor.kind != @intFromEnum(kind)) continue;
        if ((descriptor.flags & init_bootstrap_abi.input_device_flag_present) == 0) continue;
        return descriptor;
    }
    return null;
}

fn findBlockDeviceDescriptor(kind: init_bootstrap_abi.BlockDeviceKind) ?init_bootstrap_abi.BlockDeviceDescriptor {
    const page = descriptorPage() orelse return null;
    var i: usize = 0;
    while (i < page.block_device_count and i < init_bootstrap_abi.max_block_device_descriptors) : (i += 1) {
        const descriptor = page.block_devices[i];
        if (descriptor.kind != @intFromEnum(kind)) continue;
        if ((descriptor.flags & init_bootstrap_abi.block_device_flag_present) == 0) continue;
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
    kind: init_bootstrap_abi.InputDeviceKind,
    failure_message: []const u8,
) init_bootstrap_abi.InputDeviceDescriptor {
    return findInputDeviceDescriptor(kind) orelse {
        _ = userLog(failure_message);
        while (true) asm volatile ("pause");
    };
}

fn requireBlockDeviceDescriptor(
    kind: init_bootstrap_abi.BlockDeviceKind,
    failure_message: []const u8,
) init_bootstrap_abi.BlockDeviceDescriptor {
    return findBlockDeviceDescriptor(kind) orelse {
        _ = userLog(failure_message);
        while (true) asm volatile ("pause");
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

fn requireRootMountToken() u64 {
    const page = descriptorPage() orelse {
        _ = userLog("Init: bootstrap descriptor page missing\n");
        while (true) asm volatile ("pause");
    };
    const token = page.filesystem.root_mount_token;
    if (!fs_abi.isCapToken(token)) {
        _ = userLog("Init: root mount token missing\n");
        while (true) asm volatile ("pause");
    }
    return token;
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

fn sendFsCap(token: u64, endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_fs_send_cap),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (endpoint_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantFsCap(token: u64, to_process_slot: u64, rights: fs_abi.Rights) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (fs_abi.syscall_grant_cap),
          [arg0] "{rdi}" (token),
          [arg1] "{rsi}" (to_process_slot),
          [arg2] "{rdx}" (fs_abi.rightsToBits(rights)),
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

fn sendFsCapRetry(token: u64, endpoint_id: u64) bool {
    var attempt: usize = 0;
    while (attempt < 1_000_000) : (attempt += 1) {
        if (sendFsCap(token, endpoint_id) == 0) return true;
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

fn grantInputDriverResources(descriptor: init_bootstrap_abi.InputDeviceDescriptor, child_process_slot: u64) bool {
    const page_right_cpu_read: u64 = 0x1;
    const page_right_cpu_write: u64 = 0x2;
    const common_paddr = descriptor.common_page_paddr;
    const notify_paddr = descriptor.notify_page_paddr;
    const isr_paddr = descriptor.isr_page_paddr;
    const device_paddr = descriptor.device_page_paddr;
    const submit_source_token = descriptor.init_queue_submit_token;
    const notify_source_token = descriptor.init_queue_notify_token;
    var grant_paddrs: [4]u64 = [_]u64{0} ** 4;
    var grant_rights: [4]u64 = [_]u64{0} ** 4;
    var grant_count: usize = 0;
    if (common_paddr == 0 or notify_paddr == 0 or submit_source_token == 0 or notify_source_token == 0) return false;

    const collectGrant = struct {
        fn append(grant_paddrs_buf: *[4]u64, grant_rights_buf: *[4]u64, count: *usize, paddr: u64, rights_bits: u64) bool {
            if (paddr == 0) return true;
            var i: usize = 0;
            while (i < count.*) : (i += 1) {
                if (grant_paddrs_buf[i] != paddr) continue;
                grant_rights_buf[i] |= rights_bits;
                return true;
            }
            if (count.* >= grant_paddrs_buf.len) return false;
            grant_paddrs_buf[count.*] = paddr;
            grant_rights_buf[count.*] = rights_bits;
            count.* += 1;
            return true;
        }
    };

    if (!collectGrant.append(&grant_paddrs, &grant_rights, &grant_count, common_paddr, page_right_cpu_read | page_right_cpu_write)) return false;
    if (!collectGrant.append(&grant_paddrs, &grant_rights, &grant_count, notify_paddr, page_right_cpu_read | page_right_cpu_write)) return false;
    if (!collectGrant.append(&grant_paddrs, &grant_rights, &grant_count, isr_paddr, page_right_cpu_read)) return false;
    if (!collectGrant.append(&grant_paddrs, &grant_rights, &grant_count, device_paddr, page_right_cpu_read)) return false;

    var grant_idx: usize = 0;
    while (grant_idx < grant_count) : (grant_idx += 1) {
        if (grantCap(child_process_slot, grant_paddrs[grant_idx], grant_rights[grant_idx]) != 0) return false;
    }

    const submit_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(submit_source_token), child_process_slot);
    const notify_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(notify_source_token), child_process_slot);
    const submit_child = queue_abi.decodeQueueCapToken(submit_child_encoded) orelse return false;
    const notify_child = queue_abi.decodeQueueCapToken(notify_child_encoded) orelse return false;
    input_bootstrap.writeGrantedQueueTokens(descriptor.config_source_va, submit_child, notify_child);
    return true;
}

fn grantBlockDriverResources(descriptor: init_bootstrap_abi.BlockDeviceDescriptor, child_process_slot: u64) bool {
    const page_right_cpu_read: u64 = 0x1;
    const page_right_cpu_write: u64 = 0x2;
    const common_paddr = descriptor.common_page_paddr;
    const notify_paddr = descriptor.notify_page_paddr;
    const isr_paddr = descriptor.isr_page_paddr;
    const device_paddr = descriptor.device_page_paddr;
    const submit_source_token = descriptor.init_queue_submit_token;
    const notify_source_token = descriptor.init_queue_notify_token;
    var grant_paddrs: [4]u64 = [_]u64{0} ** 4;
    var grant_rights: [4]u64 = [_]u64{0} ** 4;
    var grant_count: usize = 0;
    if (common_paddr == 0 or notify_paddr == 0 or submit_source_token == 0 or notify_source_token == 0) return false;

    const collectGrant = struct {
        fn append(grant_paddrs_buf: *[4]u64, grant_rights_buf: *[4]u64, count: *usize, paddr: u64, rights_bits: u64) bool {
            if (paddr == 0) return true;
            var i: usize = 0;
            while (i < count.*) : (i += 1) {
                if (grant_paddrs_buf[i] != paddr) continue;
                grant_rights_buf[i] |= rights_bits;
                return true;
            }
            if (count.* >= grant_paddrs_buf.len) return false;
            grant_paddrs_buf[count.*] = paddr;
            grant_rights_buf[count.*] = rights_bits;
            count.* += 1;
            return true;
        }
    };

    if (!collectGrant.append(&grant_paddrs, &grant_rights, &grant_count, common_paddr, page_right_cpu_read | page_right_cpu_write)) return false;
    if (!collectGrant.append(&grant_paddrs, &grant_rights, &grant_count, notify_paddr, page_right_cpu_read | page_right_cpu_write)) return false;
    if (!collectGrant.append(&grant_paddrs, &grant_rights, &grant_count, isr_paddr, page_right_cpu_read)) return false;
    if (!collectGrant.append(&grant_paddrs, &grant_rights, &grant_count, device_paddr, page_right_cpu_read)) return false;

    var grant_idx: usize = 0;
    while (grant_idx < grant_count) : (grant_idx += 1) {
        if (grantCap(child_process_slot, grant_paddrs[grant_idx], grant_rights[grant_idx]) != 0) return false;
    }

    const submit_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(submit_source_token), child_process_slot);
    const notify_child_encoded = grantQueueCap(queue_abi.encodeQueueCapToken(notify_source_token), child_process_slot);
    const submit_child = queue_abi.decodeQueueCapToken(submit_child_encoded) orelse return false;
    const notify_child = queue_abi.decodeQueueCapToken(notify_child_encoded) orelse return false;
    block_bootstrap.writeGrantedQueueTokens(descriptor.config_source_va, submit_child, notify_child);
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

const StartupManifestLine = struct {
    role: startup_plan_abi.StartupProgramRole,
    path: []const u8,
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
var block_bootstrap_pages_storage: [process_abi.max_bootstrap_page_descriptors]process_abi.BootstrapPageDescriptor = undefined;
var block_bootstrap_table_storage = process_abi.BootstrapDescriptorTable{};
var persistent_fs_bootstrap_pages_storage: [process_abi.max_bootstrap_page_descriptors]process_abi.BootstrapPageDescriptor = undefined;
var persistent_fs_bootstrap_table_storage = process_abi.BootstrapDescriptorTable{};
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

fn parseStartupManifestLine(raw_line: []const u8) ?StartupManifestLine {
    const line = std.mem.trim(u8, raw_line, " \t\r");
    if (line.len == 0 or line[0] == '#') return null;

    var split_at: usize = 0;
    while (split_at < line.len and line[split_at] != ' ' and line[split_at] != '\t') : (split_at += 1) {}
    if (split_at == 0 or split_at >= line.len) {
        _ = userLog("Init: malformed startup manifest line\n");
        while (true) asm volatile ("pause");
    }

    const key = line[0..split_at];
    const path = std.mem.trimLeft(u8, line[split_at..], " \t");
    if (path.len == 0) {
        _ = userLog("Init: empty startup manifest path\n");
        while (true) asm volatile ("pause");
    }

    const role = startup_plan_abi.roleFromKey(key) orelse {
        _ = userLog("Init: unknown startup manifest role\n");
        while (true) asm volatile ("pause");
    };
    return .{
        .role = role,
        .path = path,
    };
}

var next_dynamic_service_endpoint_id: u64 = service_registry_abi.init_window_service_endpoint_id + 1;
var next_dynamic_bootstrap_source_va: u64 = dynamic_bootstrap_source_base_va;

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

const StartupAction = enum {
    bootstrap_vfs,
    input_driver,
    block_driver,
    persistent_fs_server,
    block_client,
    window_client,
    deferred_compositor,
};

const CompositorVariant = enum {
    classic,
    gpu,
};

const window_bootstrap_flag_service_registry: u64 = 1 << 0;
const window_bootstrap_flag_keyboard_shared: u64 = 1 << 1;
const window_bootstrap_flag_pointer_shared: u64 = 1 << 2;
const window_bootstrap_flag_taskbar_panel: u64 = 1 << 3;

const WindowBootstrapConfigKind = enum {
    terminal,
    taskbar,
    mouse_demo,
};

const StartupRoleSpec = struct {
    role: startup_plan_abi.StartupProgramRole,
    label: []const u8,
    action: StartupAction,
    needs_boot_display: bool = false,
    input_kind: ?init_bootstrap_abi.InputDeviceKind = null,
    block_kind: ?init_bootstrap_abi.BlockDeviceKind = null,
    window_flags: u64 = 0,
    window_config_kind: ?WindowBootstrapConfigKind = null,
    compositor_variant: ?CompositorVariant = null,
};

const startup_role_specs = [_]StartupRoleSpec{
    .{
        .role = .vfs,
        .label = "VFS",
        .action = .bootstrap_vfs,
    },
    .{
        .role = .keyboard_driver,
        .label = "keyboard driver",
        .action = .input_driver,
        .input_kind = .keyboard,
    },
    .{
        .role = .mouse_driver,
        .label = "mouse driver",
        .action = .input_driver,
        .needs_boot_display = true,
        .input_kind = .pointer,
    },
    .{
        .role = .block_driver,
        .label = "block driver",
        .action = .block_driver,
        .block_kind = .virtio_blk,
    },
    .{
        .role = .persistent_fs,
        .label = "persistent fs",
        .action = .persistent_fs_server,
    },
    .{
        .role = .block_demo,
        .label = "block demo",
        .action = .block_client,
    },
    .{
        .role = .terminal_window,
        .label = "terminal",
        .action = .window_client,
        .needs_boot_display = true,
        .window_flags = window_bootstrap_flag_service_registry | window_bootstrap_flag_keyboard_shared,
        .window_config_kind = .terminal,
    },
    .{
        .role = .taskbar,
        .label = "taskbar",
        .action = .window_client,
        .needs_boot_display = true,
        .window_flags = window_bootstrap_flag_service_registry | window_bootstrap_flag_pointer_shared | window_bootstrap_flag_taskbar_panel,
        .window_config_kind = .taskbar,
    },
    .{
        .role = .mouse_button_demo,
        .label = "mouse demo",
        .action = .window_client,
        .needs_boot_display = true,
        .window_flags = window_bootstrap_flag_service_registry | window_bootstrap_flag_pointer_shared,
        .window_config_kind = .mouse_demo,
    },
    .{
        .role = .compositor,
        .label = "compositor",
        .action = .deferred_compositor,
        .needs_boot_display = true,
        .compositor_variant = .classic,
    },
    .{
        .role = .gpu_compositor,
        .label = "gpu compositor",
        .action = .deferred_compositor,
        .needs_boot_display = true,
        .compositor_variant = .gpu,
    },
};

fn findStartupRoleSpec(role: startup_plan_abi.StartupProgramRole) ?StartupRoleSpec {
    inline for (startup_role_specs) |spec| {
        if (spec.role == role) return spec;
    }
    return null;
}

const PendingWindowClient = struct {
    line: StartupManifestLine,
    launched: bool = false,
};

const PendingLateRole = struct {
    line: StartupManifestLine,
};

const LaunchContext = struct {
    client: ?vfs_client.Client,
    has_boot_display: bool,
    root_mount_token: u64,
    bootfs: BootFsArchiveView,
    primary_display: init_bootstrap_abi.DisplayDescriptor,
    window_service: service_registry_abi.ServiceEntry,
    keyboard_input: init_bootstrap_abi.InputDeviceDescriptor,
    keyboard_shared_page: init_bootstrap_abi.SpawnPageDescriptor,
    pointer_input: init_bootstrap_abi.InputDeviceDescriptor,
    pointer_shared_page: init_bootstrap_abi.SpawnPageDescriptor,
    block_device: init_bootstrap_abi.BlockDeviceDescriptor,
    primary_panel_config_page: init_bootstrap_abi.SpawnPageDescriptor,
    primary_panel_state_page: init_bootstrap_abi.SpawnPageDescriptor,
    primary_panel_command_page: init_bootstrap_abi.SpawnPageDescriptor,
    window_service_page: init_bootstrap_abi.SpawnPageDescriptor,
    vfs_process_slot: ?u64 = null,
    vfs_endpoint_id: ?u64 = null,
    block_process_slot: ?u64 = null,
    block_endpoint_id: ?u64 = null,
    persistent_fs_process_slot: ?u64 = null,
    persistent_fs_endpoint_id: ?u64 = null,
    keyboard_spawned: bool = false,
    keyboard_shared_ready: bool = false,
    pointer_spawned: bool = false,
    pointer_shared_ready: bool = false,
    first_window_spawn_logged: bool = false,

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
            .client_process_slot = init_process_slot,
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

    fn requireExecFromBootFs(self: *LaunchContext, path: []const u8, label: []const u8) vfs_client.OpenResult {
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
        _ = userLog("Init: boot display image absent, skipping UI bootstrap\n");
        while (true) asm volatile ("pause");
    }

    fn inputDescriptorForKind(self: *LaunchContext, kind: init_bootstrap_abi.InputDeviceKind) init_bootstrap_abi.InputDeviceDescriptor {
        return switch (kind) {
            .keyboard => self.keyboard_input,
            .pointer => self.pointer_input,
        };
    }

    fn blockDescriptorForKind(self: *LaunchContext, kind: init_bootstrap_abi.BlockDeviceKind) init_bootstrap_abi.BlockDeviceDescriptor {
        _ = kind;
        return self.block_device;
    }

    fn inputSharedPageForKind(self: *LaunchContext, kind: init_bootstrap_abi.InputDeviceKind) init_bootstrap_abi.SpawnPageDescriptor {
        return switch (kind) {
            .keyboard => self.keyboard_shared_page,
            .pointer => self.pointer_shared_page,
        };
    }

    fn writeInputConfigForKind(self: *LaunchContext, kind: init_bootstrap_abi.InputDeviceKind, descriptor: init_bootstrap_abi.InputDeviceDescriptor) void {
        const shared_desc = self.inputSharedPageForKind(kind);
        switch (kind) {
            .keyboard => input_bootstrap.writeKeyboardConfigPage(descriptor.config_source_va, .{
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
            .pointer => input_bootstrap.writeMouseConfigPage(descriptor.config_source_va, .{
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

    fn validateInputSharedPage(self: *LaunchContext, kind: init_bootstrap_abi.InputDeviceKind, shared_desc: init_bootstrap_abi.SpawnPageDescriptor) void {
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

    fn inputSharedPaddrFromConfig(self: *LaunchContext, kind: init_bootstrap_abi.InputDeviceKind) u64 {
        const descriptor = self.inputDescriptorForKind(kind);
        const words: [*]const volatile u64 = @ptrFromInt(descriptor.config_source_va);
        return words[input_shared_page_paddr_index];
    }

    fn finishInputSharedPage(self: *LaunchContext, kind: init_bootstrap_abi.InputDeviceKind, shared_paddr: u64) void {
        const shared_desc = self.inputSharedPageForKind(kind);
        if (mapPage(shared_desc.source_va, shared_paddr, false) != 0) {
            _ = userLog("Init: input shared page map failed\n");
            while (true) asm volatile ("pause");
        }
        self.validateInputSharedPage(kind, shared_desc);
        switch (kind) {
            .keyboard => {
                self.keyboard_shared_ready = true;
                _ = userLog("Init: keyboard shared ready\n");
            },
            .pointer => {
                self.pointer_shared_ready = true;
                _ = userLog("Init: mouse shared ready\n");
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

    fn prepareWindowBootstrapForSpec(
        self: *LaunchContext,
        spec: StartupRoleSpec,
        out: []process_abi.BootstrapPageDescriptor,
    ) []const process_abi.BootstrapPageDescriptor {
        var count: usize = 0;
        const registry_target_va = self.window_service_page.target_va;
        var registry_source_va: u64 = 0;
        if ((spec.window_flags & window_bootstrap_flag_taskbar_panel) != 0) {
            if ((spec.window_flags & window_bootstrap_flag_service_registry) != 0 and registry_source_va == 0) {
                registry_source_va = allocChildServiceRegistryPage(
                    self.window_service,
                    self.vfs_process_slot,
                    self.vfs_endpoint_id,
                    self.block_process_slot,
                    self.block_endpoint_id,
                    self.persistent_fs_process_slot,
                    self.persistent_fs_endpoint_id,
                ) orelse {
                    self.logRoleLine("alloc", spec.label, "window service registry failed");
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
        if ((spec.window_flags & window_bootstrap_flag_keyboard_shared) != 0) {
            self.appendBootstrapPage(out, &count, self.keyboard_shared_page.source_va, self.keyboard_shared_page.target_va, self.keyboard_shared_page.spawn_flags);
        }
        if ((spec.window_flags & window_bootstrap_flag_pointer_shared) != 0) {
            self.appendBootstrapPage(out, &count, self.pointer_shared_page.source_va, self.pointer_shared_page.target_va, self.pointer_shared_page.spawn_flags);
        }
        if ((spec.window_flags & window_bootstrap_flag_service_registry) != 0) {
            if (registry_source_va == 0) {
                registry_source_va = allocChildServiceRegistryPage(
                    self.window_service,
                    self.vfs_process_slot,
                    self.vfs_endpoint_id,
                    self.block_process_slot,
                    self.block_endpoint_id,
                    self.persistent_fs_process_slot,
                    self.persistent_fs_endpoint_id,
                ) orelse {
                    self.logRoleLine("alloc", spec.label, "window service registry failed");
                    while (true) asm volatile ("pause");
                };
            }
            const config_kind = spec.window_config_kind orelse unreachable;
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

    fn launchVfsForSpec(self: *LaunchContext, spec: StartupRoleSpec, path: []const u8) void {
        const image = self.bootfs.findRegularFile(path) orelse {
            self.logRoleLine("lookup", spec.label, "failed");
            while (true) asm volatile ("pause");
        };
        self.logRoleLine("lookup", spec.label, "ok");
        const vm_token = installVmObject(@intFromPtr(image.ptr), image.len, .{ .read = true });
        if (image_abi.decodeVmObjectToken(vm_token) == null) {
            self.logRoleLine("install_vm", spec.label, "failed");
            while (true) asm volatile ("pause");
        }
        const exec_token = installExecImage(vm_token, .{ .exec = true });
        if (image_abi.decodeExecImageToken(exec_token) == null) {
            self.logRoleLine("open_exec", spec.label, "failed");
            while (true) asm volatile ("pause");
        }
        self.logRoleLine("open_exec", spec.label, "ok");

        const vfs_config_source_va = allocDynamicBootstrapSourceVa();
        var cfg_paddr: u64 = 0;
        if (allocMapPages(vfs_config_source_va, 1, true, @intFromPtr(&cfg_paddr)) != 0 or cfg_paddr < 0x1000) {
            self.logRoleLine("alloc", spec.label, "config page failed");
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
        vfs_bootstrap_table_storage.cap_count = 2;
        vfs_bootstrap_table_storage.page_descriptors[0] = vfs_bootstrap_pages_storage[0];
        vfs_bootstrap_table_storage.cap_descriptors[0] = .{
            .source_token = self.root_mount_token,
            .target_token_va = process_abi.standard_config_target_va + 3 * 8,
            .rights_bits = fs_abi.rightsToBits(.{
                .lookup = true,
                .read = true,
                .readdir = true,
                .stat = true,
                .mount = true,
                .grant = true,
                .admin = true,
            }),
            .kind = .fs,
        };
        vfs_bootstrap_table_storage.cap_descriptors[1] = .{
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
            self.logRoleLine("spawn", spec.label, "failed");
            self.logRoleHex(spec.label, " spawn ret=", spawned);
            while (true) asm volatile ("pause");
        };
        self.logRoleLine("spawn", spec.label, "ok");
        _ = userLog("Init: VFS spawn done\n");
        self.vfs_process_slot = child_slot;
        self.vfs_endpoint_id = vfs_endpoint_id;
        if (installEndpoint(vfs_endpoint_id, child_slot) != 0) {
            _ = userLog("Init: install VFS endpoint failed\n");
            while (true) asm volatile ("pause");
        }
    }

    fn launchInputDriverForSpec(self: *LaunchContext, spec: StartupRoleSpec, path: []const u8) void {
        if (spec.needs_boot_display) self.requireBootDisplay();
        const input_kind = spec.input_kind orelse unreachable;
        const exec = self.requireExecFromBootFs(path, spec.label);

        const input_desc = self.inputDescriptorForKind(input_kind);
        self.writeInputConfigForKind(input_kind, input_desc);

        const spawned = spawnExec(exec.token, input_desc.config_source_va, input_desc.config_target_va, input_desc.config_spawn_flags);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", spec.label, "failed");
            self.logRoleHex(spec.label, " spawn ret=", spawned);
            while (true) asm volatile ("pause");
        };
        self.logRoleLine("spawn", spec.label, "ok");
        switch (input_kind) {
            .keyboard => {
                self.keyboard_spawned = true;
                _ = userLog("Init: keyboard spawn done\n");
            },
            .pointer => {
                self.pointer_spawned = true;
                _ = userLog("Init: mouse spawn done\n");
            },
        }
        if (!grantInputDriverResources(input_desc, child_slot)) {
            self.logRoleLine("grant", spec.label, "resources failed");
            while (true) asm volatile ("pause");
        }
        self.logRoleLine("grant", spec.label, "resources ok");
    }

    fn launchBlockDriverForSpec(self: *LaunchContext, spec: StartupRoleSpec, path: []const u8) void {
        const block_kind = spec.block_kind orelse unreachable;
        const exec = self.requireExecFromBootFs(path, spec.label);
        const block_desc = self.blockDescriptorForKind(block_kind);
        if (!fs_abi.isCapToken(block_desc.init_root_token)) {
            self.logRoleLine("bootstrap", spec.label, "root token missing");
            while (true) asm volatile ("pause");
        }

        const endpoint_id = allocDynamicServiceEndpointId();
        block_bootstrap.writeConfigPage(block_desc.config_source_va, .{
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
            .capacity_sectors = block_desc.capacity_sectors,
            .logical_block_size = block_desc.logical_block_size,
        });

        block_bootstrap_pages_storage[0] = .{
            .source_va = block_desc.config_source_va,
            .target_va = block_desc.config_target_va,
            .flags = block_desc.config_spawn_flags,
        };
        block_bootstrap_table_storage = .{};
        block_bootstrap_table_storage.page_count = 1;
        block_bootstrap_table_storage.cap_count = 1;
        block_bootstrap_table_storage.page_descriptors[0] = block_bootstrap_pages_storage[0];
        block_bootstrap_table_storage.cap_descriptors[0] = .{
            .source_token = block_desc.init_root_token,
            .target_token_va = block_desc.config_target_va + block_bootstrap.root_token_index * 8,
            .rights_bits = fs_abi.rightsToBits(.{
                .read = true,
                .write = true,
                .grant = true,
                .admin = true,
            }),
            .kind = .fs,
        };

        const spawned = spawnExecWithExtendedBootstrapTable(exec.token, &block_bootstrap_table_storage);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", spec.label, "failed");
            self.logRoleHex(spec.label, " spawn ret=", spawned);
            while (true) asm volatile ("pause");
        };
        self.logRoleLine("spawn", spec.label, "ok");
        if (installEndpoint(endpoint_id, child_slot) != 0) {
            self.logRoleLine("endpoint", spec.label, "install failed");
            while (true) asm volatile ("pause");
        }
        if (!grantBlockDriverResources(block_desc, child_slot)) {
            self.logRoleLine("grant", spec.label, "resources failed");
            while (true) asm volatile ("pause");
        }
        self.logRoleLine("grant", spec.label, "resources ok");
        _ = signalEndpoint(endpoint_id);
        service_registry_abi.addService(self.window_service_page.source_va, .block, child_slot, endpoint_id);
        self.block_process_slot = child_slot;
        self.block_endpoint_id = endpoint_id;
    }

    fn launchPersistentFsForSpec(self: *LaunchContext, spec: StartupRoleSpec, path: []const u8) void {
        if (self.block_process_slot == null or self.block_endpoint_id == null) {
            self.logRoleLine("bootstrap", spec.label, "block service missing");
            while (true) asm volatile ("pause");
        }
        const exec = self.requireExecFromBootFs(path, spec.label);
        const endpoint_id = allocDynamicServiceEndpointId();
        const registry_source_va = allocChildServiceRegistryPage(
            self.window_service,
            self.vfs_process_slot,
            self.vfs_endpoint_id,
            self.block_process_slot,
            self.block_endpoint_id,
            null,
            null,
        ) orelse {
            self.logRoleLine("alloc", spec.label, "service registry failed");
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
        persistent_fs_bootstrap_table_storage.cap_count = 1;
        persistent_fs_bootstrap_table_storage.page_descriptors[0] = persistent_fs_bootstrap_pages_storage[0];
        persistent_fs_bootstrap_table_storage.page_descriptors[1] = persistent_fs_bootstrap_pages_storage[1];
        persistent_fs_bootstrap_table_storage.cap_descriptors[0] = .{
            .source_token = self.root_mount_token,
            .target_token_va = process_abi.standard_config_target_va + persistent_fs_bootstrap.admin_token_index * 8,
            .rights_bits = fs_abi.rightsToBits(.{
                .admin = true,
            }),
            .kind = .fs,
        };
        const spawned = spawnExecWithExtendedBootstrapTable(exec.token, &persistent_fs_bootstrap_table_storage);
        const child_slot = process_abi.decodeSpawnedProcessSlot(spawned) orelse {
            self.logRoleLine("spawn", spec.label, "failed");
            self.logRoleHex(spec.label, " spawn ret=", spawned);
            while (true) asm volatile ("pause");
        };
        self.logRoleLine("spawn", spec.label, "ok");
        if (installEndpoint(endpoint_id, child_slot) != 0) {
            self.logRoleLine("endpoint", spec.label, "install failed");
            while (true) asm volatile ("pause");
        }
        service_registry_abi.addService(self.window_service_page.source_va, .persistent_fs, child_slot, endpoint_id);
        self.persistent_fs_process_slot = child_slot;
        self.persistent_fs_endpoint_id = endpoint_id;
    }

    fn launchBlockDemoForSpec(self: *LaunchContext, spec: StartupRoleSpec, path: []const u8) void {
        if (self.block_process_slot == null or self.block_endpoint_id == null) {
            self.logRoleLine("bootstrap", spec.label, "block service missing");
            while (true) asm volatile ("pause");
        }
        const exec = self.requireExecFromBootFs(path, spec.label);
        const registry_source_va = allocChildServiceRegistryPage(
            self.window_service,
            self.vfs_process_slot,
            self.vfs_endpoint_id,
            self.block_process_slot,
            self.block_endpoint_id,
            self.persistent_fs_process_slot,
            self.persistent_fs_endpoint_id,
        ) orelse {
            self.logRoleLine("alloc", spec.label, "service registry failed");
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
            self.logRoleLine("spawn", spec.label, "failed");
            self.logRoleHex(spec.label, " spawn ret=", spawned);
            while (true) asm volatile ("pause");
        };
        block_demo_bootstrap.writeProcessSlot(config_source_va, child_slot);
        self.logRoleLine("spawn", spec.label, "ok");
    }

    fn launchWindowClientForSpec(self: *LaunchContext, spec: StartupRoleSpec, path: []const u8) void {
        if (spec.needs_boot_display) self.requireBootDisplay();
        const exec = self.requireExecFromBootFs(path, spec.label);

        var bootstrap_pages: [5]process_abi.BootstrapPageDescriptor = undefined;
        const bootstrap_slice = self.prepareWindowBootstrapForSpec(spec, bootstrap_pages[0..]);
        const spawned = spawnExecWithBootstrapPages(exec.token, bootstrap_slice);
        if (process_abi.decodeSpawnedProcessSlot(spawned) == null) {
            self.logRoleLine("spawn", spec.label, "failed");
            self.logRoleHex(spec.label, " spawn ret=", spawned);
            while (true) asm volatile ("pause");
        }
        const child_thread = process_abi.decodeSpawnedThreadSlot(spawned);
        const is_first_window = !self.first_window_spawn_logged;
        self.logRoleLine("spawn", spec.label, "ok");
        if (is_first_window) {
            self.first_window_spawn_logged = true;
            _ = userLog("Init: first window spawn done\n");
            return;
        }
        if (child_thread) |thread_index| {
            _ = switchThread(thread_index);
        }
    }

    fn armCompositor(self: *LaunchContext, classic_path: ?[]const u8, gpu_path: ?[]const u8) void {
        if (classic_path == null or gpu_path == null) return;
        self.requireBootDisplay();
        const classic = classic_path.?;
        const gpu = gpu_path.?;

        const compositor_exec = self.requireExecFromBootFs(classic, "compositor");
        const gpu_compositor_exec = self.requireExecFromBootFs(gpu, "gpu compositor");

        const ret = armDeferredCompositor(compositor_exec.token, gpu_compositor_exec.token, self.window_service.process_slot);
        if (ret != 0) {
            _ = userLog("Init: arm deferred compositor failed\n");
            userLogHex("Init: arm deferred compositor ret=", ret);
            while (true) asm volatile ("pause");
        }
        _ = userLog("Init: arm deferred compositor ok\n");
    }

    fn launchRole(self: *LaunchContext, role: startup_plan_abi.StartupProgramRole, path: []const u8) void {
        const spec = findStartupRoleSpec(role) orelse {
            _ = userLog("Init: startup role spec missing\n");
            while (true) asm volatile ("pause");
        };
        switch (spec.action) {
            .bootstrap_vfs => self.launchVfsForSpec(spec, path),
            .input_driver => self.launchInputDriverForSpec(spec, path),
            .block_driver => self.launchBlockDriverForSpec(spec, path),
            .persistent_fs_server => self.launchPersistentFsForSpec(spec, path),
            .block_client => self.launchBlockDemoForSpec(spec, path),
            .window_client => self.launchWindowClientForSpec(spec, path),
            .deferred_compositor => {},
        }
    }
};

fn windowClientReadyForLaunch(ctx: *const LaunchContext, spec: StartupRoleSpec) bool {
    if ((spec.window_flags & window_bootstrap_flag_keyboard_shared) != 0 and !ctx.keyboard_shared_ready) return false;
    if ((spec.window_flags & window_bootstrap_flag_pointer_shared) != 0 and !ctx.pointer_shared_ready) return false;
    return true;
}

fn tryLaunchReadyWindowClients(ctx: *LaunchContext, pending: []PendingWindowClient) usize {
    var launched_count: usize = 0;
    for (pending) |*entry| {
        if (entry.launched) continue;
        const spec = findStartupRoleSpec(entry.line.role) orelse {
            _ = userLog("Init: startup role spec missing\n");
            while (true) asm volatile ("pause");
        };
        if (spec.action != .window_client) continue;
        if (!windowClientReadyForLaunch(ctx, spec)) continue;
        ctx.launchWindowClientForSpec(spec, entry.line.path);
        entry.launched = true;
        launched_count += 1;
    }
    return launched_count;
}

fn allWindowClientsLaunched(pending: []const PendingWindowClient) bool {
    for (pending) |entry| {
        if (!entry.launched) return false;
    }
    return true;
}

var launch_ctx_storage: LaunchContext = undefined;

fn runStartupManifest(ctx: *LaunchContext, manifest: *const StartupManifest) void {
    _ = userLog("Init: startup manifest begin\n");
    var compositor_path: ?[]const u8 = null;
    var gpu_compositor_path: ?[]const u8 = null;
    var pending_window_clients: [startup_plan_abi.max_startup_program_descriptors]PendingWindowClient = undefined;
    var pending_window_count: usize = 0;
    var pending_late_roles: [startup_plan_abi.max_startup_program_descriptors]PendingLateRole = undefined;
    var pending_late_count: usize = 0;
    var lines = std.mem.tokenizeScalar(u8, manifest.slice(), '\n');
    while (lines.next()) |raw_line| {
        const parsed = parseStartupManifestLine(raw_line) orelse continue;
        const spec = findStartupRoleSpec(parsed.role) orelse {
            _ = userLog("Init: startup role spec missing\n");
            while (true) asm volatile ("pause");
        };
        switch (spec.action) {
            .input_driver => ctx.launchRole(parsed.role, parsed.path),
            .deferred_compositor => switch (spec.compositor_variant orelse unreachable) {
                .classic => compositor_path = parsed.path,
                .gpu => gpu_compositor_path = parsed.path,
            },
            .bootstrap_vfs, .block_driver, .persistent_fs_server, .block_client => {
                if (pending_late_count >= pending_late_roles.len) {
                    _ = userLog("Init: too many late startup roles\n");
                    while (true) asm volatile ("pause");
                }
                pending_late_roles[pending_late_count] = .{
                    .line = parsed,
                };
                pending_late_count += 1;
            },
            .window_client => {
                if (pending_window_count >= pending_window_clients.len) {
                    _ = userLog("Init: too many startup window clients\n");
                    while (true) asm volatile ("pause");
                }
                pending_window_clients[pending_window_count] = .{
                    .line = parsed,
                };
                pending_window_count += 1;
            },
        }
    }
    ctx.armCompositor(compositor_path, gpu_compositor_path);
    const pending_slice = pending_window_clients[0..pending_window_count];
    while (!allWindowClientsLaunched(pending_slice)) {
        if (tryLaunchReadyWindowClients(ctx, pending_slice) != 0) continue;
        const shared_paddr = waitRecvCap();
        if (shared_paddr < cap_transfer_abi.transfer_id_min) continue;
        if (!ctx.tryFinishInputSharedFromPaddr(shared_paddr)) {
            _ = userLog("Init: unexpected input shared page\n");
            while (true) asm volatile ("pause");
        }
    }
    ctx.finishPendingInputSharedCaps();
    var late_index: usize = 0;
    while (late_index < pending_late_count) : (late_index += 1) {
        const entry = pending_late_roles[late_index];
        ctx.launchRole(entry.line.role, entry.line.path);
    }
    _ = userLog("Init: startup manifest done\n");
}

pub export fn _start() noreturn {
    _ = userLog("Init: started\n");

    requireBootImage(.init_app, "Init: boot manifest missing init image\n");
    requireBootImage(.bootfs_image, "Init: boot manifest missing bootfs image\n");
    const has_boot_display = bootImagePresent(.boot_log_console);
    _ = userLog("Init: boot manifest ok\n");

    const primary_display = requirePrimaryDisplayDescriptor();
    const root_mount_token = requireRootMountToken();
    const bootfs = requireBootFsArchiveView();
    const keyboard_input = requireInputDeviceDescriptor(.keyboard, "Init: keyboard input descriptor missing\n");
    const keyboard_shared_page = requireSpawnPageDescriptor(.input_shared, .keyboard, "Init: keyboard shared descriptor missing\n");
    const pointer_input = requireInputDeviceDescriptor(.pointer, "Init: pointer input descriptor missing\n");
    const pointer_shared_page = requireSpawnPageDescriptor(.input_shared, .pointer, "Init: pointer shared descriptor missing\n");
    const block_device = requireBlockDeviceDescriptor(.virtio_blk, "Init: block device descriptor missing\n");
    const primary_panel_config_page = requireSpawnPageDescriptor(.ui_config, .primary_panel, "Init: primary panel config descriptor missing\n");
    const primary_panel_state_page = requireSpawnPageDescriptor(.ui_state, .primary_panel, "Init: primary panel state descriptor missing\n");
    const primary_panel_command_page = requireSpawnPageDescriptor(.ui_command, .primary_panel, "Init: primary panel command descriptor missing\n");
    const window_service_page = requireSpawnPageDescriptor(.service_config, .window_service, "Init: window service descriptor missing\n");
    const window_service = requireWindowServiceEntry(window_service_page.source_va);
    const startup_manifest = loadStartupManifestFromBootFs(bootfs) orelse {
        _ = userLog("Init: load startup manifest failed\n");
        while (true) asm volatile ("pause");
    };
    _ = userLog("Init: startup manifest ok\n");
    _ = userLog("Init: bootfs archive ready\n");
    _ = ensureBootFsVmObjectCache(bootfs);
    launch_ctx_storage = .{
        .client = null,
        .has_boot_display = has_boot_display,
        .root_mount_token = root_mount_token,
        .bootfs = bootfs.*,
        .primary_display = primary_display,
        .window_service = window_service,
        .keyboard_input = keyboard_input,
        .keyboard_shared_page = keyboard_shared_page,
        .pointer_input = pointer_input,
        .pointer_shared_page = pointer_shared_page,
        .block_device = block_device,
        .primary_panel_config_page = primary_panel_config_page,
        .primary_panel_state_page = primary_panel_state_page,
        .primary_panel_command_page = primary_panel_command_page,
        .window_service_page = window_service_page,
    };
    runStartupManifest(&launch_ctx_storage, startup_manifest);

    while (true) asm volatile ("pause");
}
