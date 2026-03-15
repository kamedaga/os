const std = @import("std");
const kernel = @import("kernel.zig");
const capability = @import("capability.zig");
const untyped_memory = @import("untyped_memory.zig");
const elf_loader = @import("elf_loader.zig");
const fs_abi = @import("fs_abi.zig");
const image_abi = @import("image_abi.zig");
const process_abi = @import("process_abi.zig");
const boot_timing = @import("boot_timing.zig");
const deferred_compositor = @import("deferred_compositor.zig");
const kernel_log = @import("kernel_log.zig");
const interrupts = @import("interrupts.zig");
const lapic = @import("lapic.zig");
const x86_platform = @import("arch/x86_64/platform.zig");
const page_fault_log = @import("page_fault_log.zig");
const scheduler = @import("scheduler.zig");
const syscalls = @import("syscalls.zig");
const traps = @import("traps.zig");
const user_copy = @import("user_copy.zig");
const kernel_vm = @import("memory/kernel_vm.zig");
const pmm = @import("memory/pmm.zig");
const user_vm = @import("memory/user_vm.zig");
const virtio_probe = @import("virtio_probe.zig");
const serial = @import("serial.zig");
const user_programs = @import("user_programs.zig");
const uefi = std.os.uefi;
const TrapFrame = interrupts.TrapFrame;
const ExceptionTrapFrame = interrupts.ExceptionTrapFrame;
const serialWrite = kernel_log.write;
const serialWriteFmt = kernel_log.writeFmt;
const serialWriteHexRaw = kernel_log.writeHexRaw;
const serialWriteBool01 = kernel_log.writeBool01;
const serialWriteThreadIndexLabel = kernel_log.writeThreadIndexLabel;
const serialOnlyWrite = kernel_log.writeOnly;
const serialOnlyWriteBool01 = kernel_log.writeOnlyBool01;
const serialOnlyWriteThreadIndexLabel = kernel_log.writeOnlyThreadIndexLabel;
const getThreadContext = scheduler.getThreadContext;
const getThreadContextConst = scheduler.getThreadContextConst;
const activateThread = scheduler.activateThread;
const switchToThread = scheduler.switchToThread;
const blockCurrentThreadForEvent = scheduler.blockCurrentThreadForEvent;
const wakeWaitingThreadForPrincipal = scheduler.wakeWaitingThreadForPrincipal;
const wakeThreadsForTimer = scheduler.wakeThreadsForTimer;
const pickNextReadyThreadIndex = scheduler.pickNextReadyThreadIndex;
const threadContextLooksCorrupted = scheduler.threadContextLooksCorrupted;
const setThreadReady = scheduler.setThreadReady;
const thread_contexts = &scheduler.thread_contexts;

pub fn panic(msg: []const u8, trace: ?*std.builtin.StackTrace, ret_addr: ?usize) noreturn {
    _ = trace;
    _ = ret_addr;
    earlyUefiWrite(&[_:0]u16{ 'P', 'A', 'N', 'I', 'C', ':', ' ', 0 });
    earlyUefiWriteAscii(msg);
    earlyUefiWrite(&[_:0]u16{ '\r', '\n', 0 });
    while (true) asm volatile ("hlt");
}

const four_gib: u64 = 4 * 1024 * 1024 * 1024;
const one_tib: u64 = 1024 * 1024 * 1024 * 1024;
const two_mib: u64 = x86_platform.two_mib;
const page_entries: usize = x86_platform.page_entries;
const user_va: u64 = 0x20000000;
const user_elf_base_va: u64 = user_va; // PIE base is chosen by kernel.
const user_stack_top: u64 = 0x3C00_0000;
const user_stack_page_va: u64 = user_stack_top - 0x1000;
const user_entry_rsp: u64 = user_stack_top - 8; // mimic normal call ABI stack alignment at function entry
const user_aux_base_va: u64 = 0x3C00_0000;
const user_aux_reserved_va: u64 = user_aux_base_va;
const user_program_max_load_bytes: usize = @intCast(user_aux_reserved_va - user_va);
const boot_log_console_stack_page_va: u64 = user_aux_base_va + 0x0000;
const boot_log_console_stack_top: u64 = boot_log_console_stack_page_va + 0x1000;
const boot_log_console_entry_rsp: u64 = boot_log_console_stack_top - 8;
const boot_log_user_va: u64 = user_aux_base_va + 0x1000;
const vfs_config_va: u64 = user_aux_base_va + 0x2000;
const vfs_bootfs_image_va: u64 = 0x3C08_0000;
const mouse_driver_config_va: u64 = user_aux_base_va + 0x2000;
const keyboard_driver_config_va: u64 = user_aux_base_va + 0x2000;
const compositor_gpu_config_va: u64 = user_aux_base_va + 0x2000;
const taskbar_config_va: u64 = user_aux_base_va + 0x2000;
const mouse_shared_driver_va: u64 = user_aux_base_va + 0x3000;
const mouse_shared_draw_va: u64 = user_aux_base_va + 0x3000;
const taskbar_state_shared_va: u64 = user_aux_base_va + 0x4000;
const taskbar_command_shared_va: u64 = user_aux_base_va + 0x6000;
const init_taskbar_config_source_va: u64 = user_aux_base_va + 0x8000;
const init_taskbar_state_source_va: u64 = user_aux_base_va + 0x9000;
const init_taskbar_command_source_va: u64 = user_aux_base_va + 0xA000;
const init_taskbar_mouse_source_va: u64 = user_aux_base_va + 0xB000;
const mouse_driver_queue_page0_va: u64 = 0x2000_8000;
const framebuffer_user_va: u64 = user_aux_base_va + 0x5000;
const keyboard_shared_driver_va: u64 = user_aux_base_va + 0x6000;
const keyboard_shared_draw_va: u64 = user_aux_base_va + 0x6000;
const keyboard_driver_queue_page0_va: u64 = 0x2000_8000;
const framebuffer_window_bytes: u64 = two_mib - 0x5000; // reserve one page for compositor IPC mapping
const enable_framebuffer_server_step1 = true;
const enable_boot_log_console_process = true;
const enable_vfs_process = true;
const enable_init_process = true;
const enable_virtio_input_mouse = true;
const enable_virtio_input_keyboard = true;
const enable_bootlog_wait_for_enter = false;
const enable_title_only_ready_logs = true;
const enable_cap_table_dump_logs = false;
const enable_scheduler_perf_logs = false;
const suppress_compositor_perf_user_logs = true;
const enable_iommu_no_cap_driver = true;
const enforce_iommu_no_cap_driver = false;
const user_entry_rflags: u64 = 0x202;
const user_unmapped_test_va: u64 = 0x20100000;
const user_dma_verify_va: u64 = 0x20110000;
const user_recovery_stop_va: u64 = 0x20200000;
const reserved_low_mem_end: u64 = 64 * 1024 * 1024;
const page_addr_mask: u64 = 0x000F_FFFF_FFFF_F000;
const canonical_user_limit_exclusive: u64 = 0x0000_8000_0000_0000;
const gdt_kernel_code_selector: u16 = x86_platform.gdt_kernel_code_selector;
const gdt_kernel_data_selector: u16 = x86_platform.gdt_kernel_data_selector;
const gdt_user_code_selector: u16 = x86_platform.gdt_user_code_selector;
const gdt_user_data_selector: u16 = x86_platform.gdt_user_data_selector;
const gdt_tss_selector: u16 = x86_platform.gdt_tss_selector;

const page_present: u64 = x86_platform.page_present;
const page_rw: u64 = x86_platform.page_rw;
const page_user: u64 = x86_platform.page_user;
const page_ps: u64 = x86_platform.page_ps;
const lapic_timer_vector: u8 = 0x40;
const lapic_timer_initial_count: u32 = 50_000;
const scheduler_quantum_ticks: u64 = 2;
const bootlog_gate_input_start_delay_ticks: u64 = 8;
const bootlog_gate_auto_input_start_delay_ticks: u64 = 1;
const deferred_compositor_auto_launch_delay_ticks: u64 = 1;
const bootlog_auto_min_visible_ticks: u64 = 2;
const startup_client_settle_ticks: u64 = 2;
const scheduler_log_switch = false;
const scheduler_switch_log_max_lines: u64 = 96;
const scheduler_log_int80 = false;
const scheduler_int80_log_max_lines: u64 = 192;
const scheduler_race_log_max_lines: u64 = 128;
const phys_copy_window_va: u64 = kernel_vm.phys_copy_window_va;
const scheduler_probe_log_max_lines: u64 = 64;
const enable_switch_thread_syscall_log = false;
const user_log_max_bytes: usize = 256;
const fx_state_bytes: usize = 512;
const debug_skip_syscall_fx_state = true;
const debug_skip_timer_fx_state = false;
const user_elf_disk_path: [*:0]const u16 = &[_:0]u16{
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'U', 'S', 'E', 'R', 'A', 'P', 'P', '.', 'E', 'L', 'F',
};
const user_elf_disk_path_log = "\\EFI\\BOOT\\USERAPP.ELF";
const framebuffer_server_elf_disk_path: [*:0]const u16 = &[_:0]u16{
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'F', 'B', 'S', 'R', 'V', '.', 'E', 'L', 'F',
};
const framebuffer_server_elf_disk_path_log = "\\EFI\\BOOT\\FBSRV.ELF";
const draw_client_elf_disk_path: [*:0]const u16 = &[_:0]u16{
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'D', 'R', 'A', 'W', 'C', 'L', 'I', '.', 'E', 'L', 'F',
};
const draw_client_elf_disk_path_log = "\\EFI\\BOOT\\DRAWCLI.ELF";
const boot_log_console_elf_disk_path: [*:0]const u16 = &[_:0]u16{
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'B', 'O', 'O', 'T', 'L', 'O', 'G', '.', 'E', 'L', 'F',
};
const boot_log_console_elf_disk_path_log = "\\EFI\\BOOT\\BOOTLOG.ELF";
const mouse_driver_elf_disk_path: [*:0]const u16 = &[_:0]u16{
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'M', 'O', 'U', 'S', 'E', 'D', 'R', 'V', '.', 'E', 'L', 'F',
};
const mouse_driver_elf_disk_path_log = "\\EFI\\BOOT\\MOUSEDRV.ELF";
const keyboard_driver_elf_disk_path: [*:0]const u16 = &[_:0]u16{
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'K', 'E', 'Y', 'B', 'D', 'R', 'V', '.', 'E', 'L', 'F',
};
const keyboard_driver_elf_disk_path_log = "\\EFI\\BOOT\\KEYBDRV.ELF";
const bootlog_sender_elf_disk_path: [*:0]const u16 = &[_:0]u16{
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'B', 'L', 'O', 'G', 'S', 'N', 'D', '.', 'E', 'L', 'F',
};
const bootlog_sender_elf_disk_path_log = "\\EFI\\BOOT\\BLOGSND.ELF";
const compositor_elf_disk_path: [*:0]const u16 = &[_:0]u16{
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'C', 'O', 'M', 'P', 'O', 'S', '.', 'E', 'L', 'F',
};
const compositor_elf_disk_path_log = "\\EFI\\BOOT\\COMPOS.ELF";
const gpu_compositor_elf_disk_path: [*:0]const u16 = &[_:0]u16{
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'G', 'P', 'U', 'C', 'O', 'M', 'P', '.', 'E', 'L', 'F',
};
const gpu_compositor_elf_disk_path_log = "\\EFI\\BOOT\\GPUCOMP.ELF";
const mouse_button_demo_elf_disk_path: [*:0]const u16 = &[_:0]u16{
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'M', 'B', 'T', 'N', 'D', 'E', 'M', 'O', '.', 'E', 'L', 'F',
};
const mouse_button_demo_elf_disk_path_log = "\\EFI\\BOOT\\MBTNDEMO.ELF";
const keyboard_ascii_demo_elf_disk_path: [*:0]const u16 = &[_:0]u16{
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'K', 'E', 'Y', 'B', 'D', 'E', 'M', 'O', '.', 'E', 'L', 'F',
};
const keyboard_ascii_demo_elf_disk_path_log = "\\EFI\\BOOT\\KEYBDEMO.ELF";
const taskbar_elf_disk_path: [*:0]const u16 = &[_:0]u16{
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'T', 'A', 'S', 'K', 'B', 'A', 'R', '.', 'E', 'L', 'F',
};
const taskbar_elf_disk_path_log = "\\EFI\\BOOT\\TASKBAR.ELF";
const vfs_elf_disk_path: [*:0]const u16 = &[_:0]u16{
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'V', 'F', 'S', '.', 'E', 'L', 'F',
};
const vfs_elf_disk_path_log = "\\EFI\\BOOT\\VFS.ELF";
const init_elf_disk_path: [*:0]const u16 = &[_:0]u16{
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'I', 'N', 'I', 'T', 'A', 'P', 'P', '.', 'E', 'L', 'F',
};
const init_elf_disk_path_log = "\\EFI\\BOOT\\INITAPP.ELF";
const bootfs_image_disk_path: [*:0]const u16 = &[_:0]u16{
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'B', 'O', 'O', 'T', 'F', 'S', '.', 'I', 'M', 'G',
};
const bootfs_image_disk_path_log = "\\EFI\\BOOT\\BOOTFS.IMG";
const boot_log_max_bytes: usize = 32 * 1024;
const boot_log_page_header_bytes: usize = 8;
const boot_log_page_payload_bytes: usize = 4096 - boot_log_page_header_bytes;
const boot_log_status_offset: usize = 4;
const boot_log_status_mouse_queue_ready: u32 = 1 << 0;
const boot_log_status_keyboard_queue_ready: u32 = 1 << 1;
const mouse_shared_header_bytes: usize = 128;
const mouse_shared_log_max_bytes: usize = 4096 - mouse_shared_header_bytes;
const mouse_driver_config_magic: u64 = 0x4D4F5553; // "MOUS"
const keyboard_driver_config_magic: u64 = 0x4B455942; // "KEYB"
const virtio_gpu_config_magic: u64 = 0x56475055; // "VGPU"
const taskbar_config_magic: u64 = 0x54424152; // "TBAR"
const vfs_boot_config_magic: u64 = 0x5646_5343; // "VFSC"
const vfs_boot_config_version: u64 = 1;
const vfs_boot_config_flag_bootfs_present: u64 = 1 << 0;
const vfs_bootfs_root_object_id: u64 = 1;
const mouse_shared_magic: u64 = 0x4D534852; // "MSHR"
const keyboard_shared_magic: u64 = 0x4B534852; // "KSHR"
const taskbar_state_magic: u32 = 0x54425354; // "TBST"
const taskbar_command_magic: u32 = 0x5442434D; // "TBCM"
const taskbar_protocol_version: u16 = 1;
const taskbar_window_title_max_bytes: usize = 64;
const taskbar_entry_max: usize = 5;
const TaskbarEntryPage = extern struct {
    window_id: u32,
    flags: u32,
    title_len: u16,
    reserved0: u16 = 0,
    title: [taskbar_window_title_max_bytes]u8,
};
const TaskbarStatePage = extern struct {
    magic: u32,
    version: u16,
    entry_count: u16,
    seq: u64,
    entries: [taskbar_entry_max]TaskbarEntryPage,
};
const TaskbarCommandPage = extern struct {
    magic: u32,
    version: u16,
    command: u16,
    seq: u64,
    window_id: u32,
    reserved0: u32 = 0,
};

const debug_skip_exit_boot_services = false;
const debug_skip_cr3_switch = false;
const debug_trigger_page_fault_test = false;
const user_process_count: usize = kernel.process_count;
const user_thread_count: usize = scheduler.max_thread_slots;
const UserAddressSpace = capability.UserAddressSpace;
const init_process_index: usize = kernel.vfs_process_index + 1;
const init_principal: kernel.PrincipalId = @enumFromInt(init_process_index);
const dynamic_spawn_first_process_index: usize = init_process_index + 1;

comptime {
    if (init_process_index >= kernel.process_count) @compileError("init process index must fit process table");
    if (dynamic_spawn_first_process_index >= kernel.process_count) @compileError("spawnable process range must not be empty");
}

var user_spaces_storage: [user_process_count]UserAddressSpace align(4096) = [_]UserAddressSpace{.{}} ** user_process_count;
var user_spaces: []UserAddressSpace = user_spaces_storage[0..];
var global_free_list: kernel.FreePageList = .{};
var kernel_state_global: kernel.KernelState = undefined;
var kernel_state_ready = false;
export var user_return_saved_r10: u64 = 0;
export var user_return_saved_gprs: [15]u64 align(16) = [_]u64{0} ** 15;
const scheduler_perf_report_interval_ticks: u64 = 256;
const compositor_thread1_priority_hold_quanta: u64 = 6;
var syscall_identity_diag_count: u64 = 0;
var syscall_entry_diag_count: u64 = 0;
var scheduler_perf_last_switch_count: u64 = 0;
var uefi_mmap_buffer: [64 * 1024]u8 align(@alignOf(uefi.tables.MemoryDescriptor)) = undefined;
var uefi_exitbs_mmap_buffer: [64 * 1024]u8 align(@alignOf(uefi.tables.MemoryDescriptor)) = undefined;
var user_log_scratch: [user_log_max_bytes]u8 align(16) = undefined;
export var user_return_iret_frame: [5]u64 align(16) = [_]u64{0} ** 5;
var bootlog_entered_user_tick: u64 = 0;
var runtime_framebuffer_info: ?FramebufferInfo = null;
var boot_log_status_flags: u32 = 0;
var boot_log_status_page_paddr: u64 = 0;
var bootlog_gate_input_start_armed = false;
var bootlog_gate_input_start_tick: u64 = 0;
var bootlog_gate_start_thread0 = false;
var bootlog_gate_start_thread3 = false;
var bootlog_gate_start_thread2 = false;
var bootlog_gate_start_thread4 = false;
var bootlog_gate_start_thread5 = false;
var bootlog_gate_start_thread7 = false;
var startup_process0_shared_sent = false;
var startup_process2_window_sent = false;
var startup_process3_shared_sent = false;
var startup_process4_window_sent = false;
var startup_process5_window_sent = false;
var startup_process7_window_sent = false;
var startup_dynamic_window_sent_count: u8 = 0;
var startup_last_client_cap_tick: u64 = 0;
var boot_services_cache: ?*uefi.tables.BootServices = null;
var kernel_image_base_paddr: u64 = 0;
var kernel_image_size_bytes: usize = 0;
var post_exit_load_scratch: [8 * 1024 * 1024]u8 align(4096) = [_]u8{0} ** (8 * 1024 * 1024);
var post_exit_load_scratch_used: usize = 0;
var pie_user_launch_thread_available = false;
var pie_user_process6_initialized = false;
var pie_user_disk_image: ?[]const u8 = null;
var mouse_button_demo_launch_thread_available = false;
var keyboard_ascii_demo_launch_thread_available = false;
var mouse_button_demo_disk_image: ?[]const u8 = null;
var keyboard_ascii_demo_disk_image: ?[]const u8 = null;
var process2_launch_page_paddr: u64 = 0;
var process2_launch_stack_paddr: u64 = 0;
var process4_launch_page_paddr: u64 = 0;
var process4_launch_stack_paddr: u64 = 0;
var compositor_runtime_priority_requested = false;

const syscall_alloc_page: u64 = 0x1;
const syscall_map_page: u64 = 0x2;
const syscall_move_cap: u64 = 0x3;
const syscall_drop_present: u64 = 0x4;
const syscall_switch_thread: u64 = 0x5;
const syscall_send_cap: u64 = 0x6;
const syscall_revoke_tree: u64 = 0x7;
const syscall_grant_cap: u64 = 0x8;
const syscall_log: u64 = 0x9;
const syscall_recv_cap: u64 = 0xA;
const syscall_map_mmio: u64 = 0xB;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_create_window: u64 = 0xD;
const syscall_queue_submit: u64 = 0xE;
const syscall_queue_notify: u64 = 0xF;
const syscall_untyped_alloc: u64 = 0x10;
const syscall_untyped_retype_pages: u64 = 0x11;
const syscall_untyped_reset: u64 = 0x12;
const syscall_untyped_alloc_map_pages: u64 = 0x13;
const syscall_grant_caps_batch: u64 = 0x14;
const syscall_map_pages_batch: u64 = 0x15;
const syscall_launch_pie_user: u64 = 0x16;
const syscall_wait_event: u64 = 0x17;
const syscall_batch_max_pages: usize = 64;

const user_program_cfg: user_programs.Config = .{
    .syscall_alloc_page = syscall_alloc_page,
    .syscall_map_page = syscall_map_page,
    .syscall_move_cap = syscall_move_cap,
    .syscall_drop_present = syscall_drop_present,
    .syscall_switch_thread = syscall_switch_thread,
    .syscall_send_cap = syscall_send_cap,
    .syscall_revoke_tree = syscall_revoke_tree,
    .syscall_grant_cap = syscall_grant_cap,
    .syscall_log = syscall_log,
    .user_unmapped_test_va = user_unmapped_test_va,
    .user_dma_verify_va = user_dma_verify_va,
    .user_recovery_stop_va = user_recovery_stop_va,
};

const syscall_ok: u64 = 0;
const syscall_err_invalid = 1;
const syscall_err_not_ready = 2;
const syscall_err_alloc = 4;
const syscall_err_map = 5;
const syscall_err_move = 6;
const syscall_err_drop_present = 7;
const syscall_err_send = 8;
const syscall_err_endpoint = 9;
const syscall_err_revoke = 10;
const syscall_err_grant = 11;
const syscall_err_log = 12;
const syscall_err_empty = 13;
const syscall_alloc_map_drop_cap_flag: u64 = 0x2;

const MemoryStats = pmm.MemoryStats;

const CreatedUserProcess = struct {
    user_page: kernel.PageCapability,
    user_stack_page: kernel.PageCapability,
    thread_slot: usize,
};

const DynamicUserProcess = struct {
    principal: kernel.PrincipalId,
    process: CreatedUserProcess,
};

const MouseDriverProcessSetup = struct {
    process: CreatedUserProcess,
    runtime_stack_page: kernel.PageCapability,
    config_page: kernel.PageCapability,
    shared_page: kernel.PageCapability,
};

const BootLogConsoleProcessSetup = struct {
    process: CreatedUserProcess,
    boot_log_page: kernel.PageCapability,
    boot_log_stack_page: kernel.PageCapability,
    compositor_gpu_config_page: kernel.PageCapability,
};

const VfsProcessSetup = struct {
    process: CreatedUserProcess,
    config_page: kernel.PageCapability,
};

const VfsBootFsSetup = struct {
    first_page_paddr: u64,
    size_bytes: u64,
};

const MouseButtonDemoProcessSetup = struct {
    process: CreatedUserProcess,
};

const KeyboardDriverProcessSetup = struct {
    process: CreatedUserProcess,
    config_page: kernel.PageCapability,
    shared_page: kernel.PageCapability,
};

const KeyboardAsciiDemoProcessSetup = struct {
    process: CreatedUserProcess,
};

