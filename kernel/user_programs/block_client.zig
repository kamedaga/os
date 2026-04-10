const std = @import("std");
const fs_abi = @import("fs_abi.zig");
const block_protocol = @import("block_protocol.zig");
const service_registry_abi = @import("service_registry_abi.zig");

const syscall_alloc_page: u64 = 0x1;
const syscall_map_page: u64 = 0x2;
const syscall_send_cap: u64 = 0x6;
const syscall_grant_cap: u64 = 0x8;
const syscall_wait_event: u64 = 0x17;
const syscall_install_endpoint: u64 = 0x26;
const syscall_share_cap: u64 = 0x2B;
const syscall_signal_endpoint: u64 = 0x2C;

const page_right_cpu_read: u64 = 0x1;
const page_right_cpu_write: u64 = 0x2;
const default_response_poll_limit: u64 = 256;

pub const Error = error{
    RequestAllocFailed,
    RequestMapFailed,
    ResponseAllocFailed,
    ResponseMapFailed,
    ResponseGrantFailed,
    ConnectSendFailed,
    EndpointInstallFailed,
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
    request_va: u64,
    response_va: u64,
    client_process_slot: u64,
    endpoint_id: u64,
    server_process_slot: u64,
    response_poll_limit: u64 = default_response_poll_limit,
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
    server_endpoint_id: u64,
    root_token: u64,
    block_size: u64,
    capacity_blocks: u64,
    next_seq: u64 = 2,
    response_poll_limit: u64 = default_response_poll_limit,

    pub fn connect(options: ConnectOptions) Error!Client {
        const request_paddr = allocPage();
        if (request_paddr < 0x1000) return error.RequestAllocFailed;
        if (mapPage(options.request_va, request_paddr, true) != 0) return error.RequestMapFailed;

        const response_paddr = allocPage();
        if (response_paddr < 0x1000) return error.ResponseAllocFailed;
        if (mapPage(options.response_va, response_paddr, true) != 0) return error.ResponseMapFailed;
        if (grantCap(response_paddr, options.server_process_slot, page_right_cpu_read | page_right_cpu_write) != 0) {
            return error.ResponseGrantFailed;
        }

        var client = Client{
            .request_va = options.request_va,
            .response_va = options.response_va,
            .request_paddr = request_paddr,
            .response_paddr = response_paddr,
            .server_endpoint_id = options.endpoint_id,
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
        request.arg0 = response_paddr;
        request.arg1 = options.client_process_slot;
        compilerBarrier();
        request.request_seq = 1;

        if (shareCap(request_paddr, options.endpoint_id) != 0) return error.ConnectSendFailed;
        const response = try client.finishRequestOk(1, .connect);
        client.root_token = response.result_token;
        if (!fs_abi.isCapToken(client.root_token)) return error.InvalidResponse;
        client.block_size = response.arg0;
        client.capacity_blocks = response.arg1;
        if (client.block_size == 0 or client.capacity_blocks == 0) return error.InvalidResponse;
        return client;
    }

    pub fn connectFromRegistryPage(registry_page_va: u64, request_va: u64, response_va: u64, client_process_slot: u64) Error!Client {
        const entry = service_registry_abi.findService(registry_page_va, .block) orelse return error.NotFound;
        if (entry.process_slot == 0 or entry.endpoint_id == 0) return error.Invalid;
        if (installEndpoint(entry.endpoint_id, entry.process_slot) != 0) return error.EndpointInstallFailed;
        return connect(.{
            .request_va = request_va,
            .response_va = response_va,
            .client_process_slot = client_process_slot,
            .endpoint_id = entry.endpoint_id,
            .server_process_slot = entry.process_slot,
        });
    }

    pub fn connectFromServiceRegistry(request_va: u64, response_va: u64, client_process_slot: u64) Error!Client {
        return connectFromRegistryPage(service_registry_abi.page_va, request_va, response_va, client_process_slot);
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
        const seq = try self.beginRequest(.read_blocks, self.root_token, block_index, @intCast(block_count), 0, &[_]u8{});
        const response = try self.finishRequestOk(seq, .read_blocks);
        const inline_bytes: usize = response.inline_bytes;
        if (inline_bytes != out.len or inline_bytes > block_protocol.response_payload_bytes) return error.InvalidResponse;
        copyVolatileBytes(self.responsePayload(), out[0..inline_bytes]);
        return inline_bytes;
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

fn grantCap(paddr: u64, target_process_slot: u64, rights: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_grant_cap),
          [arg0] "{rdi}" (paddr),
          [arg1] "{rsi}" (target_process_slot),
          [arg2] "{rdx}" (rights),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
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
