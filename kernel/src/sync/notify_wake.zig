//! NOTIFY-only wake edge. Publishers hold K across request; the edge is sent
//! by the same CPU right after that K release (sync/deferred_wake.zig). A
//! later publisher may see the token still pending before the send and rely on
//! it, which is safe because the send is unconditional and a failed deferred
//! send halts the kernel. The receiver clears before inspecting its runqueue
//! or returning to idle.
pub const Token = struct {
    pending: u32 = 0,

    pub fn request(self: *Token) bool {
        return @atomicRmw(u32, &self.pending, .Xchg, 1, .acq_rel) == 0;
    }
    pub fn received(self: *Token) void {
        _ = @atomicRmw(u32, &self.pending, .Xchg, 0, .acq_rel);
    }
    pub fn failed(self: *Token) void {
        // Only a direct send (no K deferral row yet) returns failure to its
        // caller while K still excludes a second publisher.
        self.received();
    }
};

const std = @import("std");
test "notify wake publication before receiver clear shares edge" {
    var token: Token = .{};
    try std.testing.expect(token.request());
    try std.testing.expect(!token.request());
    try std.testing.expect(!token.request());
    token.received();
    // The handler's subsequent queue observation includes all publications.
    try std.testing.expect(token.request());
}
test "notify wake publication after receiver clear requires new edge" {
    var token: Token = .{};
    try std.testing.expect(token.request());
    token.received();
    // Includes ring-0 early return and the idle final-check/sti-hlt window:
    // an edge consumed before publication cannot suppress this new edge.
    try std.testing.expect(token.request());
    token.received();
    try std.testing.expect(token.request());
}
test "notify wake failed send can retry and CPU tokens are independent" {
    var tokens = [_]Token{ .{}, .{} };
    try std.testing.expect(tokens[0].request());
    tokens[0].failed();
    try std.testing.expect(tokens[0].request());
    try std.testing.expect(tokens[1].request());
    tokens[1].received();
    try std.testing.expect(!tokens[0].request());
    try std.testing.expect(tokens[1].request());
}

test "wake preemption ring0 edge acknowledges token without clearing scheduling work" {
    var token: Token = .{};
    var scheduler_pending = true;
    try std.testing.expect(token.request());
    try std.testing.expect(!token.request()); // Publications share the first edge.
    token.received(); // Ring-0 entry acknowledges; it cannot switch context.
    try std.testing.expect(scheduler_pending);
    try std.testing.expect(token.request()); // Later idle publication needs an edge.
    token.received(); // Unrelated maintenance is an idempotent acknowledgement.
    token.received();
    try std.testing.expect(token.request());
    // A committed K-deferred send is unconditional even if an intervening
    // maintenance arrival cleared its token. Its arrival clears the newer
    // token too, but every publication still has a real, outstanding edge.
    token.received();
    try std.testing.expect(token.request());
    scheduler_pending = false; // Only a safe policy consumer settles the work.
    try std.testing.expect(!scheduler_pending);
}
