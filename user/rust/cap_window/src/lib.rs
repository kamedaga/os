#![no_std]

//! Generic CapabilityOS window protocol.
//!
//! This crate is the protocol boundary between applications and whichever
//! userland window server is currently providing `ServiceKind::Window`.
//! Pachaland is one implementation of that service, not the owner of the
//! protocol.
//!
//! Design rules:
//!
//! - `cap_window` owns wire structs, object ids, lifecycle opcodes, and
//!   capability meaning.
//! - Window managers own placement, focus, decorations, animation, and policy.
//! - Applications request content surfaces; they do not position top-level
//!   windows directly.
//! - GPU details are represented as optional surface handles, but the protocol
//!   must remain usable by a CPU-buffer or future non-virgl compositor.
//! - A create request must be cheap enough to acknowledge before expensive
//!   first-frame work. Expensive rendering belongs after `Present`.
//! - Window size is the content size. Frame, border, titlebar, and placement are
//!   server policy and are intentionally not app-controlled.

use core::mem::size_of;
use core::ptr::{addr_of, addr_of_mut, read_volatile, write_volatile};
use core::sync::atomic::{Ordering, compiler_fence};

use rt_core::{SyscallError, syscall, vm};
use rt_handle::{ServiceKind, snapshot_service_registry_shadow};

pub mod protocol {
    pub const REQUEST_MAGIC: u32 = 0x5157_494E; // "QWIN"
    pub const RESPONSE_MAGIC: u32 = 0x5257_494E; // "RWIN"
    pub const VERSION: u16 = 1;
    pub const REQUEST_PAYLOAD_BYTES: usize = 4096 - core::mem::size_of::<RequestHeader>();
    pub const TITLE_INLINE_BYTES: usize = 96;

    pub const RESPONSE_FLAG_GPU_SURFACE: u32 = 1 << 0;
    pub const EVENT_FLAGS_KIND_SHIFT: u32 = 0;
    pub const EVENT_FLAGS_KIND_MASK: u32 = 0x0000_FFFF;
    pub const EVENT_FLAGS_VALUE_SHIFT: u32 = 16;
    pub const EVENT_FLAGS_VALUE_MASK: u32 = 0xFFFF_0000;

    pub const WINDOW_STATE_FOCUSED: u32 = 1 << 0;
    pub const WINDOW_STATE_FULLSCREEN: u32 = 1 << 1;
    pub const WINDOW_STATE_MAXIMIZED: u32 = 1 << 2;
    pub const WINDOW_STATE_RESIZING: u32 = 1 << 3;

    #[derive(Copy, Clone, Debug, Eq, PartialEq)]
    #[repr(u16)]
    pub enum Opcode {
        Hello = 1,
        QueryOutput = 2,
        CreateWindow = 3,
        DestroyWindow = 4,
        Present = 5,
        PollEvent = 6,
        AckConfigure = 7,
        QueryWindow = 8,
    }

    impl Opcode {
        pub const fn from_raw(raw: u16) -> Option<Self> {
            match raw {
                1 => Some(Self::Hello),
                2 => Some(Self::QueryOutput),
                3 => Some(Self::CreateWindow),
                4 => Some(Self::DestroyWindow),
                5 => Some(Self::Present),
                6 => Some(Self::PollEvent),
                7 => Some(Self::AckConfigure),
                8 => Some(Self::QueryWindow),
                _ => None,
            }
        }
    }

    #[derive(Copy, Clone, Debug, Eq, PartialEq)]
    #[repr(i32)]
    pub enum Status {
        Ok = 0,
        Invalid = 1,
        NotFound = 2,
        NoRight = 3,
        Busy = 4,
        Unsupported = 5,
        IoError = 6,
    }

    impl Status {
        pub const fn from_raw(raw: i32) -> Option<Self> {
            match raw {
                0 => Some(Self::Ok),
                1 => Some(Self::Invalid),
                2 => Some(Self::NotFound),
                3 => Some(Self::NoRight),
                4 => Some(Self::Busy),
                5 => Some(Self::Unsupported),
                6 => Some(Self::IoError),
                _ => None,
            }
        }
    }

    #[derive(Copy, Clone, Debug, Eq, PartialEq)]
    #[repr(u16)]
    pub enum EventKind {
        None = 0,
        Configure = 1,
        Focus = 2,
        Key = 3,
        PointerMotion = 4,
        PointerButton = 5,
        Close = 6,
        FrameDone = 7,
    }

