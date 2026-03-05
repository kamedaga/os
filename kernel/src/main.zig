const std = @import("std");
const kernel = @import("kernel.zig");
const capability = @import("capability.zig");
const elf_loader = @import("elf_loader.zig");
const interrupts = @import("interrupts.zig");
const lapic = @import("lapic.zig");
const virtio_probe = @import("virtio_probe.zig");
const serial = @import("serial.zig");
const user_programs = @import("user_programs.zig");
const uefi = std.os.uefi;
const TrapFrame = interrupts.TrapFrame;
const ExceptionTrapFrame = interrupts.ExceptionTrapFrame;

const page_entries: usize = 512;
const four_gib: u64 = 4 * 1024 * 1024 * 1024;
const one_tib: u64 = 1024 * 1024 * 1024 * 1024;
const two_mib: u64 = 2 * 1024 * 1024;
const pd_table_count: usize = 16; // 16 * 1GiB = 16GiB
const high_mmio_pml4_index: usize = 1; // 512GiB..1024GiB window
const high_mmio_pdp_table_count: usize = page_entries; // 512 * 1GiB = 512GiB
const user_va: u64 = 0x20000000;
const user_elf_base_va: u64 = user_va; // PIE base is chosen by kernel.
const user_stack_top: u64 = 0x20002000;
const user_stack_page_va: u64 = user_stack_top - 0x1000;
const user_entry_rsp: u64 = user_stack_top - 8; // mimic normal call ABI stack alignment at function entry
const user_aux_base_va: u64 = 0x3C00_0000;
const user_aux_reserved_va: u64 = user_aux_base_va;
const user_program_max_load_bytes: usize = @intCast(user_aux_reserved_va - user_va);
const boot_log_console_stack_page_va: u64 = user_aux_base_va + 0x0000;
const boot_log_console_stack_top: u64 = boot_log_console_stack_page_va + 0x1000;
const boot_log_console_entry_rsp: u64 = boot_log_console_stack_top - 8;
const boot_log_user_va: u64 = user_aux_base_va + 0x1000;
const mouse_driver_config_va: u64 = user_aux_base_va + 0x2000;
const keyboard_driver_config_va: u64 = user_aux_base_va + 0x2000;
const mouse_shared_driver_va: u64 = user_aux_base_va + 0x3000;
const mouse_shared_draw_va: u64 = user_aux_base_va + 0x3000;
const virtual_framebuffer_app_va: u64 = user_aux_base_va + 0x4000;
const virtual_framebuffer_compositor_va: u64 = user_aux_base_va + 0x4000;
const framebuffer_user_va: u64 = user_aux_base_va + 0x5000;
const framebuffer_window_bytes: u64 = two_mib - 0x5000; // reserve one page for compositor IPC mapping
const virtual_framebuffer_width: u32 = 32;
const virtual_framebuffer_height: u32 = 32;
const virtual_framebuffer_pitch: u32 = 32;
const enable_framebuffer_server_step1 = true;
const enable_boot_log_console_process = true;
const enable_virtio_input_mouse = true;
const enable_virtio_input_keyboard = true;
const enable_bootlog_wait_for_enter = true;
const enable_title_only_ready_logs = true;
const enable_cap_table_dump_logs = false;
const user_entry_rflags: u64 = 0x202;
const user_unmapped_test_va: u64 = 0x20100000;
const user_dma_verify_va: u64 = 0x20110000;
const user_recovery_stop_va: u64 = 0x20200000;
const reserved_low_mem_end: u64 = 64 * 1024 * 1024;
const page_addr_mask: u64 = 0x000F_FFFF_FFFF_F000;
const canonical_user_limit_exclusive: u64 = 0x0000_8000_0000_0000;
const gdt_kernel_code_selector: u16 = 0x08;
const gdt_kernel_data_selector: u16 = 0x10;
const gdt_user_code_selector: u16 = 0x18;
const gdt_user_data_selector: u16 = 0x20;
const gdt_tss_selector: u16 = 0x28;

const page_present: u64 = 1 << 0;
const page_rw: u64 = 1 << 1;
const page_user: u64 = 1 << 2;
const page_ps: u64 = 1 << 7;
const lapic_timer_vector: u8 = 0x40;
const lapic_timer_initial_count: u32 = 50_000;
const scheduler_quantum_ticks: u64 = 1;
const bootlog_gate_input_start_delay_ticks: u64 = 8;
const scheduler_log_switch = false;
const scheduler_switch_log_max_lines: u64 = 96;
const scheduler_log_int80 = false;
const scheduler_int80_log_max_lines: u64 = 192;
const scheduler_race_log_max_lines: u64 = 128;
const enable_switch_thread_syscall_log = false;
const user_log_max_bytes: usize = 256;
const user_elf_max_size: usize = 8 * 1024 * 1024;
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
const mouse_shared_magic: u64 = 0x4D534852; // "MSHR"

const debug_skip_exit_boot_services = false;
const debug_skip_cr3_switch = false;
const debug_trigger_page_fault_test = false;
const user_process_count: usize = 4;
const user_thread_count: usize = 4;
const UserAddressSpace = capability.UserAddressSpace;

const ThreadContext = struct {
    id: u32 = 0,
    owner_process: kernel.PrincipalId,
    cr3: u64 = 0,
    ready: bool = false,
    frame: TrapFrame = std.mem.zeroes(TrapFrame),
};

var pml4_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var pdp_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var pd_tables: [pd_table_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** pd_table_count;
var high_mmio_pdp_table: [page_entries]u64 align(4096) = [_]u64{0} ** page_entries;
var high_mmio_pd_tables: [high_mmio_pdp_table_count][page_entries]u64 align(4096) = [_][page_entries]u64{[_]u64{0} ** page_entries} ** high_mmio_pdp_table_count;
var user_spaces: [user_process_count]UserAddressSpace = [_]UserAddressSpace{.{}} ** user_process_count;
var thread_contexts: [user_thread_count]ThreadContext = .{
    .{ .id = 0, .owner_process = .Process0 },
    .{ .id = 1, .owner_process = .Process1 },
    .{ .id = 2, .owner_process = .Process2 },
    .{ .id = 3, .owner_process = .Process3 },
};
var global_free_list: kernel.FreePageList = .{};
var idt: [256]interrupts.IdtEntry align(16) = [_]interrupts.IdtEntry{interrupts.zeroIdtEntry()} ** 256;
var gdt: [7]u64 align(16) = .{
    0x0000000000000000, // 0x00 null
    0x00AF9A000000FFFF, // 0x08 kernel code (DPL=0)
    0x00AF92000000FFFF, // 0x10 kernel data (DPL=0)
    0x00AFFA000000FFFF, // 0x18 user code (DPL=3)
    0x00AFF2000000FFFF, // 0x20 user data (DPL=3)
    0x0000000000000000, // 0x28 TSS low
    0x0000000000000000, // 0x30 TSS high
};
var ring0_stack: [64 * 1024]u8 align(16) = [_]u8{0} ** (64 * 1024);
var tss: Tss = std.mem.zeroes(Tss);
var kernel_state_global: kernel.KernelState = undefined;
var kernel_state_ready = false;
var int80_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var pf_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var gp_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var df_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var ud_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var ts_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var np_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var ss_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var timer_trampoline_page: [4096]u8 align(4096) = [_]u8{0} ** 4096;
var int80_trampoline_entry: usize = 0;
var pf_trampoline_entry: usize = 0;
var gp_trampoline_entry: usize = 0;
var df_trampoline_entry: usize = 0;
var ud_trampoline_entry: usize = 0;
var ts_trampoline_entry: usize = 0;
var np_trampoline_entry: usize = 0;
var ss_trampoline_entry: usize = 0;
var timer_trampoline_entry: usize = 0;
export var kernel_cr3_value: u64 = 0;
export var user_cr3_value: u64 = 0;
var current_user_principal: kernel.PrincipalId = .Process0;
var current_thread_index: usize = 0;
var lapic_tick_count: u64 = 0;
var scheduler_tick_accum: u64 = 0;
var scheduler_switch_count: u64 = 0;
var scheduler_int80_log_count: u64 = 0;
var scheduler_race_log_count: u64 = 0;
var user_log_scratch: [user_log_max_bytes]u8 align(16) = undefined;
var user_elf_staging: [user_elf_max_size]u8 align(16) = undefined;
var framebuffer_server_elf_staging: [user_elf_max_size]u8 align(16) = undefined;
var draw_client_elf_staging: [user_elf_max_size]u8 align(16) = undefined;
var boot_log_console_elf_staging: [user_elf_max_size]u8 align(16) = undefined;
var mouse_driver_elf_staging: [user_elf_max_size]u8 align(16) = undefined;
var keyboard_driver_elf_staging: [user_elf_max_size]u8 align(16) = undefined;
var bootlog_sender_elf_staging: [user_elf_max_size]u8 align(16) = undefined;
var compositor_elf_staging: [user_elf_max_size]u8 align(16) = undefined;
var user_elf_load_window: [user_elf_max_size]u8 align(16) = undefined;
var boot_log_buffer: [boot_log_max_bytes]u8 = [_]u8{0} ** boot_log_max_bytes;
var boot_log_len: usize = 0;
var deferred_compositor_launch_armed = false;
var deferred_compositor_launched = false;
var deferred_compositor_user_page_paddr: u64 = 0;
var deferred_compositor_user_stack_page_paddr: u64 = 0;
var deferred_compositor_image: ?[]const u8 = null;
var runtime_framebuffer_info: ?FramebufferInfo = null;
var runtime_framebuffer_log_before_send_cap_done = false;
var boot_log_status_flags: u32 = 0;
var boot_log_status_page_paddr: u64 = 0;
var bootlog_gate_input_start_armed = false;
var bootlog_gate_input_start_tick: u64 = 0;
var bootlog_gate_start_thread0 = false;
var bootlog_gate_start_thread3 = false;

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

const MemoryStats = struct {
    detected_regions: usize,
    total_usable_bytes: u64,
};

const CreatedUserProcess = struct {
    user_page: kernel.PageCapability,
    user_stack_page: kernel.PageCapability,
};

const MouseDriverProcessSetup = struct {
    process: CreatedUserProcess,
    runtime_stack_page: kernel.PageCapability,
    config_page: kernel.PageCapability,
    shared_page: kernel.PageCapability,
    virtual_framebuffer_page: kernel.PageCapability,
};

const BootLogConsoleProcessSetup = struct {
    process: CreatedUserProcess,
    boot_log_page: kernel.PageCapability,
    boot_log_stack_page: kernel.PageCapability,
};

const BootLogSenderProcessSetup = struct {
    process: CreatedUserProcess,
};

const KeyboardDriverProcessSetup = struct {
    process: CreatedUserProcess,
    config_page: kernel.PageCapability,
};

const BootRuntimeMode = enum {
    DiskUser,
    FramebufferIpc,
    BootLogConsole,
    BootLogGateCompositor,
    MouseCompositor,
};

const UserBootProcessSetup = struct {
    process0_user_page: ?kernel.PageCapability = null,
    process0_user_stack_page: ?kernel.PageCapability = null,
    framebuffer_server_user_page: ?kernel.PageCapability = null,
    framebuffer_server_user_stack_page: ?kernel.PageCapability = null,
    process2_user_page: ?kernel.PageCapability = null,
    process2_user_stack_page: ?kernel.PageCapability = null,
    process3_user_page: ?kernel.PageCapability = null,
    process3_user_stack_page: ?kernel.PageCapability = null,
    boot_log_console_page: ?kernel.PageCapability = null,
    boot_log_console_stack_page: ?kernel.PageCapability = null,
    mouse_driver_runtime_stack_page: ?kernel.PageCapability = null,
    mouse_driver_config_page: ?kernel.PageCapability = null,
    keyboard_driver_config_page: ?kernel.PageCapability = null,
    mouse_shared_page: ?kernel.PageCapability = null,
    virtual_framebuffer_page: ?kernel.PageCapability = null,
};

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
};

const ReservedRange = struct {
    start: u64,
    end: u64, // exclusive
};

const GdtPtr = packed struct {
    limit: u16,
    base: u64,
};

const Tss = packed struct {
    _rsv0: u32 = 0,
    rsp0: u64 = 0,
    rsp1: u64 = 0,
    rsp2: u64 = 0,
    _rsv1: u64 = 0,
    ist1: u64 = 0,
    ist2: u64 = 0,
    ist3: u64 = 0,
    ist4: u64 = 0,
    ist5: u64 = 0,
    ist6: u64 = 0,
    ist7: u64 = 0,
    _rsv2: u64 = 0,
    _rsv3: u16 = 0,
    iomap_base: u16 = 0,
};

fn serialInit() void {
    serial.init();
}

fn appendBootLog(text: []const u8) void {
    if (text.len == 0) return;
    if (boot_log_len >= boot_log_buffer.len) return;
    const remaining = boot_log_buffer.len - boot_log_len;
    const copy_len = if (text.len > remaining) remaining else text.len;
    @memcpy(boot_log_buffer[boot_log_len .. boot_log_len + copy_len], text[0..copy_len]);
    boot_log_len += copy_len;
}

fn serialWrite(text: []const u8) void {
    serial.write(text);
    appendBootLog(text);
}

