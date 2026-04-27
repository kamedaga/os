#![no_std]

use core::ffi::{c_char, c_void};
use core::mem::{MaybeUninit, size_of};
use core::ptr::{addr_of, addr_of_mut, read_volatile, write_bytes, write_volatile};
use core::slice;
use core::sync::atomic::{Ordering, compiler_fence};

use rt_core::{SyscallError, syscall, vm};
use rt_handle::{ServiceKind, snapshot_service_registry_shadow};

const PAGE_BYTES: usize = 4096;
const REQUEST_MAGIC: u32 = 0x5147_5047;
const RESPONSE_MAGIC: u32 = 0x5247_5047;
const VERSION: u16 = 1;
const DEFAULT_ENDPOINT_ID: u64 = 0x80 + 0x20;
const DEFAULT_RESPONSE_POLL_LIMIT: u64 = 65_536;
const SERVICE_FLAG_PROCESS_SLOT_COMPAT: u64 = 1 << 0;
const PAGE_RIGHT_CPU_READ: u64 = 0x1;
const PAGE_RIGHT_CPU_WRITE: u64 = 0x2;
const BULK_TEXTURE_UPLOAD_PAGES: usize = 16;
pub const TEXTURE_BULK_UPLOAD_BYTES: usize = BULK_TEXTURE_UPLOAD_PAGES * PAGE_BYTES;
const CAPCTL_MAGIC: u64 = 0x4C54_5043;
const CAPCTL_VERSION: u64 = 1;
const CAPCTL_OPCODE_LAUNCH_GPU: u64 = 9;
const CAPCTL_STATUS_OK: u64 = 0;
const CAPCTL_STATUS_ALREADY: u64 = 4;
pub const FEATURE_VIRGL: u64 = 1 << 0;
pub const FEATURE_SUBMIT_3D: u64 = 1 << 1;
pub const FEATURE_PRESENT_2D: u64 = 1 << 2;
pub const FEATURE_PRESENT_3D: u64 = 1 << 3;
pub const FEATURE_TEXTURE_2D: u64 = 1 << 4;
pub const FEATURE_APP_SURFACE: u64 = 1 << 5;
pub const FEATURE_CURSOR: u64 = 1 << 6;
pub const FEATURE_TEXTURE_BULK: u64 = 1 << 7;
pub const FEATURE_SHELL_FRAMEBUFFER: u64 = 1 << 8;
pub const DEFAULT_VIRGL_VERTEX_BUFFER_ID: u32 = 3;
pub const GL_FALSE: u32 = 0;
pub const GL_TRUE: u32 = 1;
pub const GL_NO_ERROR: u32 = 0;
pub const GL_INVALID_ENUM: u32 = 0x0500;
pub const GL_INVALID_VALUE: u32 = 0x0501;
pub const GL_INVALID_OPERATION: u32 = 0x0502;
pub const GL_STACK_OVERFLOW: u32 = 0x0503;
pub const GL_STACK_UNDERFLOW: u32 = 0x0504;
pub const GL_OUT_OF_MEMORY: u32 = 0x0505;
pub const GL_ZERO: u32 = 0;
pub const GL_ONE: u32 = 1;
pub const GL_TRIANGLES: u32 = 0x0004;
pub const GL_TRIANGLE_STRIP: u32 = 0x0005;
pub const GL_SRC_COLOR: u32 = 0x0300;
pub const GL_ONE_MINUS_SRC_COLOR: u32 = 0x0301;
pub const GL_SRC_ALPHA: u32 = 0x0302;
pub const GL_ONE_MINUS_SRC_ALPHA: u32 = 0x0303;
pub const GL_DST_ALPHA: u32 = 0x0304;
pub const GL_ONE_MINUS_DST_ALPHA: u32 = 0x0305;
pub const GL_DST_COLOR: u32 = 0x0306;
pub const GL_ONE_MINUS_DST_COLOR: u32 = 0x0307;
pub const GL_BLEND: u32 = 0x0BE2;
pub const GL_VIEWPORT: u32 = 0x0BA2;
pub const GL_SCISSOR_TEST: u32 = 0x0C11;
pub const GL_COLOR_BUFFER_BIT: u32 = 0x0000_4000;
pub const GL_FLOAT: u32 = 0x1406;
pub const GL_UNSIGNED_BYTE: u32 = 0x1401;
pub const GL_UNSIGNED_SHORT: u32 = 0x1403;
pub const GL_UNSIGNED_INT: u32 = 0x1405;
pub const GL_RGBA: u32 = 0x1908;
pub const GL_TEXTURE_2D: u32 = 0x0DE1;
pub const GL_NEAREST: u32 = 0x2600;
pub const GL_LINEAR: u32 = 0x2601;
pub const GL_TEXTURE_MAG_FILTER: u32 = 0x2800;
pub const GL_TEXTURE_MIN_FILTER: u32 = 0x2801;
pub const GL_TEXTURE_WRAP_S: u32 = 0x2802;
pub const GL_TEXTURE_WRAP_T: u32 = 0x2803;
pub const GL_ARRAY_BUFFER: u32 = 0x8892;
pub const GL_ELEMENT_ARRAY_BUFFER: u32 = 0x8893;
pub const GL_TEXTURE0: u32 = 0x84C0;
pub const GL_CLAMP_TO_EDGE: u32 = 0x812F;
pub const GL_STATIC_DRAW: u32 = 0x88E4;
pub const GL_DYNAMIC_DRAW: u32 = 0x88E8;
pub const GL_FRAGMENT_SHADER: u32 = 0x8B30;
pub const GL_VERTEX_SHADER: u32 = 0x8B31;
pub const GL_COMPILE_STATUS: u32 = 0x8B81;
pub const GL_LINK_STATUS: u32 = 0x8B82;
pub const GL_MODELVIEW: u32 = 0x1700;
pub const GL_PROJECTION: u32 = 0x1701;
pub const GL_VERTEX_ARRAY: u32 = 0x8074;
pub const GL_COLOR_ARRAY: u32 = 0x8076;
pub const GL_TEXTURE_COORD_ARRAY: u32 = 0x8078;
pub const COLOR_BUFFER_BIT: u32 = GL_COLOR_BUFFER_BIT;
const MAX_GL_BUFFERS: usize = 8;
const MAX_GL_TEXTURES: usize = 8;
const MAX_GL_TEXTURE_UNITS: usize = 4;
const MAX_GL_VERTEX_ATTRIBS: usize = 8;
const MAX_GL_SHADERS: usize = 8;
const MAX_GL_PROGRAMS: usize = 4;
const MAX_GL_UNIFORMS: usize = 3;
const MAX_CAPGL_CONTEXTS: usize = 4;
const MAX_CAPGL_SURFACES: usize = 4;
const MAX_GL_ARRAY_BUFFER_BYTES: usize = 4096;
const MAX_GL_TEXTURE_BYTES: usize = 16 * 1024;
const MAX_GL_SHADER_SOURCE_BYTES: usize = 1024;
const MAX_GL_NAME_BYTES: usize = 64;
const MAX_GL_DRAW_VERTICES: usize = 96;
const MAX_MODELVIEW_STACK_DEPTH: usize = 16;
const MAX_PROJECTION_STACK_DEPTH: usize = 4;
const DEBUG_GRID_COLUMNS: usize = 16;
const DEBUG_GRID_ROWS: usize = 10;
pub const FRAMEBUFFER_CLEAR_COMMAND_BYTES: usize = 52;
pub const VERTEX_UPLOAD_COMMAND_OVERHEAD_BYTES: usize = 48;
pub const DRAW_ARRAYS_COMMAND_BYTES: usize = 52;
pub const BASIC_PIPELINE_SCRATCH_BYTES: usize = 1024;
pub const FRAME_SCRATCH_BYTES: usize = 4096;

const VIRGL_OBJECT_NULL: u32 = 0;
const VIRGL_OBJECT_BLEND: u32 = 1;
const VIRGL_OBJECT_RASTERIZER: u32 = 2;
const VIRGL_OBJECT_DSA: u32 = 3;
const VIRGL_OBJECT_SHADER: u32 = 4;
const VIRGL_OBJECT_VERTEX_ELEMENTS: u32 = 5;
const VIRGL_OBJECT_SAMPLER_VIEW: u32 = 6;
const VIRGL_OBJECT_SAMPLER_STATE: u32 = 7;
const VIRGL_OBJECT_SURFACE: u32 = 8;
const VIRGL_CCMD_CREATE_OBJECT: u32 = 1;
const VIRGL_CCMD_BIND_OBJECT: u32 = 2;
const VIRGL_CCMD_SET_VIEWPORT_STATE: u32 = 4;
const VIRGL_CCMD_SET_FRAMEBUFFER_STATE: u32 = 5;
const VIRGL_CCMD_SET_VERTEX_BUFFERS: u32 = 6;
const VIRGL_CCMD_CLEAR: u32 = 7;
const VIRGL_CCMD_DRAW_VBO: u32 = 8;
const VIRGL_CCMD_RESOURCE_INLINE_WRITE: u32 = 9;
const VIRGL_CCMD_SET_SAMPLER_VIEWS: u32 = 10;
const VIRGL_CCMD_SET_SCISSOR_STATE: u32 = 15;
const VIRGL_CCMD_BIND_SAMPLER_STATES: u32 = 18;
const VIRGL_CCMD_BIND_SHADER: u32 = 31;
const VIRGL_FORMAT_B8G8R8A8_UNORM: u32 = 1;
const VIRGL_FORMAT_R8_UNORM: u32 = 64;
const VIRGL_FORMAT_R32G32B32A32_FLOAT: u32 = 31;
const VIRGL_OBJ_SURFACE_SIZE: u32 = 5;
const VIRGL_OBJ_BLEND_SIZE: u32 = 11;
const VIRGL_OBJ_DSA_SIZE: u32 = 5;
const VIRGL_OBJ_RASTERIZER_SIZE: u32 = 9;
const VIRGL_OBJ_SHADER_BASE_SIZE: u32 = 5;
const VIRGL_OBJ_VERTEX_ELEMENTS_SIZE_3: u32 = 13;
const VIRGL_OBJ_SAMPLER_VIEW_SIZE: u32 = 6;
const VIRGL_OBJ_SAMPLER_STATE_SIZE: u32 = 9;
const VIRGL_SET_VIEWPORT_STATE_SIZE_1: u32 = 7;
const VIRGL_SET_FRAMEBUFFER_STATE_SIZE_1: u32 = 3;
const VIRGL_SET_VERTEX_BUFFERS_SIZE_1: u32 = 3;
const VIRGL_SET_SAMPLER_VIEWS_SIZE_1: u32 = 3;
const VIRGL_SET_SCISSOR_STATE_SIZE_1: u32 = 3;
const VIRGL_BIND_SAMPLER_STATES_SIZE_1: u32 = 3;
const VIRGL_OBJ_CLEAR_SIZE: u32 = 8;
const VIRGL_DRAW_VBO_SIZE: u32 = 12;
const PIPE_CLEAR_COLOR0: u32 = 1 << 2;
const PIPE_MASK_RGBA: u32 = 0xf;
const PIPE_BLEND_ADD: u32 = 0;
const PIPE_BLENDFACTOR_ONE: u32 = 1;
const PIPE_BLENDFACTOR_SRC_COLOR: u32 = 2;
const PIPE_BLENDFACTOR_SRC_ALPHA: u32 = 3;
const PIPE_BLENDFACTOR_DST_ALPHA: u32 = 4;
const PIPE_BLENDFACTOR_DST_COLOR: u32 = 5;
const PIPE_BLENDFACTOR_ZERO: u32 = 0x11;
const PIPE_BLENDFACTOR_INV_SRC_COLOR: u32 = 0x12;
const PIPE_BLENDFACTOR_INV_SRC_ALPHA: u32 = 0x13;
const PIPE_BLENDFACTOR_INV_DST_ALPHA: u32 = 0x14;
const PIPE_BLENDFACTOR_INV_DST_COLOR: u32 = 0x15;
const PIPE_SHADER_VERTEX: u32 = 0;
const PIPE_SHADER_FRAGMENT: u32 = 1;
const PIPE_TEX_WRAP_CLAMP_TO_EDGE: u32 = 2;
const PIPE_TEX_FILTER_LINEAR: u32 = 1;
const PIPE_TEX_MIPFILTER_NONE: u32 = 2;
const PIPE_PRIM_TRIANGLES: u32 = 4;
const PIPE_PRIM_TRIANGLE_STRIP: u32 = 5;
const VERTEX_STRIDE: u32 = size_of::<Vertex>() as u32;
const IDENTITY_MATRIX: [f32; 16] = [
    1.0, 0.0, 0.0, 0.0, //
    0.0, 1.0, 0.0, 0.0, //
    0.0, 0.0, 1.0, 0.0, //
    0.0, 0.0, 0.0, 1.0,
];

const LINEAR_CLAMP_SAMPLER_S0: u32 = PIPE_TEX_WRAP_CLAMP_TO_EDGE
    | (PIPE_TEX_WRAP_CLAMP_TO_EDGE << 3)
    | (PIPE_TEX_WRAP_CLAMP_TO_EDGE << 6)
    | (PIPE_TEX_FILTER_LINEAR << 9)
    | (PIPE_TEX_MIPFILTER_NONE << 11)
    | (PIPE_TEX_FILTER_LINEAR << 13);
const SAMPLER_SWIZZLE_RGBA: u32 = 0x688;
const SAMPLER_SWIZZLE_R_TO_ALPHA: u32 = 0x16d;

const VERTEX_SHADER: &[u8] = b"VERT\n\
DCL IN[0]\n\
DCL IN[1]\n\
DCL IN[2]\n\
DCL OUT[0], POSITION\n\
DCL OUT[1], COLOR\n\
DCL OUT[2], GENERIC[0]\n\
 0: MOV OUT[1], IN[1]\n\
 1: MOV OUT[2], IN[2]\n\
 2: MOV OUT[0], IN[0]\n\
 3: END\n";

const FRAGMENT_SHADER: &[u8] = b"FRAG\n\
DCL IN[0], COLOR, LINEAR\n\
DCL IN[1], GENERIC[0], LINEAR\n\
DCL OUT[0], COLOR\n\
DCL SAMP[0]\n\
DCL SVIEW[0], 2D, FLOAT\n\
DCL TEMP[0]\n\
 0: TEX TEMP[0], IN[1], SAMP[0], 2D\n\
 1: MUL OUT[0], IN[0], TEMP[0]\n\
 2: END\n";

const SOLID_FRAGMENT_SHADER: &[u8] = b"FRAG\n\
DCL IN[0], COLOR, LINEAR\n\
DCL OUT[0], COLOR\n\
 0: MOV OUT[0], IN[0]\n\
 1: END\n";

const LOADING_GLOSS_FRAGMENT_SHADER: &[u8] = b"FRAG\n\
DCL IN[0], COLOR, LINEAR\n\
DCL IN[1], GENERIC[0], LINEAR\n\
DCL OUT[0], COLOR\n\
DCL TEMP[0]\n\
DCL TEMP[1]\n\
 0: ADD TEMP[0].x, IN[1].xxxx, IN[1].zzzz\n\
 1: FRC TEMP[0].x, TEMP[0].xxxx\n\
 2: ADD TEMP[0].x, TEMP[0].xxxx, IN[1].yyyy\n\
 3: ABS TEMP[0].x, TEMP[0].xxxx\n\
 4: MUL TEMP[0].x, TEMP[0].xxxx, IN[1].wwww\n\
 5: ADD TEMP[0].x, TEMP[0].xxxx, IN[0].wwww\n\
 6: MUL TEMP[1], IN[0], TEMP[0].xxxx\n\
 7: MOV OUT[0], TEMP[1]\n\
 8: END\n";

static mut GLOBAL_CONTEXT: MaybeUninit<Context> = MaybeUninit::uninit();
static mut GLOBAL_CONTEXT_READY: bool = false;
static mut GLOBAL_CAPGL_CONTEXTS: [CapglContextState; MAX_CAPGL_CONTEXTS] =
    [CapglContextState::EMPTY; MAX_CAPGL_CONTEXTS];
static mut GLOBAL_CAPGL_SURFACES: [CapglSurfaceState; MAX_CAPGL_SURFACES] =
    [CapglSurfaceState::EMPTY; MAX_CAPGL_SURFACES];
static mut GLOBAL_NEXT_CAPGL_CONTEXT_NAME: u32 = 1;
static mut GLOBAL_NEXT_CAPGL_SURFACE_NAME: u32 = 1;
static mut GLOBAL_CURRENT_CAPGL_CONTEXT: u32 = 0;
static mut GLOBAL_CURRENT_CAPGL_SURFACE: u32 = 0;
static mut GLOBAL_SCRATCH: [u8; FRAME_SCRATCH_BYTES] = [0; FRAME_SCRATCH_BYTES];
static mut GLOBAL_LAST_ERROR: u32 = GL_NO_ERROR;
static mut GLOBAL_NEXT_BUFFER_NAME: u32 = 1;
static mut GLOBAL_NEXT_TEXTURE_NAME: u32 = 1;
static mut GLOBAL_CURRENT_ARRAY_BUFFER: u32 = 0;
static mut GLOBAL_CURRENT_ELEMENT_ARRAY_BUFFER: u32 = 0;
static mut GLOBAL_BUFFERS: [GlBuffer; MAX_GL_BUFFERS] = [GlBuffer::EMPTY; MAX_GL_BUFFERS];
static mut GLOBAL_TEXTURES: [GlTexture; MAX_GL_TEXTURES] = [GlTexture::EMPTY; MAX_GL_TEXTURES];
static mut GLOBAL_ACTIVE_TEXTURE_UNIT: usize = 0;
static mut GLOBAL_TEXTURE_BINDINGS_2D: [u32; MAX_GL_TEXTURE_UNITS] = [0; MAX_GL_TEXTURE_UNITS];
static mut GLOBAL_VERTEX_ATTRIBS: [VertexAttrib; MAX_GL_VERTEX_ATTRIBS] =
    [VertexAttrib::DISABLED; MAX_GL_VERTEX_ATTRIBS];
static mut GLOBAL_TEXTURE_UPLOAD_SCRATCH: [u8; MAX_GL_TEXTURE_BYTES] = [0; MAX_GL_TEXTURE_BYTES];
static mut GLOBAL_NEXT_SHADER_NAME: u32 = 1;
static mut GLOBAL_NEXT_PROGRAM_NAME: u32 = 1;
static mut GLOBAL_SHADERS: [GlShader; MAX_GL_SHADERS] = [GlShader::EMPTY; MAX_GL_SHADERS];
static mut GLOBAL_PROGRAMS: [GlProgram; MAX_GL_PROGRAMS] = [GlProgram::EMPTY; MAX_GL_PROGRAMS];
static mut GLOBAL_CURRENT_PROGRAM: u32 = 0;
static mut GLOBAL_CURRENT_COLOR: [f32; 4] = [1.0, 1.0, 1.0, 1.0];
static mut GLOBAL_DRAW_VERTICES: [Vertex; MAX_GL_DRAW_VERTICES] =
    [Vertex::ZERO; MAX_GL_DRAW_VERTICES];
static mut GLOBAL_MATRIX_MODE: u32 = GL_MODELVIEW;
static mut GLOBAL_MODELVIEW_MATRIX: [f32; 16] = IDENTITY_MATRIX;
static mut GLOBAL_PROJECTION_MATRIX: [f32; 16] = IDENTITY_MATRIX;
static mut GLOBAL_MODELVIEW_STACK: [[f32; 16]; MAX_MODELVIEW_STACK_DEPTH] =
    [IDENTITY_MATRIX; MAX_MODELVIEW_STACK_DEPTH];
static mut GLOBAL_PROJECTION_STACK: [[f32; 16]; MAX_PROJECTION_STACK_DEPTH] =
    [IDENTITY_MATRIX; MAX_PROJECTION_STACK_DEPTH];
