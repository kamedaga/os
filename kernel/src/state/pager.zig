//! Port and pending-request lifetime. Callers exclude VMA replacement with AS
//! and shared-VM locks when reading a VMO. The pager lock protects only the
//! lists below; it is never held while a thread is descheduled or while a
//! user buffer or physical copy window is touched.
const std = @import("std");
const types = @import("types.zig");
const diag = @import("../sync/lock_diagnostics.zig");
const scheduler = @import("../scheduler.zig").connection;
const abi = @import("kernel_abi_root").pager_abi;
const clock = @import("../realtime_clock.zig");

fn pagerNowNs() u64 {
    if (comptime @import("builtin").is_test) return 0;
    return clock.monotonicNs() orelse 0;
}

fn waitBucket(us: u64) usize {
    if (us < 640) return @intCast(us / 10);
    var remaining = us - 640;
    for (0..12) |group| {
        const width: u64 = @as(u64, 10) << @as(u6, @intCast(group));
        if (remaining < width * 8) return 64 + group * 8 + @as(usize, @intCast(remaining / width));
        remaining -= width * 8;
    }
    return 159;
}

fn recordWaitLatency(self: anytype, started_ns: u64) void {
    if (started_ns == 0) return;
    const now = pagerNowNs();
    if (now < started_ns) return;
    const bucket = waitBucket((now - started_ns) / 1000);
    _ = @atomicRmw(u64, &self.pager_wait_histogram[bucket], .Add, 1, .monotonic);
    _ = @atomicRmw(u64, &self.pager_wait_samples, .Add, 1, .monotonic);
}

pub const Lock = struct {
    byte: diag.Byte = .{},
    pub fn lock(self: *Lock) bool {
        const restore = diag.maskInterrupts();
        diag.beforeAcquire(&self.byte.record, .{ .class = .pager });
        while (@cmpxchgWeak(u8, &self.byte.value, 0, 1, .acquire, .monotonic) != null)
            std.atomic.spinLoopHint();
        diag.acquired(&self.byte.record, .{ .class = .pager });
        return restore;
    }
    pub fn unlock(self: *Lock, restore: bool) void {
        diag.releasing(&self.byte.record);
        @atomicStore(u8, &self.byte.value, 0, .release);
        diag.restoreInterrupts(restore);
    }
};

fn port(self: anytype, ref: types.PagerPortRef) ?*types.PagerPortSlot {
    if (ref.isNull()) return null;
    return self.pager_ports.resolve(.{ .index = ref.index, .generation = ref.generation });
}
fn request(self: anytype, ref: types.PagerRequestRef) ?*types.PagerRequestSlot {
    if (ref.isNull()) return null;
    return self.pager_requests.resolve(.{ .index = ref.index, .generation = ref.generation });
}
fn waiter(self: anytype, ref: types.PagerWaiterRef) ?*types.PagerWaiterSlot {
    if (ref.isNull()) return null;
    return self.pager_waiters.resolve(.{ .index = ref.index, .generation = ref.generation });
}
fn release(self: anytype, ref: types.PagerReleaseRef) ?*types.PagerReleaseSlot {
    if (ref.isNull()) return null;
    return self.pager_releases.resolve(.{ .index = ref.index, .generation = ref.generation });
}

