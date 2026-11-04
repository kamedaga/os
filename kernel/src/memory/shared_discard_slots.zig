//! Boot-reserved ownership records for detached shared VMO pages. Payloads
//! contain only PAs, never a live store borrow or data-page metadata.
const std = @import("std");
pub const capacity = 1024;
pub const State = enum(u8) { free, occupied, filled, draining };
pub const Slot = struct {
    state: State = .free,
    count: usize = 0,
    pages: [capacity]u64 = undefined,
    pub fn publish(self: *Slot) void {
        // The producer owns occupied exclusively. Release publishes both PA
        // ownership and count; reclaim cannot observe a half-detached batch.
        @atomicStore(State, &self.state, .filled, .release);
    }
};
pub const Pool = struct {
    slots: [8]Slot = @splat(.{}),
    active: usize = 0,
    context: *anyopaque = undefined,
    return_run: *const fn (*anyopaque, u64, usize) void = undefined,
    pub fn init(self: *Pool, cpus: usize, fl: anytype) void {
        // Boot/test setup only, before publication. BSS reserves the maximum;
        // only min(CPUs, 8) records participate, with no runtime allocation.
        self.* = .{ .active = @min(@max(cpus, 1), 8), .context = fl, .return_run = struct {
            fn give(context: *anyopaque, start: u64, count: usize) void {
                const list: @TypeOf(fl) = @ptrCast(@alignCast(context));
                list.appendContiguousRange(start, count) catch @panic("shared discard PMM ownership");
            }
        }.give };
    }
    pub fn claim(self: *Pool) ?*Slot {
        for (self.slots[0..self.active]) |*slot| {
            if (@cmpxchgStrong(State, &slot.state, .free, .occupied, .acquire, .monotonic) == null) return slot;
        }
        for (self.slots[0..self.active]) |*slot| {
            if (@cmpxchgStrong(State, &slot.state, .filled, .draining, .acquire, .monotonic) != null) continue;
            // Caller holds K but no store/quarantine leaf. drain takes PMM
            // only after each quarantine query releases its own leaf.
            _ = self.drain(slot);
            if (@cmpxchgStrong(State, &slot.state, .free, .occupied, .acquire, .monotonic) == null) return slot;
        }
        // Never wait for occupied/draining producers or steal their payload.
        return null;
    }
    fn drain(self: *Pool, slot: *Slot) usize {
        // Only a successful filled->draining acquire may call this function.
        // Exclusive ownership also excludes IRQ/reentrant OOM readers.
        const count = slot.count;
        if (count > capacity) @panic("shared discard slot count");
        const pages = slot.pages[0..count];
        std.mem.sort(u64, pages, {}, std.sort.asc(u64));
        for (pages, 0..) |pa, i| {
            if (pa == 0 or pa & 4095 != 0 or (i != 0 and pages[i - 1] >= pa)) @panic("shared discard slot PA");
        }
        var first: usize = 0;
        while (first < count) {
            var end = first + 1;
            while (end < count and pages[end] - pages[end - 1] == 4096) : (end += 1) {}
            self.return_run(self.context, pages[first], end - first);
            first = end;
        }
        slot.count = 0;
        // Publish reuse only after all returns and count erasure. No PA is
        // read after publishing free, since another producer may own it.
        @atomicStore(State, &slot.state, .free, .release);
        return count;
    }
    pub fn reclaim(self: *Pool) usize {
        var returned: usize = 0;
        for (self.slots[0..self.active]) |*slot| {
            if (@cmpxchgStrong(State, &slot.state, .filled, .draining, .acquire, .monotonic) != null) continue;
            returned += self.drain(slot);
        }
        return returned;
    }
};
pub var global: Pool = .{};