static mut GLOBAL_MODELVIEW_STACK_DEPTH: usize = 0;
static mut GLOBAL_PROJECTION_STACK_DEPTH: usize = 0;
static mut GLOBAL_FIXED_MATRIX_ACTIVE: bool = false;

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Error {
    RequestAllocFailed,
    RequestMapFailed,
    ResponseAllocFailed,
    ResponseMapFailed,
    EndpointNotFound,
    EndpointInstallFailed,
    ResponseGrantFailed,
    RequestSendFailed,
    BulkBufferAllocFailed,
    BulkBufferGrantFailed,
    Timeout,
    InvalidResponse,
    BufferTooSmall,
    Invalid,
    Unavailable,
    IoError,
    MissingService,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
#[repr(u16)]
enum Opcode {
    QueryCaps = 1,
    SubmitNoop = 2,
    Submit3d = 3,
    PresentTestPattern = 4,
    Prepare3d = 5,
    Present3d = 6,
    UploadTexture2d = 7,
    UpdateTexture2d = 8,
    DeleteTexture2d = 9,
    CreateAppSurface = 10,
    SetCursorPosition = 11,
    CreateAlphaTexture2d = 12,
    UpdateTextureAlpha2d = 13,
    UpdateTextureAlpha2dBulk = 14,
    #[allow(dead_code)]
    PresentShellFramebuffer = 15,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
#[repr(i32)]
enum Status {
    Ok = 0,
    Invalid = 1,
    Unavailable = 2,
    IoError = 3,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Caps {
    pub features: u64,
    pub capset_id: u32,
    pub capset_max_version: u32,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct RenderTarget {
    pub width: u32,
    pub height: u32,
    pub resource_id: u32,
    pub surface_id: u32,
    pub vertex_buffer_id: u32,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
struct TextureBinding {
    resource_id: u32,
    view_format: u32,
    swizzle: u32,
}

impl TextureBinding {
    const fn bgra8(resource_id: u32) -> Self {
        Self {
            resource_id,
            view_format: VIRGL_FORMAT_B8G8R8A8_UNORM,
            swizzle: SAMPLER_SWIZZLE_RGBA,
        }
    }

    const fn alpha8(resource_id: u32) -> Self {
        Self {
            resource_id,
            view_format: VIRGL_FORMAT_R8_UNORM,
            swizzle: SAMPLER_SWIZZLE_R_TO_ALPHA,
        }
    }
}

#[derive(Copy, Clone, Debug, PartialEq)]
pub struct Viewport {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

impl Viewport {
    fn from_target(target: RenderTarget) -> Self {
        Self {
            x: 0,
            y: 0,
            width: target.width as i32,
            height: target.height as i32,
        }
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct PresentInfo {
    pub width: u32,
    pub height: u32,
    pub resource_id: u32,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
struct CapglContextState {
    name: u32,
    in_use: bool,
}

impl CapglContextState {
    const EMPTY: Self = Self {
        name: 0,
        in_use: false,
    };
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
struct CapglSurfaceState {
    name: u32,
    in_use: bool,
    width: u32,
    height: u32,
    resource_id: u32,
    surface_id: u32,
    swap_count: u32,
    is_default: bool,
}

impl CapglSurfaceState {
    const EMPTY: Self = Self {
        name: 0,
        in_use: false,
        width: 0,
        height: 0,
        resource_id: 0,
        surface_id: 0,
        swap_count: 0,
        is_default: false,
    };
}

#[derive(Copy, Clone, Debug, PartialEq)]
#[repr(C)]
pub struct Color {
    pub r: f32,
    pub g: f32,
    pub b: f32,
    pub a: f32,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
struct BlendState {
    enabled: bool,
    src_factor: u32,
    dst_factor: u32,
}

impl BlendState {
    const DEFAULT: Self = Self {
        enabled: false,
        src_factor: GL_ONE,
        dst_factor: GL_ZERO,
    };
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
struct ScissorBox {
    x: u16,
    y: u16,
    width: u16,
    height: u16,
}

impl ScissorBox {
    fn from_target(target: RenderTarget) -> Self {
        Self {
            x: 0,
            y: 0,
            width: clamp_u32_to_u16(target.width),
            height: clamp_u32_to_u16(target.height),
        }
    }

    fn max_x(self) -> u16 {
        self.x.saturating_add(self.width)
    }

    fn max_y(self) -> u16 {
        self.y.saturating_add(self.height)
    }
}

#[derive(Copy, Clone, Debug, PartialEq)]
#[repr(C)]
pub struct Vertex {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub w: f32,
    pub r: f32,
    pub g: f32,
    pub b: f32,
    pub a: f32,
    pub u: f32,
    pub v: f32,
    pub s: f32,
    pub t: f32,
}

impl Vertex {
    const ZERO: Self = Self {
        x: 0.0,
        y: 0.0,
        z: 0.0,
        w: 1.0,
        r: 1.0,
        g: 1.0,
        b: 1.0,
        a: 1.0,
        u: 0.0,
        v: 0.0,
        s: 0.0,
        t: 1.0,
    };
}

#[derive(Copy, Clone)]
struct GlBuffer {
    name: u32,
    len: usize,
    usage: u32,
    data: [u8; MAX_GL_ARRAY_BUFFER_BYTES],
}

impl GlBuffer {
    const EMPTY: Self = Self {
        name: 0,
        len: 0,
        usage: 0,
        data: [0; MAX_GL_ARRAY_BUFFER_BYTES],
    };
}

#[derive(Copy, Clone)]
struct GlTexture {
    name: u32,
    width: i32,
    height: i32,
    internal_format: i32,
    format: u32,
    type_: u32,
    min_filter: i32,
    mag_filter: i32,
    wrap_s: i32,
    wrap_t: i32,
    resource_id: u32,
    owns_resource: bool,
    data_len: usize,
    data: [u8; MAX_GL_TEXTURE_BYTES],
}

impl GlTexture {
    const EMPTY: Self = Self {
        name: 0,
        width: 0,
        height: 0,
        internal_format: 0,
        format: 0,
        type_: 0,
        min_filter: GL_NEAREST as i32,
        mag_filter: GL_NEAREST as i32,
        wrap_s: GL_CLAMP_TO_EDGE as i32,
        wrap_t: GL_CLAMP_TO_EDGE as i32,
        resource_id: 0,
        owns_resource: false,
        data_len: 0,
        data: [0; MAX_GL_TEXTURE_BYTES],
    };
}

#[derive(Copy, Clone)]
struct VertexAttrib {
    enabled: bool,
    buffer_name: u32,
    size: i32,
    type_: u32,
    normalized: u8,
    stride: usize,
    offset: usize,
}

impl VertexAttrib {
    const DISABLED: Self = Self {
        enabled: false,
        buffer_name: 0,
        size: 4,
        type_: GL_FLOAT,
        normalized: GL_FALSE as u8,
        stride: 0,
        offset: 0,
    };
}

#[derive(Copy, Clone)]
struct GlShader {
    name: u32,
    type_: u32,
    source_len: usize,
    compiled: bool,
    source: [u8; MAX_GL_SHADER_SOURCE_BYTES],
}

impl GlShader {
    const EMPTY: Self = Self {
        name: 0,
        type_: 0,
        source_len: 0,
        compiled: false,
        source: [0; MAX_GL_SHADER_SOURCE_BYTES],
    };
}

#[derive(Copy, Clone)]
struct GlProgram {
    name: u32,
    vertex_shader: u32,
    fragment_shader: u32,
    linked: bool,
    uniform_tint: [f32; 4],
    uniform_mvp: [f32; 16],
    uniform_texture0: i32,
    uniform_tint_set: bool,
    uniform_mvp_set: bool,
    uniform_texture0_set: bool,
}

impl GlProgram {
    const EMPTY: Self = Self {
        name: 0,
        vertex_shader: 0,
        fragment_shader: 0,
        linked: false,
        uniform_tint: [1.0, 1.0, 1.0, 1.0],
        uniform_mvp: IDENTITY_MATRIX,
        uniform_texture0: 0,
        uniform_tint_set: false,
        uniform_mvp_set: false,
        uniform_texture0_set: false,
    };
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Primitive {
    Triangles,
    TriangleStrip,
}

#[repr(C)]
struct RequestHeader {
    magic: u32,
    version: u16,
    op: u16,
    request_seq: u64,
    response_paddr: u64,
    arg0: u64,
    arg1: u64,
    inline_bytes: u32,
    reserved0: u32,
    session_nonce: u64,
}

#[repr(C)]
struct ResponseHeader {
    magic: u32,
    version: u16,
    op: u16,
    response_seq: u64,
    status: i32,
    result_flags: u32,
    arg0: u64,
    arg1: u64,
    arg2: u64,
    inline_bytes: u32,
    reserved0: u32,
}

pub struct Client {
    request_va: u64,
    response_va: u64,
    request_paddr: u64,
    response_paddr: u64,
    session_nonce: u64,
    server_endpoint_id: u64,
    server_process_slot: u64,
    request_shared: bool,
    next_seq: u64,
    response_poll_limit: u64,
    bulk_upload_ready: bool,
    bulk_upload_va: u64,
    bulk_upload_paddrs: [u64; BULK_TEXTURE_UPLOAD_PAGES],
}

impl Client {
    pub fn connect_from_registry_shadow() -> Result<Self, Error> {
        Self::connect_from_registry_shadow_at(0, 0)
    }

    pub fn connect_from_registry_shadow_at(
        request_va: u64,
        response_va: u64,
    ) -> Result<Self, Error> {
        let binding = unsafe {
            snapshot_service_registry_shadow()
                .and_then(|snapshot| snapshot.find_kind(ServiceKind::Gpu))
        };
        if let Some(binding) = binding {
            let allow_compat = binding.process_slot != 0
                && (binding.flags & SERVICE_FLAG_PROCESS_SLOT_COMPAT) != 0;
            let result = Self::connect(ConnectOptions {
                request_va,
                response_va,
                endpoint_id: binding.endpoint_id,
                response_poll_limit: DEFAULT_RESPONSE_POLL_LIMIT,
                server_process_slot: binding.process_slot,
                compat_process_slot: binding.process_slot,
                allow_process_slot_compat: allow_compat,
            });
            if !matches!(result, Err(Error::EndpointNotFound)) {
                return result;
            }
        }

        if let Ok(gpu) = launch_gpu_from_capctl() {
            return Self::connect(ConnectOptions {
                request_va,
                response_va,
                endpoint_id: gpu.endpoint_id,
                response_poll_limit: DEFAULT_RESPONSE_POLL_LIMIT,
                server_process_slot: gpu.process_slot,
                compat_process_slot: gpu.process_slot,
                allow_process_slot_compat: gpu.process_slot != 0,
            });
        }

        Self::connect(ConnectOptions {
            request_va,
            response_va,
            ..ConnectOptions::default()
        })
    }

    pub fn connect(options: ConnectOptions) -> Result<Self, Error> {
        Self::connect_mapped(options)
    }

    fn connect_mapped(options: ConnectOptions) -> Result<Self, Error> {
        let (request_va, request_paddr, response_va, response_paddr) =
            if options.request_va == 0 && options.response_va == 0 {
                let request = vm::alloc_map_page(true).map_err(map_request_page_error)?;
                let response = vm::alloc_map_page(true).map_err(map_response_page_error)?;
                (
                    request.va(),
                    request.paddr(),
                    response.va(),
                    response.paddr(),
                )
            } else if options.request_va != 0 && options.response_va != 0 {
                let request_paddr = alloc_page().ok_or(Error::RequestAllocFailed)?;
                if syscall::call3(syscall::MAP_PAGE, options.request_va, request_paddr, 1)
                    != syscall::OK
                {
                    return Err(Error::RequestMapFailed);
                }

                let response_paddr = alloc_page().ok_or(Error::ResponseAllocFailed)?;
                if syscall::call3(syscall::MAP_PAGE, options.response_va, response_paddr, 1)
                    != syscall::OK
                {
                    return Err(Error::ResponseMapFailed);
                }
                (
                    options.request_va,
                    request_paddr,
                    options.response_va,
                    response_paddr,
                )
            } else {
                return Err(Error::Invalid);
            };

        grant_response_cap(response_paddr, &options)?;
        let session_nonce = make_session_nonce(
            request_paddr,
            response_paddr,
            options.endpoint_id,
            options.server_process_slot,
        );

        let client = Self {
            request_va,
            response_va,
            request_paddr,
            response_paddr,
            session_nonce,
            server_endpoint_id: options.endpoint_id,
            server_process_slot: options.server_process_slot,
            request_shared: false,
            next_seq: 1,
            response_poll_limit: options.response_poll_limit,
            bulk_upload_ready: false,
            bulk_upload_va: 0,
            bulk_upload_paddrs: [0; BULK_TEXTURE_UPLOAD_PAGES],
        };
        client.clear_mapped_pages();
        Ok(client)
    }
}

impl Client {
    pub const fn request_va(&self) -> u64 {
        self.request_va
    }

    pub const fn response_va(&self) -> u64 {
        self.response_va
    }

    pub const fn request_paddr(&self) -> u64 {
        self.request_paddr
    }

    pub const fn response_paddr(&self) -> u64 {
        self.response_paddr
    }

    pub fn query_caps(&mut self) -> Result<Caps, Error> {
        let seq = self.begin_request(Opcode::QueryCaps, 0, 0, &[])?;
        let response = self.finish_request_ok(seq, Opcode::QueryCaps)?;
        let features = unsafe { read_volatile(addr_of!((*response).arg0)) };
        let capset_id = unsafe { read_volatile(addr_of!((*response).arg1)) as u32 };
        let capset_max_version = unsafe { read_volatile(addr_of!((*response).arg2)) as u32 };
        Ok(Caps {
            features,
            capset_id,
            capset_max_version,
        })
    }

    pub fn submit_noop(&mut self) -> Result<(), Error> {
        let seq = self.begin_request(Opcode::SubmitNoop, 0, 0, &[])?;
        self.finish_request_ok(seq, Opcode::SubmitNoop)?;
        Ok(())
    }

    pub fn submit_3d(&mut self, commands: &[u8]) -> Result<(), Error> {
        if commands.is_empty() || (commands.len() & 0x3) != 0 {
            return Err(Error::Invalid);
        }
        let seq = self.begin_request(Opcode::Submit3d, commands.len() as u64, 0, commands)?;
        self.finish_request_ok(seq, Opcode::Submit3d)?;
        Ok(())
    }

    pub fn prepare_3d(&mut self) -> Result<RenderTarget, Error> {
        let seq = self.begin_request(Opcode::Prepare3d, 0, 0, &[])?;
        let response = self.finish_request_ok(seq, Opcode::Prepare3d)?;
        let width = unsafe { read_volatile(addr_of!((*response).arg0)) } as u32;
        let height = unsafe { read_volatile(addr_of!((*response).arg1)) } as u32;
        let handles = unsafe { read_volatile(addr_of!((*response).arg2)) };
        Ok(RenderTarget {
            width,
            height,
            resource_id: handles as u32,
            surface_id: (handles >> 32) as u32,
            vertex_buffer_id: DEFAULT_VIRGL_VERTEX_BUFFER_ID,
        })
    }

    pub fn present_3d(&mut self) -> Result<PresentInfo, Error> {
        let seq = self.begin_request(Opcode::Present3d, 0, 0, &[])?;
        let response = self.finish_request_ok(seq, Opcode::Present3d)?;
        Ok(PresentInfo {
            width: unsafe { read_volatile(addr_of!((*response).arg0)) } as u32,
            height: unsafe { read_volatile(addr_of!((*response).arg1)) } as u32,
            resource_id: unsafe { read_volatile(addr_of!((*response).arg2)) } as u32,
        })
    }

    pub fn upload_texture_2d(
        &mut self,
        resource_id_hint: u32,
        width: u32,
        height: u32,
        pixels: &[u8],
    ) -> Result<u32, Error> {
        if width == 0 || height == 0 || pixels.len() != width as usize * height as usize * 4 {
            return Err(Error::Invalid);
        }
        let packed_size = width as u64 | ((height as u64) << 32);
        let seq = self.begin_request(
            Opcode::UploadTexture2d,
            resource_id_hint as u64,
            packed_size,
            pixels,
        )?;
        let response = self.finish_request_ok(seq, Opcode::UploadTexture2d)?;
        Ok(unsafe { read_volatile(addr_of!((*response).arg0)) } as u32)
    }

    pub fn update_texture_2d(
        &mut self,
        resource_id: u32,
        x: u32,
        y: u32,
        width: u32,
        height: u32,
        pixels: &[u8],
    ) -> Result<(), Error> {
        if resource_id == 0
            || width == 0
            || height == 0
            || pixels.len() != width as usize * height as usize * 4
            || y > 0xffff
            || width > 0xffff
            || height > 0xffff
        {
            return Err(Error::Invalid);
        }
        let arg0 = resource_id as u64 | ((x as u64) << 32);
        let arg1 = y as u64 | ((width as u64) << 16) | ((height as u64) << 32);
        let seq = self.begin_request(Opcode::UpdateTexture2d, arg0, arg1, pixels)?;
        self.finish_request_ok(seq, Opcode::UpdateTexture2d)?;
        Ok(())
    }

    pub fn create_alpha_texture_2d(&mut self, width: u32, height: u32) -> Result<u32, Error> {
        if width == 0 || height == 0 {
            return Err(Error::Invalid);
        }
        let arg0 = width as u64 | ((height as u64) << 32);
        let seq = self.begin_request(Opcode::CreateAlphaTexture2d, arg0, 0, &[])?;
        let response = self.finish_request_ok(seq, Opcode::CreateAlphaTexture2d)?;
        Ok(unsafe { read_volatile(addr_of!((*response).arg0)) } as u32)
    }

    pub fn update_texture_alpha_2d(
        &mut self,
        resource_id: u32,
        x: u32,
        y: u32,
        width: u32,
        height: u32,
        alpha: &[u8],
    ) -> Result<(), Error> {
        if alpha.len() > PAGE_BYTES - size_of::<RequestHeader>() {
            return self.update_texture_alpha_2d_bulk(resource_id, x, y, width, height, alpha);
        }
        if resource_id == 0
            || width == 0
            || height == 0
            || alpha.len() != width as usize * height as usize
            || y > 0xffff
            || width > 0xffff
            || height > 0xffff
        {
            return Err(Error::Invalid);
        }
        let arg0 = resource_id as u64 | ((x as u64) << 32);
        let arg1 = y as u64 | ((width as u64) << 16) | ((height as u64) << 32);
        let seq = self.begin_request(Opcode::UpdateTextureAlpha2d, arg0, arg1, alpha)?;
        self.finish_request_ok(seq, Opcode::UpdateTextureAlpha2d)?;
        Ok(())
    }

    fn update_texture_alpha_2d_bulk(
        &mut self,
        resource_id: u32,
        x: u32,
        y: u32,
        width: u32,
        height: u32,
        alpha: &[u8],
    ) -> Result<(), Error> {
        if resource_id == 0
            || width == 0
            || height == 0
            || alpha.len() != width as usize * height as usize
            || alpha.len() > TEXTURE_BULK_UPLOAD_BYTES
            || y > 0xffff
            || width > 0xffff
            || height > 0xffff
        {
            return Err(Error::Invalid);
        }
        self.ensure_bulk_texture_upload_buffer()?;
        copy_bytes_to_volatile(self.bulk_upload_va as *mut u8, alpha);
        let page_count = (alpha.len() + PAGE_BYTES - 1) / PAGE_BYTES;
        let payload = unsafe {
            slice::from_raw_parts(
                self.bulk_upload_paddrs.as_ptr() as *const u8,
                page_count * size_of::<u64>(),
            )
        };
        let arg0 = resource_id as u64 | ((x as u64) << 32);
        let arg1 = y as u64 | ((width as u64) << 16) | ((height as u64) << 32);
        let seq = self.begin_request(Opcode::UpdateTextureAlpha2dBulk, arg0, arg1, payload)?;
        self.finish_request_ok(seq, Opcode::UpdateTextureAlpha2dBulk)?;
        Ok(())
    }

    pub fn delete_texture_2d(&mut self, resource_id: u32) -> Result<(), Error> {
        if resource_id == 0 {
            return Ok(());
        }
        let seq = self.begin_request(Opcode::DeleteTexture2d, resource_id as u64, 0, &[])?;
        self.finish_request_ok(seq, Opcode::DeleteTexture2d)?;
        Ok(())
    }

    pub fn create_app_surface(&mut self, width: u32, height: u32) -> Result<RenderTarget, Error> {
        if width == 0 || height == 0 {
            return Err(Error::Invalid);
        }
        let arg0 = width as u64 | ((height as u64) << 32);
        let seq = self.begin_request(Opcode::CreateAppSurface, arg0, 0, &[])?;
        let response = self.finish_request_ok(seq, Opcode::CreateAppSurface)?;
        let response_width = unsafe { read_volatile(addr_of!((*response).arg0)) } as u32;
        let response_height = unsafe { read_volatile(addr_of!((*response).arg1)) } as u32;
        let handles = unsafe { read_volatile(addr_of!((*response).arg2)) };
        Ok(RenderTarget {
            width: response_width,
            height: response_height,
            resource_id: handles as u32,
            surface_id: (handles >> 32) as u32,
            vertex_buffer_id: DEFAULT_VIRGL_VERTEX_BUFFER_ID,
        })
    }

    pub fn set_cursor_position(&mut self, x: i32, y: i32) -> Result<(), Error> {
        let arg0 = (x as u32 as u64) | ((y as u32 as u64) << 32);
        let seq = self.begin_request(Opcode::SetCursorPosition, arg0, 0, &[])?;
        self.finish_request_ok(seq, Opcode::SetCursorPosition)?;
        Ok(())
    }

    pub fn present_test_pattern(&mut self) -> Result<PresentInfo, Error> {
        let seq = self.begin_request(Opcode::PresentTestPattern, 0, 0, &[])?;
        let response = self.finish_request_ok(seq, Opcode::PresentTestPattern)?;
        Ok(PresentInfo {
            width: unsafe { read_volatile(addr_of!((*response).arg0)) } as u32,
            height: unsafe { read_volatile(addr_of!((*response).arg1)) } as u32,
            resource_id: unsafe { read_volatile(addr_of!((*response).arg2)) } as u32,
        })
    }

    pub fn create_context(mut self) -> Result<Context, Error> {
        let caps = self.query_caps()?;
        if (caps.features & FEATURE_SUBMIT_3D) == 0 || (caps.features & FEATURE_PRESENT_3D) == 0 {
            return Err(Error::Unavailable);
        }
        let target = self.prepare_3d()?;
        Ok(Context {
            client: self,
            caps,
            default_target: target,
            target,
            pipeline_ready: false,
            loading_gloss_shader_ready: false,
            next_object_handle: target.surface_id + 16,
            viewport: Viewport::from_target(target),
            clear_color: Color {
                r: 0.0,
                g: 0.0,
                b: 0.0,
                a: 1.0,
            },
            blend_state: BlendState::DEFAULT,
            scissor_enabled: false,
            scissor_box: ScissorBox::from_target(target),
        })
    }

    fn request_header(&self) -> *mut RequestHeader {
        self.request_va as *mut RequestHeader
    }

    fn response_header(&self) -> *mut ResponseHeader {
        self.response_va as *mut ResponseHeader
    }

    fn request_payload(&self) -> *mut u8 {
        (self.request_va + size_of::<RequestHeader>() as u64) as *mut u8
    }

    fn clear_mapped_pages(&self) {
        clear_page(self.request_va);
        clear_page(self.response_va);
    }

    fn ensure_bulk_texture_upload_buffer(&mut self) -> Result<(), Error> {
        if self.bulk_upload_ready {
            return Ok(());
        }
        if self.server_process_slot == 0 {
            return Err(Error::MissingService);
        }
        let bulk_upload_va = match vm::alloc_map_pages_into(
            BULK_TEXTURE_UPLOAD_PAGES,
            true,
            &mut self.bulk_upload_paddrs,
        ) {
            Ok(va) => va,
            Err(_) => return Err(Error::BulkBufferAllocFailed),
        };
        self.bulk_upload_va = bulk_upload_va;
        if self.bulk_upload_va == 0 {
            return Err(Error::BulkBufferAllocFailed);
        }
        for paddr in self.bulk_upload_paddrs {
            if paddr < 0x1000
                || syscall::call3(
                    syscall::GRANT_CAP,
                    paddr,
                    self.server_process_slot,
                    PAGE_RIGHT_CPU_READ,
                ) != syscall::OK
            {
                return Err(Error::BulkBufferGrantFailed);
            }
        }
        self.bulk_upload_ready = true;
        Ok(())
    }

    fn begin_request(
        &mut self,
        op: Opcode,
        arg0: u64,
        arg1: u64,
        payload: &[u8],
    ) -> Result<u64, Error> {
        if payload.len() > PAGE_BYTES - size_of::<RequestHeader>() {
            return Err(Error::BufferTooSmall);
        }

        self.clear_mapped_pages();
        let request = self.request_header();
        unsafe {
            write_volatile(addr_of_mut!((*request).magic), REQUEST_MAGIC);
            write_volatile(addr_of_mut!((*request).version), VERSION);
            write_volatile(addr_of_mut!((*request).op), op as u16);
            write_volatile(addr_of_mut!((*request).response_paddr), self.response_paddr);
            write_volatile(addr_of_mut!((*request).arg0), arg0);
            write_volatile(addr_of_mut!((*request).arg1), arg1);
            write_volatile(addr_of_mut!((*request).inline_bytes), payload.len() as u32);
            write_volatile(addr_of_mut!((*request).reserved0), 0);
            write_volatile(addr_of_mut!((*request).session_nonce), self.session_nonce);
        }
        copy_bytes_to_volatile(self.request_payload(), payload);

        let seq = self.next_seq;
        self.next_seq = self.next_seq.wrapping_add(1);
        if self.next_seq == 0 {
            self.next_seq = 1;
        }
        compiler_fence(Ordering::SeqCst);
        unsafe {
            write_volatile(addr_of_mut!((*request).request_seq), seq);
        }

        let result = if self.request_shared {
            syscall::call1(syscall::SIGNAL_ENDPOINT, self.server_endpoint_id)
        } else {
            let send = syscall::call2(
                syscall::SHARE_CAP,
                self.request_paddr,
                self.server_endpoint_id,
            );
            if send == syscall::OK {
                self.request_shared = true;
            }
            send
        };
        if result == syscall::ERR_ENDPOINT {
            return Err(Error::EndpointNotFound);
        }
        if result != syscall::OK {
            return Err(Error::RequestSendFailed);
        }
        Ok(seq)
    }

    fn finish_request(
        &self,
        expected_seq: u64,
        expected_op: Opcode,
    ) -> Result<*mut ResponseHeader, Error> {
        if !self.wait_for_response(expected_seq) {
            return Err(Error::Timeout);
        }
        let response = self.response_header();
        let magic = unsafe { read_volatile(addr_of!((*response).magic)) };
        let version = unsafe { read_volatile(addr_of!((*response).version)) };
        let op = unsafe { read_volatile(addr_of!((*response).op)) };
        let seq = unsafe { read_volatile(addr_of!((*response).response_seq)) };
        if magic != RESPONSE_MAGIC
            || version != VERSION
            || op != expected_op as u16
            || seq != expected_seq
        {
            return Err(Error::InvalidResponse);
        }
        let status = unsafe { read_volatile(addr_of!((*response).status)) };
        parse_status(status).ok_or(Error::InvalidResponse)?;
        Ok(response)
    }

    fn finish_request_ok(
        &self,
        expected_seq: u64,
        expected_op: Opcode,
    ) -> Result<*mut ResponseHeader, Error> {
        let response = self.finish_request(expected_seq, expected_op)?;
        let status = unsafe { read_volatile(addr_of!((*response).status)) };
        match parse_status(status).ok_or(Error::InvalidResponse)? {
            Status::Ok => Ok(response),
            Status::Invalid => Err(Error::Invalid),
            Status::Unavailable => Err(Error::Unavailable),
            Status::IoError => Err(Error::IoError),
        }
    }

    fn wait_for_response(&self, expected_seq: u64) -> bool {
        let response = self.response_header();
        let mut poll_count = 0;
        while poll_count < self.response_poll_limit {
            if unsafe { read_volatile(addr_of!((*response).response_seq)) } == expected_seq {
                return true;
            }
            let _ = syscall::call2(syscall::WAIT_EVENT, 0, 1);
            poll_count += 1;
        }
        false
    }
}

pub struct Context {
    client: Client,
    caps: Caps,
    default_target: RenderTarget,
    target: RenderTarget,
    pipeline_ready: bool,
    loading_gloss_shader_ready: bool,
    next_object_handle: u32,
    viewport: Viewport,
    clear_color: Color,
    blend_state: BlendState,
    scissor_enabled: bool,
    scissor_box: ScissorBox,
}

impl Context {
    pub fn connect_from_registry_shadow() -> Result<Self, Error> {
        Client::connect_from_registry_shadow()?.create_context()
    }

    pub fn connect(options: ConnectOptions) -> Result<Self, Error> {
        Client::connect(options)?.create_context()
    }

    pub const fn caps(&self) -> Caps {
        self.caps
    }

    pub const fn surface(&self) -> RenderTarget {
        self.target
    }

    pub const fn default_surface(&self) -> RenderTarget {
        self.default_target
    }

    pub fn make_surface_current(&mut self, target: RenderTarget) {
        if self.target == target {
            return;
        }
        self.target = target;
        self.viewport = Viewport::from_target(target);
        self.scissor_box = ScissorBox::from_target(target);
        self.pipeline_ready = false;
        self.loading_gloss_shader_ready = false;
        if self.next_object_handle < target.surface_id + 16 {
            self.next_object_handle = target.surface_id + 16;
        }
    }

    pub fn submit_3d(&mut self, commands: &[u8]) -> Result<(), Error> {
        self.client.submit_3d(commands)
    }

    pub fn submit_noop(&mut self) -> Result<(), Error> {
        self.client.submit_noop()
    }

    pub fn present(&mut self) -> Result<PresentInfo, Error> {
        self.client.present_3d()
    }

    pub fn gl_clear_color(&mut self, r: f32, g: f32, b: f32, a: f32) {
        self.clear_color = Color { r, g, b, a };
    }

    pub fn gl_clear(&mut self, mask: u32, scratch: &mut [u8]) -> Result<(), Error> {
        if (mask & !GL_COLOR_BUFFER_BIT) != 0 || (mask & GL_COLOR_BUFFER_BIT) == 0 {
            return Err(Error::Invalid);
        }
        self.clear_color(self.clear_color, scratch)
    }

    pub fn gl_viewport(
        &mut self,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        scratch: &mut [u8],
    ) -> Result<(), Error> {
        if width <= 0 || height <= 0 {
            return Err(Error::Invalid);
        }
        self.viewport = Viewport {
            x,
            y,
            width,
            height,
        };
        if self.pipeline_ready {
            let mut commands = CommandBuffer::new(scratch);
            commands.append_viewport_state(self.viewport)?;
            self.client.submit_3d(commands.bytes())?;
        }
        Ok(())
    }

    pub fn gl_enable(&mut self, cap: u32, scratch: &mut [u8]) -> Result<(), Error> {
        match cap {
            GL_BLEND => {
                self.blend_state.enabled = true;
                self.sync_blend_state(scratch)
            }
            GL_SCISSOR_TEST => {
                self.scissor_enabled = true;
                self.sync_rasterizer_and_scissor_state(scratch)
            }
            _ => Err(Error::Invalid),
        }
    }

    pub fn gl_disable(&mut self, cap: u32, scratch: &mut [u8]) -> Result<(), Error> {
        match cap {
            GL_BLEND => {
                self.blend_state.enabled = false;
                self.sync_blend_state(scratch)
            }
            GL_SCISSOR_TEST => {
                self.scissor_enabled = false;
                self.sync_rasterizer_and_scissor_state(scratch)
            }
            _ => Err(Error::Invalid),
        }
    }

    pub fn gl_blend_func(
        &mut self,
        sfactor: u32,
        dfactor: u32,
        scratch: &mut [u8],
    ) -> Result<(), Error> {
        gl_blend_factor_to_pipe(sfactor)?;
        gl_blend_factor_to_pipe(dfactor)?;
        self.blend_state.src_factor = sfactor;
        self.blend_state.dst_factor = dfactor;
        self.sync_blend_state(scratch)
    }

    pub fn gl_scissor(
        &mut self,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        scratch: &mut [u8],
    ) -> Result<(), Error> {
        let scissor_box = scissor_box_from_i32(x, y, width, height)?;
        self.scissor_box = scissor_box;
        if self.pipeline_ready && self.scissor_enabled {
            let mut commands = CommandBuffer::new(scratch);
            commands.append_scissor_state(scissor_box)?;
            self.client.submit_3d(commands.bytes())?;
        }
        Ok(())
    }

    pub fn gl_buffer_data_vertices(
        &mut self,
        vertices: &[Vertex],
        scratch: &mut [u8],
    ) -> Result<(), Error> {
        let mut commands = CommandBuffer::new(scratch);
        self.append_pipeline_if_needed(&mut commands)?;
        commands.append_vertex_upload(self.target, vertices)?;
        self.client.submit_3d(commands.bytes())?;
        self.pipeline_ready = true;
        Ok(())
    }

    pub fn gl_draw_vertices(
        &mut self,
        primitive: Primitive,
        vertices: &[Vertex],
        texture_resource_id: Option<u32>,
        scratch: &mut [u8],
    ) -> Result<(), Error> {
        let texture = texture_resource_id.map(TextureBinding::bgra8);
        self.gl_draw_vertices_with_texture(primitive, vertices, texture, scratch)
    }

    pub fn gl_draw_alpha_texture_vertices(
        &mut self,
        primitive: Primitive,
        vertices: &[Vertex],
        texture_resource_id: u32,
        scratch: &mut [u8],
    ) -> Result<(), Error> {
        if texture_resource_id == 0 {
            return Err(Error::Invalid);
        }
        self.gl_draw_vertices_with_texture(
            primitive,
            vertices,
            Some(TextureBinding::alpha8(texture_resource_id)),
            scratch,
        )
    }

    fn gl_draw_vertices_with_texture(
        &mut self,
        primitive: Primitive,
        vertices: &[Vertex],
        texture: Option<TextureBinding>,
        scratch: &mut [u8],
    ) -> Result<(), Error> {
        let mut commands = CommandBuffer::new(scratch);
        self.append_pipeline_if_needed(&mut commands)?;
        self.append_draw_binding(&mut commands, texture)?;
        self.append_draw_common_state(&mut commands)?;
        commands.append_vertex_upload(self.target, vertices)?;
        commands.append_draw_arrays(primitive, 0, vertices.len() as u32)?;
        self.client.submit_3d(commands.bytes())?;
        self.pipeline_ready = true;
        Ok(())
    }

    pub fn gl_draw_arrays(
        &mut self,
        primitive: Primitive,
        first: u32,
        count: u32,
        texture_resource_id: Option<u32>,
        scratch: &mut [u8],
    ) -> Result<(), Error> {
        let mut commands = CommandBuffer::new(scratch);
        self.append_pipeline_if_needed(&mut commands)?;
        self.append_draw_binding(
            &mut commands,
            texture_resource_id.map(TextureBinding::bgra8),
        )?;
        self.append_draw_common_state(&mut commands)?;
        commands.append_draw_arrays(primitive, first, count)?;
        self.client.submit_3d(commands.bytes())?;
        self.pipeline_ready = true;
        Ok(())
    }

    pub fn draw_loading_gloss_overlay(
        &mut self,
        vertices: &[Vertex],
        scratch: &mut [u8],
    ) -> Result<(), Error> {
        if vertices.len() != 4 {
            return Err(Error::Invalid);
        }
        self.gl_blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, scratch)?;
        self.gl_enable(GL_BLEND, scratch)?;

        let mut commands = CommandBuffer::new(scratch);
        self.append_pipeline_if_needed(&mut commands)?;
        if !self.loading_gloss_shader_ready {
            commands.append_shader(
                self.loading_gloss_fragment_shader_handle(),
                PIPE_SHADER_FRAGMENT,
                LOADING_GLOSS_FRAGMENT_SHADER,
            )?;
        }
        commands.append_bind_shader(
            self.loading_gloss_fragment_shader_handle(),
            PIPE_SHADER_FRAGMENT,
        )?;
        self.append_draw_common_state(&mut commands)?;
        commands.append_vertex_upload(self.target, vertices)?;
        commands.append_draw_arrays(Primitive::TriangleStrip, 0, vertices.len() as u32)?;
        self.client.submit_3d(commands.bytes())?;
        self.pipeline_ready = true;
        self.loading_gloss_shader_ready = true;
        Ok(())
    }

    pub fn draw_loading_placeholder_preview(
        &mut self,
        phase: f32,
        scratch: &mut [u8],
    ) -> Result<(), Error> {
        self.draw_loading_grid_preview(scratch)?;
        let vertices = loading_gloss_overlay_vertices(phase);
        self.draw_loading_gloss_overlay(&vertices, scratch)
    }

    fn draw_loading_grid_preview(&mut self, scratch: &mut [u8]) -> Result<(), Error> {
        let mut row_vertices = [Vertex::ZERO; DEBUG_GRID_COLUMNS * 4];
        let cell_width = 2.0 / DEBUG_GRID_COLUMNS as f32;
        let cell_height = 2.0 / DEBUG_GRID_ROWS as f32;

        for row in 0..DEBUG_GRID_ROWS {
            let top = 1.0 - row as f32 * cell_height;
            let bottom = top - cell_height;
            let mut vertex_count = 0;
            for col in 0..DEBUG_GRID_COLUMNS {
                let left = -1.0 + col as f32 * cell_width;
                let right = left + cell_width;
                let color = debug_grid_color(col, row);
                row_vertices[vertex_count] = solid_vertex(left, top, color);
                row_vertices[vertex_count + 1] = solid_vertex(left, bottom, color);
                row_vertices[vertex_count + 2] = solid_vertex(right, top, color);
                row_vertices[vertex_count + 3] = solid_vertex(right, bottom, color);
                vertex_count += 4;
            }

            let mut commands = CommandBuffer::new(scratch);
            self.append_pipeline_if_needed(&mut commands)?;
            self.append_draw_binding(&mut commands, None)?;
            self.append_draw_common_state(&mut commands)?;
            commands.append_vertex_upload(self.target, &row_vertices[..vertex_count])?;
            let mut first = 0;
            while first < vertex_count as u32 {
                commands.append_draw_arrays(Primitive::TriangleStrip, first, 4)?;
                first += 4;
            }
            self.client.submit_3d(commands.bytes())?;
            self.pipeline_ready = true;
        }
        Ok(())
    }

    pub fn upload_texture_2d(
        &mut self,
        resource_id_hint: u32,
        width: u32,
        height: u32,
        pixels: &[u8],
    ) -> Result<u32, Error> {
        self.client
            .upload_texture_2d(resource_id_hint, width, height, pixels)
    }

    pub fn update_texture_2d(
        &mut self,
        resource_id: u32,
        x: u32,
        y: u32,
        width: u32,
        height: u32,
        pixels: &[u8],
    ) -> Result<(), Error> {
        self.client
            .update_texture_2d(resource_id, x, y, width, height, pixels)
    }

    pub fn create_alpha_texture_2d(&mut self, width: u32, height: u32) -> Result<u32, Error> {
        self.client.create_alpha_texture_2d(width, height)
    }

    pub fn update_texture_alpha_2d(
        &mut self,
        resource_id: u32,
        x: u32,
        y: u32,
        width: u32,
        height: u32,
        alpha: &[u8],
    ) -> Result<(), Error> {
        self.client
            .update_texture_alpha_2d(resource_id, x, y, width, height, alpha)
    }

    pub fn delete_texture_2d(&mut self, resource_id: u32) -> Result<(), Error> {
        self.client.delete_texture_2d(resource_id)
    }

    pub fn gl_present(&mut self) -> Result<PresentInfo, Error> {
        self.present()
    }

    pub fn clear_color(&mut self, color: Color, scratch: &mut [u8]) -> Result<(), Error> {
        let mut commands = CommandBuffer::new(scratch);
        self.append_pipeline_if_needed(&mut commands)?;
        commands.append_framebuffer_clear(self.target, color)?;
        self.client.submit_3d(commands.bytes())?;
        self.pipeline_ready = true;
        Ok(())
    }

    pub fn clear_color_and_present(
        &mut self,
        color: Color,
        scratch: &mut [u8],
    ) -> Result<PresentInfo, Error> {
        self.clear_color(color, scratch)?;
        self.present()
    }

    pub fn draw_triangles_and_present(
        &mut self,
        vertices: &[Vertex],
        clear_color: Color,
        scratch: &mut [u8],
    ) -> Result<PresentInfo, Error> {
        let mut commands = CommandBuffer::new(scratch);
        self.append_pipeline_if_needed(&mut commands)?;
        commands.append_framebuffer_clear(self.target, clear_color)?;
        self.append_draw_binding(&mut commands, None)?;
        self.append_draw_common_state(&mut commands)?;
        commands.append_vertex_upload(self.target, vertices)?;
        commands.append_draw_arrays(Primitive::Triangles, 0, vertices.len() as u32)?;
        self.client.submit_3d(commands.bytes())?;
        self.pipeline_ready = true;
        self.present()
    }

    pub fn client(&mut self) -> &mut Client {
        &mut self.client
    }

    fn append_pipeline_if_needed(&mut self, commands: &mut CommandBuffer<'_>) -> Result<(), Error> {
        if !self.pipeline_ready {
            commands.append_basic_pipeline(
                self.target,
                self.viewport,
                self.blend_state,
                self.scissor_enabled,
                self.scissor_box,
            )?;
        }
        Ok(())
    }

    fn alloc_object_handle(&mut self) -> u32 {
        let handle = self.next_object_handle;
        self.next_object_handle = self.next_object_handle.wrapping_add(1);
        if self.next_object_handle < self.target.surface_id + 16 {
            self.next_object_handle = self.target.surface_id + 16;
        }
        handle
    }

    fn textured_fragment_shader_handle(&self) -> u32 {
        self.target.surface_id + 3
    }

    fn solid_fragment_shader_handle(&self) -> u32 {
        self.target.surface_id + 7
    }

    fn loading_gloss_fragment_shader_handle(&self) -> u32 {
        self.target.surface_id + 8
    }

    fn append_draw_binding(
        &mut self,
        commands: &mut CommandBuffer<'_>,
        texture: Option<TextureBinding>,
    ) -> Result<(), Error> {
        if let Some(texture) = texture {
            commands
                .append_bind_shader(self.textured_fragment_shader_handle(), PIPE_SHADER_FRAGMENT)?;
            let sampler_view_handle = self.alloc_object_handle();
            let sampler_state_handle = self.alloc_object_handle();
            commands.append_texture_binding(
                texture.resource_id,
                sampler_view_handle,
                sampler_state_handle,
                texture.view_format,
                texture.swizzle,
            )?;
        } else {
            commands
                .append_bind_shader(self.solid_fragment_shader_handle(), PIPE_SHADER_FRAGMENT)?;
        }
        Ok(())
    }

    fn append_draw_common_state(&mut self, commands: &mut CommandBuffer<'_>) -> Result<(), Error> {
        commands.append_framebuffer_state(self.target)?;
        commands.append_bind_object(VIRGL_OBJECT_VERTEX_ELEMENTS, self.target.surface_id + 1)?;
        commands.append_vertex_buffer_binding(self.target)?;
        commands.append_bind_shader(self.target.surface_id + 2, PIPE_SHADER_VERTEX)?;
        commands.append_viewport_state(self.viewport)?;

        let blend_handle = self.alloc_object_handle();
        commands.append_blend_state(blend_handle, self.blend_state)?;
        let rasterizer_handle = self.alloc_object_handle();
        commands.append_rasterizer_state(rasterizer_handle, self.scissor_enabled)?;
        if self.scissor_enabled {
            commands.append_scissor_state(self.scissor_box)?;
        }
        Ok(())
    }

    fn sync_blend_state(&mut self, scratch: &mut [u8]) -> Result<(), Error> {
        if !self.pipeline_ready {
            return Ok(());
        }
        let mut commands = CommandBuffer::new(scratch);
        let blend_handle = self.alloc_object_handle();
        commands.append_blend_state(blend_handle, self.blend_state)?;
        self.client.submit_3d(commands.bytes())
    }

    fn sync_rasterizer_and_scissor_state(&mut self, scratch: &mut [u8]) -> Result<(), Error> {
        if !self.pipeline_ready {
            return Ok(());
        }
        let mut commands = CommandBuffer::new(scratch);
        let rasterizer_handle = self.alloc_object_handle();
        commands.append_rasterizer_state(rasterizer_handle, self.scissor_enabled)?;
        if self.scissor_enabled {
            commands.append_scissor_state(self.scissor_box)?;
        }
        self.client.submit_3d(commands.bytes())
    }
}

pub struct CommandBuffer<'a> {
    buffer: &'a mut [u8],
    index: usize,
}

impl<'a> CommandBuffer<'a> {
    pub fn new(buffer: &'a mut [u8]) -> Self {
        Self { buffer, index: 0 }
    }

    pub fn reset(&mut self) {
        self.index = 0;
    }

    pub fn bytes(&self) -> &[u8] {
        &self.buffer[..self.index]
    }

    fn append_basic_pipeline(
        &mut self,
        target: RenderTarget,
        viewport: Viewport,
        blend_state: BlendState,
        scissor_enabled: bool,
        scissor_box: ScissorBox,
    ) -> Result<(), Error> {
        let ve_handle = target.surface_id + 1;
        let vs_handle = target.surface_id + 2;
        let textured_fs_handle = target.surface_id + 3;
        let blend_handle = target.surface_id + 4;
        let dsa_handle = target.surface_id + 5;
        let rasterizer_handle = target.surface_id + 6;
        let solid_fs_handle = target.surface_id + 7;

        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_CREATE_OBJECT,
            VIRGL_OBJECT_SURFACE,
            VIRGL_OBJ_SURFACE_SIZE,
        ))?;
        self.append_u32(target.surface_id)?;
        self.append_u32(target.resource_id)?;
        self.append_u32(VIRGL_FORMAT_B8G8R8A8_UNORM)?;
        self.append_u32(0)?;
        self.append_u32(0)?;

        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_CREATE_OBJECT,
            VIRGL_OBJECT_VERTEX_ELEMENTS,
            VIRGL_OBJ_VERTEX_ELEMENTS_SIZE_3,
        ))?;
        self.append_u32(ve_handle)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(VIRGL_FORMAT_R32G32B32A32_FLOAT)?;
        self.append_u32(16)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(VIRGL_FORMAT_R32G32B32A32_FLOAT)?;
        self.append_u32(32)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(VIRGL_FORMAT_R32G32B32A32_FLOAT)?;
        self.append_bind_object(VIRGL_OBJECT_VERTEX_ELEMENTS, ve_handle)?;

        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_SET_VERTEX_BUFFERS,
            VIRGL_OBJECT_NULL,
            VIRGL_SET_VERTEX_BUFFERS_SIZE_1,
        ))?;
        self.append_u32(VERTEX_STRIDE)?;
        self.append_u32(0)?;
        self.append_u32(target.vertex_buffer_id)?;

        self.append_shader(vs_handle, PIPE_SHADER_VERTEX, VERTEX_SHADER)?;
        self.append_bind_shader(vs_handle, PIPE_SHADER_VERTEX)?;
        self.append_shader(textured_fs_handle, PIPE_SHADER_FRAGMENT, FRAGMENT_SHADER)?;
        self.append_shader(solid_fs_handle, PIPE_SHADER_FRAGMENT, SOLID_FRAGMENT_SHADER)?;
        self.append_bind_shader(solid_fs_handle, PIPE_SHADER_FRAGMENT)?;

        self.append_blend_state(blend_handle, blend_state)?;

        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_CREATE_OBJECT,
            VIRGL_OBJECT_DSA,
            VIRGL_OBJ_DSA_SIZE,
        ))?;
        self.append_u32(dsa_handle)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_bind_object(VIRGL_OBJECT_DSA, dsa_handle)?;

        self.append_rasterizer_state(rasterizer_handle, scissor_enabled)?;
        if scissor_enabled {
            self.append_scissor_state(scissor_box)?;
        }

        self.append_viewport_state(viewport)?;
        Ok(())
    }

    fn append_blend_state(
        &mut self,
        blend_handle: u32,
        blend_state: BlendState,
    ) -> Result<(), Error> {
        let src_factor = gl_blend_factor_to_pipe(blend_state.src_factor)?;
        let dst_factor = gl_blend_factor_to_pipe(blend_state.dst_factor)?;
        let rt_state = ((blend_state.enabled as u32) & 0x1)
            | (PIPE_BLEND_ADD << 1)
            | (src_factor << 4)
            | (dst_factor << 9)
            | (PIPE_BLEND_ADD << 14)
            | (src_factor << 17)
            | (dst_factor << 22)
            | (PIPE_MASK_RGBA << 27);

        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_CREATE_OBJECT,
            VIRGL_OBJECT_BLEND,
            VIRGL_OBJ_BLEND_SIZE,
        ))?;
        self.append_u32(blend_handle)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(rt_state)?;
        for _ in 1..8 {
            self.append_u32(0)?;
        }
        self.append_bind_object(VIRGL_OBJECT_BLEND, blend_handle)
    }

    fn append_rasterizer_state(
        &mut self,
        rasterizer_handle: u32,
        scissor_enabled: bool,
    ) -> Result<(), Error> {
        let rasterizer_s0 = (1 << 1) | ((scissor_enabled as u32) << 14) | (1 << 29) | (1 << 30);
        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_CREATE_OBJECT,
            VIRGL_OBJECT_RASTERIZER,
            VIRGL_OBJ_RASTERIZER_SIZE,
        ))?;
        self.append_u32(rasterizer_handle)?;
        self.append_u32(rasterizer_s0)?;
        self.append_f32(1.0)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_f32(1.0)?;
        self.append_f32(0.0)?;
        self.append_f32(0.0)?;
        self.append_f32(0.0)?;
        self.append_bind_object(VIRGL_OBJECT_RASTERIZER, rasterizer_handle)
    }

    pub fn append_texture_binding(
        &mut self,
        texture_resource_id: u32,
        sampler_view_handle: u32,
        sampler_state_handle: u32,
        view_format: u32,
        swizzle: u32,
    ) -> Result<(), Error> {
        if texture_resource_id == 0 {
            return Err(Error::Invalid);
        }

        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_CREATE_OBJECT,
            VIRGL_OBJECT_SAMPLER_VIEW,
            VIRGL_OBJ_SAMPLER_VIEW_SIZE,
        ))?;
        self.append_u32(sampler_view_handle)?;
        self.append_u32(texture_resource_id)?;
        self.append_u32(view_format)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(swizzle)?;

        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_CREATE_OBJECT,
            VIRGL_OBJECT_SAMPLER_STATE,
            VIRGL_OBJ_SAMPLER_STATE_SIZE,
        ))?;
        self.append_u32(sampler_state_handle)?;
        self.append_u32(LINEAR_CLAMP_SAMPLER_S0)?;
        self.append_f32(0.0)?;
        self.append_f32(0.0)?;
        self.append_f32(1000.0)?;
        self.append_f32(0.0)?;
        self.append_f32(0.0)?;
        self.append_f32(0.0)?;
        self.append_f32(0.0)?;

        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_SET_SAMPLER_VIEWS,
            VIRGL_OBJECT_NULL,
            VIRGL_SET_SAMPLER_VIEWS_SIZE_1,
        ))?;
        self.append_u32(PIPE_SHADER_FRAGMENT)?;
        self.append_u32(0)?;
        self.append_u32(sampler_view_handle)?;

        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_BIND_SAMPLER_STATES,
            VIRGL_OBJECT_NULL,
            VIRGL_BIND_SAMPLER_STATES_SIZE_1,
        ))?;
        self.append_u32(PIPE_SHADER_FRAGMENT)?;
        self.append_u32(0)?;
        self.append_u32(sampler_state_handle)?;
        Ok(())
    }

    pub fn append_viewport_state(&mut self, viewport: Viewport) -> Result<(), Error> {
        if viewport.width <= 0 || viewport.height <= 0 {
            return Err(Error::Invalid);
        }
        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_SET_VIEWPORT_STATE,
            VIRGL_OBJECT_NULL,
            VIRGL_SET_VIEWPORT_STATE_SIZE_1,
        ))?;
        self.append_u32(0)?;
        self.append_f32(viewport.width as f32 / 2.0)?;
        self.append_f32(-(viewport.height as f32) / 2.0)?;
        self.append_f32(0.5)?;
        self.append_f32(viewport.x as f32 + viewport.width as f32 / 2.0)?;
        self.append_f32(viewport.y as f32 + viewport.height as f32 / 2.0)?;
        self.append_f32(0.5)?;
        Ok(())
    }

    fn append_scissor_state(&mut self, scissor_box: ScissorBox) -> Result<(), Error> {
        let min = scissor_box.x as u32 | ((scissor_box.y as u32) << 16);
        let max = scissor_box.max_x() as u32 | ((scissor_box.max_y() as u32) << 16);
        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_SET_SCISSOR_STATE,
            VIRGL_OBJECT_NULL,
            VIRGL_SET_SCISSOR_STATE_SIZE_1,
        ))?;
        self.append_u32(0)?;
        self.append_u32(min)?;
        self.append_u32(max)?;
        Ok(())
    }

    pub fn append_framebuffer_clear(
        &mut self,
        target: RenderTarget,
        color: Color,
    ) -> Result<(), Error> {
        self.append_framebuffer_state(target)?;

        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_CLEAR,
            VIRGL_OBJECT_NULL,
            VIRGL_OBJ_CLEAR_SIZE,
        ))?;
        self.append_u32(PIPE_CLEAR_COLOR0)?;
        self.append_f32(color.r)?;
        self.append_f32(color.g)?;
        self.append_f32(color.b)?;
        self.append_f32(color.a)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        Ok(())
    }

    fn append_framebuffer_state(&mut self, target: RenderTarget) -> Result<(), Error> {
        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_SET_FRAMEBUFFER_STATE,
            VIRGL_OBJECT_NULL,
            VIRGL_SET_FRAMEBUFFER_STATE_SIZE_1,
        ))?;
        self.append_u32(1)?;
        self.append_u32(0)?;
        self.append_u32(target.surface_id)
    }

    fn append_vertex_buffer_binding(&mut self, target: RenderTarget) -> Result<(), Error> {
        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_SET_VERTEX_BUFFERS,
            VIRGL_OBJECT_NULL,
            VIRGL_SET_VERTEX_BUFFERS_SIZE_1,
        ))?;
        self.append_u32(VERTEX_STRIDE)?;
        self.append_u32(0)?;
        self.append_u32(target.vertex_buffer_id)
    }

    pub fn append_vertex_upload(
        &mut self,
        target: RenderTarget,
        vertices: &[Vertex],
    ) -> Result<(), Error> {
        if vertices.is_empty() {
            return Err(Error::Invalid);
        }
        let vertex_bytes = vertices.len() * size_of::<Vertex>();
        let vertex_bytes_u32 = vertex_bytes as u32;
        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_RESOURCE_INLINE_WRITE,
            VIRGL_OBJECT_NULL,
            11 + vertex_bytes_u32.div_ceil(4),
        ))?;
        self.append_u32(target.vertex_buffer_id)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(vertex_bytes_u32)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(vertex_bytes_u32)?;
        self.append_u32(1)?;
        self.append_u32(1)?;
        for vertex in vertices {
            self.append_f32(vertex.x)?;
            self.append_f32(vertex.y)?;
            self.append_f32(vertex.z)?;
            self.append_f32(vertex.w)?;
            self.append_f32(vertex.r)?;
            self.append_f32(vertex.g)?;
            self.append_f32(vertex.b)?;
            self.append_f32(vertex.a)?;
            self.append_f32(vertex.u)?;
            self.append_f32(vertex.v)?;
            self.append_f32(vertex.s)?;
            self.append_f32(vertex.t)?;
        }
        Ok(())
    }

    pub fn append_draw_arrays(
        &mut self,
        primitive: Primitive,
        first: u32,
        count: u32,
    ) -> Result<(), Error> {
        if count == 0 {
            return Err(Error::Invalid);
        }
        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_DRAW_VBO,
            VIRGL_OBJECT_NULL,
            VIRGL_DRAW_VBO_SIZE,
        ))?;
        self.append_u32(first)?;
        self.append_u32(count)?;
        self.append_u32(primitive_raw(primitive))?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(0)?;
        self.append_u32(first + count - 1)?;
        self.append_u32(0)?;
        Ok(())
    }

    fn append_bind_object(&mut self, object: u32, handle: u32) -> Result<(), Error> {
        self.append_u32(virgl_cmd0(VIRGL_CCMD_BIND_OBJECT, object, 1))?;
        self.append_u32(handle)
    }

    fn append_shader(&mut self, handle: u32, shader_type: u32, source: &[u8]) -> Result<(), Error> {
        let shader_len = source.len() as u32 + 1;
        let padded_dwords = (shader_len + 3) / 4;
        self.append_u32(virgl_cmd0(
            VIRGL_CCMD_CREATE_OBJECT,
            VIRGL_OBJECT_SHADER,
            VIRGL_OBJ_SHADER_BASE_SIZE + padded_dwords,
        ))?;
        self.append_u32(handle)?;
        self.append_u32(shader_type)?;
        self.append_u32(shader_len)?;
        self.append_u32(300)?;
        self.append_u32(0)?;
        self.append_bytes_nul_padded(source)
    }

    fn append_bind_shader(&mut self, handle: u32, shader_type: u32) -> Result<(), Error> {
        self.append_u32(virgl_cmd0(VIRGL_CCMD_BIND_SHADER, VIRGL_OBJECT_NULL, 2))?;
        self.append_u32(handle)?;
        self.append_u32(shader_type)
    }

    fn append_u32(&mut self, value: u32) -> Result<(), Error> {
        let dst = self.reserve(4)?;
        let bytes = value.to_le_bytes();
        dst.copy_from_slice(&bytes);
        Ok(())
    }

    fn append_f32(&mut self, value: f32) -> Result<(), Error> {
        self.append_u32(value.to_bits())
    }

    fn append_bytes_nul_padded(&mut self, source: &[u8]) -> Result<(), Error> {
        let padded_len = (source.len() + 1 + 3) & !3;
        let dst = self.reserve(padded_len)?;
        let mut index = 0;
        while index < source.len() {
            dst[index] = source[index];
            index += 1;
        }
        while index < padded_len {
            dst[index] = 0;
            index += 1;
        }
        Ok(())
    }

    fn reserve(&mut self, byte_count: usize) -> Result<&mut [u8], Error> {
        if self.index + byte_count > self.buffer.len() {
            return Err(Error::BufferTooSmall);
        }
        let start = self.index;
        self.index += byte_count;
        Ok(&mut self.buffer[start..self.index])
    }
}

