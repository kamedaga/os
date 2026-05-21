const kernel = @import("kernel.zig");

pub const max_thread_slots: usize = kernel.max_thread_slots;
pub const max_queue_depth: usize = 128;

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
    entries: [max_queue_depth]Message = [_]Message{.{}} ** max_queue_depth,
    head: u8 = 0,
    len: u8 = 0,

    fn enqueue(self: *Queue, msg: Message) bool {
        if (self.len >= max_queue_depth) return false;
        const tail = (@as(usize, self.head) + @as(usize, self.len)) % max_queue_depth;
        self.entries[tail] = msg;
        self.len += 1;
        return true;
    }

    fn dequeue(self: *Queue) ?Message {
        if (self.len == 0) return null;
        const index: usize = self.head;
        const msg = self.entries[index];
        self.entries[index] = .{};
        self.head = @intCast((@as(usize, self.head) + 1) % max_queue_depth);
        self.len -= 1;
        return msg;
    }

    fn pushFront(self: *Queue, msg: Message) bool {
        if (self.len >= max_queue_depth) return false;
        const head = @as(usize, self.head);
        self.head = @intCast((head + max_queue_depth - 1) % max_queue_depth);
        self.entries[self.head] = msg;
        self.len += 1;
        return true;
    }

    fn removeFromSender(self: *Queue, sender_thread: usize) void {
        var compacted: Queue = .{};
        var index: usize = 0;
        while (index < self.len) : (index += 1) {
            const source_index = (@as(usize, self.head) + index) % max_queue_depth;
            const msg = self.entries[source_index];
            if (msg.sender_thread == sender_thread) continue;
            _ = compacted.enqueue(msg);
        }
        self.* = compacted;
    }

    fn removeMatching(self: *Queue, sender_thread: usize, endpoint_id: u64, grants_reply: bool) usize {
        var compacted: Queue = .{};
        var removed: usize = 0;
        var offset: usize = 0;
        while (offset < self.len) : (offset += 1) {
            const index = (@as(usize, self.head) + offset) % max_queue_depth;
            const msg = self.entries[index];
            if (msg.sender_thread == sender_thread and msg.endpoint_id == endpoint_id and msg.grants_reply == grants_reply) {
                removed += 1;
                continue;
            }
            _ = compacted.enqueue(msg);
        }
        self.* = compacted;
        return removed;
    }

    fn dequeueReplyFromSender(self: *Queue, sender_thread: usize) ?Message {
        if (self.len == 0) return null;
        var offset: usize = 0;
        while (offset < self.len) : (offset += 1) {
            const index = (@as(usize, self.head) + offset) % max_queue_depth;
            const msg = self.entries[index];
            if (msg.sender_thread != sender_thread or msg.endpoint_id != 0 or msg.grants_reply) continue;

            var shift = offset;
            while (shift + 1 < self.len) : (shift += 1) {
                const dst = (@as(usize, self.head) + shift) % max_queue_depth;
                const src = (@as(usize, self.head) + shift + 1) % max_queue_depth;
                self.entries[dst] = self.entries[src];
            }
            const tail = (@as(usize, self.head) + @as(usize, self.len) - 1) % max_queue_depth;
            self.entries[tail] = .{};
            self.len -= 1;
            return msg;
        }
        return null;
    }

    fn countMatching(self: *const Queue, endpoint_id: u64, grants_reply: bool) usize {
        var count: usize = 0;
        var offset: usize = 0;
        while (offset < self.len) : (offset += 1) {
            const index = (@as(usize, self.head) + offset) % max_queue_depth;
            const msg = self.entries[index];
            if (msg.endpoint_id == endpoint_id and msg.grants_reply == grants_reply) count += 1;
        }
        return count;
    }
};

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
    ipc_queues[thread_index] = .{};
}

pub fn resetDelegate(thread_index: usize) void {
    if (thread_index >= max_thread_slots) return;
    delegate_send_queues[thread_index] = .{};
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
