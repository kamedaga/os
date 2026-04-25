use crate::capwm::{AppSurface, CompositedWindow, Position, Size};

const PANEL_HEIGHT: u32 = 28;
const WINDOW_BORDER: u32 = 3;
const TITLEBAR_HEIGHT: u32 = 24;
const SHADOW_OFFSET: i32 = 7;
const LOADING_CELL_TARGET_WIDTH: u32 = 96;
const LOADING_CELL_TARGET_HEIGHT: u32 = 88;
const LOADING_MIN_COLUMNS: u32 = 6;
const LOADING_MAX_COLUMNS: u32 = 24;
const LOADING_MIN_ROWS: u32 = 4;
const LOADING_MAX_ROWS: u32 = 14;
const LOADING_BORDER: u32 = 3;
const LOADING_GLOSS_PHASE_STEP: f32 = 0.095;
const LOADING_GLOSS_STRENGTH: f32 = -0.78;
const LOADING_GLOSS_CYCLES: f32 = 1.0;

pub struct GpuCompositor {
    context: Option<caplibgl::Context>,
    features: u64,
    capset_id: u32,
    capset_max_version: u32,
    app_surface_supported: bool,
    scratch: [u8; caplibgl::FRAME_SCRATCH_BYTES],
    cursor_position: Option<Position>,
    hardware_cursor_supported: bool,
    hardware_cursor_active: bool,
    loading_frame_index: u32,
}

impl GpuCompositor {
    pub fn connect() -> Self {
        let mut compositor = Self::missing();
        compositor.try_connect();
        compositor
    }

    fn try_connect(&mut self) -> bool {
        if self.context.is_some() {
            return true;
        }
        let Ok(context) = caplibgl::Context::connect_from_registry_shadow() else {
            return false;
        };
        let caps = context.caps();
        self.features = caps.features;
        self.capset_id = caps.capset_id;
        self.capset_max_version = caps.capset_max_version;
        self.app_surface_supported = (caps.features & caplibgl::FEATURE_APP_SURFACE) != 0;
        self.hardware_cursor_supported = (caps.features & caplibgl::FEATURE_CURSOR) != 0;
        self.context = Some(context);
        true
    }

    pub const fn missing() -> Self {
        Self {
            context: None,
            features: 0,
            capset_id: 0,
            capset_max_version: 0,
            app_surface_supported: false,
            scratch: [0; caplibgl::FRAME_SCRATCH_BYTES],
            cursor_position: None,
            hardware_cursor_supported: false,
            hardware_cursor_active: false,
            loading_frame_index: 0,
        }
    }

    pub fn create_app_surface(&mut self, size: Size) -> Option<AppSurface> {
        if !self.try_connect() {
            return None;
        }
        if !self.app_surface_supported {
            return None;
        }
        let Some(context) = self.context.as_mut() else {
            return None;
        };
        match context.client().create_app_surface(size.width, size.height) {
            Ok(target) if target.resource_id != 0 && target.surface_id != 0 => Some(AppSurface {
                resource_id: target.resource_id,
                gpu_surface_id: target.surface_id,
                size: Size {
                    width: target.width,
                    height: target.height,
                },
            }),
            Ok(_) => None,
            Err(_) => {
                self.context = None;
                None
            }
        }
    }

    pub fn destroy_app_surface(&mut self, app_surface: AppSurface) {
        let Some(context) = self.context.as_mut() else {
            return;
        };
        let _ = context.client().delete_texture_2d(app_surface.resource_id);
    }

