#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use core::fmt::Write;

use rt_alloc as _;
use rt_core::entry_point;
use rt_handle::{FsConnectionId, HandleTable};
use rt_io::{PersistentFsClient, monotonic_now_ticks};

const DEMO_PATH: &str = "/sys/startup_manifest.txt";

fn sanitize_append(dst: &mut String, bytes: &[u8]) {
    for &byte in bytes {
        match byte {
            b'\r' => {}
            b'\n' => dst.push('|'),
            b'\t' => dst.push(' '),
            0x20..=0x7e => dst.push(byte as char),
            _ => dst.push('.'),
        }
    }
}

fn abort_with(mut message: String) -> ! {
    message.push('\n');
    rt_core::log(&message);
    rt_core::abort()
}

fn log_stage(stage: &str) {
    let mut message = String::from("rust fs demo stage=");
    message.push_str(stage);
    message.push('\n');
    rt_core::log(&message);
}

fn main() -> ! {
    let mut message = String::from("rust fs demo");
    let now = monotonic_now_ticks();
    let _ = write!(&mut message, " tick={}", now);
    log_stage("start");

    let mut client = match PersistentFsClient::connect_from_shadow(FsConnectionId::new(1)) {
        Ok(client) => client,
        Err(err) => {
            let _ = write!(&mut message, " connect_failed={err:?}");
            abort_with(message);
        }
    };
    log_stage("connected");
    let _ = write!(
        &mut message,
        " conn={} req_va=0x{:x} resp_va=0x{:x}",
        client.connection_id().raw(),
        client.request_va(),
        client.response_va()
    );

    let root = match client.root_dir() {
        Ok(dir) => dir,
        Err(err) => {
            let _ = write!(&mut message, " root_failed={err:?}");
            abort_with(message);
        }
    };
    log_stage("root_ready");

    let file = match client.lookup_file(root, DEMO_PATH) {
        Ok(file) => file,
        Err(err) => {
            let _ = write!(&mut message, " lookup_failed={err:?}");
            abort_with(message);
        }
    };
    log_stage("lookup_ready");

    let mut handles = HandleTable::new();
    let root_handle =
        handles.insert_dir(client.connection_id(), root.token(), root.assumed_rights());
    let file_handle =
        handles.insert_vnode_file(client.connection_id(), file.token(), file.assumed_rights());
    let _ = write!(
        &mut message,
        " handles={} root_handle={} file_handle={} path={}",
        handles.len(),
        root_handle.raw(),
        file_handle.raw(),
        DEMO_PATH
    );

    let stat = match client.stat_file(file) {
        Ok(stat) => stat,
        Err(err) => {
            let _ = write!(&mut message, " stat_failed={err:?}");
            abort_with(message);
        }
    };
    let _ = write!(
        &mut message,
        " kind={} size={}",
        stat.object_kind.name(),
        stat.size_bytes
    );
    log_stage("stat_ready");

    let open_file = match client.open_file(file) {
        Ok(open_file) => open_file,
        Err(err) => {
            let _ = write!(&mut message, " open_failed={err:?}");
            abort_with(message);
        }
    };
    let open_handle = handles.insert_open_file(
        client.connection_id(),
        open_file.token(),
        open_file.assumed_rights(),
    );
    let _ = write!(&mut message, " open_handle={}", open_handle.raw());
    log_stage("open_ready");

    let mut read_buf = [0u8; 96];
    let read = match client.read(open_file, 0, &mut read_buf) {
        Ok(read) => read,
        Err(err) => {
            let _ = write!(&mut message, " read_failed={err:?}");
            abort_with(message);
        }
    };
    let _ = write!(
        &mut message,
        " bytes_read={} next_offset={}",
        read.bytes_read, read.next_offset
    );
    message.push_str(" sample=\"");
    sanitize_append(&mut message, &read_buf[..read.bytes_read]);
    message.push('"');
    log_stage("read_ready");
    abort_with(message)
}

entry_point!(main);
