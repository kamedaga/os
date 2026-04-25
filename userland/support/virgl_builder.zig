const std = @import("std");
const gpu_client = @import("gpu_client.zig");

pub const Error = error{
    BufferTooSmall,
    Invalid,
};

pub const Color = extern struct {
    r: f32,
    g: f32,
    b: f32,
    a: f32,
};

pub const Vertex = extern struct {
    x: f32,
    y: f32,
    z: f32,
    w: f32,
    r: f32,
    g: f32,
    b: f32,
    a: f32,
};

pub const Primitive = enum {
    triangles,
};

const virgl_object_null: u32 = 0;
const virgl_object_blend: u32 = 1;
const virgl_object_rasterizer: u32 = 2;
const virgl_object_dsa: u32 = 3;
const virgl_object_shader: u32 = 4;
const virgl_object_vertex_elements: u32 = 5;
const virgl_object_surface: u32 = 8;
const virgl_ccmd_create_object: u32 = 1;
const virgl_ccmd_bind_object: u32 = 2;
const virgl_ccmd_set_viewport_state: u32 = 4;
const virgl_ccmd_set_framebuffer_state: u32 = 5;
const virgl_ccmd_set_vertex_buffers: u32 = 6;
const virgl_ccmd_clear: u32 = 7;
const virgl_ccmd_draw_vbo: u32 = 8;
const virgl_ccmd_resource_inline_write: u32 = 9;
const virgl_ccmd_bind_shader: u32 = 31;
const virgl_format_b8g8r8a8_unorm: u32 = 1;
const virgl_format_r32g32b32a32_float: u32 = 31;
const virgl_obj_surface_size: u32 = 5;
const virgl_obj_blend_size: u32 = 11;
const virgl_obj_dsa_size: u32 = 5;
const virgl_obj_rasterizer_size: u32 = 9;
const virgl_obj_shader_base_size: u32 = 5;
const virgl_obj_vertex_elements_size_2: u32 = 9;
const virgl_set_viewport_state_size_1: u32 = 7;
const virgl_set_framebuffer_state_size_1: u32 = 3;
const virgl_set_vertex_buffers_size_1: u32 = 3;
const virgl_obj_clear_size: u32 = 8;
const virgl_draw_vbo_size: u32 = 12;
const pipe_clear_color0: u32 = 1 << 2;
const pipe_mask_rgba: u32 = 0xf;
const pipe_shader_vertex: u32 = 0;
const pipe_shader_fragment: u32 = 1;
const pipe_prim_triangles: u32 = 4;
const vertex_stride: u32 = @sizeOf(Vertex);

pub const framebuffer_clear_command_bytes: usize = 52;
pub const vertex_upload_command_overhead_bytes: usize = 48;
pub const draw_arrays_command_bytes: usize = 52;

const vertex_shader =
    "VERT\n" ++
    "DCL IN[0]\n" ++
    "DCL IN[1]\n" ++
    "DCL OUT[0], POSITION\n" ++
    "DCL OUT[1], COLOR\n" ++
    " 0: MOV OUT[1], IN[1]\n" ++
    " 1: MOV OUT[0], IN[0]\n" ++
    " 2: END\n";

const fragment_shader =
    "FRAG\n" ++
    "DCL IN[0], COLOR, LINEAR\n" ++
    "DCL OUT[0], COLOR\n" ++
    " 0: MOV OUT[0], IN[0]\n" ++
    " 1: END\n";

pub fn frameCommandBytesForVertices(vertex_count: usize) usize {
    return framebuffer_clear_command_bytes +
        vertex_upload_command_overhead_bytes +
        vertex_count * @sizeOf(Vertex) +
        draw_arrays_command_bytes;
}

