const std = @import("std");
const kernel = @import("kernel.zig");

pub const max_thread_slots: usize = kernel.max_thread_slots;
pub const inline_queue_depth: usize = 128;
pub const queue_chunk_entries: usize = 64;
pub const queue_chunk_pool_count: usize = 512;
pub const max_queue_depth: usize = inline_queue_depth + queue_chunk_entries * queue_chunk_pool_count;
const invalid_chunk: u16 = std.math.maxInt(u16);

pub const Message = struct {
    endpoint_id: u64 = 0,
    sender_thread: usize = 0,
    grants_reply: bool = false,
    mr0: u64 = 0,
    mr1: u64 = 0,
    mr2: u64 = 0,
    mr3: u64 = 0,
};

const Queue = struct {
    entries: [inline_queue_depth]Message = [_]Message{.{}} ** inline_queue_depth,
    overflow_head: u16 = invalid_chunk,
    len: usize = 0,

    fn enqueue(self: *Queue, msg: Message) bool {
        if (self.hasEquivalentCoalescibleSignal(msg)) return true;
        const slot = self.slotAtOrAllocate(self.len) orelse return false;
        slot.* = msg;
        self.len += 1;
        return true;
    }

    fn dequeue(self: *Queue) ?Message {
        if (self.len == 0) return null;
        const msg = (self.slotAtConst(0) orelse return null).*;
        var index: usize = 1;
        while (index < self.len) : (index += 1) {
            const item = (self.slotAtConst(index) orelse return null).*;
            (self.slotAt(index - 1) orelse return null).* = item;
        }
        self.len -= 1;
        if (self.slotAt(self.len)) |slot| slot.* = .{};
        self.trimUnusedChunks();
        return msg;
    }

    fn pushFront(self: *Queue, msg: Message) bool {
        _ = self.slotAtOrAllocate(self.len) orelse return false;
        var index = self.len;
        while (index > 0) {
            const item = (self.slotAtConst(index - 1) orelse return false).*;
            (self.slotAt(index) orelse return false).* = item;
            index -= 1;
        }
        self.entries[0] = msg;
        self.len += 1;
        return true;
    }

    fn removeFromSender(self: *Queue, sender_thread: usize) void {
        var read_index: usize = 0;
        var write_index: usize = 0;
        const old_len = self.len;
        while (read_index < old_len) : (read_index += 1) {
            const msg = (self.slotAtConst(read_index) orelse return).*;
            if (msg.sender_thread == sender_thread) continue;
            if (write_index != read_index) (self.slotAt(write_index) orelse return).* = msg;
            write_index += 1;
        }
        self.clearRange(write_index, old_len);
        self.len = write_index;
        self.trimUnusedChunks();
    }

    fn removeMatching(self: *Queue, sender_thread: usize, endpoint_id: u64, grants_reply: bool) usize {
        var removed: usize = 0;
        var read_index: usize = 0;
        var write_index: usize = 0;
        const old_len = self.len;
        while (read_index < old_len) : (read_index += 1) {
            const msg = (self.slotAtConst(read_index) orelse return removed).*;
            if (msg.sender_thread == sender_thread and msg.endpoint_id == endpoint_id and msg.grants_reply == grants_reply) {
                removed += 1;
                continue;
            }
            if (write_index != read_index) (self.slotAt(write_index) orelse return removed).* = msg;
            write_index += 1;
        }
        self.clearRange(write_index, old_len);
        self.len = write_index;
        self.trimUnusedChunks();
        return removed;
    }

    fn dequeueReplyFromSender(self: *Queue, sender_thread: usize) ?Message {
        if (self.len == 0) return null;
        var offset: usize = 0;
        while (offset < self.len) : (offset += 1) {
            const msg = (self.slotAtConst(offset) orelse return null).*;
            if (msg.sender_thread != sender_thread or msg.endpoint_id != 0 or msg.grants_reply) continue;

            var index = offset + 1;
            while (index < self.len) : (index += 1) {
                const item = (self.slotAtConst(index) orelse return null).*;
                (self.slotAt(index - 1) orelse return null).* = item;
            }
            self.len -= 1;
            if (self.slotAt(self.len)) |slot| slot.* = .{};
            self.trimUnusedChunks();
            return msg;
        }
        return null;
    }

    fn countMatching(self: *const Queue, endpoint_id: u64, grants_reply: bool) usize {
        var count: usize = 0;
        var offset: usize = 0;
        while (offset < self.len) : (offset += 1) {
            const msg = (self.slotAtConst(offset) orelse return count).*;
            if (msg.endpoint_id == endpoint_id and msg.grants_reply == grants_reply) count += 1;
        }
        return count;
    }

    fn hasEquivalentCoalescibleSignal(self: *const Queue, msg: Message) bool {
        if (!isCoalescibleSignal(msg)) return false;
        var offset: usize = 0;
        while (offset < self.len) : (offset += 1) {
            const queued = (self.slotAtConst(offset) orelse return false).*;
            if (!isCoalescibleSignal(queued)) continue;
            if (queued.endpoint_id == msg.endpoint_id and queued.sender_thread == msg.sender_thread) return true;
        }
        return false;
    }

    fn isCoalescibleSignal(msg: Message) bool {
        return msg.endpoint_id != 0 and
            !msg.grants_reply and
            msg.mr0 == 0 and
            msg.mr1 == 0 and
            msg.mr2 == 0 and
            msg.mr3 == 0;
    }

    fn reset(self: *Queue) void {
        freeQueueChunks(self.overflow_head);
        self.* = .{};
    }

    fn clearRange(self: *Queue, start: usize, end: usize) void {
        var index = start;
        while (index < end) : (index += 1) {
            if (self.slotAt(index)) |slot| slot.* = .{};
        }
    }

    fn slotAt(self: *Queue, index: usize) ?*Message {
        if (index < inline_queue_depth) return &self.entries[index];
        var remaining = index - inline_queue_depth;
        var chunk_index = self.overflow_head;
        while (chunk_index != invalid_chunk) {
            if (remaining < queue_chunk_entries) return &queue_chunk_pool[chunk_index].entries[remaining];
            remaining -= queue_chunk_entries;
            chunk_index = queue_chunk_pool[chunk_index].next;
        }
        return null;
    }

    fn slotAtConst(self: *const Queue, index: usize) ?*const Message {
        if (index < inline_queue_depth) return &self.entries[index];
        var remaining = index - inline_queue_depth;
        var chunk_index = self.overflow_head;
        while (chunk_index != invalid_chunk) {
            if (remaining < queue_chunk_entries) return &queue_chunk_pool[chunk_index].entries[remaining];
            remaining -= queue_chunk_entries;
            chunk_index = queue_chunk_pool[chunk_index].next;
        }
        return null;
    }

    fn slotAtOrAllocate(self: *Queue, index: usize) ?*Message {
        if (index < inline_queue_depth) return &self.entries[index];
        const needed_chunk_offset = (index - inline_queue_depth) / queue_chunk_entries;
        if (needed_chunk_offset >= queue_chunk_pool_count) return null;
        if (self.overflow_head == invalid_chunk) self.overflow_head = allocQueueChunk() orelse return null;

        var chunk_index = self.overflow_head;
        var offset: usize = 0;
        while (offset < needed_chunk_offset) : (offset += 1) {
            if (queue_chunk_pool[chunk_index].next == invalid_chunk) {
                queue_chunk_pool[chunk_index].next = allocQueueChunk() orelse return null;
            }
            chunk_index = queue_chunk_pool[chunk_index].next;
        }
        return &queue_chunk_pool[chunk_index].entries[(index - inline_queue_depth) % queue_chunk_entries];
    }

    fn trimUnusedChunks(self: *Queue) void {
        if (self.overflow_head == invalid_chunk) return;
        if (self.len <= inline_queue_depth) {
            freeQueueChunks(self.overflow_head);
            self.overflow_head = invalid_chunk;
            return;
        }
        const needed_chunks = (self.len - inline_queue_depth + queue_chunk_entries - 1) / queue_chunk_entries;
        var chunk_index = self.overflow_head;
        var kept: usize = 1;
        while (kept < needed_chunks and chunk_index != invalid_chunk) : (kept += 1) {
            chunk_index = queue_chunk_pool[chunk_index].next;
        }
        if (chunk_index == invalid_chunk) return;
        const free_head = queue_chunk_pool[chunk_index].next;
        queue_chunk_pool[chunk_index].next = invalid_chunk;
        freeQueueChunks(free_head);
    }
};