fn serialWriteFmt(comptime fmt: []const u8, args: anytype) void {
    var buf: [256]u8 = undefined;
    const s = std.fmt.bufPrint(buf[0..], fmt, args) catch return;
    serialWrite(s);
}

fn probeWriteLog(text: []const u8) void {
    if (enable_title_only_ready_logs and std.mem.startsWith(u8, text, "virtio-probe:")) return;
    serialWrite(text);
}

fn logReadyTitle(title: []const u8) void {
    serialWrite(title);
    serialWrite("\n");
}

fn serialWriteHexRaw(value: u64) void {
    serial.writeHexRaw(value);
    var buf: [16]u8 = undefined;
    const s = std.fmt.bufPrint(buf[0..], "{x:0>16}", .{value}) catch return;
    appendBootLog(s);
}

fn serialWriteBool01(value: bool) void {
    serial.writeBool01(value);
    appendBootLog(if (value) "1" else "0");
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

fn writeU64LEBytes(ptr: [*]u8, offset: usize, value: u64) void {
    var i: usize = 0;
    while (i < 8) : (i += 1) {
        ptr[offset + i] = @intCast((value >> @intCast(i * 8)) & 0xFF);
    }
}

fn buildCr3SwitchTrampoline(page: *[4096]u8, target: usize) usize {
    @memset(page[0..], 0x90);
    const out: [*]u8 = @ptrCast(page);
    var off: usize = 0;

    out[off] = 0x50; // push rax
    off += 1;
    out[off] = 0x48; // mov rax, imm64
    out[off + 1] = 0xB8;
    writeU64LEBytes(out, off + 2, kernel_cr3_value);
    off += 10;
    out[off] = 0x0F; // mov cr3, rax
    out[off + 1] = 0x22;
    out[off + 2] = 0xD8;
    off += 3;
    out[off] = 0x58; // pop rax
    off += 1;
    out[off] = 0xFF; // jmp qword ptr [rip+0]
    out[off + 1] = 0x25;
    out[off + 2] = 0x00;
    out[off + 3] = 0x00;
    out[off + 4] = 0x00;
    out[off + 5] = 0x00;
    off += 6;
    writeU64LEBytes(out, off, target);
    return @intFromPtr(page);
}

fn installInterruptTrampolines() void {
    int80_trampoline_entry = buildCr3SwitchTrampoline(&int80_trampoline_page, @intFromPtr(&syscallHandlerStub));
    pf_trampoline_entry = buildCr3SwitchTrampoline(&pf_trampoline_page, @intFromPtr(&pageFaultHandlerStub));
    gp_trampoline_entry = buildCr3SwitchTrampoline(&gp_trampoline_page, @intFromPtr(&generalProtectionHandlerStub));
    df_trampoline_entry = buildCr3SwitchTrampoline(&df_trampoline_page, @intFromPtr(&doubleFaultHandlerStub));
    ud_trampoline_entry = buildCr3SwitchTrampoline(&ud_trampoline_page, @intFromPtr(&invalidOpcodeHandlerStub));
    ts_trampoline_entry = buildCr3SwitchTrampoline(&ts_trampoline_page, @intFromPtr(&invalidTssHandlerStub));
    np_trampoline_entry = buildCr3SwitchTrampoline(&np_trampoline_page, @intFromPtr(&segmentNotPresentHandlerStub));
    ss_trampoline_entry = buildCr3SwitchTrampoline(&ss_trampoline_page, @intFromPtr(&stackSegmentFaultHandlerStub));
    timer_trampoline_entry = buildCr3SwitchTrampoline(&timer_trampoline_page, @intFromPtr(&timerInterruptHandlerStub));
}

fn readCr2() u64 {
    var value: u64 = 0;
    asm volatile ("mov %%cr2, %[out]"
        : [out] "=r" (value),
    );
    return value;
}

fn exceptionName(vec: u64) []const u8 {
    return switch (vec) {
        13 => "GENERAL PROTECTION",
        14 => "PAGE FAULT",
        else => "EXCEPTION",
    };
}

fn processIndex(principal: kernel.PrincipalId) ?usize {
    return switch (principal) {
        .Process0 => 0,
        .Process1 => 1,
        .Process2 => 2,
        .Process3 => 3,
        else => null,
    };
}

fn principalLabel(principal: kernel.PrincipalId) []const u8 {
    return switch (principal) {
        .Process0 => "Process0",
        .Process1 => "Process1",
        .Process2 => "Process2",
        .Process3 => "Process3",
        .Device0 => "Device0",
    };
}

fn threadLabel(thread_index: usize) []const u8 {
    return switch (thread_index) {
        0 => "Thread0",
        1 => "Thread1",
        2 => "Thread2",
        3 => "Thread3",
        else => "Thread?",
    };
}

fn tryBeginSchedulerRaceLog() bool {
    if (scheduler_race_log_count >= scheduler_race_log_max_lines) return false;
    scheduler_race_log_count +%= 1;
    serialWrite("SCHED race ");
    return true;
}

fn logSchedulerRaceSendCap(
    from: kernel.PrincipalId,
    to: ?kernel.PrincipalId,
    endpoint_id: u64,
    paddr: u64,
    reason: []const u8,
) void {
    if (!tryBeginSchedulerRaceLog()) return;
    serialWrite("send_cap from=");
    serialWrite(principalLabel(from));
    serialWrite(" to=");
    if (to) |target| {
        serialWrite(principalLabel(target));
    } else {
        serialWrite("unknown");
    }
    serialWrite(" ep=");
    printHex(endpoint_id);
    serialWrite(" paddr=");
    printHex(paddr);
    serialWrite(" reason=");
    serialWrite(reason);
    serialWrite("\n");
}

fn logSchedulerRaceSwitch(current_thread: usize, target_thread: usize, reason: []const u8) void {
    if (!tryBeginSchedulerRaceLog()) return;
    serialWrite("switch_thread from=");
    serialWrite(threadLabel(current_thread));
    serialWrite(" to=");
    serialWrite(threadLabel(target_thread));
    serialWrite(" reason=");
    serialWrite(reason);
    serialWrite("\n");
}

fn getThreadContext(thread_index: usize) ?*ThreadContext {
    if (thread_index >= user_thread_count) return null;
    return &thread_contexts[thread_index];
}

fn getThreadContextConst(thread_index: usize) ?*const ThreadContext {
    if (thread_index >= user_thread_count) return null;
    return &thread_contexts[thread_index];
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

fn initThreadContext(thread_index: usize, owner_process: kernel.PrincipalId) bool {
    const space = getUserSpace(owner_process) orelse return false;
    const ctx = getThreadContext(thread_index) orelse return false;
    ctx.id = @intCast(thread_index);
    ctx.owner_process = owner_process;
    ctx.cr3 = space.cr3;
    ctx.ready = true;
    ctx.frame = buildInitialUserTrapFrame();
    return true;
}

fn activateThread(thread_index: usize) bool {
    const ctx = getThreadContextConst(thread_index) orelse return false;
    if (!ctx.ready) return false;
    current_thread_index = thread_index;
    current_user_principal = ctx.owner_process;
    user_cr3_value = ctx.cr3;
    return true;
}

fn saveCurrentThreadContextFromFrame(frame: *const TrapFrame) void {
    const ctx = getThreadContext(current_thread_index) orelse return;
    ctx.frame = frame.*;
    ctx.cr3 = user_cr3_value;
    ctx.ready = true;
}

fn loadThreadContextToFrame(thread_index: usize, frame: *TrapFrame) bool {
    const ctx = getThreadContextConst(thread_index) orelse return false;
    if (!ctx.ready) return false;
    frame.* = ctx.frame;
    return true;
}

fn switchToThread(next_thread: usize, frame: *TrapFrame, saved_rax: ?u64) bool {
    if (next_thread >= user_thread_count) return false;
    const current_thread = current_thread_index;
    if (next_thread == current_thread) {
        if (saved_rax) |value| frame.rax = value;
        return true;
    }

    var saved = frame.*;
    if (saved_rax) |value| saved.rax = value;
    saveCurrentThreadContextFromFrame(&saved);

    if (!activateThread(next_thread)) return false;
    if (!loadThreadContextToFrame(next_thread, frame)) {
        _ = activateThread(current_thread);
        _ = loadThreadContextToFrame(current_thread, frame);
        return false;
    }
    return true;
}

fn isUserTrapFrame(frame: *const TrapFrame) bool {
    return ((frame.cs & 0x3) == 0x3) and ((frame.ss & 0x3) == 0x3);
}

fn pickNextReadyThreadIndex(current_index: usize) usize {
    if (current_index >= user_thread_count) return 0;
    var step: usize = 1;
    while (step <= user_thread_count) : (step += 1) {
        const idx = (current_index + step) % user_thread_count;
        const ctx = getThreadContextConst(idx) orelse continue;
        if (ctx.ready) return idx;
    }
    return current_index;
}

fn getUserSpace(principal: kernel.PrincipalId) ?*UserAddressSpace {
    const idx = processIndex(principal) orelse return null;
    if (idx >= user_spaces.len) return null;
    return &user_spaces[idx];
}

fn currentUserSpace() *UserAddressSpace {
    return getUserSpace(current_user_principal).?;
}

fn findUserPtSlotForPd(space: *const UserAddressSpace, pd_index: usize) ?usize {
    var slot: usize = 0;
    const used_len: usize = @intCast(space.pt_page_used_len);
    while (slot < used_len) : (slot += 1) {
        if (space.pt_page_pd_index[slot] == pd_index) return slot;
    }
    return null;
}

fn ensureUserPtSlotForPd(space: *UserAddressSpace, pd_index: usize) ?usize {
    if (pd_index >= page_entries) return null;
    if (findUserPtSlotForPd(space, pd_index)) |slot| return slot;

    var used_len: usize = @intCast(space.pt_page_used_len);
    if (used_len >= UserAddressSpace.max_dynamic_pt_pages) return null;

    const slot = used_len;
    space.pt_page_pd_index[slot] = @intCast(pd_index);
    @memset(space.pt_pages[slot][0..], 0);
    const pt_pa: u64 = @intFromPtr(&space.pt_pages[slot]);
    if (pt_pa >= four_gib) return null;
    space.pd[pd_index] = pt_pa | page_present | page_rw | page_user;
    used_len += 1;
    space.pt_page_used_len = @intCast(used_len);
    return slot;
}

fn mapUserLinearRegion(
    principal: kernel.PrincipalId,
    va_start: u64,
    paddr_start: u64,
    size_bytes: usize,
    writable: bool,
) bool {
    const space = getUserSpace(principal) orelse return false;
    if (size_bytes == 0) return false;
    if ((va_start & 0xFFF) != 0 or (paddr_start & 0xFFF) != 0) return false;

    const map_end_va = va_start + size_bytes - 1;
    const map_end_pa = paddr_start + size_bytes - 1;
    if (map_end_pa >= four_gib) return false;

    const user_pdp_index: usize = @intCast((user_va >> 30) & 0x1FF);
    const user_pd_index_base: usize = @intCast((user_va >> 21) & 0x1FF);
    const start_pml4: usize = @intCast((va_start >> 39) & 0x1FF);
    const start_pdp: usize = @intCast((va_start >> 30) & 0x1FF);
    const end_pml4: usize = @intCast((map_end_va >> 39) & 0x1FF);
    const end_pdp: usize = @intCast((map_end_va >> 30) & 0x1FF);
    if (start_pml4 != 0 or end_pml4 != 0) return false;
    if (start_pdp != user_pdp_index or end_pdp != user_pdp_index) return false;
    _ = user_pd_index_base;

    var offset: u64 = 0;
    while (offset < size_bytes) : (offset += 4096) {
        const va = va_start + offset;
        const paddr = paddr_start + offset;
        const pd_index: usize = @intCast((va >> 21) & 0x1FF);
        const pt_slot = ensureUserPtSlotForPd(space, pd_index) orelse return false;
        const pt_index: usize = @intCast((va >> 12) & 0x1FF);
        space.pt_pages[pt_slot][pt_index] = paddr | page_present | page_user | (if (writable) page_rw else 0);
    }

    return true;
}

fn logPageFaultStep2(cr2: u64, frame: *const ExceptionTrapFrame) void {
    const ec_user = (frame.error_code & (1 << 2)) != 0;
    const va_user = capability.isUserCanonicalVa(cr2);

    serialWrite("  USER_MODE=");
    serialWriteBool01(ec_user);
    serialWrite("\n");
    serialWrite("  USER_VA=");
    serialWriteBool01(va_user);
    serialWrite("\n");

    const pf_cap = capability.issuePageFaultCapability(current_user_principal, frame, cr2) orelse {
        serialWrite("  PF_CAP=none\n");
        serialWrite("  CAP_LOOKUP=skip\n");
        return;
    };
    serialWrite("  PF_CAP=issued\n");

    const candidate_paddr = pf_cap.candidate_paddr orelse {
        serialWrite("  CAND_PADDR=none\n");
        serialWrite("  CAP_LOOKUP=none\n");
        return;
    };
    serialWrite("  CAND_PADDR=");
    serialWriteHexRaw(candidate_paddr);
    serialWrite("\n");

    if (!kernel_state_ready) {
        serialWrite("  CAP_LOOKUP=kernel_state_not_ready\n");
        return;
    }

    const has_cap = kernel_state_global.getTableConst(pf_cap.principal).find(candidate_paddr) != null;
    serialWrite("  CAP_LOOKUP=");
    serialWrite(if (has_cap) "found(current)\n" else "none(current)\n");
}

pub export fn pageFaultDispatch(frame: *const ExceptionTrapFrame) callconv(.c) u64 {
    const cr2 = readCr2();
    const pf_cap = capability.issuePageFaultCapability(current_user_principal, frame, cr2) orelse return 0;
    if (!kernel_state_ready) return 0;
    if (!capability.resolvePageFaultCapability(&kernel_state_global, pf_cap)) return 0;

    serialWrite("PAGE FAULT RESOLVED\n");
    serialWrite("  CR2=");
    serialWriteHexRaw(cr2);
    serialWrite("\n");
    serialWrite("  PF_CAP=consumed\n");
    return 1;
}

pub export fn exceptionWithErrorCommon(vec: u64, frame: *const ExceptionTrapFrame) callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite(exceptionName(vec));
    serialWrite("\n");
    if (vec == 14) {
        serialWrite("  CR2=");
        const cr2 = readCr2();
        serialWriteHexRaw(cr2);
        serialWrite("\n");
        logPageFaultStep2(cr2, frame);
    }
    serialWrite("  EC=");
    serialWriteHexRaw(frame.error_code);
    serialWrite("\n");
    serialWrite("  RIP=");
    serialWriteHexRaw(frame.rip);
    serialWrite("\n");
    haltLoop();
}

pub export fn doubleFaultHandlerCommon(error_code: u64) callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("DOUBLE FAULT\n");
    serialWrite("  EC=");
    serialWriteHexRaw(error_code);
    serialWrite("\n");
    while (true) {
        asm volatile ("hlt");
    }
}

