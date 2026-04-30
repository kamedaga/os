const std = @import("std");
const fs_abi = @import("fs_abi.zig");
const block_protocol = @import("block_protocol.zig");
const service_registry_abi = @import("service_registry_abi.zig");
const user_vm = @import("user_vm.zig");

const syscall_alloc_page: u64 = 0x1;
const syscall_map_page: u64 = 0x2;
const syscall_send_cap: u64 = 0x6;
const syscall_grant_cap: u64 = 0x8;
const syscall_log: u64 = 0x9;
const syscall_alloc_map_pages: u64 = 0xC;
const syscall_wait_event: u64 = 0x17;
const syscall_grant_cap_on_endpoint: u64 = 0x24;
const syscall_install_endpoint: u64 = 0x26;
const syscall_share_cap: u64 = 0x2B;
const syscall_signal_endpoint: u64 = 0x2C;
const syscall_ok: u64 = 0;
const syscall_err_endpoint: u64 = 9;

const page_right_cpu_read: u64 = 0x1;
const page_right_cpu_write: u64 = 0x2;
const default_response_poll_limit: u64 = 256;
const default_bulk_read_va_gap: u64 = 0x20_000;
const page_bytes: usize = block_protocol.page_bytes;
const bulk_read_pages: usize = 16;
const bulk_read_bytes: usize = bulk_read_pages * page_bytes;

pub const Error = error{
    RequestAllocFailed,
    RequestMapFailed,
    ResponseAllocFailed,
    ResponseMapFailed,
    BulkBufferAllocFailed,
    BulkBufferGrantFailed,
    EndpointNotFound,
    EndpointInstallFailed,
    ResponseGrantFailed,
    ConnectSendFailed,
    Timeout,
    InvalidResponse,
    BufferTooSmall,
    Invalid,
    NotFound,
    NoRight,
    TooBig,
    IoError,
    Busy,
};

pub const ConnectOptions = struct {
    request_va: u64 = 0,
    response_va: u64 = 0,
    client_process_slot: u64,
    endpoint_id: u64,
    server_process_slot: u64 = 0,
    response_poll_limit: u64 = default_response_poll_limit,
    compat_process_slot: u64 = 0,
    allow_process_slot_compat: bool = false,
    bulk_read_va: u64 = 0,
};

pub const RegistryConnectOptions = struct {
    response_poll_limit: u64 = default_response_poll_limit,
    allow_process_slot_compat: bool = true,
    bulk_read_va: u64 = 0,
};

pub const IdentifyResult = struct {
    block_size: u64,
    capacity_blocks: u64,
};