    pub fn compose_windows(&mut self, windows: &[CompositedWindow]) -> bool {
        if !self.try_connect() {
            return false;
        }
        let target = {
            let Some(context) = self.context.as_ref() else {
                return false;
            };
            context.default_surface()
        };
        if self.hardware_cursor_supported {
            let position = self
                .cursor_position
                .unwrap_or_else(|| cursor_position_for_target(target));
            let _ = self.move_hardware_cursor(position);
        }
        let Some(context) = self.context.as_mut() else {
            return false;
        };
        let has_loading_windows = windows.iter().any(|window| window.presented_frames == 0);
        let loading_phase = self.loading_frame_index as f32 * LOADING_GLOSS_PHASE_STEP;
        context.make_surface_current(target);
        let color = caplibgl::Color {
            r: 0.03,
            g: 0.045,
            b: 0.065,
            a: 1.0,
        };
        let result = context
            .gl_disable(caplibgl::GL_SCISSOR_TEST, &mut self.scratch)
            .and_then(|_| context.clear_color(color, &mut self.scratch))
            .and_then(|_| context.gl_enable(caplibgl::GL_BLEND, &mut self.scratch))
            .and_then(|_| {
                context.gl_blend_func(
                    caplibgl::GL_SRC_ALPHA,
                    caplibgl::GL_ONE_MINUS_SRC_ALPHA,
                    &mut self.scratch,
                )
            })
            .and_then(|_| {
                draw_desktop_panel(context, target, &mut self.scratch)?;
                for window in windows {
                    draw_composited_window(
                        context,
                        target,
                        window,
                        loading_phase,
                        &mut self.scratch,
                    )?;
                }
                if !self.hardware_cursor_supported {
                    let cursor_position = self
                        .cursor_position
                        .unwrap_or_else(|| cursor_position_for_target(target));
                    draw_cursor(context, target, cursor_position, &mut self.scratch)?;
                }
                context.gl_present()
            });
        match result {
            Ok(_) => {
                if has_loading_windows {
                    self.loading_frame_index = self.loading_frame_index.wrapping_add(1);
                }
                true
            }
            Err(_) => {
                self.context = None;
                false
            }
        }
    }

    pub fn target_size(&mut self) -> Option<Size> {
        if !self.try_connect() {
            return None;
        }
        let context = self.context.as_ref()?;
        let target = context.default_surface();
        Some(Size {
            width: target.width,
            height: target.height,
        })
    }

    pub fn set_cursor_position(&mut self, position: Position) {
        self.cursor_position = Some(position);
    }

    pub fn move_hardware_cursor(&mut self, position: Position) -> bool {
        if !self.hardware_cursor_supported || !self.try_connect() {
            return false;
        }
        let Some(context) = self.context.as_mut() else {
            return false;
        };
        match context.client().set_cursor_position(position.x, position.y) {
            Ok(()) => {
                self.hardware_cursor_active = true;
                true
            }
            Err(_) => {
                self.hardware_cursor_active = false;
                false
            }
        }
    }

    pub fn status_label(&self) -> &'static str {
        if self.context.is_none() {
            "missing"
        } else if self.app_surface_supported {
            "app_surface"
        } else {
            "ready"
        }
    }

    pub const fn features(&self) -> u64 {
        self.features
    }

    pub const fn capset_id(&self) -> u32 {
        self.capset_id
    }

    pub const fn capset_max_version(&self) -> u32 {
        self.capset_max_version
    }
}

fn cursor_position_for_target(target: caplibgl::RenderTarget) -> Position {
    Position {
        x: (target.width / 2) as i32,
        y: (target.height / 2) as i32,
    }
}

fn draw_composited_window(
    context: &mut caplibgl::Context,
    target: caplibgl::RenderTarget,
    window: &CompositedWindow,
    loading_phase: f32,
    scratch: &mut [u8],
) -> Result<(), caplibgl::Error> {
    draw_window_frame(context, target, window, scratch)?;
    let content_position = Position {
        x: window.position.x + WINDOW_BORDER as i32,
        y: window.position.y + TITLEBAR_HEIGHT as i32,
    };
    let content_size = Size {
        width: window.size.width.saturating_sub(WINDOW_BORDER * 2).max(1),
        height: window
            .size
            .height
            .saturating_sub(TITLEBAR_HEIGHT + WINDOW_BORDER)
            .max(1),
    };
    if window.presented_frames == 0 {
        return draw_loading_placeholder(
            context,
            target,
            content_position,
            content_size,
            loading_phase,
            scratch,
        );
    }
    let vertices =
        window_quad_vertices(target.width, target.height, content_position, content_size);
    context.gl_draw_vertices(
        caplibgl::Primitive::TriangleStrip,
        &vertices,
        Some(window.app_surface.resource_id),
        scratch,
    )
}

