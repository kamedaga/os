const std = @import("std");
const gpu_client = @import("support_root").gpu_client;
const gpu_protocol = @import("support_root").gpu_protocol;

const syscall_log: u64 = 0x9;
const syscall_wait_event: u64 = 0x17;
const request_va: u64 = 0x3C11_4000;
const response_va: u64 = 0x3C11_5000;
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
const vertex_stride: u32 = 32;
const cube_face_vertex_count: usize = 36;
const cube_edge_vertex_count: usize = 72;
const cube_vertex_count: usize = cube_face_vertex_count + cube_edge_vertex_count;
const cube_vertex_float_count: usize = cube_vertex_count * 8;
const cube_vertex_bytes: u32 = cube_vertex_float_count * @sizeOf(f32);
const frame_command_overhead_bytes: usize = 52 + 48 + 52;
const frame_command_storage_bytes: usize = 4096;
const frame_count: usize = 180;
const edge_width: f32 = 0.014;

comptime {
    const frame_command_bytes = @as(usize, cube_vertex_bytes) + frame_command_overhead_bytes;
    if (frame_command_bytes > gpu_protocol.request_payload_bytes) {
        @compileError("gpu_demo frame command exceeds gpu IPC payload");
    }
    if (frame_command_bytes > frame_command_storage_bytes) {
        @compileError("gpu_demo frame command exceeds local command storage");
    }
}

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

const Vec3 = struct {
    x: f32,
    y: f32,
    z: f32,
};

const Face = struct {
    a: usize,
    b: usize,
    c: usize,
    d: usize,
    r: f32,
    g: f32,
    bcol: f32,
};

const cube_points = [_]Vec3{
    .{ .x = -1, .y = -1, .z = -1 },
    .{ .x = 1, .y = -1, .z = -1 },
    .{ .x = 1, .y = 1, .z = -1 },
    .{ .x = -1, .y = 1, .z = -1 },
    .{ .x = -1, .y = -1, .z = 1 },
    .{ .x = 1, .y = -1, .z = 1 },
    .{ .x = 1, .y = 1, .z = 1 },
    .{ .x = -1, .y = 1, .z = 1 },
};

const cube_faces = [_]Face{
    .{ .a = 0, .b = 1, .c = 2, .d = 3, .r = 0.95, .g = 0.18, .bcol = 0.12 },
    .{ .a = 5, .b = 4, .c = 7, .d = 6, .r = 0.15, .g = 0.75, .bcol = 0.95 },
    .{ .a = 4, .b = 0, .c = 3, .d = 7, .r = 0.20, .g = 0.90, .bcol = 0.35 },
    .{ .a = 1, .b = 5, .c = 6, .d = 2, .r = 0.95, .g = 0.80, .bcol = 0.20 },
    .{ .a = 3, .b = 2, .c = 6, .d = 7, .r = 0.65, .g = 0.35, .bcol = 0.95 },
    .{ .a = 4, .b = 5, .c = 1, .d = 0, .r = 0.95, .g = 0.45, .bcol = 0.20 },
};

const cube_edges = [_][2]usize{
    .{ 0, 1 },
    .{ 1, 2 },
    .{ 2, 3 },
    .{ 3, 0 },
    .{ 4, 5 },
    .{ 5, 6 },
    .{ 6, 7 },
    .{ 7, 4 },
    .{ 0, 4 },
    .{ 1, 5 },
    .{ 2, 6 },
    .{ 3, 7 },
};

var setup_commands_storage: [1024]u8 = undefined;
var frame_commands_storage: [frame_command_storage_bytes]u8 = undefined;
var cube_vertices_storage: [cube_vertex_float_count]f32 = undefined;

fn userLog(message: []const u8) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_log),
          [arg0] "{rdi}" (@as(u64, @intFromPtr(message.ptr))),
          [arg1] "{rsi}" (@as(u64, @intCast(message.len))),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn waitEvent(timeout_ticks: u64) u64 {
    return asm volatile (
        \\int $0x80
        : [ret] "={rax}" (-> u64),
        : [nr] "{rax}" (syscall_wait_event),
          [arg0] "{rdi}" (@as(u64, 0)),
          [arg1] "{rsi}" (timeout_ticks),
        : .{ .rcx = true, .rdx = true, .r8 = true, .r9 = true, .r10 = true, .r11 = true, .memory = true });
}

fn userLogNum(label: []const u8, value: u64) void {
    var buf: [96]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, "{s}{d}\n", .{ label, value }) catch return;
    _ = userLog(msg);
}

fn userLogHex(label: []const u8, value: u64) void {
    var buf: [96]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, "{s}0x{X}\n", .{ label, value }) catch return;
    _ = userLog(msg);
}