    impl EventKind {
        pub const fn from_raw(raw: u16) -> Option<Self> {
            match raw {
                0 => Some(Self::None),
                1 => Some(Self::Configure),
                2 => Some(Self::Focus),
                3 => Some(Self::Key),
                4 => Some(Self::PointerMotion),
                5 => Some(Self::PointerButton),
                6 => Some(Self::Close),
                7 => Some(Self::FrameDone),
                _ => None,
            }
        }
    }

    #[repr(C)]
    #[derive(Copy, Clone)]
    pub struct RequestHeader {
        pub magic: u32,
        pub version: u16,
        pub op: u16,
        pub request_seq: u64,
        pub window_id: u32,
        pub surface_id: u32,
        pub arg0: u64,
        pub arg1: u64,
        pub inline_bytes: u32,
        pub flags: u32,
        pub response_paddr: u64,
        pub session_nonce: u64,
    }

    #[repr(C)]
    #[derive(Copy, Clone)]
    pub struct ResponseHeader {
        pub magic: u32,
        pub version: u16,
        pub op: u16,
        pub response_seq: u64,
        pub status: i32,
        pub result_flags: u32,
        pub window_id: u32,
        pub surface_id: u32,
        pub arg0: u64,
        pub arg1: u64,
        pub arg2: u64,
        pub inline_bytes: u32,
        pub reserved0: u32,
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
    pub struct Window {
        pub id: u32,
        pub surface_id: u32,
        pub content_size: Size,
        pub gpu_resource_id: u32,
        pub gpu_surface_id: u32,
        pub configure_serial: u64,
    }

    #[derive(Copy, Clone, Debug, Eq, PartialEq)]
    pub struct WindowInfo {
        pub id: u32,
        pub surface_id: u32,
        pub content_size: Size,
        pub state_flags: u32,
        pub configure_serial: u64,
    }

    #[derive(Copy, Clone, Debug, Eq, PartialEq)]
    pub struct OutputInfo {
        pub pixel_size: Size,
        pub recommended_content_size: Size,
    }

    #[derive(Copy, Clone, Debug, Eq, PartialEq)]
    pub struct ConfigureEvent {
        pub window_id: u32,
        pub surface_id: u32,
        pub content_size: Size,
        pub state_flags: u32,
        pub configure_serial: u64,
    }

    #[derive(Copy, Clone, Debug, Eq, PartialEq)]
    pub struct Event {
        pub kind: EventKind,
        pub window_id: u32,
        pub surface_id: u32,
        pub flags: u32,
        pub arg0: u64,
        pub arg1: u64,
    }

    pub const fn pack_size(width: u32, height: u32) -> u64 {
        width as u64 | ((height as u64) << 32)
    }

    pub const fn unpack_size(raw: u64) -> Size {
        Size {
            width: raw as u32,
            height: (raw >> 32) as u32,
        }
    }

    pub const fn pack_position(x: i32, y: i32) -> u64 {
        x as u32 as u64 | ((y as u32 as u64) << 32)
    }

    pub const fn unpack_position(raw: u64) -> Position {
        Position {
            x: raw as u32 as i32,
            y: (raw >> 32) as u32 as i32,
        }
    }

    pub const fn pack_event_result_flags(kind: EventKind, flags: u32) -> u32 {
        ((kind as u32) << EVENT_FLAGS_KIND_SHIFT)
            | ((flags << EVENT_FLAGS_VALUE_SHIFT) & EVENT_FLAGS_VALUE_MASK)
    }

    pub const fn unpack_event_kind(raw: u32) -> Option<EventKind> {
        EventKind::from_raw(((raw & EVENT_FLAGS_KIND_MASK) >> EVENT_FLAGS_KIND_SHIFT) as u16)
    }

    pub const fn unpack_event_flags(raw: u32) -> u32 {
        (raw & EVENT_FLAGS_VALUE_MASK) >> EVENT_FLAGS_VALUE_SHIFT
    }

