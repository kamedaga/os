use alloc::vec::Vec;

use cap_window::protocol;

use crate::capwm::{ConfigureChange, Position, Size, SurfaceId, WindowId, WindowRegistry};
use crate::compositor::{GpuCompositor, content_size_for_window_frame, frame_size_for_content};
use crate::pointer;

pub const SUPPORTED_REQUESTS: [protocol::Opcode; 8] = [
    protocol::Opcode::Hello,
    protocol::Opcode::QueryOutput,
    protocol::Opcode::CreateWindow,
    protocol::Opcode::DestroyWindow,
    protocol::Opcode::Present,
    protocol::Opcode::PollEvent,
    protocol::Opcode::AckConfigure,
    protocol::Opcode::QueryWindow,
];

pub struct Server {
    registry: WindowRegistry,
    compositor: GpuCompositor,
    last_pointer_seq: u64,
    last_pointer_buttons: u64,
    current_pointer_position: Option<Position>,
    pending_events: Vec<PendingEvent>,
}

struct PendingEvent {
    owner_session: u32,
    event: protocol::Event,
}

impl Server {
    pub fn new() -> Self {
        Self {
            registry: WindowRegistry::new(),
            compositor: GpuCompositor::connect(),
            last_pointer_seq: 0,
            last_pointer_buttons: 0,
            current_pointer_position: None,
            pending_events: Vec::new(),
        }
    }

    pub fn handle_request(
        &mut self,
        session_id: u32,
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
                0,
            ),
            protocol::Opcode::QueryOutput => self.handle_query_output(request, payload),
            protocol::Opcode::CreateWindow => {
                self.handle_create_window(session_id, request, payload)
            }
            protocol::Opcode::DestroyWindow => self.handle_destroy_window(request, payload),
            protocol::Opcode::Present => self.handle_present(request, payload),
            protocol::Opcode::PollEvent => self.handle_poll_event(session_id, request, payload),
            protocol::Opcode::AckConfigure => self.handle_ack_configure(request, payload),
            protocol::Opcode::QueryWindow => self.handle_query_window(request, payload),
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
        self.current_pointer_position = Some(position);
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