fn encodeRequest(ref: types.PagerRequestRef) u64 {
    return (@as(u64, ref.generation) << 32) | ref.index;
}
fn decodeRequest(value: u64) types.PagerRequestRef {
    return .{ .index = @truncate(value), .generation = @truncate(value >> 32) };
}
fn encodeRelease(ref: types.PagerReleaseRef) u64 {
    return (@as(u64, ref.generation) << 32) | ref.index;
}
fn decodeRelease(value: u64) types.PagerReleaseRef {
    return .{ .index = @truncate(value), .generation = @truncate(value >> 32) };
}
// Caller holds pager_lock; the VMO lifetime is excluded by AS/shared-VM or a
// pending request retain. Backing-index lock ranks after pager_lock.
fn pageRequest(self: anytype, vmo: *const types.NativeVmoSlot, page: u32) ?*types.PagerRequestSlot {
    const store = @constCast(&vmo.pager_request_store);
    store.lock.lock();
    const encoded = store.getLocked(page);
    store.lock.unlock();
    if (encoded == 0) return null;
    return request(self, decodeRequest(encoded));
}
fn setPageRequest(vmo: *types.NativeVmoSlot, page: u32, ref: types.PagerRequestRef) void {
    const store = &vmo.pager_request_store;
    store.lock.lock();
    // requestFault prepared the path before taking pager_lock.
    store.putLocked(page, encodeRequest(ref));
    store.lock.unlock();
}

// Both per-port lists are intrusive so an out-of-order FAIL can unlink its
// request without scanning other processes' faults with IRQs disabled.
fn enqueueFault(self: anytype, p: *types.PagerPortSlot, ref: types.PagerRequestRef, published: bool) void {
    const r = request(self, ref).?;
    const tail = if (published) p.last_fault else p.last_unpublished;
    r.prev = tail;
    r.next = .{};
    r.queue = if (published) .fault else .unpublished;
    if (published) {
        if (tail.isNull()) p.first_fault = ref else request(self, tail).?.next = ref;
        p.last_fault = ref;
        p.queued_faults += 1;
        r.published = true;
    } else {
        if (tail.isNull()) p.first_unpublished = ref else request(self, tail).?.next = ref;
        p.last_unpublished = ref;
    }
}

fn unlinkFault(self: anytype, p: *types.PagerPortSlot, r: *types.PagerRequestSlot) void {
    if (r.queue == .none) return;
    const previous = r.prev;
    const next = r.next;
    const first = if (r.queue == .fault) &p.first_fault else &p.first_unpublished;
    const last = if (r.queue == .fault) &p.last_fault else &p.last_unpublished;
    if (previous.isNull()) first.* = next else request(self, previous).?.next = next;
    if (next.isNull()) last.* = previous else request(self, next).?.prev = previous;
    if (r.queue == .fault) p.queued_faults -= 1;
    r.prev = .{};
    r.next = .{};
    r.queue = .none;
}

fn linkPortRequest(self: anytype, p: *types.PagerPortSlot, ref: types.PagerRequestRef) void {
    const r = request(self, ref).?;
    r.next_port = p.first_request;
    r.prev_port = .{};
    if (request(self, p.first_request)) |head| head.prev_port = ref;
    p.first_request = ref;
}
fn unlinkPortRequest(self: anytype, p: *types.PagerPortSlot, r: *types.PagerRequestSlot) void {
    if (r.prev_port.isNull()) p.first_request = r.next_port else request(self, r.prev_port).?.next_port = r.next_port;
    if (request(self, r.next_port)) |next| next.prev_port = r.prev_port;
    r.next_port = .{};
    r.prev_port = .{};
}
fn promoteUnpublished(self: anytype, p: *types.PagerPortSlot) types.PagerWaiterRef {
    if (p.queued_faults >= abi.fault_queue_capacity) return .{};
    const ref = p.first_unpublished;
    const r = request(self, ref) orelse return .{};
    unlinkFault(self, p, r);
    enqueueFault(self, p, ref, true);
    return detachPortWaiter(self, p);
}

pub fn portRefForFd(self: anytype, owner: types.PrincipalId, fd: types.Fd, rights: types.FdRights) ?types.PagerPortRef {
    const view = self.fdPayloadWithRightsConst(owner, fd, rights) orelse return null;
    return switch (view.payload.*) {
        .pager_port => |ref| ref,
        else => null,
    };
}