fn virglCmd0(command: u32, object: u32, len: u32) u32 {
    return command | (object << 8) | (len << 16);
}

fn f32Bits(value: f32) u32 {
    return @bitCast(value);
}

fn appendU32(buffer: []u8, index: *usize, value: u32) void {
    const start = index.*;
    std.mem.writeInt(u32, buffer[start..][0..4], value, .little);
    index.* += 4;
}

fn appendF32(buffer: []u8, index: *usize, value: f32) void {
    appendU32(buffer, index, f32Bits(value));
}

fn appendVertexData(buffer: []u8, index: *usize, vertices: []const f32) void {
    for (vertices) |value| {
        appendF32(buffer, index, value);
    }
}

fn rotatePoint(point: Vec3, sin_x: f32, cos_x: f32, sin_y: f32, cos_y: f32) Vec3 {
    const y1 = point.y * cos_x - point.z * sin_x;
    const z1 = point.y * sin_x + point.z * cos_x;
    const x2 = point.x * cos_y + z1 * sin_y;
    const z2 = -point.x * sin_y + z1 * cos_y;
    return .{ .x = x2, .y = y1, .z = z2 };
}

fn projectPoint(point: Vec3) Vec3 {
    return .{
        .x = point.x * 0.38 + point.z * 0.18,
        .y = point.y * 0.38 - point.z * 0.12,
        .z = point.z * 0.18,
    };
}

fn faceDepth(face: Face, rotated: *const [cube_points.len]Vec3) f32 {
    return (rotated[face.a].z + rotated[face.b].z + rotated[face.c].z + rotated[face.d].z) * 0.25;
}

fn sortedFaces(rotated: *const [cube_points.len]Vec3) [cube_faces.len]usize {
    var order = [_]usize{ 0, 1, 2, 3, 4, 5 };
    var i: usize = 0;
    while (i < order.len) : (i += 1) {
        var best = i;
        var j = i + 1;
        while (j < order.len) : (j += 1) {
            if (faceDepth(cube_faces[order[j]], rotated) < faceDepth(cube_faces[order[best]], rotated)) {
                best = j;
            }
        }
        const tmp = order[i];
        order[i] = order[best];
        order[best] = tmp;
    }
    return order;
}

fn emitVertex(out: *[cube_vertex_float_count]f32, index: *usize, position: Vec3, r: f32, g: f32, b: f32) void {
    out[index.* + 0] = position.x;
    out[index.* + 1] = position.y;
    out[index.* + 2] = position.z;
    out[index.* + 3] = 1.0;
    out[index.* + 4] = r;
    out[index.* + 5] = g;
    out[index.* + 6] = b;
    out[index.* + 7] = 1.0;
    index.* += 8;
}

fn emitCubeVertex(out: *[cube_vertex_float_count]f32, index: *usize, point: Vec3, face: Face, shade: f32) void {
    const projected = projectPoint(point);
    emitVertex(out, index, projected, face.r * shade, face.g * shade, face.bcol * shade);
}

fn edgeQuadPoints(a: Vec3, b: Vec3) [4]Vec3 {
    const dx = b.x - a.x;
    const dy = b.y - a.y;
    const len = @sqrt(dx * dx + dy * dy);
    const inv_len = if (len > 0.0001) 1.0 / len else 1.0;
    const px = -dy * inv_len * edge_width;
    const py = dx * inv_len * edge_width;
    const z = if (a.z > b.z) a.z + 0.02 else b.z + 0.02;
    return .{
        .{ .x = a.x + px, .y = a.y + py, .z = z },
        .{ .x = b.x + px, .y = b.y + py, .z = z },
        .{ .x = b.x - px, .y = b.y - py, .z = z },
        .{ .x = a.x - px, .y = a.y - py, .z = z },
    };
}

fn emitEdge(out: *[cube_vertex_float_count]f32, index: *usize, a: Vec3, b: Vec3) void {
    const quad = edgeQuadPoints(a, b);
    emitVertex(out, index, quad[0], 0.015, 0.018, 0.024);
    emitVertex(out, index, quad[1], 0.015, 0.018, 0.024);
    emitVertex(out, index, quad[2], 0.015, 0.018, 0.024);
    emitVertex(out, index, quad[0], 0.015, 0.018, 0.024);
    emitVertex(out, index, quad[2], 0.015, 0.018, 0.024);
    emitVertex(out, index, quad[3], 0.015, 0.018, 0.024);
}