const QueueChunk = struct {
    used: bool = false,
    next: u16 = invalid_chunk,
    entries: [queue_chunk_entries]Message = [_]Message{.{}} ** queue_chunk_entries,
};

var queue_chunk_pool: [queue_chunk_pool_count]QueueChunk = [_]QueueChunk{.{}} ** queue_chunk_pool_count;

fn allocQueueChunk() ?u16 {
    var i: usize = 0;
    while (i < queue_chunk_pool.len) : (i += 1) {
        if (queue_chunk_pool[i].used) continue;
        queue_chunk_pool[i] = .{ .used = true };
        return @intCast(i);
    }
    return null;
}

fn freeQueueChunks(head: u16) void {
    var chunk_index = head;
    while (chunk_index != invalid_chunk) {
        const next = queue_chunk_pool[chunk_index].next;
        queue_chunk_pool[chunk_index] = .{};
        chunk_index = next;
    }
}

fn buildInitialQueues() [max_thread_slots]Queue {
    var queues: [max_thread_slots]Queue = undefined;
    inline for (0..max_thread_slots) |i| {
        queues[i] = .{};
    }
    return queues;
}

var ipc_queues: [max_thread_slots]Queue = buildInitialQueues();
var delegate_send_queues: [max_thread_slots]Queue = buildInitialQueues();