const TaskbarProcessSetup = struct {
    process: CreatedUserProcess,
};

const BootRuntimeMode = enum {
    DiskUser,
    FramebufferIpc,
    BootLogConsole,
    BootLogGateCompositor,
    MouseCompositor,
};

const BootRoleKind = enum {
    disk_user,
    draw_client,
    framebuffer_server,
    boot_log_console,
    mouse_driver,
    keyboard_driver,
    mouse_button_demo,
    keyboard_ascii_demo,
    taskbar,
    init,
};

const BootRoleDescriptor = struct {
    kind: BootRoleKind,
    principal: kernel.PrincipalId,
    activate_immediately: bool = false,
};

const UserBootProcessSetup = struct {
    user_pages: [user_process_count]?kernel.PageCapability = [_]?kernel.PageCapability{null} ** user_process_count,
    user_stack_pages: [user_process_count]?kernel.PageCapability = [_]?kernel.PageCapability{null} ** user_process_count,
    boot_log_console_page: ?kernel.PageCapability = null,
    boot_log_console_stack_page: ?kernel.PageCapability = null,
    vfs_config_page: ?kernel.PageCapability = null,
    mouse_driver_runtime_stack_page: ?kernel.PageCapability = null,
    mouse_driver_config_page: ?kernel.PageCapability = null,
    keyboard_driver_config_page: ?kernel.PageCapability = null,
    compositor_gpu_config_page: ?kernel.PageCapability = null,
    mouse_shared_page: ?kernel.PageCapability = null,
    keyboard_shared_page: ?kernel.PageCapability = null,

    fn setProcess(self: *UserBootProcessSetup, principal: kernel.PrincipalId, process: CreatedUserProcess) void {
        const idx = processIndex(principal) orelse unreachable;
        self.user_pages[idx] = process.user_page;
        self.user_stack_pages[idx] = process.user_stack_page;
    }

    fn setMouseDriver(self: *UserBootProcessSetup, setup: MouseDriverProcessSetup) void {
        self.setProcess(.Process0, setup.process);
        self.mouse_driver_runtime_stack_page = setup.runtime_stack_page;
        self.mouse_driver_config_page = setup.config_page;
        self.mouse_shared_page = setup.shared_page;
    }

    fn setKeyboardDriver(self: *UserBootProcessSetup, setup: KeyboardDriverProcessSetup) void {
        self.setProcess(.Process3, setup.process);
        self.keyboard_driver_config_page = setup.config_page;
        self.keyboard_shared_page = setup.shared_page;
    }

    fn setBootLogConsole(self: *UserBootProcessSetup, setup: BootLogConsoleProcessSetup) void {
        self.setProcess(.Process1, setup.process);
        self.boot_log_console_page = setup.boot_log_page;
        self.boot_log_console_stack_page = setup.boot_log_stack_page;
        self.compositor_gpu_config_page = setup.compositor_gpu_config_page;
    }

    fn setVfs(self: *UserBootProcessSetup, setup: VfsProcessSetup) void {
        self.setProcess(kernel.vfs_principal, setup.process);
        self.vfs_config_page = setup.config_page;
    }
};

const disk_user_boot_roles = [_]BootRoleDescriptor{
    .{ .kind = .disk_user, .principal = .Process0, .activate_immediately = true },
};

const framebuffer_ipc_boot_roles = [_]BootRoleDescriptor{
    .{ .kind = .draw_client, .principal = .Process0 },
    .{ .kind = .framebuffer_server, .principal = .Process1, .activate_immediately = true },
};

const boot_log_console_roles = [_]BootRoleDescriptor{
    .{ .kind = .boot_log_console, .principal = .Process1, .activate_immediately = true },
    .{ .kind = .keyboard_driver, .principal = .Process3, .activate_immediately = true },
    .{ .kind = .keyboard_ascii_demo, .principal = .Process4, .activate_immediately = true },
    .{ .kind = .init, .principal = init_principal, .activate_immediately = true },
};

const boot_log_gate_compositor_roles = [_]BootRoleDescriptor{
    .{ .kind = .mouse_driver, .principal = .Process0 },
    .{ .kind = .boot_log_console, .principal = .Process1, .activate_immediately = true },
    .{ .kind = .keyboard_driver, .principal = .Process3 },
    .{ .kind = .mouse_button_demo, .principal = .Process2 },
    .{ .kind = .keyboard_ascii_demo, .principal = .Process4 },
    .{ .kind = .init, .principal = init_principal, .activate_immediately = true },
};

const mouse_compositor_roles = [_]BootRoleDescriptor{
    .{ .kind = .mouse_driver, .principal = .Process0, .activate_immediately = true },
    .{ .kind = .boot_log_console, .principal = .Process1 },
    .{ .kind = .keyboard_driver, .principal = .Process3, .activate_immediately = true },
    .{ .kind = .mouse_button_demo, .principal = .Process2 },
    .{ .kind = .keyboard_ascii_demo, .principal = .Process4 },
    .{ .kind = .init, .principal = init_principal, .activate_immediately = true },
};

fn bootRoleDescriptorsForMode(mode: BootRuntimeMode) []const BootRoleDescriptor {
    return switch (mode) {
        .DiskUser => disk_user_boot_roles[0..],
        .FramebufferIpc => framebuffer_ipc_boot_roles[0..],
        .BootLogConsole => boot_log_console_roles[0..],
        .BootLogGateCompositor => boot_log_gate_compositor_roles[0..],
        .MouseCompositor => mouse_compositor_roles[0..],
    };
}

const FramebufferInfo = struct {
    paddr: u64,
    size_bytes: usize,
    width: u32,
    height: u32,
    pixels_per_scan_line: u32,
    pixel_format: u32,
    mode: u32,
};

const MmioPageWithOffset = struct {
    page_paddr: u64,
    page_offset: u64,
};

const MouseDriverConfig = struct {
    common: MmioPageWithOffset,
    notify: MmioPageWithOffset,
    isr: MmioPageWithOffset,
    device: MmioPageWithOffset,
    notify_off_multiplier: u64,
    queue_paddr0: u64 = 0,
    queue_paddr1: u64 = 0,
};

const VirtioGpuDriverConfig = MouseDriverConfig;

fn serialInit() void {
    serial.init();
}

fn earlyUefiWrite(msg: [*:0]const u16) void {
    const st = uefi.system_table;
    const con_out = st.con_out orelse return;
    _ = con_out.outputString(msg) catch {};
}

fn earlyUefiWriteAscii(msg: []const u8) void {
    var buf: [160:0]u16 = [_:0]u16{0} ** 160;
    var i: usize = 0;
    while (i < msg.len and i + 1 < buf.len) : (i += 1) {
        const ch = msg[i];
        buf[i] = if (ch == '\n') '\r' else ch;
        if (ch == '\n' and i + 2 < buf.len) {
            buf[i + 1] = '\n';
            i += 1;
        }
    }
    buf[@min(i, buf.len - 1)] = 0;
    earlyUefiWrite(&buf);
}

fn probeWriteLog(text: []const u8) void {
    if (enable_title_only_ready_logs and std.mem.startsWith(u8, text, "virtio-probe:")) return;
    serialWrite(text);
}

fn logReadyTitle(title: []const u8) void {
    serialWrite(title);
    serialWrite("\n");
}

fn framebufferBytesForModeInfo(info: *const uefi.protocol.GraphicsOutput.Mode.Info) u64 {
    return @as(u64, info.pixels_per_scan_line) * @as(u64, info.vertical_resolution) * 4;
}

fn selectFramebufferMode(
    gop: *uefi.protocol.GraphicsOutput,
    max_bytes: u64,
) ?u32 {
    var best_mode: ?u32 = null;
    var best_bytes: u64 = 0;

    var mode_id: u32 = 0;
    while (mode_id < gop.mode.max_mode) : (mode_id += 1) {
        const info = gop.queryMode(mode_id) catch continue;
        if (info.pixel_format == .blt_only) continue;

        const mode_bytes = framebufferBytesForModeInfo(info);
        if (mode_bytes > max_bytes) continue;
        if (mode_bytes >= best_bytes) {
            best_bytes = mode_bytes;
            best_mode = mode_id;
        }
    }
    return best_mode;
}

fn acquireFramebufferInfo(bs: *uefi.tables.BootServices) ?FramebufferInfo {
    const gop = bs.locateProtocol(uefi.protocol.GraphicsOutput, null) catch return null;
    const graphics = gop orelse return null;

    const chosen_mode = selectFramebufferMode(graphics, framebuffer_window_bytes) orelse graphics.mode.mode;
    if (chosen_mode != graphics.mode.mode) {
        graphics.setMode(chosen_mode) catch return null;
    }

    const mode = graphics.mode;
    const info = mode.info;
    if (info.pixel_format == .blt_only) return null;

    const size_bytes_u64 = framebufferBytesForModeInfo(info);
    if (size_bytes_u64 == 0 or size_bytes_u64 > framebuffer_window_bytes) return null;
    if (mode.frame_buffer_base >= four_gib) return null;
    if ((mode.frame_buffer_base & 0xFFF) != 0) return null;

    return .{
        .paddr = mode.frame_buffer_base,
        .size_bytes = @intCast(size_bytes_u64),
        .width = info.horizontal_resolution,
        .height = info.vertical_resolution,
        .pixels_per_scan_line = info.pixels_per_scan_line,
        .pixel_format = @intFromEnum(info.pixel_format),
        .mode = mode.mode,
    };
}

fn installInterruptTrampolines() void {
    x86_platform.installInterruptTrampolines(.{
        .syscall_stub = @intFromPtr(&traps.syscallHandlerStub),
        .page_fault_stub = @intFromPtr(&traps.pageFaultHandlerStub),
        .general_protection_stub = @intFromPtr(&traps.generalProtectionHandlerStub),
        .double_fault_stub = @intFromPtr(&traps.doubleFaultHandlerStub),
        .invalid_opcode_stub = @intFromPtr(&traps.invalidOpcodeHandlerStub),
        .invalid_tss_stub = @intFromPtr(&traps.invalidTssHandlerStub),
        .segment_not_present_stub = @intFromPtr(&traps.segmentNotPresentHandlerStub),
        .stack_segment_fault_stub = @intFromPtr(&traps.stackSegmentFaultHandlerStub),
        .timer_interrupt_stub = @intFromPtr(&traps.timerInterruptHandlerStub),
        .lapic_timer_vector = lapic_timer_vector,
    });
}

fn readCr2() u64 {
    return x86_platform.readCr2();
}

fn exceptionName(vec: u64) []const u8 {
    return switch (vec) {
        13 => "GENERAL PROTECTION",
        14 => "PAGE FAULT",
        else => "EXCEPTION",
    };
}

fn processIndex(principal: kernel.PrincipalId) ?usize {
    return kernel.processIndexFromPrincipal(principal);
}

fn principalLabel(principal: kernel.PrincipalId) []const u8 {
    return kernel.principalLabel(principal);
}

fn principalFromProcessSlot(raw: u64) ?kernel.PrincipalId {
    const idx = std.math.cast(usize, raw) orelse return null;
    return kernel.processPrincipalFromIndex(idx);
}

fn processPageForPrincipal(process_pages: []const ?kernel.PageCapability, principal: kernel.PrincipalId) ?kernel.PageCapability {
    const idx = processIndex(principal) orelse return null;
    if (idx >= process_pages.len) return null;
    return process_pages[idx];
}

fn threadSlotForPrincipal(principal: kernel.PrincipalId) ?usize {
    return scheduler.threadSlotForPrincipal(principal);
}

fn threadSlotForPrincipalOrHalt(principal: kernel.PrincipalId, role_label: []const u8) usize {
    return threadSlotForPrincipal(principal) orelse {
        serialWrite(role_label);
        serialWrite(" thread slot missing\n");
        while (true) asm volatile ("hlt");
    };
}

fn threadReadyForPrincipal(principal: kernel.PrincipalId) bool {
    const slot = threadSlotForPrincipal(principal) orelse return false;
    const ctx = getThreadContextConst(slot) orelse return false;
    return ctx.allocated and ctx.ready;
}

fn setThreadEntryForPrincipal(principal: kernel.PrincipalId, role_label: []const u8, entry: u64, rsp: ?u64) void {
    setThreadEntry(threadSlotForPrincipalOrHalt(principal, role_label), entry, rsp);
}

fn setThreadEntryIfReadyForPrincipal(principal: kernel.PrincipalId, role_label: []const u8, entry: u64, rsp: u64) void {
    setThreadEntryIfReady(threadSlotForPrincipalOrHalt(principal, role_label), entry, rsp);
}

fn dumpAllProcessCaps(state: *const kernel.KernelState) void {
    var i: usize = 0;
    while (i < user_process_count) : (i += 1) {
        const principal = kernel.processPrincipalFromIndex(i) orelse unreachable;
        capability.dumpPrincipalCaps(state, principal, principalLabel(principal));
    }
}

fn iommuReasonLabel(reason: kernel.IommuSyncReason) []const u8 {
    return switch (reason) {
        .grant_dma => "grant_dma",
        .grant_no_dma => "grant_no_dma",
        .move_from => "move_from",
        .move_to => "move_to",
        .revoke => "revoke",
    };
}

fn iommuAuditHook(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    paddr: u64,
    mapped: bool,
    reason: kernel.IommuSyncReason,
) void {
    _ = state;
    _ = principal;
    _ = paddr;
    _ = mapped;
    _ = reason;
}

fn threadLabel(thread_index: usize) []const u8 {
    const labels = comptime blk: {
        var items: [user_thread_count][]const u8 = undefined;
        for (0..user_thread_count) |i| {
            items[i] = std.fmt.comptimePrint("Thread{}", .{i});
        }
        break :blk items;
    };
    if (thread_index < labels.len) return labels[thread_index];
    return "Thread?";
}

fn logReadyThreadsSummary(current_thread: usize, next_thread: usize) void {
    if (scheduler.scheduler_probe_log_count >= scheduler_probe_log_max_lines) return;
    scheduler.scheduler_probe_log_count +%= 1;
    serialWrite("READY current=");
    printNumber(current_thread);
    serialWrite(" next=");
    printNumber(next_thread);
    serialWrite(" threads=");
    var idx: usize = 0;
    var first = true;
    while (idx < user_thread_count) : (idx += 1) {
        const ctx = getThreadContextConst(idx) orelse continue;
        if (!ctx.ready) continue;
        if (!first) serialWrite(",");
        first = false;
        printNumber(idx);
        serialWrite(":");
        serialWrite(principalLabel(ctx.owner_process));
    }
    if (first) serialWrite("none");
    serialWrite("\n");
    serialWrite("SWITCH ");
    serialWrite(threadLabel(current_thread));
    serialWrite(" -> ");
    serialWrite(threadLabel(next_thread));
    serialWrite("\n");
}

fn updateBootTimingFromUserLog(proc: kernel.PrincipalId, message: []const u8) void {
    boot_timing.updateFromUserLog(proc, message, scheduler.lapic_tick_count, serialWrite);
}

fn logSchedulerRaceSendCap(
    from: kernel.PrincipalId,
    to: ?kernel.PrincipalId,
    endpoint_id: u64,
    paddr: u64,
    reason: []const u8,
) void {
    scheduler.logRaceSendCap(
        .{ .write = serialWrite, .print_hex = printHex },
        scheduler_race_log_max_lines,
        from,
        to,
        endpoint_id,
        paddr,
        reason,
    );
}

fn logSchedulerRaceSwitch(current_thread: usize, target_thread: usize, reason: []const u8) void {
    scheduler.logRaceSwitch(
        .{ .write = serialWrite, .print_hex = printHex },
        scheduler_race_log_max_lines,
        current_thread,
        target_thread,
        reason,
    );
}

fn queueCapOpLabel(op: kernel.QueueOperation) []const u8 {
    return switch (op) {
        .submit => "submit",
        .notify => "notify",
    };
}

fn logQueueCapDeny(
    proc: kernel.PrincipalId,
    token: u64,
    device: kernel.DmaDeviceId,
    queue_index: u16,
    op: kernel.QueueOperation,
    err: anyerror,
) void {
    serialWrite("queue_cap deny proc=");
    serialWrite(principalLabel(proc));
    serialWrite(" op=");
    serialWrite(queueCapOpLabel(op));
    serialWrite(" device=");
    serialWrite(switch (device) {
        .virtio_gpu => "virtio_gpu",
        .virtio_input => "virtio_input",
    });
    serialWrite(" q=");
    printNumber(queue_index);
    serialWrite(" token=");
    printHex(token);
    serialWrite(" err=");
    serialWrite(@errorName(err));
    serialWrite("\n");
}
fn buildInitialUserTrapFrame() TrapFrame {
    var frame: TrapFrame = std.mem.zeroes(TrapFrame);
    frame.rip = user_va;
    frame.cs = @as(u64, gdt_user_code_selector) | 0x3;
    frame.rflags = user_entry_rflags;
    frame.rsp = user_entry_rsp;
    frame.ss = @as(u64, gdt_user_data_selector) | 0x3;
    return frame;
}

fn allocateThreadContext(owner_process: kernel.PrincipalId) ?usize {
    return scheduler.allocateThreadSlot(owner_process, user_spaces, buildInitialUserTrapFrame());
}

fn repairThreadContext(thread_index: usize) bool {
    return scheduler.repairThreadContextWithSpaces(thread_index, user_spaces, buildInitialUserTrapFrame());
}

fn sanitizeAllThreadContexts() void {
    scheduler.sanitizeAllThreadContextsWithSpaces(user_spaces, buildInitialUserTrapFrame());
}

fn rawOwnerTag(ctx: *const scheduler.ThreadContext) u8 {
    const raw: *const u8 = @ptrCast(&ctx.owner_process);
    return raw.*;
}

fn principalFromRawTag(raw: u8) ?kernel.PrincipalId {
    if (raw >= kernel.principal_count) return null;
    return @enumFromInt(raw);
}

fn expectedPrincipalForThread(thread_index: usize) ?kernel.PrincipalId {
    if (getThreadContextConst(thread_index)) |ctx| {
        if (ctx.allocated) return ctx.owner_process;
    }
    return null;
}

fn readCr0() u64 {
    var value: u64 = 0;
    asm volatile ("mov %%cr0, %[out]"
        : [out] "=r" (value),
    );
    return value;
}

fn writeCr0(value: u64) void {
    asm volatile ("mov %[value], %%cr0"
        :
        : [value] "r" (value),
        : .{ .memory = true });
}

fn readCr4() u64 {
    var value: u64 = 0;
    asm volatile ("mov %%cr4, %[out]"
        : [out] "=r" (value),
    );
    return value;
}

fn writeCr4(value: u64) void {
    asm volatile ("mov %[value], %%cr4"
        :
        : [value] "r" (value),
        : .{ .memory = true });
}

fn initFxStateSupport() void {
    var cr0 = readCr0();
    cr0 &= ~@as(u64, 1 << 2); // EM=0
    cr0 |= @as(u64, 1 << 1); // MP=1
    writeCr0(cr0);

    var cr4 = readCr4();
    cr4 |= @as(u64, (1 << 9) | (1 << 10)); // OSFXSR | OSXMMEXCPT
    writeCr4(cr4);

    asm volatile ("clts");
    asm volatile ("fninit");
    asm volatile ("fxsave64 (%[ptr])"
        :
        : [ptr] "r" (&scheduler.initial_fx_state),
        : .{ .memory = true });
}

pub export fn saveCurrentThreadFxState() callconv(.c) void {
    if (!kernel_state_ready) return;
    const ctx = getThreadContext(scheduler.current_thread_index) orelse return;
    if (!ctx.ready) return;
    asm volatile ("fxsave64 (%[ptr])"
        :
        : [ptr] "r" (&ctx.fx_state),
        : .{ .memory = true });
}

pub export fn restoreCurrentThreadFxState() callconv(.c) void {
    if (!kernel_state_ready) return;
    const ctx = getThreadContext(scheduler.current_thread_index) orelse return;
    if (!ctx.ready) return;
    asm volatile ("fxrstor64 (%[ptr])"
        :
        : [ptr] "r" (&ctx.fx_state),
        : .{ .memory = true });
}

pub export fn saveKernelInterruptFxState() callconv(.c) void {
    asm volatile ("fxsave64 (%[ptr])"
        :
        : [ptr] "r" (&scheduler.kernel_interrupt_fx_state),
        : .{ .memory = true });
}

pub export fn restoreKernelInterruptFxState() callconv(.c) void {
    asm volatile ("fxrstor64 (%[ptr])"
        :
        : [ptr] "r" (&scheduler.kernel_interrupt_fx_state),
        : .{ .memory = true });
}

fn launchPieUserThread(frame: *TrapFrame) u64 {
    const selector = frame.rdi;
    const target_thread: usize = switch (selector) {
        0 => blk: {
            if (!preparePieUserLaunchThread()) return syscall_err_invalid;
            if (!pie_user_launch_thread_available) return syscall_err_invalid;
            break :blk 6;
        },
        1 => blk: {
            if (!prepareMouseButtonDemoLaunchThread()) return syscall_err_invalid;
            if (!mouse_button_demo_launch_thread_available) return syscall_err_invalid;
            break :blk 2;
        },
        2 => blk: {
            if (!prepareKeyboardAsciiDemoLaunchThread()) return syscall_err_invalid;
            if (!keyboard_ascii_demo_launch_thread_available) return syscall_err_invalid;
            break :blk 4;
        },
        else => return syscall_err_invalid,
    };
    if (!setThreadReady(target_thread, true)) return syscall_err_invalid;
    if (!switchToThread(target_thread, frame, syscall_ok)) {
        return syscall_err_not_ready;
    }
    return syscall_ok;
}

fn isUserTrapFrame(frame: *const TrapFrame) bool {
    return ((frame.cs & 0x3) == 0x3) and ((frame.ss & 0x3) == 0x3);
}

fn getUserSpace(principal: kernel.PrincipalId) ?*UserAddressSpace {
    return user_vm.getUserSpace(principal);
}

fn haltLoop() noreturn {
    while (true) asm volatile ("hlt");
}

fn maybeLogSchedulerPerfTick() void {
    if (!enable_scheduler_perf_logs) return;
    const report = scheduler.takePerfReport(scheduler_perf_report_interval_ticks, &scheduler_perf_last_switch_count) orelse return;
    serialWrite("SCHED perf ticks=");
    printNumber(report.ticks);
    serialWrite(" proc1=");
    printNumber(report.process1_ticks);
    serialWrite(" thread1=");
    printNumber(report.thread1_ticks);
    serialWrite(" switches=");
    printNumber(report.switch_delta);
    serialWrite(" active=");
    serialWrite(if (report.compositor_priority_active) "1" else "0");
    serialWrite(" streak=");
    printNumber(report.compositor_priority_streak);
    serialWrite("\n");
}

fn bootlogGateHasPendingStartupThreads() bool {
    return bootlog_gate_start_thread0 or
        bootlog_gate_start_thread3 or
        bootlog_gate_start_thread2 or
        bootlog_gate_start_thread4 or
        bootlog_gate_start_thread5 or
        bootlog_gate_start_thread7;
}

