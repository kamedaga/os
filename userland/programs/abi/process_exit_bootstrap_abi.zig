const std = @import("std");
const process_abi = @import("process_abi.zig");

pub const magic: u64 = 0x5052_5845_5449_5431;
pub const version: u64 = 1;

pub const target_va: u64 = process_abi.auxPageVa(35);

pub const state_idle: u64 = 0;
pub const state_exited: u64 = 1;

pub const page_bytes: usize = @intCast(process_abi.aux_page_bytes);

pub const PageHeader = extern struct {
    magic: u64 = magic,
    version: u64 = version,
    state: u64 = state_idle,
    exit_code: u64 = 0,
};

pub const reserved_bytes: usize = page_bytes - @sizeOf(PageHeader);

pub const Page = extern struct {
    header: PageHeader = .{},
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
    page.header.exit_code = 0;
}

comptime {
    std.debug.assert(@sizeOf(Page) == page_bytes);
}
