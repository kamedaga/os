#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write;
use rt_core::entry_point;
use rt_alloc as _;
use rt_handle::{HandleTable, snapshot_service_registry_shadow};

fn main() -> ! {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(b"hello");
    bytes.extend_from_slice(b" from ");
    bytes.extend_from_slice(b"rust handle");

    let mut message = String::from_utf8(bytes).unwrap_or_else(|_| String::from("rt_handle failed"));
    let mut handles = HandleTable::new();
    let registry_id = handles.insert_service_registry_shadow();
    let _ = write!(&mut message, " handles={}", handles.len());
    let _ = write!(&mut message, " registry_handle={}", registry_id.raw());

    // SAFETY: shell now bootstraps the shared service-registry page into spawned children.
    unsafe {
        match snapshot_service_registry_shadow() {
            Some(snapshot) => {
                let _ = write!(&mut message, " services={}", snapshot.len());
                for entry in snapshot.entries() {
                    let _ = write!(
                        &mut message,
                        " {}@{}:{}",
                        entry.kind_enum().name(),
                        entry.process_slot,
                        entry.endpoint_id
                    );
                }
            },
            None => message.push_str(" services=unavailable"),
        }
    }
    message.push('\n');

    rt_core::log(&message);
    rt_core::abort()
}

entry_point!(main);