    fn refresh_pointer_position(&mut self) -> Option<Position> {
        let pointer = pointer::read()?;
        let position = self.scale_pointer_position(pointer.position, pointer.size);
        self.last_pointer_seq = pointer.seq;
        self.last_pointer_buttons = pointer.buttons;
        self.current_pointer_position = Some(position);
        Some(position)
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
        session_id: u32,
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
                0,
            );
        }
        let requested_size = protocol::unpack_size(request.arg0);
        let requested_content_size = Size {
            width: requested_size.width,
            height: requested_size.height,
        };
        let requested_frame_size = frame_size_for_content(Size {
            width: requested_content_size.width,
            height: requested_content_size.height,
        });
        let size = Size {
            width: requested_frame_size.width,
            height: requested_frame_size.height,
        };
        let title = core::str::from_utf8(payload).unwrap_or("app");
        match self
            .registry
            .create_window(session_id, title, size.width, size.height)
        {
            Ok(window) => {
                let surface = self.registry.attach_surface(window).ok();
                let app_surface = self
                    .compositor
                    .create_app_surface(requested_content_size)
                    .and_then(|app_surface| {
                        self.registry
                            .attach_app_surface(window, app_surface)
                            .ok()
                            .map(|_| app_surface)
                    });
                if app_surface.is_some() {
                    self.refresh_pointer_position();
                    self.compose_tiled_skip_event_for(Some(window));
                }
                let content_size = self
                    .registry
                    .geometry(window)
                    .map(|(_, frame_size)| content_size_for_window_frame(frame_size))
                    .unwrap_or(requested_content_size);
                let configure_serial = self.registry.configure_serial(window).unwrap_or(1);
                response_for(
                    request,
                    protocol::Opcode::CreateWindow,
                    protocol::Status::Ok,
                    window.raw(),
                    surface.map(|surface| surface.raw()).unwrap_or(0),
                    protocol::pack_size(content_size.width, content_size.height),
                    app_surface
                        .map(|_| protocol::RESPONSE_FLAG_GPU_SURFACE)
                        .unwrap_or(0),
                    app_surface
                        .map(|app_surface| app_surface.packed_handles())
                        .unwrap_or(0),
                    configure_serial,
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
                0,
            );
        }
        if let Some(app_surface) = app_surface {
            self.compositor.destroy_app_surface(app_surface);
        }
        self.compose_tiled();
        response_for(
            request,
            protocol::Opcode::DestroyWindow,
            protocol::Status::Ok,
            request.window_id,
            0,
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
            0,
        )
    }

    fn handle_query_output(
        &mut self,
        request: &protocol::RequestHeader,
        payload: &[u8],
    ) -> protocol::ResponseHeader {
        if !payload.is_empty() || request.inline_bytes != 0 {
            return response_for(
                request,
                protocol::Opcode::QueryOutput,
                protocol::Status::Invalid,
                0,
                0,
                0,
                0,
                0,
                0,
            );
        }
        let output_size = self.compositor.target_size().unwrap_or(Size {
            width: 0,
            height: 0,
        });
        let recommended = recommended_content_size(output_size);
        response_for(
            request,
            protocol::Opcode::QueryOutput,
            protocol::Status::Ok,
            0,
            0,
            protocol::pack_size(output_size.width, output_size.height),
            0,
            protocol::pack_size(recommended.width, recommended.height),
            0,
        )
    }

    fn handle_query_window(
        &mut self,
        request: &protocol::RequestHeader,
        payload: &[u8],
    ) -> protocol::ResponseHeader {
        if !payload.is_empty() || request.inline_bytes != 0 {
            return response_for(
                request,
                protocol::Opcode::QueryWindow,
                protocol::Status::Invalid,
                0,
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
                protocol::Opcode::QueryWindow,
                protocol::Status::Invalid,
                0,
                0,
                0,
                0,
                0,
                0,
            );
        };
        let Some(surface) = self.registry.surface(window) else {
            return response_for(
                request,
                protocol::Opcode::QueryWindow,
                protocol::Status::NotFound,
                0,
                0,
                0,
                0,
                0,
                0,
            );
        };
        let Some((_, frame_size)) = self.registry.geometry(window) else {
            return response_for(
                request,
                protocol::Opcode::QueryWindow,
                protocol::Status::NotFound,
                0,
                0,
                0,
                0,
                0,
                0,
            );
        };
        let content_size = content_size_for_window_frame(frame_size);
        response_for(
            request,
            protocol::Opcode::QueryWindow,
            protocol::Status::Ok,
            request.window_id,
            surface.raw(),
            protocol::pack_size(content_size.width, content_size.height),
            0,
            self.registry.configure_serial(window).unwrap_or(0),
            0,
        )
    }

    fn handle_ack_configure(
        &mut self,
        request: &protocol::RequestHeader,
        payload: &[u8],
    ) -> protocol::ResponseHeader {
        if !payload.is_empty() || request.inline_bytes != 0 {
            return response_for(
                request,
                protocol::Opcode::AckConfigure,
                protocol::Status::Invalid,
                0,
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
                protocol::Opcode::AckConfigure,
                protocol::Status::Invalid,
                0,
                0,
                0,
                0,
                0,
                0,
            );
        };
        if !self.registry.ack_configure(window, request.arg0) {
            return response_for(
                request,
                protocol::Opcode::AckConfigure,
                protocol::Status::NotFound,
                0,
                0,
                0,
                0,
                0,
                0,
            );
        }
        response_for(
            request,
            protocol::Opcode::AckConfigure,
            protocol::Status::Ok,
            request.window_id,
            0,
            request.arg0,
            0,
            0,
            0,
        )
    }

    fn handle_poll_event(
        &mut self,
        session_id: u32,
        request: &protocol::RequestHeader,
        payload: &[u8],
    ) -> protocol::ResponseHeader {
        if !payload.is_empty() || request.inline_bytes != 0 {
            return response_for(
                request,
                protocol::Opcode::PollEvent,
                protocol::Status::Invalid,
                0,
                0,
                0,
                0,
                0,
                0,
            );
        }
        let event = if self.pending_events.is_empty() {
            None
        } else {
            let Some(index) = self
                .pending_events
                .iter()
                .position(|event| event.owner_session == session_id)
            else {
                return response_for(
                    request,
                    protocol::Opcode::PollEvent,
                    protocol::Status::Ok,
                    0,
                    0,
                    0,
                    protocol::pack_event_result_flags(protocol::EventKind::None, 0),
                    0,
                    0,
                );
            };
            Some(self.pending_events.remove(index).event)
        };
        let Some(event) = event else {
            return response_for(
                request,
                protocol::Opcode::PollEvent,
                protocol::Status::Ok,
                0,
                0,
                0,
                protocol::pack_event_result_flags(protocol::EventKind::None, 0),
                0,
                0,
            );
        };
        response_for(
            request,
            protocol::Opcode::PollEvent,
            protocol::Status::Ok,
            event.window_id,
            event.surface_id,
            event.arg0,
            protocol::pack_event_result_flags(event.kind, event.flags),
            event.arg1,
            0,
        )
    }

    fn compose_tiled(&mut self) {
        self.compose_tiled_skip_event_for(None);
    }

    fn compose_tiled_skip_event_for(&mut self, skip_window: Option<WindowId>) {
        if let Some(target_size) = self.compositor.target_size() {
            let pointer_position = self
                .refresh_pointer_position()
                .or(self.current_pointer_position)
                .or_else(|| {
                    Some(Position {
                        x: (target_size.width / 2) as i32,
                        y: (target_size.height / 2) as i32,
                    })
                });
            let changes = self
                .registry
                .apply_tiling_layout(target_size, pointer_position);
            self.queue_configure_changes(changes, skip_window);
        }
        let windows = self.registry.composited_windows();
        let _ = self.compositor.compose_windows(&windows);
    }

    fn queue_configure_changes(
        &mut self,
        changes: Vec<ConfigureChange>,
        skip_window: Option<WindowId>,
    ) {
        for change in changes {
            if skip_window == Some(change.id) {
                continue;
            }
            let content_size = content_size_for_window_frame(change.size);
            self.pending_events.push(PendingEvent {
                owner_session: change.owner_session,
                event: protocol::Event {
                    kind: protocol::EventKind::Configure,
                    window_id: change.id.raw(),
                    surface_id: change.surface.raw(),
                    flags: 0,
                    arg0: protocol::pack_size(content_size.width, content_size.height),
                    arg1: change.serial,
                },
            });
        }
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

fn recommended_content_size(output_size: Size) -> Size {
    if output_size.width == 0 || output_size.height == 0 {
        return Size {
            width: 760,
            height: 440,
        };
    }
    let frame = Size {
        width: output_size.width.saturating_sub(56).max(1),
        height: output_size.height.saturating_sub(56).max(1),
    };
    content_size_for_window_frame(frame)
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
    arg2: u64,
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
        arg2,
        inline_bytes: 0,
        reserved0: 0,
    }
}