fn staticStorageEnd(comptime T: type, ptr: *T) usize {
    return @intFromPtr(ptr) + @sizeOf(T);
}

fn maxStaticEnd(a: usize, b: usize) usize {
    return if (a > b) a else b;
}

pub fn kernelStaticStorageEndAddr() usize {
    var end: usize = 0;
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(ipc_queues), &ipc_queues));
    end = maxStaticEnd(end, staticStorageEnd(@TypeOf(delegate_send_queues), &delegate_send_queues));
    return end;
}

pub fn resetIpc(thread_index: usize) void {
    if (thread_index >= max_thread_slots) return;
    ipc_queues[thread_index].reset();
}

pub fn resetDelegate(thread_index: usize) void {
    if (thread_index >= max_thread_slots) return;
    delegate_send_queues[thread_index].reset();
}

pub fn purgeThread(thread_index: usize) void {
    if (thread_index >= max_thread_slots) return;
    resetIpc(thread_index);
    resetDelegate(thread_index);
    var i: usize = 0;
    while (i < max_thread_slots) : (i += 1) {
        ipc_queues[i].removeFromSender(thread_index);
        delegate_send_queues[i].removeFromSender(thread_index);
    }
}

pub fn enqueueDelegate(target_thread: usize, msg: Message) bool {
    if (target_thread >= max_thread_slots) return false;
    return delegate_send_queues[target_thread].enqueue(msg);
}

pub fn dequeueDelegate(target_thread: usize) ?Message {
    if (target_thread >= max_thread_slots) return null;
    return delegate_send_queues[target_thread].dequeue();
}

pub fn restoreDelegateFront(target_thread: usize, msg: Message) bool {
    if (target_thread >= max_thread_slots) return false;
    return delegate_send_queues[target_thread].pushFront(msg);
}

pub fn delegateLen(thread_index: usize) usize {
    if (thread_index >= max_thread_slots) return 0;
    return delegate_send_queues[thread_index].len;
}

pub fn enqueueIpc(target_thread: usize, msg: Message) bool {
    if (target_thread >= max_thread_slots) return false;
    return ipc_queues[target_thread].enqueue(msg);
}

pub fn dequeueIpc(thread_index: usize) ?Message {
    if (thread_index >= max_thread_slots) return null;
    return ipc_queues[thread_index].dequeue();
}

pub fn dequeueReplyFromSender(thread_index: usize, sender_thread: usize) ?Message {
    if (thread_index >= max_thread_slots or sender_thread >= max_thread_slots) return null;
    return ipc_queues[thread_index].dequeueReplyFromSender(sender_thread);
}

pub fn discardReplyFromSender(thread_index: usize, sender_thread: usize) usize {
    if (thread_index >= max_thread_slots or sender_thread >= max_thread_slots) return 0;
    return ipc_queues[thread_index].removeMatching(sender_thread, 0, false);
}

pub fn discardFromSenderOnEndpoint(thread_index: usize, sender_thread: usize, endpoint_id: u64, grants_reply: bool) usize {
    if (thread_index >= max_thread_slots or sender_thread >= max_thread_slots) return 0;
    return ipc_queues[thread_index].removeMatching(sender_thread, endpoint_id, grants_reply);
}

pub fn ipcLen(thread_index: usize) usize {
    if (thread_index >= max_thread_slots) return 0;
    return ipc_queues[thread_index].len;
}

pub fn ipcLenOnEndpoint(thread_index: usize, endpoint_id: u64, grants_reply: bool) usize {
    if (thread_index >= max_thread_slots) return 0;
    return ipc_queues[thread_index].countMatching(endpoint_id, grants_reply);
}
