use capwm_client::protocol;

use crate::capwm::{Position, Size, SurfaceId, WindowId, WindowRegistry};
use crate::compositor::GpuCompositor;
use crate::pointer;

pub const SUPPORTED_REQUESTS: [protocol::Opcode; 6] = [
    protocol::Opcode::Hello,
    protocol::Opcode::CreateWindow,
    protocol::Opcode::DestroyWindow,
    protocol::Opcode::Present,
    protocol::Opcode::PollEvent,
    protocol::Opcode::SetGeometry,
];

pub struct Server {
    registry: WindowRegistry,
    compositor: GpuCompositor,
    last_pointer_seq: u64,
    last_pointer_buttons: u64,
}

impl Server {
    pub fn new() -> Self {
        Self {
            registry: WindowRegistry::new(),
            compositor: GpuCompositor::connect(),
            last_pointer_seq: 0,
            last_pointer_buttons: 0,
        }
    }

    pub fn handle_request(
        &mut self,
        request: &protocol::RequestHeader,
        payload: &[u8],
    ) -> protocol::ResponseHeader {
        if request.magic != protocol::REQUEST_MAGIC || request.version != protocol::VERSION {
            return response_for(
                request,
                protocol::Opcode::Hello,
                protocol::Status::Invalid,
                0,
                0,
                0,
                0,
                0,
            );
        }
        let Some(opcode) = protocol::Opcode::from_raw(request.op) else {
            return response_for(
                request,
                protocol::Opcode::Hello,
                protocol::Status::Invalid,
                0,
                0,
                0,
                0,
                0,
            );
        };
        match opcode {
            protocol::Opcode::Hello => response_for(
                request,
                opcode,
                protocol::Status::Ok,
                0,
                0,
                protocol::VERSION as u64,
                0,
                0,
            ),
            protocol::Opcode::CreateWindow => self.handle_create_window(request, payload),
            protocol::Opcode::DestroyWindow => self.handle_destroy_window(request, payload),
            protocol::Opcode::Present => self.handle_present(request, payload),
            protocol::Opcode::SetGeometry => self.handle_set_geometry(request, payload),
            protocol::Opcode::PollEvent => response_for(
                request,
                opcode,
                protocol::Status::Unsupported,
                0,
                0,
                0,
                0,
                0,
            ),
        }
    }

    pub fn window_count(&self) -> usize {
        self.registry.len()
    }