/// A fork inherits non-private FD entries even though the port's creator
/// identity stays unchanged. Test actual control capabilities when mapping so
/// a fork child cannot both wait on and fault into its inherited pager.
pub fn processControlsPort(self: anytype, owner: types.PrincipalId, port_ref: types.PagerPortRef) bool {
    const table = self.getFdTableConst(owner) orelse return false;
    for (0..table.capacity) |index| {
        const entry = table.at(index);
        if (entry.object.isNull() or !entry.rights.pager_fault) continue;
        const slot = self.kernelObjectSlotConst(entry.object) orelse continue;
        switch (slot.payload) {
            .pager_port => |held| if (held.index == port_ref.index and held.generation == port_ref.generation) return true,
            else => {},
        }
    }
    return false;
}

pub fn createPortFd(self: anytype, owner: types.PrincipalId, flags: types.FdFlags, min_fd: types.Fd) types.KernelError!types.Fd {
    try self.requireActiveProcess(owner);
    const restore = self.pager_lock.lock();
    const allocation = self.allocatePool(&self.pager_ports, null, std.math.maxInt(u32)) catch |err| {
        self.pager_lock.unlock(restore);
        return err;
    };
    const ref = types.PagerPortRef{ .index = allocation.ref.index, .generation = allocation.ref.generation };
    allocation.value.* = .{ .generation = ref.generation, .live = true, .owner = @intFromEnum(owner) };
    self.pager_lock.unlock(restore);
    const object = self.createKernelObject(.pager_port, .{ .pager_port = ref }) catch |err| {
        const held = self.pager_lock.lock();
        self.pager_ports.remove(allocation.value, null);
        self.pager_lock.unlock(held);
        return err;
    };
    const rights: types.FdRights = .{ .inspect = true, .dup = true, .close = true, .wait = true, .pager_fault = true };
    return self.installFd(owner, object, rights, flags, min_fd) catch |err| {
        if (self.kernelObjectSlot(object)) |slot| self.clearKernelObjectSlot(slot);
        return err;
    };
}

pub fn attach(self: anytype, owner: types.PrincipalId, port_fd: types.Fd, vmo_fd: types.Fd, key: u64) types.KernelError!u64 {
    const port_ref = portRefForFd(self, owner, port_fd, .{ .pager_fault = true }) orelse return error.InvalidState;
    const vmo_view = self.fdPayloadWithRightsConst(owner, vmo_fd, .{ .pager_attach = true }) orelse return error.InvalidState;
    const vmo_ref = switch (vmo_view.payload.*) { .vmo => |ref| ref, else => return error.InvalidState };
    // The caller holds shared-VM exclusion. It fixes the VMO identity while
    // the pager lock publishes its weak port registration and release node.
    const vmo = self.nativeVmoSlot(vmo_ref) orelse return error.InvalidState;
    if (vmo.kind != .pager or !vmo.pager_port.isNull()) return error.InvalidState;
    const held = self.pager_lock.lock();
    defer self.pager_lock.unlock(held);
    const p = port(self, port_ref) orelse return error.InvalidState;
    if (!p.live or p.attach_count == std.math.maxInt(u32)) return error.InvalidState;
    const node_allocation = try self.allocatePool(&self.pager_releases, null, std.math.maxInt(u32));
    const node_ref = types.PagerReleaseRef{ .index = node_allocation.ref.index, .generation = node_allocation.ref.generation };
    // The generation is an opaque tombstone handle. Key reuse is safe because
    // every SUPPLY/FAIL must return this identity and pass port/key checks.
    const generation = encodeRelease(node_ref);
    node_allocation.value.* = .{ .generation = node_ref.generation, .port = port_ref, .vmo = vmo_ref, .key = key, .attach_generation = generation };
    p.attach_count += 1;
    // Publish attach only after the reserved tombstone exists. Final VMO
    // release must never allocate and can therefore always queue RELEASE.
    vmo.pager_release = node_ref;
    vmo.pager_owner = p.owner;
    vmo.pager_key = key;
    vmo.pager_attach_generation = generation;
    vmo.pager_port = port_ref;
    return generation;
}