fn isDynamicSpawnPrincipal(principal: kernel.PrincipalId) bool {
    const index = kernel.processIndexFromPrincipal(principal) orelse return false;
    return index >= dynamic_spawn_first_process_index;
}

fn refreshCompositorPriorityActive() void {
    scheduler.refreshCompositorPriorityActive(
        compositor_runtime_priority_requested,
        !bootlogGateHasPendingStartupThreads(),
    );
}

fn updateCompositorPriorityFromUserLog(proc: kernel.PrincipalId, message: []const u8) void {
    if (proc != .Process1) return;
    if (std.mem.eql(u8, message, "Compositor: drag boost on\n")) {
        compositor_runtime_priority_requested = true;
        refreshCompositorPriorityActive();
    } else if (std.mem.eql(u8, message, "Compositor: drag boost off\n")) {
        compositor_runtime_priority_requested = false;
        refreshCompositorPriorityActive();
    }
}

fn syscallPostSendCap(from: kernel.PrincipalId, to: kernel.PrincipalId, endpoint_id: u64) void {
    noteStartupClientCapSent(from, to, endpoint_id);
    if (deferred_compositor.launched and
        (from == .Process2 or from == .Process4 or from == .Process5 or isDynamicSpawnPrincipal(from)) and
        to == .Process1 and
        endpoint_id == kernel.endpoint_to_process1)
    {
        scheduler.compositor_thread1_priority_active = true;
    }
}

fn syscallOnMailboxReceive(proc: kernel.PrincipalId, received: u64) void {
    if (proc == .Process1 and received >= 0x1000 and !compositor_runtime_priority_requested) {
        refreshCompositorPriorityActive();
    }
}

fn syscallHandleUserLog(frame: *TrapFrame, proc: kernel.PrincipalId, msg: []const u8) void {
    updateCompositorPriorityFromUserLog(proc, msg);
    updateBootLogQueueReadyStatusFromUserLog(proc, msg);
    tryLaunchDeferredCompositorFromLog(frame, proc, msg);
}

fn haltLoopTrapHook() noreturn {
    haltLoop();
}

fn loadGdtAndReloadSegments() void {
    x86_platform.loadGdtAndReloadSegments();
}

fn initIdtPageFaultOnly() void {
    installInterruptTrampolines();
}

fn triggerPageFaultTest() noreturn {
    serialWrite("triggering page fault test...\n");
    const bad_ptr: *volatile u64 = @ptrFromInt(0xFFFF_8000_0000_0000);
    bad_ptr.* = 0xDEADBEEF;
    while (true) {
        asm volatile ("hlt");
    }
}

fn printNumber(value: anytype) void {
    serial.printNumber(value);
    var buf: [32]u8 = undefined;
    const s = std.fmt.bufPrint(buf[0..], "{}", .{value}) catch return;
    kernel_log.appendText(s);
}

fn printNumberU64(value: u64) void {
    printNumber(value);
}

fn printHex(value: u64) void {
    const hex = "0123456789abcdef";
    serial.write("0x");
    kernel_log.appendText("0x");
    var started = false;
    var shift: u6 = 60;
    while (true) {
        const nibble: u4 = @intCast((value >> shift) & 0xF);
        if (started or nibble != 0 or shift == 0) {
            serial.writeByte(hex[nibble]);
            kernel_log.appendByte(hex[nibble]);
            started = true;
        }
        if (shift == 0) break;
        shift -= 4;
    }
}

const ExitBootResult = enum {
    success,
    failed,
};

fn exitBootServicesWithRetry() ExitBootResult {
    const st = uefi.system_table;
    const bs = st.boot_services orelse return .failed;

    var attempt: usize = 0;
    while (attempt < 8) : (attempt += 1) {
        const mmap = bs.getMemoryMap(uefi_exitbs_mmap_buffer[0..]) catch return .failed;
        bs.exitBootServices(uefi.handle, mmap.info.key) catch |err| switch (err) {
            // map key 競合は再取得で回復可能。
            error.InvalidParameter => continue,
            else => return .failed,
        };

        // UEFI仕様: ExitBootServices 後に該当ポインタを null 化し、CRC を再計算する。
        st.console_in_handle = null;
        st.con_in = null;
        st.console_out_handle = null;
        st.con_out = null;
        st.standard_error_handle = null;
        st.std_err = null;
        st.boot_services = null;
        st.hdr.crc32 = 0;
        const st_bytes = @as([*]u8, @ptrCast(st))[0..@as(usize, st.hdr.header_size)];
        st.hdr.crc32 = std.hash.Crc32.hash(st_bytes);

        return .success;
    }

    return .failed;
}

fn writeCr3(value: u64) void {
    kernel_vm.writeCr3(value);
}

fn readCr3() u64 {
    return kernel_vm.readCr3();
}

fn invlpg(addr: u64) void {
    kernel_vm.invlpg(addr);
}

fn installIdentityPageTables0To1GiB() bool {
    return kernel_vm.installIdentityPageTables0To1GiB();
}

fn hardenKernelMappingsSupervisorOnly() void {
    kernel_vm.hardenKernelMappingsSupervisorOnly();
}

fn buildUserAddressSpaceFromCapabilities(
    state: *const kernel.KernelState,
    principal: kernel.PrincipalId,
    user_page: kernel.PageCapability,
    user_stack_page: kernel.PageCapability,
) bool {
    const table = state.getTableConst(principal);
    const user_cap = table.find(user_page.paddr) orelse return false;
    const stack_cap = table.find(user_stack_page.paddr) orelse return false;
    if (!user_cap.rights.cpu_read or !user_cap.rights.cpu_write) return false;
    if (!stack_cap.rights.cpu_read or !stack_cap.rights.cpu_write) return false;
    return user_vm.buildUserAddressSpace(principal, user_cap.paddr, stack_cap.paddr);
}