    pub fn gpu_status_label(&self) -> &'static str {
        self.compositor.status_label()
    }

    pub fn gpu_features(&self) -> u64 {
        self.compositor.features()
    }

    pub fn gpu_capset_id(&self) -> u32 {
        self.compositor.capset_id()
    }

    pub fn gpu_capset_max_version(&self) -> u32 {
        self.compositor.capset_max_version()
    }

    pub fn target_size(&mut self) -> Option<Size> {
        self.compositor.target_size()
    }

    pub fn pump_pointer(&mut self) {
        let Some(pointer) = pointer::read() else {
            return;
        };
        if pointer.seq == self.last_pointer_seq && pointer.buttons == self.last_pointer_buttons {
            return;
        }
        self.last_pointer_seq = pointer.seq;
        self.last_pointer_buttons = pointer.buttons;
        let position = self.scale_pointer_position(pointer.position, pointer.size);
        self.compositor.set_cursor_position(position);
        if !self.compositor.move_hardware_cursor(position) {
            self.compose_tiled();
        }
    }

    pub fn present_desktop(&mut self) {
        self.compose_tiled();
    }

    pub fn has_loading_windows(&self) -> bool {
        self.registry.has_loading_windows()
    }

    #[allow(dead_code)]
    pub fn surface(&self, id: WindowId) -> Option<SurfaceId> {
        self.registry.surface(id)
    }

    #[allow(dead_code)]
    pub fn title(&self, id: WindowId) -> Option<&str> {
        self.registry.title(id)
    }

    #[allow(dead_code)]
    pub fn size(&self, id: WindowId) -> Option<crate::capwm::Size> {
        self.registry.size(id)
    }

    fn handle_create_window(
        &mut self,
        request: &protocol::RequestHeader,
        payload: &[u8],
    ) -> protocol::ResponseHeader {
        if payload.len() != request.inline_bytes as usize {
            return response_for(
                request,
                protocol::Opcode::CreateWindow,
                protocol::Status::Invalid,
                0,
                0,
                0,
                0,
                0,
            );
        }
        let requested_size = protocol::unpack_size(request.arg0);
        let size = Size {
            width: requested_size.width,
            height: requested_size.height,
        };
        let title = core::str::from_utf8(payload).unwrap_or("app");
        match self.registry.create_window(title, size.width, size.height) {
            Ok(window) => {
                let surface = self.registry.attach_surface(window).ok();
                let app_surface =
                    self.compositor
                        .create_app_surface(size)
                        .and_then(|app_surface| {
                            self.registry
                                .attach_app_surface(window, app_surface)
                                .ok()
                                .map(|_| app_surface)
                        });
                if app_surface.is_some() {
                    self.compose_tiled();
                }
                response_for(
                    request,
                    protocol::Opcode::CreateWindow,
                    protocol::Status::Ok,
                    window.raw(),
                    surface.map(|surface| surface.raw()).unwrap_or(0),
                    protocol::pack_size(size.width, size.height),
                    app_surface
                        .map(|_| protocol::RESPONSE_FLAG_APP_SURFACE)
                        .unwrap_or(0),
                    app_surface
                        .map(|app_surface| app_surface.packed_handles())
                        .unwrap_or(0),
                )
            }
            Err(_) => response_for(
                request,
                protocol::Opcode::CreateWindow,
                protocol::Status::Invalid,
                0,
                0,
                0,
                0,
                0,
            ),
        }
    }

    fn handle_destroy_window(
        &mut self,
        request: &protocol::RequestHeader,
        payload: &[u8],
    ) -> protocol::ResponseHeader {
        if !payload.is_empty() || request.inline_bytes != 0 {
            return response_for(
                request,
                protocol::Opcode::DestroyWindow,
                protocol::Status::Invalid,
                0,
                0,
                0,
                0,
                0,
            );
        }
        let Some(window) = WindowId::from_raw(request.window_id) else {
            return response_for(
                request,
                protocol::Opcode::DestroyWindow,
                protocol::Status::Invalid,
                0,
                0,
                0,
                0,
                0,
            );
        };
        let app_surface = self.registry.app_surface(window);
        if !self.registry.destroy_window(window) {
            return response_for(
                request,
                protocol::Opcode::DestroyWindow,
                protocol::Status::NotFound,
                0,
                0,
                0,
                0,
                0,
            );
        }
        if let Some(app_surface) = app_surface {
            self.compositor.destroy_app_surface(app_surface);
        }
        response_for(
            request,
            protocol::Opcode::DestroyWindow,
            protocol::Status::Ok,
            request.window_id,
            0,
            0,
            0,
            0,
        )
    }

    fn handle_present(
        &mut self,
        request: &protocol::RequestHeader,
        payload: &[u8],
    ) -> protocol::ResponseHeader {
        if !payload.is_empty() || request.inline_bytes != 0 {
            return response_for(
                request,
                protocol::Opcode::Present,
                protocol::Status::Invalid,
                0,
                0,
                0,
                0,
                0,
            );
        }
        let Some(window) = WindowId::from_raw(request.window_id) else {
            return response_for(
                request,
                protocol::Opcode::Present,
                protocol::Status::Invalid,
                0,
                0,
                0,
                0,
                0,
            );
        };
        let Some(surface) = SurfaceId::from_raw(request.surface_id) else {
            return response_for(
                request,
                protocol::Opcode::Present,
                protocol::Status::Invalid,
                0,
                0,
                0,
                0,
                0,
            );
        };
        if !self.registry.present(window, surface) {
            return response_for(
                request,
                protocol::Opcode::Present,
                protocol::Status::NotFound,
                0,
                0,
                0,
                0,
                0,
            );
        }
        self.compose_tiled();
        response_for(
            request,
            protocol::Opcode::Present,
            protocol::Status::Ok,
            request.window_id,
            request.surface_id,
            0,
            0,
            0,
        )
    }

    fn handle_set_geometry(
        &mut self,
        request: &protocol::RequestHeader,
        payload: &[u8],
    ) -> protocol::ResponseHeader {
        if !payload.is_empty() || request.inline_bytes != 0 {
            return response_for(
                request,
                protocol::Opcode::SetGeometry,
                protocol::Status::Invalid,
                0,
                0,
                0,
                0,
                0,
            );
        }
        let Some(window) = WindowId::from_raw(request.window_id) else {
            return response_for(
                request,
                protocol::Opcode::SetGeometry,
                protocol::Status::Invalid,
                0,
                0,
                0,
                0,
                0,
            );
        };
        let Some(surface) = SurfaceId::from_raw(request.surface_id) else {
            return response_for(
                request,
                protocol::Opcode::SetGeometry,
                protocol::Status::Invalid,
                0,
                0,
                0,
                0,
                0,
            );
        };
        let position = protocol::unpack_position(request.arg0);
        let size = protocol::unpack_size(request.arg1);
        let position = Position {
            x: position.x,
            y: position.y,
        };
        let size = Size {
            width: size.width,
            height: size.height,
        };
        if !self.registry.set_geometry(window, surface, position, size) {
            return response_for(
                request,
                protocol::Opcode::SetGeometry,
                protocol::Status::NotFound,
                0,
                0,
                0,
                0,
                0,
            );
        }
        self.compose_tiled();
        let (actual_position, actual_size) =
            self.registry.geometry(window).unwrap_or((position, size));
        response_for(
            request,
            protocol::Opcode::SetGeometry,
            protocol::Status::Ok,
            request.window_id,
            request.surface_id,
            protocol::pack_size(actual_size.width, actual_size.height),
            0,
            protocol::pack_position(actual_position.x, actual_position.y),
        )
    }

    fn compose_tiled(&mut self) {
        if let Some(target_size) = self.compositor.target_size() {
            self.registry.apply_tiling_layout(target_size);
        }
        let windows = self.registry.composited_windows();
        let _ = self.compositor.compose_windows(&windows);
    }

    fn scale_pointer_position(&mut self, position: Position, pointer_size: Size) -> Position {
        let Some(target_size) = self.compositor.target_size() else {
            return position;
        };
        if pointer_size.width == 0 || pointer_size.height == 0 {
            return position;
        }
        Position {
            x: scale_axis(position.x, pointer_size.width, target_size.width),
            y: scale_axis(position.y, pointer_size.height, target_size.height),
        }
    }
}

fn scale_axis(value: i32, source: u32, target: u32) -> i32 {
    if source == 0 || source == target {
        return value;
    }
    let scaled = (value as i64) * (target as i64) / (source as i64);
    if scaled > i32::MAX as i64 {
        i32::MAX
    } else if scaled < i32::MIN as i64 {
        i32::MIN
    } else {
        scaled as i32
    }
}

fn response_for(
    request: &protocol::RequestHeader,
    opcode: protocol::Opcode,
    status: protocol::Status,
    window_id: u32,
    surface_id: u32,
    arg0: u64,
    result_flags: u32,
    arg1: u64,
) -> protocol::ResponseHeader {
    protocol::ResponseHeader {
        magic: protocol::RESPONSE_MAGIC,
        version: protocol::VERSION,
        op: opcode as u16,
        response_seq: request.request_seq,
        status: status as i32,
        result_flags,
        window_id,
        surface_id,
        arg0,
        arg1,
        inline_bytes: 0,
        reserved0: 0,
    }
}