fn detachPortWaiter(self: anytype, p: *types.PagerPortSlot) types.PagerWaiterRef {
    const head = p.waiting;
    if (waiter(self, head)) |node| p.waiting = node.next;
    return head;
}

fn wakeAndRemove(self: anytype, first: types.PagerWaiterRef) void {
    var current = first;
    while (!current.isNull()) {
        const held = self.pager_lock.lock();
        const node = waiter(self, current) orelse {
            self.pager_lock.unlock(held);
            break;
        };
        const next = node.next;
        const target = node.*;
        self.pager_waiters.remove(node, null);
        self.pager_lock.unlock(held);
        _ = scheduler.wakeIfWaitingTokenGenerationWithRax(target.thread_index, target.thread_generation, target.owner, target.token, target.resume_rax);
        current = next;
    }
}

pub fn registerWaiter(self: anytype, owner: types.PrincipalId, thread_index: usize, generation: u32, token: u64, resume_rax: u64) types.KernelError!types.PagerWaiterRef {
    const allocation = try self.allocatePool(&self.pager_waiters, null, std.math.maxInt(u32));
    const ref = types.PagerWaiterRef{ .index = allocation.ref.index, .generation = allocation.ref.generation };
    allocation.value.* = .{ .generation = ref.generation, .owner = owner, .thread_index = thread_index, .thread_generation = generation, .token = token, .resume_rax = resume_rax };
    return ref;
}

pub const FaultResult = enum { pending, failed, already_present, invalid };

/// Caller holds the faulting AS and shared-VM locks and reserved a scheduler
/// token before entering either. The pending request takes its own VMO retain
/// so unmap can win without destroying the backing while the pager works.
pub fn requestFault(self: anytype, vmo_ref: types.NativeVmoRef, page_index: u32, access: u32, owner: types.PrincipalId, thread_index: usize, generation: u32, token: u64, resume_rax: u64) types.KernelError!FaultResult {
    const vmo = self.nativeVmoSlot(vmo_ref) orelse return .invalid;
    if (vmo.kind != .pager or page_index >= vmo.page_count or vmo.pager_port.isNull()) return .invalid;
    if (self.nativeVmoResolvedPagePaddr(vmo_ref, page_index) != null) return .already_present;
    vmo.pager_request_store.prepare(vmo.pager_request_start, page_index, try self.poolAllocator()) catch return error.OutOfFreePages;
    const wait_started_ns = pagerNowNs();
    const held = self.pager_lock.lock();
    const p = port(self, vmo.pager_port) orelse {
        self.pager_lock.unlock(held);
        return .failed;
    };
    if (!p.live) {
        self.pager_lock.unlock(held);
        return .failed;
    }
    const existing = pageRequest(self, vmo, page_index);
    if (existing) |r| {
        if (r.status == .failed) {
            self.pager_lock.unlock(held);
            return .failed;
        }
        if (token != 0) {
            const wref = registerWaiter(self, owner, thread_index, generation, token, resume_rax) catch |err| {
                self.pager_lock.unlock(held);
                return err;
            };
            waiter(self, wref).?.next = r.waiters;
            r.waiters = wref;
        }
        self.pager_lock.unlock(held);
        return .pending;
    }
    const allocation = self.allocatePool(&self.pager_requests, null, std.math.maxInt(u32)) catch |err| {
        self.pager_lock.unlock(held);
        return err;
    };
    const ref = types.PagerRequestRef{ .index = allocation.ref.index, .generation = allocation.ref.generation };
    // A user-copy probe publishes demand without a scheduler waiter. Its
    // caller returns to user mode and retries through the ordinary #PF path.
    const wref = if (token != 0) registerWaiter(self, owner, thread_index, generation, token, resume_rax) catch |err| {
        self.pager_requests.remove(allocation.value, null);
        self.pager_lock.unlock(held);
        return err;
    } else types.PagerWaiterRef{};
    self.retainNativeVmo(vmo_ref) catch |err| {
        if (!wref.isNull()) self.pager_waiters.remove(waiter(self, wref).?, null);
        self.pager_requests.remove(allocation.value, null);
        self.pager_lock.unlock(held);
        return err;
    };
    // Slot generation makes a reused request index distinguishable without a
    // port-wide ID search on every SUPPLY/FAIL.
    const id = encodeRequest(ref);
    allocation.value.* = .{ .generation = ref.generation, .active = true, .vmo = vmo_ref, .port = vmo.pager_port, .page_index = page_index, .request_id = id, .wait_started_ns = wait_started_ns, .access = access, .waiters = wref, .retained = true };
    setPageRequest(vmo, page_index, ref);
    linkPortRequest(self, p, ref);
    var to_wake: types.PagerWaiterRef = .{};
    if (p.queued_faults < abi.fault_queue_capacity) {
        enqueueFault(self, p, ref, true);
        to_wake = detachPortWaiter(self, p);
    } else enqueueFault(self, p, ref, false);
    self.pager_lock.unlock(held);
    if (!to_wake.isNull()) wakeAndRemove(self, to_wake);
    return .pending;
}