fn buildCubeVertices(out: *[cube_vertex_float_count]f32, sin_x: f32, cos_x: f32, sin_y: f32, cos_y: f32) void {
    var rotated: [cube_points.len]Vec3 = undefined;
    var projected: [cube_points.len]Vec3 = undefined;
    for (cube_points, 0..) |point, i| {
        rotated[i] = rotatePoint(point, sin_x, cos_x, sin_y, cos_y);
        projected[i] = projectPoint(rotated[i]);
    }

    const order = sortedFaces(&rotated);
    var index: usize = 0;
    for (order) |face_index| {
        const face = cube_faces[face_index];
        const depth = faceDepth(face, &rotated);
        const shade = 0.72 + (depth + 1.0) * 0.12;
        emitCubeVertex(out, &index, rotated[face.a], face, shade);
        emitCubeVertex(out, &index, rotated[face.b], face, shade);
        emitCubeVertex(out, &index, rotated[face.c], face, shade);
        emitCubeVertex(out, &index, rotated[face.a], face, shade);
        emitCubeVertex(out, &index, rotated[face.c], face, shade);
        emitCubeVertex(out, &index, rotated[face.d], face, shade);
    }
    for (cube_edges) |edge| {
        emitEdge(out, &index, projected[edge[0]], projected[edge[1]]);
    }
}

fn appendBindObject(buffer: []u8, index: *usize, object: u32, handle: u32) void {
    appendU32(buffer, index, virglCmd0(virgl_ccmd_bind_object, object, 1));
    appendU32(buffer, index, handle);
}

fn appendShader(buffer: []u8, index: *usize, handle: u32, shader_type: u32, source: []const u8) void {
    const shader_len: u32 = @intCast(source.len + 1);
    const padded_dwords = (shader_len + 3) / 4;
    appendU32(buffer, index, virglCmd0(virgl_ccmd_create_object, virgl_object_shader, virgl_obj_shader_base_size + padded_dwords));
    appendU32(buffer, index, handle);
    appendU32(buffer, index, shader_type);
    appendU32(buffer, index, shader_len);
    appendU32(buffer, index, 300);
    appendU32(buffer, index, 0);
    var i: usize = 0;
    while (i < source.len) : (i += 1) {
        buffer[index.*] = source[i];
        index.* += 1;
    }
    buffer[index.*] = 0;
    index.* += 1;
    while ((index.* & 0x3) != 0) {
        buffer[index.*] = 0;
        index.* += 1;
    }
}

fn appendBindShader(buffer: []u8, index: *usize, handle: u32, shader_type: u32) void {
    appendU32(buffer, index, virglCmd0(virgl_ccmd_bind_shader, virgl_object_null, 2));
    appendU32(buffer, index, handle);
    appendU32(buffer, index, shader_type);
}

fn appendFramebufferAndClear(buffer: []u8, index: *usize, target: gpu_client.RenderTarget) void {
    appendU32(buffer, index, virglCmd0(virgl_ccmd_set_framebuffer_state, virgl_object_null, virgl_set_framebuffer_state_size_1));
    appendU32(buffer, index, 1);
    appendU32(buffer, index, 0);
    appendU32(buffer, index, target.surface_id);

    appendU32(buffer, index, virglCmd0(virgl_ccmd_clear, virgl_object_null, virgl_obj_clear_size));
    appendU32(buffer, index, pipe_clear_color0);
    appendU32(buffer, index, f32Bits(0.03));
    appendU32(buffer, index, f32Bits(0.045));
    appendU32(buffer, index, f32Bits(0.065));
    appendU32(buffer, index, f32Bits(1.0));
    appendU32(buffer, index, 0);
    appendU32(buffer, index, 0);
    appendU32(buffer, index, 0);
}

fn appendVertexBufferWrite(buffer: []u8, index: *usize, target: gpu_client.RenderTarget, vertices: []const f32) void {
    appendU32(buffer, index, virglCmd0(virgl_ccmd_resource_inline_write, virgl_object_null, 11 + ((cube_vertex_bytes + 3) / 4)));
    appendU32(buffer, index, target.vertex_buffer_id);
    appendU32(buffer, index, 0);
    appendU32(buffer, index, 0);
    appendU32(buffer, index, cube_vertex_bytes);
    appendU32(buffer, index, 0);
    appendU32(buffer, index, 0);
    appendU32(buffer, index, 0);
    appendU32(buffer, index, 0);
    appendU32(buffer, index, cube_vertex_bytes);
    appendU32(buffer, index, 1);
    appendU32(buffer, index, 1);
    appendVertexData(buffer, index, vertices);
}

