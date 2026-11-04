//! The published RAM cache policy snapshot. Kept apart from user_copy so that
//! VMO install and shared discard can consult per-PA policy without pulling the
//! copy-window, TLB and trap code into every state-level host test.
const ram_cache = @import("ram_cache_policy.zig");

pub var current: ?*const ram_cache.Snapshot = null;

/// Only the serialized physical-cache transition owner may publish. The
/// snapshot and its slices stay immutable and mapped until an acknowledged
/// all-CPU rendezvous after replacement; the caller owns their reclamation.
pub fn publish(snapshot: ?*const ram_cache.Snapshot) void {
    @atomicStore(?*const ram_cache.Snapshot, &current, snapshot, .release);
}

/// Writer-side identity check without lending out the immutable snapshot.
pub fn isCurrent(snapshot: ?*const ram_cache.Snapshot) bool {
    return @atomicLoad(?*const ram_cache.Snapshot, &current, .acquire) == snapshot;
}

/// Caller holds either its IRQ-disabled copy guard or an address-space lock
/// included in every writer's all-VM transaction, or K excluding that writer.
/// Return values own no borrow into a snapshot, so they remain valid after
/// the short lookup completes.
pub fn policyForLockedPage(paddr: u64) ram_cache.Snapshot.Decision {
    const snapshot = @atomicLoad(?*const ram_cache.Snapshot, &current, .acquire) orelse
        return .ordinary;
    return snapshot.query(paddr);
}

/// Same exclusion as the per-page lookup; pending-only snapshots are active.
pub fn hasPolicyLocked() bool {
    const snapshot = @atomicLoad(?*const ram_cache.Snapshot, &current, .acquire) orelse return false;
    return snapshot.policy.len != 0 or snapshot.pending.len != 0;
}