pub fn peekEvent(self: anytype, port_ref: types.PagerPortRef) ?abi.Event {
    const held = self.pager_lock.lock();
    defer self.pager_lock.unlock(held);
    const p = port(self, port_ref) orelse return null;
    if (!p.live) return null;
    // Release nodes were reserved at attach and have a separate queue. A
    // saturated fault stream cannot delay the final owner-handle close.
    if (release(self, p.first_release)) |node| {
        return .{ .kind = abi.event_vmo_release, .flags = 0, .key = node.key, .vmo_generation = node.attach_generation, .request_id = 0, .page_offset = 0, .page_length = 0, .access = 0 };
    }
    if (request(self, p.first_fault)) |r| {
        const vmo = self.nativeVmoSlotConst(r.vmo) orelse return null;
        return .{ .kind = abi.event_fault, .flags = 0, .key = vmo.pager_key, .vmo_generation = vmo.pager_attach_generation, .request_id = r.request_id, .page_offset = @as(u64, r.page_index) * types.native_page_size, .page_length = 4096, .access = r.access };
    }
    return null;
}

/// K serializes consumers while the output copy runs with no pager lock.
/// A new release may arrive between peek and consume; the chosen fault event
/// is still the fault queue head and cannot be replaced by another consumer.
pub fn consumeEvent(self: anytype, port_ref: types.PagerPortRef, event: abi.Event) bool {
    const held = self.pager_lock.lock();
    const p = port(self, port_ref) orelse { self.pager_lock.unlock(held); return false; };
    if (!p.live) { self.pager_lock.unlock(held); return false; }
    if (event.kind == abi.event_vmo_release) {
        const node = release(self, p.first_release) orelse { self.pager_lock.unlock(held); return false; };
        if (node.key != event.key or node.attach_generation != event.vmo_generation) { self.pager_lock.unlock(held); return false; }
        p.first_release = node.next;
        if (p.first_release.isNull()) p.last_release = .{};
        p.attach_count -= 1;
        self.pager_releases.remove(node, null);
        self.pager_lock.unlock(held);
        return true;
    }
    if (event.kind == abi.event_fault) {
        const r = request(self, p.first_fault) orelse { self.pager_lock.unlock(held); return false; };
        if (r.request_id != event.request_id) { self.pager_lock.unlock(held); return false; }
        unlinkFault(self, p, r);
        // A bounded public fault queue cannot reject a fault. The producer
        // stays blocked on an unpublished pending request until dequeue makes
        // room, then the oldest request becomes visible to the pager.
        const wake = promoteUnpublished(self, p);
        self.pager_lock.unlock(held);
        if (!wake.isNull()) wakeAndRemove(self, wake);
        return true;
    }
    self.pager_lock.unlock(held);
    return false;
}

