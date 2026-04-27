#![no_std]
#![no_main]

extern crate alloc;

use rt_alloc as _;

const FONT_REGULAR_PATH: &str = "/share/fonts/JetBrainsMono-Regular.capfont";
const FONT_BOLD_PATH: &str = "/share/fonts/JetBrainsMono-Bold.capfont";
const INITIAL_WINDOW_WIDTH: u32 = 760;
const INITIAL_WINDOW_HEIGHT: u32 = 440;
const MIN_SURFACE_WIDTH: u32 = 420;
const MIN_SURFACE_HEIGHT: u32 = 320;
const MAX_SURFACE_WIDTH: u32 = 1920;
const MAX_SURFACE_HEIGHT: u32 = 1080;
const PADDING_X: f32 = 28.0;
const PADDING_Y: f32 = 34.0;
const CELL_SCALE: f32 = 1.0;
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
        Ok(()) => {}
        Err(err) => cap_std::println!("Pitty: failed: {:?}", err)?,
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
    regular: capfont::LoadedFont,
    bold: capfont::LoadedFont,
}

impl FontBook {
    fn load() -> Result<Self, AppError> {
        let root = cap_std::fs::RootDir::connect_default().map_err(capfont::Error::from)?;
        Ok(Self {
            regular: capfont::LoadedFont::load_from_root(&root, FONT_REGULAR_PATH)?,
            bold: capfont::LoadedFont::load_from_root(&root, FONT_BOLD_PATH)?,
        })
    }

    fn font(&self, face: Face) -> Result<capfont::Font<'_>, AppError> {
        match face {
            Face::Regular => Ok(self.regular.font()?),
            Face::Bold => Ok(self.bold.font()?),
        }
    }
}

fn run() -> Result<(), AppError> {
    let mut client = cap_window::Client::connect_from_registry_shadow()?;
    client.hello()?;
    let preferred_size = choose_surface_size(&mut client)?;
    let mut window = client.create_window("pitty", preferred_size.width, preferred_size.height)?;

    let mut context = cap_window::connect_gl_for_window(window)?;
    let mut scratch = [0_u8; caplibgl::FRAME_SCRATCH_BYTES];
    let mut atlases = UploadedFontAtlases::new();
    let fonts = FontBook::load()?;
    let render_canvas = Canvas::new(window.content_size.width, window.content_size.height);

    render_scene(
        &mut context,
        &mut scratch,
        render_canvas,
        &fonts,
        &mut atlases,
    )?;
    client.present(window)?;
    cap_std::println!(
        "Pitty: ok window={} surface={} gpu_resource={} gpu_surface={} size={}x{}",
        window.id,
        window.surface_id,
        window.gpu_resource_id,
        window.gpu_surface_id,
        window.content_size.width,
        window.content_size.height
    )
    .ok();

    loop {
        if let Some(event) = client.poll_event()? {
            match event.kind {
                cap_window::protocol::EventKind::Configure if event.window_id == window.id => {
                    let configure = cap_window::protocol::configure_event_from_parts(
                        event.window_id,
                        event.surface_id,
                        cap_window::protocol::pack_event_result_flags(event.kind, event.flags),
                        event.arg0,
                        event.arg1,
                    );
                    window.surface_id = configure.surface_id;
                    window.configure_serial = configure.configure_serial;
                    client.ack_configure(window.id, configure.configure_serial)?;
                }
                cap_window::protocol::EventKind::Close if event.window_id == window.id => {
                    let _ = client.destroy_window(window.id);
                    return Ok(());
                }
                _ => {}
            }
        }
        wait_ticks(1);
    }
}

fn render_scene(
    context: &mut caplibgl::Context,
    mut scratch: &mut [u8],
    canvas: Canvas,
    fonts: &FontBook,
    atlases: &mut UploadedFontAtlases,
) -> Result<(), AppError> {
    context.clear_color(
        caplibgl::Color {
            r: 0.0,
            g: 0.0,
            b: 0.0,
            a: 0.0,
        },
        &mut scratch,
    )?;
    context.gl_enable(caplibgl::GL_BLEND, &mut scratch)?;
    context.gl_blend_func(
        caplibgl::GL_SRC_ALPHA,
        caplibgl::GL_ONE_MINUS_SRC_ALPHA,
        &mut scratch,
    )?;

    draw_chrome(context, scratch, canvas)?;
    draw_terminal_text(context, scratch, canvas, fonts, atlases)?;
    Ok(())
}

