#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write as _;

use pachaland_assets::cantarell_pachafetch as cantarell;
use pitty_assets::jetbrains_mono_pachafetch as jetbrains;
use rt_alloc as _;

const WINDOW_WIDTH: u32 = 600;
const WINDOW_HEIGHT: u32 = 360;

fn main() -> cap_std::Result<()> {
    match run() {
        Ok(window) => {
            cap_std::println!(
                "PachaFetch: ok window={} surface={} gpu_resource={} gpu_surface={} size={}x{}",
                window.id,
                window.surface_id,
                window.gpu_resource_id,
                window.gpu_surface_id,
                window.size.width,
                window.size.height
            )?;
        }
        Err(err) => {
            cap_std::println!("PachaFetch: failed: {:?}", err)?;
        }
    }
    Ok(())
}

cap_std::entry_point!(main);

#[derive(Copy, Clone, Debug)]
#[allow(dead_code)]
enum AppError {
    Window(capwm_client::Error),
    Gl(caplibgl::Error),
    MissingGlyph,
    OutOfMemory,
}

impl From<capwm_client::Error> for AppError {
    fn from(value: capwm_client::Error) -> Self {
        Self::Window(value)
    }
}

impl From<caplibgl::Error> for AppError {
    fn from(value: caplibgl::Error) -> Self {
        Self::Gl(value)
    }
}

fn run() -> Result<capwm_client::protocol::Window, AppError> {
    let mut client = capwm_client::Client::connect_from_registry_shadow()?;
    client.hello()?;
    let window = client.create_window("pachafetch", WINDOW_WIDTH, WINDOW_HEIGHT)?;
    let _ = client.set_geometry(window, 72, 72, WINDOW_WIDTH, WINDOW_HEIGHT);

    let mut context = capwm_client::connect_gl_for_window(window)?;
    let mut scratch = [0_u8; caplibgl::FRAME_SCRATCH_BYTES];
    let mut glyphs = UploadedGlyphs::new();

    context.clear_color(
        caplibgl::Color {
            r: 0.025,
            g: 0.030,
            b: 0.040,
            a: 1.0,
        },
        &mut scratch,
    )?;
    context.gl_enable(caplibgl::GL_BLEND, &mut scratch)?;
    context.gl_blend_func(
        caplibgl::GL_SRC_ALPHA,
        caplibgl::GL_ONE_MINUS_SRC_ALPHA,
        &mut scratch,
    )?;

    draw_rect(
        &mut context,
        &mut scratch,
        0.0,
        0.0,
        WINDOW_WIDTH as f32,
        WINDOW_HEIGHT as f32,
        [0.025, 0.030, 0.040, 1.0],
    )?;
    draw_rect(
        &mut context,
        &mut scratch,
        0.0,
        0.0,
        112.0,
        WINDOW_HEIGHT as f32,
        [0.055, 0.075, 0.095, 1.0],
    )?;
    draw_rect(
        &mut context,
        &mut scratch,
        24.0,
        34.0,
        64.0,
        64.0,
        [0.15, 0.73, 0.66, 0.92],
    )?;
    draw_rect(
        &mut context,
        &mut scratch,
        40.0,
        50.0,
        64.0,
        64.0,
        [0.30, 0.38, 0.95, 0.72],
    )?;

    draw_text(
        &mut context,
        &mut scratch,
        &mut glyphs,
        Face::Cantarell,
        134.0,
        34.0,
        1.35,
        "CapabilityOS",
        [0.88, 0.94, 0.96, 1.0],
    )?;
    draw_text(
        &mut context,
        &mut scratch,
        &mut glyphs,
        Face::JetBrains,
        136.0,
        78.0,
        1.0,
        "pachafetch.elf",
        [0.34, 0.90, 0.78, 1.0],
    )?;

    let caps = context.caps();
    let mut features = String::new();
    write!(
        &mut features,
        "virgl:  0x{:X} capset {}:{}",
        caps.features, caps.capset_id, caps.capset_max_version
    )
    .map_err(|_| AppError::OutOfMemory)?;

    let memory_line = memory_line();
    let storage_line = storage_line();
    let lines = [
        "wm:     Pachaland / capwm",
        "gpu:    virtio-gpu virgl",
        "font:   Cantarell + JetBrains Mono",
        "api:    capwm_client + caplibgl",
        "path:   /cmd/pachafetch.elf",
        memory_line.as_str(),
        storage_line.as_str(),
    ];

    let mut y = 132.0;
    for line in lines {
        draw_text(
            &mut context,
            &mut scratch,
            &mut glyphs,
            Face::JetBrains,
            136.0,
            y,
            1.0,
            line,
            [0.74, 0.80, 0.86, 1.0],
        )?;
        y += 28.0;
    }
    draw_text(
        &mut context,
        &mut scratch,
        &mut glyphs,
        Face::JetBrains,
        136.0,
        y,
        1.0,
        features.as_str(),
        [0.62, 0.70, 0.78, 1.0],
    )?;

    client.present(window)?;
    Ok(window)
}