pub fn vmoForKey(self: anytype, port_ref: types.PagerPortRef, key: u64, attach_generation: u64) ?types.NativeVmoRef {
    const held = self.pager_lock.lock();
    defer self.pager_lock.unlock(held);
    const p = port(self, port_ref) orelse return null;
    if (!p.live) return null;
    const node = release(self, decodeRelease(attach_generation)) orelse return null;
    if (node.port.index != port_ref.index or node.port.generation != port_ref.generation or
        node.key != key or node.attach_generation != attach_generation or node.vmo.isNull()) return null;
    return node.vmo;
}

pub fn requestCovers(self: anytype, port_ref: types.PagerPortRef, vmo_ref: types.NativeVmoRef, request_id: u64, first_page: u32, count: u32) bool {
    if (request_id == 0) return true;
    const held = self.pager_lock.lock();
    defer self.pager_lock.unlock(held);
    const r = request(self, decodeRequest(request_id)) orelse return false;
    return r.active and r.status == .pending and r.port.index == port_ref.index and r.port.generation == port_ref.generation and
        r.vmo.index == vmo_ref.index and r.vmo.generation == vmo_ref.generation and
        r.page_index >= first_page and r.page_index - first_page < count;
}

pub fn pageFailed(self: anytype, vmo_ref: types.NativeVmoRef, page_index: u32) bool {
    const vmo = self.nativeVmoSlotConst(vmo_ref) orelse return true;
    if (vmo.kind != .pager or vmo.pager_port.isNull()) return true;
    const held = self.pager_lock.lock();
    defer self.pager_lock.unlock(held);
    const p = port(self, vmo.pager_port) orelse return true;
    if (!p.live) return true;
    const r = pageRequest(self, vmo, page_index) orelse return false;
    return r.status == .failed;
}

pub fn registerPortWait(self: anytype, port_ref: types.PagerPortRef, owner: types.PrincipalId, thread_index: usize, generation: u32, token: u64) types.KernelError!bool {
    const held = self.pager_lock.lock();
    defer self.pager_lock.unlock(held);
    const p = port(self, port_ref) orelse return error.InvalidState;
    if (!p.live) return error.InvalidState;
    if (!p.first_release.isNull() or !p.first_fault.isNull()) return false;
    const ref = try registerWaiter(self, owner, thread_index, generation, token, abi.wait_wake_retry_status);
    waiter(self, ref).?.next = p.waiting;
    p.waiting = ref;
    return true;
}

pub fn cancelPortWait(self: anytype, port_ref: types.PagerPortRef, token: u64) void {
    const held = self.pager_lock.lock();
    defer self.pager_lock.unlock(held);
    const p = port(self, port_ref) orelse return;
    var link = &p.waiting;
    while (waiter(self, link.*)) |node| {
        if (node.token == token) {
            link.* = node.next;
            self.pager_waiters.remove(node, null);
            return;
        }
        link = &node.next;
    }
}

