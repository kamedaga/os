#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use core::fmt::Write as _;

use rt_alloc as _;

const FONT_CANTARELL_PATH: &str = "/share/fonts/Cantarell-Regular.capfont";
const FONT_JETBRAINS_PATH: &str = "/share/fonts/JetBrainsMono-Regular.capfont";
const WINDOW_WIDTH: u32 = 600;
const WINDOW_HEIGHT: u32 = 360;
const FONT_RENDER_SCALE: f32 = 0.25;
const MAX_ATLAS_UPLOAD_BYTES: u32 = caplibgl::TEXTURE_BULK_UPLOAD_BYTES as u32;
const TEXT_BATCH_GLYPHS: usize = 8;
const TEXT_BATCH_VERTICES: usize = TEXT_BATCH_GLYPHS * 6;
const MAX_UPLOADED_FONT_ATLASES: usize = 2;
const EMPTY_VERTEX: caplibgl::Vertex = caplibgl::Vertex {
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

fn main() -> cap_std::Result<()> {
    match run() {
        Ok(window) => {
            cap_std::println!(
                "PachaFetch: ok window={} surface={} gpu_resource={} gpu_surface={} size={}x{}",
                window.id,
                window.surface_id,
                window.gpu_resource_id,
                window.gpu_surface_id,
                window.content_size.width,
                window.content_size.height
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
    Window(cap_window::Error),
    Gl(caplibgl::Error),
    Font(capfont::Error),
    MissingGlyph,
    OutOfMemory,
}

impl From<cap_window::Error> for AppError {
    fn from(value: cap_window::Error) -> Self {
        Self::Window(value)
    }
}

impl From<caplibgl::Error> for AppError {
    fn from(value: caplibgl::Error) -> Self {
        Self::Gl(value)
    }
}

impl From<capfont::Error> for AppError {
    fn from(value: capfont::Error) -> Self {
        Self::Font(value)
    }
}

#[derive(Copy, Clone)]
struct Canvas {
    width: u32,
    height: u32,
}

impl Canvas {
    const fn new(width: u32, height: u32) -> Self {
        Self { width, height }
    }

    fn width_f32(self) -> f32 {
        self.width as f32
    }

    fn height_f32(self) -> f32 {
        self.height as f32
    }
}

struct FontBook {
    cantarell: capfont::LoadedFont,
    jetbrains: capfont::LoadedFont,
}

impl FontBook {
    fn load(root: &cap_std::fs::RootDir) -> Result<Self, AppError> {
        Ok(Self {
            cantarell: capfont::LoadedFont::load_from_root(root, FONT_CANTARELL_PATH)?,
            jetbrains: capfont::LoadedFont::load_from_root(root, FONT_JETBRAINS_PATH)?,
        })
    }

    fn font(&self, face: Face) -> Result<capfont::Font<'_>, AppError> {
        match face {
            Face::Cantarell => Ok(self.cantarell.font()?),
            Face::JetBrains => Ok(self.jetbrains.font()?),
        }
    }
}

fn run() -> Result<cap_window::protocol::Window, AppError> {
    let mut client = cap_window::Client::connect_from_registry_shadow()?;
    client.hello()?;
    let window = client.create_window("pachafetch", WINDOW_WIDTH, WINDOW_HEIGHT)?;
    let canvas = Canvas::new(window.content_size.width, window.content_size.height);

    let mut context = cap_window::connect_gl_for_window(window)?;
    let mut scratch = [0_u8; caplibgl::FRAME_SCRATCH_BYTES];
    let mut atlases = UploadedFontAtlases::new();
    let root = cap_std::fs::RootDir::connect_default().map_err(capfont::Error::from)?;
    let fonts = FontBook::load(&root)?;

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
        canvas,
        0.0,
        0.0,
        canvas.width_f32(),
        canvas.height_f32(),
        [0.025, 0.030, 0.040, 1.0],
    )?;
    let side_width = (if canvas.width < 360 {
        74.0_f32
    } else {
        112.0_f32
    })
    .min(canvas.width_f32());
    let text_x = (side_width + 22.0).min((canvas.width_f32() - 16.0).max(12.0));
    draw_rect(
        &mut context,
        &mut scratch,
        canvas,
        0.0,
        0.0,
        side_width,
        canvas.height_f32(),
        [0.055, 0.075, 0.095, 1.0],
    )?;
    let logo_size = if canvas.width < 360 { 46.0 } else { 64.0 };
    let logo_x = ((side_width - logo_size) * 0.5).max(10.0);
    let logo_y = 34.0f32.min((canvas.height_f32() - logo_size - 12.0).max(12.0));
    draw_rect(
        &mut context,
        &mut scratch,
        canvas,
        logo_x,
        logo_y,
        logo_size,
        logo_size,
        [0.15, 0.73, 0.66, 0.92],
    )?;
    draw_rect(
        &mut context,
        &mut scratch,
        canvas,
        logo_x + logo_size * 0.25,
        logo_y + logo_size * 0.25,
        logo_size,
        logo_size,
        [0.30, 0.38, 0.95, 0.72],
    )?;

    let text_max = (canvas.width_f32() - text_x - 18.0).max(40.0);
    let mut y = 34.0;
    y = draw_wrapped_text(
        &mut context,
        &mut scratch,
        canvas,
        &fonts,
        &mut atlases,
        Face::Cantarell,
        text_x,
        y,
        text_max,
        1.35,
        "CapabilityOS",
        [0.88, 0.94, 0.96, 1.0],
    )?;
    y = draw_wrapped_text(
        &mut context,
        &mut scratch,
        canvas,
        &fonts,
        &mut atlases,
        Face::JetBrains,
        text_x + 2.0,
        y + 4.0,
        text_max,
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
    let storage_line = storage_line(&root);
    let lines = [
        "wm:     Pachaland / cap_window",
        "gpu:    virtio-gpu virgl",
        "font:   Cantarell + JetBrains Mono",
        "api:    cap_window + caplibgl",
        "path:   /cmd/pachafetch.elf",
        memory_line.as_str(),
        storage_line.as_str(),
    ];

    y += 22.0;
    for line in lines {
        y = draw_wrapped_text(
            &mut context,
            &mut scratch,
            canvas,
            &fonts,
            &mut atlases,
            Face::JetBrains,
            text_x + 2.0,
            y,
            text_max,
            1.0,
            line,
            [0.74, 0.80, 0.86, 1.0],
        )?;
    }
    draw_wrapped_text(
        &mut context,
        &mut scratch,
        canvas,
        &fonts,
        &mut atlases,
        Face::JetBrains,
        text_x + 2.0,
        y + 2.0,
        text_max,
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

fn storage_line(root: &cap_std::fs::RootDir) -> String {
    let mut line = String::from("disk:   unavailable");
    if let Ok(stats) = root.storage_stats() {
        line.clear();
        let _ = write!(
            &mut line,
            "disk:   {} / {}",
            format_mib(stats.used_bytes()),
            format_mib(stats.capacity_bytes())
        );
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
struct UploadedFontAtlas {
    face: Face,
    resource_id: u32,
    width: u32,
    height: u32,
}

struct UploadedFontAtlases {
    items: [Option<UploadedFontAtlas>; MAX_UPLOADED_FONT_ATLASES],
    len: usize,
}

impl UploadedFontAtlases {
    fn new() -> Self {
        Self {
            items: [None; MAX_UPLOADED_FONT_ATLASES],
            len: 0,
        }
    }

    fn get_or_upload(
        &mut self,
        context: &mut caplibgl::Context,
        font: capfont::Font<'_>,
        face: Face,
    ) -> Result<UploadedFontAtlas, AppError> {
        for item in self.items[..self.len].iter().copied().flatten() {
            if item.face == face {
                return Ok(item);
            }
        }

        let resource_id =
            context.create_alpha_texture_2d(font.atlas_width(), font.atlas_height())?;
        upload_font_atlas(context, resource_id, font)?;
        let uploaded = UploadedFontAtlas {
            face,
            resource_id,
            width: font.atlas_width(),
            height: font.atlas_height(),
        };
        if self.len >= self.items.len() {
            return Err(AppError::OutOfMemory);
        }
        self.items[self.len] = Some(uploaded);
        self.len += 1;
        Ok(uploaded)
    }
}

fn atlas_upload_rows(font: capfont::Font<'_>) -> u32 {
    let row_bytes = font.atlas_width().max(1);
    (MAX_ATLAS_UPLOAD_BYTES / row_bytes)
        .max(1)
        .min(font.atlas_height())
}

fn upload_font_atlas(
    context: &mut caplibgl::Context,
    resource_id: u32,
    font: capfont::Font<'_>,
) -> Result<(), AppError> {
    let width = font.atlas_width() as usize;
    let rows_per_upload = atlas_upload_rows(font);
    let atlas = font.atlas_alpha();
    let mut y = 0_u32;
    while y < font.atlas_height() {
        let height = rows_per_upload.min(font.atlas_height() - y);
        let start = y as usize * width;
        let end = start + width * height as usize;
        context.update_texture_alpha_2d(
            resource_id,
            0,
            y,
            font.atlas_width(),
            height,
            &atlas[start..end],
        )?;
        y += height;
    }
    Ok(())
}

struct TextBatch {
    vertices: [caplibgl::Vertex; TEXT_BATCH_VERTICES],
    len: usize,
}

impl TextBatch {
    fn new() -> Self {
        Self {
            vertices: [EMPTY_VERTEX; TEXT_BATCH_VERTICES],
            len: 0,
        }
    }

    fn has_space_for_quad(&self) -> bool {
        self.len + 6 <= self.vertices.len()
    }

    fn push_quad(&mut self, quad: [caplibgl::Vertex; 6]) {
        let mut index = 0;
        while index < quad.len() {
            self.vertices[self.len + index] = quad[index];
            index += 1;
        }
        self.len += quad.len();
    }

    fn as_slice(&self) -> &[caplibgl::Vertex] {
        &self.vertices[..self.len]
    }

    fn clear(&mut self) {
        self.len = 0;
    }
}

fn draw_text(
    context: &mut caplibgl::Context,
    scratch: &mut [u8],
    canvas: Canvas,
    fonts: &FontBook,
    atlases: &mut UploadedFontAtlases,
    face: Face,
    mut x: f32,
    y: f32,
    scale: f32,
    text: &str,
    color: [f32; 4],
) -> Result<(), AppError> {
    let font = fonts.font(face)?;
    let atlas = atlases.get_or_upload(context, font, face)?;
    let scale = scale * FONT_RENDER_SCALE;
    let cell_width = font.cell_width() as f32 * scale;
    let cell_height = font.cell_height() as f32 * scale;
    let mut batch = TextBatch::new();
    for byte in text.bytes() {
        let codepoint = if (0x20..=0x7E).contains(&byte) {
            byte as u32
        } else {
            b'?' as u32
        };
        let glyph = font.glyph_or_fallback(codepoint)?;
        if codepoint != b' ' as u32 {
            if !batch.has_space_for_quad() {
                flush_text_batch(context, scratch, &mut batch, atlas.resource_id)?;
            }
            let u0 = glyph.atlas_x as f32 / atlas.width as f32;
            let v0 = glyph.atlas_y as f32 / atlas.height as f32;
            let u1 = (glyph.atlas_x as u32 + font.cell_width()) as f32 / atlas.width as f32;
            let v1 = (glyph.atlas_y as u32 + font.cell_height()) as f32 / atlas.height as f32;
            batch.push_quad(textured_quad_vertices(
                canvas,
                x,
                y,
                cell_width,
                cell_height,
                color,
                [u0, v0, u1, v1],
            ));
        }
        x += glyph.advance as f32 * scale;
    }
    flush_text_batch(context, scratch, &mut batch, atlas.resource_id)?;
    Ok(())
}

fn draw_wrapped_text(
    context: &mut caplibgl::Context,
    scratch: &mut [u8],
    canvas: Canvas,
    fonts: &FontBook,
    atlases: &mut UploadedFontAtlases,
    face: Face,
    x: f32,
    mut y: f32,
    max_width: f32,
    scale: f32,
    text: &str,
    color: [f32; 4],
) -> Result<f32, AppError> {
    let font = fonts.font(face)?;
    let scaled = scale * FONT_RENDER_SCALE;
    let line_h = font.line_height() as f32 * scaled + 5.0;
    let bytes = text.as_bytes();
    let mut line_start = 0usize;
    let mut index = 0usize;
    let mut line_width = 0.0f32;
    let mut last_space: Option<usize> = None;

    while index < bytes.len() {
        let byte = bytes[index];
        let codepoint = if (0x20..=0x7E).contains(&byte) {
            byte as u32
        } else {
            b'?' as u32
        };
        let glyph = font.glyph_or_fallback(codepoint)?;
        let advance = glyph.advance as f32 * scaled;
        if byte == b' ' {
            last_space = Some(index);
        }
        if line_width + advance > max_width && index > line_start {
            let break_at = last_space
                .filter(|space| *space > line_start)
                .unwrap_or(index);
            let line = text[line_start..break_at].trim_matches(' ');
            if !line.is_empty() {
                draw_text(
                    context, scratch, canvas, fonts, atlases, face, x, y, scale, line, color,
                )?;
            }
            y += line_h;
            line_start = if break_at < index {
                break_at + 1
            } else {
                index
            };
            index = line_start;
            line_width = 0.0;
            last_space = None;
            continue;
        }
        line_width += advance;
        index += 1;
    }

    let line = text[line_start..].trim_matches(' ');
    if !line.is_empty() {
        draw_text(
            context, scratch, canvas, fonts, atlases, face, x, y, scale, line, color,
        )?;
        y += line_h;
    }
    Ok(y)
}

fn flush_text_batch(
    context: &mut caplibgl::Context,
    scratch: &mut [u8],
    vertices: &mut TextBatch,
    resource_id: u32,
) -> Result<(), AppError> {
    if vertices.as_slice().is_empty() {
        return Ok(());
    }
    context.gl_draw_alpha_texture_vertices(
        caplibgl::Primitive::Triangles,
        vertices.as_slice(),
        resource_id,
        scratch,
    )?;
    vertices.clear();
    Ok(())
}

fn draw_rect(
    context: &mut caplibgl::Context,
    scratch: &mut [u8],
    canvas: Canvas,
    x: f32,
    y: f32,
    width: f32,
    height: f32,
    color: [f32; 4],
) -> Result<(), AppError> {
    let vertices = quad_vertices(canvas, x, y, width, height, color);
    context.gl_draw_vertices(caplibgl::Primitive::Triangles, &vertices, None, scratch)?;
    Ok(())
}

fn quad_vertices(
    canvas: Canvas,
    x: f32,
    y: f32,
    width: f32,
    height: f32,
    color: [f32; 4],
) -> [caplibgl::Vertex; 6] {
    let (x0, y0) = pixel_to_ndc(canvas, x, y);
    let (x1, y1) = pixel_to_ndc(canvas, x + width, y + height);
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
    canvas: Canvas,
    x: f32,
    y: f32,
    width: f32,
    height: f32,
    color: [f32; 4],
    uv: [f32; 4],
) -> [caplibgl::Vertex; 6] {
    let (x0, y0) = pixel_to_ndc(canvas, x, y);
    let (x1, y1) = pixel_to_ndc(canvas, x + width, y + height);
    let [u0, v0, u1, v1] = uv;
    [
        textured_vertex(x0, y0, color, u0, v0),
        textured_vertex(x0, y1, color, u0, v1),
        textured_vertex(x1, y0, color, u1, v0),
        textured_vertex(x1, y0, color, u1, v0),
        textured_vertex(x0, y1, color, u0, v1),
        textured_vertex(x1, y1, color, u1, v1),
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

fn pixel_to_ndc(canvas: Canvas, x: f32, y: f32) -> (f32, f32) {
    (
        x / canvas.width_f32() * 2.0 - 1.0,
        1.0 - y / canvas.height_f32() * 2.0,
    )
}
