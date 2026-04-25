const gpu_client = @import("gpu_client.zig");
const gpu_protocol = @import("gpu_protocol.zig");
const virgl_builder = @import("virgl_builder.zig");

pub const Color = virgl_builder.Color;
pub const Vertex = virgl_builder.Vertex;

pub const DrawMode = enum {
    triangles,
};

pub const Error = gpu_client.Error || virgl_builder.Error;

pub const ConnectOptions = struct {
    request_va: u64,
    response_va: u64,
    endpoint_id: u64 = gpu_protocol.endpoint_id,
    response_poll_limit: u64 = 4096,
    compat_process_slot: u64 = 0,
    allow_process_slot_compat: bool = false,
};

pub const Context = struct {
    client: gpu_client.Client,
    caps: gpu_client.Caps,
    target: gpu_client.RenderTarget,

    pub fn connect(options: ConnectOptions, setup_scratch: []u8) Error!Context {
        const client = try gpu_client.Client.connect(.{
            .request_va = options.request_va,
            .response_va = options.response_va,
            .endpoint_id = options.endpoint_id,
            .response_poll_limit = options.response_poll_limit,
            .compat_process_slot = options.compat_process_slot,
            .allow_process_slot_compat = options.allow_process_slot_compat,
        });
        return initWithClient(client, setup_scratch);
    }

    pub fn connectFromRegistryPage(
        registry_page_va: u64,
        request_va: u64,
        response_va: u64,
        setup_scratch: []u8,
    ) Error!Context {
        const client = try gpu_client.Client.connectFromRegistryPage(registry_page_va, request_va, response_va);
        return initWithClient(client, setup_scratch);
    }

    pub fn beginFrame(self: *Context, scratch: []u8) Frame {
        return .{
            .context = self,
            .commands = virgl_builder.CommandBuffer.init(scratch),
        };
    }

    pub fn clearColor(self: *Context, color: Color, scratch: []u8) Error!void {
        var frame = self.beginFrame(scratch);
        try frame.clearColor(color);
        try frame.submit();
    }

    pub fn uploadVertices(self: *Context, vertices: []const Vertex, scratch: []u8) Error!void {
        var frame = self.beginFrame(scratch);
        try frame.uploadVertices(vertices);
        try frame.submit();
    }

    pub fn drawArrays(self: *Context, mode: DrawMode, first: u32, count: u32, scratch: []u8) Error!void {
        var frame = self.beginFrame(scratch);
        try frame.drawArrays(mode, first, count);
        try frame.submit();
    }

    pub fn drawTrianglesAndPresent(self: *Context, vertices: []const Vertex, clear_color: Color, scratch: []u8) Error!void {
        var frame = self.beginFrame(scratch);
        try frame.clearColor(clear_color);
        try frame.uploadVertices(vertices);
        try frame.drawArrays(.triangles, 0, @intCast(vertices.len));
        try frame.submit();
        try self.present();
    }

    pub fn present(self: *Context) Error!void {
        try self.client.present3d();
    }
};

pub const Frame = struct {
    context: *Context,
    commands: virgl_builder.CommandBuffer,

    pub fn clearColor(self: *Frame, color: Color) Error!void {
        try self.commands.appendFramebufferClear(self.context.target, color);
    }

    pub fn uploadVertices(self: *Frame, vertices: []const Vertex) Error!void {
        try self.commands.appendVertexUpload(self.context.target, vertices);
    }

    pub fn drawArrays(self: *Frame, mode: DrawMode, first: u32, count: u32) Error!void {
        try self.commands.appendDrawArrays(primitiveFromDrawMode(mode), first, count);
    }

    pub fn submit(self: *Frame) Error!void {
        try self.context.client.submit3d(self.commands.bytes());
    }
};

pub fn frameCommandBytesForVertices(vertex_count: usize) usize {
    return virgl_builder.frameCommandBytesForVertices(vertex_count);
}

fn initWithClient(client: gpu_client.Client, setup_scratch: []u8) Error!Context {
    var owned_client = client;
    const caps = try owned_client.queryCaps();
    if ((caps.features & gpu_protocol.feature_submit_3d) == 0 or
        (caps.features & gpu_protocol.feature_present_3d) == 0)
    {
        return error.Unavailable;
    }

    const target = try owned_client.prepare3d();
    var setup_commands = virgl_builder.CommandBuffer.init(setup_scratch);
    try setup_commands.appendBasicPipeline(target);
    try owned_client.submit3d(setup_commands.bytes());

    return .{
        .client = owned_client,
        .caps = caps,
        .target = target,
    };
}

fn primitiveFromDrawMode(mode: DrawMode) virgl_builder.Primitive {
    return switch (mode) {
        .triangles => .triangles,
    };
}