pub fn completeRequest(self: anytype, port_ref: types.PagerPortRef, key: u64, attach_generation: u64, request_id: u64, failed: bool, reason: u32) types.KernelError!void {
    if (request_id == 0) return error.InvalidState;
    const held = self.pager_lock.lock();
    const p = port(self, port_ref) orelse {
        self.pager_lock.unlock(held);
        return error.InvalidState;
    };
    if (!p.live) {
        self.pager_lock.unlock(held);
        return error.InvalidState;
    }
    const r = request(self, decodeRequest(request_id)) orelse {
        self.pager_lock.unlock(held);
        return error.InvalidState;
    };
    const vmo = self.nativeVmoSlot(r.vmo) orelse {
        self.pager_lock.unlock(held);
        return error.InvalidState;
    };
    if (!r.active or r.status != .pending or r.request_id != request_id or
        r.port.index != port_ref.index or r.port.generation != port_ref.generation or
        vmo.pager_key != key or vmo.pager_attach_generation != attach_generation) {
        self.pager_lock.unlock(held);
        return error.InvalidState;
    }
    const freed_public_slot = r.queue == .fault;
    unlinkFault(self, p, r);
    unlinkPortRequest(self, p, r);
    const port_wake = if (freed_public_slot) promoteUnpublished(self, p) else types.PagerWaiterRef{};
    const wake = r.waiters;
    r.waiters = .{};
    const vmo_ref = r.vmo;
    const wait_started_ns = r.wait_started_ns;
    r.retained = false;
    if (failed) {
        r.status = .failed;
        r.reason = reason;
        const ref = decodeRequest(request_id);
        r.next_failed = vmo.pager_failed_head;
        vmo.pager_failed_head = ref;
    } else {
        setPageRequest(vmo, r.page_index, .{});
        self.pager_requests.remove(r, null);
    }
    self.pager_lock.unlock(held);
    recordWaitLatency(self, wait_started_ns);
    // The request's retain is dropped after every waiter has been detached.
    // A stale signal token is harmless; a wake before block is latched by T.
    wakeAndRemove(self, wake);
    if (!port_wake.isNull()) wakeAndRemove(self, port_wake);
    self.releaseNativeVmo(vmo_ref);
}

pub fn completePublishedRange(self: anytype, port_ref: types.PagerPortRef, vmo_ref: types.NativeVmoRef, key: u64, attach_generation: u64, first: u32, count: u32) void {
    // SUPPLY is capped at 128 pages. Each lookup uses the VMO's sparse page
    // state, so this bounded loop never scans unrelated requests.
    for (0..count) |offset| {
        const held = self.pager_lock.lock();
        const vmo = self.nativeVmoSlotConst(vmo_ref);
        const r = if (vmo) |slot| pageRequest(self, slot, first + @as(u32, @intCast(offset))) else null;
        const id: u64 = if (r) |req| if (req.status == .pending) req.request_id else 0 else 0;
        self.pager_lock.unlock(held);
        if (id != 0) completeRequest(self, port_ref, key, attach_generation, id, false, 0) catch {};
    }
}

pub fn vmoReleased(self: anytype, vmo: *types.NativeVmoSlot) void {
    if (vmo.kind != .pager or vmo.pager_release.isNull()) return;
    var held = self.pager_lock.lock();
    // Failed records are weak tombstones linked only to this VMO. The global
    // request pool may contain unrelated processes' faults at final drop.
    var failed_ref = vmo.pager_failed_head;
    vmo.pager_failed_head = .{};
    self.pager_lock.unlock(held);
    while (!failed_ref.isNull()) {
        held = self.pager_lock.lock();
        const r = request(self, failed_ref) orelse unreachable;
        const next = r.next_failed;
        self.pager_requests.remove(r, null);
        self.pager_lock.unlock(held);
        failed_ref = next;
    }
    held = self.pager_lock.lock();
    const node = release(self, vmo.pager_release) orelse {
        self.pager_lock.unlock(held);
        return;
    };
    const p = port(self, node.port) orelse {
        self.pager_releases.remove(node, null);
        self.pager_lock.unlock(held);
        return;
    };
    node.vmo = .{};
    if (p.live) {
        node.queued = true;
        if (p.last_release.isNull()) p.first_release = vmo.pager_release else release(self, p.last_release).?.next = vmo.pager_release;
        p.last_release = vmo.pager_release;
        const wake = detachPortWaiter(self, p);
        self.pager_lock.unlock(held);
        if (!wake.isNull()) wakeAndRemove(self, wake);
    } else {
        p.attach_count -= 1;
        self.pager_releases.remove(node, null);
        if (p.attach_count == 0) self.pager_ports.remove(p, null);
        self.pager_lock.unlock(held);
    }
}