fn tryCreateUserProcess(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    role_label: []const u8,
) ?CreatedUserProcess {
    if (!state.ensureProcessDescriptor(principal, role_label)) {
        serialWrite("ensureProcessDescriptor failed for ");
        serialWrite(role_label);
        serialWrite("\n");
        return null;
    }
    const user_page = state.allocPageTo(principal, &global_free_list) catch |err| {
        serialWrite("allocPageTo for ");
        serialWrite(role_label);
        serialWrite(" user map failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        return null;
    };
    const user_stack_page = state.allocPageTo(principal, &global_free_list) catch |err| {
        serialWrite("allocPageTo for ");
        serialWrite(role_label);
        serialWrite(" user stack failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        return null;
    };
    if (!buildUserAddressSpaceFromCapabilities(state, principal, user_page, user_stack_page)) {
        serialWrite(role_label);
        serialWrite(" process page table build failed\n");
        return null;
    }
    const thread_slot = allocateThreadContext(principal) orelse {
        serialWrite(role_label);
        serialWrite(" thread context init failed\n");
        return null;
    };
    return .{
        .user_page = user_page,
        .user_stack_page = user_stack_page,
        .thread_slot = thread_slot,
    };
}

fn createUserProcess(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    role_label: []const u8,
) CreatedUserProcess {
    return tryCreateUserProcess(state, principal, role_label) orelse {
        while (true) asm volatile ("hlt");
    };
}

fn tryCreateDynamicUserProcess(
    state: *kernel.KernelState,
    min_process_index: usize,
    role_label: []const u8,
) ?DynamicUserProcess {
    var process_index = min_process_index;
    while (process_index < user_process_count) : (process_index += 1) {
        const principal = kernel.processPrincipalFromIndex(process_index) orelse continue;
        if (state.hasActivePrincipal(principal)) continue;
        const process = tryCreateUserProcess(state, principal, role_label) orelse return null;
        return .{
            .principal = principal,
            .process = process,
        };
    }
    return null;
}

fn allocPageForProcessOrHalt(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    role_label: []const u8,
    page_label: []const u8,
) kernel.PageCapability {
    return state.allocPageTo(principal, &global_free_list) catch |err| {
        serialWrite("allocPageTo for ");
        serialWrite(role_label);
        serialWrite(" ");
        serialWrite(page_label);
        serialWrite(" failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
}

fn mapUserLinearRegionOrHalt(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddr_start: u64,
    size_bytes: usize,
    writable: bool,
    what: []const u8,
) void {
    if (!user_vm.mapUserLinearRegion(principal, va_start, paddr_start, size_bytes, writable)) {
        serialWrite(what);
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    }
}

fn userPageRights(writable: bool) kernel.Rights {
    return .{
        .cpu_read = true,
        .cpu_write = writable,
        .dma = false,
    };
}

fn installPageForProcessOrHalt(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    page: kernel.PageCapability,
    writable: bool,
    role_label: []const u8,
    page_label: []const u8,
) void {
    state.installCap(principal, page.paddr, userPageRights(writable)) catch |err| {
        serialWrite(role_label);
        serialWrite(" ");
        serialWrite(page_label);
        serialWrite(" cap install failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
}

fn grantPageForProcessOrHalt(
    state: *kernel.KernelState,
    from_principal: kernel.PrincipalId,
    to_principal: kernel.PrincipalId,
    page: kernel.PageCapability,
    writable: bool,
    role_label: []const u8,
    page_label: []const u8,
) void {
    state.grantCap(from_principal, to_principal, page.paddr, userPageRights(writable)) catch |err| {
        serialWrite(role_label);
        serialWrite(" ");
        serialWrite(page_label);
        serialWrite(" cap grant failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
}

fn mapUserPageOrHalt(
    principal: kernel.PrincipalId,
    va_start: u64,
    page: kernel.PageCapability,
    writable: bool,
    role_label: []const u8,
    page_label: []const u8,
) void {
    if (!user_vm.mapUserLinearRegion(principal, va_start, page.paddr, 4096, writable)) {
        serialWrite(role_label);
        serialWrite(" ");
        serialWrite(page_label);
        serialWrite(" map failed\n");
        while (true) asm volatile ("hlt");
    }
}

fn allocAndMapOwnedPageForProcessOrHalt(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    role_label: []const u8,
    page_label: []const u8,
    va_start: u64,
    writable: bool,
) kernel.PageCapability {
    const page = allocPageForProcessOrHalt(state, principal, role_label, page_label);
    mapUserPageOrHalt(principal, va_start, page, writable, role_label, page_label);
    return page;
}

fn installVfsEndpointsOrHalt(state: *kernel.KernelState) void {
    state.installServiceEndpointForActiveProcesses(kernel.endpoint_to_vfs, kernel.vfs_principal) catch |err| {
        serialWrite("VFS endpoint install failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
}

fn installSpawnCompatibilityEndpoints(state: *kernel.KernelState, principal: kernel.PrincipalId) !void {
    if (state.hasActivePrincipal(kernel.vfs_principal)) {
        try state.installEndpoint(principal, kernel.endpoint_to_vfs, kernel.vfs_principal);
    }
    if (principal != .Process1 and state.hasActivePrincipal(.Process1)) {
        try state.installEndpoint(principal, kernel.endpoint_to_process1, .Process1);
    }
}

fn installSpawnBootstrapPage(
    caller: kernel.PrincipalId,
    child: kernel.PrincipalId,
    source_va: u64,
    target_va: u64,
    descriptor_flags: u64,
) u64 {
    if (source_va == 0 or target_va == 0) {
        serialWrite("spawn_exec bootstrap missing va\n");
        return syscall_err_invalid;
    }
    if ((source_va & 0xFFF) != 0 or (target_va & 0xFFF) != 0) {
        serialWrite("spawn_exec bootstrap unaligned va\n");
        return syscall_err_invalid;
    }
    if ((descriptor_flags & ~process_abi.spawn_flag_bootstrap_page_writable) != 0) {
        serialWrite("spawn_exec bootstrap bad flags\n");
        return syscall_err_invalid;
    }

    const bootstrap_paddr = capability.lookupUserMappedPaddrForVa(caller, source_va) orelse {
        serialWrite("spawn_exec bootstrap lookup fail\n");
        return syscall_err_invalid;
    };
    const bootstrap_cap = kernel_state_global.getTableConst(caller).find(bootstrap_paddr) orelse {
        serialWrite("spawn_exec bootstrap cap missing\n");
        return syscall_err_invalid;
    };
    const writable = (descriptor_flags & process_abi.spawn_flag_bootstrap_page_writable) != 0;
    if (!bootstrap_cap.rights.cpu_read) {
        serialWrite("spawn_exec bootstrap no read right\n");
        return syscall_err_invalid;
    }
    if (writable and !bootstrap_cap.rights.cpu_write) {
        serialWrite("spawn_exec bootstrap no write right\n");
        return syscall_err_invalid;
    }
    kernel_state_global.grantCap(caller, child, bootstrap_paddr, .{
        .cpu_read = true,
        .cpu_write = writable,
        .dma = false,
    }) catch |err| {
        serialWrite("spawn_exec bootstrap grant failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        return syscall_err_grant;
    };
    if (!user_vm.mapUserLinearRegion(child, target_va, bootstrap_paddr, 4096, writable)) {
        serialWrite("spawn_exec bootstrap map fail\n");
        return syscall_err_map;
    }
    return syscall_ok;
}

fn installAndMapPageForProcessOrHalt(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    page: kernel.PageCapability,
    va_start: u64,
    writable: bool,
    role_label: []const u8,
    page_label: []const u8,
) void {
    installPageForProcessOrHalt(state, principal, page, writable, role_label, page_label);
    mapUserPageOrHalt(principal, va_start, page, writable, role_label, page_label);
}

fn grantAndMapPageForProcessOrHalt(
    state: *kernel.KernelState,
    from_principal: kernel.PrincipalId,
    to_principal: kernel.PrincipalId,
    page: kernel.PageCapability,
    va_start: u64,
    writable: bool,
    role_label: []const u8,
    page_label: []const u8,
) void {
    grantPageForProcessOrHalt(state, from_principal, to_principal, page, writable, role_label, page_label);
    mapUserPageOrHalt(to_principal, va_start, page, writable, role_label, page_label);
}

fn setupMouseDriverProcess(state: *kernel.KernelState, cfg: MouseDriverConfig) MouseDriverProcessSetup {
    const process = createUserProcess(state, .Process0, "mouse driver");
    const runtime_stack_page = allocPageForProcessOrHalt(state, .Process0, "mouse driver", "runtime stack");
    const config_page = allocPageForProcessOrHalt(state, .Process0, "mouse driver", "config page");
    const shared_page = allocPageForProcessOrHalt(state, .Process0, "mouse driver", "shared page");
    const queue_page0 = allocPageForProcessOrHalt(state, .Process0, "mouse driver", "queue page0");
    const queue_page1 = allocPageForProcessOrHalt(state, .Process0, "mouse driver", "queue page1");

    mapUserLinearRegionOrHalt(.Process0, mouse_driver_config_va, config_page.paddr, 4096, true, "mouse driver config page map failed");
    mapUserLinearRegionOrHalt(.Process0, mouse_shared_driver_va, shared_page.paddr, 4096, true, "mouse shared page map failed (Process0)");
    mapUserLinearRegionOrHalt(.Process0, mouse_driver_queue_page0_va, queue_page0.paddr, 4096, true, "mouse driver queue page0 map failed");
    mapUserLinearRegionOrHalt(.Process0, mouse_driver_queue_page0_va + 4096, queue_page1.paddr, 4096, true, "mouse driver queue page1 map failed");
    mapUserLinearRegionOrHalt(.Process0, boot_log_console_stack_page_va, runtime_stack_page.paddr, 4096, true, "mouse driver runtime stack map failed");

    const mmio_rw_rights = kernel.Rights{ .cpu_read = true, .cpu_write = true, .dma = false };
    const mmio_ro_rights = kernel.Rights{ .cpu_read = true, .cpu_write = false, .dma = false };
    state.installCap(.Process0, cfg.common.page_paddr, mmio_rw_rights) catch |err| {
        serialWrite("mouse driver install common cap failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    state.installCap(.Process0, cfg.notify.page_paddr, mmio_rw_rights) catch |err| {
        serialWrite("mouse driver install notify cap failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    if (cfg.isr.page_paddr != 0) {
        state.installCap(.Process0, cfg.isr.page_paddr, mmio_ro_rights) catch |err| {
            serialWrite("mouse driver install isr cap failed: ");
            serialWrite(@errorName(err));
            serialWrite("\n");
            while (true) asm volatile ("hlt");
        };
    }
    if (cfg.device.page_paddr != 0) {
        state.installCap(.Process0, cfg.device.page_paddr, mmio_ro_rights) catch |err| {
            serialWrite("mouse driver install device cap failed: ");
            serialWrite(@errorName(err));
            serialWrite("\n");
            while (true) asm volatile ("hlt");
        };
    }
    var runtime_cfg = cfg;
    runtime_cfg.queue_paddr0 = queue_page0.paddr;
    runtime_cfg.queue_paddr1 = queue_page1.paddr;
    const mouse_queue_submit_token = state.queueCapGrantStage2(.Process0, .virtio_input, 0, true, false) catch |err| {
        serialWrite("mouse driver queue submit cap grant failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    const mouse_queue_notify_token = state.queueCapGrantStage2(.Process0, .virtio_input, 0, false, true) catch |err| {
        serialWrite("mouse driver queue notify cap grant failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    publishMouseDriverConfigPage(config_page.paddr, runtime_cfg, shared_page.paddr, mouse_queue_submit_token, mouse_queue_notify_token);

    return .{
        .process = process,
        .runtime_stack_page = runtime_stack_page,
        .config_page = config_page,
        .shared_page = shared_page,
    };
}

fn publishVfsConfigPage(user_page_paddr: u64, root_mount_token: u64, bootfs: ?VfsBootFsSetup) void {
    const words: [*]volatile u64 = @ptrFromInt(user_page_paddr);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        words[i] = 0;
    }
    words[0] = vfs_boot_config_magic;
    words[1] = vfs_boot_config_version;
    words[2] = kernel.endpoint_to_vfs;
    words[3] = root_mount_token;
    words[4] = if (bootfs) |info| info.first_page_paddr else 0;
    words[5] = if (bootfs) |info| info.size_bytes else 0;
    words[6] = if (bootfs != null) vfs_boot_config_flag_bootfs_present else 0;
}

fn mapBootFsImageIntoVfsOrHalt(
    state: *kernel.KernelState,
    image: []const u8,
) VfsBootFsSetup {
    const page_count = std.math.divCeil(usize, image.len, 4096) catch unreachable;
    var first_page_paddr: u64 = 0;
    var copied: usize = 0;
    var page_index: usize = 0;
    while (page_index < page_count) : (page_index += 1) {
        const page = allocPageForProcessOrHalt(state, kernel.vfs_principal, "VFS", "bootfs image page");
        if (first_page_paddr == 0) first_page_paddr = page.paddr;
        const dst: [*]u8 = @ptrFromInt(page.paddr);
        @memset(dst[0..4096], 0);
        const remaining = image.len - copied;
        const chunk_len: usize = if (remaining > 4096) 4096 else remaining;
        @memcpy(dst[0..chunk_len], image[copied .. copied + chunk_len]);
        mapUserPageOrHalt(
            kernel.vfs_principal,
            vfs_bootfs_image_va + @as(u64, @intCast(page_index)) * 4096,
            page,
            false,
            "VFS",
            "bootfs image page",
        );
        copied += chunk_len;
    }
    return .{
        .first_page_paddr = first_page_paddr,
        .size_bytes = image.len,
    };
}

fn setupVfsProcess(state: *kernel.KernelState, bootfs_image: ?[]const u8) VfsProcessSetup {
    _ = state.ensureVfsProcess();
    const process = createUserProcess(state, kernel.vfs_principal, "VFS");
    const config_page = allocAndMapOwnedPageForProcessOrHalt(state, kernel.vfs_principal, "VFS", "config page", vfs_config_va, true);
    const root_mount_cap = state.installFsCap(kernel.vfs_principal, vfs_bootfs_root_object_id, .mount, .{
        .lookup = true,
        .read = true,
        .readdir = true,
        .stat = true,
        .mount = true,
        .grant = true,
        .admin = true,
    }) catch |err| {
        serialWrite("VFS root mount cap install failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    const bootfs_setup = if (bootfs_image) |image| mapBootFsImageIntoVfsOrHalt(state, image) else null;
    publishVfsConfigPage(config_page.paddr, fs_abi.encodeCapToken(root_mount_cap), bootfs_setup);
    return .{
        .process = process,
        .config_page = config_page,
    };
}

fn setupKeyboardDriverProcess(state: *kernel.KernelState, cfg: MouseDriverConfig) KeyboardDriverProcessSetup {
    const process = createUserProcess(state, .Process3, "keyboard driver");
    const config_page = allocPageForProcessOrHalt(state, .Process3, "keyboard driver", "config page");
    const shared_page = allocPageForProcessOrHalt(state, .Process3, "keyboard driver", "shared page");
    const queue_page0 = allocPageForProcessOrHalt(state, .Process3, "keyboard driver", "queue page0");
    const queue_page1 = allocPageForProcessOrHalt(state, .Process3, "keyboard driver", "queue page1");
    mapUserLinearRegionOrHalt(.Process3, keyboard_driver_config_va, config_page.paddr, 4096, true, "keyboard driver config page map failed");
    mapUserLinearRegionOrHalt(.Process3, keyboard_shared_driver_va, shared_page.paddr, 4096, true, "keyboard shared page map failed (Process3)");
    mapUserLinearRegionOrHalt(.Process3, keyboard_driver_queue_page0_va, queue_page0.paddr, 4096, true, "keyboard driver queue page0 map failed");
    mapUserLinearRegionOrHalt(.Process3, keyboard_driver_queue_page0_va + 4096, queue_page1.paddr, 4096, true, "keyboard driver queue page1 map failed");

    const mmio_rw_rights = kernel.Rights{ .cpu_read = true, .cpu_write = true, .dma = false };
    const mmio_ro_rights = kernel.Rights{ .cpu_read = true, .cpu_write = false, .dma = false };
    state.installCap(.Process3, cfg.common.page_paddr, mmio_rw_rights) catch |err| {
        serialWrite("keyboard driver install common cap failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    state.installCap(.Process3, cfg.notify.page_paddr, mmio_rw_rights) catch |err| {
        serialWrite("keyboard driver install notify cap failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    if (cfg.isr.page_paddr != 0) {
        state.installCap(.Process3, cfg.isr.page_paddr, mmio_ro_rights) catch |err| {
            serialWrite("keyboard driver install isr cap failed: ");
            serialWrite(@errorName(err));
            serialWrite("\n");
            while (true) asm volatile ("hlt");
        };
    }
    if (cfg.device.page_paddr != 0) {
        state.installCap(.Process3, cfg.device.page_paddr, mmio_ro_rights) catch |err| {
            serialWrite("keyboard driver install device cap failed: ");
            serialWrite(@errorName(err));
            serialWrite("\n");
            while (true) asm volatile ("hlt");
        };
    }
    var runtime_cfg = cfg;
    runtime_cfg.queue_paddr0 = queue_page0.paddr;
    runtime_cfg.queue_paddr1 = queue_page1.paddr;
    const keyboard_queue_submit_token = state.queueCapGrantStage2(.Process3, .virtio_input, 0, true, false) catch |err| {
        serialWrite("keyboard driver queue submit cap grant failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    const keyboard_queue_notify_token = state.queueCapGrantStage2(.Process3, .virtio_input, 0, false, true) catch |err| {
        serialWrite("keyboard driver queue notify cap grant failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    publishKeyboardDriverConfigPage(config_page.paddr, runtime_cfg, shared_page.paddr, keyboard_queue_submit_token, keyboard_queue_notify_token);
    publishKeyboardSharedPage(shared_page.paddr);

    return .{
        .process = process,
        .config_page = config_page,
        .shared_page = shared_page,
    };
}

fn setupBootLogConsoleProcess(state: *kernel.KernelState, gpu_cfg: ?VirtioGpuDriverConfig) BootLogConsoleProcessSetup {
    const process = createUserProcess(state, .Process1, "boot log console");
    const boot_log_page = allocPageForProcessOrHalt(state, .Process1, "boot log", "page");
    const boot_log_stack_page = allocPageForProcessOrHalt(state, .Process1, "boot log console", "stack page");
    const compositor_gpu_config_page = allocPageForProcessOrHalt(state, .Process1, "compositor", "virtio gpu config page");
    mapUserLinearRegionOrHalt(.Process1, compositor_gpu_config_va, compositor_gpu_config_page.paddr, 4096, true, "compositor gpu config page map failed");

    const mmio_rw_rights = kernel.Rights{ .cpu_read = true, .cpu_write = true, .dma = false };
    const mmio_ro_rights = kernel.Rights{ .cpu_read = true, .cpu_write = false, .dma = false };
    var gpu_control_submit_token: u64 = 0;
    var gpu_control_notify_token: u64 = 0;
    var gpu_cursor_submit_token: u64 = 0;
    var gpu_cursor_notify_token: u64 = 0;
    if (gpu_cfg) |cfg| {
        state.installCap(.Process1, cfg.common.page_paddr, mmio_rw_rights) catch |err| {
            serialWrite("compositor install common cap failed: ");
            serialWrite(@errorName(err));
            serialWrite("\n");
            while (true) asm volatile ("hlt");
        };
        state.installCap(.Process1, cfg.notify.page_paddr, mmio_rw_rights) catch |err| {
            serialWrite("compositor install notify cap failed: ");
            serialWrite(@errorName(err));
            serialWrite("\n");
            while (true) asm volatile ("hlt");
        };
        if (cfg.isr.page_paddr != 0) {
            state.installCap(.Process1, cfg.isr.page_paddr, mmio_ro_rights) catch |err| {
                serialWrite("compositor install isr cap failed: ");
                serialWrite(@errorName(err));
                serialWrite("\n");
                while (true) asm volatile ("hlt");
            };
        }
        if (cfg.device.page_paddr != 0) {
            state.installCap(.Process1, cfg.device.page_paddr, mmio_ro_rights) catch |err| {
                serialWrite("compositor install device cap failed: ");
                serialWrite(@errorName(err));
                serialWrite("\n");
                while (true) asm volatile ("hlt");
            };
        }
        gpu_control_submit_token = state.queueCapGrantStage2(.Process1, .virtio_gpu, 0, true, false) catch |err| {
            serialWrite("compositor queue control submit cap grant failed: ");
            serialWrite(@errorName(err));
            serialWrite("\n");
            while (true) asm volatile ("hlt");
        };
        gpu_control_notify_token = state.queueCapGrantStage2(.Process1, .virtio_gpu, 0, false, true) catch |err| {
            serialWrite("compositor queue control notify cap grant failed: ");
            serialWrite(@errorName(err));
            serialWrite("\n");
            while (true) asm volatile ("hlt");
        };
        gpu_cursor_submit_token = state.queueCapGrantStage2(.Process1, .virtio_gpu, 1, true, false) catch |err| {
            serialWrite("compositor queue cursor submit cap grant failed: ");
            serialWrite(@errorName(err));
            serialWrite("\n");
            while (true) asm volatile ("hlt");
        };
        gpu_cursor_notify_token = state.queueCapGrantStage2(.Process1, .virtio_gpu, 1, false, true) catch |err| {
            serialWrite("compositor queue cursor notify cap grant failed: ");
            serialWrite(@errorName(err));
            serialWrite("\n");
            while (true) asm volatile ("hlt");
        };
    }
    publishVirtioGpuConfigPage(compositor_gpu_config_page.paddr, gpu_cfg, gpu_control_submit_token, gpu_control_notify_token, gpu_cursor_submit_token, gpu_cursor_notify_token);
    return .{
        .process = process,
        .boot_log_page = boot_log_page,
        .boot_log_stack_page = boot_log_stack_page,
        .compositor_gpu_config_page = compositor_gpu_config_page,
    };
}

fn setupMouseButtonDemoProcess(
    state: *kernel.KernelState,
    mouse_shared_page: kernel.PageCapability,
) MouseButtonDemoProcessSetup {
    const process = createUserProcess(state, .Process2, "mouse button demo");
    _ = setThreadReady(process.thread_slot, false);
    installAndMapPageForProcessOrHalt(state, .Process2, mouse_shared_page, mouse_shared_draw_va, false, "mouse button demo", "mouse shared page");
    return .{ .process = process };
}

fn setupTaskbarProcess(
    state: *kernel.KernelState,
    mouse_shared_page: kernel.PageCapability,
) TaskbarProcessSetup {
    const process = createUserProcess(state, .Process7, "taskbar");
    const config_page = allocAndMapOwnedPageForProcessOrHalt(state, .Process7, "taskbar", "config page", taskbar_config_va, true);
    const state_page = allocAndMapOwnedPageForProcessOrHalt(state, .Process7, "taskbar", "state page", taskbar_state_shared_va, false);
    const command_page = allocAndMapOwnedPageForProcessOrHalt(state, .Process7, "taskbar", "command page", taskbar_command_shared_va, true);
    installAndMapPageForProcessOrHalt(state, .Process7, mouse_shared_page, mouse_shared_draw_va, false, "taskbar", "mouse shared page");
    installAndMapPageForProcessOrHalt(state, .Process1, state_page, taskbar_state_shared_va, true, "compositor", "taskbar state page");
    installAndMapPageForProcessOrHalt(state, .Process1, command_page, taskbar_command_shared_va, true, "compositor", "taskbar command page");

    publishTaskbarConfigPage(config_page.paddr, runtime_framebuffer_info.?);
    publishTaskbarStatePage(state_page.paddr);
    publishTaskbarCommandPage(command_page.paddr);
    return .{ .process = process };
}

fn setupTaskbarBootstrapForInit(
    state: *kernel.KernelState,
    init_process_principal: kernel.PrincipalId,
    mouse_shared_page: kernel.PageCapability,
) void {
    const config_page = allocAndMapOwnedPageForProcessOrHalt(state, init_process_principal, "init", "taskbar config page", init_taskbar_config_source_va, true);
    const state_page = allocAndMapOwnedPageForProcessOrHalt(state, init_process_principal, "init", "taskbar state page", init_taskbar_state_source_va, false);
    const command_page = allocAndMapOwnedPageForProcessOrHalt(state, init_process_principal, "init", "taskbar command page", init_taskbar_command_source_va, true);
    installAndMapPageForProcessOrHalt(state, .Process1, state_page, taskbar_state_shared_va, true, "compositor", "taskbar state page");
    installAndMapPageForProcessOrHalt(state, .Process1, command_page, taskbar_command_shared_va, true, "compositor", "taskbar command page");
    grantAndMapPageForProcessOrHalt(state, .Process0, init_process_principal, mouse_shared_page, init_taskbar_mouse_source_va, false, "init", "taskbar mouse shared page");

    publishTaskbarConfigPage(config_page.paddr, runtime_framebuffer_info.?);
    publishTaskbarStatePage(state_page.paddr);
    publishTaskbarCommandPage(command_page.paddr);
}

fn setupKeyboardAsciiDemoProcess(
    state: *kernel.KernelState,
    keyboard_shared_page: ?kernel.PageCapability,
) KeyboardAsciiDemoProcessSetup {
    const process = createUserProcess(state, .Process4, "keyboard ascii demo");
    _ = setThreadReady(process.thread_slot, false);
    if (keyboard_shared_page) |shared_page| {
        grantAndMapPageForProcessOrHalt(state, .Process3, .Process4, shared_page, keyboard_shared_draw_va, false, "keyboard ascii demo", "keyboard shared page");
    } else {
        const fallback_page = allocAndMapOwnedPageForProcessOrHalt(state, .Process4, "keyboard ascii demo", "keyboard fallback shared page", keyboard_shared_draw_va, true);
        publishKeyboardSharedPage(fallback_page.paddr);
    }
    return .{ .process = process };
}

fn determineBootRuntimeMode(mouse_driver_cfg: ?MouseDriverConfig, keyboard_driver_cfg: ?MouseDriverConfig) BootRuntimeMode {
    if (!enable_framebuffer_server_step1) return .DiskUser;
    if (!enable_boot_log_console_process) return .FramebufferIpc;
    if (enable_virtio_input_mouse and mouse_driver_cfg != null) {
        _ = keyboard_driver_cfg;
        return .BootLogGateCompositor;
    }
    return .BootLogConsole;
}

fn activateThreadOrHalt(thread_index: usize) void {
    if (threadContextLooksCorrupted(thread_index)) {
        if (repairThreadContext(thread_index)) {
            serialOnlyWrite("activate Thread REPAIR idx=");
            serialOnlyWriteThreadIndexLabel(thread_index);
            serialOnlyWrite(" ok\n");
        }
    }
    if (activateThread(thread_index)) return;
    // Retry once after repair, even if fast corruption check did not trigger.
    if (repairThreadContext(thread_index) and activateThread(thread_index)) {
        serialOnlyWrite("activate Thread REPAIR idx=");
        serialOnlyWriteThreadIndexLabel(thread_index);
        serialOnlyWrite(" retry-ok\n");
        return;
    }

    // Prevent timer IRQ preemption while emitting failure diagnostics.
    asm volatile ("cli");
    serialOnlyWrite("activate Thread DIAG3 idx=");
    serialOnlyWriteThreadIndexLabel(thread_index);
    serialOnlyWrite(" failed");
    if (getThreadContextConst(thread_index)) |ctx| {
        serialOnlyWrite(" ready=");
        serialOnlyWriteBool01(ctx.ready);
        const owner_raw = rawOwnerTag(ctx);
        serialOnlyWrite(" owner_raw=0x");
        const hex = "0123456789abcdef";
        serial.writeByte(hex[(owner_raw >> 4) & 0xF]);
        serial.writeByte(hex[owner_raw & 0xF]);
        serialOnlyWrite(" owner=");
        if (principalFromRawTag(owner_raw)) |owner| {
            serialOnlyWrite(principalLabel(owner));
        } else {
            serialOnlyWrite("invalid");
        }
        serialOnlyWrite(" cr3=");
        serial.writeHexRaw(ctx.cr3);
    } else {
        serialOnlyWrite(" ctx=none");
    }
    if (expectedPrincipalForThread(thread_index)) |expected| {
        serialOnlyWrite(" expected_owner=");
        serialOnlyWrite(principalLabel(expected));
        if (getUserSpace(expected)) |space| {
            serialOnlyWrite(" space_cr3=");
            serial.writeHexRaw(space.cr3);
        } else {
            serialOnlyWrite(" space=none");
        }
    }
    serialOnlyWrite("\n");
    while (true) asm volatile ("hlt");
}

fn armBootLogGateDeferredInputStart(
    start_thread0: bool,
    start_thread3: bool,
    start_thread2: bool,
    start_thread4: bool,
    start_thread5: bool,
    start_thread7: bool,
    delay_ticks: u64,
) void {
    bootlog_gate_input_start_armed = false;
    bootlog_gate_start_thread0 = false;
    bootlog_gate_start_thread3 = false;
    bootlog_gate_start_thread2 = false;
    bootlog_gate_start_thread4 = false;
    bootlog_gate_start_thread5 = false;
    bootlog_gate_start_thread7 = false;
    startup_process0_shared_sent = false;
    startup_process2_window_sent = false;
    startup_process3_shared_sent = false;
    startup_process4_window_sent = false;
    startup_process5_window_sent = false;
    startup_process7_window_sent = false;
    startup_dynamic_window_sent_count = 0;
    startup_last_client_cap_tick = 0;

    if (start_thread0) {
        if (threadSlotForPrincipal(.Process0)) |slot| if (setThreadReady(slot, false)) {
            bootlog_gate_start_thread0 = true;
        };
    }
    if (start_thread3) {
        if (threadSlotForPrincipal(.Process3)) |slot| if (setThreadReady(slot, false)) {
            bootlog_gate_start_thread3 = true;
        };
    }
    if (start_thread2) {
        if (threadSlotForPrincipal(.Process2)) |slot| if (setThreadReady(slot, false)) {
            bootlog_gate_start_thread2 = true;
        };
    }
    if (start_thread4) {
        if (threadSlotForPrincipal(.Process4)) |slot| if (setThreadReady(slot, false)) {
            bootlog_gate_start_thread4 = true;
        };
    }
    if (start_thread5) {
        if (threadSlotForPrincipal(.Process5)) |slot| if (setThreadReady(slot, false)) {
            bootlog_gate_start_thread5 = true;
        };
    }
    if (start_thread7) {
        if (threadSlotForPrincipal(.Process7)) |slot| if (setThreadReady(slot, false)) {
            bootlog_gate_start_thread7 = true;
        };
    }
    if (!bootlog_gate_start_thread0 and !bootlog_gate_start_thread3 and !bootlog_gate_start_thread2 and !bootlog_gate_start_thread4 and !bootlog_gate_start_thread5 and !bootlog_gate_start_thread7) return;

    bootlog_gate_input_start_tick = scheduler.lapic_tick_count + delay_ticks;
    bootlog_gate_input_start_armed = true;
    refreshCompositorPriorityActive();
}

fn tryStartBootLogGateDeferredInput() void {
    if (!bootlog_gate_input_start_armed) return;
    const mouse_ready = (boot_log_status_flags & boot_log_status_mouse_queue_ready) != 0;
    const keyboard_ready = (boot_log_status_flags & boot_log_status_keyboard_queue_ready) != 0;

    if (scheduler.lapic_tick_count >= bootlog_gate_input_start_tick) {
        if (bootlog_gate_start_thread0) {
            if (threadSlotForPrincipal(.Process0)) |slot| _ = setThreadReady(slot, true);
            bootlog_gate_start_thread0 = false;
        }
        if (bootlog_gate_start_thread3) {
            if (threadSlotForPrincipal(.Process3)) |slot| _ = setThreadReady(slot, true);
            bootlog_gate_start_thread3 = false;
        }
    }

    if (bootlog_gate_start_thread2 and mouse_ready) {
        if (threadSlotForPrincipal(.Process2)) |slot| _ = setThreadReady(slot, true);
        bootlog_gate_start_thread2 = false;
    }
    if (bootlog_gate_start_thread4 and keyboard_ready) {
        if (threadSlotForPrincipal(.Process4)) |slot| _ = setThreadReady(slot, true);
        bootlog_gate_start_thread4 = false;
    }
    if (bootlog_gate_start_thread5 and keyboard_ready) {
        if (threadSlotForPrincipal(.Process5)) |slot| _ = setThreadReady(slot, true);
        bootlog_gate_start_thread5 = false;
    }
    if (bootlog_gate_start_thread7 and scheduler.lapic_tick_count >= bootlog_gate_input_start_tick) {
        if (threadSlotForPrincipal(.Process7)) |slot| _ = setThreadReady(slot, true);
        bootlog_gate_start_thread7 = false;
    }

    bootlog_gate_input_start_armed = bootlogGateHasPendingStartupThreads();
    refreshCompositorPriorityActive();
}

fn setupUserProcessesForMode(
    state: *kernel.KernelState,
    mode: BootRuntimeMode,
    mouse_driver_cfg: ?MouseDriverConfig,
    keyboard_driver_cfg: ?MouseDriverConfig,
    gpu_cfg: ?VirtioGpuDriverConfig,
    bootfs_image: ?[]const u8,
) UserBootProcessSetup {
    var result = UserBootProcessSetup{};
    if (enable_vfs_process) {
        const setup = setupVfsProcess(state, bootfs_image);
        result.setVfs(setup);
        activateThreadOrHalt(setup.process.thread_slot);
    }
    for (bootRoleDescriptorsForMode(mode)) |role| {
        _ = role.principal;
        switch (role.kind) {
            .disk_user => {
                const user_process = createUserProcess(state, .Process0, "user");
                result.setProcess(.Process0, user_process);
                if (role.activate_immediately) activateThreadOrHalt(user_process.thread_slot);
            },
            .draw_client => {
                const draw_client_process = createUserProcess(state, .Process0, "draw client");
                result.setProcess(.Process0, draw_client_process);
                if (role.activate_immediately) activateThreadOrHalt(draw_client_process.thread_slot);
            },
            .framebuffer_server => {
                const framebuffer_process = createUserProcess(state, .Process1, "framebuffer");
                result.setProcess(.Process1, framebuffer_process);
                if (role.activate_immediately) activateThreadOrHalt(framebuffer_process.thread_slot);
            },
            .boot_log_console => {
                const setup = setupBootLogConsoleProcess(state, gpu_cfg);
                result.setBootLogConsole(setup);
                if (role.activate_immediately) activateThreadOrHalt(setup.process.thread_slot);
            },
            .mouse_driver => {
                const setup = setupMouseDriverProcess(state, mouse_driver_cfg.?);
                result.setMouseDriver(setup);
                if (role.activate_immediately) activateThreadOrHalt(setup.process.thread_slot);
            },
            .keyboard_driver => {
                if (keyboard_driver_cfg) |cfg| {
                    const setup = setupKeyboardDriverProcess(state, cfg);
                    result.setKeyboardDriver(setup);
                    if (role.activate_immediately) activateThreadOrHalt(setup.process.thread_slot);
                }
            },
            .mouse_button_demo => {
                if (result.mouse_shared_page) |shared_page| {
                    const setup = setupMouseButtonDemoProcess(state, shared_page);
                    result.setProcess(.Process2, setup.process);
                    if (role.activate_immediately) activateThreadOrHalt(setup.process.thread_slot);
                }
            },
            .keyboard_ascii_demo => {
                if (result.keyboard_shared_page) |shared_page| {
                    const setup = setupKeyboardAsciiDemoProcess(state, shared_page);
                    result.setProcess(.Process4, setup.process);
                    if (role.activate_immediately) activateThreadOrHalt(setup.process.thread_slot);
                }
            },
            .taskbar => {
                if (result.mouse_shared_page) |shared_page| {
                    const setup = setupTaskbarProcess(state, shared_page);
                    result.setProcess(.Process7, setup.process);
                    if (role.activate_immediately) activateThreadOrHalt(setup.process.thread_slot);
                }
            },
            .init => {
                if (enable_init_process) {
                    const process = createUserProcess(state, init_principal, "init");
                    result.setProcess(init_principal, process);
                    if (result.keyboard_shared_page) |shared_page| {
                        grantAndMapPageForProcessOrHalt(state, .Process3, init_principal, shared_page, keyboard_shared_draw_va, false, "init", "keyboard shared page");
                    }
                    if (result.mouse_shared_page) |shared_page| {
                        setupTaskbarBootstrapForInit(state, init_principal, shared_page);
                    }
                    if (role.activate_immediately) activateThreadOrHalt(process.thread_slot);
                }
            },
        }
    }
    if (enable_vfs_process) {
        installVfsEndpointsOrHalt(state);
    }
    return result;
}

fn setupFramebufferServerAccess(state: *kernel.KernelState, info: FramebufferInfo) void {
    const framebuffer_cap = kernel.FramebufferCapability{
        .paddr = info.paddr,
        .size_bytes = info.size_bytes,
        .width = info.width,
        .height = info.height,
        .pixels_per_scan_line = info.pixels_per_scan_line,
        .pixel_format = info.pixel_format,
        .allow_draw = true,
    };
    state.grantFramebufferCap(.Process1, framebuffer_cap) catch {
        serialWrite("framebuffer capability grant failed\n");
        while (true) asm volatile ("hlt");
    };
    if (!state.canDrawToFramebuffer(.Process1, framebuffer_cap.paddr, framebuffer_cap.size_bytes, true)) {
        serialWrite("framebuffer capability check failed\n");
        while (true) asm volatile ("hlt");
    }
    mapUserLinearRegionOrHalt(.Process1, framebuffer_user_va, framebuffer_cap.paddr, framebuffer_cap.size_bytes, true, "framebuffer user mapping failed");

    // Always emit a compact framebuffer capability line before ring3 start.
    serialWriteFmt(
        "framebuffer cap paddr=0x{x} size={} bytes res={}x{} pitch={}\n",
        .{ framebuffer_cap.paddr, framebuffer_cap.size_bytes, framebuffer_cap.width, framebuffer_cap.height, framebuffer_cap.pixels_per_scan_line },
    );

    if (enable_title_only_ready_logs) {
        logReadyTitle("FRAMEBUFFER_SERVER_READY");
    } else {
        serialWrite("framebuffer server ready\n");
        serialWrite("  draw_cap=Process1\n");
        serialWrite("  mode=");
        printNumber(info.mode);
        serialWrite("\n");
        serialWrite("  fb_paddr=");
        printHex(framebuffer_cap.paddr);
        serialWrite("\n");
        serialWrite("  fb_size=");
        printNumber(framebuffer_cap.size_bytes);
        serialWrite(" bytes\n");
        serialWrite("  fb_va=");
        printHex(framebuffer_user_va);
        serialWrite("\n");
        serialWrite("  fb_resolution=");
        printNumber(framebuffer_cap.width);
        serialWrite("x");
        printNumber(framebuffer_cap.height);
        serialWrite("\n");
        serialWrite("  fb_pitch=");
        printNumber(framebuffer_cap.pixels_per_scan_line);
        serialWrite("\n");
    }
}

fn installUserMemoryWritePfTestCode(user_page_paddr: u64) void {
    user_programs.installMemoryWritePfTestCode(user_program_cfg, user_page_paddr);
}

fn installUserIdleTaskCode(user_page_paddr: u64) void {
    user_programs.installIdleTaskCode(user_page_paddr);
}

fn loadFileFromDisk(
    bs: *uefi.tables.BootServices,
    path: [*:0]const u16,
) ?[]const u8 {
    const loaded_image = (bs.handleProtocol(uefi.protocol.LoadedImage, uefi.handle) catch return null) orelse return null;
    const device_handle = loaded_image.device_handle orelse return null;
    const fs = (bs.handleProtocol(uefi.protocol.SimpleFileSystem, device_handle) catch return null) orelse return null;

    var root = fs.openVolume() catch return null;
    defer root.close() catch {};

    var user_file = root.open(path, .read, .{}) catch return null;
    defer user_file.close() catch {};

    var info_buffer: [512]u8 align(8) = undefined;
    const info = user_file.getInfo(.file, info_buffer[0..]) catch return null;
    const read_len: usize = @intCast(info.file_size);
    if (read_len == 0) return null;

    const staging = bs.allocatePool(.loader_data, read_len) catch return null;
    errdefer bs.freePool(staging.ptr) catch {};

    const read_bytes = user_file.read(staging[0..read_len]) catch return null;
    if (read_bytes != read_len) return null;
    return staging[0..read_len];
}

fn loadElfFromDisk(
    bs: *uefi.tables.BootServices,
    path: [*:0]const u16,
) ?[]const u8 {
    return loadFileFromDisk(bs, path);
}

fn loadUserElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, user_elf_disk_path);
}

fn loadFramebufferServerElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, framebuffer_server_elf_disk_path);
}

fn loadDrawClientElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, draw_client_elf_disk_path);
}

fn loadBootLogConsoleElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, boot_log_console_elf_disk_path);
}

fn loadMouseDriverElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, mouse_driver_elf_disk_path);
}

fn loadKeyboardDriverElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, keyboard_driver_elf_disk_path);
}

fn loadBootLogSenderElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, bootlog_sender_elf_disk_path);
}

fn loadCompositorElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, compositor_elf_disk_path);
}

fn loadGpuCompositorElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, gpu_compositor_elf_disk_path);
}

fn loadMouseButtonDemoElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, mouse_button_demo_elf_disk_path);
}

fn loadKeyboardAsciiDemoElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, keyboard_ascii_demo_elf_disk_path);
}