fn appendDraw(buffer: []u8, index: *usize, vertex_count: u32) void {
    appendU32(buffer, index, virglCmd0(virgl_ccmd_draw_vbo, virgl_object_null, virgl_draw_vbo_size));
    appendU32(buffer, index, 0);
    appendU32(buffer, index, vertex_count);
    appendU32(buffer, index, pipe_prim_triangles);
    appendU32(buffer, index, 0);
    appendU32(buffer, index, 0);
    appendU32(buffer, index, 0);
    appendU32(buffer, index, 0);
    appendU32(buffer, index, 0);
    appendU32(buffer, index, 0);
    appendU32(buffer, index, 0);
    appendU32(buffer, index, vertex_count - 1);
    appendU32(buffer, index, 0);
}

fn buildSetupCommands(buffer: []u8, target: gpu_client.RenderTarget) []const u8 {
    var index: usize = 0;
    const ve_handle = target.surface_id + 1;
    const vs_handle = target.surface_id + 2;
    const fs_handle = target.surface_id + 3;
    const blend_handle = target.surface_id + 4;
    const dsa_handle = target.surface_id + 5;
    const rasterizer_handle = target.surface_id + 6;

    appendU32(buffer, &index, virglCmd0(virgl_ccmd_create_object, virgl_object_surface, virgl_obj_surface_size));
    appendU32(buffer, &index, target.surface_id);
    appendU32(buffer, &index, target.resource_id);
    appendU32(buffer, &index, virgl_format_b8g8r8a8_unorm);
    appendU32(buffer, &index, 0);
    appendU32(buffer, &index, 0);

    appendU32(buffer, &index, virglCmd0(virgl_ccmd_create_object, virgl_object_vertex_elements, virgl_obj_vertex_elements_size_2));
    appendU32(buffer, &index, ve_handle);
    appendU32(buffer, &index, 0);
    appendU32(buffer, &index, 0);
    appendU32(buffer, &index, 0);
    appendU32(buffer, &index, virgl_format_r32g32b32a32_float);
    appendU32(buffer, &index, 16);
    appendU32(buffer, &index, 0);
    appendU32(buffer, &index, 0);
    appendU32(buffer, &index, virgl_format_r32g32b32a32_float);
    appendBindObject(buffer, &index, virgl_object_vertex_elements, ve_handle);

    appendU32(buffer, &index, virglCmd0(virgl_ccmd_set_vertex_buffers, virgl_object_null, virgl_set_vertex_buffers_size_1));
    appendU32(buffer, &index, vertex_stride);
    appendU32(buffer, &index, 0);
    appendU32(buffer, &index, target.vertex_buffer_id);

    appendShader(buffer, &index, vs_handle, pipe_shader_vertex, vertex_shader);
    appendBindShader(buffer, &index, vs_handle, pipe_shader_vertex);
    appendShader(buffer, &index, fs_handle, pipe_shader_fragment, fragment_shader);
    appendBindShader(buffer, &index, fs_handle, pipe_shader_fragment);

    appendU32(buffer, &index, virglCmd0(virgl_ccmd_create_object, virgl_object_blend, virgl_obj_blend_size));
    appendU32(buffer, &index, blend_handle);
    appendU32(buffer, &index, 0);
    appendU32(buffer, &index, 0);
    appendU32(buffer, &index, pipe_mask_rgba << 27);
    var blend_pad: usize = 1;
    while (blend_pad < 8) : (blend_pad += 1) appendU32(buffer, &index, 0);
    appendBindObject(buffer, &index, virgl_object_blend, blend_handle);

    appendU32(buffer, &index, virglCmd0(virgl_ccmd_create_object, virgl_object_dsa, virgl_obj_dsa_size));
    appendU32(buffer, &index, dsa_handle);
    appendU32(buffer, &index, 0);
    appendU32(buffer, &index, 0);
    appendU32(buffer, &index, 0);
    appendU32(buffer, &index, 0);
    appendBindObject(buffer, &index, virgl_object_dsa, dsa_handle);

    appendU32(buffer, &index, virglCmd0(virgl_ccmd_create_object, virgl_object_rasterizer, virgl_obj_rasterizer_size));
    appendU32(buffer, &index, rasterizer_handle);
    appendU32(buffer, &index, (1 << 1) | (1 << 29) | (1 << 30));
    appendF32(buffer, &index, 1.0);
    appendU32(buffer, &index, 0);
    appendU32(buffer, &index, 0);
    appendF32(buffer, &index, 1.0);
    appendF32(buffer, &index, 0.0);
    appendF32(buffer, &index, 0.0);
    appendF32(buffer, &index, 0.0);
    appendBindObject(buffer, &index, virgl_object_rasterizer, rasterizer_handle);

    appendU32(buffer, &index, virglCmd0(virgl_ccmd_set_viewport_state, virgl_object_null, virgl_set_viewport_state_size_1));
    appendU32(buffer, &index, 0);
    appendF32(buffer, &index, @as(f32, @floatFromInt(target.width)) / 2.0);
    appendF32(buffer, &index, @as(f32, @floatFromInt(target.height)) / 2.0);
    appendF32(buffer, &index, 0.5);
    appendF32(buffer, &index, @as(f32, @floatFromInt(target.width)) / 2.0);
    appendF32(buffer, &index, @as(f32, @floatFromInt(target.height)) / 2.0);
    appendF32(buffer, &index, 0.5);

    return buffer[0..index];
}