fn draw_desktop_panel(
    context: &mut caplibgl::Context,
    target: caplibgl::RenderTarget,
    scratch: &mut [u8],
) -> Result<(), caplibgl::Error> {
    draw_rect(
        context,
        target,
        Position { x: 0, y: 0 },
        Size {
            width: target.width,
            height: PANEL_HEIGHT.min(target.height.max(1)),
        },
        [0.025, 0.032, 0.045, 0.94],
        scratch,
    )?;
    draw_rect(
        context,
        target,
        Position {
            x: 0,
            y: PANEL_HEIGHT as i32,
        },
        Size {
            width: target.width,
            height: 1,
        },
        [0.18, 0.24, 0.32, 0.85],
        scratch,
    )
}

fn draw_window_frame(
    context: &mut caplibgl::Context,
    target: caplibgl::RenderTarget,
    window: &CompositedWindow,
    scratch: &mut [u8],
) -> Result<(), caplibgl::Error> {
    draw_rect(
        context,
        target,
        Position {
            x: window.position.x + SHADOW_OFFSET,
            y: window.position.y + SHADOW_OFFSET,
        },
        window.size,
        [0.0, 0.0, 0.0, 0.28],
        scratch,
    )?;
    draw_rect(
        context,
        target,
        window.position,
        window.size,
        if window.focused {
            [0.24, 0.63, 0.72, 1.0]
        } else {
            [0.09, 0.13, 0.18, 1.0]
        },
        scratch,
    )?;
    let inner_position = Position {
        x: window.position.x + WINDOW_BORDER as i32,
        y: window.position.y + WINDOW_BORDER as i32,
    };
    let inner_size = Size {
        width: window.size.width.saturating_sub(WINDOW_BORDER * 2).max(1),
        height: window.size.height.saturating_sub(WINDOW_BORDER * 2).max(1),
    };
    draw_rect(
        context,
        target,
        inner_position,
        inner_size,
        [0.035, 0.047, 0.064, 1.0],
        scratch,
    )?;
    draw_rect(
        context,
        target,
        inner_position,
        Size {
            width: inner_size.width,
            height: TITLEBAR_HEIGHT
                .saturating_sub(WINDOW_BORDER)
                .min(inner_size.height),
        },
        if window.focused {
            [0.11, 0.20, 0.25, 1.0]
        } else {
            [0.06, 0.08, 0.11, 1.0]
        },
        scratch,
    )
}

fn draw_cursor(
    context: &mut caplibgl::Context,
    target: caplibgl::RenderTarget,
    position: Position,
    scratch: &mut [u8],
) -> Result<(), caplibgl::Error> {
    let shadow = cursor_vertices_from_points(
        target.width,
        target.height,
        cursor_shadow_points(position),
        cursor_shadow_color(),
    );
    context.gl_draw_vertices(caplibgl::Primitive::Triangles, &shadow, None, scratch)?;

    let outline = cursor_vertices_from_points(
        target.width,
        target.height,
        cursor_outline_points(position),
        cursor_outline_color(),
    );
    context.gl_draw_vertices(caplibgl::Primitive::Triangles, &outline, None, scratch)?;

    let fill = cursor_vertices_from_points(
        target.width,
        target.height,
        cursor_fill_points(position),
        cursor_fill_color(),
    );
    context.gl_draw_vertices(caplibgl::Primitive::Triangles, &fill, None, scratch)
}

