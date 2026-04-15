const std = @import("std");
const process_abi = @import("process_abi.zig");

pub const magic: u64 = 0x5354_4449_4F53_4831;
pub const version: u64 = 1;

pub const target_va: u64 = process_abi.auxPageVa(34);
pub const shell_endpoint_id: u64 = 0x90;

pub const state_idle: u64 = 0;
pub const state_ready: u64 = 1;

pub const stream_kind_log: u64 = 0;
pub const stream_kind_stdout: u64 = 1;
pub const stream_kind_stderr: u64 = 2;
pub const stream_kind_control: u64 = 3;

pub const control_magic: u64 = 0x5354_4449_4F43_5431;
pub const control_version: u64 = 1;
pub const control_op_allocate_inherited: u64 = 1;
pub const control_op_bind_inherited: u64 = 2;

pub const mode_inherit: u64 = 0;
pub const mode_kernel_log: u64 = 1;
pub const mode_null: u64 = 2;

pub const log_mode_shift: u6 = 0;
pub const stdout_mode_shift: u6 = 2;
pub const stderr_mode_shift: u6 = 4;
pub const mode_mask: u64 = 0x3;

pub fn encodeModeFlags(log_mode: u64, stdout_mode: u64, stderr_mode: u64) u64 {
    return ((log_mode & mode_mask) << log_mode_shift) |
        ((stdout_mode & mode_mask) << stdout_mode_shift) |
        ((stderr_mode & mode_mask) << stderr_mode_shift);
}

pub const payload_bytes: usize = 512;
pub const page_bytes: usize = @intCast(process_abi.aux_page_bytes);

pub const PageHeader = extern struct {
    magic: u64 = magic,
    version: u64 = version,
    state: u64 = state_idle,
    endpoint_id: u64 = 0,
    shell_process_slot: u64 = 0,
    stream_kind: u64 = stream_kind_log,
    byte_len: u64 = 0,
    reserved0: u64 = 0,
};

pub const payload_offset: usize = @sizeOf(PageHeader);
pub const reserved_bytes: usize = page_bytes - payload_offset - payload_bytes;

pub const Page = extern struct {
    header: PageHeader = .{},
    payload: [payload_bytes]u8 = [_]u8{0} ** payload_bytes,
    reserved: [reserved_bytes]u8 = [_]u8{0} ** reserved_bytes,
};

fn clearWords(source_va: u64) void {
    const words: [*]volatile u64 = @ptrFromInt(source_va);
    var i: usize = 0;
    while (i < page_bytes / @sizeOf(u64)) : (i += 1) {
        words[i] = 0;
    }
}

pub fn initZeroPage(source_va: u64) void {
    const page: *volatile Page = @ptrFromInt(source_va);
    clearWords(source_va);
    page.header.magic = magic;
    page.header.version = version;
    page.header.state = state_idle;
    page.header.endpoint_id = 0;
    page.header.shell_process_slot = 0;
    page.header.stream_kind = stream_kind_log;
    page.header.byte_len = 0;
    page.header.reserved0 = encodeModeFlags(mode_kernel_log, mode_kernel_log, mode_kernel_log);
}

pub fn initShellSinkPage(source_va: u64, shell_process_slot: u64) void {
    const page: *volatile Page = @ptrFromInt(source_va);
    clearWords(source_va);
    page.header.magic = magic;
    page.header.version = version;
    page.header.state = state_idle;
    page.header.endpoint_id = shell_endpoint_id;
    page.header.shell_process_slot = shell_process_slot;
    page.header.stream_kind = stream_kind_log;
    page.header.byte_len = 0;
    page.header.reserved0 = encodeModeFlags(mode_inherit, mode_inherit, mode_inherit);
}

pub fn initNullPage(source_va: u64) void {
    const page: *volatile Page = @ptrFromInt(source_va);
    clearWords(source_va);
    page.header.magic = magic;
    page.header.version = version;
    page.header.state = state_idle;
    page.header.endpoint_id = 0;
    page.header.shell_process_slot = 0;
    page.header.stream_kind = stream_kind_log;
    page.header.byte_len = 0;
    page.header.reserved0 = encodeModeFlags(mode_null, mode_null, mode_null);
}

comptime {
    std.debug.assert(@sizeOf(Page) == page_bytes);
}
