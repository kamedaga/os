const std = @import("std");
const gl_client = @import("support_root").gl_client;
const gpu_protocol = @import("support_root").gpu_protocol;

const syscall_log: u64 = 0x9;
const syscall_wait_event: u64 = 0x17;
const request_va: u64 = 0x3C11_4000;
const response_va: u64 = 0x3C11_5000;
const cube_face_vertex_count: usize = 36;
const cube_edge_vertex_count: usize = 72;
const cube_vertex_count: usize = cube_face_vertex_count + cube_edge_vertex_count;
const frame_command_storage_bytes: usize = 4096;
const frame_count: usize = 180;
const edge_width: f32 = 0.014;

comptime {
    if (gl_client.frameCommandBytesForVertices(cube_vertex_count) > gpu_protocol.request_payload_bytes) {
        @compileError("gpu_demo frame command exceeds gpu IPC payload");
    }
    if (gl_client.frameCommandBytesForVertices(cube_vertex_count) > frame_command_storage_bytes) {
        @compileError("gpu_demo frame command exceeds local command storage");
    }
}

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
var cube_vertices_storage: [cube_vertex_count]gl_client.Vertex = undefined;

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

fn emitVertex(out: *[cube_vertex_count]gl_client.Vertex, index: *usize, position: Vec3, r: f32, g: f32, b: f32) void {
    out[index.*] = .{
        .x = position.x,
        .y = position.y,
        .z = position.z,
        .w = 1.0,
        .r = r,
        .g = g,
        .b = b,
        .a = 1.0,
    };
    index.* += 1;
}

fn emitCubeVertex(out: *[cube_vertex_count]gl_client.Vertex, index: *usize, point: Vec3, face: Face, shade: f32) void {
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

fn emitEdge(out: *[cube_vertex_count]gl_client.Vertex, index: *usize, a: Vec3, b: Vec3) void {
    const quad = edgeQuadPoints(a, b);
    emitVertex(out, index, quad[0], 0.015, 0.018, 0.024);
    emitVertex(out, index, quad[1], 0.015, 0.018, 0.024);
    emitVertex(out, index, quad[2], 0.015, 0.018, 0.024);
    emitVertex(out, index, quad[0], 0.015, 0.018, 0.024);
    emitVertex(out, index, quad[2], 0.015, 0.018, 0.024);
    emitVertex(out, index, quad[3], 0.015, 0.018, 0.024);
}

fn buildCubeVertices(out: *[cube_vertex_count]gl_client.Vertex, sin_x: f32, cos_x: f32, sin_y: f32, cos_y: f32) void {
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

pub export fn _start() noreturn {
    _ = userLog("GpuDemo: started\n");
    var gl = gl_client.Context.connect(.{
        .request_va = request_va,
        .response_va = response_va,
        .endpoint_id = gpu_protocol.endpoint_id,
    }, setup_commands_storage[0..]) catch |err| {
        _ = userLog("GpuDemo: gl connect failed\n");
        _ = userLog(@errorName(err));
        _ = userLog("\n");
        while (true) asm volatile ("pause");
    };
    _ = userLog("GpuDemo: gl connect ok\n");
    userLogHex("GpuDemo: features=", gl.caps.features);
    userLogNum("GpuDemo: capset_id=", gl.caps.capset_id);
    userLogNum("GpuDemo: capset_max_version=", gl.caps.capset_max_version);
    userLogNum("GpuDemo: render_width=", gl.target.width);
    userLogNum("GpuDemo: render_height=", gl.target.height);
    userLogNum("GpuDemo: resource_id=", gl.target.resource_id);
    userLogNum("GpuDemo: surface_id=", gl.target.surface_id);
    userLogNum("GpuDemo: vertex_buffer_id=", gl.target.vertex_buffer_id);
    _ = userLog("GpuDemo: setup_3d ok\n");

    var sin_x: f32 = 0.0;
    var cos_x: f32 = 1.0;
    var sin_y: f32 = 0.0;
    var cos_y: f32 = 1.0;
    const step_x_sin: f32 = 0.033;
    const step_x_cos: f32 = 0.999455;
    const step_y_sin: f32 = 0.052;
    const step_y_cos: f32 = 0.998647;
    const clear_color: gl_client.Color = .{ .r = 0.03, .g = 0.045, .b = 0.065, .a = 1.0 };

    var frame: usize = 0;
    while (frame < frame_count) : (frame += 1) {
        buildCubeVertices(&cube_vertices_storage, sin_x, cos_x, sin_y, cos_y);
        gl.drawTrianglesAndPresent(cube_vertices_storage[0..], clear_color, frame_commands_storage[0..]) catch |err| {
            _ = userLog("GpuDemo: frame draw failed\n");
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