pub const Client = struct {
    request_va: u64,
    response_va: u64,
    request_paddr: u64,
    response_paddr: u64,
    bulk_read_va: u64,
    server_endpoint_id: u64,
    server_process_slot: u64,
    session_nonce: u64,
    root_token: u64,
    block_size: u64,
    capacity_blocks: u64,
    next_seq: u64 = 2,
    response_poll_limit: u64 = default_response_poll_limit,
    bulk_read_ready: bool = false,
    bulk_read_disabled: bool = false,
    bulk_read_paddrs: [bulk_read_pages]u64 = [_]u64{0} ** bulk_read_pages,

    pub fn connect(options: ConnectOptions) Error!Client {
        const pages = try allocConnectPages(options.request_va, options.response_va);
        var compat_installed = false;
        try grantResponseCapForConnect(pages.response_paddr, options, &compat_installed);
        const session_nonce = makeSessionNonce(pages.request_paddr, pages.response_paddr, options.endpoint_id, options.client_process_slot);

        var client = Client{
            .request_va = pages.request_va,
            .response_va = pages.response_va,
            .request_paddr = pages.request_paddr,
            .response_paddr = pages.response_paddr,
            .bulk_read_va = options.bulk_read_va,
            .server_endpoint_id = options.endpoint_id,
            .server_process_slot = options.server_process_slot,
            .session_nonce = session_nonce,
            .root_token = 0,
            .block_size = 0,
            .capacity_blocks = 0,
            .response_poll_limit = options.response_poll_limit,
        };

        client.clearMappedPages();
        const request = client.requestHeader();
        request.magic = block_protocol.request_magic;
        request.version = block_protocol.version;
        request.op = block_protocol.opcodeRaw(.connect);
        request.object_token = 0;
        request.block_index = 0;
        request.block_count = 0;
        request.flags = 0;
        request.inline_bytes = 0;
        request.reserved0 = 0;
        request.reserved1 = 0;
        request.arg0 = pages.response_paddr;
        request.arg1 = options.client_process_slot;
        request.session_nonce = session_nonce;
        compilerBarrier();
        request.request_seq = 1;

        try shareConnectRequest(pages.request_paddr, options, &compat_installed);
        const response = try client.finishRequestOk(1, .connect);
        client.root_token = response.result_token;
        if (!fs_abi.isCapToken(client.root_token)) return error.InvalidResponse;
        client.block_size = response.arg0;
        client.capacity_blocks = response.arg1;
        if (client.block_size == 0 or client.capacity_blocks == 0) return error.InvalidResponse;
        return client;
    }

    pub fn connectFromRegistryPage(registry_page_va: u64, request_va: u64, response_va: u64, client_process_slot: u64) Error!Client {
        return connectFromRegistryPageOptions(registry_page_va, request_va, response_va, client_process_slot, .{});
    }

    pub fn connectFromRegistryPageOptions(
        registry_page_va: u64,
        request_va: u64,
        response_va: u64,
        client_process_slot: u64,
        options: RegistryConnectOptions,
    ) Error!Client {
        const entry = service_registry_abi.findService(registry_page_va, .block) orelse return error.NotFound;
        if (entry.endpoint_id == 0) return error.Invalid;
        const compat_allowed = service_registry_abi.allowsProcessSlotCompat(entry);
        const allow_process_slot_compat = options.allow_process_slot_compat and compat_allowed;
        return connect(.{
            .request_va = request_va,
            .response_va = response_va,
            .client_process_slot = client_process_slot,
            .endpoint_id = entry.endpoint_id,
            .server_process_slot = entry.process_slot,
            .response_poll_limit = options.response_poll_limit,
            .compat_process_slot = if (compat_allowed) entry.process_slot else 0,
            .allow_process_slot_compat = allow_process_slot_compat,
            .bulk_read_va = options.bulk_read_va,
        }) catch |err| {
            logRegistryConnectFailure(registry_page_va, entry, compat_allowed, allow_process_slot_compat, err);
            return err;
        };
    }

    pub fn connectFromServiceRegistry(request_va: u64, response_va: u64, client_process_slot: u64) Error!Client {
        return connectFromServiceRegistryOptions(request_va, response_va, client_process_slot, .{});
    }

    pub fn connectFromServiceRegistryOptions(
        request_va: u64,
        response_va: u64,
        client_process_slot: u64,
        options: RegistryConnectOptions,
    ) Error!Client {
        return connectFromRegistryPageOptions(service_registry_abi.page_va, request_va, response_va, client_process_slot, options);
    }

    pub fn identify(self: *Client) Error!IdentifyResult {
        const seq = try self.beginRequest(.identify, self.root_token, 0, 0, 0, &[_]u8{});
        const response = try self.finishRequestOk(seq, .identify);
        if (response.arg0 == 0 or response.arg1 == 0) return error.InvalidResponse;
        self.block_size = response.arg0;
        self.capacity_blocks = response.arg1;
        return .{
            .block_size = response.arg0,
            .capacity_blocks = response.arg1,
        };
    }

    pub fn readBlocks(self: *Client, block_index: u64, out: []u8) Error!usize {
        if (self.block_size == 0) return error.Invalid;
        if ((out.len % self.block_size) != 0) return error.BufferTooSmall;
        const block_count: usize = out.len / @as(usize, @intCast(self.block_size));
        if (block_count == 0 or block_count > std.math.maxInt(u32)) return error.BufferTooSmall;
        if (out.len > block_protocol.response_payload_bytes and self.server_process_slot != 0 and !self.bulk_read_disabled) {
            return self.readBlocksBulk(block_index, out) catch {
                self.bulk_read_disabled = true;
                self.bulk_read_ready = false;
                return self.readBlocksInlineLoop(block_index, out);
            };
        }
        return self.readBlocksInlineLoop(block_index, out);
    }

    fn readBlocksInlineLoop(self: *Client, block_index: u64, out: []u8) Error!usize {
        const block_bytes: usize = @intCast(self.block_size);
        const max_inline_bytes = (block_protocol.response_payload_bytes / block_bytes) * block_bytes;
        if (max_inline_bytes == 0) return error.BufferTooSmall;
        var copied: usize = 0;
        var current_block = block_index;
        while (copied < out.len) {
            const remaining = out.len - copied;
            const chunk_bytes = @min(remaining, max_inline_bytes);
            const aligned_chunk_bytes = chunk_bytes - (chunk_bytes % block_bytes);
            if (aligned_chunk_bytes == 0) return error.BufferTooSmall;
            const chunk_blocks: u32 = @intCast(aligned_chunk_bytes / block_bytes);
            const bytes_read = try self.readBlocksInline(current_block, chunk_blocks, out[copied .. copied + aligned_chunk_bytes]);
            if (bytes_read != aligned_chunk_bytes) return error.InvalidResponse;
            copied += aligned_chunk_bytes;
            current_block += chunk_blocks;
        }
        return copied;
    }

    fn readBlocksInline(self: *Client, block_index: u64, block_count: u32, out: []u8) Error!usize {
        const seq = try self.beginRequest(.read_blocks, self.root_token, block_index, @intCast(block_count), 0, &[_]u8{});
        const response = try self.finishRequestOk(seq, .read_blocks);
        const inline_bytes: usize = response.inline_bytes;
        if (inline_bytes != out.len or inline_bytes > block_protocol.response_payload_bytes) return error.InvalidResponse;
        copyVolatileBytes(self.responsePayload(), out[0..inline_bytes]);
        return inline_bytes;
    }

    fn readBlocksBulk(self: *Client, block_index: u64, out: []u8) Error!usize {
        try self.ensureBulkReadBuffer();
        const block_bytes: usize = @intCast(self.block_size);
        var copied: usize = 0;
        var current_block = block_index;
        while (copied < out.len) {
            const remaining = out.len - copied;
            const chunk_bytes = @min(remaining, bulk_read_bytes);
            const aligned_chunk_bytes = chunk_bytes - (chunk_bytes % block_bytes);
            if (aligned_chunk_bytes == 0) return error.BufferTooSmall;
            const page_count = (aligned_chunk_bytes + page_bytes - 1) / page_bytes;
            const payload = std.mem.sliceAsBytes(self.bulk_read_paddrs[0..page_count]);
            const chunk_blocks: u32 = @intCast(aligned_chunk_bytes / block_bytes);
            const seq = try self.beginRequest(.read_blocks_bulk, self.root_token, current_block, chunk_blocks, @intCast(page_count), payload);
            const response = try self.finishRequestOk(seq, .read_blocks_bulk);
            const bytes_read: usize = @intCast(response.arg0);
            if (bytes_read != aligned_chunk_bytes) return error.InvalidResponse;
            copyPlainBytes(@ptrFromInt(self.bulk_read_va), out[copied .. copied + aligned_chunk_bytes]);
            copied += aligned_chunk_bytes;
            current_block += chunk_blocks;
        }
        return copied;
    }

    fn ensureBulkReadBuffer(self: *Client) Error!void {
        if (self.bulk_read_ready) return;
        if (self.server_process_slot == 0) return error.Invalid;
        if (self.bulk_read_va == 0) {
            self.bulk_read_va = @intCast(user_vm.allocMapPagesInto(bulk_read_pages, true, self.bulk_read_paddrs[0..]) orelse {
                return error.BulkBufferAllocFailed;
            });
        } else if (allocMapPages(self.bulk_read_va, bulk_read_pages, true, @intFromPtr(&self.bulk_read_paddrs)) != syscall_ok) {
            return error.BulkBufferAllocFailed;
        }
        for (self.bulk_read_paddrs) |paddr| {
            if (paddr < 0x1000 or grantCap(paddr, self.server_process_slot, page_right_cpu_write) != syscall_ok) {
                return error.BulkBufferGrantFailed;
            }
        }
        self.bulk_read_ready = true;
    }

    pub fn writeBlocks(self: *Client, block_index: u64, bytes: []const u8) Error!void {
        if (self.block_size == 0) return error.Invalid;
        if ((bytes.len % self.block_size) != 0) return error.BufferTooSmall;
        const block_count: usize = bytes.len / @as(usize, @intCast(self.block_size));
        if (bytes.len > block_protocol.request_payload_bytes or block_count == 0 or block_count > std.math.maxInt(u32)) {
            return error.BufferTooSmall;
        }
        const seq = try self.beginRequest(.write_blocks, self.root_token, block_index, @intCast(block_count), 0, bytes);
        _ = try self.finishRequestOk(seq, .write_blocks);
    }

    pub fn flush(self: *Client) Error!void {
        const seq = try self.beginRequest(.flush, self.root_token, 0, 0, 0, &[_]u8{});
        _ = try self.finishRequestOk(seq, .flush);
    }

    fn requestHeader(self: *const Client) *volatile block_protocol.BlockRequestHeader {
        return @ptrFromInt(self.request_va);
    }

    fn responseHeader(self: *const Client) *volatile block_protocol.BlockResponseHeader {
        return @ptrFromInt(self.response_va);
    }

    fn requestPayload(self: *const Client) [*]volatile u8 {
        return @ptrFromInt(self.request_va + block_protocol.request_header_bytes);
    }

    fn responsePayload(self: *const Client) [*]volatile u8 {
        return @ptrFromInt(self.response_va + block_protocol.response_header_bytes);
    }

    fn clearMappedPages(self: *const Client) void {
        clearPage(self.request_va);
        clearPage(self.response_va);
    }

    fn beginRequest(
        self: *Client,
        op: block_protocol.Opcode,
        object_token: u64,
        block_index: u64,
        block_count: u32,
        flags: u32,
        payload: []const u8,
    ) Error!u64 {
        if (payload.len > block_protocol.request_payload_bytes) return error.BufferTooSmall;
        self.clearMappedPages();
        const request = self.requestHeader();
        request.magic = block_protocol.request_magic;
        request.version = block_protocol.version;
        request.op = block_protocol.opcodeRaw(op);
        request.object_token = object_token;
        request.block_index = block_index;
        request.block_count = block_count;
        request.flags = flags;
        request.inline_bytes = @intCast(payload.len);
        request.reserved0 = 0;
        request.reserved1 = 0;
        request.arg0 = 0;
        request.arg1 = 0;
        request.session_nonce = self.session_nonce;
        copyBytesToVolatile(self.requestPayload(), payload);
        const seq = self.next_seq;
        self.next_seq += 1;
        compilerBarrier();
        request.request_seq = seq;
        _ = signalEndpoint(self.server_endpoint_id);
        return seq;
    }

    fn finishRequest(self: *Client, expected_seq: u64, expected_op: block_protocol.Opcode) Error!*volatile block_protocol.BlockResponseHeader {
        if (!self.waitForResponse(expected_seq)) return error.Timeout;
        const response = self.responseHeader();
        if (response.magic != block_protocol.response_magic or response.version != block_protocol.version) return error.InvalidResponse;
        if (response.op != block_protocol.opcodeRaw(expected_op) or response.response_seq != expected_seq) return error.InvalidResponse;
        _ = parseStatus(response.status) orelse return error.InvalidResponse;
        return response;
    }

    fn finishRequestOk(self: *Client, expected_seq: u64, expected_op: block_protocol.Opcode) Error!*volatile block_protocol.BlockResponseHeader {
        const response = try self.finishRequest(expected_seq, expected_op);
        const status = parseStatus(response.status) orelse return error.InvalidResponse;
        if (status != .ok) return statusToError(status);
        return response;
    }

    fn waitForResponse(self: *const Client, expected_seq: u64) bool {
        const response = self.responseHeader();
        var poll_count: u64 = 0;
        while (poll_count < self.response_poll_limit) : (poll_count += 1) {
            if (response.response_seq == expected_seq) return true;
            _ = waitEvent(false, 1);
        }
        return false;
    }
};