fn draw_loading_placeholder(
    context: &mut caplibgl::Context,
    target: caplibgl::RenderTarget,
    position: Position,
    size: Size,
    phase: f32,
    scratch: &mut [u8],
) -> Result<(), caplibgl::Error> {
    draw_rect(
        context,
        target,
        position,
        size,
        [0.015, 0.023, 0.032, 0.96],
        scratch,
    )?;

    let border = LOADING_BORDER
        .min(size.width / 3)
        .min(size.height / 3)
        .max(1);
    let grid_position = Position {
        x: position.x + border as i32,
        y: position.y + border as i32,
    };
    let grid_size = Size {
        width: size.width.saturating_sub(border * 2).max(1),
        height: size.height.saturating_sub(border * 2).max(1),
    };
    let columns = clamp_u32(
        grid_size.width.div_ceil(LOADING_CELL_TARGET_WIDTH),
        LOADING_MIN_COLUMNS,
        LOADING_MAX_COLUMNS,
    )
    .min(grid_size.width.max(1));
    let rows = clamp_u32(
        grid_size.height.div_ceil(LOADING_CELL_TARGET_HEIGHT),
        LOADING_MIN_ROWS,
        LOADING_MAX_ROWS,
    )
    .min(grid_size.height.max(1));

    let mut row = 0;
    while row < rows {
        let mut col = 0;
        while col < columns {
            let x0 = grid_position.x + ((col * grid_size.width) / columns) as i32;
            let x1 = grid_position.x + (((col + 1) * grid_size.width) / columns) as i32;
            let y0 = grid_position.y + ((row * grid_size.height) / rows) as i32;
            let y1 = grid_position.y + (((row + 1) * grid_size.height) / rows) as i32;
            let color = loading_cell_color(col, row, columns, rows);
            draw_rect(
                context,
                target,
                Position { x: x0, y: y0 },
                Size {
                    width: (x1 - x0).max(1) as u32,
                    height: (y1 - y0).max(1) as u32,
                },
                color,
                scratch,
            )?;
            col += 1;
        }
        row += 1;
    }

    let gloss_vertices =
        loading_gloss_vertices(target.width, target.height, grid_position, grid_size, phase);
    context.draw_loading_gloss_overlay(&gloss_vertices, scratch)?;

    draw_rect(
        context,
        target,
        position,
        Size {
            width: size.width,
            height: border,
        },
        [0.18, 0.68, 0.78, 0.92],
        scratch,
    )?;
    draw_rect(
        context,
        target,
        Position {
            x: position.x,
            y: position.y + size.height.saturating_sub(border) as i32,
        },
        Size {
            width: size.width,
            height: border,
        },
        [0.13, 0.56, 0.66, 0.88],
        scratch,
    )?;
    draw_rect(
        context,
        target,
        position,
        Size {
            width: border,
            height: size.height,
        },
        [0.18, 0.68, 0.78, 0.92],
        scratch,
    )?;
    draw_rect(
        context,
        target,
        Position {
            x: position.x + size.width.saturating_sub(border) as i32,
            y: position.y,
        },
        Size {
            width: border,
            height: size.height,
        },
        [0.13, 0.56, 0.66, 0.88],
        scratch,
    )
}

fn loading_cell_color(col: u32, row: u32, columns: u32, rows: u32) -> [f32; 4] {
    let nx = ratio(col, columns.saturating_sub(1).max(1));
    let ny = ratio(row, rows.saturating_sub(1).max(1));
    let checker = if ((col ^ row) & 1) == 0 { 0.09 } else { -0.07 };
    let r = clamp_f32(0.02 + 0.78 * ny + checker, 0.0, 1.0);
    let g = clamp_f32(
        0.26 + 0.52 * (1.0 - ny) + 0.22 * (1.0 - nx) + checker,
        0.0,
        1.0,
    );
    let b = clamp_f32(0.07 + 0.68 * nx + 0.24 * ny + checker * 0.65, 0.0, 1.0);
    [r, g, b, 0.88]
}

fn loading_gloss_vertices(
    target_width: u32,
    target_height: u32,
    position: Position,
    size: Size,
    phase: f32,
) -> [caplibgl::Vertex; 4] {
    let target_w = target_width.max(1) as f32;
    let target_h = target_height.max(1) as f32;
    let left = position.x.max(0) as f32;
    let top = position.y.max(0) as f32;
    let right = left + size.width.max(1) as f32;
    let bottom = top + size.height.max(1) as f32;
    let x0 = pixel_x_to_ndc(left, target_w);
    let x1 = pixel_x_to_ndc(right, target_w);
    let y0 = pixel_y_to_ndc(top, target_h);
    let y1 = pixel_y_to_ndc(bottom, target_h);
    [
        loading_gloss_vertex(x0, y0, 0.0, phase),
        loading_gloss_vertex(x0, y1, 0.0, phase),
        loading_gloss_vertex(x1, y0, LOADING_GLOSS_CYCLES, phase),
        loading_gloss_vertex(x1, y1, LOADING_GLOSS_CYCLES, phase),
    ]
}

