pub const read_max_bytes: usize = 1024;

// The request and reply occupy one user-owned buffer. The kernel copies the
// input header first and never retains a user pointer while holding its ring
// lock. A cursor of zero starts at the oldest retained byte.
pub const Read = extern struct {
    reserved0: u64,
    cursor: u64,
    limit: u64,
    reserved: u64,
    next_cursor: u64,
    oldest_cursor: u64,
    written: u64,
    dropped: u64,
    overrun: u64,
    bytes: [read_max_bytes]u8,
};

pub fn requestIsValid(request: *const Read) bool {
    // Every byte in the input header is checked; alignment padding must not
    // accidentally become an accepted flag or an implicit format selector.
    return request.reserved0 == 0 and request.reserved == 0 and
        request.limit != 0 and request.limit <= read_max_bytes;
}

comptime {
    if (@offsetOf(Read, "bytes") != 72 or @sizeOf(Read) != 1096)
        @compileError("kernel log read ABI layout mismatch");
}

test "log request checks both complete reserved slots and the byte limit" {
    const std = @import("std");
    var request = std.mem.zeroes(Read);
    request.limit = read_max_bytes;
    try std.testing.expect(requestIsValid(&request));
    request.reserved0 = 1;
    try std.testing.expect(!requestIsValid(&request));
    request.reserved0 = @as(u64, 1) << 32;
    try std.testing.expect(!requestIsValid(&request));
    request.reserved0 = 0;
    request.reserved = 1;
    try std.testing.expect(!requestIsValid(&request));
    request.reserved = 0;
    request.limit = 0;
    try std.testing.expect(!requestIsValid(&request));
    request.limit = read_max_bytes + 1;
    try std.testing.expect(!requestIsValid(&request));
}