fn choose_surface_size(
    client: &mut cap_window::Client,
) -> Result<cap_window::protocol::Size, AppError> {
    let output = client.query_output()?;
    let recommended = output.recommended_content_size;
    Ok(cap_window::protocol::Size {
        width: recommended
            .width
            .clamp(MIN_SURFACE_WIDTH, MAX_SURFACE_WIDTH)
            .max(INITIAL_WINDOW_WIDTH.min(MAX_SURFACE_WIDTH)),
        height: recommended
            .height
            .clamp(MIN_SURFACE_HEIGHT, MAX_SURFACE_HEIGHT)
            .max(INITIAL_WINDOW_HEIGHT.min(MAX_SURFACE_HEIGHT)),
    })
}

fn wait_ticks(ticks: u64) {
    let _ = rt_core::syscall::call2(rt_core::syscall::WAIT_EVENT, 0, ticks);
}

fn draw_chrome(
    context: &mut caplibgl::Context,
    scratch: &mut [u8],
    canvas: Canvas,
) -> Result<(), AppError> {
    draw_rect(
        context,
        scratch,
        canvas,
        0.0,
        0.0,
        canvas.width_f32(),
        canvas.height_f32(),
        [0.004, 0.007, 0.012, 0.68],
    )?;
    draw_rect(
        context,
        scratch,
        canvas,
        0.0,
        0.0,
        canvas.width_f32(),
        38.0,
        [0.028, 0.120, 0.150, 0.54],
    )?;
    draw_rect(
        context,
        scratch,
        canvas,
        0.0,
        38.0,
        canvas.width_f32(),
        1.0,
        [0.28, 0.95, 1.00, 0.36],
    )?;

    draw_border(context, scratch, canvas, 2.0, [0.16, 0.86, 0.98, 0.66])?;
    draw_rect(
        context,
        scratch,
        canvas,
        6.0,
        6.0,
        (canvas.width_f32() - 12.0).max(1.0),
        (canvas.height_f32() - 12.0).max(1.0),
        [0.03, 0.13, 0.16, 0.10],
    )?;

    let mut y = 62.0;
    while y < canvas.height_f32() - 18.0 {
        draw_rect(
            context,
            scratch,
            canvas,
            18.0,
            y,
            (canvas.width_f32() - 36.0).max(1.0),
            1.0,
            [0.20, 0.70, 0.80, 0.055],
        )?;
        y += 24.0;
    }

    draw_rect(
        context,
        scratch,
        canvas,
        18.0,
        78.0,
        (canvas.width_f32() - 36.0).max(1.0),
        72.0,
        [0.04, 0.42, 0.38, 0.10],
    )?;
    draw_rect(
        context,
        scratch,
        canvas,
        18.0,
        150.0,
        (canvas.width_f32() - 36.0).max(1.0),
        92.0,
        [0.11, 0.17, 0.52, 0.08],
    )?;
    draw_rect(
        context,
        scratch,
        canvas,
        18.0,
        242.0,
        (canvas.width_f32() - 36.0).max(1.0),
        122.0,
        [0.34, 0.10, 0.42, 0.07],
    )?;

    Ok(())
}