fn loading_gloss_vertex(x: f32, y: f32, u: f32, phase: f32) -> caplibgl::Vertex {
    caplibgl::Vertex {
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
        t: LOADING_GLOSS_STRENGTH,
    }
}

fn ratio(value: u32, max: u32) -> f32 {
    value as f32 / max.max(1) as f32
}

fn clamp_u32(value: u32, min: u32, max: u32) -> u32 {
    value.max(min).min(max)
}

fn clamp_f32(value: f32, min: f32, max: f32) -> f32 {
    if value < min {
        min
    } else if value > max {
        max
    } else {
        value
    }
}

fn draw_rect(
    context: &mut caplibgl::Context,
    target: caplibgl::RenderTarget,
    position: Position,
    size: Size,
    color: [f32; 4],
    scratch: &mut [u8],
) -> Result<(), caplibgl::Error> {
    let vertices = colored_rect_vertices(target.width, target.height, position, size, color);
    context.gl_draw_vertices(caplibgl::Primitive::TriangleStrip, &vertices, None, scratch)
}

fn colored_rect_vertices(
    target_width: u32,
    target_height: u32,
    position: Position,
    size: Size,
    color: [f32; 4],
) -> [caplibgl::Vertex; 4] {
    let target_w = target_width.max(1) as f32;
    let target_h = target_height.max(1) as f32;
    let left_px = position.x.max(0) as u32;
    let top_px = position.y.max(0) as u32;
    let max_w = target_width.saturating_sub(left_px).max(1);
    let max_h = target_height.saturating_sub(top_px).max(1);
    let left = left_px as f32;
    let top = top_px as f32;
    let right = left + size.width.min(max_w).max(1) as f32;
    let bottom = top + size.height.min(max_h).max(1) as f32;
    [
        colored_vertex(
            pixel_x_to_ndc(left, target_w),
            pixel_y_to_ndc(top, target_h),
            color,
        ),
        colored_vertex(
            pixel_x_to_ndc(left, target_w),
            pixel_y_to_ndc(bottom, target_h),
            color,
        ),
        colored_vertex(
            pixel_x_to_ndc(right, target_w),
            pixel_y_to_ndc(top, target_h),
            color,
        ),
        colored_vertex(
            pixel_x_to_ndc(right, target_w),
            pixel_y_to_ndc(bottom, target_h),
            color,
        ),
    ]
}

fn cursor_vertices_from_points(
    target_width: u32,
    target_height: u32,
    points: [(f32, f32); 7],
    color: [f32; 4],
) -> [caplibgl::Vertex; 15] {
    let p = points;
    [
        point_vertex(target_width, target_height, p[0], color),
        point_vertex(target_width, target_height, p[1], color),
        point_vertex(target_width, target_height, p[2], color),
        point_vertex(target_width, target_height, p[0], color),
        point_vertex(target_width, target_height, p[2], color),
        point_vertex(target_width, target_height, p[6], color),
        point_vertex(target_width, target_height, p[2], color),
        point_vertex(target_width, target_height, p[3], color),
        point_vertex(target_width, target_height, p[4], color),
        point_vertex(target_width, target_height, p[2], color),
        point_vertex(target_width, target_height, p[4], color),
        point_vertex(target_width, target_height, p[5], color),
        point_vertex(target_width, target_height, p[2], color),
        point_vertex(target_width, target_height, p[5], color),
        point_vertex(target_width, target_height, p[6], color),
    ]
}

