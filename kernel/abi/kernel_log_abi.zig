pub const read_version: u64 = 1;
pub const read_max_bytes: usize = 1024;

// The request and reply occupy one user-owned buffer. The kernel copies the
// input header first and never retains a user pointer while holding its ring
// lock. A cursor of zero starts at the oldest retained byte.
pub const Read = extern struct {
    version: u64,
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

comptime {
    if (@offsetOf(Read, "bytes") != 72 or @sizeOf(Read) != 1096)
        @compileError("kernel log read ABI layout mismatch");
}