fn loadTaskbarElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, taskbar_elf_disk_path);
}

fn loadVfsElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, vfs_elf_disk_path);
}

fn loadInitElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, init_elf_disk_path);
}

fn loadBootFsImageFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadFileFromDisk(bs, bootfs_image_disk_path);
}

fn writeBootLogStatusToUserPage(user_page_paddr: u64) void {
    if (user_page_paddr < 0x1000) return;
    const page: [*]volatile u8 = @ptrFromInt(user_page_paddr);
    const status = boot_log_status_flags;
    page[boot_log_status_offset + 0] = @intCast(status & 0xFF);
    page[boot_log_status_offset + 1] = @intCast((status >> 8) & 0xFF);
    page[boot_log_status_offset + 2] = @intCast((status >> 16) & 0xFF);
    page[boot_log_status_offset + 3] = @intCast((status >> 24) & 0xFF);
}

fn updateBootLogQueueReadyStatusFromUserLog(proc: kernel.PrincipalId, message: []const u8) void {
    var new_status = boot_log_status_flags;
    if (proc == .Process0 and containsBytes(message, "MouseDriver: queue ready")) {
        new_status |= boot_log_status_mouse_queue_ready;
    }
    if (proc == .Process3 and containsBytes(message, "KeyboardDriver: queue ready")) {
        new_status |= boot_log_status_keyboard_queue_ready;
    }
    if (new_status == boot_log_status_flags) return;
    boot_log_status_flags = new_status;
    writeBootLogStatusToUserPage(boot_log_status_page_paddr);
}

fn noteStartupClientCapSent(from: kernel.PrincipalId, to: kernel.PrincipalId, endpoint_id: u64) void {
    if (to != .Process1 or endpoint_id != kernel.endpoint_to_process1) return;
    switch (from) {
        .Process0 => startup_process0_shared_sent = true,
        .Process2 => startup_process2_window_sent = true,
        .Process3 => startup_process3_shared_sent = true,
        .Process4 => startup_process4_window_sent = true,
        .Process5 => startup_process5_window_sent = true,
        .Process7 => startup_process7_window_sent = true,
        else => {
            if (!isDynamicSpawnPrincipal(from)) return;
            if (startup_dynamic_window_sent_count == 0) {
                startup_process5_window_sent = true;
            } else if (startup_dynamic_window_sent_count == 1) {
                startup_process7_window_sent = true;
            }
            if (startup_dynamic_window_sent_count < std.math.maxInt(u8)) {
                startup_dynamic_window_sent_count += 1;
            }
        },
    }
    startup_last_client_cap_tick = scheduler.lapic_tick_count;
}

fn publishBootLogToUserPage(user_page_paddr: u64) void {
    const page: [*]volatile u8 = @ptrFromInt(user_page_paddr);
    var zero_i: usize = 0;
    while (zero_i < 4096) : (zero_i += 1) {
        page[zero_i] = 0;
    }

    const copy_len: usize = if (kernel_log.boot_log_len > boot_log_page_payload_bytes) boot_log_page_payload_bytes else kernel_log.boot_log_len;
    const start_index: usize = if (kernel_log.boot_log_len > copy_len) kernel_log.boot_log_len - copy_len else 0;
    const len_u32: u32 = @intCast(copy_len);
    page[0] = @intCast(len_u32 & 0xFF);
    page[1] = @intCast((len_u32 >> 8) & 0xFF);
    page[2] = @intCast((len_u32 >> 16) & 0xFF);
    page[3] = @intCast((len_u32 >> 24) & 0xFF);
    writeBootLogStatusToUserPage(user_page_paddr);
    var i: usize = 0;
    while (i < copy_len) : (i += 1) {
        page[boot_log_page_header_bytes + i] = kernel_log.boot_log_buffer[start_index + i];
    }
}

fn mmioPageWithOffset(addr: u64) MmioPageWithOffset {
    if (addr == 0) {
        return .{ .page_paddr = 0, .page_offset = 0 };
    }
    return .{
        .page_paddr = kernel_vm.pageAlignDown(addr),
        .page_offset = addr & 0xFFF,
    };
}

fn resetKernelBootState() void {
    resetKernelBootState();
}

fn acquireBootServicesOrHalt() *uefi.tables.BootServices {
    const bs = uefi.system_table.boot_services orelse {
        serialWrite("boot services missing\n");
        while (true) asm volatile ("hlt");
    };
    boot_services_cache = bs;
    return bs;
}

fn logKernelImageRangeIfKnown() void {
    if (kernel_image_base_paddr != 0 and kernel_image_size_bytes != 0) {
        serialWriteFmt(
            "kernel image range base=0x{x} size={} bytes\n",
            .{ kernel_image_base_paddr, kernel_image_size_bytes },
        );
    }
}

fn initMemoryModules() void {
    if (!allocateUserSpaces()) {
        serialWrite("user address space backing alloc failed\n");
        while (true) asm volatile ("hlt");
    }
    user_vm.init(.{
        .user_spaces = user_spaces,
        .current_user_principal = &scheduler.current_user_principal,
        .four_gib = four_gib,
        .user_va = user_va,
        .user_stack_page_va = user_stack_page_va,
        .page_entries = page_entries,
        .page_present = page_present,
        .page_rw = page_rw,
        .page_user = page_user,
        .seed_user_pd_with_kernel_identity = x86_platform.seedUserPdWithKernelIdentity,
    });
    pmm.init(.{
        .write = serialWrite,
        .main_addr = @intFromPtr(&main),
        .kernel_cr3_addr = @intFromPtr(&x86_platform.kernel_cr3_value),
        .kernel_image_base_paddr = &kernel_image_base_paddr,
        .kernel_image_size_bytes = &kernel_image_size_bytes,
        .post_exit_load_scratch_addr = @intFromPtr(&post_exit_load_scratch) + @sizeOf(@TypeOf(post_exit_load_scratch)),
        .reserved_low_mem_end = reserved_low_mem_end,
    });
}

fn collectBootMemoryStatsOrHalt(bs: *uefi.tables.BootServices) MemoryStats {
    serialWrite("[stage] before memory stats collect\n");
    const user_spaces_start = if (user_spaces.len == 0) 0 else kernel_vm.pageAlignDown(@intFromPtr(user_spaces.ptr));
    const user_spaces_end = if (user_spaces.len == 0) 0 else kernel_vm.pageAlignUp(@intFromPtr(user_spaces.ptr) + (user_spaces.len * @sizeOf(UserAddressSpace)));
    const memory_stats = pmm.collectMemoryStatsAndFreePages(bs, &global_free_list, uefi_mmap_buffer[0..], user_spaces_start, user_spaces_end) orelse {
        serialWrite("memory map parse failed\n");
        while (true) asm volatile ("hlt");
    };
    serialWrite("[stage] memory stats collected\n");
    serialWrite("Detected ");
    printNumber(memory_stats.detected_regions);
    serialWrite(" regions\n");

    const total_usable_mb = memory_stats.total_usable_bytes / (1024 * 1024);
    serialWrite("Total usable memory: ");
    printNumber(total_usable_mb);
    serialWrite("MB\n");
    serialWrite("free pages: ");
    printNumber(global_free_list.len);
    serialWrite("\n");
    return memory_stats;
}

fn exitBootServicesOrHalt() void {
    if (debug_skip_exit_boot_services) {
        serialWrite("[debug] skip ExitBootServices\n");
    } else {
        serialWrite("try ExitBootServices...\n");
        switch (exitBootServicesWithRetry()) {
            .success => serialWrite("ExitBootServices success\n"),
            .failed => {
                serialWrite("ExitBootServices failed\n");
                while (true) asm volatile ("hlt");
            },
        }
        serialWrite("UEFI services terminated\n");
    }
    boot_services_cache = null;
}

fn initKernelRuntimeOrHalt() void {
    loadGdtAndReloadSegments();
    initFxStateSupport();
    serialWrite("GDT loaded (kernel/user segments)\n");

    if (debug_skip_cr3_switch) {
        serialWrite("[debug] skip CR3 switch\n");
    } else {
        serialWrite("build page tables (identity 0..");
        printNumber(x86_platform.pd_table_count);
        serialWrite("GiB, plus 512..1024GiB mmio)\n");
        if (!installIdentityPageTables0To1GiB()) {
            serialWrite("page table install failed\n");
            while (true) asm volatile ("hlt");
        }
        hardenKernelMappingsSupervisorOnly();
        installInterruptTrampolines();
        serialWrite("CR3 switched to custom PML4\n");
    }
    initIdtPageFaultOnly();
    serialWrite("IDT loaded (#PF/#GP/#DF/#INT80/#LAPIC-TIMER)\n");
    asm volatile ("cli");
    if (!lapic.initTimer(lapic_timer_vector, lapic_timer_initial_count)) {
        serialWrite("LAPIC timer init failed\n");
        while (true) asm volatile ("hlt");
    }
    serialWrite("LAPIC timer enabled\n");
    if (!elf_loader.probe()) {
        serialWrite("ELF loader probe failed\n");
        while (true) asm volatile ("hlt");
    }
    serialWrite("ELF loader PIE+RELATIVE ready\n");
    if (debug_trigger_page_fault_test) {
        triggerPageFaultTest();
    }
}

fn printKernelBootBanner() void {
    serialWrite("\x1b[96m========================================\n");
    serialWrite("  MicroKernel\n");
    serialWrite("\x1b[95m  enter bare-metal capability kernel\n");
    serialWrite("\x1b[96m========================================\x1b[0m\n");
}

fn publishMouseDriverConfigPage(user_page_paddr: u64, cfg: MouseDriverConfig, shared_page_paddr: u64, queue_submit_token: u64, queue_notify_token: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(user_page_paddr);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        words[i] = 0;
    }

    words[0] = mouse_driver_config_magic;
    words[1] = cfg.common.page_paddr;
    words[2] = cfg.notify.page_paddr;
    words[3] = cfg.isr.page_paddr;
    words[4] = cfg.device.page_paddr;
    words[5] = cfg.common.page_offset;
    words[6] = cfg.notify.page_offset;
    words[7] = cfg.isr.page_offset;
    words[8] = cfg.device.page_offset;
    words[9] = cfg.notify_off_multiplier;
    words[10] = shared_page_paddr;
    words[11] = cfg.queue_paddr0;
    words[12] = cfg.queue_paddr1;
    words[13] = queue_submit_token;
    words[14] = queue_notify_token;
}

fn publishKeyboardDriverConfigPage(user_page_paddr: u64, cfg: MouseDriverConfig, shared_page_paddr: u64, queue_submit_token: u64, queue_notify_token: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(user_page_paddr);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        words[i] = 0;
    }

    words[0] = keyboard_driver_config_magic;
    words[1] = cfg.common.page_paddr;
    words[2] = cfg.notify.page_paddr;
    words[3] = cfg.isr.page_paddr;
    words[4] = cfg.device.page_paddr;
    words[5] = cfg.common.page_offset;
    words[6] = cfg.notify.page_offset;
    words[7] = cfg.isr.page_offset;
    words[8] = cfg.device.page_offset;
    words[9] = cfg.notify_off_multiplier;
    words[10] = shared_page_paddr;
    words[11] = cfg.queue_paddr0;
    words[12] = cfg.queue_paddr1;
    words[13] = queue_submit_token;
    words[14] = queue_notify_token;
}

fn publishVirtioGpuConfigPage(user_page_paddr: u64, cfg: ?VirtioGpuDriverConfig, control_submit_token: u64, control_notify_token: u64, cursor_submit_token: u64, cursor_notify_token: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(user_page_paddr);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        words[i] = 0;
    }

    if (cfg) |gpu_cfg| {
        words[0] = virtio_gpu_config_magic;
        words[1] = gpu_cfg.common.page_paddr;
        words[2] = gpu_cfg.notify.page_paddr;
        words[3] = gpu_cfg.isr.page_paddr;
        words[4] = gpu_cfg.device.page_paddr;
        words[5] = gpu_cfg.common.page_offset;
        words[6] = gpu_cfg.notify.page_offset;
        words[7] = gpu_cfg.isr.page_offset;
        words[8] = gpu_cfg.device.page_offset;
        words[9] = gpu_cfg.notify_off_multiplier;
        words[10] = 0; // default scanout id
        words[21] = control_submit_token;
        words[22] = control_notify_token;
        words[23] = cursor_submit_token;
        words[24] = cursor_notify_token;
    }
}

fn publishTaskbarConfigPage(user_page_paddr: u64, info: FramebufferInfo) void {
    const words: [*]volatile u64 = @ptrFromInt(user_page_paddr);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        words[i] = 0;
    }
    words[0] = taskbar_config_magic;
    words[1] = info.width;
    words[2] = info.height;
    words[3] = 0; // optional self process slot, unset for Init-spawned taskbar
}

fn publishTaskbarStatePage(user_page_paddr: u64) void {
    const page: *volatile TaskbarStatePage = @ptrFromInt(user_page_paddr);
    page.* = .{
        .magic = taskbar_state_magic,
        .version = taskbar_protocol_version,
        .entry_count = 0,
        .seq = 1,
        .entries = [_]TaskbarEntryPage{.{
            .window_id = 0,
            .flags = 0,
            .title_len = 0,
            .title = [_]u8{0} ** taskbar_window_title_max_bytes,
        }} ** taskbar_entry_max,
    };
}

fn publishTaskbarCommandPage(user_page_paddr: u64) void {
    const page: *volatile TaskbarCommandPage = @ptrFromInt(user_page_paddr);
    page.* = .{
        .magic = taskbar_command_magic,
        .version = taskbar_protocol_version,
        .command = 0,
        .seq = 1,
        .window_id = 0,
    };
}

fn publishKeyboardSharedPage(user_page_paddr: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(user_page_paddr);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        words[i] = 0;
    }
    words[0] = keyboard_shared_magic;
    words[1] = 1; // seq
    words[2] = '?'; // ascii
    words[3] = 0; // key code
    words[4] = 0; // key value
}

fn publishMouseSharedPage(user_page_paddr: u64, fb: FramebufferInfo) void {
    const bytes: [*]volatile u8 = @ptrFromInt(user_page_paddr);
    const words: [*]volatile u64 = @ptrFromInt(user_page_paddr);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        words[i] = 0;
    }

    words[0] = mouse_shared_magic;
    words[1] = fb.width;
    words[2] = fb.height;
    words[3] = fb.pixels_per_scan_line;
    words[4] = fb.width / 2;
    words[5] = fb.height / 2;
    words[6] = 0;
    words[7] = 1; // seq
    words[8] = 0; // wheel
    const text_len: usize = if (kernel_log.boot_log_len > mouse_shared_log_max_bytes) mouse_shared_log_max_bytes else kernel_log.boot_log_len;
    words[9] = @intCast(text_len);
    var j: usize = 0;
    while (j < text_len) : (j += 1) {
        bytes[mouse_shared_header_bytes + j] = kernel_log.boot_log_buffer[j];
    }
}

fn loadUserElfIntoUserPage(user_page_paddr: u64, image_bytes: []const u8) ?elf_loader.Image {
    if ((user_page_paddr & 0xFFF) != 0) return null;
    const page: [*]u8 = @ptrFromInt(user_page_paddr);
    return elf_loader.loadToSinglePage(image_bytes, user_elf_base_va, page[0..4096]) catch null;
}

fn computeUserElfRequiredBytes(image_bytes: []const u8) ?usize {
    const parsed = elf_loader.parse(image_bytes) catch return null;
    var max_end: u64 = 0;
    var i: usize = 0;
    while (i < parsed.load_segment_len) : (i += 1) {
        const seg = parsed.load_segments[i];
        const seg_end, const overflow = @addWithOverflow(seg.vaddr, seg.mem_size);
        if (overflow != 0) return null;
        if (seg_end > max_end) max_end = seg_end;
    }
    if (max_end == 0) max_end = 4096;
    const aligned_end = kernel_vm.pageAlignUp(max_end);
    if (aligned_end > std.math.maxInt(usize)) return null;
    return @intCast(aligned_end);
}

fn allocateBootScratch(bytes: usize) ?[]align(8) u8 {
    if (boot_services_cache) |bs| {
        return bs.allocatePool(.loader_data, bytes) catch null;
    }
    const aligned_bytes = std.mem.alignForward(usize, bytes, 8);
    const start = std.mem.alignForward(usize, post_exit_load_scratch_used, 8);
    if (aligned_bytes > post_exit_load_scratch.len - start) return null;
    post_exit_load_scratch_used = start + aligned_bytes;
    const ptr: [*]align(8) u8 = @alignCast(@ptrCast(&post_exit_load_scratch[start]));
    return ptr[0..bytes];
}

fn logDeferredCompositorLoadFailure(image_bytes: []const u8) void {
    serialWrite("deferred compositor load diag\n");
    const required_bytes = computeUserElfRequiredBytes(image_bytes) orelse {
        serialWrite("  required_bytes=parse_failed\n");
        return;
    };
    serialWrite("  required_bytes=");
    printNumber(required_bytes);
    serialWrite("\n");
    serialWrite("  scratch_bytes=");
    printNumber(post_exit_load_scratch.len);
    serialWrite("\n");
    serialWrite("  max_load_bytes=");
    printNumber(user_program_max_load_bytes);
    serialWrite("\n");
}

fn freeBootScratch(buf: []align(8) u8) void {
    if (boot_services_cache) |bs| {
        bs.freePool(buf.ptr) catch {};
        return;
    }
    const base = @intFromPtr(&post_exit_load_scratch[0]);
    const limit = base + post_exit_load_scratch.len;
    const ptr = @intFromPtr(buf.ptr);
    if (ptr < base or ptr > limit) return;
    const offset = ptr - base;
    const aligned_bytes = std.mem.alignForward(usize, buf.len, 8);
    if (offset + aligned_bytes == post_exit_load_scratch_used) {
        post_exit_load_scratch_used = offset;
    }
}

fn allocateUserSpaces() bool {
    user_spaces = user_spaces_storage[0..];
    for (user_spaces) |*space| {
        space.* = .{};
    }
    return true;
}

fn captureKernelImageRange(bs: *uefi.tables.BootServices) void {
    const loaded_image = (bs.handleProtocol(uefi.protocol.LoadedImage, uefi.handle) catch return) orelse return;
    kernel_image_base_paddr = @intFromPtr(loaded_image.image_base);
    kernel_image_size_bytes = @intCast(loaded_image.image_size);
}