fn memory_line() -> String {
    let mut line = String::from("mem:    unavailable");
    if let Ok(stats) = rt_core::memory_stats() {
        line.clear();
        let _ = write!(
            &mut line,
            "mem:    {} / {}",
            format_mib(stats.used_bytes),
            format_mib(stats.total_bytes)
        );
    }
    line
}

fn storage_line() -> String {
    let mut line = String::from("disk:   unavailable");
    if let Ok(root) = cap_std::fs::RootDir::connect_default() {
        if let Ok(stats) = root.storage_stats() {
            line.clear();
            let _ = write!(
                &mut line,
                "disk:   {} / {}",
                format_mib(stats.used_bytes()),
                format_mib(stats.capacity_bytes())
            );
        }
    }
    line
}

fn format_mib(bytes: u64) -> String {
    let whole = bytes / (1024 * 1024);
    let frac = ((bytes % (1024 * 1024)) * 10) / (1024 * 1024);
    let mut out = String::new();
    let _ = write!(&mut out, "{}.{}MiB", whole, frac);
    out
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
enum Face {
    Cantarell,
    JetBrains,
}

#[derive(Copy, Clone)]
struct Metrics {
    codepoint: u32,
    advance: u32,
    atlas_x: u32,
    atlas_y: u32,
}

#[derive(Copy, Clone)]
struct UploadedGlyph {
    face: Face,
    codepoint: u32,
    resource_id: u32,
}

struct UploadedGlyphs {
    items: Vec<UploadedGlyph>,
}

impl UploadedGlyphs {
    fn new() -> Self {
        Self { items: Vec::new() }
    }

    fn get_or_upload(
        &mut self,
        context: &mut caplibgl::Context,
        face: Face,
        codepoint: u32,
    ) -> Result<u32, AppError> {
        for item in self.items.iter().copied() {
            if item.face == face && item.codepoint == codepoint {
                return Ok(item.resource_id);
            }
        }

        let metrics = glyph_metrics(face, codepoint)?;
        let pixels = glyph_rgba(face, metrics)?;
        let resource_id =
            context.upload_texture_2d(0, face_cell_width(face), face_cell_height(face), &pixels)?;
        self.items.push(UploadedGlyph {
            face,
            codepoint,
            resource_id,
        });
        Ok(resource_id)
    }
}

fn glyph_metrics(face: Face, codepoint: u32) -> Result<Metrics, AppError> {
    match face {
        Face::Cantarell => cantarell::GLYPHS
            .iter()
            .copied()
            .find(|glyph| glyph.codepoint == codepoint)
            .or_else(|| cantarell::GLYPHS.get(cantarell::FALLBACK_INDEX).copied())
            .map(|glyph| Metrics {
                codepoint: glyph.codepoint,
                advance: glyph.advance as u32,
                atlas_x: glyph.atlas_x as u32,
                atlas_y: glyph.atlas_y as u32,
            })
            .ok_or(AppError::MissingGlyph),
        Face::JetBrains => jetbrains::GLYPHS
            .iter()
            .copied()
            .find(|glyph| glyph.codepoint == codepoint)
            .or_else(|| jetbrains::GLYPHS.get(jetbrains::FALLBACK_INDEX).copied())
            .map(|glyph| Metrics {
                codepoint: glyph.codepoint,
                advance: glyph.advance as u32,
                atlas_x: glyph.atlas_x as u32,
                atlas_y: glyph.atlas_y as u32,
            })
            .ok_or(AppError::MissingGlyph),
    }
}

fn glyph_rgba(face: Face, metrics: Metrics) -> Result<Vec<u8>, AppError> {
    let width = face_cell_width(face) as usize;
    let height = face_cell_height(face) as usize;
    let atlas_width = face_atlas_width(face) as usize;
    let atlas = face_atlas_alpha(face);
    let mut pixels = Vec::new();
    pixels
        .try_reserve_exact(width * height * 4)
        .map_err(|_| AppError::OutOfMemory)?;
    pixels.resize(width * height * 4, 0);

    for y in 0..height {
        for x in 0..width {
            let atlas_index =
                (metrics.atlas_y as usize + y) * atlas_width + metrics.atlas_x as usize + x;
            let alpha = atlas.get(atlas_index).copied().unwrap_or(0);
            let dst = (y * width + x) * 4;
            pixels[dst] = 255;
            pixels[dst + 1] = 255;
            pixels[dst + 2] = 255;
            pixels[dst + 3] = alpha;
        }
    }
    Ok(pixels)
}

fn draw_text(
    context: &mut caplibgl::Context,
    scratch: &mut [u8],
    glyphs: &mut UploadedGlyphs,
    face: Face,
    mut x: f32,
    y: f32,
    scale: f32,
    text: &str,
    color: [f32; 4],
) -> Result<(), AppError> {
    let cell_width = face_cell_width(face) as f32 * scale;
    let cell_height = face_cell_height(face) as f32 * scale;
    for byte in text.bytes() {
        let codepoint = if (0x20..=0x7E).contains(&byte) {
            byte as u32
        } else {
            b'?' as u32
        };
        let metrics = glyph_metrics(face, codepoint)?;
        if codepoint != b' ' as u32 {
            let resource_id = glyphs.get_or_upload(context, face, metrics.codepoint)?;
            draw_textured_rect(
                context,
                scratch,
                x,
                y,
                cell_width,
                cell_height,
                color,
                resource_id,
            )?;
        }
        x += metrics.advance as f32 * scale;
    }
    Ok(())
}

fn draw_rect(
    context: &mut caplibgl::Context,
    scratch: &mut [u8],
    x: f32,
    y: f32,
    width: f32,
    height: f32,
    color: [f32; 4],
) -> Result<(), AppError> {
    let vertices = quad_vertices(x, y, width, height, color);
    context.gl_draw_vertices(caplibgl::Primitive::Triangles, &vertices, None, scratch)?;
    Ok(())
}

fn draw_textured_rect(
    context: &mut caplibgl::Context,
    scratch: &mut [u8],
    x: f32,
    y: f32,
    width: f32,
    height: f32,
    color: [f32; 4],
    resource_id: u32,
) -> Result<(), AppError> {
    let vertices = textured_quad_vertices(x, y, width, height, color);
    context.gl_draw_vertices(
        caplibgl::Primitive::Triangles,
        &vertices,
        Some(resource_id),
        scratch,
    )?;
    Ok(())
}

fn quad_vertices(
    x: f32,
    y: f32,
    width: f32,
    height: f32,
    color: [f32; 4],
) -> [caplibgl::Vertex; 6] {
    let (x0, y0) = pixel_to_ndc(x, y);
    let (x1, y1) = pixel_to_ndc(x + width, y + height);
    [
        solid_vertex(x0, y0, color),
        solid_vertex(x0, y1, color),
        solid_vertex(x1, y0, color),
        solid_vertex(x1, y0, color),
        solid_vertex(x0, y1, color),
        solid_vertex(x1, y1, color),
    ]
}

fn textured_quad_vertices(
    x: f32,
    y: f32,
    width: f32,
    height: f32,
    color: [f32; 4],
) -> [caplibgl::Vertex; 6] {
    let (x0, y0) = pixel_to_ndc(x, y);
    let (x1, y1) = pixel_to_ndc(x + width, y + height);
    [
        textured_vertex(x0, y0, color, 0.0, 0.0),
        textured_vertex(x0, y1, color, 0.0, 1.0),
        textured_vertex(x1, y0, color, 1.0, 0.0),
        textured_vertex(x1, y0, color, 1.0, 0.0),
        textured_vertex(x0, y1, color, 0.0, 1.0),
        textured_vertex(x1, y1, color, 1.0, 1.0),
    ]
}

fn solid_vertex(x: f32, y: f32, rgba: [f32; 4]) -> caplibgl::Vertex {
    textured_vertex(x, y, rgba, 0.0, 0.0)
}

fn textured_vertex(x: f32, y: f32, rgba: [f32; 4], u: f32, v: f32) -> caplibgl::Vertex {
    caplibgl::Vertex {
        x,
        y,
        z: 0.0,
        w: 1.0,
        r: rgba[0],
        g: rgba[1],
        b: rgba[2],
        a: rgba[3],
        u,
        v,
        s: 0.0,
        t: 1.0,
    }
}

fn pixel_to_ndc(x: f32, y: f32) -> (f32, f32) {
    (
        x / WINDOW_WIDTH as f32 * 2.0 - 1.0,
        1.0 - y / WINDOW_HEIGHT as f32 * 2.0,
    )
}

fn face_cell_width(face: Face) -> u32 {
    match face {
        Face::Cantarell => cantarell::CELL_WIDTH,
        Face::JetBrains => jetbrains::CELL_WIDTH,
    }
}

fn face_cell_height(face: Face) -> u32 {
    match face {
        Face::Cantarell => cantarell::CELL_HEIGHT,
        Face::JetBrains => jetbrains::CELL_HEIGHT,
    }
}

fn face_atlas_width(face: Face) -> u32 {
    match face {
        Face::Cantarell => cantarell::ATLAS_WIDTH,
        Face::JetBrains => jetbrains::ATLAS_WIDTH,
    }
}

fn face_atlas_alpha(face: Face) -> &'static [u8] {
    match face {
        Face::Cantarell => &cantarell::ATLAS_ALPHA,
        Face::JetBrains => &jetbrains::ATLAS_ALPHA,
    }
}