fn haltLoop() noreturn {
    while (true) asm volatile ("hlt");
}

fn copyUserBytesFromVa(principal: kernel.PrincipalId, src_user_va: u64, dest: []u8) bool {
    if (dest.len == 0) return true;

    var copied: usize = 0;
    while (copied < dest.len) {
        const copied_u64: u64 = @intCast(copied);
        const cur_va, const va_overflow = @addWithOverflow(src_user_va, copied_u64);
        if (va_overflow != 0) return false;

        const page_va = cur_va & ~@as(u64, 0xFFF);
        const page_off: usize = @intCast(cur_va & 0xFFF);
        const page_paddr = capability.lookupUserMappedPaddrForVa(principal, page_va) orelse return false;
        if (page_paddr >= four_gib) return false;

        const page_remaining: usize = 4096 - page_off;
        const total_remaining: usize = dest.len - copied;
        const chunk_len: usize = if (total_remaining < page_remaining) total_remaining else page_remaining;

        const page_off_u64: u64 = @intCast(page_off);
        const src_paddr, const paddr_overflow = @addWithOverflow(page_paddr, page_off_u64);
        if (paddr_overflow != 0) return false;
        if (src_paddr >= four_gib) return false;
        if (chunk_len == 0) return false;
        const last_paddr, const last_overflow = @addWithOverflow(src_paddr, @as(u64, @intCast(chunk_len - 1)));
        if (last_overflow != 0 or last_paddr >= four_gib) return false;

        const src: [*]const u8 = @ptrFromInt(src_paddr);
        var i: usize = 0;
        while (i < chunk_len) : (i += 1) {
            dest[copied + i] = src[i];
        }
        copied += chunk_len;
    }
    return true;
}

fn copyBytesToUserVa(principal: kernel.PrincipalId, dest_user_va: u64, src: []const u8) bool {
    if (src.len == 0) return true;

    var copied: usize = 0;
    while (copied < src.len) {
        const copied_u64: u64 = @intCast(copied);
        const cur_va, const va_overflow = @addWithOverflow(dest_user_va, copied_u64);
        if (va_overflow != 0) return false;

        const page_va = cur_va & ~@as(u64, 0xFFF);
        const page_off: usize = @intCast(cur_va & 0xFFF);
        const page_paddr = capability.lookupUserMappedPaddrForVa(principal, page_va) orelse return false;
        if (page_paddr >= four_gib) return false;

        const page_remaining: usize = 4096 - page_off;
        const total_remaining: usize = src.len - copied;
        const chunk_len: usize = if (total_remaining < page_remaining) total_remaining else page_remaining;
        if (chunk_len == 0) return false;

        const page_off_u64: u64 = @intCast(page_off);
        const dst_paddr, const paddr_overflow = @addWithOverflow(page_paddr, page_off_u64);
        if (paddr_overflow != 0) return false;
        if (dst_paddr >= four_gib) return false;
        const last_paddr, const last_overflow = @addWithOverflow(dst_paddr, @as(u64, @intCast(chunk_len - 1)));
        if (last_overflow != 0 or last_paddr >= four_gib) return false;

        const dst: [*]u8 = @ptrFromInt(dst_paddr);
        var i: usize = 0;
        while (i < chunk_len) : (i += 1) {
            dst[i] = src[copied + i];
        }
        copied += chunk_len;
    }
    return true;
}

fn writeUserU64(principal: kernel.PrincipalId, dest_user_va: u64, value: u64) bool {
    var buf: [8]u8 = undefined;
    std.mem.writeInt(u64, buf[0..], value, .little);
    return copyBytesToUserVa(principal, dest_user_va, buf[0..]);
}

pub export fn invalidTssHandlerCommon(error_code: u64) callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("INVALID TSS\n");
    serialWrite("  EC=");
    serialWriteHexRaw(error_code);
    serialWrite("\n");
    haltLoop();
}

pub export fn segmentNotPresentHandlerCommon(error_code: u64) callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("SEGMENT NOT PRESENT\n");
    serialWrite("  EC=");
    serialWriteHexRaw(error_code);
    serialWrite("\n");
    haltLoop();
}

pub export fn stackSegmentFaultHandlerCommon(error_code: u64) callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("STACK SEGMENT FAULT\n");
    serialWrite("  EC=");
    serialWriteHexRaw(error_code);
    serialWrite("\n");
    haltLoop();
}

pub export fn invalidOpcodeHandlerCommon() callconv(.c) noreturn {
    asm volatile ("cli");
    serialWrite("INVALID OPCODE\n");
    haltLoop();
}

pub export fn syscallDispatch(frame: *TrapFrame) callconv(.c) u64 {
    if (!kernel_state_ready) return syscall_err_not_ready;
    const state = &kernel_state_global;
    const proc = current_user_principal;
    if (scheduler_log_int80 and scheduler_int80_log_count < scheduler_int80_log_max_lines) {
        serialWrite("INT80 dispatch ");
        serialWrite(threadLabel(current_thread_index));
        serialWrite("/");
        serialWrite(principalLabel(proc));
        serialWrite(" SYS=");
        printHex(frame.rax);
        serialWrite("\n");
        scheduler_int80_log_count +%= 1;
    }

    switch (frame.rax) {
        syscall_alloc_page => {
            const cap = state.allocPageTo(proc, &global_free_list) catch |err| {
                serialWrite("sys_alloc_page failed proc=");
                serialWrite(principalLabel(proc));
                serialWrite(" err=");
                serialWrite(@errorName(err));
                serialWrite(" caps=");
                printNumber(state.getTableConst(proc).len);
                serialWrite("/");
                printNumber(kernel.CNode.max_caps);
                serialWrite(" free_pages=");
                printNumber(global_free_list.len);
                serialWrite("\n");
                return syscall_err_alloc;
            };
            return cap.paddr;
        },
        syscall_map_page => {
            const writable = (frame.rdx & 0x1) != 0;
            if (capability.mapUserPageFromCapability(state, proc, frame.rdi, frame.rsi, writable)) {
                return syscall_ok;
            }
            return syscall_err_map;
        },
        syscall_map_mmio => {
            const writable = (frame.rdx & 0x1) != 0;
            if (capability.mapUserPageFromCapability(state, proc, frame.rdi, frame.rsi, writable)) {
                return syscall_ok;
            }
            return syscall_err_map;
        },
        syscall_alloc_map_pages => {
            const base_va = frame.rdi;
            const page_count_u64 = frame.rsi;
            const writable = (frame.rdx & 0x1) != 0;
            const out_paddr_list_va = frame.rcx;

            if ((base_va & 0xFFF) != 0) return syscall_err_invalid;
            if (page_count_u64 == 0) return syscall_err_invalid;
            if (page_count_u64 > syscall_batch_max_pages) return syscall_err_invalid;

            const page_count: usize = @intCast(page_count_u64);
            var i: usize = 0;
            while (i < page_count) : (i += 1) {
                const cap = state.allocPageTo(proc, &global_free_list) catch return syscall_err_alloc;

                const i_u64: u64 = @intCast(i);
                const offset_4k, const mul_overflow = @mulWithOverflow(i_u64, @as(u64, 4096));
                if (mul_overflow != 0) return syscall_err_map;
                const map_va, const va_overflow = @addWithOverflow(base_va, offset_4k);
                if (va_overflow != 0) return syscall_err_map;

                if (!capability.mapUserPageFromCapability(state, proc, map_va, cap.paddr, writable)) {
                    return syscall_err_map;
                }

                if (out_paddr_list_va != 0) {
                    const offset_8, const list_mul_overflow = @mulWithOverflow(i_u64, @as(u64, 8));
                    if (list_mul_overflow != 0) return syscall_err_invalid;
                    const list_va, const list_va_overflow = @addWithOverflow(out_paddr_list_va, offset_8);
                    if (list_va_overflow != 0) return syscall_err_invalid;
                    if (!writeUserU64(proc, list_va, cap.paddr)) return syscall_err_map;
                }
            }
            return syscall_ok;
        },
        syscall_move_cap => {
            const to = switch (frame.rsi) {
                0 => proc,
                1 => kernel.PrincipalId.Device0,
                else => return syscall_err_invalid,
            };
            const from = if (to == proc) kernel.PrincipalId.Device0 else proc;
            const rights = capability.parseRights(frame.rdx);
            state.moveCap(from, to, frame.rdi, rights) catch return syscall_err_move;
            return syscall_ok;
        },
        syscall_grant_cap => {
            const to = switch (frame.rsi) {
                0 => kernel.PrincipalId.Process0,
                1 => kernel.PrincipalId.Process1,
                2 => kernel.PrincipalId.Process2,
                3 => kernel.PrincipalId.Process3,
                else => return syscall_err_invalid,
            };
            const rights = capability.parseRights(frame.rdx);
            state.grantCap(proc, to, frame.rdi, rights) catch return syscall_err_grant;
            serialWrite("grant_cap from=");
            serialWrite(principalLabel(proc));
            serialWrite(" to=");
            serialWrite(principalLabel(to));
            serialWrite(" paddr=");
            printHex(frame.rdi);
            serialWrite("\n");
            if (enable_cap_table_dump_logs) {
                capability.dumpPrincipalCaps(state, .Process0, "Process0");
                capability.dumpPrincipalCaps(state, .Process1, "Process1");
                capability.dumpPrincipalCaps(state, .Process2, "Process2");
                capability.dumpPrincipalCaps(state, .Process3, "Process3");
            }
            return syscall_ok;
        },
        syscall_send_cap => {
            const endpoint_id = frame.rsi;
            const to = state.endpointTargetFor(proc, endpoint_id) orelse {
                logSchedulerRaceSendCap(proc, null, endpoint_id, frame.rdi, "endpoint_not_found");
                return syscall_err_endpoint;
            };
            if (proc == .Process0) {
                logRuntimeFramebufferSummaryBeforeSendCap();
            }
            state.sendCapOnEndpoint(proc, endpoint_id, frame.rdi) catch |err| switch (err) {
                kernel.KernelError.EndpointNotFound => {
                    logSchedulerRaceSendCap(proc, null, endpoint_id, frame.rdi, "endpoint_not_found");
                    return syscall_err_endpoint;
                },
                kernel.KernelError.CapabilityNotFound => {
                    logSchedulerRaceSendCap(proc, to, endpoint_id, frame.rdi, "cap_missing");
                    return syscall_err_send;
                },
                else => {
                    logSchedulerRaceSendCap(proc, to, endpoint_id, frame.rdi, @errorName(err));
                    return syscall_err_send;
                },
            };
            serialWriteFmt(
                "send_cap from={s} to={s} ep=0x{x} paddr=0x{x}\n",
                .{ principalLabel(proc), principalLabel(to), endpoint_id, frame.rdi },
            );
            if (enable_cap_table_dump_logs) {
                capability.dumpPrincipalCaps(state, .Process0, "Process0");
                capability.dumpPrincipalCaps(state, .Process1, "Process1");
                capability.dumpPrincipalCaps(state, .Process2, "Process2");
                capability.dumpPrincipalCaps(state, .Process3, "Process3");
            }
            return syscall_ok;
        },
        syscall_recv_cap => {
            return state.recvCap(proc) catch |err| switch (err) {
                kernel.KernelError.MailboxEmpty => syscall_err_empty,
                else => syscall_err_send,
            };
        },
        syscall_revoke_tree => {
            state.revokeCapTree(proc, frame.rdi) catch return syscall_err_revoke;
            serialWrite("revoke_tree by=");
            serialWrite(principalLabel(proc));
            serialWrite(" paddr=");
            printHex(frame.rdi);
            serialWrite("\n");
            if (enable_cap_table_dump_logs) {
                capability.dumpPrincipalCaps(state, .Process0, "Process0");
                capability.dumpPrincipalCaps(state, .Process1, "Process1");
                capability.dumpPrincipalCaps(state, .Process2, "Process2");
                capability.dumpPrincipalCaps(state, .Process3, "Process3");
            }
            return syscall_ok;
        },
        syscall_drop_present => {
            if (capability.dropPresentForUserMappedPaddr(state, proc, frame.rdi)) {
                return syscall_ok;
            }
            return syscall_err_drop_present;
        },
        syscall_switch_thread => {
            const target_thread: usize = @intCast(frame.rdi);
            if (target_thread >= user_thread_count) {
                serialWrite("switch_thread invalid target=");
                printHex(frame.rdi);
                serialWrite("\n");
                return syscall_err_invalid;
            }
            const current_thread = current_thread_index;
            const target_ctx = getThreadContextConst(target_thread).?;
            if (!target_ctx.ready) {
                logSchedulerRaceSwitch(current_thread, target_thread, "target_not_ready");
                return syscall_err_not_ready;
            }
            if (!switchToThread(target_thread, frame, syscall_ok)) {
                logSchedulerRaceSwitch(current_thread, target_thread, "context_switch_failed");
                return syscall_err_not_ready;
            }
            if (enable_switch_thread_syscall_log) {
                serialWrite("switch_thread ok from=");
                serialWrite(threadLabel(current_thread));
                serialWrite(" to=");
                serialWrite(threadLabel(target_thread));
                serialWrite("\n");
            }
            return syscall_ok;
        },
        syscall_log => {
            const req_len_u64 = frame.rsi;
            if (req_len_u64 == 0) return syscall_ok;
            if (req_len_u64 > user_log_scratch.len) return syscall_err_log;

            const req_len: usize = @intCast(req_len_u64);
            if (!copyUserBytesFromVa(proc, frame.rdi, user_log_scratch[0..req_len])) return syscall_err_log;

            serialWrite("userlog ");
            serialWrite(threadLabel(current_thread_index));
            serialWrite(": ");
            serialWrite(user_log_scratch[0..req_len]);
            if (user_log_scratch[req_len - 1] != '\n') {
                serialWrite("\n");
            }
            updateBootLogQueueReadyStatusFromUserLog(proc, user_log_scratch[0..req_len]);
            tryLaunchDeferredCompositorFromLog(frame, proc, user_log_scratch[0..req_len]);
            return syscall_ok;
        },
        else => return syscall_err_invalid,
    }
}