fn virgl_cmd0(command: u32, object: u32, len: u32) -> u32 {
    command | (object << 8) | (len << 16)
}

fn primitive_raw(primitive: Primitive) -> u32 {
    match primitive {
        Primitive::Triangles => PIPE_PRIM_TRIANGLES,
        Primitive::TriangleStrip => PIPE_PRIM_TRIANGLE_STRIP,
    }
}

fn solid_vertex(x: f32, y: f32, color: [f32; 4]) -> Vertex {
    Vertex {
        x,
        y,
        z: 0.0,
        w: 1.0,
        r: color[0],
        g: color[1],
        b: color[2],
        a: color[3],
        u: 0.0,
        v: 0.0,
        s: 0.0,
        t: 1.0,
    }
}

fn gloss_vertex(x: f32, y: f32, u: f32, phase: f32, strength: f32) -> Vertex {
    Vertex {
        x,
        y,
        z: 0.0,
        w: 1.0,
        r: 0.92,
        g: 0.98,
        b: 1.0,
        a: 0.42,
        u,
        v: -0.5,
        s: phase,
        t: strength,
    }
}

fn loading_gloss_overlay_vertices(phase: f32) -> [Vertex; 4] {
    let strength = -0.78;
    let cycles = 1.0;
    [
        gloss_vertex(-1.0, 1.0, 0.0, phase, strength),
        gloss_vertex(-1.0, -1.0, 0.0, phase, strength),
        gloss_vertex(1.0, 1.0, cycles, phase, strength),
        gloss_vertex(1.0, -1.0, cycles, phase, strength),
    ]
}