fn buildFrameCommands(buffer: []u8, target: gpu_client.RenderTarget, vertices: []const f32) []const u8 {
    var index: usize = 0;
    appendFramebufferAndClear(buffer, &index, target);
    appendVertexBufferWrite(buffer, &index, target, vertices);
    appendDraw(buffer, &index, cube_vertex_count);
    return buffer[0..index];
}

pub export fn _start() noreturn {
    _ = userLog("GpuDemo: started\n");
    var client = gpu_client.Client.connect(.{
        .request_va = request_va,
        .response_va = response_va,
        .endpoint_id = gpu_protocol.endpoint_id,
    }) catch |err| {
        _ = userLog("GpuDemo: gpu connect failed\n");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        while (true) asm volatile ("pause");
    };
    _ = userLog("GpuDemo: gpu connect ok\n");

    const caps = client.queryCaps() catch |err| {
        _ = userLog("GpuDemo: query_caps failed\n");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        while (true) asm volatile ("pause");
    };
    userLogHex("GpuDemo: features=", caps.features);
    userLogNum("GpuDemo: capset_id=", caps.capset_id);
    userLogNum("GpuDemo: capset_max_version=", caps.capset_max_version);
    if ((caps.features & gpu_protocol.feature_submit_3d) == 0 or
        (caps.features & gpu_protocol.feature_present_3d) == 0)
    {
        _ = userLog("GpuDemo: 3d render unavailable\n");
        while (true) asm volatile ("pause");
    }

    const target = client.prepare3d() catch |err| {
        _ = userLog("GpuDemo: prepare_3d failed\n");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        while (true) asm volatile ("pause");
    };
    userLogNum("GpuDemo: render_width=", target.width);
    userLogNum("GpuDemo: render_height=", target.height);
    userLogNum("GpuDemo: resource_id=", target.resource_id);
    userLogNum("GpuDemo: surface_id=", target.surface_id);
    userLogNum("GpuDemo: vertex_buffer_id=", target.vertex_buffer_id);

    client.submit3d(buildSetupCommands(setup_commands_storage[0..], target)) catch |err| {
        _ = userLog("GpuDemo: submit_3d failed\n");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        while (true) asm volatile ("pause");
    };
    _ = userLog("GpuDemo: setup_3d ok\n");

    var sin_x: f32 = 0.0;
    var cos_x: f32 = 1.0;
    var sin_y: f32 = 0.0;
    var cos_y: f32 = 1.0;
    const step_x_sin: f32 = 0.033;
    const step_x_cos: f32 = 0.999455;
    const step_y_sin: f32 = 0.052;
    const step_y_cos: f32 = 0.998647;

    var frame: usize = 0;
    while (frame < frame_count) : (frame += 1) {
        buildCubeVertices(&cube_vertices_storage, sin_x, cos_x, sin_y, cos_y);
        client.submit3d(buildFrameCommands(frame_commands_storage[0..], target, cube_vertices_storage[0..])) catch |err| {
            _ = userLog("GpuDemo: frame submit failed\n");
            _ = userLog(@errorName(err));
            _ = userLog("\n");
            while (true) asm volatile ("pause");
        };
        client.present3d() catch |err| {
            _ = userLog("GpuDemo: present failed\n");
            _ = userLog(@errorName(err));
            _ = userLog("\n");
            while (true) asm volatile ("pause");
        };

        const next_sin_x = sin_x * step_x_cos + cos_x * step_x_sin;
        const next_cos_x = cos_x * step_x_cos - sin_x * step_x_sin;
        const next_sin_y = sin_y * step_y_cos + cos_y * step_y_sin;
        const next_cos_y = cos_y * step_y_cos - sin_y * step_y_sin;
        sin_x = next_sin_x;
        cos_x = next_cos_x;
        sin_y = next_sin_y;
        cos_y = next_cos_y;
        _ = waitEvent(2);
    }
    _ = userLog("GpuDemo: cube animation done\n");
    while (true) asm volatile ("pause");
}
