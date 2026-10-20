//! Intrusive cancellation ownership shared by IPC (K) and scheduler (T).
//! The IPC call body is stable while a node is bound, pending, or draining.
//! T only moves that owned reference; it never calls IPC or an allocator.
pub const Kind = enum { outgoing, implicit, receive, block_owner };
pub const State = enum { detached, bound, pending, draining };
pub const Node = struct {
    context: ?*anyopaque = null,
    kind: Kind = .outgoing,
    state: State = .detached,
    next: ?*Node = null,
    wait_token: u64 = 0,
    terminal_token: u64 = 0,
};
