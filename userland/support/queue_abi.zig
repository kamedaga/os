const std = @import("std");

pub const syscall_grant_cap: u64 = 0x23;

pub const queue_cap_token_tag: u64 = (1 << 62) | (1 << 60);

pub fn encodeQueueCapToken(token: u64) u64 {
    std.debug.assert(token != 0);
    std.debug.assert((token & queue_cap_token_tag) == 0);
    return queue_cap_token_tag | token;
}

pub fn decodeQueueCapToken(value: u64) ?u64 {
    if ((value & queue_cap_token_tag) != queue_cap_token_tag) return null;
    const token = value & ~queue_cap_token_tag;
    if (token == 0) return null;
    return token;
}
