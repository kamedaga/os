use alloc::vec::Vec;
use core::mem::size_of;
use core::ptr::{addr_of_mut, read_volatile, write_volatile};
use core::sync::atomic::{compiler_fence, Ordering};

use cap_window::protocol;
use rt_core::{syscall, vm};

use crate::server::Server;

const CAP_TRANSFER_ID_MIN: u64 = 0x1000;
const MAX_SESSIONS: usize = 64;
const POINTER_POLL_TICKS: u64 = 1;

#[derive(Copy, Clone)]
struct Session {
    active: bool,
    request_paddr: u64,
    response_paddr: u64,
    session_nonce: u64,
    request_va: u64,
    response_va: u64,
    last_request_seq: u64,
}

pub struct ServiceHost {
    server: Server,
    sessions: Vec<Session>,
}

impl ServiceHost {
    pub fn new(server: Server) -> Self {
        Self {
            server,
            sessions: Vec::new(),
        }
    }

    pub fn run_forever(&mut self) -> ! {
        self.server.present_desktop();
        cap_std::println!("pachaland: service loop ready").ok();
        loop {
            let received = syscall::call2(syscall::WAIT_EVENT, 1, POINTER_POLL_TICKS);
            if received >= CAP_TRANSFER_ID_MIN {
                let accepted = syscall::call1(syscall::ACCEPT_CAP_TRANSFER, received);
                if accepted >= CAP_TRANSFER_ID_MIN {
                    self.accept_request_page(accepted);
                } else {
                    cap_std::println!("pachaland: accept cap transfer failed status={}", accepted)
                        .ok();
                }
            }
            self.server.pump_pointer();
            self.process_sessions();
            if self.server.has_loading_windows() {
                self.server.present_desktop();
            }
        }
    }

    fn accept_request_page(&mut self, request_paddr: u64) {
        for session in &mut self.sessions {
            if session.active && session.request_paddr == request_paddr {
                session.active = false;
            }
        }
        if self.sessions.len() >= MAX_SESSIONS {
            cap_std::println!("pachaland: client session table full").ok();
            return;
        }
        let request_page = match vm::map_page_at_dynamic_va(request_paddr, false) {
            Ok(page) => page,
            Err(_) => {
                cap_std::println!("pachaland: map request page failed").ok();
                return;
            }
        };
        let request_va = request_page.va();
        if request_va == 0 {
            cap_std::println!("pachaland: map request page failed").ok();
            return;
        }
        let request = unsafe { read_volatile(request_va as *const protocol::RequestHeader) };
        if request.magic != protocol::REQUEST_MAGIC
            || request.version != protocol::VERSION
            || request.request_seq == 0
            || request.response_paddr < CAP_TRANSFER_ID_MIN
            || request.session_nonce == 0
        {
            cap_std::println!("pachaland: invalid session request").ok();
            self.sessions.push(Session {
                active: false,
                request_paddr,
                response_paddr: 0,
                session_nonce: 0,
                request_va,
                response_va: 0,
                last_request_seq: request.request_seq,
            });
            return;
        }
        self.sessions.push(Session {
            active: true,
            request_paddr,
            response_paddr: 0,
            session_nonce: request.session_nonce,
            request_va,
            response_va: 0,
            last_request_seq: 0,
        });
    }

    fn process_sessions(&mut self) {
        let mut index = 0;
        while index < self.sessions.len() {
            if self.sessions[index].active {
                self.process_session(index);
            }
            index += 1;
        }
    }

    fn process_session(&mut self, index: usize) {
        let request_ptr = self.sessions[index].request_va as *const protocol::RequestHeader;
        let request = unsafe { read_volatile(request_ptr) };
        if request.request_seq == 0 || request.request_seq == self.sessions[index].last_request_seq
        {
            return;
        }
        if request.session_nonce != self.sessions[index].session_nonce {
            return;
        }
        if request.magic != protocol::REQUEST_MAGIC || request.version != protocol::VERSION {
            self.sessions[index].last_request_seq = request.request_seq;
            return;
        }
        if request.inline_bytes as usize > protocol::REQUEST_PAYLOAD_BYTES
            || request.response_paddr < CAP_TRANSFER_ID_MIN
        {
            self.sessions[index].last_request_seq = request.request_seq;
            return;
        }
        if !self.ensure_response_page(index, request.response_paddr) {
            return;
        }

        let payload = read_payload(
            self.sessions[index].request_va + size_of::<protocol::RequestHeader>() as u64,
            request.inline_bytes as usize,
        );
        let response =
            self.server
                .handle_request((index as u32).saturating_add(1), &request, &payload);
        write_response(self.sessions[index].response_va, response);
        self.sessions[index].last_request_seq = request.request_seq;
    }

    fn ensure_response_page(&mut self, index: usize, response_paddr: u64) -> bool {
        let session = &mut self.sessions[index];
        if session.response_paddr == response_paddr {
            return true;
        }
        if session.response_paddr != 0 {
            cap_std::println!("pachaland: response page changed").ok();
            session.active = false;
            return false;
        }
        let response_page = match vm::map_page_at_dynamic_va(response_paddr, true) {
            Ok(page) => page,
            Err(_) => {
                cap_std::println!("pachaland: map response page failed").ok();
                return false;
            }
        };
        session.response_va = response_page.va();
        session.response_paddr = response_paddr;
        true
    }
}

fn read_payload(base_va: u64, len: usize) -> Vec<u8> {
    let mut payload = Vec::with_capacity(len);
    let mut index = 0;
    while index < len {
        let byte = unsafe { read_volatile((base_va + index as u64) as *const u8) };
        payload.push(byte);
        index += 1;
    }
    payload
}

fn write_response(response_va: u64, response: protocol::ResponseHeader) {
    let response_ptr = response_va as *mut protocol::ResponseHeader;
    unsafe {
        write_volatile(
            response_ptr,
            protocol::ResponseHeader {
                response_seq: 0,
                ..response
            },
        );
        compiler_fence(Ordering::SeqCst);
        write_volatile(
            addr_of_mut!((*response_ptr).response_seq),
            response.response_seq,
        );
    }
}