pub const CommandBuffer = struct {
    buffer: []u8,
    index: usize = 0,

    pub fn init(buffer: []u8) CommandBuffer {
        return .{ .buffer = buffer };
    }

    pub fn reset(self: *CommandBuffer) void {
        self.index = 0;
    }

    pub fn bytes(self: *const CommandBuffer) []const u8 {
        return self.buffer[0..self.index];
    }

    pub fn appendBasicPipeline(self: *CommandBuffer, target: gpu_client.RenderTarget) Error!void {
        const ve_handle = target.surface_id + 1;
        const vs_handle = target.surface_id + 2;
        const fs_handle = target.surface_id + 3;
        const blend_handle = target.surface_id + 4;
        const dsa_handle = target.surface_id + 5;
        const rasterizer_handle = target.surface_id + 6;

        try self.appendU32(virglCmd0(virgl_ccmd_create_object, virgl_object_surface, virgl_obj_surface_size));
        try self.appendU32(target.surface_id);
        try self.appendU32(target.resource_id);
        try self.appendU32(virgl_format_b8g8r8a8_unorm);
        try self.appendU32(0);
        try self.appendU32(0);

        try self.appendU32(virglCmd0(virgl_ccmd_create_object, virgl_object_vertex_elements, virgl_obj_vertex_elements_size_2));
        try self.appendU32(ve_handle);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(virgl_format_r32g32b32a32_float);
        try self.appendU32(16);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(virgl_format_r32g32b32a32_float);
        try self.appendBindObject(virgl_object_vertex_elements, ve_handle);

        try self.appendU32(virglCmd0(virgl_ccmd_set_vertex_buffers, virgl_object_null, virgl_set_vertex_buffers_size_1));
        try self.appendU32(vertex_stride);
        try self.appendU32(0);
        try self.appendU32(target.vertex_buffer_id);

        try self.appendShader(vs_handle, pipe_shader_vertex, vertex_shader);
        try self.appendBindShader(vs_handle, pipe_shader_vertex);
        try self.appendShader(fs_handle, pipe_shader_fragment, fragment_shader);
        try self.appendBindShader(fs_handle, pipe_shader_fragment);

        try self.appendU32(virglCmd0(virgl_ccmd_create_object, virgl_object_blend, virgl_obj_blend_size));
        try self.appendU32(blend_handle);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(pipe_mask_rgba << 27);
        var blend_pad: usize = 1;
        while (blend_pad < 8) : (blend_pad += 1) try self.appendU32(0);
        try self.appendBindObject(virgl_object_blend, blend_handle);

        try self.appendU32(virglCmd0(virgl_ccmd_create_object, virgl_object_dsa, virgl_obj_dsa_size));
        try self.appendU32(dsa_handle);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendBindObject(virgl_object_dsa, dsa_handle);

        try self.appendU32(virglCmd0(virgl_ccmd_create_object, virgl_object_rasterizer, virgl_obj_rasterizer_size));
        try self.appendU32(rasterizer_handle);
        try self.appendU32((1 << 1) | (1 << 29) | (1 << 30));
        try self.appendF32(1.0);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendF32(1.0);
        try self.appendF32(0.0);
        try self.appendF32(0.0);
        try self.appendF32(0.0);
        try self.appendBindObject(virgl_object_rasterizer, rasterizer_handle);

        try self.appendU32(virglCmd0(virgl_ccmd_set_viewport_state, virgl_object_null, virgl_set_viewport_state_size_1));
        try self.appendU32(0);
        try self.appendF32(@as(f32, @floatFromInt(target.width)) / 2.0);
        try self.appendF32(@as(f32, @floatFromInt(target.height)) / 2.0);
        try self.appendF32(0.5);
        try self.appendF32(@as(f32, @floatFromInt(target.width)) / 2.0);
        try self.appendF32(@as(f32, @floatFromInt(target.height)) / 2.0);
        try self.appendF32(0.5);
    }

    pub fn appendFramebufferClear(self: *CommandBuffer, target: gpu_client.RenderTarget, color: Color) Error!void {
        try self.appendU32(virglCmd0(virgl_ccmd_set_framebuffer_state, virgl_object_null, virgl_set_framebuffer_state_size_1));
        try self.appendU32(1);
        try self.appendU32(0);
        try self.appendU32(target.surface_id);

        try self.appendU32(virglCmd0(virgl_ccmd_clear, virgl_object_null, virgl_obj_clear_size));
        try self.appendU32(pipe_clear_color0);
        try self.appendF32(color.r);
        try self.appendF32(color.g);
        try self.appendF32(color.b);
        try self.appendF32(color.a);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(0);
    }

    pub fn appendVertexUpload(self: *CommandBuffer, target: gpu_client.RenderTarget, vertices: []const Vertex) Error!void {
        if (vertices.len == 0) return error.Invalid;
        const vertex_bytes: u32 = @intCast(vertices.len * @sizeOf(Vertex));
        try self.appendU32(virglCmd0(virgl_ccmd_resource_inline_write, virgl_object_null, 11 + ((vertex_bytes + 3) / 4)));
        try self.appendU32(target.vertex_buffer_id);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(vertex_bytes);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(vertex_bytes);
        try self.appendU32(1);
        try self.appendU32(1);
        for (vertices) |vertex| {
            try self.appendF32(vertex.x);
            try self.appendF32(vertex.y);
            try self.appendF32(vertex.z);
            try self.appendF32(vertex.w);
            try self.appendF32(vertex.r);
            try self.appendF32(vertex.g);
            try self.appendF32(vertex.b);
            try self.appendF32(vertex.a);
        }
    }

    pub fn appendDrawArrays(self: *CommandBuffer, primitive: Primitive, first: u32, count: u32) Error!void {
        if (count == 0) return error.Invalid;
        try self.appendU32(virglCmd0(virgl_ccmd_draw_vbo, virgl_object_null, virgl_draw_vbo_size));
        try self.appendU32(first);
        try self.appendU32(count);
        try self.appendU32(primitiveRaw(primitive));
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(0);
        try self.appendU32(first + count - 1);
        try self.appendU32(0);
    }

    fn appendBindObject(self: *CommandBuffer, object: u32, handle: u32) Error!void {
        try self.appendU32(virglCmd0(virgl_ccmd_bind_object, object, 1));
        try self.appendU32(handle);
    }

    fn appendShader(self: *CommandBuffer, handle: u32, shader_type: u32, source: []const u8) Error!void {
        const shader_len: u32 = @intCast(source.len + 1);
        const padded_dwords = (shader_len + 3) / 4;
        try self.appendU32(virglCmd0(virgl_ccmd_create_object, virgl_object_shader, virgl_obj_shader_base_size + padded_dwords));
        try self.appendU32(handle);
        try self.appendU32(shader_type);
        try self.appendU32(shader_len);
        try self.appendU32(300);
        try self.appendU32(0);
        try self.appendBytesNulPadded(source);
    }

    fn appendBindShader(self: *CommandBuffer, handle: u32, shader_type: u32) Error!void {
        try self.appendU32(virglCmd0(virgl_ccmd_bind_shader, virgl_object_null, 2));
        try self.appendU32(handle);
        try self.appendU32(shader_type);
    }

    fn appendU32(self: *CommandBuffer, value: u32) Error!void {
        const dst = try self.reserve(4);
        std.mem.writeInt(u32, dst[0..4], value, .little);
    }

    fn appendF32(self: *CommandBuffer, value: f32) Error!void {
        try self.appendU32(@bitCast(value));
    }

    fn appendBytesNulPadded(self: *CommandBuffer, source: []const u8) Error!void {
        const padded_len = (source.len + 1 + 3) & ~@as(usize, 3);
        const dst = try self.reserve(padded_len);
        var i: usize = 0;
        while (i < source.len) : (i += 1) dst[i] = source[i];
        while (i < padded_len) : (i += 1) dst[i] = 0;
    }

    fn reserve(self: *CommandBuffer, byte_count: usize) Error![]u8 {
        if (self.index + byte_count > self.buffer.len) return error.BufferTooSmall;
        const start = self.index;
        self.index += byte_count;
        return self.buffer[start..self.index];
    }
};

fn virglCmd0(command: u32, object: u32, len: u32) u32 {
    return command | (object << 8) | (len << 16);
}

fn primitiveRaw(primitive: Primitive) u32 {
    return switch (primitive) {
        .triangles => pipe_prim_triangles,
    };
}