pub export fn timerInterruptDispatch(frame: *TrapFrame) callconv(.c) void {
    lapic_tick_count +%= 1;
    lapic.eoi();
    if (!kernel_state_ready) return;
    if (scheduler_quantum_ticks == 0) return;
    if (!isUserTrapFrame(frame)) return;
    tryStartBootLogGateDeferredInput();

    scheduler_tick_accum +%= 1;
    if (scheduler_tick_accum < scheduler_quantum_ticks) return;
    scheduler_tick_accum = 0;

    const current_thread = current_thread_index;
    const next_thread = pickNextReadyThreadIndex(current_thread);
    if (next_thread == current_thread) return;

    if (!switchToThread(next_thread, frame, null)) {
        logSchedulerRaceSwitch(current_thread, next_thread, "timer_preempt_switch_failed");
        return;
    }

    scheduler_switch_count +%= 1;
    if (scheduler_log_switch and scheduler_switch_count <= scheduler_switch_log_max_lines) {
        serialWrite("SCHED switch ");
        const current_ctx = getThreadContextConst(current_thread).?;
        const next_ctx = getThreadContextConst(next_thread).?;
        serialWrite(threadLabel(current_thread));
        serialWrite("/");
        serialWrite(principalLabel(current_ctx.owner_process));
        serialWrite(" -> ");
        serialWrite(threadLabel(next_thread));
        serialWrite("/");
        serialWrite(principalLabel(next_ctx.owner_process));
        serialWrite("\n");
    }
}

pub export fn pageFaultHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\push %rax
        \\push %rbx
        \\push %rcx
        \\push %rdx
        \\push %rsi
        \\push %rdi
        \\push %rbp
        \\push %r8
        \\push %r9
        \\push %r10
        \\push %r11
        \\push %r12
        \\push %r13
        \\push %r14
        \\push %r15
        \\mov %rsp, %rcx
        // Keep original stack pointer in a callee-saved register across the C call.
        \\mov %rsp, %r15
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\call pageFaultDispatch
        \\mov %r15, %rsp
        \\test %rax, %rax
        \\jz 1f
        \\pop %r15
        \\pop %r14
        \\pop %r13
        \\pop %r12
        \\pop %r11
        \\pop %r10
        \\pop %r9
        \\pop %r8
        \\pop %rbp
        \\pop %rdi
        \\pop %rsi
        \\pop %rdx
        \\pop %rcx
        \\pop %rbx
        \\pop %rax
        \\push %r10
        \\mov user_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        // #PF has error_code on stack; remove it before iretq.
        \\add $8, %rsp
        \\iretq
        \\1:
        \\mov %rsp, %rdx
        \\mov %rsp, %r15
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\mov $14, %rcx
        \\call exceptionWithErrorCommon
        \\ud2
    );
}

pub export fn doubleFaultHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\mov (%rsp), %rdi
        \\mov %rdi, %rcx
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp doubleFaultHandlerCommon
    );
}

pub export fn syscallHandlerStub() callconv(.naked) noreturn {
    asm volatile (
    // ring3 -> kernel entry: scratch 利用前に r10 を退避し、完全保存を維持する。
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\push %rax
        \\push %rbx
        \\push %rcx
        \\push %rdx
        \\push %rsi
        \\push %rdi
        \\push %rbp
        \\push %r8
        \\push %r9
        \\push %r10
        \\push %r11
        \\push %r12
        \\push %r13
        \\push %r14
        \\push %r15
        // Win64: 32-byte shadow space を確保しつつ call 前 16-byte alignment を維持する。
        \\sub $32, %rsp
        \\lea 32(%rsp), %rdi
        \\mov %rdi, %rcx
        \\call syscallDispatch
        \\add $32, %rsp
        // TrapFrame.rax へ戻り値を書き戻す（offsetは comptime で検証済み）
        \\mov %rax, 112(%rsp)
        \\pop %r15
        \\pop %r14
        \\pop %r13
        \\pop %r12
        \\pop %r11
        \\pop %r10
        \\pop %r9
        \\pop %r8
        \\pop %rbp
        \\pop %rdi
        \\pop %rsi
        \\pop %rdx
        \\pop %rcx
        \\pop %rbx
        \\pop %rax
        // kernel -> ring3 return: r10 を壊さず user CR3 を復帰して iretq する。
        \\push %r10
        \\mov user_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\iretq
    );
}

pub export fn timerInterruptHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\push %rax
        \\push %rbx
        \\push %rcx
        \\push %rdx
        \\push %rsi
        \\push %rdi
        \\push %rbp
        \\push %r8
        \\push %r9
        \\push %r10
        \\push %r11
        \\push %r12
        \\push %r13
        \\push %r14
        \\push %r15
        \\sub $32, %rsp
        \\lea 32(%rsp), %rdi
        \\mov %rdi, %rcx
        \\call timerInterruptDispatch
        \\add $32, %rsp
        \\pop %r15
        \\pop %r14
        \\pop %r13
        \\pop %r12
        \\pop %r11
        \\pop %r10
        \\pop %r9
        \\pop %r8
        \\pop %rbp
        \\pop %rdi
        \\pop %rsi
        \\pop %rdx
        \\pop %rcx
        \\pop %rbx
        \\pop %rax
        \\push %r10
        \\mov user_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\iretq
    );
}

pub export fn generalProtectionHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\push %rax
        \\push %rbx
        \\push %rcx
        \\push %rdx
        \\push %rsi
        \\push %rdi
        \\push %rbp
        \\push %r8
        \\push %r9
        \\push %r10
        \\push %r11
        \\push %r12
        \\push %r13
        \\push %r14
        \\push %r15
        \\mov %rsp, %r8
        \\and $-16, %rsp
        \\sub $32, %rsp
        \\mov $13, %rcx
        \\mov %r8, %rdx
        \\call exceptionWithErrorCommon
        \\ud2
    );
}

pub export fn invalidTssHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\mov (%rsp), %rdi
        \\mov %rdi, %rcx
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp invalidTssHandlerCommon
    );
}

pub export fn segmentNotPresentHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\mov (%rsp), %rdi
        \\mov %rdi, %rcx
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp segmentNotPresentHandlerCommon
    );
}

pub export fn stackSegmentFaultHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\mov (%rsp), %rdi
        \\mov %rdi, %rcx
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp stackSegmentFaultHandlerCommon
    );
}

pub export fn invalidOpcodeHandlerStub() callconv(.naked) noreturn {
    asm volatile (
        \\push %r10
        \\mov kernel_cr3_value(%rip), %r10
        \\mov %r10, %cr3
        \\pop %r10
        \\and $-16, %rsp
        \\sub $8, %rsp
        \\jmp invalidOpcodeHandlerCommon
    );
}

fn loadGdtAndReloadSegments() void {
    const tss_base = @intFromPtr(&tss);
    const tss_limit: u64 = @sizeOf(Tss) - 1;
    tss.rsp0 = @intFromPtr(&ring0_stack) + ring0_stack.len;
    tss.iomap_base = @sizeOf(Tss);
    gdt[5] =
        (tss_limit & 0xFFFF) |
        ((tss_base & 0x00FF_FFFF) << 16) |
        (@as(u64, 0x89) << 40) |
        (((tss_limit >> 16) & 0xF) << 48) |
        (((tss_base >> 24) & 0xFF) << 56);
    gdt[6] = (tss_base >> 32) & 0xFFFF_FFFF;

    const gdt_ptr = GdtPtr{
        .limit = @as(u16, @intCast(@sizeOf(@TypeOf(gdt)) - 1)),
        .base = @intFromPtr(&gdt),
    };
    asm volatile ("lgdt (%[ptr])"
        :
        : [ptr] "r" (&gdt_ptr),
        : .{ .memory = true });

    // CS/SS/DS を新しい GDT の selector へ揃える。
    asm volatile (
        \\pushq %[kcs]
        \\pushq $1f
        \\lretq
        \\1:
        \\mov %[kds], %%ax
        \\mov %%ax, %%ds
        \\mov %%ax, %%es
        \\mov %%ax, %%ss
        :
        : [kcs] "i" (@as(u64, gdt_kernel_code_selector)),
          [kds] "i" (gdt_kernel_data_selector),
        : .{ .memory = true });
    asm volatile (
        \\mov %[tss_sel], %%ax
        \\ltr %%ax
        :
        : [tss_sel] "i" (gdt_tss_selector),
        : .{ .memory = true });
}

fn initIdtPageFaultOnly() void {
    interrupts.clearIdt(&idt);
    interrupts.setIdtEntry(&idt, 6, gdt_kernel_code_selector, ud_trampoline_entry, 0x8E); // #UD
    interrupts.setIdtEntry(&idt, 10, gdt_kernel_code_selector, ts_trampoline_entry, 0x8E); // #TS
    interrupts.setIdtEntry(&idt, 11, gdt_kernel_code_selector, np_trampoline_entry, 0x8E); // #NP
    interrupts.setIdtEntry(&idt, 12, gdt_kernel_code_selector, ss_trampoline_entry, 0x8E); // #SS
    interrupts.setIdtEntry(&idt, 13, gdt_kernel_code_selector, gp_trampoline_entry, 0x8E); // #GP
    interrupts.setIdtEntry(&idt, 14, gdt_kernel_code_selector, pf_trampoline_entry, 0x8E); // #PF
    interrupts.setIdtEntry(&idt, 8, gdt_kernel_code_selector, df_trampoline_entry, 0x8E); // #DF
    interrupts.setIdtEntry(&idt, lapic_timer_vector, gdt_kernel_code_selector, timer_trampoline_entry, 0x8E); // LAPIC timer
    // DPL=3 interrupt gate: syscall 中の割り込みネストを抑止する。
    interrupts.setIdtEntry(&idt, 0x80, gdt_kernel_code_selector, int80_trampoline_entry, 0xEE);
    interrupts.loadIdt(&idt);
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
    appendBootLog(s);
}

fn printHex(value: u64) void {
    serial.printHex(value);
    var buf: [18]u8 = undefined;
    const s = std.fmt.bufPrint(buf[0..], "0x{x}", .{value}) catch return;
    appendBootLog(s);
}

fn logRuntimeFramebufferSummaryBeforeSendCap() void {
    if (runtime_framebuffer_log_before_send_cap_done) return;
    const info = runtime_framebuffer_info orelse return;
    runtime_framebuffer_log_before_send_cap_done = true;

    serialWriteFmt(
        "framebuffer before send_cap\n  fb_paddr=0x{x}\n  fb_size={} bytes\n  fb_resolution={}x{}\n  fb_pitch={}\n",
        .{ info.paddr, info.size_bytes, info.width, info.height, info.pixels_per_scan_line },
    );
}

