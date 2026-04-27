use alloc::string::String;
use alloc::vec::Vec;

const OUTER_GAP: i32 = 28;
const INNER_GAP: i32 = 10;

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

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct ConfigureChange {
    pub id: WindowId,
    pub surface: SurfaceId,
    pub size: Size,
    pub serial: u64,
    pub owner_session: u32,
}

#[derive(Clone, Debug)]
#[allow(dead_code)]
struct Window {
    id: WindowId,
    owner_session: u32,
    title: String,
    size: Size,
    position: Position,
    surface: Option<SurfaceId>,
    app_surface: Option<AppSurface>,
    presented_frames: u64,
    configure_serial: u64,
    acked_configure_serial: u64,
}

#[derive(Debug)]
pub struct WindowRegistry {
    next_window_id: u32,
    next_surface_id: u32,
    windows: Vec<Window>,
    tiled_visible_ids: Vec<WindowId>,
}

impl WindowRegistry {
    pub const fn new() -> Self {
        Self {
            next_window_id: 1,
            next_surface_id: 1,
            windows: Vec::new(),
            tiled_visible_ids: Vec::new(),
        }
    }

    pub fn len(&self) -> usize {
        self.windows.len()
    }

    pub fn create_window(
        &mut self,
        owner_session: u32,
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
            owner_session,
            title: String::from(title),
            size: Size { width, height },
            position,
            surface: None,
            app_surface: None,
            presented_frames: 0,
            configure_serial: 1,
            acked_configure_serial: 0,
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

    pub fn configure_serial(&self, id: WindowId) -> Option<u64> {
        self.windows
            .iter()
            .find(|window| window.id == id)
            .map(|window| window.configure_serial)
    }

    pub fn ack_configure(&mut self, id: WindowId, serial: u64) -> bool {
        let Some(window) = self.windows.iter_mut().find(|window| window.id == id) else {
            return false;
        };
        if serial == 0 || serial > window.configure_serial {
            return false;
        }
        window.acked_configure_serial = window.acked_configure_serial.max(serial);
        true
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

    pub fn apply_tiling_layout(
        &mut self,
        viewport: Size,
        pointer_position: Option<Position>,
    ) -> Vec<ConfigureChange> {
        let mut changes = Vec::new();
        let visible_indexes: Vec<usize> = self
            .windows
            .iter()
            .enumerate()
            .filter_map(|(index, window)| window.app_surface.map(|_| index))
            .collect();
        let visible_ids: Vec<WindowId> = visible_indexes
            .iter()
            .map(|index| self.windows[*index].id)
            .collect();
        let count = visible_indexes.len();
        if count == 0 {
            self.tiled_visible_ids.clear();
            return changes;
        }
        let area = TileRect::from_viewport(viewport);
        if count == 1 {
            maybe_set_window_rect(&mut self.windows[visible_indexes[0]], area, &mut changes);
            self.tiled_visible_ids = visible_ids;
            return changes;
        }

        let new_window_id = visible_ids
            .iter()
            .copied()
            .find(|id| !contains_window_id(&self.tiled_visible_ids, *id));
        if let Some(new_window_id) = new_window_id {
            if self.tiled_visible_ids.is_empty() {
                maybe_set_window_rect(&mut self.windows[visible_indexes[0]], area, &mut changes);
            }
            let Some(new_index) = self.window_index(new_window_id) else {
                self.tiled_visible_ids = visible_ids;
                return changes;
            };
            let target_index = self
                .window_index_at(pointer_position)
                .filter(|index| *index != new_index)
                .or_else(|| self.window_index_nearest(pointer_position, new_index))
                .unwrap_or_else(|| {
                    visible_indexes
                        .iter()
                        .copied()
                        .rev()
                        .find(|index| *index != new_index)
                        .unwrap_or(visible_indexes[0])
                });
            let target_rect = TileRect::from_window(&self.windows[target_index]);
            let split_position =
                pointer_position.map(|position| target_rect.clamp_position(position));
            let (target_next, new_next) = target_rect.split_for_new_window(split_position);
            maybe_set_window_rect(&mut self.windows[target_index], target_next, &mut changes);
            maybe_set_window_rect(&mut self.windows[new_index], new_next, &mut changes);
        } else if !same_window_id_set(&self.tiled_visible_ids, &visible_ids) {
            if count == 1 {
                maybe_set_window_rect(&mut self.windows[visible_indexes[0]], area, &mut changes);
            }
        }
        self.tiled_visible_ids = visible_ids;
        changes
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

    fn window_index(&self, id: WindowId) -> Option<usize> {
        self.windows.iter().position(|window| window.id == id)
    }

    fn window_index_at(&self, position: Option<Position>) -> Option<usize> {
        let position = position?;
        self.windows
            .iter()
            .enumerate()
            .rev()
            .find(|(_, window)| {
                window.app_surface.is_some() && TileRect::from_window(window).contains(position)
            })
            .map(|(index, _)| index)
    }

    fn window_index_nearest(
        &self,
        position: Option<Position>,
        exclude_index: usize,
    ) -> Option<usize> {
        let position = position?;
        self.windows
            .iter()
            .enumerate()
            .rev()
            .filter(|(index, window)| *index != exclude_index && window.app_surface.is_some())
            .min_by_key(|(_, window)| TileRect::from_window(window).distance_score(position))
            .map(|(index, _)| index)
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

    fn from_window(window: &Window) -> Self {
        Self {
            x: window.position.x,
            y: window.position.y,
            width: window.size.width.max(1) as i32,
            height: window.size.height.max(1) as i32,
        }
    }

    fn contains(self, position: Position) -> bool {
        position.x >= self.x
            && position.y >= self.y
            && position.x < self.x + self.width
            && position.y < self.y + self.height
    }

    fn clamp_position(self, position: Position) -> Position {
        Position {
            x: position.x.clamp(self.x, self.x + self.width - 1),
            y: position.y.clamp(self.y, self.y + self.height - 1),
        }
    }

    fn distance_score(self, position: Position) -> i64 {
        let clamped = self.clamp_position(position);
        let dx = position.x as i64 - clamped.x as i64;
        let dy = position.y as i64 - clamped.y as i64;
        dx * dx + dy * dy
    }

    fn split_for_new_window(self, pointer_position: Option<Position>) -> (Self, Self) {
        if self.width >= self.height {
            self.split_vertical(pointer_position)
        } else {
            self.split_horizontal(pointer_position)
        }
    }

    fn split_vertical(self, pointer_position: Option<Position>) -> (Self, Self) {
        let gap = INNER_GAP.min((self.width - 2).max(0));
        let available = (self.width - gap).max(2);
        let first_width = (available / 2).max(1);
        let second_width = (available - first_width).max(1);
        let left = Self {
            width: first_width,
            ..self
        };
        let right = Self {
            x: self.x + first_width + gap,
            width: second_width,
            ..self
        };
        if pointer_position
            .map(|position| position.x < self.x + self.width / 2)
            .unwrap_or(false)
        {
            (right, left)
        } else {
            (left, right)
        }
    }

    fn split_horizontal(self, pointer_position: Option<Position>) -> (Self, Self) {
        let gap = INNER_GAP.min((self.height - 2).max(0));
        let available = (self.height - gap).max(2);
        let first_height = (available / 2).max(1);
        let second_height = (available - first_height).max(1);
        let top = Self {
            height: first_height,
            ..self
        };
        let bottom = Self {
            y: self.y + first_height + gap,
            height: second_height,
            ..self
        };
        if pointer_position
            .map(|position| position.y < self.y + self.height / 2)
            .unwrap_or(false)
        {
            (bottom, top)
        } else {
            (top, bottom)
        }
    }
}

fn contains_window_id(ids: &[WindowId], id: WindowId) -> bool {
    ids.iter().any(|candidate| *candidate == id)
}

fn same_window_id_set(a: &[WindowId], b: &[WindowId]) -> bool {
    a.len() == b.len() && a.iter().all(|id| contains_window_id(b, *id))
}

fn maybe_set_window_rect(window: &mut Window, rect: TileRect, changes: &mut Vec<ConfigureChange>) {
    let new_size = Size {
        width: rect.width.max(1) as u32,
        height: rect.height.max(1) as u32,
    };
    let size_changed = window.size != new_size;
    window.position = Position {
        x: rect.x,
        y: rect.y,
    };
    window.size = new_size;
    if !size_changed {
        return;
    }
    window.configure_serial = window.configure_serial.saturating_add(1).max(1);
    if let Some(surface) = window.surface {
        changes.push(ConfigureChange {
            id: window.id,
            surface,
            size: window.size,
            serial: window.configure_serial,
            owner_session: window.owner_session,
        });
    }
}

fn default_window_position(id: WindowId) -> Position {
    let offset = ((id.raw().saturating_sub(1) % 6) * 42) as i32;
    Position {
        x: 48 + offset,
        y: 48 + offset,
    }
}
