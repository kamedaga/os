const block_client = @import("block_client.zig");
const user_vm = @import("user_vm.zig");

const syscall_alloc_page: u64 = 0x1;
const syscall_map_page: u64 = 0x2;
const syscall_log: u64 = 0x9;
const block_cache_invalid_tag: u64 = ~@as(u64, 0);

pub const InitOptions = struct {
    request_va: u64 = 0,
    response_va: u64 = 0,
    cache_tags_va: u64 = 0,
    cache_data_base_va: u64 = 0,
    bulk_read_va: u64 = 0,
    max_block_bytes: usize,
    cache_entries: usize,
    response_poll_limit: u64,
};

pub const Store = struct {
    client: block_client.Client = undefined,
    ready: bool = false,
    block_size: u64 = 0,
    capacity_blocks: u64 = 0,
    cache_tags_va: u64 = 0,
    cache_data_base_va: u64 = 0,
    max_block_bytes: usize = 0,
    cache_entries: usize = 0,

    pub fn init(self: *Store, client_process_slot: u64, options: InitOptions) bool {
        if (options.max_block_bytes == 0 or options.cache_entries == 0) return false;

        self.client = block_client.Client.connectFromServiceRegistryOptions(options.request_va, options.response_va, client_process_slot, .{
            .response_poll_limit = options.response_poll_limit,
            .bulk_read_va = options.bulk_read_va,
        }) catch |err| {
            userLogError("PersistentVolumeStore: block connect err=", err);
            return false;
        };

        if (self.client.block_size == 0 or
            self.client.block_size > options.max_block_bytes or
            self.client.capacity_blocks == 0)
        {
            return false;
        }

        self.block_size = self.client.block_size;
        self.capacity_blocks = self.client.capacity_blocks;
        self.cache_tags_va = options.cache_tags_va;
        self.cache_data_base_va = options.cache_data_base_va;
        self.max_block_bytes = options.max_block_bytes;
        self.cache_entries = options.cache_entries;
        if (!self.initCache()) return false;

        self.ready = true;
        return true;
    }

    pub fn readBlock(self: *Store, block_index: u64, out: []u8) bool {
        const block_len = self.blockLen(out.len) orelse return false;
        const block = out[0..block_len];
        if (self.cacheRead(block_index, block)) return true;
        _ = self.client.readBlocks(block_index, block) catch return false;
        self.cacheStore(block_index, block);
        return true;
    }

    pub fn writeBlock(self: *Store, block_index: u64, bytes: []const u8) bool {
        const block_len = self.blockLen(bytes.len) orelse return false;
        const block = bytes[0..block_len];
        self.client.writeBlocks(block_index, block) catch return false;
        self.cacheStore(block_index, block);
        return true;
    }

    pub fn flush(self: *Store) bool {
        if (!self.ready) return false;
        self.client.flush() catch return false;
        return true;
    }

    fn blockLen(self: *const Store, len: usize) ?usize {
        if (self.block_size == 0 or self.block_size > self.max_block_bytes) return null;
        const block_len: usize = @intCast(self.block_size);
        if (len < block_len) return null;
        return block_len;
    }

    fn initCache(self: *Store) bool {
        if (self.cache_tags_va == 0) {
            const page = user_vm.allocMapPage(true) orelse return false;
            self.cache_tags_va = @intCast(page.va);
            clearPage(self.cache_tags_va);
        } else if (!mapScratchPage(self.cache_tags_va)) return false;
        var index: usize = 0;
        while (index < self.cache_entries) : (index += 1) {
            self.cacheTagPtr(index).* = block_cache_invalid_tag;
        }
        if (self.cache_data_base_va == 0) {
            if (self.max_block_bytes > user_vm.page_bytes) return false;
            self.cache_data_base_va = @intCast(user_vm.allocMapPages(self.cache_entries, true) orelse return false);
            index = 0;
            while (index < self.cache_entries) : (index += 1) clearPage(self.cache_data_base_va + @as(u64, @intCast(index * user_vm.page_bytes)));
        } else {
            index = 0;
            while (index < self.cache_entries) : (index += 1) {
                const data_va = self.cache_data_base_va + @as(u64, @intCast(index * self.max_block_bytes));
                if (!mapScratchPage(data_va)) return false;
            }
        }
        return true;
    }

    fn cacheIndex(self: *const Store, block_index: u64) usize {
        return @intCast(block_index % self.cache_entries);
    }

    fn cacheRead(self: *const Store, block_index: u64, out: []u8) bool {
        if (out.len == 0 or out.len > self.max_block_bytes) return false;
        const index = self.cacheIndex(block_index);
        if (self.cacheTagPtr(index).* != block_index) return false;
        copyVolatileBytes(self.cacheData(index), out);
        return true;
    }

    fn cacheStore(self: *const Store, block_index: u64, bytes: []const u8) void {
        if (bytes.len == 0 or bytes.len > self.max_block_bytes) return;
        const index = self.cacheIndex(block_index);
        copyBytesToVolatile(self.cacheData(index), bytes);
        self.cacheTagPtr(index).* = block_index;
    }

    fn cacheData(self: *const Store, index: usize) [*]volatile u8 {
        return @ptrFromInt(self.cache_data_base_va + @as(u64, @intCast(index * self.max_block_bytes)));
    }

    fn cacheTagPtr(self: *const Store, index: usize) *volatile u64 {
        return @ptrFromInt(self.cache_tags_va + @as(u64, @intCast(index * @sizeOf(u64))));
    }
};

fn mapScratchPage(va: u64) bool {
    const paddr = allocPage();
    if (paddr < 0x1000) return false;
    if (mapPage(va, paddr, true) != 0) return false;
    clearPage(va);
    return true;
}

fn allocPage() u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_alloc_page),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_page),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn clearPage(base_va: u64) void {
    const ptr: [*]volatile u64 = @ptrFromInt(base_va);
    var i: usize = 0;
    while (i < 512) : (i += 1) {
        ptr[i] = 0;
    }
}

fn copyBytesToVolatile(dest: [*]volatile u8, src: []const u8) void {
    var i: usize = 0;
    while (i < src.len) : (i += 1) {
        dest[i] = src[i];
    }
}

fn copyVolatileBytes(src: [*]volatile u8, dest: []u8) void {
    var i: usize = 0;
    while (i < dest.len) : (i += 1) {
        dest[i] = src[i];
    }
}

fn userLogError(prefix: []const u8, err: anyerror) void {
    var buf: [128]u8 = undefined;
    const msg = @import("std").fmt.bufPrint(&buf, "{s}{s}\n", .{ prefix, @errorName(err) }) catch return;
    _ = userLog(msg);
}

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\syscall
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}