const ExitBootResult = enum {
    success,
    failed,
};

fn exitBootServicesWithRetry() ExitBootResult {
    var mmap_buffer: [64 * 1024]u8 align(@alignOf(uefi.tables.MemoryDescriptor)) = undefined;

    const st = uefi.system_table;
    const bs = st.boot_services orelse return .failed;

    var attempt: usize = 0;
    while (attempt < 8) : (attempt += 1) {
        const mmap = bs.getMemoryMap(mmap_buffer[0..]) catch return .failed;
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
    asm volatile ("mov %[value], %%cr3"
        :
        : [value] "r" (value),
        : .{ .memory = true });
}

fn readCr3() u64 {
    var value: u64 = 0;
    asm volatile ("mov %%cr3, %[out]"
        : [out] "=r" (value),
    );
    return value;
}

fn invlpg(addr: u64) void {
    asm volatile ("invlpg (%[addr])"
        :
        : [addr] "r" (addr),
        : .{ .memory = true });
}

fn flushTlbForCr3Va(target_cr3: u64, va: u64) void {
    if (target_cr3 == 0) return;
    const current_cr3 = readCr3();
    if (current_cr3 == target_cr3) {
        invlpg(va);
        return;
    }

    writeCr3(target_cr3);
    invlpg(va);
    writeCr3(current_cr3);
}

fn flushUserTlbForPrincipalVa(principal: kernel.PrincipalId, va: u64) void {
    // This kernel always returns to user mode via iretq path that reloads CR3
    // (kernel -> user address space), so user-TLB entries are naturally refreshed
    // at the next user resume. Avoid expensive remote-CR3 switches here.
    _ = principal;
    _ = va;
}

fn installIdentityPageTables0To1GiB() bool {
    @memset(pml4_table[0..], 0);
    @memset(pdp_table[0..], 0);
    var pd_idx: usize = 0;
    while (pd_idx < pd_table_count) : (pd_idx += 1) {
        @memset(pd_tables[pd_idx][0..], 0);
    }
    @memset(high_mmio_pdp_table[0..], 0);
    pd_idx = 0;
    while (pd_idx < high_mmio_pdp_table_count) : (pd_idx += 1) {
        @memset(high_mmio_pd_tables[pd_idx][0..], 0);
    }

    const pml4_pa: u64 = @intFromPtr(&pml4_table);
    const pdp_pa: u64 = @intFromPtr(&pdp_table);
    const pd0_pa: u64 = @intFromPtr(&pd_tables[0]);
    const high_pdp_pa: u64 = @intFromPtr(&high_mmio_pdp_table);
    const high_pd0_pa: u64 = @intFromPtr(&high_mmio_pd_tables[0]);

    // この段階では 0..4GiB を identity map する。テーブル実体も 4GiB 未満前提。
    if (pml4_pa >= four_gib or pdp_pa >= four_gib or pd0_pa >= four_gib or high_pdp_pa >= four_gib or high_pd0_pa >= four_gib) return false;

    const kernel_table_flags = page_present | page_rw;
    const kernel_large_page_flags = page_present | page_rw | page_ps;

    pml4_table[0] = pdp_pa | kernel_table_flags;
    pd_idx = 0;
    while (pd_idx < pd_table_count) : (pd_idx += 1) {
        const pd_pa: u64 = @intFromPtr(&pd_tables[pd_idx]);
        pdp_table[pd_idx] = pd_pa | kernel_table_flags;

        var i: usize = 0;
        while (i < page_entries) : (i += 1) {
            const absolute_entry = (pd_idx * page_entries) + i;
            const base = @as(u64, absolute_entry) * two_mib;
            pd_tables[pd_idx][i] = base | kernel_large_page_flags;
        }
    }
    pml4_table[high_mmio_pml4_index] = high_pdp_pa | kernel_table_flags;
    var high_pdp_idx: usize = 0;
    while (high_pdp_idx < high_mmio_pdp_table_count) : (high_pdp_idx += 1) {
        const high_pd_pa: u64 = @intFromPtr(&high_mmio_pd_tables[high_pdp_idx]);
        high_mmio_pdp_table[high_pdp_idx] = high_pd_pa | kernel_table_flags;

        const region_base = (@as(u64, @intCast(high_mmio_pml4_index)) << 39) + (@as(u64, @intCast(high_pdp_idx)) << 30);
        var i: usize = 0;
        while (i < page_entries) : (i += 1) {
            const base = region_base + (@as(u64, @intCast(i)) * two_mib);
            high_mmio_pd_tables[high_pdp_idx][i] = base | kernel_large_page_flags;
        }
    }

    writeCr3(pml4_pa);
    kernel_cr3_value = pml4_pa;
    return true;
}

fn hardenKernelMappingsSupervisorOnly() void {
    // kernel の既存 map は ring3 から見えないよう User ビットを強制的に落とす。
    pml4_table[0] &= ~page_user;
    pml4_table[high_mmio_pml4_index] &= ~page_user;

    var pdp_idx: usize = 0;
    while (pdp_idx < pd_table_count) : (pdp_idx += 1) {
        pdp_table[pdp_idx] &= ~page_user;

        var pd_idx: usize = 0;
        while (pd_idx < page_entries) : (pd_idx += 1) {
            pd_tables[pdp_idx][pd_idx] &= ~page_user;
        }
    }
    var high_pdp_idx: usize = 0;
    while (high_pdp_idx < high_mmio_pdp_table_count) : (high_pdp_idx += 1) {
        high_mmio_pdp_table[high_pdp_idx] &= ~page_user;
        var pd_idx2: usize = 0;
        while (pd_idx2 < page_entries) : (pd_idx2 += 1) {
            high_mmio_pd_tables[high_pdp_idx][pd_idx2] &= ~page_user;
        }
    }
}

fn buildUserAddressSpace(principal: kernel.PrincipalId, user_page_paddr: u64, user_stack_paddr: u64) bool {
    const space = getUserSpace(principal) orelse return false;
    @memset(space.pml4[0..], 0);
    @memset(space.pdp[0..], 0);
    @memset(space.pd[0..], 0);
    var pt_slot_init: usize = 0;
    while (pt_slot_init < UserAddressSpace.max_dynamic_pt_pages) : (pt_slot_init += 1) {
        space.pt_page_pd_index[pt_slot_init] = UserAddressSpace.no_pd_index;
    }
    space.pt_page_used_len = 0;

    const user_pml4_pa: u64 = @intFromPtr(&space.pml4);
    const user_pdp_pa: u64 = @intFromPtr(&space.pdp);
    const user_pd_pa: u64 = @intFromPtr(&space.pd);
    if (user_pml4_pa >= four_gib or user_pdp_pa >= four_gib or user_pd_pa >= four_gib) return false;
    if (user_page_paddr >= four_gib or user_stack_paddr >= four_gib) return false;

    const pdp_index: usize = @intCast((user_va >> 30) & 0x1FF);
    const pd_index_base: usize = @intCast((user_va >> 21) & 0x1FF);
    const user_pt_index: usize = @intCast((user_va >> 12) & 0x1FF);
    const stack_pt_index: usize = @intCast((user_stack_page_va >> 12) & 0x1FF);
    const stack_pd_index: usize = @intCast((user_stack_page_va >> 21) & 0x1FF);
    if (stack_pd_index != pd_index_base) return false;

    // user CR3 は最小構成: user mapping + 例外/割り込み入口に必要な supervisor bridge のみ。
    space.pml4[0] = user_pdp_pa | page_present | page_rw | page_user;

    space.pdp[pdp_index] = user_pd_pa | page_present | page_rw | page_user;
    // Start from kernel supervisor identity mapping for the whole PDP(0) range.
    // User-mapped regions override specific PD entries with PT pages on demand.
    var pd_idx_copy: usize = 0;
    while (pd_idx_copy < page_entries) : (pd_idx_copy += 1) {
        space.pd[pd_idx_copy] = pd_tables[0][pd_idx_copy] & ~page_user;
    }
    const user_slot = ensureUserPtSlotForPd(space, pd_index_base) orelse return false;
    space.pt_pages[user_slot][user_pt_index] = user_page_paddr | page_present | page_rw | page_user;
    space.pt_pages[user_slot][stack_pt_index] = user_stack_paddr | page_present | page_rw | page_user;

    space.cr3 = user_pml4_pa;
    return true;
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
    return buildUserAddressSpace(principal, user_cap.paddr, stack_cap.paddr);
}

fn createUserProcess(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    thread_index: usize,
    role_label: []const u8,
) CreatedUserProcess {
    const user_page = state.allocPageTo(principal, &global_free_list) catch |err| {
        serialWrite("allocPageTo for ");
        serialWrite(role_label);
        serialWrite(" user map failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    const user_stack_page = state.allocPageTo(principal, &global_free_list) catch |err| {
        serialWrite("allocPageTo for ");
        serialWrite(role_label);
        serialWrite(" user stack failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    if (!buildUserAddressSpaceFromCapabilities(state, principal, user_page, user_stack_page)) {
        serialWrite(role_label);
        serialWrite(" process page table build failed\n");
        while (true) asm volatile ("hlt");
    }
    if (!initThreadContext(thread_index, principal)) {
        serialWrite(role_label);
        serialWrite(" thread context init failed\n");
        while (true) asm volatile ("hlt");
    }
    return .{
        .user_page = user_page,
        .user_stack_page = user_stack_page,
    };
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
    if (!mapUserLinearRegion(principal, va_start, paddr_start, size_bytes, writable)) {
        serialWrite(what);
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    }
}

fn setupMouseDriverProcess(state: *kernel.KernelState, cfg: MouseDriverConfig) MouseDriverProcessSetup {
    const process = createUserProcess(state, .Process0, 0, "mouse driver");
    const runtime_stack_page = allocPageForProcessOrHalt(state, .Process0, "mouse driver", "runtime stack");
    const config_page = allocPageForProcessOrHalt(state, .Process0, "mouse driver", "config page");
    const shared_page = allocPageForProcessOrHalt(state, .Process0, "mouse driver", "shared page");
    const virtual_framebuffer_page = allocPageForProcessOrHalt(state, .Process0, "virtual framebuffer", "page");

    mapUserLinearRegionOrHalt(.Process0, mouse_driver_config_va, config_page.paddr, 4096, true, "mouse driver config page map failed");
    mapUserLinearRegionOrHalt(.Process0, mouse_shared_driver_va, shared_page.paddr, 4096, true, "mouse shared page map failed (Process0)");
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
    publishMouseDriverConfigPage(config_page.paddr, cfg, shared_page.paddr);

    return .{
        .process = process,
        .runtime_stack_page = runtime_stack_page,
        .config_page = config_page,
        .shared_page = shared_page,
        .virtual_framebuffer_page = virtual_framebuffer_page,
    };
}

fn setupKeyboardDriverProcess(state: *kernel.KernelState, cfg: MouseDriverConfig) KeyboardDriverProcessSetup {
    const process = createUserProcess(state, .Process3, 3, "keyboard driver");
    const config_page = allocPageForProcessOrHalt(state, .Process3, "keyboard driver", "config page");
    mapUserLinearRegionOrHalt(.Process3, keyboard_driver_config_va, config_page.paddr, 4096, true, "keyboard driver config page map failed");

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
    publishKeyboardDriverConfigPage(config_page.paddr, cfg);

    return .{
        .process = process,
        .config_page = config_page,
    };
}

fn setupBootLogConsoleProcess(state: *kernel.KernelState) BootLogConsoleProcessSetup {
    const process = createUserProcess(state, .Process1, 1, "boot log console");
    const boot_log_page = allocPageForProcessOrHalt(state, .Process1, "boot log", "page");
    const boot_log_stack_page = allocPageForProcessOrHalt(state, .Process1, "boot log console", "stack page");
    return .{
        .process = process,
        .boot_log_page = boot_log_page,
        .boot_log_stack_page = boot_log_stack_page,
    };
}

fn setupBootLogSenderProcess(
    state: *kernel.KernelState,
    shared_page: kernel.PageCapability,
) BootLogSenderProcessSetup {
    const process = createUserProcess(state, .Process2, 2, "bootlog sender");
    state.installCap(.Process2, shared_page.paddr, .{
        .cpu_read = true,
        .cpu_write = false,
        .dma = false,
    }) catch |err| {
        serialWrite("mouse shared page cap install for Process2 failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    mapUserLinearRegionOrHalt(.Process2, mouse_shared_draw_va, shared_page.paddr, 4096, false, "mouse shared page map failed (Process2)");
    return .{ .process = process };
}

fn setupVirtualFramebufferSharing(state: *kernel.KernelState, vfb_page: kernel.PageCapability) void {
    state.grantCap(.Process0, .Process1, vfb_page.paddr, .{
        .cpu_read = true,
        .cpu_write = true,
        .dma = false,
    }) catch |err| {
        serialWrite("virtual framebuffer cap grant to Process1 failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };

    const vfb_cap = kernel.VirtualFramebufferCapability{
        .paddr = vfb_page.paddr,
        .size_bytes = 4096,
        .width = virtual_framebuffer_width,
        .height = virtual_framebuffer_height,
        .pixels_per_scan_line = virtual_framebuffer_pitch,
        .pixel_format = 0,
        .allow_read = true,
        .allow_write = true,
    };
    state.grantVirtualFramebufferCap(.Process0, vfb_cap) catch |err| {
        serialWrite("virtual framebuffer cap grant to Process0 failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    state.grantVirtualFramebufferCap(.Process1, vfb_cap) catch |err| {
        serialWrite("virtual framebuffer cap grant to Process1 failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };

    if (!state.canAccessVirtualFramebuffer(.Process0, vfb_page.paddr, 4096, true)) {
        serialWrite("virtual framebuffer cap check failed (Process0)\n");
        while (true) asm volatile ("hlt");
    }
    if (!state.canAccessVirtualFramebuffer(.Process1, vfb_page.paddr, 4096, true)) {
        serialWrite("virtual framebuffer cap check failed (Process1)\n");
        while (true) asm volatile ("hlt");
    }

    mapUserLinearRegionOrHalt(.Process0, virtual_framebuffer_app_va, vfb_page.paddr, 4096, true, "virtual framebuffer map failed (Process0)");
    mapUserLinearRegionOrHalt(.Process1, virtual_framebuffer_compositor_va, vfb_page.paddr, 4096, true, "virtual framebuffer map failed (Process1)");

    const vfb_words: [*]volatile u64 = @ptrFromInt(vfb_page.paddr);
    var w: usize = 0;
    while (w < 512) : (w += 1) {
        vfb_words[w] = 0;
    }

    if (enable_title_only_ready_logs) {
        logReadyTitle("VIRTUAL_FRAMEBUFFER_READY");
    } else {
        serialWrite("virtual framebuffer capability ready\n");
        serialWrite("  vfb_paddr=");
        printHex(vfb_page.paddr);
        serialWrite("\n");
        serialWrite("  app_va=");
        printHex(virtual_framebuffer_app_va);
        serialWrite("\n");
        serialWrite("  compositor_va=");
        printHex(virtual_framebuffer_compositor_va);
        serialWrite("\n");
    }
}

fn determineBootRuntimeMode(mouse_driver_cfg: ?MouseDriverConfig, keyboard_driver_cfg: ?MouseDriverConfig) BootRuntimeMode {
    if (!enable_framebuffer_server_step1) return .DiskUser;
    if (!enable_boot_log_console_process) return .FramebufferIpc;
    if (enable_virtio_input_mouse and mouse_driver_cfg != null) {
        if (enable_bootlog_wait_for_enter and keyboard_driver_cfg != null) return .BootLogGateCompositor;
        return .MouseCompositor;
    }
    return .BootLogConsole;
}

fn activateThreadOrHalt(thread_index: usize) void {
    if (!activateThread(thread_index)) {
        serialWrite("activate Thread");
        printNumber(thread_index);
        serialWrite(" failed\n");
        while (true) asm volatile ("hlt");
    }
}

fn armBootLogGateDeferredInputStart(start_thread0: bool, start_thread3: bool) void {
    bootlog_gate_input_start_armed = false;
    bootlog_gate_start_thread0 = false;
    bootlog_gate_start_thread3 = false;

    if (start_thread0) {
        if (getThreadContext(0)) |ctx| {
            ctx.ready = false;
            bootlog_gate_start_thread0 = true;
        }
    }
    if (start_thread3) {
        if (getThreadContext(3)) |ctx| {
            ctx.ready = false;
            bootlog_gate_start_thread3 = true;
        }
    }
    if (!bootlog_gate_start_thread0 and !bootlog_gate_start_thread3) return;

    bootlog_gate_input_start_tick = lapic_tick_count + bootlog_gate_input_start_delay_ticks;
    bootlog_gate_input_start_armed = true;
}

fn tryStartBootLogGateDeferredInput() void {
    if (!bootlog_gate_input_start_armed) return;
    if (lapic_tick_count < bootlog_gate_input_start_tick) return;

    if (bootlog_gate_start_thread0) {
        if (getThreadContext(0)) |ctx| ctx.ready = true;
    }
    if (bootlog_gate_start_thread3) {
        if (getThreadContext(3)) |ctx| ctx.ready = true;
    }
    bootlog_gate_input_start_armed = false;
    bootlog_gate_start_thread0 = false;
    bootlog_gate_start_thread3 = false;
}

fn setupUserProcessesForMode(
    state: *kernel.KernelState,
    mode: BootRuntimeMode,
    mouse_driver_cfg: ?MouseDriverConfig,
    keyboard_driver_cfg: ?MouseDriverConfig,
) UserBootProcessSetup {
    var result = UserBootProcessSetup{};
    switch (mode) {
        .DiskUser => {
            const user_process = createUserProcess(state, .Process0, 0, "user");
            result.process0_user_page = user_process.user_page;
            result.process0_user_stack_page = user_process.user_stack_page;
            activateThreadOrHalt(0);
        },
        .FramebufferIpc => {
            const draw_client_process = createUserProcess(state, .Process0, 0, "draw client");
            result.process0_user_page = draw_client_process.user_page;
            result.process0_user_stack_page = draw_client_process.user_stack_page;

            const framebuffer_process = createUserProcess(state, .Process1, 1, "framebuffer");
            result.framebuffer_server_user_page = framebuffer_process.user_page;
            result.framebuffer_server_user_stack_page = framebuffer_process.user_stack_page;
            activateThreadOrHalt(1);
        },
        .BootLogConsole => {
            const boot_log_console_setup = setupBootLogConsoleProcess(state);
            result.framebuffer_server_user_page = boot_log_console_setup.process.user_page;
            result.framebuffer_server_user_stack_page = boot_log_console_setup.process.user_stack_page;
            result.boot_log_console_page = boot_log_console_setup.boot_log_page;
            result.boot_log_console_stack_page = boot_log_console_setup.boot_log_stack_page;
            activateThreadOrHalt(1);
            if (keyboard_driver_cfg) |cfg| {
                const keyboard_driver_setup = setupKeyboardDriverProcess(state, cfg);
                result.process3_user_page = keyboard_driver_setup.process.user_page;
                result.process3_user_stack_page = keyboard_driver_setup.process.user_stack_page;
                result.keyboard_driver_config_page = keyboard_driver_setup.config_page;
                activateThreadOrHalt(3);
            }
        },
        .BootLogGateCompositor => {
            const mouse_driver_setup = setupMouseDriverProcess(state, mouse_driver_cfg.?);
            result.process0_user_page = mouse_driver_setup.process.user_page;
            result.process0_user_stack_page = mouse_driver_setup.process.user_stack_page;
            result.mouse_driver_runtime_stack_page = mouse_driver_setup.runtime_stack_page;
            result.mouse_driver_config_page = mouse_driver_setup.config_page;
            result.mouse_shared_page = mouse_driver_setup.shared_page;
            result.virtual_framebuffer_page = mouse_driver_setup.virtual_framebuffer_page;

            const boot_log_console_setup = setupBootLogConsoleProcess(state);
            result.framebuffer_server_user_page = boot_log_console_setup.process.user_page;
            result.framebuffer_server_user_stack_page = boot_log_console_setup.process.user_stack_page;
            result.boot_log_console_page = boot_log_console_setup.boot_log_page;
            result.boot_log_console_stack_page = boot_log_console_setup.boot_log_stack_page;

            setupVirtualFramebufferSharing(state, result.virtual_framebuffer_page.?);
            if (keyboard_driver_cfg) |cfg| {
                const keyboard_driver_setup = setupKeyboardDriverProcess(state, cfg);
                result.process3_user_page = keyboard_driver_setup.process.user_page;
                result.process3_user_stack_page = keyboard_driver_setup.process.user_stack_page;
                result.keyboard_driver_config_page = keyboard_driver_setup.config_page;
            }
            activateThreadOrHalt(1);
        },
        .MouseCompositor => {
            const mouse_driver_setup = setupMouseDriverProcess(state, mouse_driver_cfg.?);
            result.process0_user_page = mouse_driver_setup.process.user_page;
            result.process0_user_stack_page = mouse_driver_setup.process.user_stack_page;
            result.mouse_driver_runtime_stack_page = mouse_driver_setup.runtime_stack_page;
            result.mouse_driver_config_page = mouse_driver_setup.config_page;
            result.mouse_shared_page = mouse_driver_setup.shared_page;
            result.virtual_framebuffer_page = mouse_driver_setup.virtual_framebuffer_page;

            const boot_log_console_setup = setupBootLogConsoleProcess(state);
            result.framebuffer_server_user_page = boot_log_console_setup.process.user_page;
            result.framebuffer_server_user_stack_page = boot_log_console_setup.process.user_stack_page;
            result.boot_log_console_page = boot_log_console_setup.boot_log_page;
            result.boot_log_console_stack_page = boot_log_console_setup.boot_log_stack_page;

            setupVirtualFramebufferSharing(state, result.virtual_framebuffer_page.?);
            const bootlog_sender_setup = setupBootLogSenderProcess(state, result.mouse_shared_page.?);
            result.process2_user_page = bootlog_sender_setup.process.user_page;
            result.process2_user_stack_page = bootlog_sender_setup.process.user_stack_page;
            if (keyboard_driver_cfg) |cfg| {
                const keyboard_driver_setup = setupKeyboardDriverProcess(state, cfg);
                result.process3_user_page = keyboard_driver_setup.process.user_page;
                result.process3_user_stack_page = keyboard_driver_setup.process.user_stack_page;
                result.keyboard_driver_config_page = keyboard_driver_setup.config_page;
                activateThreadOrHalt(3);
            }
            activateThreadOrHalt(0);
        },
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

fn loadElfFromDisk(
    bs: *uefi.tables.BootServices,
    path: [*:0]const u16,
    staging: []u8,
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
    if (info.file_size == 0 or info.file_size > staging.len) return null;

    const read_len: usize = @intCast(info.file_size);
    const read_bytes = user_file.read(staging[0..read_len]) catch return null;
    if (read_bytes != read_len) return null;
    return staging[0..read_len];
}

fn loadUserElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, user_elf_disk_path, user_elf_staging[0..]);
}

fn loadFramebufferServerElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, framebuffer_server_elf_disk_path, framebuffer_server_elf_staging[0..]);
}

fn loadDrawClientElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, draw_client_elf_disk_path, draw_client_elf_staging[0..]);
}

fn loadBootLogConsoleElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, boot_log_console_elf_disk_path, boot_log_console_elf_staging[0..]);
}

fn loadMouseDriverElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, mouse_driver_elf_disk_path, mouse_driver_elf_staging[0..]);
}

fn loadKeyboardDriverElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, keyboard_driver_elf_disk_path, keyboard_driver_elf_staging[0..]);
}

