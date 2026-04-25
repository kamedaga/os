use alloc::string::String;
use alloc::vec::Vec;

const OUTER_GAP: i32 = 28;
const INNER_GAP: i32 = 10;
const MASTER_RATIO_PERCENT: i32 = 58;

pub const PUBLIC_NAME: &str = "Pachaland";
pub const INTERNAL_SERVICE_NAME: &str = "capwm";
pub const PROTOCOL_VERSION: u32 = 1;

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct WindowId(u32);

impl WindowId {
    #[allow(dead_code)]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        if raw == 0 { None } else { Some(Self(raw)) }
    }

    pub const fn raw(self) -> u32 {
        self.0
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct SurfaceId(u32);

impl SurfaceId {
    pub const fn from_raw(raw: u32) -> Option<Self> {
        if raw == 0 { None } else { Some(Self(raw)) }
    }

    pub const fn raw(self) -> u32 {
        self.0
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Size {
    pub width: u32,
    pub height: u32,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Position {
    pub x: i32,
    pub y: i32,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct AppSurface {
    pub resource_id: u32,
    pub gpu_surface_id: u32,
    pub size: Size,
}

impl AppSurface {
    pub const fn packed_handles(self) -> u64 {
        self.resource_id as u64 | ((self.gpu_surface_id as u64) << 32)
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct CompositedWindow {
    pub id: WindowId,
    pub position: Position,
    pub size: Size,
    pub app_surface: AppSurface,
    pub focused: bool,
    pub presented_frames: u64,
}

#[derive(Clone, Debug)]
#[allow(dead_code)]
struct Window {
    id: WindowId,
    title: String,
    size: Size,
    position: Position,
    surface: Option<SurfaceId>,
    app_surface: Option<AppSurface>,
    presented_frames: u64,
}

#[derive(Debug)]
pub struct WindowRegistry {
    next_window_id: u32,
    next_surface_id: u32,
    windows: Vec<Window>,
}

impl WindowRegistry {
    pub const fn new() -> Self {
        Self {
            next_window_id: 1,
            next_surface_id: 1,
            windows: Vec::new(),
        }
    }

    pub fn len(&self) -> usize {
        self.windows.len()
    }

    pub fn create_window(
        &mut self,
        title: &str,
        width: u32,
        height: u32,
    ) -> cap_std::Result<WindowId> {
        if width == 0 || height == 0 {
            return Err(cap_std::Error::new(cap_std::ErrorKind::InvalidInput));
        }
        let id = WindowId(self.next_window_id);
        self.next_window_id = self.next_window_id.saturating_add(1);
        let position = default_window_position(id);
        self.windows.push(Window {
            id,
            title: String::from(title),
            size: Size { width, height },
            position,
            surface: None,
            app_surface: None,
            presented_frames: 0,
        });
        Ok(id)
    }

    pub fn attach_surface(&mut self, id: WindowId) -> cap_std::Result<SurfaceId> {
        let surface = SurfaceId(self.next_surface_id);
        self.next_surface_id = self.next_surface_id.saturating_add(1);
        let Some(window) = self.windows.iter_mut().find(|window| window.id == id) else {
            return Err(cap_std::Error::new(cap_std::ErrorKind::NotFound));
        };
        window.surface = Some(surface);
        Ok(surface)
    }

    pub fn attach_app_surface(
        &mut self,
        id: WindowId,
        app_surface: AppSurface,
    ) -> cap_std::Result<()> {
        let Some(window) = self.windows.iter_mut().find(|window| window.id == id) else {
            return Err(cap_std::Error::new(cap_std::ErrorKind::NotFound));
        };
        window.app_surface = Some(app_surface);
        Ok(())
    }

    pub fn destroy_window(&mut self, id: WindowId) -> bool {
        let Some(index) = self.windows.iter().position(|window| window.id == id) else {
            return false;
        };
        self.windows.remove(index);
        true
    }

    pub fn present(&mut self, id: WindowId, surface: SurfaceId) -> bool {
        let Some(window) = self.windows.iter_mut().find(|window| window.id == id) else {
            return false;
        };
        if window.surface != Some(surface) {
            return false;
        }
        window.presented_frames = window.presented_frames.saturating_add(1);
        let Some(index) = self.windows.iter().position(|window| window.id == id) else {
            return false;
        };
        if index + 1 != self.windows.len() {
            let window = self.windows.remove(index);
            self.windows.push(window);
        }
        true
    }

    pub fn set_geometry(
        &mut self,
        id: WindowId,
        surface: SurfaceId,
        position: Position,
        size: Size,
    ) -> bool {
        if size.width == 0 || size.height == 0 {
            return false;
        }
        let Some(window) = self.windows.iter_mut().find(|window| window.id == id) else {
            return false;
        };
        if window.surface != Some(surface) {
            return false;
        }
        window.position = position;
        window.size = size;
        true
    }

    #[allow(dead_code)]
    pub fn surface(&self, id: WindowId) -> Option<SurfaceId> {
        self.windows
            .iter()
            .find(|window| window.id == id)
            .and_then(|window| window.surface)
    }

    pub fn app_surface(&self, id: WindowId) -> Option<AppSurface> {
        self.windows
            .iter()
            .find(|window| window.id == id)
            .and_then(|window| window.app_surface)
    }

    pub fn composited_windows(&self) -> Vec<CompositedWindow> {
        let focused_id = self
            .windows
            .iter()
            .rev()
            .find_map(|window| window.app_surface.map(|_| window.id));
        self.windows
            .iter()
            .filter_map(|window| {
                window.app_surface.map(|app_surface| CompositedWindow {
                    id: window.id,
                    position: window.position,
                    size: window.size,
                    app_surface,
                    focused: focused_id == Some(window.id),
                    presented_frames: window.presented_frames,
                })
            })
            .collect()
    }

    pub fn has_loading_windows(&self) -> bool {
        self.windows
            .iter()
            .any(|window| window.app_surface.is_some() && window.presented_frames == 0)
    }

    pub fn apply_tiling_layout(&mut self, viewport: Size) {
        let visible_indexes: Vec<usize> = self
            .windows
            .iter()
            .enumerate()
            .filter_map(|(index, window)| window.app_surface.map(|_| index))
            .collect();
        let count = visible_indexes.len();
        if count == 0 {
            return;
        }
        let area = TileRect::from_viewport(viewport);
        if count == 1 {
            set_window_rect(&mut self.windows[visible_indexes[0]], area);
            return;
        }

        let master_index = visible_indexes[count - 1];
        let stack_count = count - 1;
        let master_width = ((area.width * MASTER_RATIO_PERCENT) / 100).max(1);
        let stack_width = (area.width - master_width - INNER_GAP).max(1);
        let master_rect = TileRect {
            x: area.x,
            y: area.y,
            width: master_width,
            height: area.height,
        };
        set_window_rect(&mut self.windows[master_index], master_rect);

        let stack_x = area.x + master_width + INNER_GAP;
        let available_stack_height = (area.height - INNER_GAP * (stack_count as i32 - 1)).max(1);
        let base_height = (available_stack_height / stack_count as i32).max(1);
        let mut y = area.y;
        for (stack_slot, window_index) in visible_indexes[..stack_count].iter().enumerate() {
            let remaining_slots = stack_count - stack_slot;
            let used_tail_gap = INNER_GAP * (remaining_slots.saturating_sub(1) as i32);
            let height = if remaining_slots == 1 {
                (area.y + area.height - y).max(1)
            } else {
                base_height.min((area.y + area.height - y - used_tail_gap).max(1))
            };
            set_window_rect(
                &mut self.windows[*window_index],
                TileRect {
                    x: stack_x,
                    y,
                    width: stack_width,
                    height,
                },
            );
            y += height + INNER_GAP;
        }
    }

    #[allow(dead_code)]
    pub fn title(&self, id: WindowId) -> Option<&str> {
        self.windows
            .iter()
            .find(|window| window.id == id)
            .map(|window| window.title.as_str())
    }

    #[allow(dead_code)]
    pub fn size(&self, id: WindowId) -> Option<Size> {
        self.windows
            .iter()
            .find(|window| window.id == id)
            .map(|window| window.size)
    }

    pub fn geometry(&self, id: WindowId) -> Option<(Position, Size)> {
        self.windows
            .iter()
            .find(|window| window.id == id)
            .map(|window| (window.position, window.size))
    }
}

#[derive(Copy, Clone)]
struct TileRect {
    x: i32,
    y: i32,
    width: i32,
    height: i32,
}

impl TileRect {
    fn from_viewport(viewport: Size) -> Self {
        let width = viewport.width as i32;
        let height = viewport.height as i32;
        let gap_x = OUTER_GAP.min((width / 4).max(0));
        let gap_y = OUTER_GAP.min((height / 4).max(0));
        Self {
            x: gap_x,
            y: gap_y,
            width: (width - gap_x * 2).max(1),
            height: (height - gap_y * 2).max(1),
        }
    }
}

fn set_window_rect(window: &mut Window, rect: TileRect) {
    window.position = Position {
        x: rect.x,
        y: rect.y,
    };
    window.size = Size {
        width: rect.width.max(1) as u32,
        height: rect.height.max(1) as u32,
    };
}

fn default_window_position(id: WindowId) -> Position {
    let offset = ((id.raw().saturating_sub(1) % 6) * 42) as i32;
    Position {
        x: 48 + offset,
        y: 48 + offset,
    }
}