fn draw_terminal_text(
    context: &mut caplibgl::Context,
    scratch: &mut [u8],
    canvas: Canvas,
    fonts: &FontBook,
    atlases: &mut UploadedFontAtlases,
) -> Result<(), AppError> {
    draw_text(
        context,
        scratch,
        canvas,
        fonts,
        atlases,
        Face::Bold,
        22.0,
        9.0,
        0.74,
        "pitty",
        [0.62, 0.98, 0.92, 0.96],
    )?;
    draw_text(
        context,
        scratch,
        canvas,
        fonts,
        atlases,
        Face::Regular,
        92.0,
        9.0,
        0.74,
        "GPU terminal",
        [0.58, 0.72, 0.78, 0.86],
    )?;
    draw_text(
        context,
        scratch,
        canvas,
        fonts,
        atlases,
        Face::Regular,
        (canvas.width_f32() - 176.0).max(250.0),
        9.0,
        0.74,
        "capwm / virgl",
        [0.26, 0.90, 1.00, 0.78],
    )?;

    let regular_font = fonts.font(Face::Regular)?;
    let line_h = regular_font.line_height() as f32 * CELL_SCALE * FONT_RENDER_SCALE + 5.0;
    let mut y = PADDING_Y + 38.0;
    draw_line(
        context,
        scratch,
        canvas,
        fonts,
        atlases,
        y,
        "$",
        " pitty",
        [0.38, 0.95, 0.80, 0.96],
    )?;
    y += line_h;
    y = draw_wrapped_text(
        context,
        scratch,
        canvas,
        fonts,
        atlases,
        Face::Regular,
        PADDING_X,
        y,
        (canvas.width_f32() - PADDING_X * 2.0).max(1.0),
        CELL_SCALE,
        "CapabilityOS pitty 0.1",
        [0.86, 0.92, 0.96, 0.95],
    )?;
    y = draw_wrapped_text(
        context,
        scratch,
        canvas,
        fonts,
        atlases,
        Face::Regular,
        PADDING_X,
        y,
        (canvas.width_f32() - PADDING_X * 2.0).max(1.0),
        CELL_SCALE,
        "renderer: caplibgl app-surface",
        [0.56, 0.70, 0.78, 0.88],
    )?;
    y = draw_wrapped_text(
        context,
        scratch,
        canvas,
        fonts,
        atlases,
        Face::Regular,
        PADDING_X,
        y,
        (canvas.width_f32() - PADDING_X * 2.0).max(1.0),
        CELL_SCALE,
        "wm: Pachaland / capwm",
        [0.56, 0.70, 0.78, 0.88],
    )?;
    y = draw_wrapped_text(
        context,
        scratch,
        canvas,
        fonts,
        atlases,
        Face::Regular,
        PADDING_X,
        y,
        (canvas.width_f32() - PADDING_X * 2.0).max(1.0),
        CELL_SCALE,
        "font: JetBrains Mono",
        [0.56, 0.70, 0.78, 0.88],
    )?;
    y += line_h * 0.35;
    draw_line(
        context,
        scratch,
        canvas,
        fonts,
        atlases,
        y,
        "$",
        " echo $TERM",
        [0.38, 0.95, 0.80, 0.96],
    )?;
    y += line_h;
    draw_text(
        context,
        scratch,
        canvas,
        fonts,
        atlases,
        Face::Regular,
        PADDING_X,
        y,
        CELL_SCALE,
        "pitty-capgl",
        [0.86, 0.92, 0.96, 0.95],
    )?;
    y += line_h * 1.35;
    draw_line(
        context,
        scratch,
        canvas,
        fonts,
        atlases,
        y,
        "$",
        " _",
        [0.38, 0.95, 0.80, 0.96],
    )?;

    let cursor_x =
        PADDING_X + regular_font.cell_advance() as f32 * CELL_SCALE * FONT_RENDER_SCALE * 2.0;
    let cursor_y = y + 3.0;
    draw_rect(
        context,
        scratch,
        canvas,
        cursor_x,
        cursor_y,
        regular_font.cell_advance() as f32 * CELL_SCALE * FONT_RENDER_SCALE,
        regular_font.cell_height() as f32 * CELL_SCALE * FONT_RENDER_SCALE,
        [0.32, 0.95, 0.84, 0.34],
    )?;
    Ok(())
}

fn draw_line(
    context: &mut caplibgl::Context,
    scratch: &mut [u8],
    canvas: Canvas,
    fonts: &FontBook,
    atlases: &mut UploadedFontAtlases,
    y: f32,
    prompt: &str,
    text: &str,
    prompt_color: [f32; 4],
) -> Result<(), AppError> {
    draw_text(
        context,
        scratch,
        canvas,
        fonts,
        atlases,
        Face::Bold,
        PADDING_X,
        y,
        CELL_SCALE,
        prompt,
        prompt_color,
    )?;
    draw_text(
        context,
        scratch,
        canvas,
        fonts,
        atlases,
        Face::Regular,
        PADDING_X
            + fonts.font(Face::Regular)?.cell_advance() as f32 * CELL_SCALE * FONT_RENDER_SCALE,
        y,
        CELL_SCALE,
        text,
        [0.86, 0.92, 0.96, 0.95],
    )
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
            let line = trim_ascii(&text[line_start..break_at]);
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

    let line = trim_ascii(&text[line_start..]);
    if !line.is_empty() {
        draw_text(
            context, scratch, canvas, fonts, atlases, face, x, y, scale, line, color,
        )?;
        y += line_h;
    }
    Ok(y)
}

fn trim_ascii(text: &str) -> &str {
    text.trim_matches(' ')
}

fn draw_border(
    context: &mut caplibgl::Context,
    scratch: &mut [u8],
    canvas: Canvas,
    width: f32,
    color: [f32; 4],
) -> Result<(), AppError> {
    draw_rect(
        context,
        scratch,
        canvas,
        0.0,
        0.0,
        canvas.width_f32(),
        width,
        color,
    )?;
    draw_rect(
        context,
        scratch,
        canvas,
        0.0,
        canvas.height_f32() - width,
        canvas.width_f32(),
        width,
        color,
    )?;
    draw_rect(
        context,
        scratch,
        canvas,
        0.0,
        0.0,
        width,
        canvas.height_f32(),
        color,
    )?;
    draw_rect(
        context,
        scratch,
        canvas,
        canvas.width_f32() - width,
        0.0,
        width,
        canvas.height_f32(),
        color,
    )
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
enum Face {
    Regular,
    Bold,
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