fn loadUserElfIntoProcessPages(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    page0_paddr: u64,
    page1_paddr: u64,
    image_bytes: []const u8,
) ?elf_loader.Image {
    if ((page0_paddr & 0xFFF) != 0 or (page1_paddr & 0xFFF) != 0) {
        serialWrite("loadUserElfIntoProcessPages: unaligned base pages\n");
        return null;
    }
    const required_bytes = computeUserElfRequiredBytes(image_bytes) orelse {
        serialWrite("loadUserElfIntoProcessPages: required_bytes failed\n");
        return null;
    };
    if (required_bytes > user_program_max_load_bytes) {
        serialWrite("loadUserElfIntoProcessPages: image too large required=");
        printNumber(required_bytes);
        serialWrite(" max=");
        printNumber(user_program_max_load_bytes);
        serialWrite("\n");
        return null;
    }

    const load_window = allocateBootScratch(required_bytes) orelse {
        serialWrite("loadUserElfIntoProcessPages: scratch alloc failed bytes=");
        printNumber(required_bytes);
        serialWrite("\n");
        return null;
    };
    defer freeBootScratch(load_window);

    @memset(load_window[0..required_bytes], 0);
    const loaded = elf_loader.loadToSinglePage(image_bytes, user_elf_base_va, load_window[0..required_bytes]) catch |err| {
        serialWrite("loadUserElfIntoProcessPages: loadToSinglePage failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        return null;
    };

    const required_pages = required_bytes / 4096;
    const page0: [*]u8 = @ptrFromInt(page0_paddr);
    @memcpy(page0[0..4096], load_window[0..4096]);

    var page_index: usize = 1;
    while (page_index < required_pages) : (page_index += 1) {
        const extra_page = state.allocPageTo(principal, &global_free_list) catch |err| {
            serialWrite("loadUserElfIntoProcessPages: allocPageTo failed idx=");
            printNumber(page_index);
            serialWrite(" err=");
            serialWrite(@errorName(err));
            serialWrite("\n");
            return null;
        };
        const map_va = user_va + (@as(u64, @intCast(page_index)) * 4096);
        if (!user_vm.mapUserLinearRegion(principal, map_va, extra_page.paddr, 4096, true)) {
            serialWrite("loadUserElfIntoProcessPages: map failed idx=");
            printNumber(page_index);
            serialWrite(" va=");
            printHex(map_va);
            serialWrite(" paddr=");
            printHex(extra_page.paddr);
            serialWrite("\n");
            return null;
        }
        const page_bytes: [*]u8 = @ptrFromInt(extra_page.paddr);
        const off = page_index * 4096;
        @memcpy(page_bytes[0..4096], load_window[off .. off + 4096]);
    }

    return loaded;
}

fn loadUserElfIntoUserPageOrHalt(user_page_paddr: u64, image_bytes: []const u8, fail_message: []const u8) elf_loader.Image {
    return loadUserElfIntoUserPage(user_page_paddr, image_bytes) orelse {
        serialWrite(fail_message);
        while (true) asm volatile ("hlt");
    };
}

fn loadUserElfIntoProcessPagesOrHalt(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    page0_paddr: u64,
    page1_paddr: u64,
    image_bytes: []const u8,
    fail_message: []const u8,
) elf_loader.Image {
    return loadUserElfIntoProcessPages(state, principal, page0_paddr, page1_paddr, image_bytes) orelse {
        serialWrite(fail_message);
        while (true) asm volatile ("hlt");
    };
}

fn copyImageBackingBytes(backing: kernel.ImageBacking, dest: []u8) bool {
    if (dest.len < backing.size_bytes) return false;
    if (backing.page_count == 0 or backing.page_count > kernel.max_image_backing_pages) return false;

    var copied: usize = 0;
    var page_index: usize = 0;
    var page_offset: usize = backing.page_offset_bytes;
    while (copied < backing.size_bytes and page_index < backing.page_count) : (page_index += 1) {
        const page_paddr = backing.page_paddrs[page_index];
        if ((page_paddr & 0xFFF) != 0) return false;
        const page: [*]const u8 = @ptrFromInt(page_paddr);
        const page_available = 4096 - page_offset;
        const remaining = @as(usize, @intCast(backing.size_bytes)) - copied;
        const chunk_len: usize = if (remaining < page_available) remaining else page_available;
        @memcpy(dest[copied .. copied + chunk_len], page[page_offset .. page_offset + chunk_len]);
        copied += chunk_len;
        page_offset = 0;
    }
    return copied == backing.size_bytes;
}

fn spawnExecFromSyscall(frame: *TrapFrame) u64 {
    const caller = scheduler.current_user_principal;
    const cap_id = image_abi.decodeExecImageToken(frame.rdi) orelse return syscall_err_invalid;
    const bootstrap_source_va = frame.rsi;
    const bootstrap_target_va = frame.rdx;
    const bootstrap_flags = frame.rcx;
    const exec_cap = kernel_state_global.getExecImageTableConst(caller).findByCapId(cap_id) orelse return syscall_err_invalid;
    if (!exec_cap.rights.exec) return syscall_err_invalid;
    if (exec_cap.backing.size_bytes > user_program_max_load_bytes) return syscall_err_invalid;

    const descriptor_mode = (bootstrap_flags & process_abi.spawn_flag_bootstrap_descriptor_table) != 0;
    const bootstrap_requested = bootstrap_source_va != 0 or bootstrap_target_va != 0 or bootstrap_flags != 0;
    var bootstrap_descriptors: [process_abi.max_bootstrap_page_descriptors]process_abi.BootstrapPageDescriptor = undefined;
    var bootstrap_descriptor_count: usize = 0;
    if (bootstrap_requested) {
        if (descriptor_mode) {
            if ((bootstrap_flags & ~process_abi.spawn_flag_bootstrap_descriptor_table) != 0) {
                serialWrite("spawn_exec bootstrap bad flags\n");
                return syscall_err_invalid;
            }
            if (bootstrap_source_va == 0 or bootstrap_target_va == 0) {
                serialWrite("spawn_exec bootstrap descriptor table missing\n");
                return syscall_err_invalid;
            }
            if (bootstrap_target_va > process_abi.max_bootstrap_page_descriptors) {
                serialWrite("spawn_exec bootstrap descriptor count invalid\n");
                return syscall_err_invalid;
            }
            bootstrap_descriptor_count = @intCast(bootstrap_target_va);
            if (bootstrap_descriptor_count == 0) {
                serialWrite("spawn_exec bootstrap descriptor count invalid\n");
                return syscall_err_invalid;
            }
            const desc_bytes = std.mem.sliceAsBytes(bootstrap_descriptors[0..bootstrap_descriptor_count]);
            if (!user_copy.copyUserBytesFromVa(caller, bootstrap_source_va, desc_bytes)) {
                serialWrite("spawn_exec bootstrap descriptor copy fail\n");
                return syscall_err_invalid;
            }
        }
    }

    const image_bytes = allocateBootScratch(@intCast(exec_cap.backing.size_bytes)) orelse return syscall_err_alloc;
    defer freeBootScratch(image_bytes);
    if (!copyImageBackingBytes(exec_cap.backing, image_bytes)) return syscall_err_invalid;
    if (image_bytes.len < 4 or image_bytes[0] != 0x7F or image_bytes[1] != 'E' or image_bytes[2] != 'L' or image_bytes[3] != 'F') {
        return syscall_err_invalid;
    }

    const created = tryCreateDynamicUserProcess(&kernel_state_global, dynamic_spawn_first_process_index, "spawned exec") orelse return syscall_err_alloc;
    _ = setThreadReady(created.process.thread_slot, false);

    installSpawnCompatibilityEndpoints(&kernel_state_global, created.principal) catch return syscall_err_endpoint;

    const loaded = loadUserElfIntoProcessPages(
        &kernel_state_global,
        created.principal,
        created.process.user_page.paddr,
        created.process.user_stack_page.paddr,
        image_bytes,
    ) orelse {
        serialWrite("spawn_exec ELF load failed\n");
        return syscall_err_invalid;
    };
    if (bootstrap_requested) {
        if (descriptor_mode) {
            var i: usize = 0;
            while (i < bootstrap_descriptor_count) : (i += 1) {
                const desc = bootstrap_descriptors[i];
                const rc = installSpawnBootstrapPage(caller, created.principal, desc.source_va, desc.target_va, desc.flags);
                if (rc != syscall_ok) return rc;
            }
        } else {
            const rc = installSpawnBootstrapPage(caller, created.principal, bootstrap_source_va, bootstrap_target_va, bootstrap_flags);
            if (rc != syscall_ok) return rc;
        }
    }
    setThreadEntry(created.process.thread_slot, loaded.entry, user_entry_rsp);
    if (!setThreadReady(created.process.thread_slot, true)) return syscall_err_not_ready;

    return process_abi.encodeSpawnedProcessSlot(@intCast(kernel.processIndexFromPrincipal(created.principal).?));
}

fn preparePieUserLaunchThread() bool {
    if (pie_user_launch_thread_available) return true;
    if (pie_user_process6_initialized) return false;
    const image = pie_user_disk_image orelse return false;

    pie_user_process6_initialized = true;
    const process = tryCreateUserProcess(&kernel_state_global, .Process6, "pie user") orelse return false;
    _ = setThreadReady(process.thread_slot, false);

    const loaded = loadUserElfIntoProcessPages(
        &kernel_state_global,
        .Process6,
        process.user_page.paddr,
        process.user_stack_page.paddr,
        image,
    ) orelse return false;
    setThreadEntry(process.thread_slot, loaded.entry, user_entry_rsp);
    _ = setThreadReady(process.thread_slot, false);
    pie_user_launch_thread_available = true;
    logElfLoadSummary("PieUser ELF mapped", loaded);
    return true;
}

fn prepareMouseButtonDemoLaunchThread() bool {
    if (mouse_button_demo_launch_thread_available) return true;
    const image = mouse_button_demo_disk_image orelse return false;
    if (process2_launch_page_paddr == 0 or process2_launch_stack_paddr == 0) return false;

    const loaded = loadUserElfIntoProcessPages(
        &kernel_state_global,
        .Process2,
        process2_launch_page_paddr,
        process2_launch_stack_paddr,
        image,
    ) orelse return false;
    const thread_slot = threadSlotForPrincipalOrHalt(.Process2, "mouse button demo");
    setThreadEntry(thread_slot, loaded.entry, user_entry_rsp);
    _ = setThreadReady(thread_slot, false);
    mouse_button_demo_launch_thread_available = true;
    logElfLoadSummary("MouseButtonDemo ELF mapped", loaded);
    return true;
}

fn prepareKeyboardAsciiDemoLaunchThread() bool {
    if (keyboard_ascii_demo_launch_thread_available) return true;
    const image = keyboard_ascii_demo_disk_image orelse return false;
    if (process4_launch_page_paddr == 0 or process4_launch_stack_paddr == 0) return false;

    const loaded = loadUserElfIntoProcessPages(
        &kernel_state_global,
        .Process4,
        process4_launch_page_paddr,
        process4_launch_stack_paddr,
        image,
    ) orelse return false;
    const thread_slot = threadSlotForPrincipalOrHalt(.Process4, "keyboard ascii demo");
    setThreadEntry(thread_slot, loaded.entry, user_entry_rsp);
    _ = setThreadReady(thread_slot, false);
    keyboard_ascii_demo_launch_thread_available = true;
    logElfLoadSummary("KeyboardAsciiDemo ELF mapped", loaded);
    return true;
}

fn setThreadEntry(thread_index: usize, entry: u64, rsp: ?u64) void {
    const ctx = getThreadContext(thread_index).?;
    ctx.frame.rip = entry;
    if (rsp) |stack_top| {
        ctx.frame.rsp = stack_top;
    }
}

fn setThreadEntryIfReady(thread_index: usize, entry: u64, rsp: u64) void {
    const ctx = getThreadContext(thread_index).?;
    if (ctx.ready) {
        ctx.frame.rip = entry;
        ctx.frame.rsp = rsp;
    }
}

fn resetThreadForEntry(thread_index: usize, role_label: []const u8, entry: u64, rsp: u64) void {
    if (!repairThreadContext(thread_index)) {
        serialWrite(role_label);
        serialWrite(" thread context reset failed\n");
        haltLoop();
    }
    setThreadEntry(thread_index, entry, rsp);
}

fn resetThreadForPrincipal(principal: kernel.PrincipalId, role_label: []const u8, entry: u64, rsp: u64) void {
    resetThreadForEntry(threadSlotForPrincipalOrHalt(principal, role_label), role_label, entry, rsp);
}

fn logElfLoadSummary(header: []const u8, loaded: elf_loader.Image) void {
    serialWrite(header);
    serialWrite("\n");
    if (enable_title_only_ready_logs) return;
    serialWrite("  base=");
    printHex(user_elf_base_va);
    serialWrite("\n");
    serialWrite("  entry=");
    printHex(loaded.entry);
    serialWrite("\n");
    serialWrite("  load_segments=");
    printNumber(loaded.load_segment_len);
    serialWrite("\n");
}

fn armDeferredCompositorLaunch(
    classic_image: ?[]const u8,
    gpu_image: ?[]const u8,
    process1_user_page_paddr: u64,
    process1_user_stack_paddr: u64,
    wait_mouse_queue_ready: bool,
    wait_keyboard_queue_ready: bool,
) void {
    deferred_compositor.arm(
        classic_image,
        gpu_image,
        process1_user_page_paddr,
        process1_user_stack_paddr,
        !enable_bootlog_wait_for_enter,
        scheduler.lapic_tick_count + deferred_compositor_auto_launch_delay_ticks,
        wait_mouse_queue_ready,
        wait_keyboard_queue_ready,
    );
}

fn containsBytes(haystack: []const u8, needle: []const u8) bool {
    return std.mem.indexOf(u8, haystack, needle) != null;
}

fn shouldSuppressSerialUserLog(message: []const u8) bool {
    return std.mem.eql(u8, message, "Compositor: drag boost on\n") or
        std.mem.eql(u8, message, "Compositor: drag boost off\n") or
        (suppress_compositor_perf_user_logs and
            std.mem.startsWith(u8, message, "Compositor: perf frames="));
}

fn launchDeferredCompositor(frame: *TrapFrame, reason: []const u8) void {
    if (!deferred_compositor.launch_armed or deferred_compositor.launched) return;

    const image = deferred_compositor.currentImage() orelse return;
    const compositor_thread = threadSlotForPrincipalOrHalt(.Process1, "deferred compositor");

    const loaded = loadUserElfIntoProcessPages(
        &kernel_state_global,
        .Process1,
        deferred_compositor.user_page_paddr,
        deferred_compositor.user_stack_page_paddr,
        image,
    ) orelse {
        deferred_compositor.markLoadFailed();
        logDeferredCompositorLoadFailure(image);
        serialWrite("deferred compositor launch failed: ELF load\n");
        return;
    };
    resetThreadForEntry(compositor_thread, "deferred compositor", loaded.entry, user_entry_rsp);
    _ = setThreadReady(compositor_thread, true);
    if (!boot_timing.state.compositor_launch) {
        boot_timing.state.compositor_launch = true;
        boot_timing.state.compositor_launch_tick = scheduler.lapic_tick_count;
        boot_timing.recordEvent("compositor_launch", boot_timing.state.compositor_launch_tick, null);
    }
    deferred_compositor.markLaunched();
    compositor_runtime_priority_requested = deferred_compositor.selected_kind == .gpu;
    refreshCompositorPriorityActive();
    serialWrite("deferred compositor launch: ");
    serialWrite(reason);
    serialWrite("\n");
    serialWrite("  selected_mode=");
    serialWrite(deferred_compositor.label(deferred_compositor.selected_kind));
    serialWrite("\n");
    logElfLoadSummary("Compositor ELF remapped", loaded);

    if (scheduler.current_thread_index == compositor_thread) {
        frame.* = buildInitialUserTrapFrame();
        frame.rip = loaded.entry;
        frame.rsp = user_entry_rsp;
        return;
    }
    if (bootlogGateHasPendingStartupThreads()) {
        serialWrite("deferred compositor launch: startup threads pending, no immediate switch\n");
        return;
    }
    if (std.mem.eql(u8, reason, "auto")) {
        serialWrite("deferred compositor launch: auto, no immediate switch\n");
        return;
    }
    if (switchToThread(compositor_thread, frame, syscall_ok)) {
        serialWrite("deferred compositor switch to compositor thread\n");
    }
}

fn tryLaunchDeferredCompositorFromLog(frame: *TrapFrame, proc: kernel.PrincipalId, message: []const u8) void {
    if (!deferred_compositor.handleKeyboardLog(proc, message, serialWrite)) return;
    launchDeferredCompositor(frame, "keyboard enter");
}

fn tryAutoLaunchDeferredCompositor(frame: *TrapFrame) void {
    if (!startup_process5_window_sent or !startup_process7_window_sent) return;
    if (scheduler.lapic_tick_count < startup_last_client_cap_tick + startup_client_settle_ticks) return;
    if (!deferred_compositor.shouldAutoLaunch(
        scheduler.lapic_tick_count,
        bootlog_entered_user_tick,
        bootlog_auto_min_visible_ticks,
        boot_log_status_flags,
        boot_log_status_mouse_queue_ready,
        boot_log_status_keyboard_queue_ready,
    )) return;
    launchDeferredCompositor(frame, "auto");
}

fn installUserFramebufferFillCode(
    user_page_paddr: u64,
    framebuffer_va: u64,
    pixel_count: u64,
    color: u32,
) void {
    user_programs.installFramebufferFillCode(user_page_paddr, framebuffer_va, pixel_count, color);
}

fn installUserGeneralProtectionTestCode(user_page_paddr: u64) void {
    user_programs.installGeneralProtectionTestCode(user_page_paddr);
}

fn installUserPfRecoveryDemoCode(user_page_paddr: u64) void {
    user_programs.installPfRecoveryDemoCode(user_program_cfg, user_page_paddr);
}

fn installUserPfRecoveryThenSwitchCode(user_page_paddr: u64, target_thread: u64) void {
    user_programs.installPfRecoveryThenSwitchCode(user_program_cfg, user_page_paddr, target_thread);
}

fn installUserDmaUnmapVerifyCode(user_page_paddr: u64) void {
    user_programs.installDmaUnmapVerifyCode(user_program_cfg, user_page_paddr);
}

fn installUserSchedulerProbeCode(user_page_paddr: u64, syscall_no: u64) void {
    user_programs.installSchedulerProbeCode(user_page_paddr, syscall_no);
}

fn installUserSchedulerProbeWithSendCapCode(user_page_paddr: u64, probe_syscall_no: u64, endpoint_id: u64) void {
    user_programs.installSchedulerProbeWithSendCapCode(user_program_cfg, user_page_paddr, probe_syscall_no, endpoint_id);
}

fn installUserCapSendTransferDemoCode(
    user_page_paddr: u64,
    paddr_to_send: u64,
    send_endpoint_id: u64,
    switch_to_thread: u64,
) void {
    user_programs.installCapSendTransferDemoCode(
        user_program_cfg,
        user_page_paddr,
        paddr_to_send,
        send_endpoint_id,
        switch_to_thread,
    );
}

fn enterUserModeIretq(user_entry_va: u64, user_rsp: u64) noreturn {
    const user_cs: u64 = gdt_user_code_selector | 0x3;
    const user_ss: u64 = gdt_user_data_selector | 0x3;
    const user_rflags: u64 = user_entry_rflags; // IF=1 で ring3 中の LAPIC timer 割り込みを許可
    const kernel_transition_rsp = x86_platform.ring0StackTop();

    serialWrite("IRET precheck");
    serialWrite(" gdt_base=");
    printHex(x86_platform.gdtBase());
    serialWrite(" gdt_user_cs_desc=");
    printHex(x86_platform.userCodeDescriptor());
    serialWrite(" gdt_user_ss_desc=");
    printHex(x86_platform.userDataDescriptor());
    serialWrite(" user_cr3=");
    printHex(scheduler.user_cr3_value);
    serialWrite(" rip=");
    printHex(user_entry_va);
    serialWrite(" rsp=");
    printHex(user_rsp);
    serialWrite("\n");

    restoreCurrentThreadFxState();

    user_return_iret_frame[0] = user_entry_va;
    user_return_iret_frame[1] = user_cs;
    user_return_iret_frame[2] = user_rflags;
    user_return_iret_frame[3] = user_rsp;
    user_return_iret_frame[4] = user_ss;

    asm volatile (
        \\mov %[k_rsp], %%rsp
        \\lea user_return_iret_frame(%rip), %%rsp
        \\mov %[ucr3], %%rax
        \\mov %%rax, %%cr3
        \\iretq
        :
        : [k_rsp] "r" (kernel_transition_rsp),
          [ucr3] "r" (scheduler.user_cr3_value),
        : .{ .memory = true });
    unreachable;
}

fn kernelMain() void {
    asm volatile ("cli");
    lapic.maskLegacyPic();
    serial.writeRaw("RAW ENTER MAIN\n");
    earlyUefiWrite(&[_:0]u16{ 'E', 'N', 'T', 'E', 'R', ' ', 'M', 'A', 'I', 'N', '\r', '\n' });
    serialInit();
    kernel_log.reset();
    serial.writeRaw("RAW SERIAL INIT DONE\n");
    earlyUefiWrite(&[_:0]u16{ 'S', 'E', 'R', 'I', 'A', 'L', ' ', 'O', 'K', '\r', '\n' });
    deferred_compositor.reset();
    runtime_framebuffer_info = null;
    boot_log_status_flags = 0;
    boot_log_status_page_paddr = 0;
    bootlog_gate_input_start_armed = false;
    bootlog_gate_start_thread0 = false;
    bootlog_gate_start_thread3 = false;
    bootlog_gate_start_thread2 = false;
    bootlog_gate_start_thread4 = false;
    bootlog_gate_start_thread5 = false;
    bootlog_gate_start_thread7 = false;
    startup_process0_shared_sent = false;
    startup_process2_window_sent = false;
    startup_process3_shared_sent = false;
    startup_process4_window_sent = false;
    startup_process5_window_sent = false;
    startup_process7_window_sent = false;
    startup_dynamic_window_sent_count = 0;
    startup_last_client_cap_tick = 0;
    pie_user_launch_thread_available = false;
    pie_user_process6_initialized = false;
    pie_user_disk_image = null;
    mouse_button_demo_launch_thread_available = false;
    keyboard_ascii_demo_launch_thread_available = false;
    mouse_button_demo_disk_image = null;
    keyboard_ascii_demo_disk_image = null;
    process2_launch_page_paddr = 0;
    process2_launch_stack_paddr = 0;
    process4_launch_page_paddr = 0;
    process4_launch_stack_paddr = 0;
    serialWrite("[stage] boot entry\n");
    serialWrite("MicroKernel Phase1 boot\n");
    _ = gdt_user_code_selector;
    _ = gdt_user_data_selector;

    const bs = acquireBootServicesOrHalt();
    captureKernelImageRange(bs);
    logKernelImageRangeIfKnown();
    initMemoryModules();
    const framebuffer_info: ?FramebufferInfo = if (enable_framebuffer_server_step1)
        (acquireFramebufferInfo(bs) orelse {
            serialWrite("GraphicsOutput unavailable or mode unsupported for framebuffer server\n");
            while (true) asm volatile ("hlt");
        })
    else
        null;
    runtime_framebuffer_info = framebuffer_info;
    const disk_framebuffer_server_elf: ?[]const u8 = if (enable_framebuffer_server_step1)
        (if (enable_boot_log_console_process)
            null
        else
            (loadFramebufferServerElfFromDisk(bs) orelse {
                serialWrite("disk framebuffer server ELF load failed\n");
                while (true) asm volatile ("hlt");
            }))
    else
        null;
    const disk_draw_client_elf: ?[]const u8 = if (enable_framebuffer_server_step1)
        (if (enable_boot_log_console_process)
            null
        else
            (loadDrawClientElfFromDisk(bs) orelse {
                serialWrite("disk draw client ELF load failed\n");
                while (true) asm volatile ("hlt");
            }))
    else
        null;
    const disk_boot_log_console_elf: ?[]const u8 = if (enable_framebuffer_server_step1 and enable_boot_log_console_process)
        (loadBootLogConsoleElfFromDisk(bs) orelse {
            serialWrite("disk boot log console ELF load failed\n");
            while (true) asm volatile ("hlt");
        })
    else
        null;
    const disk_mouse_driver_elf: ?[]const u8 = if (enable_framebuffer_server_step1 and enable_boot_log_console_process and enable_virtio_input_mouse)
        (loadMouseDriverElfFromDisk(bs) orelse {
            serialWrite("disk mouse driver ELF load failed\n");
            while (true) asm volatile ("hlt");
        })
    else
        null;
    const disk_keyboard_driver_elf: ?[]const u8 = if (enable_framebuffer_server_step1 and enable_boot_log_console_process and enable_virtio_input_keyboard)
        (loadKeyboardDriverElfFromDisk(bs) orelse {
            serialWrite("disk keyboard driver ELF load failed\n");
            while (true) asm volatile ("hlt");
        })
    else
        null;
    const disk_mouse_button_demo_elf: ?[]const u8 = if (enable_framebuffer_server_step1 and enable_boot_log_console_process and enable_virtio_input_mouse)
        (loadMouseButtonDemoElfFromDisk(bs) orelse {
            serialWrite("disk mouse button demo ELF load failed\n");
            while (true) asm volatile ("hlt");
        })
    else
        null;
    const disk_keyboard_ascii_demo_elf: ?[]const u8 = if (enable_framebuffer_server_step1 and enable_boot_log_console_process and enable_virtio_input_mouse and enable_virtio_input_keyboard)
        (loadKeyboardAsciiDemoElfFromDisk(bs) orelse {
            serialWrite("disk keyboard ascii demo ELF load failed\n");
            while (true) asm volatile ("hlt");
        })
    else
        null;
    const disk_vfs_elf: ?[]const u8 = if (enable_vfs_process)
        (loadVfsElfFromDisk(bs) orelse {
            serialWrite("disk VFS ELF load failed\n");
            while (true) asm volatile ("hlt");
        })
    else
        null;
    const disk_init_elf: ?[]const u8 = if (enable_init_process)
        (loadInitElfFromDisk(bs) orelse {
            serialWrite("disk init ELF load failed\n");
            while (true) asm volatile ("hlt");
        })
    else
        null;
    const disk_bootfs_image: ?[]const u8 = if (enable_vfs_process)
        (loadBootFsImageFromDisk(bs) orelse {
            serialWrite("disk bootfs image load failed\n");
            while (true) asm volatile ("hlt");
        })
    else
        null;
    const disk_compositor_elf: ?[]const u8 = if (enable_framebuffer_server_step1 and enable_boot_log_console_process and enable_virtio_input_mouse)
        (loadCompositorElfFromDisk(bs) orelse {
            serialWrite("disk compositor ELF load failed\n");
            while (true) asm volatile ("hlt");
        })
    else
        null;
    const disk_gpu_compositor_elf: ?[]const u8 = if (enable_framebuffer_server_step1 and enable_boot_log_console_process and enable_virtio_input_mouse)
        (loadGpuCompositorElfFromDisk(bs) orelse {
            serialWrite("disk gpu compositor ELF load failed\n");
            while (true) asm volatile ("hlt");
        })
    else
        null;
    const disk_user_elf: ?[]const u8 = loadUserElfFromDisk(bs) orelse {
        serialWrite("disk user ELF load failed\n");
        while (true) asm volatile ("hlt");
    };
    pie_user_disk_image = disk_user_elf;
    mouse_button_demo_disk_image = disk_mouse_button_demo_elf;
    keyboard_ascii_demo_disk_image = disk_keyboard_ascii_demo_elf;
    if (enable_vfs_process) {
        serialWrite("VFS ELF loaded from disk\n");
        serialWrite("  path=");
        serialWrite(vfs_elf_disk_path_log);
        serialWrite("\n");
        serialWrite("  size=");
        printNumber(disk_vfs_elf.?.len);
        serialWrite(" bytes\n");
        if (disk_init_elf) |init_image| {
            serialWrite("init ELF loaded from disk\n");
            serialWrite("  path=");
            serialWrite(init_elf_disk_path_log);
            serialWrite("\n");
            serialWrite("  size=");
            printNumber(init_image.len);
            serialWrite(" bytes\n");
        }
        serialWrite("bootfs image loaded from disk\n");
        serialWrite("  path=");
        serialWrite(bootfs_image_disk_path_log);
        serialWrite("\n");
        serialWrite("  size=");
        printNumber(disk_bootfs_image.?.len);
        serialWrite(" bytes\n");
    }
    if (enable_framebuffer_server_step1) {
        if (enable_boot_log_console_process) {
            if (enable_virtio_input_mouse) {
                serialWrite("mouse driver ELF loaded from disk\n");
                serialWrite("  path=");
                serialWrite(mouse_driver_elf_disk_path_log);
                serialWrite("\n");
                serialWrite("  size=");
                printNumber(disk_mouse_driver_elf.?.len);
                serialWrite(" bytes\n");
                serialWrite("mouse button demo ELF loaded from disk\n");
                serialWrite("  path=");
                serialWrite(mouse_button_demo_elf_disk_path_log);
                serialWrite("\n");
                serialWrite("  size=");
                printNumber(disk_mouse_button_demo_elf.?.len);
                serialWrite(" bytes\n");
                serialWrite("compositor ELF loaded from disk\n");
                serialWrite("  path=");
                serialWrite(compositor_elf_disk_path_log);
                serialWrite("\n");
                serialWrite("  size=");
                printNumber(disk_compositor_elf.?.len);
                serialWrite(" bytes\n");
                serialWrite("gpu compositor ELF loaded from disk\n");
                serialWrite("  path=");
                serialWrite(gpu_compositor_elf_disk_path_log);
                serialWrite("\n");
                serialWrite("  size=");
                printNumber(disk_gpu_compositor_elf.?.len);
                serialWrite(" bytes\n");
                if (enable_bootlog_wait_for_enter) {
                    serialWrite("boot log console ELF loaded from disk\n");
                    serialWrite("  path=");
                    serialWrite(boot_log_console_elf_disk_path_log);
                    serialWrite("\n");
                    serialWrite("  size=");
                    printNumber(disk_boot_log_console_elf.?.len);
                    serialWrite(" bytes\n");
                    if (enable_virtio_input_keyboard) {
                        serialWrite("keyboard driver ELF loaded from disk\n");
                        serialWrite("  path=");
                        serialWrite(keyboard_driver_elf_disk_path_log);
                        serialWrite("\n");
                        serialWrite("  size=");
                        printNumber(disk_keyboard_driver_elf.?.len);
                        serialWrite(" bytes\n");
                        serialWrite("keyboard ascii demo ELF loaded from disk\n");
                        serialWrite("  path=");
                        serialWrite(keyboard_ascii_demo_elf_disk_path_log);
                        serialWrite("\n");
                        serialWrite("  size=");
                        printNumber(disk_keyboard_ascii_demo_elf.?.len);
                        serialWrite(" bytes\n");
                    }
                }
            } else {
                serialWrite("boot log console ELF loaded from disk\n");
                serialWrite("  path=");
                serialWrite(boot_log_console_elf_disk_path_log);
                serialWrite("\n");
                serialWrite("  size=");
                printNumber(disk_boot_log_console_elf.?.len);
                serialWrite(" bytes\n");
                if (enable_virtio_input_keyboard) {
                    serialWrite("keyboard driver ELF loaded from disk\n");
                    serialWrite("  path=");
                    serialWrite(keyboard_driver_elf_disk_path_log);
                    serialWrite("\n");
                    serialWrite("  size=");
                    printNumber(disk_keyboard_driver_elf.?.len);
                    serialWrite(" bytes\n");
                }
            }
        } else {
            serialWrite("draw client ELF loaded from disk\n");
            serialWrite("  path=");
            serialWrite(draw_client_elf_disk_path_log);
            serialWrite("\n");
            serialWrite("  size=");
            printNumber(disk_draw_client_elf.?.len);
            serialWrite(" bytes\n");
            serialWrite("framebuffer server ELF loaded from disk\n");
            serialWrite("  path=");
            serialWrite(framebuffer_server_elf_disk_path_log);
            serialWrite("\n");
            serialWrite("  size=");
            printNumber(disk_framebuffer_server_elf.?.len);
            serialWrite(" bytes\n");
        }
    } else {
        serialWrite("user ELF loaded from disk\n");
        serialWrite("  path=");
        serialWrite(user_elf_disk_path_log);
        serialWrite("\n");
        serialWrite("  size=");
        printNumber(disk_user_elf.?.len);
        serialWrite(" bytes\n");
    }
    const memory_stats = collectBootMemoryStatsOrHalt(bs);
    exitBootServicesOrHalt();
    initKernelRuntimeOrHalt();
    printKernelBootBanner();
    capability.init(.{
        .user_spaces = user_spaces[0..],
        .user_va = user_va,
        .physical_map_limit = one_tib,
        .page_entries = page_entries,
        .page_addr_mask = page_addr_mask,
        .page_present = page_present,
        .page_rw = page_rw,
        .page_user = page_user,
        .canonical_user_limit_exclusive = canonical_user_limit_exclusive,
        .serial_write = serialWrite,
        .print_hex = printHex,
        .principal_label = principalLabel,
        .flush_user_tlb_for_principal_va = user_copy.flushUserTlbForPrincipalVa,
    });
    user_copy.init(.{
        .four_gib = four_gib,
        .phys_copy_window_va = phys_copy_window_va,
        .page_present = page_present,
        .page_rw = page_rw,
        .kernel_cr3_value = &x86_platform.kernel_cr3_value,
        .phys_copy_window_pt = &x86_platform.phys_copy_window_pt,
        .read_cr3 = readCr3,
        .write_cr3 = writeCr3,
        .invlpg = invlpg,
    });
    page_fault_log.init(.{
        .page_entries = page_entries,
        .page_addr_mask = page_addr_mask,
        .page_present = page_present,
        .page_ps = page_ps,
        .kernel_state_ready = &kernel_state_ready,
        .state = &kernel_state_global,
        .current_user_principal = &scheduler.current_user_principal,
        .write = serialWrite,
        .write_hex_raw = serialWriteHexRaw,
        .write_bool01 = serialWriteBool01,
    });

    kernel_state_global.initFromDetectedRegionsInPlace(memory_stats.detected_regions) catch |err| {
        serialWrite("region init failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    const state = &kernel_state_global;
    if (enable_iommu_no_cap_driver) {
        state.setIommuNoCapDriverMode(if (enforce_iommu_no_cap_driver) .enforce else .shadow);
        state.iommu_audit_hook = iommuAuditHook;
        serialWrite("iommu: no-cap-driver mode=");
        serialWrite(if (enforce_iommu_no_cap_driver) "enforce\n" else "shadow\n");
    } else {
        state.setIommuNoCapDriverMode(.off);
        state.iommu_audit_hook = null;
        serialWrite("iommu: no-cap-driver mode=off\n");
    }
    state.debug_window_hook = null;
    state.debug_alloc_page_hook = null;
    state.pte_sync_hook = null;
    const untyped_bootstrap = untyped_memory.bootstrapProcess0Untyped(state, &global_free_list) catch |err| {
        serialWrite("untyped bootstrap failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    serialWrite("untyped blocks: ");
    printNumber(untyped_bootstrap.block_count);
    serialWrite(" bytes=");
    printNumber(untyped_bootstrap.total_bytes);
    serialWrite("\n");
    const gpu_untyped_grants = untyped_memory.grantProcess0UntypedTo(state, .Process1) catch |err| {
        serialWrite("untyped grant to Process1 failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    serialWrite("untyped grants to Process1: ");
    printNumber(gpu_untyped_grants);
    serialWrite("\n");
    const process2_untyped_grants = untyped_memory.grantProcess0UntypedTo(state, .Process2) catch |err| {
        serialWrite("untyped grant to Process2 failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    const process4_untyped_grants = untyped_memory.grantProcess0UntypedTo(state, .Process4) catch |err| {
        serialWrite("untyped grant to Process4 failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    const process5_untyped_grants = untyped_memory.grantProcess0UntypedTo(state, .Process5) catch |err| {
        serialWrite("untyped grant to Process5 failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    serialWrite("untyped grants to Process2/4/5: ");
    printNumber(process2_untyped_grants);
    serialWrite("/");
    printNumber(process4_untyped_grants);
    serialWrite("/");
    printNumber(process5_untyped_grants);
    serialWrite("\n");
    var process_user_pages: [user_process_count]?kernel.PageCapability = [_]?kernel.PageCapability{null} ** user_process_count;
    var process_user_stack_pages: [user_process_count]?kernel.PageCapability = [_]?kernel.PageCapability{null} ** user_process_count;
    var boot_log_console_page: ?kernel.PageCapability = null;
    var boot_log_console_stack_page: ?kernel.PageCapability = null;
    var mouse_driver_runtime_stack_page: ?kernel.PageCapability = null;
    var mouse_driver_config_page: ?kernel.PageCapability = null;
    var keyboard_driver_config_page: ?kernel.PageCapability = null;
    var compositor_gpu_config_page: ?kernel.PageCapability = null;
    var mouse_shared_page: ?kernel.PageCapability = null;
    var mouse_modern_info: ?virtio_probe.InputModernInfo = null;
    var keyboard_modern_info: ?virtio_probe.InputModernInfo = null;
    var gpu_modern_info: ?virtio_probe.GpuModernInfo = null;
    var mouse_driver_cfg: ?MouseDriverConfig = null;
    var keyboard_driver_cfg: ?MouseDriverConfig = null;
    var gpu_driver_cfg: ?VirtioGpuDriverConfig = null;

    if (enable_virtio_input_mouse and enable_framebuffer_server_step1 and enable_boot_log_console_process) {
        mouse_modern_info = virtio_probe.probeMouseModern(probeWriteLog);
        if (mouse_modern_info) |info| {
            mouse_driver_cfg = .{
                .common = mmioPageWithOffset(info.common_cfg),
                .notify = mmioPageWithOffset(info.notify_cfg),
                .isr = mmioPageWithOffset(info.isr_cfg),
                .device = mmioPageWithOffset(info.device_cfg),
                .notify_off_multiplier = info.notify_off_multiplier,
            };
            serialWrite("virtio-input: modern probe ready for MouseDriverProcess\n");
            serialWrite("  pci=");
            printHex(@as(u64, info.location.bus));
            serialWrite(":");
            printHex(@as(u64, info.location.device));
            serialWrite(".");
            printHex(@as(u64, info.location.function));
            serialWrite("\n");
            serialWrite("  common=");
            printHex(info.common_cfg);
            serialWrite("\n");
            serialWrite("  notify=");
            printHex(info.notify_cfg);
            serialWrite("\n");
            serialWrite("  isr=");
            printHex(info.isr_cfg);
            serialWrite("\n");
            serialWrite("  device=");
            printHex(info.device_cfg);
            serialWrite("\n");
            serialWrite("  notify_off_multiplier=");
            printNumber(info.notify_off_multiplier);
            serialWrite("\n");
        } else {
            serialWrite("virtio-input: MouseDriverProcess disabled (modern input not found)\n");
        }
    }

    if (enable_virtio_input_keyboard and enable_framebuffer_server_step1 and enable_boot_log_console_process) {
        keyboard_modern_info = virtio_probe.probeKeyboardModern(probeWriteLog);
        if (keyboard_modern_info) |info| {
            keyboard_driver_cfg = .{
                .common = mmioPageWithOffset(info.common_cfg),
                .notify = mmioPageWithOffset(info.notify_cfg),
                .isr = mmioPageWithOffset(info.isr_cfg),
                .device = mmioPageWithOffset(info.device_cfg),
                .notify_off_multiplier = info.notify_off_multiplier,
            };
            serialWrite("virtio-input: modern probe ready for KeyboardDriverProcess\n");
            serialWrite("  pci=");
            printHex(@as(u64, info.location.bus));
            serialWrite(":");
            printHex(@as(u64, info.location.device));
            serialWrite(".");
            printHex(@as(u64, info.location.function));
            serialWrite("\n");
            serialWrite("  common=");
            printHex(info.common_cfg);
            serialWrite("\n");
            serialWrite("  notify=");
            printHex(info.notify_cfg);
            serialWrite("\n");
            serialWrite("  isr=");
            printHex(info.isr_cfg);
            serialWrite("\n");
            serialWrite("  device=");
            printHex(info.device_cfg);
            serialWrite("\n");
            serialWrite("  notify_off_multiplier=");
            printNumber(info.notify_off_multiplier);
            serialWrite("\n");
        } else {
            serialWrite("virtio-input: KeyboardDriverProcess disabled (modern input not found)\n");
        }
    }

    if (enable_framebuffer_server_step1 and enable_boot_log_console_process) {
        gpu_modern_info = virtio_probe.probeGpuModern(probeWriteLog);
        if (gpu_modern_info) |info| {
            gpu_driver_cfg = .{
                .common = mmioPageWithOffset(info.common_cfg),
                .notify = mmioPageWithOffset(info.notify_cfg),
                .isr = mmioPageWithOffset(info.isr_cfg),
                .device = mmioPageWithOffset(info.device_cfg),
                .notify_off_multiplier = info.notify_off_multiplier,
            };
            serialWrite("virtio-gpu: modern probe ready for compositor\n");
            serialWrite("  pci=");
            printHex(@as(u64, info.location.bus));
            serialWrite(":");
            printHex(@as(u64, info.location.device));
            serialWrite(".");
            printHex(@as(u64, info.location.function));
            serialWrite("\n");
            serialWrite("  common=");
            printHex(info.common_cfg);
            serialWrite("\n");
            serialWrite("  notify=");
            printHex(info.notify_cfg);
            serialWrite("\n");
            serialWrite("  isr=");
            printHex(info.isr_cfg);
            serialWrite("\n");
            serialWrite("  device=");
            printHex(info.device_cfg);
            serialWrite("\n");
            serialWrite("  notify_off_multiplier=");
            printNumber(info.notify_off_multiplier);
            serialWrite("\n");
        } else {
            serialWrite("virtio-gpu: compositor DMA disabled (modern gpu not found)\n");
        }
    }

    const boot_runtime_mode = determineBootRuntimeMode(mouse_driver_cfg, keyboard_driver_cfg);
    const process_setup = setupUserProcessesForMode(state, boot_runtime_mode, mouse_driver_cfg, keyboard_driver_cfg, gpu_driver_cfg, disk_bootfs_image);
    process_user_pages = process_setup.user_pages;
    process_user_stack_pages = process_setup.user_stack_pages;
    process2_launch_page_paddr = if (processPageForPrincipal(process_user_pages[0..], .Process2)) |page| page.paddr else 0;
    process2_launch_stack_paddr = if (processPageForPrincipal(process_user_stack_pages[0..], .Process2)) |page| page.paddr else 0;
    process4_launch_page_paddr = if (processPageForPrincipal(process_user_pages[0..], .Process4)) |page| page.paddr else 0;
    process4_launch_stack_paddr = if (processPageForPrincipal(process_user_stack_pages[0..], .Process4)) |page| page.paddr else 0;
    boot_log_console_page = process_setup.boot_log_console_page;
    boot_log_console_stack_page = process_setup.boot_log_console_stack_page;
    mouse_driver_runtime_stack_page = process_setup.mouse_driver_runtime_stack_page;
    mouse_driver_config_page = process_setup.mouse_driver_config_page;
    keyboard_driver_config_page = process_setup.keyboard_driver_config_page;
    compositor_gpu_config_page = process_setup.compositor_gpu_config_page;
    mouse_shared_page = process_setup.mouse_shared_page;
    sanitizeAllThreadContexts();
    if (boot_log_console_page) |page| {
        boot_log_status_page_paddr = page.paddr;
    }
    scheduler.scheduler_tick_accum = 0;
    scheduler.scheduler_switch_count = 0;
    boot_timing.reset();
    scheduler.scheduler_int80_log_count = 0;
    scheduler.scheduler_race_log_count = 0;
    scheduler.scheduler_probe_log_count = 0;
    if (enable_title_only_ready_logs) {
        logReadyTitle("USER_PAGE_READY");
    } else {
        serialWrite("user page table ready\n");
        serialWrite("  user_va=");
        printHex(user_va);
        serialWrite("\n");
        serialWrite("  user_pa=");
        switch (boot_runtime_mode) {
            .BootLogConsole, .BootLogGateCompositor => printHex(processPageForPrincipal(process_user_pages[0..], .Process1).?.paddr),
            else => printHex(processPageForPrincipal(process_user_pages[0..], .Process0).?.paddr),
        }
        serialWrite("\n");
        serialWrite("  user_stack_top=");
        if (boot_runtime_mode == .MouseCompositor or boot_runtime_mode == .BootLogConsole or boot_runtime_mode == .BootLogGateCompositor) {
            printHex(boot_log_console_stack_top);
        } else {
            printHex(user_stack_top);
        }
        serialWrite("\n");
        serialWrite("  user_stack_pa=");
        switch (boot_runtime_mode) {
            .MouseCompositor => printHex(mouse_driver_runtime_stack_page.?.paddr),
            .BootLogConsole => printHex(boot_log_console_stack_page.?.paddr),
            .BootLogGateCompositor => printHex(boot_log_console_stack_page.?.paddr),
            else => printHex(processPageForPrincipal(process_user_stack_pages[0..], .Process0).?.paddr),
        }
        serialWrite("\n");
        serialWrite("  process_count=");
        printNumber(user_process_count);
        serialWrite("\n");
        serialWrite("  thread_count=");
        printNumber(user_thread_count);
        serialWrite("\n");
        switch (boot_runtime_mode) {
            .DiskUser => {
                serialWrite("  process0_cr3=");
                printHex(user_spaces[0].cr3);
                serialWrite("\n");
                serialWrite("  thread0_owner=");
                serialWrite(principalLabel(thread_contexts[0].owner_process));
                serialWrite("\n");
                serialWrite("  thread0_ctx_rip=");
                printHex(thread_contexts[0].frame.rip);
            },
            .FramebufferIpc, .BootLogConsole, .BootLogGateCompositor, .MouseCompositor => {
                if (boot_runtime_mode == .MouseCompositor or boot_runtime_mode == .BootLogGateCompositor) {
                    serialWrite("  mouse_owner=Process0\n");
                    serialWrite("  process0_cr3=");
                    printHex(user_spaces[0].cr3);
                    serialWrite("\n");
                } else if (boot_runtime_mode == .FramebufferIpc) {
                    serialWrite("  process0_cr3=");
                    printHex(user_spaces[0].cr3);
                    serialWrite("\n");
                    serialWrite("  server_owner=Process1\n");
                }

                if (boot_runtime_mode == .BootLogConsole) {
                    serialWrite("  console_owner=Process1\n");
                    if (keyboard_driver_cfg != null and thread_contexts[3].ready) {
                        serialWrite("  keyboard_owner=Process3\n");
                    }
                } else if (boot_runtime_mode == .BootLogGateCompositor) {
                    serialWrite("  console_owner=Process1\n");
                    if (thread_contexts[2].ready) serialWrite("  mouse_button_demo_owner=Process2\n");
                    if (keyboard_driver_cfg != null and thread_contexts[3].ready) {
                        serialWrite("  keyboard_owner=Process3\n");
                    }
                    if (thread_contexts[4].ready) {
                        serialWrite("  keyboard_log_owner=Process4\n");
                    }
                    if (thread_contexts[5].ready) {
                        serialWrite("  terminal_owner=Process5\n");
                    }
                } else if (boot_runtime_mode == .MouseCompositor) {
                    serialWrite("  compositor_owner=Process1\n");
                    if (thread_contexts[2].ready) serialWrite("  mouse_button_demo_owner=Process2\n");
                    if (keyboard_driver_cfg != null and thread_contexts[3].ready) {
                        serialWrite("  keyboard_owner=Process3\n");
                    }
                    if (thread_contexts[4].ready) {
                        serialWrite("  keyboard_log_owner=Process4\n");
                    }
                    if (thread_contexts[5].ready) {
                        serialWrite("  terminal_owner=Process5\n");
                    }
                }

                serialWrite("  process1_cr3=");
                printHex(user_spaces[1].cr3);
                serialWrite("\n");
                if (thread_contexts[2].ready) {
                    serialWrite("  process2_cr3=");
                    printHex(user_spaces[2].cr3);
                    serialWrite("\n");
                }
                if (thread_contexts[3].ready) {
                    serialWrite("  process3_cr3=");
                    printHex(user_spaces[3].cr3);
                    serialWrite("\n");
                }
                if (thread_contexts[4].ready) {
                    serialWrite("  process4_cr3=");
                    printHex(user_spaces[4].cr3);
                    serialWrite("\n");
                }
                if (thread_contexts[5].ready) {
                    serialWrite("  process5_cr3=");
                    printHex(user_spaces[5].cr3);
                    serialWrite("\n");
                }
                if (boot_runtime_mode == .FramebufferIpc or thread_contexts[0].ready) {
                    serialWrite("  thread0_owner=");
                    serialWrite(principalLabel(thread_contexts[0].owner_process));
                    serialWrite("\n");
                    serialWrite("  thread0_ctx_rip=");
                    printHex(thread_contexts[0].frame.rip);
                    serialWrite("\n");
                }
                serialWrite("  thread1_owner=");
                serialWrite(principalLabel(thread_contexts[1].owner_process));
                serialWrite("\n");
                serialWrite("  thread1_ctx_rip=");
                printHex(thread_contexts[1].frame.rip);
                serialWrite("\n");
                if (thread_contexts[2].ready) {
                    serialWrite("  thread2_owner=");
                    serialWrite(principalLabel(thread_contexts[2].owner_process));
                    serialWrite("\n");
                    serialWrite("  thread2_ctx_rip=");
                    printHex(thread_contexts[2].frame.rip);
                    serialWrite("\n");
                }
                if (thread_contexts[3].ready) {
                    serialWrite("  thread3_owner=");
                    serialWrite(principalLabel(thread_contexts[3].owner_process));
                    serialWrite("\n");
                    serialWrite("  thread3_ctx_rip=");
                    printHex(thread_contexts[3].frame.rip);
                    serialWrite("\n");
                }
                if (thread_contexts[4].ready) {
                    serialWrite("  thread4_owner=");
                    serialWrite(principalLabel(thread_contexts[4].owner_process));
                    serialWrite("\n");
                    serialWrite("  thread4_ctx_rip=");
                    printHex(thread_contexts[4].frame.rip);
                    serialWrite("\n");
                }
                if (thread_contexts[5].ready) {
                    serialWrite("  thread5_owner=");
                    serialWrite(principalLabel(thread_contexts[5].owner_process));
                    serialWrite("\n");
                    serialWrite("  thread5_ctx_rip=");
                    printHex(thread_contexts[5].frame.rip);
                }
            },
        }
        serialWrite("\n");
        serialWrite("  scheduler_quantum_ticks=");
        printNumber(scheduler_quantum_ticks);
        serialWrite("\n");
    }
    var loaded_elf: ?elf_loader.Image = null;
    if (boot_runtime_mode != .DiskUser) {
        setupFramebufferServerAccess(state, framebuffer_info.?);
    }
    if (enable_vfs_process) {
        const loaded_vfs = loadUserElfIntoProcessPagesOrHalt(
            state,
            kernel.vfs_principal,
            processPageForPrincipal(process_user_pages[0..], kernel.vfs_principal).?.paddr,
            processPageForPrincipal(process_user_stack_pages[0..], kernel.vfs_principal).?.paddr,
            disk_vfs_elf.?,
            "VFS ELF load into user page failed\n",
        );
        setThreadEntryForPrincipal(kernel.vfs_principal, "VFS", loaded_vfs.entry, user_entry_rsp);
        logElfLoadSummary("VFS ELF mapped", loaded_vfs);
        if (!enable_title_only_ready_logs) {
            serialWrite("  vfs_cfg_va=");
            printHex(vfs_config_va);
            serialWrite("\n");
        }
    }
    switch (boot_runtime_mode) {
        .BootLogGateCompositor => {
            const info = framebuffer_info.?;
            const loaded_mouse = loadUserElfIntoProcessPagesOrHalt(
                state,
                .Process0,
                processPageForPrincipal(process_user_pages[0..], .Process0).?.paddr,
                processPageForPrincipal(process_user_stack_pages[0..], .Process0).?.paddr,
                disk_mouse_driver_elf.?,
                "mouse driver ELF load into user page failed\n",
            );
            setThreadEntryForPrincipal(.Process0, "mouse driver", loaded_mouse.entry, boot_log_console_entry_rsp);
            logElfLoadSummary("MouseDriver ELF mapped", loaded_mouse);
            if (!enable_title_only_ready_logs) {
                serialWrite("  cfg_va=");
                printHex(mouse_driver_config_va);
                serialWrite("\n");
            }
            publishMouseSharedPage(mouse_shared_page.?.paddr, info);

            mapUserLinearRegionOrHalt(
                .Process1,
                boot_log_console_stack_page_va,
                boot_log_console_stack_page.?.paddr,
                4096,
                true,
                "boot log console stack page map failed",
            );
            mapUserLinearRegionOrHalt(
                .Process1,
                boot_log_user_va,
                boot_log_console_page.?.paddr,
                4096,
                false,
                "boot log page map failed",
            );
            publishBootLogToUserPage(boot_log_console_page.?.paddr);
            loaded_elf = loadUserElfIntoProcessPagesOrHalt(
                state,
                .Process1,
                processPageForPrincipal(process_user_pages[0..], .Process1).?.paddr,
                processPageForPrincipal(process_user_stack_pages[0..], .Process1).?.paddr,
                disk_boot_log_console_elf.?,
                "boot log console ELF load into user page failed\n",
            );
            setThreadEntryForPrincipal(.Process1, "boot log console", loaded_elf.?.entry, boot_log_console_entry_rsp);
            logElfLoadSummary("BootLogConsole ELF mapped", loaded_elf.?);
            if (!enable_title_only_ready_logs) {
                serialWrite("  log_va=");
                printHex(boot_log_user_va);
                serialWrite("\n");
                serialWrite("  log_bytes=");
                printNumber(if (kernel_log.boot_log_len > boot_log_page_payload_bytes) boot_log_page_payload_bytes else kernel_log.boot_log_len);
                serialWrite("\n");
            }

            if (processPageForPrincipal(process_user_pages[0..], .Process3) != null and processPageForPrincipal(process_user_stack_pages[0..], .Process3) != null and keyboard_driver_config_page != null and keyboard_driver_cfg != null) {
                const loaded_keyboard = loadUserElfIntoProcessPagesOrHalt(
                    state,
                    .Process3,
                    processPageForPrincipal(process_user_pages[0..], .Process3).?.paddr,
                    processPageForPrincipal(process_user_stack_pages[0..], .Process3).?.paddr,
                    disk_keyboard_driver_elf.?,
                    "keyboard driver ELF load into user page failed\n",
                );
                setThreadEntryIfReadyForPrincipal(.Process3, "keyboard driver", loaded_keyboard.entry, user_entry_rsp);
                logElfLoadSummary("KeyboardDriver ELF mapped", loaded_keyboard);
                if (!enable_title_only_ready_logs) {
                    serialWrite("  kbd_cfg_va=");
                    printHex(keyboard_driver_config_va);
                    serialWrite("\n");
                }
            }
            if (processPageForPrincipal(process_user_pages[0..], init_principal) != null and processPageForPrincipal(process_user_stack_pages[0..], init_principal) != null and disk_init_elf != null) {
                const loaded_init = loadUserElfIntoProcessPagesOrHalt(
                    state,
                    init_principal,
                    processPageForPrincipal(process_user_pages[0..], init_principal).?.paddr,
                    processPageForPrincipal(process_user_stack_pages[0..], init_principal).?.paddr,
                    disk_init_elf.?,
                    "init ELF load into user page failed\n",
                );
                setThreadEntryIfReadyForPrincipal(init_principal, "init", loaded_init.entry, user_entry_rsp);
                logElfLoadSummary("Init ELF mapped", loaded_init);
            }
            if (disk_compositor_elf) |compositor_image| {
                armDeferredCompositorLaunch(
                    compositor_image,
                    disk_gpu_compositor_elf,
                    processPageForPrincipal(process_user_pages[0..], .Process1).?.paddr,
                    processPageForPrincipal(process_user_stack_pages[0..], .Process1).?.paddr,
                    false,
                    false,
                );
                if (enable_bootlog_wait_for_enter) {
                    serialWrite("deferred compositor launch armed (F=classic G=gpu Enter=launch)\n");
                } else {
                    serialWrite("deferred compositor launch armed (auto)\n");
                }
            }
        },
        .MouseCompositor => {
            const info = framebuffer_info.?;
            const loaded_mouse = loadUserElfIntoProcessPagesOrHalt(
                state,
                .Process0,
                processPageForPrincipal(process_user_pages[0..], .Process0).?.paddr,
                processPageForPrincipal(process_user_stack_pages[0..], .Process0).?.paddr,
                disk_mouse_driver_elf.?,
                "mouse driver ELF load into user page failed\n",
            );
            setThreadEntryForPrincipal(.Process0, "mouse driver", loaded_mouse.entry, boot_log_console_entry_rsp);
            logElfLoadSummary("MouseDriver ELF mapped", loaded_mouse);
            if (!enable_title_only_ready_logs) {
                serialWrite("  cfg_va=");
                printHex(mouse_driver_config_va);
                serialWrite("\n");
            }

            publishMouseSharedPage(mouse_shared_page.?.paddr, info);

            loaded_elf = loadUserElfIntoProcessPagesOrHalt(
                state,
                .Process1,
                processPageForPrincipal(process_user_pages[0..], .Process1).?.paddr,
                processPageForPrincipal(process_user_stack_pages[0..], .Process1).?.paddr,
                disk_compositor_elf.?,
                "compositor ELF load into user page failed\n",
            );
            setThreadEntryForPrincipal(.Process1, "compositor", loaded_elf.?.entry, user_entry_rsp);
            logElfLoadSummary("Compositor ELF mapped", loaded_elf.?);
            if (!enable_title_only_ready_logs) {
                serialWrite("  shared_va=");
                printHex(mouse_shared_draw_va);
                serialWrite("\n");
                serialWrite("  fb_va=");
                printHex(framebuffer_user_va);
                serialWrite("\n");
            }
            if (processPageForPrincipal(process_user_pages[0..], .Process3) != null and processPageForPrincipal(process_user_stack_pages[0..], .Process3) != null and keyboard_driver_config_page != null and keyboard_driver_cfg != null) {
                const loaded_keyboard = loadUserElfIntoProcessPagesOrHalt(
                    state,
                    .Process3,
                    processPageForPrincipal(process_user_pages[0..], .Process3).?.paddr,
                    processPageForPrincipal(process_user_stack_pages[0..], .Process3).?.paddr,
                    disk_keyboard_driver_elf.?,
                    "keyboard driver ELF load into user page failed\n",
                );
                setThreadEntryIfReadyForPrincipal(.Process3, "keyboard driver", loaded_keyboard.entry, user_entry_rsp);
                logElfLoadSummary("KeyboardDriver ELF mapped", loaded_keyboard);
                if (!enable_title_only_ready_logs) {
                    serialWrite("  kbd_cfg_va=");
                    printHex(keyboard_driver_config_va);
                    serialWrite("\n");
                }
            }
            if (processPageForPrincipal(process_user_pages[0..], init_principal) != null and processPageForPrincipal(process_user_stack_pages[0..], init_principal) != null and disk_init_elf != null) {
                const loaded_init = loadUserElfIntoProcessPagesOrHalt(
                    state,
                    init_principal,
                    processPageForPrincipal(process_user_pages[0..], init_principal).?.paddr,
                    processPageForPrincipal(process_user_stack_pages[0..], init_principal).?.paddr,
                    disk_init_elf.?,
                    "init ELF load into user page failed\n",
                );
                setThreadEntryIfReadyForPrincipal(init_principal, "init", loaded_init.entry, user_entry_rsp);
                logElfLoadSummary("Init ELF mapped", loaded_init);
            }
        },
        .BootLogConsole => {
            mapUserLinearRegionOrHalt(
                .Process1,
                boot_log_console_stack_page_va,
                boot_log_console_stack_page.?.paddr,
                4096,
                true,
                "boot log console stack page map failed",
            );
            mapUserLinearRegionOrHalt(
                .Process1,
                boot_log_user_va,
                boot_log_console_page.?.paddr,
                4096,
                false,
                "boot log page map failed",
            );
            publishBootLogToUserPage(boot_log_console_page.?.paddr);
            loaded_elf = loadUserElfIntoProcessPagesOrHalt(
                state,
                .Process1,
                processPageForPrincipal(process_user_pages[0..], .Process1).?.paddr,
                processPageForPrincipal(process_user_stack_pages[0..], .Process1).?.paddr,
                disk_boot_log_console_elf.?,
                "boot log console ELF load into user page failed\n",
            );
            setThreadEntryForPrincipal(.Process1, "boot log console", loaded_elf.?.entry, boot_log_console_entry_rsp);
            logElfLoadSummary("BootLogConsole ELF mapped", loaded_elf.?);
            if (!enable_title_only_ready_logs) {
                serialWrite("  log_va=");
                printHex(boot_log_user_va);
                serialWrite("\n");
                serialWrite("  log_bytes=");
                printNumber(if (kernel_log.boot_log_len > boot_log_page_payload_bytes) boot_log_page_payload_bytes else kernel_log.boot_log_len);
                serialWrite("\n");
            }
            if (processPageForPrincipal(process_user_pages[0..], .Process3) != null and processPageForPrincipal(process_user_stack_pages[0..], .Process3) != null and keyboard_driver_config_page != null and keyboard_driver_cfg != null) {
                const loaded_keyboard = loadUserElfIntoProcessPagesOrHalt(
                    state,
                    .Process3,
                    processPageForPrincipal(process_user_pages[0..], .Process3).?.paddr,
                    processPageForPrincipal(process_user_stack_pages[0..], .Process3).?.paddr,
                    disk_keyboard_driver_elf.?,
                    "keyboard driver ELF load into user page failed\n",
                );
                setThreadEntryIfReadyForPrincipal(.Process3, "keyboard driver", loaded_keyboard.entry, user_entry_rsp);
                logElfLoadSummary("KeyboardDriver ELF mapped", loaded_keyboard);
                if (!enable_title_only_ready_logs) {
                    serialWrite("  kbd_cfg_va=");
                    printHex(keyboard_driver_config_va);
                    serialWrite("\n");
                }
            }
            if (processPageForPrincipal(process_user_pages[0..], init_principal) != null and processPageForPrincipal(process_user_stack_pages[0..], init_principal) != null and disk_init_elf != null) {
                const loaded_init = loadUserElfIntoProcessPagesOrHalt(
                    state,
                    init_principal,
                    processPageForPrincipal(process_user_pages[0..], init_principal).?.paddr,
                    processPageForPrincipal(process_user_stack_pages[0..], init_principal).?.paddr,
                    disk_init_elf.?,
                    "init ELF load into user page failed\n",
                );
                setThreadEntryIfReadyForPrincipal(init_principal, "init", loaded_init.entry, user_entry_rsp);
                logElfLoadSummary("Init ELF mapped", loaded_init);
            }
        },
        .FramebufferIpc => {
            const loaded_draw_client = loadUserElfIntoUserPageOrHalt(
                processPageForPrincipal(process_user_pages[0..], .Process0).?.paddr,
                disk_draw_client_elf.?,
                "draw client ELF load into user page failed\n",
            );
            setThreadEntryForPrincipal(.Process0, "draw client", loaded_draw_client.entry, null);
            logElfLoadSummary("DrawClient ELF mapped", loaded_draw_client);
            loaded_elf = loadUserElfIntoUserPageOrHalt(
                processPageForPrincipal(process_user_pages[0..], .Process1).?.paddr,
                disk_framebuffer_server_elf.?,
                "framebuffer server ELF load into user page failed\n",
            );
            setThreadEntryForPrincipal(.Process1, "framebuffer server", loaded_elf.?.entry, null);
            logElfLoadSummary("FramebufferServer ELF mapped", loaded_elf.?);
        },
        .DiskUser => {
            loaded_elf = loadUserElfIntoUserPageOrHalt(
                processPageForPrincipal(process_user_pages[0..], .Process0).?.paddr,
                disk_user_elf.?,
                "disk ELF load into user page failed\n",
            );
            setThreadEntryForPrincipal(.Process0, "disk user", loaded_elf.?.entry, null);
            logElfLoadSummary("ELF image loaded", loaded_elf.?);
        },
    }

    state.pte_sync_hook = capability.syncPageTableRightsForPrincipalPaddr;
    kernel_state_ready = true;
    syscalls.init(.{
        .state = &kernel_state_global,
        .free_list = &global_free_list,
        .kernel_state_ready = &kernel_state_ready,
        .enable_cap_table_dump_logs = enable_cap_table_dump_logs,
        .enable_switch_thread_syscall_log = enable_switch_thread_syscall_log,
        .scheduler_log_int80 = scheduler_log_int80,
        .scheduler_int80_log_max_lines = scheduler_int80_log_max_lines,
        .write = serialWrite,
        .print_hex = printHex,
        .print_number = printNumberU64,
        .thread_label = threadLabel,
        .principal_label = principalLabel,
        .principal_from_process_slot = principalFromProcessSlot,
        .dump_all_process_caps = dumpAllProcessCaps,
        .read_user_u64 = user_copy.readUserU64,
        .write_user_u64 = user_copy.writeUserU64,
        .copy_user_bytes_from_va = user_copy.copyUserBytesFromVa,
        .launch_pie_user_thread = launchPieUserThread,
        .spawn_exec = spawnExecFromSyscall,
        .wake_waiting_thread_for_principal = wakeWaitingThreadForPrincipal,
        .switch_to_thread = switchToThread,
        .block_current_thread_for_event = blockCurrentThreadForEvent,
        .log_queue_cap_deny = logQueueCapDeny,
        .log_race_send_cap = logSchedulerRaceSendCap,
        .log_race_switch = logSchedulerRaceSwitch,
        .post_send_cap = syscallPostSendCap,
        .on_mailbox_receive = syscallOnMailboxReceive,
        .should_suppress_serial_user_log = shouldSuppressSerialUserLog,
        .handle_user_log = syscallHandleUserLog,
    });
    traps.init(.{
        .kernel_state_ready = &kernel_state_ready,
        .state = &kernel_state_global,
        .scheduler_quantum_ticks = scheduler_quantum_ticks,
        .compositor_hold_quanta = compositor_thread1_priority_hold_quanta,
        .scheduler_log_switch = scheduler_log_switch,
        .scheduler_switch_log_max_lines = scheduler_switch_log_max_lines,
        .write = serialWrite,
        .write_hex_raw = serialWriteHexRaw,
        .write_bool01 = serialWriteBool01,
        .thread_label = threadLabel,
        .principal_label = principalLabel,
        .read_cr2 = readCr2,
        .read_cr3 = readCr3,
        .dump_page_walk_for_va = page_fault_log.dumpPageWalkForVa,
        .log_page_fault_step2 = page_fault_log.logStep2,
        .halt_loop = haltLoopTrapHook,
        .maybe_log_scheduler_perf_tick = maybeLogSchedulerPerfTick,
        .try_start_bootlog_gate_deferred_input = tryStartBootLogGateDeferredInput,
        .try_auto_launch_deferred_compositor = tryAutoLaunchDeferredCompositor,
        .switch_to_thread = switchToThread,
        .log_race_switch = logSchedulerRaceSwitch,
    });
    switch (boot_runtime_mode) {
        .MouseCompositor => {
            if (keyboard_driver_cfg != null and thread_contexts[3].ready) {
                if (thread_contexts[4].ready and thread_contexts[5].ready) {
                    serialWrite("\nenter ring3 with iretq (mouse driver + compositor + mouse window + keyboard driver + keyboard log + terminal)\n");
                } else if (thread_contexts[4].ready) {
                    serialWrite("\nenter ring3 with iretq (mouse driver + compositor + mouse window + keyboard driver + keyboard log)\n");
                } else {
                    serialWrite("\nenter ring3 with iretq (mouse driver + compositor + mouse window + keyboard driver)\n");
                }
            } else {
                serialWrite("\nenter ring3 with iretq (mouse driver + compositor + mouse window)\n");
            }
        },
        .BootLogConsole => {
            if (keyboard_driver_cfg != null and thread_contexts[3].ready) {
                serialWrite("\nenter ring3 with iretq (boot log console + keyboard driver)\n");
            } else {
                serialWrite("\nenter ring3 with iretq (boot log console)\n");
            }
        },
        .BootLogGateCompositor => {
            if (keyboard_driver_cfg != null and thread_contexts[3].ready) {
                if (thread_contexts[4].ready and thread_contexts[5].ready) {
                    serialWrite("\nenter ring3 with iretq (boot log console + mouse window + keyboard log + terminal + mouse + keyboard; auto compositor)\n");
                } else if (thread_contexts[4].ready) {
                    serialWrite("\nenter ring3 with iretq (boot log console + mouse window + keyboard log + mouse + keyboard; auto compositor)\n");
                } else {
                    serialWrite("\nenter ring3 with iretq (boot log console + mouse window + mouse + keyboard; auto compositor)\n");
                }
            } else {
                serialWrite("\nenter ring3 with iretq (boot log console + mouse window + mouse; auto compositor)\n");
            }
        },
        .FramebufferIpc => serialWrite("\nenter ring3 with iretq (framebuffer IPC mode)\n"),
        .DiskUser => serialWrite("\nenter ring3 with iretq (disk PIE ELF)\n"),
    }
    if (boot_runtime_mode == .BootLogConsole or boot_runtime_mode == .BootLogGateCompositor) {
        if (boot_log_console_page) |page| {
            publishBootLogToUserPage(page.paddr);
        }
    }
    if (boot_runtime_mode == .BootLogGateCompositor) {
        const gate_drivers = enable_bootlog_wait_for_enter;
        armBootLogGateDeferredInputStart(
            gate_drivers and thread_contexts[0].ready,
            gate_drivers and keyboard_driver_cfg != null and thread_contexts[3].ready,
            false,
            false,
            false,
            false,
            if (enable_bootlog_wait_for_enter) bootlog_gate_input_start_delay_ticks else bootlog_gate_auto_input_start_delay_ticks,
        );
    }
    bootlog_entered_user_tick = scheduler.lapic_tick_count;
    boot_timing.state.user_enter_tick = scheduler.lapic_tick_count;
    boot_timing.recordEvent("user_enter", boot_timing.state.user_enter_tick, null);
    const boot_ctx = getThreadContextConst(scheduler.current_thread_index).?;
    enterUserModeIretq(boot_ctx.frame.rip, boot_ctx.frame.rsp);
}

pub fn main() void {
    const boot_stack_top = @as(usize, @intCast(x86_platform.ring0StackTop())) & ~@as(usize, 0xF);
    asm volatile (
        \\mov %[stack_top], %%rsp
        \\mov %[target], %%rax
        \\call *%%rax
        :
        : [stack_top] "r" (boot_stack_top),
          [target] "r" (&kernelMain),
        : .{ .memory = true, .rax = true });
    unreachable;
}