    pub const fn configure_event_from_parts(
        window_id: u32,
        surface_id: u32,
        result_flags: u32,
        arg0: u64,
        arg1: u64,
    ) -> ConfigureEvent {
        ConfigureEvent {
            window_id,
            surface_id,
            content_size: unpack_size(arg0),
            state_flags: unpack_event_flags(result_flags),
            configure_serial: arg1,
        }
    }
}

const PAGE_BYTES: usize = 4096;
const DEFAULT_RESPONSE_POLL_LIMIT: u64 = 65536;
const PAGE_RIGHT_CPU_READ: u64 = 0x1;
const PAGE_RIGHT_CPU_WRITE: u64 = 0x2;

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Error {
    MissingService,
    EndpointNotFound,
    EndpointInstallFailed,
    RequestAllocFailed,
    RequestMapFailed,
    ResponseAllocFailed,
    ResponseMapFailed,
    ResponseGrantFailed,
    RequestSendFailed,
    Timeout,
    Invalid,
    InvalidResponse,
    BufferTooSmall,
    NoRight,
    Busy,
    Unsupported,
    IoError,
    GpuUnavailable,
}

impl From<Error> for cap_std::Error {
    fn from(value: Error) -> Self {
        let kind = match value {
            Error::Invalid | Error::InvalidResponse => cap_std::ErrorKind::InvalidData,
            Error::BufferTooSmall => cap_std::ErrorKind::BufferTooSmall,
            Error::MissingService
            | Error::EndpointNotFound
            | Error::EndpointInstallFailed
            | Error::RequestSendFailed => cap_std::ErrorKind::ConnectionFailed,
            Error::RequestAllocFailed
            | Error::RequestMapFailed
            | Error::ResponseAllocFailed
            | Error::ResponseMapFailed
            | Error::ResponseGrantFailed
            | Error::IoError => cap_std::ErrorKind::Other,
            Error::Timeout => cap_std::ErrorKind::TimedOut,
            Error::NoRight => cap_std::ErrorKind::PermissionDenied,
            Error::Busy => cap_std::ErrorKind::Busy,
            Error::Unsupported | Error::GpuUnavailable => cap_std::ErrorKind::Unsupported,
        };
        cap_std::Error::new(kind)
    }
}

impl protocol::Window {
    pub const fn has_gpu_surface(self) -> bool {
        self.gpu_resource_id != 0 && self.gpu_surface_id != 0
    }