fn makeSessionNonce(request_paddr: u64, response_paddr: u64, endpoint_id: u64, tag: u64) u64 {
    const nonce = request_paddr ^ ((response_paddr << 17) | (response_paddr >> 47)) ^ ((endpoint_id << 7) | (endpoint_id >> 57)) ^ tag ^ 0x517c_c1b7_2722_0a95;
    return if (nonce == 0) 1 else nonce;
}

const ConnectPages = struct {
    request_va: u64,
    response_va: u64,
    request_paddr: u64,
    response_paddr: u64,
};

fn allocConnectPages(request_va: u64, response_va: u64) Error!ConnectPages {
    if (request_va == 0 and response_va == 0) {
        var paddrs: [2]u64 = .{ 0, 0 };
        const base_va = user_vm.allocMapPagesInto(2, true, paddrs[0..]) orelse return error.RequestAllocFailed;
        if (paddrs[0] < 0x1000 or paddrs[1] < 0x1000) return error.RequestAllocFailed;
        return .{
            .request_va = @intCast(base_va),
            .response_va = @intCast(base_va + user_vm.page_bytes),
            .request_paddr = paddrs[0],
            .response_paddr = paddrs[1],
        };
    }
    if (request_va == 0 or response_va == 0) return error.Invalid;
    if (response_va == request_va + 4096) {
        var paddrs: [2]u64 = .{ 0, 0 };
        if (allocMapPages(request_va, 2, true, @intFromPtr(&paddrs)) == syscall_ok and
            paddrs[0] >= 0x1000 and paddrs[1] >= 0x1000)
        {
            return .{
                .request_va = request_va,
                .response_va = response_va,
                .request_paddr = paddrs[0],
                .response_paddr = paddrs[1],
            };
        }
    }

    const request_paddr = allocPage();
    if (request_paddr < 0x1000) return error.RequestAllocFailed;
    if (mapPage(request_va, request_paddr, true) != 0) return error.RequestMapFailed;

    const response_paddr = allocPage();
    if (response_paddr < 0x1000) return error.ResponseAllocFailed;
    if (mapPage(response_va, response_paddr, true) != 0) return error.ResponseMapFailed;
    return .{
        .request_va = request_va,
        .response_va = response_va,
        .request_paddr = request_paddr,
        .response_paddr = response_paddr,
    };
}