pub fn portClosed(self: anytype, port_ref: types.PagerPortRef) void {
    const held = self.pager_lock.lock();
    const p = port(self, port_ref) orelse {
        self.pager_lock.unlock(held);
        return;
    };
    if (!p.live) {
        self.pager_lock.unlock(held);
        return;
    }
    // Mark dead before dropping exclusion. No new request can join the
    // detached list, and each listed request retains its VMO until handled.
    p.live = false;
    const port_waiters = p.waiting;
    p.waiting = .{};
    var pending = p.first_request;
    p.first_request = .{};
    p.first_fault = .{};
    p.last_fault = .{};
    p.first_unpublished = .{};
    p.last_unpublished = .{};
    p.queued_faults = 0;
    self.pager_lock.unlock(held);
    wakeAndRemove(self, port_waiters);

    // A port can own many pending pages. Process one per IRQ-disabled scope;
    // the detached request retains are the lifetime pins between scopes.
    while (!pending.isNull()) {
        const again = self.pager_lock.lock();
        const r = request(self, pending) orelse unreachable;
        const next = r.next_port;
        r.next_port = .{};
        r.prev_port = .{};
        r.next = .{};
        r.prev = .{};
        r.queue = .none;
        r.status = .failed;
        r.reason = abi.fail_pager_dead;
        const vmo_ref = r.vmo;
        const vmo = self.nativeVmoSlot(vmo_ref) orelse unreachable;
        r.next_failed = vmo.pager_failed_head;
        vmo.pager_failed_head = pending;
        const wake = r.waiters;
        r.waiters = .{};
        std.debug.assert(r.retained);
        r.retained = false;
        self.pager_lock.unlock(again);
        wakeAndRemove(self, wake);
        self.releaseNativeVmo(vmo_ref);
        pending = next;
    }

    // Queued release tombstones also have their own reserved nodes. Drain one
    // per scope; a dead port never promises delivery to userland.
    while (true) {
        const again = self.pager_lock.lock();
        const current = port(self, port_ref) orelse {
            self.pager_lock.unlock(again);
            break;
        };
        const node = release(self, current.first_release) orelse {
            if (current.attach_count == 0) self.pager_ports.remove(current, null);
            self.pager_lock.unlock(again);
            break;
        };
        current.first_release = node.next;
        if (current.first_release.isNull()) current.last_release = .{};
        current.attach_count -= 1;
        self.pager_releases.remove(node, null);
        if (current.attach_count == 0) self.pager_ports.remove(current, null);
        self.pager_lock.unlock(again);
    }
}

pub fn ownerDied(self: anytype, owner: types.PrincipalId) void {
    // Process teardown is rare and must discover ports whose FD may have
    // escaped through fork/transfer. Scan at most 64 slots per IRQ-disabled
    // interval; K excludes new port creation for this owner during teardown.
    var cursor: usize = 0;
    while (cursor < self.pager_ports.capacity) {
        const held = self.pager_lock.lock();
        var candidate: types.PagerPortRef = .{};
        var scanned: usize = 0;
        while (cursor < self.pager_ports.capacity and scanned < 64) : ({ cursor += 1; scanned += 1; }) {
            const slot = self.pager_ports.get(@intCast(cursor)) orelse continue;
            if (!slot.live or slot.owner != @intFromEnum(owner)) continue;
            candidate = .{ .index = @intCast(cursor), .generation = slot.generation };
            cursor += 1;
            break;
        }
        self.pager_lock.unlock(held);
        if (candidate.isNull()) continue;
        // A fork may retain the control FD after its creator exits. Mark the
        // port dead separately from its final FD close so no waiter depends
        // on a process that can no longer service the queue.
        portClosed(self, candidate);
    }
}