    pub const fn render_target(self) -> Option<caplibgl::RenderTarget> {
        if !self.has_gpu_surface() {
            return None;
        }
        Some(caplibgl::RenderTarget {
            width: self.content_size.width,
            height: self.content_size.height,
            resource_id: self.gpu_resource_id,
            surface_id: self.gpu_surface_id,
            vertex_buffer_id: caplibgl::DEFAULT_VIRGL_VERTEX_BUFFER_ID,
        })
    }
}

pub fn connect_gl_for_window(window: protocol::Window) -> Result<caplibgl::Context, Error> {
    let Some(target) = window.render_target() else {
        return Err(Error::GpuUnavailable);
    };
    let mut context =
        caplibgl::Context::connect_from_registry_shadow().map_err(|_| Error::GpuUnavailable)?;
    context.make_surface_current(target);
    Ok(context)
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct ConnectOptions {
    pub request_va: u64,
    pub response_va: u64,
    pub endpoint_id: u64,
    pub server_process_slot: u64,
    pub response_poll_limit: u64,
}

pub struct Client {
    request_va: u64,
    response_va: u64,
    request_paddr: u64,
    response_paddr: u64,
    endpoint_id: u64,
    session_nonce: u64,
    next_seq: u64,
    request_shared: bool,
    response_poll_limit: u64,
}

impl Client {
    pub fn connect_from_registry_shadow() -> Result<Self, Error> {
        let binding = unsafe {
            snapshot_service_registry_shadow()
                .and_then(|snapshot| snapshot.find_kind(ServiceKind::Window))
        }
        .ok_or(Error::MissingService)?;

        let request = vm::alloc_map_page(true).map_err(map_request_page_error)?;
        let response = vm::alloc_map_page(true).map_err(map_response_page_error)?;
        Self::connect_mapped(
            request,
            response,
            binding.endpoint_id,
            binding.process_slot,
            DEFAULT_RESPONSE_POLL_LIMIT,
        )
    }

    pub fn connect(options: ConnectOptions) -> Result<Self, Error> {
        if options.endpoint_id == 0 || options.server_process_slot == 0 {
            return Err(Error::EndpointNotFound);
        }
        let request_paddr = alloc_page().ok_or(Error::RequestAllocFailed)?;
        map_page(options.request_va, request_paddr, true).map_err(|_| Error::RequestMapFailed)?;

        let response_paddr = alloc_page().ok_or(Error::ResponseAllocFailed)?;
        map_page(options.response_va, response_paddr, true)
            .map_err(|_| Error::ResponseMapFailed)?;

        if syscall::call3(
            syscall::INSTALL_ENDPOINT,
            0,
            options.endpoint_id,
            options.server_process_slot,
        ) != syscall::OK
        {
            return Err(Error::EndpointInstallFailed);
        }
        let rights = PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE;
        if syscall::call3(
            syscall::GRANT_CAP_ON_ENDPOINT,
            response_paddr,
            options.endpoint_id,
            rights,
        ) != syscall::OK
        {
            return Err(Error::ResponseGrantFailed);
        }
        let session_nonce = make_session_nonce(
            request_paddr,
            response_paddr,
            options.endpoint_id,
            options.server_process_slot,
        );

        let client = Self {
            request_va: options.request_va,
            response_va: options.response_va,
            request_paddr,
            response_paddr,
            endpoint_id: options.endpoint_id,
            session_nonce,
            next_seq: 1,
            request_shared: false,
            response_poll_limit: options.response_poll_limit,
        };
        client.clear_pages();
        Ok(client)
    }

    fn connect_mapped(
        request: vm::MappedPage,
        response: vm::MappedPage,
        endpoint_id: u64,
        server_process_slot: u64,
        response_poll_limit: u64,
    ) -> Result<Self, Error> {
        if endpoint_id == 0 || server_process_slot == 0 {
            return Err(Error::EndpointNotFound);
        }
        if syscall::call3(
            syscall::INSTALL_ENDPOINT,
            0,
            endpoint_id,
            server_process_slot,
        ) != syscall::OK
        {
            return Err(Error::EndpointInstallFailed);
        }
        let rights = PAGE_RIGHT_CPU_READ | PAGE_RIGHT_CPU_WRITE;
        if syscall::call3(
            syscall::GRANT_CAP_ON_ENDPOINT,
            response.paddr(),
            endpoint_id,
            rights,
        ) != syscall::OK
        {
            return Err(Error::ResponseGrantFailed);
        }
        let session_nonce = make_session_nonce(
            request.paddr(),
            response.paddr(),
            endpoint_id,
            server_process_slot,
        );
        let client = Self {
            request_va: request.va(),
            response_va: response.va(),
            request_paddr: request.paddr(),
            response_paddr: response.paddr(),
            endpoint_id,
            session_nonce,
            next_seq: 1,
            request_shared: false,
            response_poll_limit,
        };
        client.clear_pages();
        Ok(client)
    }

    pub fn hello(&mut self) -> Result<(), Error> {
        let seq = self.begin_request(protocol::Opcode::Hello, 0, 0, 0, 0, &[])?;
        self.finish_request_ok(seq, protocol::Opcode::Hello)?;
        Ok(())
    }

    pub fn query_output(&mut self) -> Result<protocol::OutputInfo, Error> {
        let seq = self.begin_request(protocol::Opcode::QueryOutput, 0, 0, 0, 0, &[])?;
        let response = self.finish_request_ok(seq, protocol::Opcode::QueryOutput)?;
        Ok(protocol::OutputInfo {
            pixel_size: protocol::unpack_size(unsafe { read_volatile(addr_of!((*response).arg0)) }),
            recommended_content_size: protocol::unpack_size(unsafe {
                read_volatile(addr_of!((*response).arg1))
            }),
        })
    }

    pub fn query_window(&mut self, window_id: u32) -> Result<protocol::WindowInfo, Error> {
        if window_id == 0 {
            return Err(Error::Invalid);
        }
        let seq = self.begin_request(protocol::Opcode::QueryWindow, window_id, 0, 0, 0, &[])?;
        let response = self.finish_request_ok(seq, protocol::Opcode::QueryWindow)?;
        let window_id = unsafe { read_volatile(addr_of!((*response).window_id)) };
        let surface_id = unsafe { read_volatile(addr_of!((*response).surface_id)) };
        if window_id == 0 || surface_id == 0 {
            return Err(Error::InvalidResponse);
        }
        Ok(protocol::WindowInfo {
            id: window_id,
            surface_id,
            content_size: protocol::unpack_size(unsafe {
                read_volatile(addr_of!((*response).arg0))
            }),
            state_flags: unsafe { read_volatile(addr_of!((*response).result_flags)) },
            configure_serial: unsafe { read_volatile(addr_of!((*response).arg1)) },
        })
    }

    pub fn create_window(
        &mut self,
        title: &str,
        preferred_width: u32,
        preferred_height: u32,
    ) -> Result<protocol::Window, Error> {
        if preferred_width == 0 || preferred_height == 0 {
            return Err(Error::Invalid);
        }
        let title = title.as_bytes();
        if title.len() > protocol::TITLE_INLINE_BYTES {
            return Err(Error::BufferTooSmall);
        }
        let seq = self.begin_request(
            protocol::Opcode::CreateWindow,
            0,
            0,
            protocol::pack_size(preferred_width, preferred_height),
            0,
            title,
        )?;
        let response = self.finish_request_ok(seq, protocol::Opcode::CreateWindow)?;
        let window_id = unsafe { read_volatile(addr_of!((*response).window_id)) };
        let surface_id = unsafe { read_volatile(addr_of!((*response).surface_id)) };
        let result_flags = unsafe { read_volatile(addr_of!((*response).result_flags)) };
        let content_size =
            protocol::unpack_size(unsafe { read_volatile(addr_of!((*response).arg0)) });
        let gpu_handles = unsafe { read_volatile(addr_of!((*response).arg1)) };
        let (gpu_resource_id, gpu_surface_id) =
            if (result_flags & protocol::RESPONSE_FLAG_GPU_SURFACE) != 0 {
                (gpu_handles as u32, (gpu_handles >> 32) as u32)
            } else {
                (0, 0)
            };
        if window_id == 0 || surface_id == 0 || content_size.width == 0 || content_size.height == 0
        {
            return Err(Error::InvalidResponse);
        }
        Ok(protocol::Window {
            id: window_id,
            surface_id,
            content_size,
            gpu_resource_id,
            gpu_surface_id,
            configure_serial: unsafe { read_volatile(addr_of!((*response).arg2)) },
        })
    }

    pub fn present(&mut self, window: protocol::Window) -> Result<(), Error> {
        let seq = self.begin_request(
            protocol::Opcode::Present,
            window.id,
            window.surface_id,
            0,
            0,
            &[],
        )?;
        self.finish_request_ok(seq, protocol::Opcode::Present)?;
        Ok(())
    }

    pub fn ack_configure(&mut self, window_id: u32, configure_serial: u64) -> Result<(), Error> {
        if window_id == 0 || configure_serial == 0 {
            return Err(Error::Invalid);
        }
        let seq = self.begin_request(
            protocol::Opcode::AckConfigure,
            window_id,
            0,
            configure_serial,
            0,
            &[],
        )?;
        self.finish_request_ok(seq, protocol::Opcode::AckConfigure)?;
        Ok(())
    }

    pub fn poll_event(&mut self) -> Result<Option<protocol::Event>, Error> {
        let seq = self.begin_request(protocol::Opcode::PollEvent, 0, 0, 0, 0, &[])?;
        let response = self.finish_request_ok(seq, protocol::Opcode::PollEvent)?;
        let result_flags = unsafe { read_volatile(addr_of!((*response).result_flags)) };
        let Some(kind) = protocol::unpack_event_kind(result_flags) else {
            return Err(Error::InvalidResponse);
        };
        if kind == protocol::EventKind::None {
            return Ok(None);
        }
        Ok(Some(protocol::Event {
            kind,
            window_id: unsafe { read_volatile(addr_of!((*response).window_id)) },
            surface_id: unsafe { read_volatile(addr_of!((*response).surface_id)) },
            flags: protocol::unpack_event_flags(result_flags),
            arg0: unsafe { read_volatile(addr_of!((*response).arg0)) },
            arg1: unsafe { read_volatile(addr_of!((*response).arg1)) },
        }))
    }

    pub fn destroy_window(&mut self, window_id: u32) -> Result<(), Error> {
        if window_id == 0 {
            return Err(Error::Invalid);
        }
        let seq = self.begin_request(protocol::Opcode::DestroyWindow, window_id, 0, 0, 0, &[])?;
        self.finish_request_ok(seq, protocol::Opcode::DestroyWindow)?;
        Ok(())
    }

    fn begin_request(
        &mut self,
        op: protocol::Opcode,
        window_id: u32,
        surface_id: u32,
        arg0: u64,
        arg1: u64,
        payload: &[u8],
    ) -> Result<u64, Error> {
        if payload.len() > protocol::REQUEST_PAYLOAD_BYTES {
            return Err(Error::BufferTooSmall);
        }
        self.clear_pages();
        let request = self.request_header();
        unsafe {
            write_volatile(addr_of_mut!((*request).magic), protocol::REQUEST_MAGIC);
            write_volatile(addr_of_mut!((*request).version), protocol::VERSION);
            write_volatile(addr_of_mut!((*request).op), op as u16);
            write_volatile(addr_of_mut!((*request).window_id), window_id);
            write_volatile(addr_of_mut!((*request).surface_id), surface_id);
            write_volatile(addr_of_mut!((*request).arg0), arg0);
            write_volatile(addr_of_mut!((*request).arg1), arg1);
            write_volatile(addr_of_mut!((*request).inline_bytes), payload.len() as u32);
            write_volatile(addr_of_mut!((*request).flags), 0);
            write_volatile(addr_of_mut!((*request).response_paddr), self.response_paddr);
            write_volatile(addr_of_mut!((*request).session_nonce), self.session_nonce);
        }
        copy_bytes_to_volatile(self.request_payload(), payload);

        let seq = self.next_seq;
        self.next_seq = self.next_seq.wrapping_add(1).max(1);
        compiler_fence(Ordering::SeqCst);
        unsafe {
            write_volatile(addr_of_mut!((*request).request_seq), seq);
        }

        let result = if self.request_shared {
            syscall::OK
        } else {
            let send = syscall::call2(syscall::SHARE_CAP, self.request_paddr, self.endpoint_id);
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

    fn finish_request_ok(
        &self,
        expected_seq: u64,
        expected_op: protocol::Opcode,
    ) -> Result<*mut protocol::ResponseHeader, Error> {
        let response = self.finish_request(expected_seq, expected_op)?;
        match protocol::Status::from_raw(unsafe { read_volatile(addr_of!((*response).status)) }) {
            Some(protocol::Status::Ok) => Ok(response),
            Some(protocol::Status::Invalid) => Err(Error::Invalid),
            Some(protocol::Status::NoRight) => Err(Error::NoRight),
            Some(protocol::Status::Busy) => Err(Error::Busy),
            Some(protocol::Status::Unsupported) => Err(Error::Unsupported),
            Some(protocol::Status::IoError) => Err(Error::IoError),
            Some(protocol::Status::NotFound) | None => Err(Error::InvalidResponse),
        }
    }

    fn finish_request(
        &self,
        expected_seq: u64,
        expected_op: protocol::Opcode,
    ) -> Result<*mut protocol::ResponseHeader, Error> {
        let response = self.response_header();
        let mut polls = 0;
        while polls < self.response_poll_limit {
            if unsafe { read_volatile(addr_of!((*response).response_seq)) } == expected_seq {
                let magic = unsafe { read_volatile(addr_of!((*response).magic)) };
                let version = unsafe { read_volatile(addr_of!((*response).version)) };
                let op = unsafe { read_volatile(addr_of!((*response).op)) };
                if magic != protocol::RESPONSE_MAGIC
                    || version != protocol::VERSION
                    || op != expected_op as u16
                {
                    return Err(Error::InvalidResponse);
                }
                return Ok(response);
            }
            let _ = syscall::call2(syscall::WAIT_EVENT, 0, 1);
            polls += 1;
        }
        Err(Error::Timeout)
    }

    fn request_header(&self) -> *mut protocol::RequestHeader {
        self.request_va as *mut protocol::RequestHeader
    }

    fn response_header(&self) -> *mut protocol::ResponseHeader {
        self.response_va as *mut protocol::ResponseHeader
    }

    fn request_payload(&self) -> *mut u8 {
        (self.request_va + size_of::<protocol::RequestHeader>() as u64) as *mut u8
    }

    fn clear_pages(&self) {
        clear_page(self.request_va);
        clear_page(self.response_va);
    }
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

fn make_session_nonce(request_paddr: u64, response_paddr: u64, endpoint_id: u64, tag: u64) -> u64 {
    let nonce = request_paddr
        ^ response_paddr.rotate_left(17)
        ^ endpoint_id.rotate_left(7)
        ^ tag
        ^ 0x4f1b_bcdc_bfa5_3c4d;
    if nonce == 0 { 1 } else { nonce }
}

fn map_page(va: u64, paddr: u64, writable: bool) -> Result<(), ()> {
    let flags = if writable { 1 } else { 0 };
    if syscall::call3(syscall::MAP_PAGE, va, paddr, flags) == syscall::OK {
        Ok(())
    } else {
        Err(())
    }
}

fn clear_page(va: u64) {
    let bytes = va as *mut u8;
    let mut index = 0usize;
    while index < PAGE_BYTES {
        unsafe { write_volatile(bytes.add(index), 0) };
        index += 1;
    }
}

fn copy_bytes_to_volatile(dst: *mut u8, src: &[u8]) {
    for (index, byte) in src.iter().copied().enumerate() {
        unsafe { write_volatile(dst.add(index), byte) };
    }
}