fn debug_grid_color(col: usize, row: usize) -> [f32; 4] {
    let x = ratio_usize(col, DEBUG_GRID_COLUMNS - 1);
    let y = ratio_usize(row, DEBUG_GRID_ROWS - 1);
    let parity = if ((col ^ row) & 1) == 0 { 0.58 } else { 0.96 };
    let vertical = 1.0 - y * 0.28;
    let shade = parity * vertical;

    let red = clamp_f32(0.02 + y * 0.92 + x * 0.18);
    let green = clamp_f32(0.88 - y * 0.30 + (1.0 - x) * 0.18);
    let blue = clamp_f32(0.06 + x * 0.86 + y * 0.42);
    [red * shade, green * shade, blue * shade, 1.0]
}

fn ratio_usize(value: usize, max: usize) -> f32 {
    if max == 0 {
        0.0
    } else {
        value as f32 / max as f32
    }
}

fn clamp_f32(value: f32) -> f32 {
    if value < 0.0 {
        0.0
    } else if value > 1.0 {
        1.0
    } else {
        value
    }
}

fn primitive_from_gl_mode(mode: u32) -> Option<Primitive> {
    match mode {
        GL_TRIANGLES => Some(Primitive::Triangles),
        GL_TRIANGLE_STRIP => Some(Primitive::TriangleStrip),
        _ => None,
    }
}

#[repr(C)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct CapglCaps {
    pub features: u64,
    pub capset_id: u32,
    pub capset_max_version: u32,
    pub width: u32,
    pub height: u32,
    pub resource_id: u32,
    pub surface_id: u32,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct CapglSurfaceInfo {
    pub width: u32,
    pub height: u32,
    pub resource_id: u32,
    pub surface_id: u32,
    pub swap_count: u32,
}