fn loadBootLogSenderElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, bootlog_sender_elf_disk_path, bootlog_sender_elf_staging[0..]);
}

fn loadCompositorElfFromDisk(bs: *uefi.tables.BootServices) ?[]const u8 {
    return loadElfFromDisk(bs, compositor_elf_disk_path, compositor_elf_staging[0..]);
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

fn publishBootLogToUserPage(user_page_paddr: u64) void {
    const page: [*]volatile u8 = @ptrFromInt(user_page_paddr);
    var zero_i: usize = 0;
    while (zero_i < 4096) : (zero_i += 1) {
        page[zero_i] = 0;
    }

    const copy_len: usize = if (boot_log_len > boot_log_page_payload_bytes) boot_log_page_payload_bytes else boot_log_len;
    const start_index: usize = if (boot_log_len > copy_len) boot_log_len - copy_len else 0;
    const len_u32: u32 = @intCast(copy_len);
    page[0] = @intCast(len_u32 & 0xFF);
    page[1] = @intCast((len_u32 >> 8) & 0xFF);
    page[2] = @intCast((len_u32 >> 16) & 0xFF);
    page[3] = @intCast((len_u32 >> 24) & 0xFF);
    writeBootLogStatusToUserPage(user_page_paddr);
    var i: usize = 0;
    while (i < copy_len) : (i += 1) {
        page[boot_log_page_header_bytes + i] = boot_log_buffer[start_index + i];
    }
}

fn mmioPageWithOffset(addr: u64) MmioPageWithOffset {
    if (addr == 0) {
        return .{ .page_paddr = 0, .page_offset = 0 };
    }
    return .{
        .page_paddr = pageAlignDown(addr),
        .page_offset = addr & 0xFFF,
    };
}

fn publishMouseDriverConfigPage(user_page_paddr: u64, cfg: MouseDriverConfig, shared_page_paddr: u64) void {
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
}

fn publishKeyboardDriverConfigPage(user_page_paddr: u64, cfg: MouseDriverConfig) void {
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
    const text_len: usize = if (boot_log_len > mouse_shared_log_max_bytes) mouse_shared_log_max_bytes else boot_log_len;
    words[9] = @intCast(text_len);
    var j: usize = 0;
    while (j < text_len) : (j += 1) {
        bytes[mouse_shared_header_bytes + j] = boot_log_buffer[j];
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
    const aligned_end = pageAlignUp(max_end);
    if (aligned_end > std.math.maxInt(usize)) return null;
    return @intCast(aligned_end);
}

fn loadUserElfIntoProcessPages(
    state: *kernel.KernelState,
    principal: kernel.PrincipalId,
    page0_paddr: u64,
    page1_paddr: u64,
    image_bytes: []const u8,
) ?elf_loader.Image {
    if ((page0_paddr & 0xFFF) != 0 or (page1_paddr & 0xFFF) != 0) return null;

    const required_bytes = computeUserElfRequiredBytes(image_bytes) orelse return null;
    if (required_bytes > user_program_max_load_bytes) return null;
    if (required_bytes > user_elf_load_window.len) return null;

    @memset(user_elf_load_window[0..required_bytes], 0);
    const loaded = elf_loader.loadToSinglePage(image_bytes, user_elf_base_va, user_elf_load_window[0..required_bytes]) catch return null;

    const required_pages = required_bytes / 4096;
    const page0: [*]u8 = @ptrFromInt(page0_paddr);
    @memcpy(page0[0..4096], user_elf_load_window[0..4096]);
    if (required_pages >= 2) {
        const page1: [*]u8 = @ptrFromInt(page1_paddr);
        @memcpy(page1[0..4096], user_elf_load_window[4096..8192]);
    }

    var page_index: usize = 2;
    while (page_index < required_pages) : (page_index += 1) {
        const extra_page = state.allocPageTo(principal, &global_free_list) catch return null;
        const map_va = user_va + (@as(u64, @intCast(page_index)) * 4096);
        if (!mapUserLinearRegion(principal, map_va, extra_page.paddr, 4096, true)) return null;
        const page_bytes: [*]u8 = @ptrFromInt(extra_page.paddr);
        const off = page_index * 4096;
        @memcpy(page_bytes[0..4096], user_elf_load_window[off .. off + 4096]);
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

fn armDeferredCompositorLaunch(image: []const u8, process1_user_page_paddr: u64, process1_user_stack_paddr: u64) void {
    deferred_compositor_image = image;
    deferred_compositor_user_page_paddr = process1_user_page_paddr;
    deferred_compositor_user_stack_page_paddr = process1_user_stack_paddr;
    deferred_compositor_launch_armed = true;
    deferred_compositor_launched = false;
}

fn containsBytes(haystack: []const u8, needle: []const u8) bool {
    return std.mem.indexOf(u8, haystack, needle) != null;
}

fn isKeyboardEnterPressLog(message: []const u8) bool {
    // KEY_ENTER=0x1c, KEY_KPENTER=0x60
    return containsBytes(message, "key code=0x1c value=1") or
        containsBytes(message, "key code=0x60 value=1");
}

fn tryLaunchDeferredCompositorFromLog(frame: *TrapFrame, proc: kernel.PrincipalId, message: []const u8) void {
    if (!deferred_compositor_launch_armed or deferred_compositor_launched) return;
    if (proc != .Process3) return;
    if (!isKeyboardEnterPressLog(message)) return;
    const image = deferred_compositor_image orelse return;

    const loaded = loadUserElfIntoProcessPages(
        &kernel_state_global,
        .Process1,
        deferred_compositor_user_page_paddr,
        deferred_compositor_user_stack_page_paddr,
        image,
    ) orelse {
        serialWrite("deferred compositor launch failed: ELF load\n");
        return;
    };
    setThreadEntry(1, loaded.entry, user_entry_rsp);
    deferred_compositor_launched = true;
    deferred_compositor_launch_armed = false;
    serialWrite("deferred compositor launch: Enter pressed\n");
    logElfLoadSummary("Compositor ELF remapped", loaded);

    if (current_thread_index != 1) {
        if (switchToThread(1, frame, syscall_ok)) {
            serialWrite("deferred compositor switch to Thread1\n");
        }
    }
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
    const kernel_transition_rsp: u64 = @intFromPtr(&ring0_stack) + ring0_stack.len;

    asm volatile (
        \\mov %[k_rsp], %%rsp
        \\pushq %[ss]
        \\pushq %[rsp]
        \\pushq %[rflags]
        \\pushq %[cs]
        \\pushq %[rip]
        \\mov %[ucr3], %%rax
        \\mov %%rax, %%cr3
        \\iretq
        :
        : [ss] "r" (user_ss),
          [rsp] "r" (user_rsp),
          [rflags] "r" (user_rflags),
          [cs] "r" (user_cs),
          [rip] "r" (user_entry_va),
          [k_rsp] "r" (kernel_transition_rsp),
          [ucr3] "r" (user_cr3_value),
        : .{ .memory = true });
    unreachable;
}

fn pageAlignDown(addr: u64) u64 {
    return addr & ~@as(u64, 4095);
}

fn pageAlignUp(addr: u64) u64 {
    return (addr + 4095) & ~@as(u64, 4095);
}

fn isReserved(paddr: u64, reserved: []const ReservedRange) bool {
    for (reserved) |r| {
        if (paddr >= r.start and paddr < r.end) return true;
    }
    return false;
}

fn collectMemoryStatsAndFreePages(
    bs: *uefi.tables.BootServices,
    free_list: *kernel.FreePageList,
) ?MemoryStats {
    var mmap_buffer: [64 * 1024]u8 align(@alignOf(uefi.tables.MemoryDescriptor)) = undefined;
    const mmap = bs.getMemoryMap(mmap_buffer[0..]) catch return null;

    free_list.* = .{};
    var detected_regions: usize = 0;
    var total_usable_bytes: u64 = 0;
    const pml4_start = pageAlignDown(@intFromPtr(&pml4_table));
    const pml4_end = pageAlignUp(@intFromPtr(&pml4_table) + @sizeOf(@TypeOf(pml4_table)));
    const pdp_start = pageAlignDown(@intFromPtr(&pdp_table));
    const pdp_end = pageAlignUp(@intFromPtr(&pdp_table) + @sizeOf(@TypeOf(pdp_table)));
    const pd_start = pageAlignDown(@intFromPtr(&pd_tables));
    const pd_end = pageAlignUp(@intFromPtr(&pd_tables) + @sizeOf(@TypeOf(pd_tables)));
    const high_mmio_pdp_start = pageAlignDown(@intFromPtr(&high_mmio_pdp_table));
    const high_mmio_pdp_end = pageAlignUp(@intFromPtr(&high_mmio_pdp_table) + @sizeOf(@TypeOf(high_mmio_pdp_table)));
    const high_mmio_pd_start = pageAlignDown(@intFromPtr(&high_mmio_pd_tables));
    const high_mmio_pd_end = pageAlignUp(@intFromPtr(&high_mmio_pd_tables) + @sizeOf(@TypeOf(high_mmio_pd_tables)));
    const user_spaces_start = pageAlignDown(@intFromPtr(&user_spaces));
    const user_spaces_end = pageAlignUp(@intFromPtr(&user_spaces) + @sizeOf(@TypeOf(user_spaces)));
    const thread_contexts_start = pageAlignDown(@intFromPtr(&thread_contexts));
    const thread_contexts_end = pageAlignUp(@intFromPtr(&thread_contexts) + @sizeOf(@TypeOf(thread_contexts)));
    const free_list_start = pageAlignDown(@intFromPtr(&global_free_list));
    const free_list_end = pageAlignUp(@intFromPtr(&global_free_list) + @sizeOf(@TypeOf(global_free_list)));
    const kernel_state_start = pageAlignDown(@intFromPtr(&kernel_state_global));
    const kernel_state_end = pageAlignUp(@intFromPtr(&kernel_state_global) + @sizeOf(@TypeOf(kernel_state_global)));
    const ring0_stack_start = pageAlignDown(@intFromPtr(&ring0_stack));
    const ring0_stack_end = pageAlignUp(@intFromPtr(&ring0_stack) + @sizeOf(@TypeOf(ring0_stack)));
    const gdt_start = pageAlignDown(@intFromPtr(&gdt));
    const gdt_end = pageAlignUp(@intFromPtr(&gdt) + @sizeOf(@TypeOf(gdt)));
    const idt_start = pageAlignDown(@intFromPtr(&idt));
    const idt_end = pageAlignUp(@intFromPtr(&idt) + @sizeOf(@TypeOf(idt)));
    const tss_start = pageAlignDown(@intFromPtr(&tss));
    const tss_end = pageAlignUp(@intFromPtr(&tss) + @sizeOf(@TypeOf(tss)));
    const tramp_start = pageAlignDown(@intFromPtr(&int80_trampoline_page));
    const tramp_end = pageAlignUp(@intFromPtr(&timer_trampoline_page) + @sizeOf(@TypeOf(timer_trampoline_page)));
    const mmap_start = pageAlignDown(@intFromPtr(&mmap_buffer));
    const mmap_end = pageAlignUp(@intFromPtr(&mmap_buffer) + mmap_buffer.len);
    // カーネル自身が使う最低限の領域は free list から除外する。
    const reserved = [_]ReservedRange{
        .{ .start = 0, .end = reserved_low_mem_end },
        .{ .start = pml4_start, .end = pml4_end },
        .{ .start = pdp_start, .end = pdp_end },
        .{ .start = pd_start, .end = pd_end },
        .{ .start = high_mmio_pdp_start, .end = high_mmio_pdp_end },
        .{ .start = high_mmio_pd_start, .end = high_mmio_pd_end },
        .{ .start = user_spaces_start, .end = user_spaces_end },
        .{ .start = thread_contexts_start, .end = thread_contexts_end },
        .{ .start = free_list_start, .end = free_list_end },
        .{ .start = kernel_state_start, .end = kernel_state_end },
        .{ .start = ring0_stack_start, .end = ring0_stack_end },
        .{ .start = gdt_start, .end = gdt_end },
        .{ .start = idt_start, .end = idt_end },
        .{ .start = tss_start, .end = tss_end },
        .{ .start = tramp_start, .end = tramp_end },
        .{ .start = mmap_start, .end = mmap_end },
    };

    var it = mmap.iterator();
    while (it.next()) |desc| {
        if (desc.type == .conventional_memory) {
            const region_id = detected_regions;
            var i: u64 = 0;
            while (i < desc.number_of_pages) : (i += 1) {
                const paddr = desc.physical_start + (i * 4096);
                if (isReserved(paddr, reserved[0..])) continue;
                free_list.appendPage(region_id, paddr) catch return null;
            }
            detected_regions += 1;
            total_usable_bytes += desc.number_of_pages * 4096;
        }
    }

    return .{
        .detected_regions = detected_regions,
        .total_usable_bytes = total_usable_bytes,
    };
}

pub fn main() void {
    serialInit();
    deferred_compositor_launch_armed = false;
    deferred_compositor_launched = false;
    deferred_compositor_user_page_paddr = 0;
    deferred_compositor_user_stack_page_paddr = 0;
    deferred_compositor_image = null;
    runtime_framebuffer_info = null;
    runtime_framebuffer_log_before_send_cap_done = false;
    boot_log_status_flags = 0;
    boot_log_status_page_paddr = 0;
    bootlog_gate_input_start_armed = false;
    bootlog_gate_start_thread0 = false;
    bootlog_gate_start_thread3 = false;
    serialWrite("[stage] boot entry\n");
    serialWrite("SakuraMicroKernel Phase1 boot\n");
    _ = gdt_user_code_selector;
    _ = gdt_user_data_selector;

    const bs = uefi.system_table.boot_services orelse {
        serialWrite("boot services missing\n");
        while (true) asm volatile ("hlt");
    };
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
    const disk_bootlog_sender_elf: ?[]const u8 = if (enable_framebuffer_server_step1 and enable_boot_log_console_process and enable_virtio_input_mouse)
        (loadBootLogSenderElfFromDisk(bs) orelse {
            serialWrite("disk bootlog sender ELF load failed\n");
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
    const disk_user_elf: ?[]const u8 = if (enable_framebuffer_server_step1)
        null
    else
        (loadUserElfFromDisk(bs) orelse {
            serialWrite("disk user ELF load failed\n");
            while (true) asm volatile ("hlt");
        });
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
                serialWrite("bootlog sender ELF loaded from disk\n");
                serialWrite("  path=");
                serialWrite(bootlog_sender_elf_disk_path_log);
                serialWrite("\n");
                serialWrite("  size=");
                printNumber(disk_bootlog_sender_elf.?.len);
                serialWrite(" bytes\n");
                serialWrite("compositor ELF loaded from disk\n");
                serialWrite("  path=");
                serialWrite(compositor_elf_disk_path_log);
                serialWrite("\n");
                serialWrite("  size=");
                printNumber(disk_compositor_elf.?.len);
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
    const memory_stats = collectMemoryStatsAndFreePages(bs, &global_free_list) orelse {
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
    loadGdtAndReloadSegments();
    serialWrite("GDT loaded (kernel/user segments)\n");

    if (debug_skip_cr3_switch) {
        serialWrite("[debug] skip CR3 switch\n");
    } else {
        serialWrite("build page tables (identity 0..");
        printNumber(pd_table_count);
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
    serialWrite("\x1b[96m========================================\n");
    serialWrite("  Sakura MicroKernel\n");
    serialWrite("\x1b[95m  enter bare-metal capability kernel\n");
    serialWrite("\x1b[96m========================================\x1b[0m\n");
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
        .flush_user_tlb_for_principal_va = flushUserTlbForPrincipalVa,
    });

    kernel_state_global = kernel.KernelState.initFromDetectedRegions(memory_stats.detected_regions) catch |err| {
        serialWrite("region init failed: ");
        serialWrite(@errorName(err));
        serialWrite("\n");
        while (true) asm volatile ("hlt");
    };
    const state = &kernel_state_global;
    state.pte_sync_hook = null;
    var process0_user_page: ?kernel.PageCapability = null;
    var process0_user_stack_page: ?kernel.PageCapability = null;
    var framebuffer_server_user_page: ?kernel.PageCapability = null;
    var framebuffer_server_user_stack_page: ?kernel.PageCapability = null;
    var process2_user_page: ?kernel.PageCapability = null;
    var process2_user_stack_page: ?kernel.PageCapability = null;
    var process3_user_page: ?kernel.PageCapability = null;
    var process3_user_stack_page: ?kernel.PageCapability = null;
    var boot_log_console_page: ?kernel.PageCapability = null;
    var boot_log_console_stack_page: ?kernel.PageCapability = null;
    var mouse_driver_runtime_stack_page: ?kernel.PageCapability = null;
    var mouse_driver_config_page: ?kernel.PageCapability = null;
    var keyboard_driver_config_page: ?kernel.PageCapability = null;
    var mouse_shared_page: ?kernel.PageCapability = null;
    var virtual_framebuffer_page: ?kernel.PageCapability = null;
    var mouse_modern_info: ?virtio_probe.InputModernInfo = null;
    var keyboard_modern_info: ?virtio_probe.InputModernInfo = null;
    var mouse_driver_cfg: ?MouseDriverConfig = null;
    var keyboard_driver_cfg: ?MouseDriverConfig = null;

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

    const boot_runtime_mode = determineBootRuntimeMode(mouse_driver_cfg, keyboard_driver_cfg);
    const process_setup = setupUserProcessesForMode(state, boot_runtime_mode, mouse_driver_cfg, keyboard_driver_cfg);
    process0_user_page = process_setup.process0_user_page;
    process0_user_stack_page = process_setup.process0_user_stack_page;
    framebuffer_server_user_page = process_setup.framebuffer_server_user_page;
    framebuffer_server_user_stack_page = process_setup.framebuffer_server_user_stack_page;
    process2_user_page = process_setup.process2_user_page;
    process2_user_stack_page = process_setup.process2_user_stack_page;
    process3_user_page = process_setup.process3_user_page;
    process3_user_stack_page = process_setup.process3_user_stack_page;
    boot_log_console_page = process_setup.boot_log_console_page;
    boot_log_console_stack_page = process_setup.boot_log_console_stack_page;
    mouse_driver_runtime_stack_page = process_setup.mouse_driver_runtime_stack_page;
    mouse_driver_config_page = process_setup.mouse_driver_config_page;
    keyboard_driver_config_page = process_setup.keyboard_driver_config_page;
    mouse_shared_page = process_setup.mouse_shared_page;
    virtual_framebuffer_page = process_setup.virtual_framebuffer_page;
    if (boot_log_console_page) |page| {
        boot_log_status_page_paddr = page.paddr;
    }
    scheduler_tick_accum = 0;
    scheduler_switch_count = 0;
    scheduler_int80_log_count = 0;
    scheduler_race_log_count = 0;
    if (enable_title_only_ready_logs) {
        logReadyTitle("USER_PAGE_READY");
    } else {
        serialWrite("user page table ready\n");
        serialWrite("  user_va=");
        printHex(user_va);
        serialWrite("\n");
        serialWrite("  user_pa=");
        switch (boot_runtime_mode) {
            .BootLogConsole, .BootLogGateCompositor => printHex(framebuffer_server_user_page.?.paddr),
            else => printHex(process0_user_page.?.paddr),
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
            else => printHex(process0_user_stack_page.?.paddr),
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
                    if (keyboard_driver_cfg != null and thread_contexts[3].ready) {
                        serialWrite("  keyboard_owner=Process3\n");
                    }
                } else if (boot_runtime_mode == .MouseCompositor) {
                    serialWrite("  compositor_owner=Process1\n");
                    if (thread_contexts[2].ready) serialWrite("  bootlog_sender_owner=Process2\n");
                    if (keyboard_driver_cfg != null and thread_contexts[3].ready) {
                        serialWrite("  keyboard_owner=Process3\n");
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
    switch (boot_runtime_mode) {
        .BootLogGateCompositor => {
            const info = framebuffer_info.?;
            const loaded_mouse = loadUserElfIntoProcessPagesOrHalt(
                state,
                .Process0,
                process0_user_page.?.paddr,
                process0_user_stack_page.?.paddr,
                disk_mouse_driver_elf.?,
                "mouse driver ELF load into user page failed\n",
            );
            setThreadEntry(0, loaded_mouse.entry, boot_log_console_entry_rsp);
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
                framebuffer_server_user_page.?.paddr,
                framebuffer_server_user_stack_page.?.paddr,
                disk_boot_log_console_elf.?,
                "boot log console ELF load into user page failed\n",
            );
            setThreadEntry(1, loaded_elf.?.entry, boot_log_console_entry_rsp);
            logElfLoadSummary("BootLogConsole ELF mapped", loaded_elf.?);
            if (!enable_title_only_ready_logs) {
                serialWrite("  log_va=");
                printHex(boot_log_user_va);
                serialWrite("\n");
                serialWrite("  log_bytes=");
                printNumber(if (boot_log_len > boot_log_page_payload_bytes) boot_log_page_payload_bytes else boot_log_len);
                serialWrite("\n");
            }

            if (process3_user_page != null and process3_user_stack_page != null and keyboard_driver_config_page != null and keyboard_driver_cfg != null) {
                const loaded_keyboard = loadUserElfIntoProcessPagesOrHalt(
                    state,
                    .Process3,
                    process3_user_page.?.paddr,
                    process3_user_stack_page.?.paddr,
                    disk_keyboard_driver_elf.?,
                    "keyboard driver ELF load into user page failed\n",
                );
                setThreadEntryIfReady(3, loaded_keyboard.entry, user_entry_rsp);
                logElfLoadSummary("KeyboardDriver ELF mapped", loaded_keyboard);
                if (!enable_title_only_ready_logs) {
                    serialWrite("  kbd_cfg_va=");
                    printHex(keyboard_driver_config_va);
                    serialWrite("\n");
                }
            }
            if (disk_compositor_elf) |compositor_image| {
                armDeferredCompositorLaunch(
                    compositor_image,
                    framebuffer_server_user_page.?.paddr,
                    framebuffer_server_user_stack_page.?.paddr,
                );
                serialWrite("deferred compositor launch armed (press Enter)\n");
            }
        },
        .MouseCompositor => {
            const info = framebuffer_info.?;
            const loaded_mouse = loadUserElfIntoProcessPagesOrHalt(
                state,
                .Process0,
                process0_user_page.?.paddr,
                process0_user_stack_page.?.paddr,
                disk_mouse_driver_elf.?,
                "mouse driver ELF load into user page failed\n",
            );
            setThreadEntry(0, loaded_mouse.entry, boot_log_console_entry_rsp);
            logElfLoadSummary("MouseDriver ELF mapped", loaded_mouse);
            if (!enable_title_only_ready_logs) {
                serialWrite("  cfg_va=");
                printHex(mouse_driver_config_va);
                serialWrite("\n");
            }

            publishMouseSharedPage(mouse_shared_page.?.paddr, info);
            const loaded_bootlog_sender = loadUserElfIntoProcessPagesOrHalt(
                state,
                .Process2,
                process2_user_page.?.paddr,
                process2_user_stack_page.?.paddr,
                disk_bootlog_sender_elf.?,
                "bootlog sender ELF load into user page failed\n",
            );
            setThreadEntryIfReady(2, loaded_bootlog_sender.entry, user_entry_rsp);
            logElfLoadSummary("BootLogSender ELF mapped", loaded_bootlog_sender);

            loaded_elf = loadUserElfIntoProcessPagesOrHalt(
                state,
                .Process1,
                framebuffer_server_user_page.?.paddr,
                framebuffer_server_user_stack_page.?.paddr,
                disk_compositor_elf.?,
                "compositor ELF load into user page failed\n",
            );
            setThreadEntry(1, loaded_elf.?.entry, user_entry_rsp);
            logElfLoadSummary("Compositor ELF mapped", loaded_elf.?);
            if (!enable_title_only_ready_logs) {
                serialWrite("  shared_va=");
                printHex(mouse_shared_draw_va);
                serialWrite("\n");
                serialWrite("  fb_va=");
                printHex(framebuffer_user_va);
                serialWrite("\n");
            }
            if (process3_user_page != null and process3_user_stack_page != null and keyboard_driver_config_page != null and keyboard_driver_cfg != null) {
                const loaded_keyboard = loadUserElfIntoProcessPagesOrHalt(
                    state,
                    .Process3,
                    process3_user_page.?.paddr,
                    process3_user_stack_page.?.paddr,
                    disk_keyboard_driver_elf.?,
                    "keyboard driver ELF load into user page failed\n",
                );
                setThreadEntryIfReady(3, loaded_keyboard.entry, user_entry_rsp);
                logElfLoadSummary("KeyboardDriver ELF mapped", loaded_keyboard);
                if (!enable_title_only_ready_logs) {
                    serialWrite("  kbd_cfg_va=");
                    printHex(keyboard_driver_config_va);
                    serialWrite("\n");
                }
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
                framebuffer_server_user_page.?.paddr,
                framebuffer_server_user_stack_page.?.paddr,
                disk_boot_log_console_elf.?,
                "boot log console ELF load into user page failed\n",
            );
            setThreadEntry(1, loaded_elf.?.entry, boot_log_console_entry_rsp);
            logElfLoadSummary("BootLogConsole ELF mapped", loaded_elf.?);
            if (!enable_title_only_ready_logs) {
                serialWrite("  log_va=");
                printHex(boot_log_user_va);
                serialWrite("\n");
                serialWrite("  log_bytes=");
                printNumber(if (boot_log_len > boot_log_page_payload_bytes) boot_log_page_payload_bytes else boot_log_len);
                serialWrite("\n");
            }
            if (process3_user_page != null and process3_user_stack_page != null and keyboard_driver_config_page != null and keyboard_driver_cfg != null) {
                const loaded_keyboard = loadUserElfIntoProcessPagesOrHalt(
                    state,
                    .Process3,
                    process3_user_page.?.paddr,
                    process3_user_stack_page.?.paddr,
                    disk_keyboard_driver_elf.?,
                    "keyboard driver ELF load into user page failed\n",
                );
                setThreadEntryIfReady(3, loaded_keyboard.entry, user_entry_rsp);
                logElfLoadSummary("KeyboardDriver ELF mapped", loaded_keyboard);
                if (!enable_title_only_ready_logs) {
                    serialWrite("  kbd_cfg_va=");
                    printHex(keyboard_driver_config_va);
                    serialWrite("\n");
                }
            }
        },
        .FramebufferIpc => {
            const loaded_draw_client = loadUserElfIntoUserPageOrHalt(
                process0_user_page.?.paddr,
                disk_draw_client_elf.?,
                "draw client ELF load into user page failed\n",
            );
            setThreadEntry(0, loaded_draw_client.entry, null);
            logElfLoadSummary("DrawClient ELF mapped", loaded_draw_client);
            loaded_elf = loadUserElfIntoUserPageOrHalt(
                framebuffer_server_user_page.?.paddr,
                disk_framebuffer_server_elf.?,
                "framebuffer server ELF load into user page failed\n",
            );
            setThreadEntry(1, loaded_elf.?.entry, null);
            logElfLoadSummary("FramebufferServer ELF mapped", loaded_elf.?);
        },
        .DiskUser => {
            loaded_elf = loadUserElfIntoUserPageOrHalt(
                process0_user_page.?.paddr,
                disk_user_elf.?,
                "disk ELF load into user page failed\n",
            );
            setThreadEntry(0, loaded_elf.?.entry, null);
            logElfLoadSummary("ELF image loaded", loaded_elf.?);
        },
    }

    state.pte_sync_hook = capability.syncPageTableRightsForPrincipalPaddr;
    kernel_state_ready = true;
    logRuntimeFramebufferSummaryBeforeSendCap();
    switch (boot_runtime_mode) {
        .MouseCompositor => {
            if (keyboard_driver_cfg != null and thread_contexts[3].ready) {
                serialWrite("\nenter ring3 with iretq (mouse driver + compositor + bootlog sender + keyboard driver)\n");
            } else {
                serialWrite("\nenter ring3 with iretq (mouse driver + compositor + bootlog sender)\n");
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
                serialWrite("\nenter ring3 with iretq (boot log console + mouse + keyboard; Enter to launch compositor)\n");
            } else {
                serialWrite("\nenter ring3 with iretq (boot log console + mouse; Enter to launch compositor)\n");
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
        armBootLogGateDeferredInputStart(
            thread_contexts[0].ready,
            keyboard_driver_cfg != null and thread_contexts[3].ready,
        );
    }
    const boot_ctx = getThreadContextConst(current_thread_index).?;
    enterUserModeIretq(boot_ctx.frame.rip, boot_ctx.frame.rsp);
}
