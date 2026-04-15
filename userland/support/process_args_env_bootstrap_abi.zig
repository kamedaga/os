const std = @import("std");
const process_abi = @import("process_abi.zig");

pub const magic: u64 = 0x5052_4147_4556_3131;
pub const version: u64 = 1;
pub const target_va: u64 = process_abi.auxPageVa(36);

pub const max_args: usize = 32;
pub const max_env: usize = 32;
pub const page_bytes: usize = @intCast(process_abi.aux_page_bytes);

pub const Entry = extern struct {
    offset: u16 = 0,
    len: u16 = 0,
};

pub const Header = extern struct {
    magic: u64 = magic,
    version: u64 = version,
    arg_count: u64 = 0,
    env_count: u64 = 0,
    string_bytes: u64 = 0,
    reserved0: u64 = 0,
};

pub const data_bytes: usize = page_bytes - @sizeOf(Header) - (@sizeOf(Entry) * max_args) - (@sizeOf(Entry) * max_env);

pub const Page = extern struct {
    header: Header = .{},
    args: [max_args]Entry = [_]Entry{.{}} ** max_args,
    env: [max_env]Entry = [_]Entry{.{}} ** max_env,
    data: [data_bytes]u8 = [_]u8{0} ** data_bytes,
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
    page.header.arg_count = 0;
    page.header.env_count = 0;
    page.header.string_bytes = 0;
    page.header.reserved0 = 0;
}

pub const Writer = struct {
    page: *volatile Page,
    cursor: usize = 0,

    pub fn init(source_va: u64) Writer {
        initZeroPage(source_va);
        return .{
            .page = @ptrFromInt(source_va),
            .cursor = 0,
        };
    }

    pub fn pushArg(self: *Writer, value: []const u8) bool {
        if (self.page.header.arg_count >= max_args) return false;
        const index: usize = @intCast(self.page.header.arg_count);
        if (!self.writeEntry(&self.page.args[index], value)) return false;
        self.page.header.arg_count += 1;
        return true;
    }

    pub fn pushEnvRaw(self: *Writer, value: []const u8) bool {
        if (self.page.header.env_count >= max_env) return false;
        const index: usize = @intCast(self.page.header.env_count);
        if (!self.writeEntry(&self.page.env[index], value)) return false;
        self.page.header.env_count += 1;
        return true;
    }

    pub fn pushEnv(self: *Writer, key: []const u8, value: []const u8) bool {
        if (std.mem.indexOfScalar(u8, key, '=')) |_| return false;
        if (self.page.header.env_count >= max_env) return false;
        const total_len = key.len + 1 + value.len;
        if (total_len > std.math.maxInt(u16)) return false;
        if (self.cursor + total_len > data_bytes) return false;

        const entry_index: usize = @intCast(self.page.header.env_count);
        self.page.env[entry_index].offset = @intCast(self.cursor);
        self.page.env[entry_index].len = @intCast(total_len);

        const data_ptr: [*]volatile u8 = @ptrFromInt(@intFromPtr(self.page) + @offsetOf(Page, "data"));
        var i: usize = 0;
        while (i < key.len) : (i += 1) data_ptr[self.cursor + i] = key[i];
        data_ptr[self.cursor + key.len] = '=';
        i = 0;
        while (i < value.len) : (i += 1) data_ptr[self.cursor + key.len + 1 + i] = value[i];

        self.cursor += total_len;
        self.page.header.env_count += 1;
        self.page.header.string_bytes = self.cursor;
        return true;
    }

    fn writeEntry(self: *Writer, entry: *volatile Entry, value: []const u8) bool {
        if (value.len > std.math.maxInt(u16)) return false;
        if (self.cursor + value.len > data_bytes) return false;
        entry.offset = @intCast(self.cursor);
        entry.len = @intCast(value.len);
        const data_ptr: [*]volatile u8 = @ptrFromInt(@intFromPtr(self.page) + @offsetOf(Page, "data"));
        var i: usize = 0;
        while (i < value.len) : (i += 1) {
            data_ptr[self.cursor + i] = value[i];
        }
        self.cursor += value.len;
        self.page.header.string_bytes = self.cursor;
        return true;
    }
};

comptime {
    std.debug.assert(@sizeOf(Page) == page_bytes);
}