fn cursor_outline_points(position: Position) -> [(f32, f32); 7] {
    let x = position.x as f32;
    let y = position.y as f32;
    [
        (x, y),
        (x, y + 23.0),
        (x + 6.0, y + 18.0),
        (x + 10.0, y + 29.0),
        (x + 15.0, y + 27.0),
        (x + 11.0, y + 16.0),
        (x + 20.0, y + 16.0),
    ]
}

fn cursor_fill_points(position: Position) -> [(f32, f32); 7] {
    let x = position.x as f32 + 2.0;
    let y = position.y as f32 + 3.0;
    [
        (x, y),
        (x, y + 16.0),
        (x + 5.0, y + 12.0),
        (x + 9.0, y + 23.0),
        (x + 11.5, y + 22.0),
        (x + 7.5, y + 11.0),
        (x + 14.5, y + 11.0),
    ]
}

fn cursor_shadow_points(position: Position) -> [(f32, f32); 7] {
    let mut points = cursor_outline_points(position);
    for point in points.iter_mut() {
        point.0 += 2.0;
        point.1 += 2.0;
    }
    points
}

fn window_quad_vertices(
    target_width: u32,
    target_height: u32,
    position: Position,
    surface_size: Size,
) -> [caplibgl::Vertex; 4] {
    let target_w = target_width.max(1) as f32;
    let target_h = target_height.max(1) as f32;
    let left_px = position.x.max(0) as u32;
    let top_px = position.y.max(0) as u32;
    let max_w = target_width.saturating_sub(left_px).max(1);
    let max_h = target_height.saturating_sub(top_px).max(1);
    let window_w = surface_size.width.min(max_w) as f32;
    let window_h = surface_size.height.min(max_h) as f32;
    let left = left_px as f32;
    let top = top_px as f32;
    let right = left + window_w;
    let bottom = top + window_h;
    let x0 = pixel_x_to_ndc(left, target_w);
    let x1 = pixel_x_to_ndc(right, target_w);
    let y0 = pixel_y_to_ndc(top, target_h);
    let y1 = pixel_y_to_ndc(bottom, target_h);
    [
        vertex(x0, y0, 0.0, 0.0),
        vertex(x0, y1, 0.0, 1.0),
        vertex(x1, y0, 1.0, 0.0),
        vertex(x1, y1, 1.0, 1.0),
    ]
}

fn colored_pixel_vertex(
    target_width: u32,
    target_height: u32,
    x: f32,
    y: f32,
    color: [f32; 4],
) -> caplibgl::Vertex {
    let target_w = target_width.max(1) as f32;
    let target_h = target_height.max(1) as f32;
    caplibgl::Vertex {
        x: pixel_x_to_ndc(x, target_w),
        y: pixel_y_to_ndc(y, target_h),
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

fn point_vertex(
    target_width: u32,
    target_height: u32,
    point: (f32, f32),
    color: [f32; 4],
) -> caplibgl::Vertex {
    colored_pixel_vertex(target_width, target_height, point.0, point.1, color)
}

fn cursor_fill_color() -> [f32; 4] {
    [0.95, 0.97, 1.0, 0.98]
}

fn cursor_outline_color() -> [f32; 4] {
    [0.02, 0.025, 0.03, 0.95]
}

fn cursor_shadow_color() -> [f32; 4] {
    [0.0, 0.0, 0.0, 0.35]
}

fn pixel_x_to_ndc(x: f32, width: f32) -> f32 {
    (x / width) * 2.0 - 1.0
}

fn pixel_y_to_ndc(y: f32, height: f32) -> f32 {
    1.0 - (y / height) * 2.0
}

fn vertex(x: f32, y: f32, u: f32, v: f32) -> caplibgl::Vertex {
    caplibgl::Vertex {
        x,
        y,
        z: 0.0,
        w: 1.0,
        r: 1.0,
        g: 1.0,
        b: 1.0,
        a: 1.0,
        u,
        v,
        s: 0.0,
        t: 1.0,
    }
}

fn colored_vertex(x: f32, y: f32, color: [f32; 4]) -> caplibgl::Vertex {
    caplibgl::Vertex {
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