fn allocPage() u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_alloc_page),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn mapPage(va: u64, paddr: u64, writable: bool) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_map_page),
          [arg0] "{rdi}" (va),
          [arg1] "{rsi}" (paddr),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn allocMapPages(base_va: u64, page_count: u64, writable: bool, out_paddr_list_va: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_alloc_map_pages),
          [arg0] "{rdi}" (base_va),
          [arg1] "{rsi}" (page_count),
          [arg2] "{rdx}" (@as(u64, if (writable) 1 else 0)),
          [arg3] "{rcx}" (out_paddr_list_va),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn sendCap(paddr: u64, endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_send_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (endpoint_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn shareCap(paddr: u64, endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_share_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (endpoint_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn signalEndpoint(endpoint_id: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_signal_endpoint),
          [arg0] "{rdi}" (endpoint_id),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantCapOnEndpoint(paddr: u64, endpoint_id: u64, rights: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_grant_cap_on_endpoint),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (endpoint_id),
          [arg2] "{rdx}" (rights),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantCap(paddr: u64, to_process_slot: u64, rights: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_grant_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (to_process_slot),
          [arg2] "{rdx}" (rights),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn grantResponseCapForConnect(paddr: u64, options: ConnectOptions, compat_installed: *bool) Error!void {
    const result = grantCapOnEndpoint(paddr, options.endpoint_id, page_right_cpu_read | page_right_cpu_write);
    if (result == syscall_ok) return;
    if (result != syscall_err_endpoint) return error.ResponseGrantFailed;

    if (!compat_installed.* and try attemptCompatEndpointInstall(options)) {
        compat_installed.* = true;
        const retry = grantCapOnEndpoint(paddr, options.endpoint_id, page_right_cpu_read | page_right_cpu_write);
        if (retry == syscall_ok) return;
        if (retry == syscall_err_endpoint) return error.EndpointNotFound;
        return error.ResponseGrantFailed;
    }
    return error.EndpointNotFound;
}

fn shareConnectRequest(paddr: u64, options: ConnectOptions, compat_installed: *bool) Error!void {
    const result = shareCap(paddr, options.endpoint_id);
    if (result == syscall_ok) return;
    if (result != syscall_err_endpoint) return error.ConnectSendFailed;

    if (!compat_installed.* and try attemptCompatEndpointInstall(options)) {
        compat_installed.* = true;
        const retry = shareCap(paddr, options.endpoint_id);
        if (retry == syscall_ok) return;
        if (retry == syscall_err_endpoint) return error.EndpointNotFound;
        return error.ConnectSendFailed;
    }
    return error.EndpointNotFound;
}

fn attemptCompatEndpointInstall(options: ConnectOptions) Error!bool {
    if (!options.allow_process_slot_compat or options.compat_process_slot == 0) return false;
    _ = userLog("block_client: process-slot compat fallback\n");
    return switch (installEndpoint(options.endpoint_id, options.compat_process_slot)) {
        syscall_ok => true,
        else => error.EndpointInstallFailed,
    };
}

fn installEndpoint(endpoint_id: u64, target_process_slot: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_install_endpoint),
          [arg0] "{rdi}" (@as(u64, 0)),
          [arg1] "{rsi}" (endpoint_id),
          [arg2] "{rdx}" (target_process_slot),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn logRegistryConnectFailure(
    registry_page_va: u64,
    entry: service_registry_abi.ServiceEntry,
    compat_allowed: bool,
    allow_process_slot_compat: bool,
    err: Error,
) void {
    switch (err) {
        error.EndpointNotFound,
        error.EndpointInstallFailed,
        error.ResponseGrantFailed,
        error.ConnectSendFailed,
        => {},
        else => return,
    }
    var buf: [192]u8 = undefined;
    const line = std.fmt.bufPrint(
        &buf,
        "block_client: registry=0x{x} ep=0x{x} slot={} flags=0x{x} compat={} allow={} err={s}\n",
        .{
            registry_page_va,
            entry.endpoint_id,
            entry.process_slot,
            entry.flags,
            @intFromBool(compat_allowed),
            @intFromBool(allow_process_slot_compat),
            @errorName(err),
        },
    ) catch return;
    _ = userLog(line);
}

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn waitEvent(wait_mailbox: bool, timeout_ticks: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_wait_event),
          [arg0] "{rdi}" (@as(u64, if (wait_mailbox) 1 else 0)),
          [arg1] "{rsi}" (timeout_ticks),
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

fn copyPlainBytes(src: [*]const u8, dest: []u8) void {
    @memcpy(dest, src[0..dest.len]);
}

fn compilerBarrier() void {
    asm volatile ("" ::: .{ .memory = true });
}

fn parseStatus(raw: i32) ?block_protocol.Status {
    return std.meta.intToEnum(block_protocol.Status, raw) catch null;
}

fn statusToError(status: block_protocol.Status) Error {
    return switch (status) {
        .ok => unreachable,
        .invalid => error.Invalid,
        .not_found => error.NotFound,
        .no_right => error.NoRight,
        .too_big => error.TooBig,
        .io_error => error.IoError,
        .busy => error.Busy,
    };
}