#[unsafe(no_mangle)]
pub extern "C" fn capglInit() -> u32 {
    match ensure_global_context().and_then(|ctx| ensure_default_capgl_binding(ctx).map(|_| ())) {
        Ok(()) => GL_TRUE,
        Err(err) => {
            record_gl_error(error_to_gl_error(err));
            GL_FALSE
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn capglCreateContext() -> u32 {
    match ensure_global_context().and_then(|ctx| {
        ensure_default_capgl_binding(ctx)?;
        create_capgl_context()
    }) {
        Ok(handle) => handle,
        Err(err) => {
            record_gl_error(error_to_gl_error(err));
            0
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn capglCreateSurface(width: u32, height: u32) -> u32 {
    match ensure_global_context().and_then(|ctx| {
        ensure_default_capgl_binding(ctx)?;
        create_capgl_surface(ctx, width, height)
    }) {
        Ok(handle) => handle,
        Err(err) => {
            record_gl_error(error_to_gl_error(err));
            0
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn capglMakeCurrent(context: u32, surface: u32) -> u32 {
    if context == 0 && surface == 0 {
        unsafe {
            write_volatile(addr_of_mut!(GLOBAL_CURRENT_CAPGL_CONTEXT), 0);
            write_volatile(addr_of_mut!(GLOBAL_CURRENT_CAPGL_SURFACE), 0);
        }
        return GL_TRUE;
    }
    if context == 0 || surface == 0 {
        record_gl_error(GL_INVALID_VALUE);
        return GL_FALSE;
    }
    match ensure_global_context().and_then(|ctx| {
        ensure_default_capgl_binding(ctx)?;
        make_capgl_current(ctx, context, surface)
    }) {
        Ok(()) => GL_TRUE,
        Err(err) => {
            record_gl_error(error_to_gl_error(err));
            GL_FALSE
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn capglGetCurrentContext() -> u32 {
    unsafe { read_volatile(addr_of!(GLOBAL_CURRENT_CAPGL_CONTEXT)) }
}

#[unsafe(no_mangle)]
pub extern "C" fn capglGetCurrentSurface() -> u32 {
    unsafe { read_volatile(addr_of!(GLOBAL_CURRENT_CAPGL_SURFACE)) }
}

#[unsafe(no_mangle)]
pub extern "C" fn capglGetSurfaceInfo(surface: u32, out_info: *mut CapglSurfaceInfo) -> u32 {
    if surface == 0 || out_info.is_null() {
        record_gl_error(GL_INVALID_VALUE);
        return GL_FALSE;
    }
    match find_capgl_surface(surface) {
        Some(index) => unsafe {
            let state = read_volatile(capgl_surface_ptr(index));
            write_volatile(
                out_info,
                CapglSurfaceInfo {
                    width: state.width,
                    height: state.height,
                    resource_id: state.resource_id,
                    surface_id: state.surface_id,
                    swap_count: state.swap_count,
                },
            );
            GL_TRUE
        },
        None => {
            record_gl_error(GL_INVALID_VALUE);
            GL_FALSE
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn capglSwapBuffers(surface: u32) -> u32 {
    if surface == 0 {
        record_gl_error(GL_INVALID_VALUE);
        return GL_FALSE;
    }
    match ensure_global_context().and_then(|ctx| {
        ensure_default_capgl_binding(ctx)?;
        let current_surface = unsafe { read_volatile(addr_of!(GLOBAL_CURRENT_CAPGL_SURFACE)) };
        if current_surface != surface {
            return Err(Error::Invalid);
        }
        let Some(index) = find_capgl_surface(surface) else {
            return Err(Error::Invalid);
        };
        let is_default = unsafe { read_volatile(addr_of!((*capgl_surface_ptr(index)).is_default)) };
        if is_default {
            ctx.gl_present()?;
        }
        unsafe {
            let state = capgl_surface_ptr(index);
            let swap_count = read_volatile(addr_of!((*state).swap_count));
            write_volatile(
                addr_of_mut!((*state).swap_count),
                swap_count.wrapping_add(1),
            );
        }
        Ok(())
    }) {
        Ok(()) => GL_TRUE,
        Err(err) => {
            record_gl_error(error_to_gl_error(err));
            GL_FALSE
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn capglGetCaps(out_caps: *mut CapglCaps) -> u32 {
    if out_caps.is_null() {
        record_gl_error(GL_INVALID_VALUE);
        return GL_FALSE;
    }
    match ensure_global_context() {
        Ok(ctx) => {
            let caps = ctx.caps();
            let target = ctx.surface();
            unsafe {
                write_volatile(
                    out_caps,
                    CapglCaps {
                        features: caps.features,
                        capset_id: caps.capset_id,
                        capset_max_version: caps.capset_max_version,
                        width: target.width,
                        height: target.height,
                        resource_id: target.resource_id,
                        surface_id: target.surface_id,
                    },
                );
            }
            GL_TRUE
        }
        Err(err) => {
            record_gl_error(error_to_gl_error(err));
            GL_FALSE
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glClearColor(red: f32, green: f32, blue: f32, alpha: f32) {
    match ensure_global_context() {
        Ok(ctx) => ctx.gl_clear_color(red, green, blue, alpha),
        Err(err) => record_gl_error(error_to_gl_error(err)),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glClear(mask: u32) {
    if (mask & !GL_COLOR_BUFFER_BIT) != 0 || (mask & GL_COLOR_BUFFER_BIT) == 0 {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    let result = match ensure_global_context() {
        Ok(ctx) => {
            let scratch = global_scratch();
            ctx.gl_clear(mask, scratch)
        }
        Err(err) => Err(err),
    };
    if let Err(err) = result {
        record_gl_error(error_to_gl_error(err));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glColor4f(red: f32, green: f32, blue: f32, alpha: f32) {
    if !red.is_finite() || !green.is_finite() || !blue.is_finite() || !alpha.is_finite() {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    unsafe {
        write_volatile(
            addr_of_mut!(GLOBAL_CURRENT_COLOR),
            [red, green, blue, alpha],
        );
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glViewport(x: i32, y: i32, width: i32, height: i32) {
    if width <= 0 || height <= 0 {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    let result = match ensure_global_context() {
        Ok(ctx) => {
            let scratch = global_scratch();
            ctx.gl_viewport(x, y, width, height, scratch)
        }
        Err(err) => Err(err),
    };
    if let Err(err) = result {
        record_gl_error(error_to_gl_error(err));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glMatrixMode(mode: u32) {
    match mode {
        GL_MODELVIEW | GL_PROJECTION => unsafe {
            write_volatile(addr_of_mut!(GLOBAL_MATRIX_MODE), mode);
        },
        _ => record_gl_error(GL_INVALID_ENUM),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glLoadIdentity() {
    if let Err(err) = write_current_matrix(IDENTITY_MATRIX) {
        record_gl_error(error_to_gl_error(err));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glPushMatrix() {
    if let Err(error) = push_current_matrix() {
        record_gl_error(error);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glPopMatrix() {
    if let Err(error) = pop_current_matrix() {
        record_gl_error(error);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glOrtho(left: f64, right: f64, bottom: f64, top: f64, z_near: f64, z_far: f64) {
    if !left.is_finite()
        || !right.is_finite()
        || !bottom.is_finite()
        || !top.is_finite()
        || !z_near.is_finite()
        || !z_far.is_finite()
        || left == right
        || bottom == top
        || z_near == z_far
    {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }

    let tx = -((right + left) / (right - left)) as f32;
    let ty = -((top + bottom) / (top - bottom)) as f32;
    let tz = -((z_far + z_near) / (z_far - z_near)) as f32;
    let matrix = [
        (2.0 / (right - left)) as f32,
        0.0,
        0.0,
        0.0,
        0.0,
        (2.0 / (top - bottom)) as f32,
        0.0,
        0.0,
        0.0,
        0.0,
        (-2.0 / (z_far - z_near)) as f32,
        0.0,
        tx,
        ty,
        tz,
        1.0,
    ];
    if let Err(err) = multiply_current_matrix(matrix) {
        record_gl_error(error_to_gl_error(err));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glTranslatef(x: f32, y: f32, z: f32) {
    if !x.is_finite() || !y.is_finite() || !z.is_finite() {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    let matrix = [
        1.0, 0.0, 0.0, 0.0, //
        0.0, 1.0, 0.0, 0.0, //
        0.0, 0.0, 1.0, 0.0, //
        x, y, z, 1.0,
    ];
    if let Err(err) = multiply_current_matrix(matrix) {
        record_gl_error(error_to_gl_error(err));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glScalef(x: f32, y: f32, z: f32) {
    if !x.is_finite() || !y.is_finite() || !z.is_finite() {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    let matrix = [
        x, 0.0, 0.0, 0.0, //
        0.0, y, 0.0, 0.0, //
        0.0, 0.0, z, 0.0, //
        0.0, 0.0, 0.0, 1.0,
    ];
    if let Err(err) = multiply_current_matrix(matrix) {
        record_gl_error(error_to_gl_error(err));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glGetIntegerv(pname: u32, params: *mut i32) {
    if params.is_null() {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    match pname {
        GL_VIEWPORT => match ensure_global_context() {
            Ok(ctx) => unsafe {
                write_volatile(params.add(0), ctx.viewport.x);
                write_volatile(params.add(1), ctx.viewport.y);
                write_volatile(params.add(2), ctx.viewport.width);
                write_volatile(params.add(3), ctx.viewport.height);
            },
            Err(err) => record_gl_error(error_to_gl_error(err)),
        },
        _ => record_gl_error(GL_INVALID_ENUM),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn capglMakePixelRectMvp(
    x: f32,
    y: f32,
    width: f32,
    height: f32,
    viewport_width: f32,
    viewport_height: f32,
    out_matrix: *mut f32,
) -> u32 {
    if out_matrix.is_null()
        || !x.is_finite()
        || !y.is_finite()
        || !width.is_finite()
        || !height.is_finite()
        || !viewport_width.is_finite()
        || !viewport_height.is_finite()
        || width <= 0.0
        || height <= 0.0
        || viewport_width <= 0.0
        || viewport_height <= 0.0
    {
        record_gl_error(GL_INVALID_VALUE);
        return GL_FALSE;
    }

    let scale_x = 2.0 * width / viewport_width;
    let scale_y = -2.0 * height / viewport_height;
    let translate_x = 2.0 * x / viewport_width - 1.0;
    let translate_y = 1.0 - 2.0 * y / viewport_height;
    let matrix = [
        scale_x,
        0.0,
        0.0,
        0.0,
        0.0,
        scale_y,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        translate_x,
        translate_y,
        0.0,
        1.0,
    ];
    for (index, value) in matrix.iter().enumerate() {
        unsafe {
            write_volatile(out_matrix.add(index), *value);
        }
    }
    GL_TRUE
}

#[unsafe(no_mangle)]
pub extern "C" fn glEnable(cap: u32) {
    if cap != GL_BLEND && cap != GL_SCISSOR_TEST {
        record_gl_error(GL_INVALID_ENUM);
        return;
    }
    let result = match ensure_global_context() {
        Ok(ctx) => {
            let scratch = global_scratch();
            ctx.gl_enable(cap, scratch)
        }
        Err(err) => Err(err),
    };
    if let Err(err) = result {
        record_gl_error(error_to_gl_error(err));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glDisable(cap: u32) {
    if cap != GL_BLEND && cap != GL_SCISSOR_TEST {
        record_gl_error(GL_INVALID_ENUM);
        return;
    }
    let result = match ensure_global_context() {
        Ok(ctx) => {
            let scratch = global_scratch();
            ctx.gl_disable(cap, scratch)
        }
        Err(err) => Err(err),
    };
    if let Err(err) = result {
        record_gl_error(error_to_gl_error(err));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glBlendFunc(sfactor: u32, dfactor: u32) {
    if gl_blend_factor_to_pipe(sfactor).is_err() || gl_blend_factor_to_pipe(dfactor).is_err() {
        record_gl_error(GL_INVALID_ENUM);
        return;
    }
    let result = match ensure_global_context() {
        Ok(ctx) => {
            let scratch = global_scratch();
            ctx.gl_blend_func(sfactor, dfactor, scratch)
        }
        Err(err) => Err(err),
    };
    if let Err(err) = result {
        record_gl_error(error_to_gl_error(err));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glScissor(x: i32, y: i32, width: i32, height: i32) {
    let result = match ensure_global_context() {
        Ok(ctx) => {
            let scratch = global_scratch();
            ctx.gl_scissor(x, y, width, height, scratch)
        }
        Err(err) => Err(err),
    };
    if let Err(err) = result {
        record_gl_error(error_to_gl_error(err));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glGenBuffers(n: i32, buffers: *mut u32) {
    if n < 0 || (n > 0 && buffers.is_null()) {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    let count = n as usize;
    for index in 0..count {
        let slot = match find_free_buffer_slot() {
            Some(slot) => slot,
            None => {
                record_gl_error(GL_OUT_OF_MEMORY);
                return;
            }
        };
        let name = next_buffer_name();
        unsafe {
            let buffer = gl_buffer_ptr(slot);
            write_volatile(addr_of_mut!((*buffer).name), name);
            write_volatile(addr_of_mut!((*buffer).len), 0);
            write_volatile(addr_of_mut!((*buffer).usage), GL_STATIC_DRAW);
            write_volatile(buffers.add(index), name);
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glBindBuffer(target: u32, buffer: u32) {
    if target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER {
        record_gl_error(GL_INVALID_ENUM);
        return;
    }
    if buffer != 0 && find_buffer_slot(buffer).is_none() {
        match find_free_buffer_slot() {
            Some(slot) => unsafe {
                write_volatile(addr_of_mut!((*gl_buffer_ptr(slot)).name), buffer);
            },
            None => {
                record_gl_error(GL_OUT_OF_MEMORY);
                return;
            }
        }
    }
    unsafe {
        match target {
            GL_ARRAY_BUFFER => write_volatile(addr_of_mut!(GLOBAL_CURRENT_ARRAY_BUFFER), buffer),
            GL_ELEMENT_ARRAY_BUFFER => {
                write_volatile(addr_of_mut!(GLOBAL_CURRENT_ELEMENT_ARRAY_BUFFER), buffer)
            }
            _ => {}
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glDeleteBuffers(n: i32, buffers: *const u32) {
    if n < 0 || (n > 0 && buffers.is_null()) {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    for index in 0..n as usize {
        let name = unsafe { read_volatile(buffers.add(index)) };
        if name == 0 {
            continue;
        }
        let Some(slot) = find_buffer_slot(name) else {
            continue;
        };
        unsafe {
            if read_volatile(addr_of!(GLOBAL_CURRENT_ARRAY_BUFFER)) == name {
                write_volatile(addr_of_mut!(GLOBAL_CURRENT_ARRAY_BUFFER), 0);
            }
            if read_volatile(addr_of!(GLOBAL_CURRENT_ELEMENT_ARRAY_BUFFER)) == name {
                write_volatile(addr_of_mut!(GLOBAL_CURRENT_ELEMENT_ARRAY_BUFFER), 0);
            }
            for attrib_index in 0..MAX_GL_VERTEX_ATTRIBS {
                let attrib = vertex_attrib_ptr(attrib_index);
                if read_volatile(addr_of!((*attrib).buffer_name)) == name {
                    write_volatile(addr_of_mut!((*attrib).enabled), false);
                    write_volatile(addr_of_mut!((*attrib).buffer_name), 0);
                }
            }
            write_volatile(gl_buffer_ptr(slot), GlBuffer::EMPTY);
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glGenTextures(n: i32, textures: *mut u32) {
    if n < 0 || (n > 0 && textures.is_null()) {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    let count = n as usize;
    for index in 0..count {
        let slot = match find_free_texture_slot() {
            Some(slot) => slot,
            None => {
                record_gl_error(GL_OUT_OF_MEMORY);
                return;
            }
        };
        let name = next_texture_name();
        unsafe {
            init_texture_slot(slot, name);
            write_volatile(textures.add(index), name);
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glDeleteTextures(n: i32, textures: *const u32) {
    if n < 0 || (n > 0 && textures.is_null()) {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    for index in 0..n as usize {
        let name = unsafe { read_volatile(textures.add(index)) };
        if name == 0 {
            continue;
        }
        let Some(slot) = find_texture_slot(name) else {
            continue;
        };
        let resource_id = unsafe { read_volatile(addr_of!((*gl_texture_ptr(slot)).resource_id)) };
        let owns_resource =
            unsafe { read_volatile(addr_of!((*gl_texture_ptr(slot)).owns_resource)) };
        if resource_id != 0 && owns_resource {
            if let Err(err) =
                ensure_global_context().and_then(|ctx| ctx.delete_texture_2d(resource_id))
            {
                record_gl_error(error_to_gl_error(err));
                return;
            }
        }
        unsafe {
            for unit in 0..MAX_GL_TEXTURE_UNITS {
                let binding = (addr_of_mut!(GLOBAL_TEXTURE_BINDINGS_2D) as *mut u32).add(unit);
                if read_volatile(binding) == name {
                    write_volatile(binding, 0);
                }
            }
            write_volatile(gl_texture_ptr(slot), GlTexture::EMPTY);
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glActiveTexture(texture: u32) {
    if texture < GL_TEXTURE0 || texture >= GL_TEXTURE0 + MAX_GL_TEXTURE_UNITS as u32 {
        record_gl_error(GL_INVALID_ENUM);
        return;
    }
    unsafe {
        write_volatile(
            addr_of_mut!(GLOBAL_ACTIVE_TEXTURE_UNIT),
            (texture - GL_TEXTURE0) as usize,
        );
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glBindTexture(target: u32, texture: u32) {
    if target != GL_TEXTURE_2D {
        record_gl_error(GL_INVALID_ENUM);
        return;
    }
    if texture != 0 && find_texture_slot(texture).is_none() {
        match find_free_texture_slot() {
            Some(slot) => {
                init_texture_slot(slot, texture);
            }
            None => {
                record_gl_error(GL_OUT_OF_MEMORY);
                return;
            }
        }
    }
    let unit = active_texture_unit();
    unsafe {
        write_volatile(
            (addr_of_mut!(GLOBAL_TEXTURE_BINDINGS_2D) as *mut u32).add(unit),
            texture,
        );
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glTexParameteri(target: u32, pname: u32, param: i32) {
    if target != GL_TEXTURE_2D {
        record_gl_error(GL_INVALID_ENUM);
        return;
    }
    let texture = match bound_texture_2d() {
        Some(texture) => texture,
        None => {
            record_gl_error(GL_INVALID_OPERATION);
            return;
        }
    };
    match pname {
        GL_TEXTURE_MIN_FILTER | GL_TEXTURE_MAG_FILTER => {
            if param != GL_NEAREST as i32 && param != GL_LINEAR as i32 {
                record_gl_error(GL_INVALID_VALUE);
                return;
            }
        }
        GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T => {
            if param != GL_CLAMP_TO_EDGE as i32 {
                record_gl_error(GL_INVALID_VALUE);
                return;
            }
        }
        _ => {
            record_gl_error(GL_INVALID_ENUM);
            return;
        }
    }
    unsafe {
        match pname {
            GL_TEXTURE_MIN_FILTER => write_volatile(addr_of_mut!((*texture).min_filter), param),
            GL_TEXTURE_MAG_FILTER => write_volatile(addr_of_mut!((*texture).mag_filter), param),
            GL_TEXTURE_WRAP_S => write_volatile(addr_of_mut!((*texture).wrap_s), param),
            GL_TEXTURE_WRAP_T => write_volatile(addr_of_mut!((*texture).wrap_t), param),
            _ => {}
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glTexImage2D(
    target: u32,
    level: i32,
    internalformat: i32,
    width: i32,
    height: i32,
    border: i32,
    format: u32,
    type_: u32,
    pixels: *const c_void,
) {
    if target != GL_TEXTURE_2D {
        record_gl_error(GL_INVALID_ENUM);
        return;
    }
    if level != 0 || border != 0 {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    if width <= 0 || height <= 0 {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    if internalformat != GL_RGBA as i32 || format != GL_RGBA || type_ != GL_UNSIGNED_BYTE {
        record_gl_error(GL_INVALID_ENUM);
        return;
    }
    let Some(byte_len) = texture_byte_len(width, height) else {
        record_gl_error(GL_OUT_OF_MEMORY);
        return;
    };
    if byte_len > MAX_GL_TEXTURE_BYTES {
        record_gl_error(GL_OUT_OF_MEMORY);
        return;
    }
    let texture = match bound_texture_2d() {
        Some(texture) => texture,
        None => {
            record_gl_error(GL_INVALID_OPERATION);
            return;
        }
    };
    let resource_id_hint = unsafe {
        let old_width = read_volatile(addr_of!((*texture).width));
        let old_height = read_volatile(addr_of!((*texture).height));
        if old_width == width && old_height == height {
            read_volatile(addr_of!((*texture).resource_id))
        } else {
            write_volatile(addr_of_mut!((*texture).resource_id), 0);
            0
        }
    };
    unsafe {
        let dst = addr_of_mut!((*texture).data) as *mut u8;
        if pixels.is_null() {
            write_bytes(dst, 0, byte_len);
        } else {
            copy_raw_bytes(dst, pixels.cast::<u8>(), byte_len);
        }
        write_volatile(addr_of_mut!((*texture).width), width);
        write_volatile(addr_of_mut!((*texture).height), height);
        write_volatile(addr_of_mut!((*texture).internal_format), internalformat);
        write_volatile(addr_of_mut!((*texture).format), format);
        write_volatile(addr_of_mut!((*texture).type_), type_);
        write_volatile(addr_of_mut!((*texture).data_len), byte_len);
    }
    let upload_result = match ensure_global_context() {
        Ok(ctx) => {
            let pixel_slice =
                unsafe { slice::from_raw_parts(addr_of!((*texture).data) as *const u8, byte_len) };
            ctx.upload_texture_2d(resource_id_hint, width as u32, height as u32, pixel_slice)
        }
        Err(err) => Err(err),
    };
    match upload_result {
        Ok(resource_id) => unsafe {
            write_volatile(addr_of_mut!((*texture).resource_id), resource_id);
            write_volatile(addr_of_mut!((*texture).owns_resource), true);
        },
        Err(err) => record_gl_error(error_to_gl_error(err)),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn capglImportTexture2D(
    texture_name: u32,
    resource_id: u32,
    width: u32,
    height: u32,
) -> u32 {
    if texture_name == 0 || resource_id == 0 || width == 0 || height == 0 {
        record_gl_error(GL_INVALID_VALUE);
        return GL_FALSE;
    }
    if width > i32::MAX as u32 || height > i32::MAX as u32 {
        record_gl_error(GL_INVALID_VALUE);
        return GL_FALSE;
    }
    let slot = match find_texture_slot(texture_name) {
        Some(slot) => slot,
        None => match find_free_texture_slot() {
            Some(slot) => {
                init_texture_slot(slot, texture_name);
                slot
            }
            None => {
                record_gl_error(GL_OUT_OF_MEMORY);
                return GL_FALSE;
            }
        },
    };
    unsafe {
        let texture = gl_texture_ptr(slot);
        write_volatile(addr_of_mut!((*texture).width), width as i32);
        write_volatile(addr_of_mut!((*texture).height), height as i32);
        write_volatile(addr_of_mut!((*texture).internal_format), GL_RGBA as i32);
        write_volatile(addr_of_mut!((*texture).format), GL_RGBA);
        write_volatile(addr_of_mut!((*texture).type_), GL_UNSIGNED_BYTE);
        write_volatile(addr_of_mut!((*texture).resource_id), resource_id);
        write_volatile(addr_of_mut!((*texture).owns_resource), false);
        write_volatile(addr_of_mut!((*texture).data_len), 0);
    }
    GL_TRUE
}

#[unsafe(no_mangle)]
pub extern "C" fn glTexSubImage2D(
    target: u32,
    level: i32,
    xoffset: i32,
    yoffset: i32,
    width: i32,
    height: i32,
    format: u32,
    type_: u32,
    pixels: *const c_void,
) {
    if target != GL_TEXTURE_2D {
        record_gl_error(GL_INVALID_ENUM);
        return;
    }
    if level != 0 || xoffset < 0 || yoffset < 0 || width <= 0 || height <= 0 || pixels.is_null() {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    if format != GL_RGBA || type_ != GL_UNSIGNED_BYTE {
        record_gl_error(GL_INVALID_ENUM);
        return;
    }
    let Some(byte_len) = texture_byte_len(width, height) else {
        record_gl_error(GL_OUT_OF_MEMORY);
        return;
    };
    if byte_len > MAX_GL_TEXTURE_BYTES {
        record_gl_error(GL_OUT_OF_MEMORY);
        return;
    }
    let texture = match bound_texture_2d() {
        Some(texture) => texture,
        None => {
            record_gl_error(GL_INVALID_OPERATION);
            return;
        }
    };
    let tex_width = unsafe { read_volatile(addr_of!((*texture).width)) };
    let tex_height = unsafe { read_volatile(addr_of!((*texture).height)) };
    let resource_id = unsafe { read_volatile(addr_of!((*texture).resource_id)) };
    if tex_width <= 0 || tex_height <= 0 || resource_id == 0 {
        record_gl_error(GL_INVALID_OPERATION);
        return;
    }
    let x_end = match xoffset.checked_add(width) {
        Some(end) => end,
        None => {
            record_gl_error(GL_INVALID_VALUE);
            return;
        }
    };
    let y_end = match yoffset.checked_add(height) {
        Some(end) => end,
        None => {
            record_gl_error(GL_INVALID_VALUE);
            return;
        }
    };
    if x_end > tex_width || y_end > tex_height {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }

    let row_bytes = width as usize * 4;
    unsafe {
        let src = pixels.cast::<u8>();
        let texture_data = addr_of_mut!((*texture).data) as *mut u8;
        let upload = addr_of_mut!(GLOBAL_TEXTURE_UPLOAD_SCRATCH) as *mut u8;
        for row in 0..height as usize {
            let src_row = src.add(row * row_bytes);
            let texture_row = texture_data
                .add(((yoffset as usize + row) * tex_width as usize + xoffset as usize) * 4);
            let upload_row = upload.add(row * row_bytes);
            copy_raw_bytes(texture_row, src_row, row_bytes);
            copy_raw_bytes(upload_row, src_row, row_bytes);
        }
    }

    let upload_result = match ensure_global_context() {
        Ok(ctx) => {
            let pixel_slice = unsafe {
                slice::from_raw_parts(
                    addr_of!(GLOBAL_TEXTURE_UPLOAD_SCRATCH) as *const u8,
                    byte_len,
                )
            };
            ctx.update_texture_2d(
                resource_id,
                xoffset as u32,
                yoffset as u32,
                width as u32,
                height as u32,
                pixel_slice,
            )
        }
        Err(err) => Err(err),
    };
    if let Err(err) = upload_result {
        record_gl_error(error_to_gl_error(err));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glBufferData(target: u32, size: isize, data: *const c_void, usage: u32) {
    if target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER {
        record_gl_error(GL_INVALID_ENUM);
        return;
    }
    if usage != GL_STATIC_DRAW && usage != GL_DYNAMIC_DRAW {
        record_gl_error(GL_INVALID_ENUM);
        return;
    }
    if size < 0 {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    let byte_len = size as usize;
    if byte_len > MAX_GL_ARRAY_BUFFER_BYTES {
        record_gl_error(GL_OUT_OF_MEMORY);
        return;
    }
    let buffer_name = unsafe {
        match target {
            GL_ARRAY_BUFFER => read_volatile(addr_of!(GLOBAL_CURRENT_ARRAY_BUFFER)),
            GL_ELEMENT_ARRAY_BUFFER => read_volatile(addr_of!(GLOBAL_CURRENT_ELEMENT_ARRAY_BUFFER)),
            _ => 0,
        }
    };
    if buffer_name == 0 {
        record_gl_error(GL_INVALID_OPERATION);
        return;
    }
    let slot = match find_buffer_slot(buffer_name) {
        Some(slot) => slot,
        None => {
            record_gl_error(GL_INVALID_OPERATION);
            return;
        }
    };
    unsafe {
        let buffer = gl_buffer_ptr(slot);
        let dst = addr_of_mut!((*buffer).data) as *mut u8;
        if byte_len > 0 {
            if data.is_null() {
                write_bytes(dst, 0, byte_len);
            } else {
                copy_raw_bytes(dst, data.cast::<u8>(), byte_len);
            }
        }
        write_volatile(addr_of_mut!((*buffer).len), byte_len);
        write_volatile(addr_of_mut!((*buffer).usage), usage);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glBufferSubData(target: u32, offset: isize, size: isize, data: *const c_void) {
    if target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER {
        record_gl_error(GL_INVALID_ENUM);
        return;
    }
    if offset < 0 || size < 0 || (size > 0 && data.is_null()) {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    let buffer_name = unsafe {
        match target {
            GL_ARRAY_BUFFER => read_volatile(addr_of!(GLOBAL_CURRENT_ARRAY_BUFFER)),
            GL_ELEMENT_ARRAY_BUFFER => read_volatile(addr_of!(GLOBAL_CURRENT_ELEMENT_ARRAY_BUFFER)),
            _ => 0,
        }
    };
    if buffer_name == 0 {
        record_gl_error(GL_INVALID_OPERATION);
        return;
    }
    let slot = match find_buffer_slot(buffer_name) {
        Some(slot) => slot,
        None => {
            record_gl_error(GL_INVALID_OPERATION);
            return;
        }
    };
    let offset = offset as usize;
    let byte_len = size as usize;
    unsafe {
        let buffer = gl_buffer_ptr(slot);
        let len = read_volatile(addr_of!((*buffer).len));
        let Some(end) = offset.checked_add(byte_len) else {
            record_gl_error(GL_INVALID_VALUE);
            return;
        };
        if end > len {
            record_gl_error(GL_INVALID_VALUE);
            return;
        }
        if byte_len > 0 {
            let dst = (addr_of_mut!((*buffer).data) as *mut u8).add(offset);
            copy_raw_bytes(dst, data.cast::<u8>(), byte_len);
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glEnableVertexAttribArray(index: u32) {
    if index as usize >= MAX_GL_VERTEX_ATTRIBS {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    unsafe {
        write_volatile(
            addr_of_mut!((*vertex_attrib_ptr(index as usize)).enabled),
            true,
        );
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glDisableVertexAttribArray(index: u32) {
    if index as usize >= MAX_GL_VERTEX_ATTRIBS {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    unsafe {
        write_volatile(
            addr_of_mut!((*vertex_attrib_ptr(index as usize)).enabled),
            false,
        );
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glVertexAttribPointer(
    index: u32,
    size: i32,
    type_: u32,
    normalized: u8,
    stride: i32,
    pointer: *const c_void,
) {
    if index as usize >= MAX_GL_VERTEX_ATTRIBS {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    if !(1..=4).contains(&size) || stride < 0 {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    if type_ != GL_FLOAT {
        record_gl_error(GL_INVALID_ENUM);
        return;
    }
    if normalized != GL_FALSE as u8 && normalized != GL_TRUE as u8 {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    let buffer_name = unsafe { read_volatile(addr_of!(GLOBAL_CURRENT_ARRAY_BUFFER)) };
    if buffer_name == 0 {
        record_gl_error(GL_INVALID_OPERATION);
        return;
    }
    let element_bytes = (size as usize) * size_of::<f32>();
    let stride_bytes = if stride == 0 {
        element_bytes
    } else {
        stride as usize
    };
    unsafe {
        let attrib = vertex_attrib_ptr(index as usize);
        write_volatile(addr_of_mut!((*attrib).buffer_name), buffer_name);
        write_volatile(addr_of_mut!((*attrib).size), size);
        write_volatile(addr_of_mut!((*attrib).type_), type_);
        write_volatile(addr_of_mut!((*attrib).normalized), normalized);
        write_volatile(addr_of_mut!((*attrib).stride), stride_bytes);
        write_volatile(addr_of_mut!((*attrib).offset), pointer as usize);
    }
}

fn client_state_attrib_index(array: u32) -> Option<u32> {
    match array {
        GL_VERTEX_ARRAY => Some(0),
        GL_COLOR_ARRAY => Some(1),
        GL_TEXTURE_COORD_ARRAY => Some(2),
        _ => None,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glEnableClientState(array: u32) {
    let Some(index) = client_state_attrib_index(array) else {
        record_gl_error(GL_INVALID_ENUM);
        return;
    };
    glEnableVertexAttribArray(index);
}

#[unsafe(no_mangle)]
pub extern "C" fn glDisableClientState(array: u32) {
    let Some(index) = client_state_attrib_index(array) else {
        record_gl_error(GL_INVALID_ENUM);
        return;
    };
    glDisableVertexAttribArray(index);
}

#[unsafe(no_mangle)]
pub extern "C" fn glVertexPointer(size: i32, type_: u32, stride: i32, pointer: *const c_void) {
    if !(2..=4).contains(&size) {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttribPointer(0, size, type_, GL_FALSE as u8, stride, pointer);
}

#[unsafe(no_mangle)]
pub extern "C" fn glColorPointer(size: i32, type_: u32, stride: i32, pointer: *const c_void) {
    if !(3..=4).contains(&size) {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttribPointer(1, size, type_, GL_FALSE as u8, stride, pointer);
}

#[unsafe(no_mangle)]
pub extern "C" fn glTexCoordPointer(size: i32, type_: u32, stride: i32, pointer: *const c_void) {
    if !(1..=4).contains(&size) {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttribPointer(2, size, type_, GL_FALSE as u8, stride, pointer);
}

#[unsafe(no_mangle)]
pub extern "C" fn glCreateShader(type_: u32) -> u32 {
    if type_ != GL_VERTEX_SHADER && type_ != GL_FRAGMENT_SHADER {
        record_gl_error(GL_INVALID_ENUM);
        return 0;
    }
    let slot = match find_free_shader_slot() {
        Some(slot) => slot,
        None => {
            record_gl_error(GL_OUT_OF_MEMORY);
            return 0;
        }
    };
    let name = next_shader_name();
    unsafe {
        let shader = gl_shader_ptr(slot);
        write_volatile(addr_of_mut!((*shader).name), name);
        write_volatile(addr_of_mut!((*shader).type_), type_);
        write_volatile(addr_of_mut!((*shader).source_len), 0);
        write_volatile(addr_of_mut!((*shader).compiled), false);
    }
    name
}

#[unsafe(no_mangle)]
pub extern "C" fn glShaderSource(
    shader: u32,
    count: i32,
    string: *const *const c_char,
    length: *const i32,
) {
    if count < 0 || (count > 0 && string.is_null()) {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    let slot = match find_shader_slot(shader) {
        Some(slot) => slot,
        None => {
            record_gl_error(GL_INVALID_VALUE);
            return;
        }
    };
    let shader_ptr = gl_shader_ptr(slot);
    let mut total_len = 0usize;
    for index in 0..count as usize {
        let source_ptr = unsafe { read_volatile(string.add(index)) };
        if source_ptr.is_null() {
            record_gl_error(GL_INVALID_VALUE);
            return;
        }
        let source_len = match shader_source_len(source_ptr, length, index) {
            Ok(source_len) => source_len,
            Err(err) => {
                record_gl_error(error_to_gl_error(err));
                return;
            }
        };
        let Some(new_total_len) = total_len.checked_add(source_len) else {
            record_gl_error(GL_OUT_OF_MEMORY);
            return;
        };
        if new_total_len > MAX_GL_SHADER_SOURCE_BYTES {
            record_gl_error(GL_OUT_OF_MEMORY);
            return;
        }
        unsafe {
            let dst = (addr_of_mut!((*shader_ptr).source) as *mut u8).add(total_len);
            copy_raw_bytes(dst, source_ptr.cast::<u8>(), source_len);
        }
        total_len = new_total_len;
    }
    unsafe {
        write_volatile(addr_of_mut!((*shader_ptr).source_len), total_len);
        write_volatile(addr_of_mut!((*shader_ptr).compiled), false);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glCompileShader(shader: u32) {
    let slot = match find_shader_slot(shader) {
        Some(slot) => slot,
        None => {
            record_gl_error(GL_INVALID_VALUE);
            return;
        }
    };
    let shader_ptr = gl_shader_ptr(slot);
    let source_len = unsafe { read_volatile(addr_of!((*shader_ptr).source_len)) };
    unsafe {
        write_volatile(addr_of_mut!((*shader_ptr).compiled), source_len != 0);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glCreateProgram() -> u32 {
    let slot = match find_free_program_slot() {
        Some(slot) => slot,
        None => {
            record_gl_error(GL_OUT_OF_MEMORY);
            return 0;
        }
    };
    let name = next_program_name();
    unsafe {
        let program = gl_program_ptr(slot);
        write_volatile(addr_of_mut!((*program).name), name);
        write_volatile(addr_of_mut!((*program).vertex_shader), 0);
        write_volatile(addr_of_mut!((*program).fragment_shader), 0);
        write_volatile(addr_of_mut!((*program).linked), false);
        write_volatile(addr_of_mut!((*program).uniform_tint), [1.0, 1.0, 1.0, 1.0]);
        write_volatile(addr_of_mut!((*program).uniform_mvp), IDENTITY_MATRIX);
        write_volatile(addr_of_mut!((*program).uniform_texture0), 0);
        write_volatile(addr_of_mut!((*program).uniform_tint_set), false);
        write_volatile(addr_of_mut!((*program).uniform_mvp_set), false);
        write_volatile(addr_of_mut!((*program).uniform_texture0_set), false);
    }
    name
}

#[unsafe(no_mangle)]
pub extern "C" fn glAttachShader(program: u32, shader: u32) {
    let program_slot = match find_program_slot(program) {
        Some(slot) => slot,
        None => {
            record_gl_error(GL_INVALID_VALUE);
            return;
        }
    };
    let shader_slot = match find_shader_slot(shader) {
        Some(slot) => slot,
        None => {
            record_gl_error(GL_INVALID_VALUE);
            return;
        }
    };
    let shader_ptr = gl_shader_ptr(shader_slot);
    let shader_type = unsafe { read_volatile(addr_of!((*shader_ptr).type_)) };
    let program_ptr = gl_program_ptr(program_slot);
    unsafe {
        match shader_type {
            GL_VERTEX_SHADER => write_volatile(addr_of_mut!((*program_ptr).vertex_shader), shader),
            GL_FRAGMENT_SHADER => {
                write_volatile(addr_of_mut!((*program_ptr).fragment_shader), shader)
            }
            _ => {
                record_gl_error(GL_INVALID_OPERATION);
                return;
            }
        }
        write_volatile(addr_of_mut!((*program_ptr).linked), false);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glLinkProgram(program: u32) {
    let slot = match find_program_slot(program) {
        Some(slot) => slot,
        None => {
            record_gl_error(GL_INVALID_VALUE);
            return;
        }
    };
    let program_ptr = gl_program_ptr(slot);
    let vertex_shader = unsafe { read_volatile(addr_of!((*program_ptr).vertex_shader)) };
    let fragment_shader = unsafe { read_volatile(addr_of!((*program_ptr).fragment_shader)) };
    let linked = shader_is_compiled(vertex_shader, GL_VERTEX_SHADER)
        && shader_is_compiled(fragment_shader, GL_FRAGMENT_SHADER);
    unsafe {
        write_volatile(addr_of_mut!((*program_ptr).linked), linked);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glUseProgram(program: u32) {
    if program == 0 {
        unsafe {
            write_volatile(addr_of_mut!(GLOBAL_CURRENT_PROGRAM), 0);
        }
        return;
    }
    let slot = match find_program_slot(program) {
        Some(slot) => slot,
        None => {
            record_gl_error(GL_INVALID_VALUE);
            return;
        }
    };
    let linked = unsafe { read_volatile(addr_of!((*gl_program_ptr(slot)).linked)) };
    if !linked {
        record_gl_error(GL_INVALID_OPERATION);
        return;
    }
    unsafe {
        write_volatile(addr_of_mut!(GLOBAL_CURRENT_PROGRAM), program);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glGetAttribLocation(program: u32, name: *const c_char) -> i32 {
    if name.is_null() {
        record_gl_error(GL_INVALID_VALUE);
        return -1;
    }
    let slot = match find_program_slot(program) {
        Some(slot) => slot,
        None => {
            record_gl_error(GL_INVALID_VALUE);
            return -1;
        }
    };
    let linked = unsafe { read_volatile(addr_of!((*gl_program_ptr(slot)).linked)) };
    if !linked {
        record_gl_error(GL_INVALID_OPERATION);
        return -1;
    }
    match scan_nul_terminated_name(name) {
        Ok(name_bytes) if name_bytes == b"position" => 0,
        Ok(name_bytes) if name_bytes == b"color" => 1,
        Ok(name_bytes) if name_bytes == b"texcoord" => 2,
        Ok(_) => -1,
        Err(err) => {
            record_gl_error(error_to_gl_error(err));
            -1
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glGetUniformLocation(program: u32, name: *const c_char) -> i32 {
    if name.is_null() {
        record_gl_error(GL_INVALID_VALUE);
        return -1;
    }
    let slot = match find_program_slot(program) {
        Some(slot) => slot,
        None => {
            record_gl_error(GL_INVALID_VALUE);
            return -1;
        }
    };
    let linked = unsafe { read_volatile(addr_of!((*gl_program_ptr(slot)).linked)) };
    if !linked {
        record_gl_error(GL_INVALID_OPERATION);
        return -1;
    }
    match scan_nul_terminated_name(name) {
        Ok(name_bytes) if name_bytes == b"u_tint" => uniform_location(slot, 0),
        Ok(name_bytes) if name_bytes == b"u_mvp" => uniform_location(slot, 1),
        Ok(name_bytes) if name_bytes == b"u_tex" || name_bytes == b"u_texture" => {
            uniform_location(slot, 2)
        }
        Ok(_) => -1,
        Err(err) => {
            record_gl_error(error_to_gl_error(err));
            -1
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glUniform4f(location: i32, v0: f32, v1: f32, v2: f32, v3: f32) {
    if location < 0 {
        return;
    }
    let (program_slot, uniform_slot) = match parse_uniform_location(location) {
        Some(parsed) => parsed,
        None => {
            record_gl_error(GL_INVALID_OPERATION);
            return;
        }
    };
    if !program_slot_is_current(program_slot) || uniform_slot != 0 {
        record_gl_error(GL_INVALID_OPERATION);
        return;
    }
    unsafe {
        let program = gl_program_ptr(program_slot);
        write_volatile(addr_of_mut!((*program).uniform_tint), [v0, v1, v2, v3]);
        write_volatile(addr_of_mut!((*program).uniform_tint_set), true);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glUniform1i(location: i32, v0: i32) {
    if location < 0 {
        return;
    }
    let (program_slot, uniform_slot) = match parse_uniform_location(location) {
        Some(parsed) => parsed,
        None => {
            record_gl_error(GL_INVALID_OPERATION);
            return;
        }
    };
    if !program_slot_is_current(program_slot) || uniform_slot != 2 {
        record_gl_error(GL_INVALID_OPERATION);
        return;
    }
    if v0 < 0 || v0 as usize >= MAX_GL_TEXTURE_UNITS {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    unsafe {
        let program = gl_program_ptr(program_slot);
        write_volatile(addr_of_mut!((*program).uniform_texture0), v0);
        write_volatile(addr_of_mut!((*program).uniform_texture0_set), true);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glUniformMatrix4fv(location: i32, count: i32, transpose: u8, value: *const f32) {
    if location < 0 {
        return;
    }
    if count < 0 || (count > 0 && value.is_null()) {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    if transpose != GL_FALSE as u8 && transpose != GL_TRUE as u8 {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    if count == 0 {
        return;
    }
    if count != 1 {
        record_gl_error(GL_INVALID_OPERATION);
        return;
    }
    let (program_slot, uniform_slot) = match parse_uniform_location(location) {
        Some(parsed) => parsed,
        None => {
            record_gl_error(GL_INVALID_OPERATION);
            return;
        }
    };
    if !program_slot_is_current(program_slot) || uniform_slot != 1 {
        record_gl_error(GL_INVALID_OPERATION);
        return;
    }
    let mut matrix = [0.0; 16];
    for (index, item) in matrix.iter_mut().enumerate() {
        *item = unsafe { read_volatile(value.add(index)) };
    }
    if transpose == GL_TRUE as u8 {
        matrix = transpose_mat4(matrix);
    }
    unsafe {
        let program = gl_program_ptr(program_slot);
        write_volatile(addr_of_mut!((*program).uniform_mvp), matrix);
        write_volatile(addr_of_mut!((*program).uniform_mvp_set), true);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glGetShaderiv(shader: u32, pname: u32, params: *mut i32) {
    if params.is_null() {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    let slot = match find_shader_slot(shader) {
        Some(slot) => slot,
        None => {
            record_gl_error(GL_INVALID_VALUE);
            return;
        }
    };
    let value = match pname {
        GL_COMPILE_STATUS => unsafe {
            read_volatile(addr_of!((*gl_shader_ptr(slot)).compiled)) as i32
        },
        _ => {
            record_gl_error(GL_INVALID_ENUM);
            return;
        }
    };
    unsafe {
        write_volatile(params, value);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glGetProgramiv(program: u32, pname: u32, params: *mut i32) {
    if params.is_null() {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    let slot = match find_program_slot(program) {
        Some(slot) => slot,
        None => {
            record_gl_error(GL_INVALID_VALUE);
            return;
        }
    };
    let value = match pname {
        GL_LINK_STATUS => unsafe { read_volatile(addr_of!((*gl_program_ptr(slot)).linked)) as i32 },
        _ => {
            record_gl_error(GL_INVALID_ENUM);
            return;
        }
    };
    unsafe {
        write_volatile(params, value);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glDrawArrays(mode: u32, first: i32, count: i32) {
    let primitive = match primitive_from_gl_mode(mode) {
        Some(primitive) => primitive,
        None => {
            record_gl_error(GL_INVALID_ENUM);
            return;
        }
    };
    if first < 0 || count < 0 {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    if current_program_is_linked().is_none() {
        record_gl_error(GL_INVALID_OPERATION);
        return;
    }
    let vertex_count = match build_draw_vertices(first as u32, count as u32) {
        Ok(vertex_count) => vertex_count,
        Err(err) => {
            record_gl_error(error_to_gl_error(err));
            return;
        }
    };
    let result = match ensure_global_context() {
        Ok(ctx) => {
            let vertices = draw_vertices_slice(vertex_count);
            let scratch = global_scratch();
            ctx.gl_draw_vertices(primitive, vertices, current_texture_resource_id(), scratch)
        }
        Err(err) => Err(err),
    };
    if let Err(err) = result {
        record_gl_error(error_to_gl_error(err));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn glDrawElements(mode: u32, count: i32, type_: u32, indices: *const c_void) {
    let primitive = match primitive_from_gl_mode(mode) {
        Some(primitive) => primitive,
        None => {
            record_gl_error(GL_INVALID_ENUM);
            return;
        }
    };
    if type_ != GL_UNSIGNED_BYTE && type_ != GL_UNSIGNED_SHORT && type_ != GL_UNSIGNED_INT {
        record_gl_error(GL_INVALID_ENUM);
        return;
    }
    if count < 0 {
        record_gl_error(GL_INVALID_VALUE);
        return;
    }
    if current_program_is_linked().is_none() {
        record_gl_error(GL_INVALID_OPERATION);
        return;
    }
    let element_buffer = unsafe { read_volatile(addr_of!(GLOBAL_CURRENT_ELEMENT_ARRAY_BUFFER)) };
    if element_buffer == 0 && indices.is_null() && count > 0 {
        record_gl_error(GL_INVALID_OPERATION);
        return;
    }
    let vertex_count = match build_draw_vertices_from_elements(count as u32, type_, indices) {
        Ok(vertex_count) => vertex_count,
        Err(err) => {
            record_gl_error(error_to_gl_error(err));
            return;
        }
    };
    let result = match ensure_global_context() {
        Ok(ctx) => {
            let vertices = draw_vertices_slice(vertex_count);
            let scratch = global_scratch();
            ctx.gl_draw_vertices(primitive, vertices, current_texture_resource_id(), scratch)
        }
        Err(err) => Err(err),
    };
    if let Err(err) = result {
        record_gl_error(error_to_gl_error(err));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn capglPresent() -> u32 {
    let surface = unsafe { read_volatile(addr_of!(GLOBAL_CURRENT_CAPGL_SURFACE)) };
    if surface == 0 {
        record_gl_error(GL_INVALID_OPERATION);
        return GL_FALSE;
    }
    capglSwapBuffers(surface)
}

#[unsafe(no_mangle)]
pub extern "C" fn glGetError() -> u32 {
    unsafe {
        let error = read_volatile(addr_of!(GLOBAL_LAST_ERROR));
        write_volatile(addr_of_mut!(GLOBAL_LAST_ERROR), GL_NO_ERROR);
        error
    }
}

fn next_capgl_context_name() -> u32 {
    unsafe {
        let name = read_volatile(addr_of!(GLOBAL_NEXT_CAPGL_CONTEXT_NAME));
        let next = if name == u32::MAX { 1 } else { name + 1 };
        write_volatile(addr_of_mut!(GLOBAL_NEXT_CAPGL_CONTEXT_NAME), next);
        name
    }
}

fn next_capgl_surface_name() -> u32 {
    unsafe {
        let name = read_volatile(addr_of!(GLOBAL_NEXT_CAPGL_SURFACE_NAME));
        let next = if name == u32::MAX { 1 } else { name + 1 };
        write_volatile(addr_of_mut!(GLOBAL_NEXT_CAPGL_SURFACE_NAME), next);
        name
    }
}

fn capgl_context_ptr(index: usize) -> *mut CapglContextState {
    unsafe { (addr_of_mut!(GLOBAL_CAPGL_CONTEXTS) as *mut CapglContextState).add(index) }
}

fn capgl_surface_ptr(index: usize) -> *mut CapglSurfaceState {
    unsafe { (addr_of_mut!(GLOBAL_CAPGL_SURFACES) as *mut CapglSurfaceState).add(index) }
}

fn find_capgl_context(name: u32) -> Option<usize> {
    for index in 0..MAX_CAPGL_CONTEXTS {
        let state = unsafe { read_volatile(capgl_context_ptr(index)) };
        if state.in_use && state.name == name {
            return Some(index);
        }
    }
    None
}

fn find_capgl_surface(name: u32) -> Option<usize> {
    for index in 0..MAX_CAPGL_SURFACES {
        let state = unsafe { read_volatile(capgl_surface_ptr(index)) };
        if state.in_use && state.name == name {
            return Some(index);
        }
    }
    None
}

fn create_capgl_context() -> Result<u32, Error> {
    for index in 0..MAX_CAPGL_CONTEXTS {
        let state = unsafe { read_volatile(capgl_context_ptr(index)) };
        if !state.in_use {
            let name = next_capgl_context_name();
            unsafe {
                write_volatile(
                    capgl_context_ptr(index),
                    CapglContextState { name, in_use: true },
                );
            }
            return Ok(name);
        }
    }
    Err(Error::Invalid)
}

fn create_capgl_surface(ctx: &mut Context, width: u32, height: u32) -> Result<u32, Error> {
    if width == 0 || height == 0 {
        return Err(Error::Invalid);
    }
    let target = ctx.client.create_app_surface(width, height)?;
    create_capgl_surface_from_target(target, false)
}

fn create_default_capgl_surface(ctx: &Context) -> Result<u32, Error> {
    create_capgl_surface_from_target(ctx.default_surface(), true)
}

fn create_capgl_surface_from_target(target: RenderTarget, is_default: bool) -> Result<u32, Error> {
    for index in 0..MAX_CAPGL_SURFACES {
        let state = unsafe { read_volatile(capgl_surface_ptr(index)) };
        if !state.in_use {
            let name = next_capgl_surface_name();
            unsafe {
                write_volatile(
                    capgl_surface_ptr(index),
                    CapglSurfaceState {
                        name,
                        in_use: true,
                        width: target.width,
                        height: target.height,
                        resource_id: target.resource_id,
                        surface_id: target.surface_id,
                        swap_count: 0,
                        is_default,
                    },
                );
            }
            return Ok(name);
        }
    }
    Err(Error::Invalid)
}

fn make_capgl_current(ctx: &mut Context, context: u32, surface: u32) -> Result<(), Error> {
    let Some(surface_index) = find_capgl_surface(surface) else {
        return Err(Error::Invalid);
    };
    if find_capgl_context(context).is_none() {
        return Err(Error::Invalid);
    }
    let state = unsafe { read_volatile(capgl_surface_ptr(surface_index)) };
    ctx.make_surface_current(RenderTarget {
        width: state.width,
        height: state.height,
        resource_id: state.resource_id,
        surface_id: state.surface_id,
        vertex_buffer_id: DEFAULT_VIRGL_VERTEX_BUFFER_ID,
    });
    unsafe {
        write_volatile(addr_of_mut!(GLOBAL_CURRENT_CAPGL_CONTEXT), context);
        write_volatile(addr_of_mut!(GLOBAL_CURRENT_CAPGL_SURFACE), surface);
    }
    Ok(())
}

fn ensure_default_capgl_binding(ctx: &mut Context) -> Result<(u32, u32), Error> {
    let current_context = unsafe { read_volatile(addr_of!(GLOBAL_CURRENT_CAPGL_CONTEXT)) };
    let current_surface = unsafe { read_volatile(addr_of!(GLOBAL_CURRENT_CAPGL_SURFACE)) };
    if current_context != 0 && current_surface != 0 {
        return Ok((current_context, current_surface));
    }
    let context = create_capgl_context()?;
    let surface = create_default_capgl_surface(ctx)?;
    make_capgl_current(ctx, context, surface)?;
    Ok((context, surface))
}

fn next_buffer_name() -> u32 {
    unsafe {
        let name = read_volatile(addr_of!(GLOBAL_NEXT_BUFFER_NAME));
        let next = if name == u32::MAX { 1 } else { name + 1 };
        write_volatile(addr_of_mut!(GLOBAL_NEXT_BUFFER_NAME), next);
        name
    }
}

fn next_texture_name() -> u32 {
    unsafe {
        let name = read_volatile(addr_of!(GLOBAL_NEXT_TEXTURE_NAME));
        let next = if name == u32::MAX { 1 } else { name + 1 };
        write_volatile(addr_of_mut!(GLOBAL_NEXT_TEXTURE_NAME), next);
        name
    }
}

fn next_shader_name() -> u32 {
    unsafe {
        let name = read_volatile(addr_of!(GLOBAL_NEXT_SHADER_NAME));
        let next = if name == u32::MAX { 1 } else { name + 1 };
        write_volatile(addr_of_mut!(GLOBAL_NEXT_SHADER_NAME), next);
        name
    }
}

fn next_program_name() -> u32 {
    unsafe {
        let name = read_volatile(addr_of!(GLOBAL_NEXT_PROGRAM_NAME));
        let next = if name == u32::MAX { 1 } else { name + 1 };
        write_volatile(addr_of_mut!(GLOBAL_NEXT_PROGRAM_NAME), next);
        name
    }
}

fn gl_buffer_ptr(index: usize) -> *mut GlBuffer {
    unsafe { (addr_of_mut!(GLOBAL_BUFFERS) as *mut GlBuffer).add(index) }
}

fn gl_texture_ptr(index: usize) -> *mut GlTexture {
    unsafe { (addr_of_mut!(GLOBAL_TEXTURES) as *mut GlTexture).add(index) }
}

fn gl_shader_ptr(index: usize) -> *mut GlShader {
    unsafe { (addr_of_mut!(GLOBAL_SHADERS) as *mut GlShader).add(index) }
}

fn gl_program_ptr(index: usize) -> *mut GlProgram {
    unsafe { (addr_of_mut!(GLOBAL_PROGRAMS) as *mut GlProgram).add(index) }
}

fn vertex_attrib_ptr(index: usize) -> *mut VertexAttrib {
    unsafe { (addr_of_mut!(GLOBAL_VERTEX_ATTRIBS) as *mut VertexAttrib).add(index) }
}

fn draw_vertex_ptr(index: usize) -> *mut Vertex {
    unsafe { (addr_of_mut!(GLOBAL_DRAW_VERTICES) as *mut Vertex).add(index) }
}

fn find_buffer_slot(name: u32) -> Option<usize> {
    for index in 0..MAX_GL_BUFFERS {
        let buffer = gl_buffer_ptr(index);
        let candidate = unsafe { read_volatile(addr_of!((*buffer).name)) };
        if candidate == name {
            return Some(index);
        }
    }
    None
}

fn find_free_buffer_slot() -> Option<usize> {
    find_buffer_slot(0)
}

fn find_texture_slot(name: u32) -> Option<usize> {
    for index in 0..MAX_GL_TEXTURES {
        let texture = gl_texture_ptr(index);
        let candidate = unsafe { read_volatile(addr_of!((*texture).name)) };
        if candidate == name {
            return Some(index);
        }
    }
    None
}

fn find_free_texture_slot() -> Option<usize> {
    find_texture_slot(0)
}

fn init_texture_slot(slot: usize, name: u32) {
    let texture = gl_texture_ptr(slot);
    unsafe {
        write_volatile(addr_of_mut!((*texture).name), name);
        write_volatile(addr_of_mut!((*texture).width), 0);
        write_volatile(addr_of_mut!((*texture).height), 0);
        write_volatile(addr_of_mut!((*texture).internal_format), 0);
        write_volatile(addr_of_mut!((*texture).format), 0);
        write_volatile(addr_of_mut!((*texture).type_), 0);
        write_volatile(addr_of_mut!((*texture).min_filter), GL_NEAREST as i32);
        write_volatile(addr_of_mut!((*texture).mag_filter), GL_NEAREST as i32);
        write_volatile(addr_of_mut!((*texture).wrap_s), GL_CLAMP_TO_EDGE as i32);
        write_volatile(addr_of_mut!((*texture).wrap_t), GL_CLAMP_TO_EDGE as i32);
        write_volatile(addr_of_mut!((*texture).resource_id), 0);
        write_volatile(addr_of_mut!((*texture).owns_resource), false);
        write_volatile(addr_of_mut!((*texture).data_len), 0);
    }
}

fn active_texture_unit() -> usize {
    unsafe { read_volatile(addr_of!(GLOBAL_ACTIVE_TEXTURE_UNIT)) }
}

fn bound_texture_2d() -> Option<*mut GlTexture> {
    let unit = active_texture_unit();
    let name =
        unsafe { read_volatile((addr_of!(GLOBAL_TEXTURE_BINDINGS_2D) as *const u32).add(unit)) };
    if name == 0 {
        return None;
    }
    find_texture_slot(name).map(gl_texture_ptr)
}

fn current_texture_resource_id() -> Option<u32> {
    let program_slot = current_program_slot()?;
    let unit = unsafe { read_volatile(addr_of!((*gl_program_ptr(program_slot)).uniform_texture0)) };
    if unit < 0 || unit as usize >= MAX_GL_TEXTURE_UNITS {
        return None;
    }
    let texture_name = unsafe {
        read_volatile((addr_of!(GLOBAL_TEXTURE_BINDINGS_2D) as *const u32).add(unit as usize))
    };
    if texture_name == 0 {
        return None;
    }
    let texture = gl_texture_ptr(find_texture_slot(texture_name)?);
    let resource_id = unsafe { read_volatile(addr_of!((*texture).resource_id)) };
    if resource_id == 0 {
        None
    } else {
        Some(resource_id)
    }
}

fn texture_byte_len(width: i32, height: i32) -> Option<usize> {
    let width = width as usize;
    let height = height as usize;
    width.checked_mul(height)?.checked_mul(4)
}

fn gl_blend_factor_to_pipe(factor: u32) -> Result<u32, Error> {
    match factor {
        GL_ZERO => Ok(PIPE_BLENDFACTOR_ZERO),
        GL_ONE => Ok(PIPE_BLENDFACTOR_ONE),
        GL_SRC_COLOR => Ok(PIPE_BLENDFACTOR_SRC_COLOR),
        GL_ONE_MINUS_SRC_COLOR => Ok(PIPE_BLENDFACTOR_INV_SRC_COLOR),
        GL_SRC_ALPHA => Ok(PIPE_BLENDFACTOR_SRC_ALPHA),
        GL_ONE_MINUS_SRC_ALPHA => Ok(PIPE_BLENDFACTOR_INV_SRC_ALPHA),
        GL_DST_ALPHA => Ok(PIPE_BLENDFACTOR_DST_ALPHA),
        GL_ONE_MINUS_DST_ALPHA => Ok(PIPE_BLENDFACTOR_INV_DST_ALPHA),
        GL_DST_COLOR => Ok(PIPE_BLENDFACTOR_DST_COLOR),
        GL_ONE_MINUS_DST_COLOR => Ok(PIPE_BLENDFACTOR_INV_DST_COLOR),
        _ => Err(Error::Invalid),
    }
}

fn scissor_box_from_i32(x: i32, y: i32, width: i32, height: i32) -> Result<ScissorBox, Error> {
    if x < 0 || y < 0 || width < 0 || height < 0 {
        return Err(Error::Invalid);
    }
    let x = x as u32;
    let y = y as u32;
    let width = width as u32;
    let height = height as u32;
    let max_x = x.checked_add(width).ok_or(Error::Invalid)?;
    let max_y = y.checked_add(height).ok_or(Error::Invalid)?;
    if max_x > u16::MAX as u32 || max_y > u16::MAX as u32 {
        return Err(Error::Invalid);
    }
    Ok(ScissorBox {
        x: x as u16,
        y: y as u16,
        width: width as u16,
        height: height as u16,
    })
}

fn clamp_u32_to_u16(value: u32) -> u16 {
    if value > u16::MAX as u32 {
        u16::MAX
    } else {
        value as u16
    }
}

fn find_shader_slot(name: u32) -> Option<usize> {
    for index in 0..MAX_GL_SHADERS {
        let shader = gl_shader_ptr(index);
        let candidate = unsafe { read_volatile(addr_of!((*shader).name)) };
        if candidate == name {
            return Some(index);
        }
    }
    None
}

fn find_free_shader_slot() -> Option<usize> {
    find_shader_slot(0)
}

fn find_program_slot(name: u32) -> Option<usize> {
    for index in 0..MAX_GL_PROGRAMS {
        let program = gl_program_ptr(index);
        let candidate = unsafe { read_volatile(addr_of!((*program).name)) };
        if candidate == name {
            return Some(index);
        }
    }
    None
}

fn find_free_program_slot() -> Option<usize> {
    find_program_slot(0)
}

fn uniform_location(program_slot: usize, uniform_slot: usize) -> i32 {
    (program_slot * MAX_GL_UNIFORMS + uniform_slot) as i32
}

fn parse_uniform_location(location: i32) -> Option<(usize, usize)> {
    if location < 0 {
        return None;
    }
    let location = location as usize;
    let program_slot = location / MAX_GL_UNIFORMS;
    let uniform_slot = location % MAX_GL_UNIFORMS;
    if program_slot >= MAX_GL_PROGRAMS || uniform_slot >= MAX_GL_UNIFORMS {
        return None;
    }
    let program = gl_program_ptr(program_slot);
    let name = unsafe { read_volatile(addr_of!((*program).name)) };
    let linked = unsafe { read_volatile(addr_of!((*program).linked)) };
    if name == 0 || !linked {
        return None;
    }
    Some((program_slot, uniform_slot))
}

fn program_slot_is_current(program_slot: usize) -> bool {
    let Some(current_slot) = current_program_slot() else {
        return false;
    };
    current_slot == program_slot
}

fn shader_source_len(
    source_ptr: *const c_char,
    length: *const i32,
    index: usize,
) -> Result<usize, Error> {
    if !length.is_null() {
        let len = unsafe { read_volatile(length.add(index)) };
        if len < 0 {
            return scan_nul_terminated_shader_source(source_ptr);
        }
        return Ok(len as usize);
    }
    scan_nul_terminated_shader_source(source_ptr)
}

fn scan_nul_terminated_shader_source(source_ptr: *const c_char) -> Result<usize, Error> {
    let mut len = 0usize;
    while len < MAX_GL_SHADER_SOURCE_BYTES {
        let byte = unsafe { read_volatile(source_ptr.add(len)) };
        if byte == 0 {
            return Ok(len);
        }
        len += 1;
    }
    Err(Error::BufferTooSmall)
}

fn scan_nul_terminated_name(name: *const c_char) -> Result<&'static [u8], Error> {
    static mut NAME_BUFFER: [u8; MAX_GL_NAME_BYTES] = [0; MAX_GL_NAME_BYTES];
    let mut len = 0usize;
    while len < MAX_GL_NAME_BYTES {
        let byte = unsafe { read_volatile(name.add(len)) };
        if byte == 0 {
            return Ok(unsafe { slice::from_raw_parts(addr_of!(NAME_BUFFER) as *const u8, len) });
        }
        unsafe {
            write_volatile((addr_of_mut!(NAME_BUFFER) as *mut u8).add(len), byte as u8);
        }
        len += 1;
    }
    Err(Error::BufferTooSmall)
}

fn shader_is_compiled(shader: u32, expected_type: u32) -> bool {
    let Some(slot) = find_shader_slot(shader) else {
        return false;
    };
    let shader_ptr = gl_shader_ptr(slot);
    unsafe {
        read_volatile(addr_of!((*shader_ptr).type_)) == expected_type
            && read_volatile(addr_of!((*shader_ptr).compiled))
    }
}

fn current_program_is_linked() -> Option<u32> {
    let program = unsafe { read_volatile(addr_of!(GLOBAL_CURRENT_PROGRAM)) };
    if program == 0 {
        return None;
    }
    let slot = find_program_slot(program)?;
    let linked = unsafe { read_volatile(addr_of!((*gl_program_ptr(slot)).linked)) };
    if linked { Some(program) } else { None }
}

fn current_program_slot() -> Option<usize> {
    let program = unsafe { read_volatile(addr_of!(GLOBAL_CURRENT_PROGRAM)) };
    if program == 0 {
        return None;
    }
    find_program_slot(program)
}

#[derive(Copy, Clone)]
struct DrawUniforms {
    tint: [f32; 4],
    current_color: [f32; 4],
    mvp: [f32; 16],
}

fn current_draw_uniforms() -> Result<DrawUniforms, Error> {
    let program_slot = current_program_slot().ok_or(Error::Invalid)?;
    let program = gl_program_ptr(program_slot);
    let program_mvp = unsafe { read_volatile(addr_of!((*program).uniform_mvp)) };
    let program_mvp_set = unsafe { read_volatile(addr_of!((*program).uniform_mvp_set)) };
    Ok(DrawUniforms {
        tint: unsafe { read_volatile(addr_of!((*program).uniform_tint)) },
        current_color: unsafe { read_volatile(addr_of!(GLOBAL_CURRENT_COLOR)) },
        mvp: effective_mvp_matrix(program_mvp, program_mvp_set),
    })
}

fn draw_vertices_slice(vertex_count: usize) -> &'static [Vertex] {
    unsafe {
        slice::from_raw_parts(
            addr_of!(GLOBAL_DRAW_VERTICES) as *const Vertex,
            vertex_count,
        )
    }
}

fn build_draw_vertices(first: u32, count: u32) -> Result<usize, Error> {
    if count == 0 {
        return Err(Error::Invalid);
    }
    let vertex_count = count as usize;
    if vertex_count > MAX_GL_DRAW_VERTICES {
        return Err(Error::BufferTooSmall);
    }
    let last = first.checked_add(count - 1).ok_or(Error::Invalid)? as usize;
    let position = load_vertex_attrib(0)?;
    let color = load_vertex_attrib(1).ok();
    let texcoord = load_vertex_attrib(2).ok();
    let uniforms = current_draw_uniforms()?;
    let first_index = first as usize;
    for out_index in 0..vertex_count {
        let source_index = first_index + out_index;
        if source_index > last {
            return Err(Error::Invalid);
        }
        write_draw_vertex(out_index, source_index, position, color, texcoord, uniforms)?;
    }
    Ok(vertex_count)
}

fn build_draw_vertices_from_elements(
    count: u32,
    type_: u32,
    indices: *const c_void,
) -> Result<usize, Error> {
    if count == 0 {
        return Err(Error::Invalid);
    }
    let vertex_count = count as usize;
    if vertex_count > MAX_GL_DRAW_VERTICES {
        return Err(Error::BufferTooSmall);
    }
    let position = load_vertex_attrib(0)?;
    let color = load_vertex_attrib(1).ok();
    let texcoord = load_vertex_attrib(2).ok();
    let uniforms = current_draw_uniforms()?;
    for out_index in 0..vertex_count {
        let source_index = read_element_index(type_, indices, out_index)?;
        write_draw_vertex(out_index, source_index, position, color, texcoord, uniforms)?;
    }
    Ok(vertex_count)
}

fn write_draw_vertex(
    out_index: usize,
    source_index: usize,
    position: VertexAttrib,
    color: Option<VertexAttrib>,
    texcoord: Option<VertexAttrib>,
    uniforms: DrawUniforms,
) -> Result<(), Error> {
    let mut vertex = Vertex::ZERO;
    let position_values =
        transform_position(read_attrib_values(position, source_index)?, uniforms.mvp);
    vertex.x = position_values[0];
    vertex.y = position_values[1];
    vertex.z = position_values[2];
    vertex.w = position_values[3];

    let color_values = match color {
        Some(color) => read_attrib_values(color, source_index)?,
        None => uniforms.current_color,
    };
    vertex.r = color_values[0] * uniforms.tint[0];
    vertex.g = color_values[1] * uniforms.tint[1];
    vertex.b = color_values[2] * uniforms.tint[2];
    vertex.a = color_values[3] * uniforms.tint[3];

    if let Some(texcoord) = texcoord {
        let texcoord_values = read_attrib_values(texcoord, source_index)?;
        vertex.u = texcoord_values[0];
        vertex.v = texcoord_values[1];
        vertex.s = texcoord_values[2];
        vertex.t = texcoord_values[3];
    }
    unsafe {
        write_volatile(draw_vertex_ptr(out_index), vertex);
    }
    Ok(())
}

fn load_vertex_attrib(index: usize) -> Result<VertexAttrib, Error> {
    let attrib = vertex_attrib_ptr(index);
    let value = unsafe { read_volatile(attrib) };
    if !value.enabled || value.buffer_name == 0 {
        return Err(Error::Invalid);
    }
    if value.type_ != GL_FLOAT {
        return Err(Error::Invalid);
    }
    Ok(value)
}

fn read_attrib_values(attrib: VertexAttrib, vertex_index: usize) -> Result<[f32; 4], Error> {
    let slot = find_buffer_slot(attrib.buffer_name).ok_or(Error::Invalid)?;
    let buffer = gl_buffer_ptr(slot);
    let len = unsafe { read_volatile(addr_of!((*buffer).len)) };
    let base = attrib
        .offset
        .checked_add(
            vertex_index
                .checked_mul(attrib.stride)
                .ok_or(Error::Invalid)?,
        )
        .ok_or(Error::Invalid)?;
    let mut values = [0.0, 0.0, 0.0, 1.0];
    let component_count = attrib.size as usize;
    for (component, value) in values.iter_mut().enumerate().take(component_count) {
        let offset = base
            .checked_add(component * size_of::<f32>())
            .ok_or(Error::Invalid)?;
        *value = read_f32_from_buffer(buffer, len, offset)?;
    }
    Ok(values)
}

fn transform_position(position: [f32; 4], matrix: [f32; 16]) -> [f32; 4] {
    [
        matrix[0] * position[0]
            + matrix[4] * position[1]
            + matrix[8] * position[2]
            + matrix[12] * position[3],
        matrix[1] * position[0]
            + matrix[5] * position[1]
            + matrix[9] * position[2]
            + matrix[13] * position[3],
        matrix[2] * position[0]
            + matrix[6] * position[1]
            + matrix[10] * position[2]
            + matrix[14] * position[3],
        matrix[3] * position[0]
            + matrix[7] * position[1]
            + matrix[11] * position[2]
            + matrix[15] * position[3],
    ]
}

fn effective_mvp_matrix(program_mvp: [f32; 16], program_mvp_set: bool) -> [f32; 16] {
    let fixed_active = unsafe { read_volatile(addr_of!(GLOBAL_FIXED_MATRIX_ACTIVE)) };
    if !fixed_active {
        return program_mvp;
    }
    let projection = unsafe { read_volatile(addr_of!(GLOBAL_PROJECTION_MATRIX)) };
    let modelview = unsafe { read_volatile(addr_of!(GLOBAL_MODELVIEW_MATRIX)) };
    let fixed_mvp = multiply_mat4(projection, modelview);
    if program_mvp_set {
        multiply_mat4(program_mvp, fixed_mvp)
    } else {
        fixed_mvp
    }
}

fn multiply_current_matrix(rhs: [f32; 16]) -> Result<(), Error> {
    let lhs = read_current_matrix()?;
    write_current_matrix(multiply_mat4(lhs, rhs))
}

fn read_current_matrix() -> Result<[f32; 16], Error> {
    let mode = unsafe { read_volatile(addr_of!(GLOBAL_MATRIX_MODE)) };
    match mode {
        GL_MODELVIEW => Ok(unsafe { read_volatile(addr_of!(GLOBAL_MODELVIEW_MATRIX)) }),
        GL_PROJECTION => Ok(unsafe { read_volatile(addr_of!(GLOBAL_PROJECTION_MATRIX)) }),
        _ => Err(Error::Invalid),
    }
}

fn write_current_matrix(matrix: [f32; 16]) -> Result<(), Error> {
    let mode = unsafe { read_volatile(addr_of!(GLOBAL_MATRIX_MODE)) };
    unsafe {
        match mode {
            GL_MODELVIEW => write_volatile(addr_of_mut!(GLOBAL_MODELVIEW_MATRIX), matrix),
            GL_PROJECTION => write_volatile(addr_of_mut!(GLOBAL_PROJECTION_MATRIX), matrix),
            _ => return Err(Error::Invalid),
        }
        write_volatile(addr_of_mut!(GLOBAL_FIXED_MATRIX_ACTIVE), true);
    }
    Ok(())
}

fn push_current_matrix() -> Result<(), u32> {
    let mode = unsafe { read_volatile(addr_of!(GLOBAL_MATRIX_MODE)) };
    unsafe {
        match mode {
            GL_MODELVIEW => {
                let depth = read_volatile(addr_of!(GLOBAL_MODELVIEW_STACK_DEPTH));
                if depth >= MAX_MODELVIEW_STACK_DEPTH {
                    return Err(GL_STACK_OVERFLOW);
                }
                let matrix = read_volatile(addr_of!(GLOBAL_MODELVIEW_MATRIX));
                write_volatile(
                    (addr_of_mut!(GLOBAL_MODELVIEW_STACK) as *mut [f32; 16]).add(depth),
                    matrix,
                );
                write_volatile(addr_of_mut!(GLOBAL_MODELVIEW_STACK_DEPTH), depth + 1);
            }
            GL_PROJECTION => {
                let depth = read_volatile(addr_of!(GLOBAL_PROJECTION_STACK_DEPTH));
                if depth >= MAX_PROJECTION_STACK_DEPTH {
                    return Err(GL_STACK_OVERFLOW);
                }
                let matrix = read_volatile(addr_of!(GLOBAL_PROJECTION_MATRIX));
                write_volatile(
                    (addr_of_mut!(GLOBAL_PROJECTION_STACK) as *mut [f32; 16]).add(depth),
                    matrix,
                );
                write_volatile(addr_of_mut!(GLOBAL_PROJECTION_STACK_DEPTH), depth + 1);
            }
            _ => return Err(GL_INVALID_OPERATION),
        }
        write_volatile(addr_of_mut!(GLOBAL_FIXED_MATRIX_ACTIVE), true);
    }
    Ok(())
}

fn pop_current_matrix() -> Result<(), u32> {
    let mode = unsafe { read_volatile(addr_of!(GLOBAL_MATRIX_MODE)) };
    unsafe {
        match mode {
            GL_MODELVIEW => {
                let depth = read_volatile(addr_of!(GLOBAL_MODELVIEW_STACK_DEPTH));
                if depth == 0 {
                    return Err(GL_STACK_UNDERFLOW);
                }
                let next_depth = depth - 1;
                let matrix = read_volatile(
                    (addr_of!(GLOBAL_MODELVIEW_STACK) as *const [f32; 16]).add(next_depth),
                );
                write_volatile(addr_of_mut!(GLOBAL_MODELVIEW_MATRIX), matrix);
                write_volatile(addr_of_mut!(GLOBAL_MODELVIEW_STACK_DEPTH), next_depth);
            }
            GL_PROJECTION => {
                let depth = read_volatile(addr_of!(GLOBAL_PROJECTION_STACK_DEPTH));
                if depth == 0 {
                    return Err(GL_STACK_UNDERFLOW);
                }
                let next_depth = depth - 1;
                let matrix = read_volatile(
                    (addr_of!(GLOBAL_PROJECTION_STACK) as *const [f32; 16]).add(next_depth),
                );
                write_volatile(addr_of_mut!(GLOBAL_PROJECTION_MATRIX), matrix);
                write_volatile(addr_of_mut!(GLOBAL_PROJECTION_STACK_DEPTH), next_depth);
            }
            _ => return Err(GL_INVALID_OPERATION),
        }
        write_volatile(addr_of_mut!(GLOBAL_FIXED_MATRIX_ACTIVE), true);
    }
    Ok(())
}

fn multiply_mat4(lhs: [f32; 16], rhs: [f32; 16]) -> [f32; 16] {
    let mut out = [0.0; 16];
    let mut column = 0;
    while column < 4 {
        let mut row = 0;
        while row < 4 {
            out[column * 4 + row] = lhs[row] * rhs[column * 4]
                + lhs[4 + row] * rhs[column * 4 + 1]
                + lhs[8 + row] * rhs[column * 4 + 2]
                + lhs[12 + row] * rhs[column * 4 + 3];
            row += 1;
        }
        column += 1;
    }
    out
}

fn transpose_mat4(matrix: [f32; 16]) -> [f32; 16] {
    [
        matrix[0], matrix[4], matrix[8], matrix[12], //
        matrix[1], matrix[5], matrix[9], matrix[13], //
        matrix[2], matrix[6], matrix[10], matrix[14], //
        matrix[3], matrix[7], matrix[11], matrix[15],
    ]
}

fn read_element_index(
    type_: u32,
    indices: *const c_void,
    element_index: usize,
) -> Result<usize, Error> {
    let element_buffer = unsafe { read_volatile(addr_of!(GLOBAL_CURRENT_ELEMENT_ARRAY_BUFFER)) };
    let element_bytes = index_element_bytes(type_)?;
    let offset = element_index
        .checked_mul(element_bytes)
        .and_then(|relative| relative.checked_add(indices as usize))
        .ok_or(Error::Invalid)?;
    if element_buffer == 0 {
        let index_ptr = offset as *const u8;
        return read_index_from_ptr(type_, index_ptr);
    }
    let slot = find_buffer_slot(element_buffer).ok_or(Error::Invalid)?;
    let buffer = gl_buffer_ptr(slot);
    let len = unsafe { read_volatile(addr_of!((*buffer).len)) };
    read_index_from_buffer(type_, buffer, len, offset)
}

fn index_element_bytes(type_: u32) -> Result<usize, Error> {
    match type_ {
        GL_UNSIGNED_BYTE => Ok(1),
        GL_UNSIGNED_SHORT => Ok(2),
        GL_UNSIGNED_INT => Ok(4),
        _ => Err(Error::Invalid),
    }
}

fn read_index_from_ptr(type_: u32, ptr: *const u8) -> Result<usize, Error> {
    if ptr.is_null() {
        return Err(Error::Invalid);
    }
    let value = unsafe {
        match type_ {
            GL_UNSIGNED_BYTE => *ptr as u32,
            GL_UNSIGNED_SHORT => {
                let bytes = [*ptr, *ptr.add(1)];
                u16::from_le_bytes(bytes) as u32
            }
            GL_UNSIGNED_INT => {
                let bytes = [*ptr, *ptr.add(1), *ptr.add(2), *ptr.add(3)];
                u32::from_le_bytes(bytes)
            }
            _ => return Err(Error::Invalid),
        }
    };
    Ok(value as usize)
}

fn read_index_from_buffer(
    type_: u32,
    buffer: *mut GlBuffer,
    len: usize,
    offset: usize,
) -> Result<usize, Error> {
    let element_bytes = index_element_bytes(type_)?;
    if offset.checked_add(element_bytes).ok_or(Error::Invalid)? > len {
        return Err(Error::Invalid);
    }
    let data = unsafe { (addr_of!((*buffer).data) as *const u8).add(offset) };
    read_index_from_ptr(type_, data)
}

fn read_f32_from_buffer(buffer: *mut GlBuffer, len: usize, offset: usize) -> Result<f32, Error> {
    if offset.checked_add(size_of::<f32>()).ok_or(Error::Invalid)? > len {
        return Err(Error::Invalid);
    }
    let data = unsafe { addr_of!((*buffer).data) as *const u8 };
    let bytes = unsafe {
        [
            *data.add(offset),
            *data.add(offset + 1),
            *data.add(offset + 2),
            *data.add(offset + 3),
        ]
    };
    Ok(f32::from_bits(u32::from_le_bytes(bytes)))
}

fn ensure_global_context() -> Result<&'static mut Context, Error> {
    unsafe {
        if !read_volatile(addr_of!(GLOBAL_CONTEXT_READY)) {
            let context = Context::connect_from_registry_shadow()?;
            write_volatile(addr_of_mut!(GLOBAL_CONTEXT), MaybeUninit::new(context));
            write_volatile(addr_of_mut!(GLOBAL_CONTEXT_READY), true);
        }
        Ok(&mut *(addr_of_mut!(GLOBAL_CONTEXT) as *mut Context))
    }
}

fn global_scratch() -> &'static mut [u8] {
    unsafe {
        slice::from_raw_parts_mut(addr_of_mut!(GLOBAL_SCRATCH) as *mut u8, FRAME_SCRATCH_BYTES)
    }
}

fn record_gl_error(error: u32) {
    if error == GL_NO_ERROR {
        return;
    }
    unsafe {
        if read_volatile(addr_of!(GLOBAL_LAST_ERROR)) == GL_NO_ERROR {
            write_volatile(addr_of_mut!(GLOBAL_LAST_ERROR), error);
        }
    }
}

fn error_to_gl_error(error: Error) -> u32 {
    match error {
        Error::Invalid => GL_INVALID_VALUE,
        Error::BufferTooSmall => GL_OUT_OF_MEMORY,
        Error::RequestAllocFailed | Error::ResponseAllocFailed | Error::BulkBufferAllocFailed => {
            GL_OUT_OF_MEMORY
        }
        Error::Unavailable
        | Error::MissingService
        | Error::EndpointNotFound
        | Error::EndpointInstallFailed
        | Error::ResponseGrantFailed
        | Error::RequestMapFailed
        | Error::ResponseMapFailed
        | Error::RequestSendFailed
        | Error::BulkBufferGrantFailed
        | Error::Timeout
        | Error::InvalidResponse
        | Error::IoError => GL_INVALID_OPERATION,
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
struct GpuService {
    process_slot: u64,
    endpoint_id: u64,
}

#[repr(C)]
struct CapctlRequest {
    magic: u64,
    version: u64,
    opcode: u64,
    request_seq: u64,
    response_paddr: u64,
    arg0: u64,
    arg1: u64,
    reserved0: u64,
}

#[repr(C)]
struct CapctlResponse {
    magic: u64,
    version: u64,
    opcode: u64,
    status: u64,
    response_seq: u64,
    detail: u64,
    block_process_slot: u64,
    block_endpoint_id: u64,
    status_flags: u64,
    block_profile: u64,
    gpu_process_slot: u64,
    gpu_endpoint_id: u64,
}

fn launch_gpu_from_capctl() -> Result<GpuService, Error> {
    let binding = unsafe {
        snapshot_service_registry_shadow()
            .and_then(|snapshot| snapshot.find_kind(ServiceKind::Capctl))
    }
    .ok_or(Error::MissingService)?;

    let request_page = vm::alloc_map_page(true).map_err(map_request_page_error)?;
    let response_page = vm::alloc_map_page(true).map_err(map_response_page_error)?;
    let request_va = request_page.va();
    let response_va = response_page.va();
    let request_paddr = request_page.paddr();
    let response_paddr = response_page.paddr();

    let options = ConnectOptions {
        request_va,
        response_va,
        endpoint_id: binding.endpoint_id,
        response_poll_limit: DEFAULT_RESPONSE_POLL_LIMIT,
        server_process_slot: binding.process_slot,
        compat_process_slot: binding.process_slot,
        allow_process_slot_compat: binding.process_slot != 0,
    };
    grant_response_cap(response_paddr, &options)?;

    clear_page(request_va);
    clear_page(response_va);
    let request = request_va as *mut CapctlRequest;
    unsafe {
        write_volatile(addr_of_mut!((*request).magic), CAPCTL_MAGIC);
        write_volatile(addr_of_mut!((*request).version), CAPCTL_VERSION);
        write_volatile(addr_of_mut!((*request).opcode), CAPCTL_OPCODE_LAUNCH_GPU);
        write_volatile(addr_of_mut!((*request).response_paddr), response_paddr);
        write_volatile(addr_of_mut!((*request).arg0), 0);
        write_volatile(addr_of_mut!((*request).arg1), 0);
        write_volatile(addr_of_mut!((*request).reserved0), 0);
    }
    compiler_fence(Ordering::SeqCst);
    unsafe {
        write_volatile(addr_of_mut!((*request).request_seq), 1);
    }

    let send = syscall::call2(syscall::SHARE_CAP, request_paddr, binding.endpoint_id);
    if send == syscall::ERR_ENDPOINT {
        return Err(Error::EndpointNotFound);
    }
    if send != syscall::OK {
        return Err(Error::RequestSendFailed);
    }

    let response = response_va as *mut CapctlResponse;
    let mut poll_count = 0;
    while poll_count < DEFAULT_RESPONSE_POLL_LIMIT {
        if unsafe { read_volatile(addr_of!((*response).response_seq)) } == 1 {
            let magic = unsafe { read_volatile(addr_of!((*response).magic)) };
            let version = unsafe { read_volatile(addr_of!((*response).version)) };
            let opcode = unsafe { read_volatile(addr_of!((*response).opcode)) };
            if magic != CAPCTL_MAGIC
                || version != CAPCTL_VERSION
                || opcode != CAPCTL_OPCODE_LAUNCH_GPU
            {
                return Err(Error::InvalidResponse);
            }
            let status = unsafe { read_volatile(addr_of!((*response).status)) };
            if status != CAPCTL_STATUS_OK && status != CAPCTL_STATUS_ALREADY {
                return match status {
                    3 => Err(Error::Unavailable),
                    5 => Err(Error::IoError),
                    _ => Err(Error::Invalid),
                };
            }
            let process_slot = unsafe { read_volatile(addr_of!((*response).gpu_process_slot)) };
            let endpoint_id = unsafe { read_volatile(addr_of!((*response).gpu_endpoint_id)) };
            if endpoint_id == 0 {
                return Err(Error::EndpointNotFound);
            }
            return Ok(GpuService {
                process_slot,
                endpoint_id,
            });
        }
        let _ = syscall::call2(syscall::WAIT_EVENT, 0, 1);
        poll_count += 1;
    }
    Err(Error::Timeout)
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct ConnectOptions {
    pub request_va: u64,
    pub response_va: u64,
    pub endpoint_id: u64,
    pub response_poll_limit: u64,
    pub server_process_slot: u64,
    pub compat_process_slot: u64,
    pub allow_process_slot_compat: bool,
}

impl Default for ConnectOptions {
    fn default() -> Self {
        Self {
            request_va: 0,
            response_va: 0,
            endpoint_id: DEFAULT_ENDPOINT_ID,
            response_poll_limit: DEFAULT_RESPONSE_POLL_LIMIT,
            server_process_slot: 0,
            compat_process_slot: 0,
            allow_process_slot_compat: false,
        }
    }
}

fn grant_response_cap(response_paddr: u64, options: &ConnectOptions) -> Result<(), Error> {
    let rights = PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE;
    let result = syscall::call3(
        syscall::GRANT_CAP_ON_ENDPOINT,
        response_paddr,
        options.endpoint_id,
        rights,
    );
    if result == syscall::OK {
        return Ok(());
    }
    if result != syscall::ERR_ENDPOINT {
        return Err(Error::ResponseGrantFailed);
    }
    if !options.allow_process_slot_compat || options.compat_process_slot == 0 {
        return Err(Error::EndpointNotFound);
    }
    if syscall::call3(
        syscall::INSTALL_ENDPOINT,
        0,
        options.endpoint_id,
        options.compat_process_slot,
    ) != syscall::OK
    {
        return Err(Error::EndpointInstallFailed);
    }
    let retry = syscall::call3(
        syscall::GRANT_CAP_ON_ENDPOINT,
        response_paddr,
        options.endpoint_id,
        rights,
    );
    if retry == syscall::OK {
        return Ok(());
    }
    if retry == syscall::ERR_ENDPOINT {
        return Err(Error::EndpointNotFound);
    }
    Err(Error::ResponseGrantFailed)
}

fn make_session_nonce(request_paddr: u64, response_paddr: u64, endpoint_id: u64, tag: u64) -> u64 {
    let nonce = request_paddr
        ^ response_paddr.rotate_left(17)
        ^ endpoint_id.rotate_left(7)
        ^ tag
        ^ 0xa24b_aed4_963e_e407;
    if nonce == 0 { 1 } else { nonce }
}

fn alloc_page() -> Option<u64> {
    let raw = syscall::call0(syscall::ALLOC_PAGE);
    if raw < 0x1000 { None } else { Some(raw) }
}

fn map_request_page_error(err: SyscallError) -> Error {
    match err {
        SyscallError::Alloc => Error::RequestAllocFailed,
        SyscallError::Map => Error::RequestMapFailed,
        _ => Error::RequestMapFailed,
    }
}

fn map_response_page_error(err: SyscallError) -> Error {
    match err {
        SyscallError::Alloc => Error::ResponseAllocFailed,
        SyscallError::Map => Error::ResponseMapFailed,
        _ => Error::ResponseMapFailed,
    }
}

fn clear_page(va: u64) {
    let words = va as *mut u64;
    let mut index = 0;
    while index < PAGE_BYTES / size_of::<u64>() {
        unsafe { write_volatile(words.add(index), 0) };
        index += 1;
    }
}

fn copy_bytes_to_volatile(dst: *mut u8, src: &[u8]) {
    let mut index = 0;
    while index < src.len() {
        unsafe { write_volatile(dst.add(index), src[index]) };
        index += 1;
    }
}

fn copy_raw_bytes(dst: *mut u8, src: *const u8, len: usize) {
    let mut index = 0;
    while index < len {
        unsafe {
            write_volatile(dst.add(index), read_volatile(src.add(index)));
        }
        index += 1;
    }
}

fn parse_status(raw: i32) -> Option<Status> {
    match raw {
        0 => Some(Status::Ok),
        1 => Some(Status::Invalid),
        2 => Some(Status::Unavailable),
        3 => Some(Status::IoError),
        _ => None,
    }
}
