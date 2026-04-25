use alloc::vec::Vec;
use core::mem::size_of;
use core::ptr::{addr_of_mut, read_volatile, write_volatile};
use core::sync::atomic::{Ordering, compiler_fence};

use capwm_client::protocol;
use rt_core::syscall;

use crate::server::Server;

const CAP_TRANSFER_ID_MIN: u64 = 0x1000;
const PAGE_BYTES: u64 = 4096;
const SESSION_BASE_VA: u64 = 0x3C18_0000;
const SESSION_STRIDE_BYTES: u64 = PAGE_BYTES * 2;
const MAX_SESSIONS: usize = 16;
const POINTER_POLL_TICKS: u64 = 1;

#[derive(Copy, Clone)]
struct Session {
    request_paddr: u64,
    response_paddr: u64,
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
            self.process_sessions();
            self.server.pump_pointer();
            if self.server.has_loading_windows() {
                self.server.present_desktop();
            }
        }
    }

    fn accept_request_page(&mut self, request_paddr: u64) {
        if self
            .sessions
            .iter()
            .any(|session| session.request_paddr == request_paddr)
        {
            return;
        }
        if self.sessions.len() >= MAX_SESSIONS {
            cap_std::println!("pachaland: client session table full").ok();
            return;
        }
        let index = self.sessions.len() as u64;
        let request_va = SESSION_BASE_VA + index * SESSION_STRIDE_BYTES;
        let response_va = request_va + PAGE_BYTES;
        if map_page(request_va, request_paddr, false).is_err() {
            cap_std::println!("pachaland: map request page failed").ok();
            return;
        }
        self.sessions.push(Session {
            request_paddr,
            response_paddr: 0,
            request_va,
            response_va,
            last_request_seq: 0,
        });
    }

    fn process_sessions(&mut self) {
        let mut index = 0;
        while index < self.sessions.len() {
            self.process_session(index);
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
        let response = self.server.handle_request(&request, &payload);
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
            return false;
        }
        if map_page(session.response_va, response_paddr, true).is_err() {
            cap_std::println!("pachaland: map response page failed").ok();
            return false;
        }
        session.response_paddr = response_paddr;
        true
    }
}

fn map_page(va: u64, paddr: u64, writable: bool) -> Result<(), ()> {
    let flags = if writable { 1 } else { 0 };
    if syscall::call3(syscall::MAP_PAGE, va, paddr, flags) == syscall::OK {
        Ok(())
    } else {
        Err(())
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
